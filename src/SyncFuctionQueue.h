#ifndef FUNCTIONQUEUE_SYNCFUCTIONQUEUE_H
#define FUNCTIONQUEUE_SYNCFUCTIONQUEUE_H

#include <atomic>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <cassert>
#include <mutex>
#include <vector>

template<typename FunctionSignature>
class SyncFunctionQueue {

};

template<typename R, typename ...Args>
class SyncFunctionQueue<R(Args...)> {
private:
    struct FunctionCxt {
        uint32_t fp_offset;
        uint16_t obj_offset;
        uint16_t stride;
    };

    std::atomic<std::byte *> m_OutPosRead;
    std::atomic<size_t> m_RemainingRead;

    std::atomic<std::byte *> m_InputPos;
    std::atomic<std::byte *> m_OutPosFollow;
    std::atomic<size_t> m_RemainingClean;
    std::atomic<size_t> m_Remaining;

    std::mutex outPosMut, input_mut;

    std::byte *const m_Memory;
    size_t const m_MemorySize;

    using Storage = std::pair<void *, void *>;
    using AtomicFunctionCxt = std::atomic<FunctionCxt>;

    template<typename Callable>
    static R invoke(void *data, Args... args) {
        auto &functor = *static_cast<Callable *>(data);
        if constexpr (std::is_same_v<R, void>) {
            functor(args...);
            functor.~Callable();
        } else {
            auto &&result = functor(args...);
            functor.~Callable();
            return std::move(result);
        }
    }

    using InvokeAndDestroy = R(*)(void *data, Args...);

    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invoke<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);

    template<typename T>
    static constexpr inline T *align(void *ptr) noexcept {
        return reinterpret_cast<T *>((reinterpret_cast<uintptr_t>(ptr) - 1u + alignof(T)) & -alignof(T));
    }

    template<typename T>
    static inline void *
    align(void *&ptr, size_t &space) noexcept {
        const auto intptr = reinterpret_cast<uintptr_t>(ptr);
        const auto aligned = (intptr - 1u + alignof(T)) & -alignof(T);
        const auto diff = aligned - intptr;
        if ((sizeof(T) + diff) > space)
            return nullptr;
        else {
            space -= diff;
            return ptr = reinterpret_cast<void *>(aligned);
        }
    }

    template<typename Callable>
    Storage getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = align<AtomicFunctionCxt>(buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(AtomicFunctionCxt);
            size -= sizeof(AtomicFunctionCxt);
            auto const callable_storage = align<Callable>(buffer, size);
            return {fp_storage, callable_storage};
        };

        Storage storage{nullptr, nullptr};

        std::byte *input_pos;

        bool search_ahead;
        do {
            input_pos = m_InputPos.load();
            auto const out_pos = checkNAdvanceOutPos();

            if (out_pos == input_pos && m_Remaining.load()) {
                return {nullptr, nullptr};
            }

            search_ahead = input_pos >= out_pos;

            if (size_t const buffer_size = m_Memory + m_MemorySize - input_pos;
                    search_ahead && buffer_size) {
                storage = getAlignedStorage(input_pos, buffer_size);
                if (storage.first && storage.second) {
                    search_ahead = false;
                    continue;
                }
            }

            {
                auto const mem = search_ahead ? m_Memory : input_pos;
                if (size_t const buffer_size = out_pos - mem) {
                    storage = getAlignedStorage(mem, buffer_size);
                }
            }

            if (!storage.first || !storage.second)
                return {nullptr, nullptr};

        } while (!m_InputPos.compare_exchange_weak(input_pos,
                                                   static_cast<std::byte *>(storage.second) + sizeof(Callable)));

        if (search_ahead) {
            std::launder(new(align<AtomicFunctionCxt>(input_pos)) AtomicFunctionCxt{})->store(
                    {std::numeric_limits<uint32_t>::max(), 0, 0});
        }

        std::lock_guard lock{input_mut};

        return storage;
    }

    std::byte *checkNAdvanceOutPos() noexcept {
        auto out_pos = m_OutPosFollow.load();
        auto const rem = m_RemainingClean.load();

        if (!rem || !outPosMut.try_lock()) {
            return out_pos;
        }

        std::lock_guard lock{outPosMut, std::adopt_lock};

        auto cleaned = 0;
        for (; cleaned != rem;) {
            auto functionCxtPtr = std::launder(align<AtomicFunctionCxt>(out_pos));
            auto functionCxt = functionCxtPtr->load();
            if (functionCxt.fp_offset == std::numeric_limits<uint32_t>::max()) {
                out_pos = m_Memory;
            } else if (functionCxt.fp_offset == 0) {
                assert(functionCxt.stride);

                out_pos = reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxt.stride;
                ++cleaned;
            } else {
                break;
            }
        }

        if (cleaned) {
            assert(cleaned <= rem);
            m_RemainingClean.fetch_sub(cleaned);
            m_Remaining.fetch_sub(cleaned);
        }
        m_OutPosFollow.store(out_pos);

        return out_pos;
    }


public:
    SyncFunctionQueue(void *mem, size_t size) : m_Memory{static_cast<std::byte *const>(mem)}, m_MemorySize{size},
                                                m_RemainingRead{0}, m_Remaining{0}, m_RemainingClean{0} {
        m_InputPos = m_OutPosRead = m_OutPosFollow = m_Memory;
        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() {
        size_t rem = m_RemainingRead.load();
        if (!rem) return false;

        while (rem && !m_RemainingRead.compare_exchange_strong(rem, rem - 1));
        return rem;
    }

    inline R callAndPop(Args ... args) noexcept {
        AtomicFunctionCxt *functionCxtPtr;
        auto outPtr = m_OutPosRead.load();

        while (!m_OutPosRead.compare_exchange_weak(outPtr, [&, outPtr]() mutable {
            functionCxtPtr = std::launder(align<AtomicFunctionCxt>(outPtr));

            if (functionCxtPtr->load().fp_offset == std::numeric_limits<uint32_t>::max()) {
                functionCxtPtr = std::launder(align<AtomicFunctionCxt>(m_Memory));
            }

            return reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxtPtr->load().stride;
        }()));

        auto const functionCxt = functionCxtPtr->load();

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxt.obj_offset, args...);

            functionCxtPtr->store({0, functionCxt.obj_offset, functionCxt.stride});
            m_RemainingClean.fetch_add(1);

        } else {
            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    reinterpret_cast<std::byte *>(functionCxtPtr) + functionCxt.obj_offset, args...);

            functionCxtPtr->store({0, functionCxt.obj_offset, functionCxt.stride});
            m_RemainingClean.fetch_add(1);

            return std::move(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;

        auto const[fp_storage, callable_storage] = getCallableStorage<Callable>();


        if (!fp_storage || !callable_storage)
            return false;

        auto const obj_ptr = std::launder(new(callable_storage) Callable{std::forward<T>(function)});
        std::launder(new(fp_storage) AtomicFunctionCxt{})->store(
                {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke<Callable> ) - fp_base),
                 static_cast<uint16_t>(std::distance(static_cast<std::byte *>(fp_storage),
                                                     static_cast<std::byte *>(callable_storage))),
                 static_cast<uint16_t>(
                         std::distance(static_cast<std::byte *>(fp_storage), reinterpret_cast<std::byte *>(obj_ptr)) +
                         sizeof(Callable))
                });

        m_Remaining.fetch_add(1);
        m_RemainingRead.fetch_add(1);

        return true;
    }

    [[nodiscard]] size_t storage_used() const noexcept {
        auto input_pos = m_InputPos.load();
        auto out_pos = m_OutPosRead.load();
        if (input_pos > out_pos) return input_pos - out_pos;
        else if (input_pos == out_pos) return m_Remaining.load() ? m_MemorySize : 0;
        else return m_MemorySize - (out_pos - input_pos);
    }

};


#endif
