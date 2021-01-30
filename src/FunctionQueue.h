//
// Created by tekinas on 12/28/20.
//

#ifndef CPP_PROJECT_FUNCTIONQUEUE2_H
#define CPP_PROJECT_FUNCTIONQUEUE2_H

#include <memory_resource>
#include <cstring>
#include <atomic>
#include <iostream>
#include <cassert>

template<bool, bool, typename R>
class FunctionQueue {
};

template<bool synced, bool fixedSize, typename R, typename ...Args>
class FunctionQueue<synced, fixedSize, R(Args...)> {
public:

    template<typename T=R>
    requires (!fixedSize)
    FunctionQueue(std::pmr::memory_resource *memoryResource, size_t initial_capacity) :
            m_MemoryResource{memoryResource},
            m_CallableQueueMemorySize{initial_capacity},
            m_Remaining{0} {
        m_CallableQueueMemory = static_cast<std::byte *>(m_MemoryResource->allocate(m_CallableQueueMemorySize,
                                                                                    alignof(CallableCxt)));
        m_InputPos = m_OutPosReadCurrent = m_CallableQueueMemory;
        m_OutPutEndPos = nullptr;
    }

    template<typename T=R>
    requires (!fixedSize)
    explicit FunctionQueue(std::pmr::memory_resource *memoryResource) : m_MemoryResource{memoryResource},
                                                                        m_CallableQueueMemorySize{0},
                                                                        m_Remaining{0} {
        m_InputPos = m_OutPosReadCurrent = m_CallableQueueMemory = nullptr;
        m_OutPutEndPos = nullptr;
    }

    template<typename T=R>
    requires fixedSize
    FunctionQueue(void *buffer, size_t buffer_size) : m_CallableQueueMemory{static_cast<std::byte *>(buffer)},
                                                      m_CallableQueueMemorySize{buffer_size},
                                                      m_Remaining{0}, m_RemainingFront{0} {
        m_InputPos = m_OutPosReadCurrent = m_OutPosStart = m_CallableQueueMemory;
        if constexpr (!(synced && fixedSize)) m_OutPutEndPos = nullptr;
        memset(m_CallableQueueMemory, 0, m_CallableQueueMemorySize);
    }

    ~FunctionQueue() noexcept {
        if constexpr (!fixedSize) {
            if (m_MemoryResource)
                m_MemoryResource->deallocate(m_CallableQueueMemory, m_CallableQueueMemorySize,
                                             alignof(CallableCxt));
        }
    }

    [[nodiscard]] size_t storage_used() const noexcept {
        if (m_InputPos > m_OutPosReadCurrent) return m_InputPos - m_OutPosReadCurrent;
        else if (m_InputPos == m_OutPosReadCurrent) return m_Remaining ? buffer_size() : 0;
        else
            return m_CallableQueueMemorySize - (m_OutPosReadCurrent.load(std::memory_order_relaxed) -
                                                m_InputPos.load(std::memory_order_relaxed));
    }

    [[nodiscard]] size_t buffer_size() const noexcept {
        return m_CallableQueueMemorySize;
    }

    [[nodiscard]] size_t size() const noexcept {
        if constexpr (synced) return m_Remaining.load(std::memory_order::relaxed);
        else return m_Remaining;
    }

    template<typename T>
    std::conditional_t<fixedSize, bool, void> push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;
//        static_assert(std::is_trivially_copyable_v<Callable>);

        auto const[fp_storage, callable_storage] = /*getCallableStorage(callable_align, callable_size);*/ getCallableStorage<Callable>();

        if constexpr(fixedSize)
            if (!fp_storage || !callable_storage)
                return false;


        new(fp_storage)
                CallableCxt{.fp_offset  {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke < Callable > ) -
                                                               fp_base)},
                .stride = static_cast<uint16_t>(std::distance(static_cast<std::byte *>(fp_storage),
                                                              static_cast<std::byte *>(callable_storage)) +
                                                sizeof(Callable)),
                .callable_offset = static_cast<uint16_t>(std::distance(static_cast<std::byte *>(fp_storage),
                                                                       static_cast<std::byte *>(callable_storage)))};
        new(callable_storage) Callable{std::forward<T>(function)};

        if constexpr (synced) {
            m_RemainingFront.fetch_add(1, std::memory_order::release);
            m_Remaining.fetch_add(1, std::memory_order::release);
        } else {
            ++m_Remaining;
        }

        if constexpr(fixedSize)
            return true;
    }

    template<typename Callable, typename ...CArgs>
    bool emplace_back(CArgs &&... constructor_args) noexcept {
        static_assert(std::is_trivially_copyable_v<Callable>);

        constexpr auto callable_align = alignof(Callable);
        constexpr auto callable_size = sizeof(Callable);

        auto[fp_storage, callable_storage] = getCallableStorage(callable_align, callable_size);

        if (!fp_storage || !callable_storage) return false;

        new(fp_storage) InvokeAndDestroy{invoke < Callable > };
        new(callable_storage) Callable{std::forward<CArgs>(constructor_args)...};

        m_InputPos = reinterpret_cast<std::byte *>(callable_storage) + callable_size;
        ++m_Remaining;

        return true;
    }

    explicit operator bool() {
        if constexpr (synced) {
            size_t rem = m_Remaining.load(std::memory_order::relaxed);
            if (!rem) return false;

            while (rem && !m_Remaining.compare_exchange_weak(rem, rem - 1,
                                                             std::memory_order::acquire,
                                                             std::memory_order::relaxed));
            return rem;
        } else
            return m_Remaining;
    }

    template<typename T=R>
    requires synced
    inline R callAndPop(Args ... args) noexcept {
        CallableCxt *callable_cxt;
        auto outPtr = m_OutPosReadCurrent.load(std::memory_order::seq_cst);

        while (!m_OutPosReadCurrent.compare_exchange_weak(outPtr, [&, outPtr]() mutable {
            callable_cxt = align<CallableCxt>(outPtr);

            if (callable_cxt->fp_offset.load(std::memory_order_relaxed) == std::numeric_limits<uint32_t>::max()) {
                callable_cxt = align<CallableCxt>(m_CallableQueueMemory);
            }

            return reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->stride;
        }(), std::memory_order_relaxed, std::memory_order_relaxed));

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base +
                                               callable_cxt->fp_offset.load(std::memory_order_relaxed))(
                    reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->callable_offset, args...);

            callable_cxt->fp_offset.store(0, std::memory_order_release);

        } else {
            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base +
                                                               callable_cxt->fp_offset.load(std::memory_order_relaxed))(
                    reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->callable_offset, args...);

            callable_cxt->fp_offset.store(0, std::memory_order_release);

            return std::move(result);
        }
    }

    template<typename T=R>
    requires (!synced)
    inline R callAndPop(Args ... args) noexcept {
        if (m_OutPosReadCurrent == m_OutPutEndPos) m_OutPosReadCurrent = m_CallableQueueMemory;
        auto const callable_cxt = align<CallableCxt>(m_OutPosReadCurrent);

        m_OutPosReadCurrent = reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->stride;
        --m_Remaining;

        if constexpr (std::is_same_v<R, void>)
            reinterpret_cast<InvokeAndDestroy>(fp_base + callable_cxt->fp_offset)(
                    reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->callable_offset, args...);
        else
            return reinterpret_cast<InvokeAndDestroy>(fp_base + callable_cxt->fp_offset)(
                    reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->callable_offset, args...);
    }

private:
    using Storage = std::pair<void *, void *>;

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

    template<typename T=R>
    requires(synced &&fixedSize)
    std::pair<std::byte *, size_t> checkNAdvanceOutPos() noexcept {
        auto check_pos = [&](std::byte *outPtr) mutable {
            auto callable_cxt = align<CallableCxt>(outPtr);
            auto fp_offset = callable_cxt->fp_offset.load(std::memory_order_acquire);

            if (fp_offset == std::numeric_limits<uint32_t>::max()) {
                callable_cxt = align<CallableCxt>(m_CallableQueueMemory);
                fp_offset = callable_cxt->fp_offset.load(std::memory_order_acquire);
            }

            return std::pair{bool{!fp_offset},
                             reinterpret_cast<std::byte *>(callable_cxt) + callable_cxt->stride};
        };

        std::byte *outPtr = m_OutPosStart.load(std::memory_order::relaxed);
        size_t rem = m_RemainingFront.load(std::memory_order::relaxed);
        for (auto[advance, nextOutPos] = check_pos(outPtr);
             advance && rem; std::tie(advance, nextOutPos) = check_pos(outPtr)) {
            assert(outPtr != nextOutPos);
            if (m_OutPosStart.compare_exchange_strong(outPtr, nextOutPos, std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
                outPtr = nextOutPos;
                auto prev_rem = m_RemainingFront.fetch_sub(1, std::memory_order::relaxed);
                rem = prev_rem - 1;
                if (!rem) {
                    break;
                } else if (prev_rem == 0) {
                    printf("fucked up !!!\n");
                }
            }
        }

        return {outPtr, rem};
    }

    template<typename Callable>
    requires(synced &&fixedSize)
    Storage getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = align<CallableCxt>(buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(CallableCxt);
            size -= sizeof(CallableCxt);
            auto const callable_storage = align<Callable>(buffer, size);
            return {fp_storage, callable_storage};
        };

        Storage storage{nullptr, nullptr};
        std::byte *input_pos;

        bool search_ahead;
        do {
            input_pos = m_InputPos.load(std::memory_order::relaxed);
            auto const[output_pos, remaining] = checkNAdvanceOutPos();

            search_ahead = (input_pos > output_pos) || (input_pos == output_pos && !remaining);

            if (size_t const buffer_size = m_CallableQueueMemory + m_CallableQueueMemorySize - input_pos;
                    search_ahead && buffer_size) {
                storage = getAlignedStorage(input_pos, buffer_size);
                if (storage.first && storage.second) {
                    search_ahead = false;
                    continue;
                }
            }

            {
                auto const mem = search_ahead ? m_CallableQueueMemory : input_pos;
                if (size_t const buffer_size = output_pos - mem) {
                    storage = getAlignedStorage(mem, buffer_size);
                }
            }

            if (!storage.first || !storage.second)
                return {nullptr, nullptr};

        } while (!m_InputPos.compare_exchange_weak(input_pos,
                                                   static_cast<std::byte *>(storage.second) + sizeof(Callable),
                                                   std::memory_order::relaxed, std::memory_order::relaxed));

        if (search_ahead) {
            auto callable_cxt = align<CallableCxt>(input_pos);
            callable_cxt->fp_offset = std::numeric_limits<uint32_t>::max();
        }

        return storage;
    }


    template<typename Callable>
    requires (!synced)
    constexpr Storage getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept -> Storage {
            auto const fp_storage = align<CallableCxt>(buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(CallableCxt);
            size -= sizeof(CallableCxt);
            auto const callable_storage = align<Callable>(buffer, size);
            return {fp_storage, callable_storage};
        };

        Storage storage{nullptr, nullptr};

        auto incrInputPos = [&] { m_InputPos = reinterpret_cast<std::byte *>(storage.second) + sizeof(Callable); };

        bool const search_ahead =
                (m_InputPos > m_OutPosReadCurrent) || (m_InputPos == m_OutPosReadCurrent && !m_Remaining);
        if (size_t const buffer_size = m_CallableQueueMemory + m_CallableQueueMemorySize - m_InputPos;
                search_ahead && buffer_size) {
            storage = getAlignedStorage(m_InputPos, buffer_size);
            if (storage.first && storage.second) {
                incrInputPos();
                return storage;
            }
        }

        auto const mem = search_ahead ? m_CallableQueueMemory : m_InputPos;
        if (size_t const buffer_size = m_OutPosReadCurrent - mem) {
            storage = getAlignedStorage(mem, buffer_size);
            if (storage.first && storage.second) {
                incrInputPos();
                if (search_ahead)
                    m_OutPutEndPos = m_InputPos;
            }
        }

        if constexpr (fixedSize) {
            return storage;
        } else {
            /// no space left in queue, must allocate a bigger buffer
            size_t const callableMem =
                    sizeof(Callable) + alignof(Callable) + sizeof(CallableCxt) + alignof(CallableCxt);
            size_t const newQueueSize = std::max(static_cast<size_t>(m_CallableQueueMemorySize * GROWTH_FACTOR),
                                                 m_CallableQueueMemorySize + callableMem);
            void *const newQueueMem = m_MemoryResource->allocate(newQueueSize, alignof(CallableCxt));

            /// trivially relocating callables into new queue memory /****************
            if (m_Remaining && m_OutPosReadCurrent >= m_InputPos) {
                size_t const rightMemSize = m_OutPutEndPos - m_OutPosReadCurrent;
                std::memcpy(newQueueMem, m_OutPosReadCurrent, rightMemSize);

                void *nextMem = align<CallableCxt>(static_cast<std::byte *>(newQueueMem) + rightMemSize);
                size_t leftMemSize = m_InputPos - m_CallableQueueMemory;
                std::memcpy(nextMem, m_CallableQueueMemory, leftMemSize);

                m_InputPos = static_cast<std::byte *>(nextMem) + leftMemSize;
            } else if (m_OutPosReadCurrent < m_InputPos) {
                size_t leftMemSize = m_InputPos - m_OutPosReadCurrent;
                std::memcpy(newQueueMem, m_OutPosReadCurrent, leftMemSize);
                m_InputPos = static_cast<std::byte *>(newQueueMem) + leftMemSize;
            } else {
                m_InputPos = static_cast<std::byte *>(newQueueMem);
            }
            /// ****************/

            if (m_CallableQueueMemory)
                m_MemoryResource->deallocate(m_CallableQueueMemory, m_CallableQueueMemorySize, alignof(CallableCxt));
            m_CallableQueueMemory = static_cast<std::byte *>(newQueueMem);
            m_CallableQueueMemorySize = newQueueSize;
            m_OutPutEndPos = nullptr;
            m_OutPosReadCurrent = m_CallableQueueMemory;

            storage = getAlignedStorage(m_InputPos, m_CallableQueueMemory + m_CallableQueueMemorySize - m_InputPos);
            incrInputPos();
            return storage;
        }
    }

    /*constexpr Storage getCallableStorage(size_t callable_align, size_t callable_size) noexcept {
        auto getAlignedStorage = [callable_size, callable_align]
                (void *buffer, size_t size) noexcept -> std::pair<void *, void *> {
            auto const fp_storage = std::align(alignof(CallableCxt), sizeof(CallableCxt), buffer, size);
            buffer = static_cast<std::byte *>(buffer) + sizeof(CallableCxt);
            size -= sizeof(CallableCxt);
            auto const callable_storage = std::align(callable_align, callable_size, buffer, size);
            return {fp_storage, callable_storage};
        };

        bool const search_ahead = (m_InputPos > m_OutPosReadCurrent) || (m_InputPos == m_OutPosReadCurrent && !m_Remaining);
        if (size_t const buffer_size = m_CallableQueueMemory + m_CallableQueueMemorySize - m_InputPos;
                search_ahead && buffer_size) {
            auto[fp_storage, callable_storage] = getAlignedStorage(m_InputPos, buffer_size);
            if (fp_storage && callable_storage) {
                return {fp_storage, callable_storage};
            }
        }

        auto const mem = search_ahead ? m_CallableQueueMemory : m_InputPos;
        if (size_t const buffer_size = m_OutPosReadCurrent - mem) {
            auto[fp_storage, callable_storage] = getAlignedStorage(mem, buffer_size);
            if (fp_storage && callable_storage) {
                if (search_ahead) m_OutPutEndPos = m_InputPos;
                return {fp_storage, callable_storage};
            }
        }

        if constexpr (fixedSize)
            return {nullptr, nullptr};
        else {
            /// no space left in queue, must allocate a bigger buffer
            size_t const callableMem =
                    callable_size + callable_align + sizeof(CallableCxt) + alignof(CallableCxt);
            size_t const newQueueSize = std::max(static_cast<size_t>(m_CallableQueueMemorySize * GROWTH_FACTOR),
                                                 m_CallableQueueMemorySize + callableMem);
            void *const newQueueMem = m_MemoryResource->allocate(newQueueSize, alignof(CallableCxt));

            /// trivially relocating callables into new queue memory ****************
            if (m_Remaining && m_OutPosReadCurrent >= m_InputPos) {
                size_t const rightMemSize = m_OutPutEndPos - m_OutPosReadCurrent;
                std::memcpy(newQueueMem, m_OutPosReadCurrent, rightMemSize);

                void *nextMem = align<CallableCxt>(static_cast<std::byte *>(newQueueMem) + rightMemSize);
                size_t leftMemSize = m_InputPos - m_CallableQueueMemory;
                std::memcpy(nextMem, m_CallableQueueMemory, leftMemSize);

                m_InputPos = static_cast<std::byte *>(nextMem) + leftMemSize;
            } else if (m_OutPosReadCurrent < m_InputPos) {
                size_t leftMemSize = m_InputPos - m_OutPosReadCurrent;
                std::memcpy(newQueueMem, m_OutPosReadCurrent, leftMemSize);
                m_InputPos = static_cast<std::byte *>(newQueueMem) + leftMemSize;
            } else {
                m_InputPos = static_cast<std::byte *>(newQueueMem);
            }
            /// ****************

            if (m_CallableQueueMemory)
                m_MemoryResource->deallocate(m_CallableQueueMemory, m_CallableQueueMemorySize, alignof(CallableCxt));
            m_CallableQueueMemory = static_cast<std::byte *>(newQueueMem);
            m_CallableQueueMemorySize = newQueueSize;
            m_OutPutEndPos = nullptr;
            m_OutPosReadCurrent = m_CallableQueueMemory;

            return getAlignedStorage(m_InputPos,
                                     m_CallableQueueMemory + m_CallableQueueMemorySize - m_InputPos);
        }
    }*/

private:
    static R baseFP(Args...) noexcept {
        if constexpr (!std::is_same_v<R, void>) {
            return R{};
        }
    }

    class Empty {
    };

    using InvokeAndDestroy = R(*)(void *data, Args...);
    static constexpr double GROWTH_FACTOR = /*2.0*/ 1.61803398875;
    static inline const uintptr_t fp_base = reinterpret_cast<uintptr_t>(&invoke<decltype(&baseFP)>) &
                                            (static_cast<uintptr_t>(std::numeric_limits<uint32_t>::max()) << 32u);

    std::conditional_t<synced, std::atomic<std::byte *>, std::byte *> m_OutPosReadCurrent;
    std::conditional_t<synced && fixedSize, Empty, std::byte *> m_OutPutEndPos;
    std::conditional_t<synced, std::atomic<size_t>, size_t> m_Remaining;

    std::conditional_t<synced, std::atomic<std::byte *>, std::byte *> m_InputPos;
    [[no_unique_address]] std::conditional_t<synced, std::atomic<std::byte *>, Empty> m_OutPosStart;
    [[no_unique_address]] std::conditional_t<synced, std::atomic<size_t>, Empty> m_RemainingFront;

    std::conditional_t<fixedSize, std::byte *const, std::byte *> m_CallableQueueMemory;
    std::conditional_t<fixedSize, size_t const, size_t> m_CallableQueueMemorySize;
    [[no_unique_address]] std::conditional_t<fixedSize, Empty, std::pmr::memory_resource *> m_MemoryResource;

    struct CallableCxt {
        std::atomic<uint32_t> fp_offset;
        uint16_t stride{};
        uint16_t callable_offset{};
    };
};

#endif //CPP_PROJECT_FUNCTIONQUEUE2_H
