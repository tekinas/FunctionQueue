#ifndef FUNCTIONQUEUE_CONCURRENTFUNCTIONQUEUE_H
#define FUNCTIONQUEUE_CONCURRENTFUNCTIONQUEUE_H

#include <atomic>
#include <cstring>
#include <mutex>

#include <boost/lockfree/queue.hpp>
#include <tbb/concurrent_hash_map.h>

template<typename FunctionSignature>
class ConcurrentFunctionQueue {

};

template<typename R, typename ...Args>
class ConcurrentFunctionQueue<R(Args...)> {
private:
    struct MemCxt {
        uint32_t offset/*: 23*/ {};
        uint32_t size/*: 9*/ {};
    };

    struct FunctionCxt {
        uint32_t fp_offset{};
        MemCxt obj{};
    };

    std::atomic<size_t> m_RemainingRead;

    std::atomic<std::byte *> m_InputPos;
    std::atomic<std::byte *> m_OutPosFollow;
    std::atomic<size_t> m_RemainingClean;
    std::atomic<size_t> m_Remaining;

    boost::lockfree::queue<FunctionCxt/*, boost::lockfree::capacity<100>*/> m_ReadFunctions;

    tbb::concurrent_hash_map<uint32_t, uint32_t> m_FreePtrSet;

    std::mutex outPosMut;

    std::byte *const m_Memory;
    size_t const m_MemorySize;

    template<typename Callable>
    static R invoke(void *data, Args... args) {
        auto &functor = *std::launder(align<Callable>(data));
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

    template<typename T>
    static inline void *
    check_storage(void *ptr, size_t space) noexcept {
        const auto intptr = reinterpret_cast<uintptr_t>(ptr);
        const auto aligned = (intptr - 1u + alignof(T)) & -alignof(T);
        const auto diff = aligned - intptr;
        if ((sizeof(T) + diff) > space)
            return nullptr;
        else
            return reinterpret_cast<void *>(aligned);
    }

    template<typename Callable>
    MemCxt getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept {
            return static_cast<std::byte *>(align<Callable>(buffer, size));
        };

        MemCxt memCxt;
        std::byte *input_pos;

        bool search_ahead;
        do {
            memCxt = {};
            auto const out_pos = checkNAdvanceOutPos();
            input_pos = m_InputPos.load();

            if (out_pos == input_pos && m_Remaining.load()) {
                return {};
            }

            search_ahead = input_pos >= out_pos;

            if (size_t const buffer_size = m_Memory + m_MemorySize - input_pos; search_ahead && buffer_size) {
                if (auto obj_buff = getAlignedStorage(input_pos, buffer_size)) {
                    search_ahead = false;
                    memCxt = {.offset = static_cast<uint32_t>(std::distance(m_Memory, input_pos)),
                            .size = static_cast<uint32_t>(std::distance(input_pos, obj_buff + sizeof(Callable)))};
                    continue;
                }
            }

            {
                auto const mem = search_ahead ? m_Memory : input_pos;
                if (size_t const buffer_size = out_pos - mem) {
                    if (auto obj_buff = getAlignedStorage(mem, buffer_size)) {
                        memCxt = {.offset = static_cast<uint32_t>(std::distance(m_Memory, mem)),
                                .size = static_cast<uint32_t>(std::distance(mem, obj_buff + sizeof(Callable)))};

                    }
                }
            }

            if (!memCxt.size) return {};

        } while (!m_InputPos.compare_exchange_weak(input_pos, m_Memory + (memCxt.offset + memCxt.size),
                                                   std::memory_order_relaxed, std::memory_order_relaxed));

        if (search_ahead) {
            m_FreePtrSet.emplace(std::distance(m_Memory, input_pos), 0);
        }

        return memCxt;
    }

    std::byte *checkNAdvanceOutPos() noexcept {
        std::byte *out_pos;
        size_t rem;
        {
            std::lock_guard lock{outPosMut};
            out_pos = m_OutPosFollow.load(std::memory_order_relaxed);
            rem = m_RemainingClean.load(std::memory_order_relaxed);
        }

        if (!rem || !outPosMut.try_lock()) {
            return out_pos;
        }

        std::lock_guard lock{outPosMut, std::adopt_lock};

        out_pos = m_OutPosFollow.load(std::memory_order_relaxed);
        rem = m_RemainingClean.load(std::memory_order_relaxed);

        uint32_t outpos_offset = std::distance(m_Memory, out_pos);

        auto cleaned = 0;
        for (; cleaned != rem;) {
            typename decltype(m_FreePtrSet)::accessor stride_iter;
            bool found = m_FreePtrSet.find(stride_iter, outpos_offset);

            if (!found) break;
            else if (stride_iter->second) {
                outpos_offset += stride_iter->second;
                ++cleaned;
            } else {
                outpos_offset = 0;
            }

            m_FreePtrSet.erase(stride_iter);
        }


        if (cleaned) {
            m_RemainingClean.fetch_sub(cleaned, std::memory_order_relaxed);
            m_Remaining.fetch_sub(cleaned, std::memory_order_relaxed);
        }

        m_OutPosFollow.store(out_pos = m_Memory + outpos_offset, std::memory_order_relaxed);

        return out_pos;
    }


public:

    ConcurrentFunctionQueue(void *mem, size_t size) : m_Memory{static_cast<std::byte *const>(mem)}, m_MemorySize{size},
                                                      m_RemainingRead{0}, m_Remaining{0}, m_RemainingClean{0},
                                                      m_ReadFunctions{1000} {
        m_InputPos = m_OutPosFollow = m_Memory;

        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() {
        size_t rem = m_RemainingRead.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem && !m_RemainingRead.compare_exchange_weak(rem, rem - 1,
                                                             std::memory_order_acquire, std::memory_order_relaxed));
        return rem;
    }

    inline R callAndPop(Args ... args) noexcept {
        FunctionCxt functionCxt;

        while (m_ReadFunctions.empty());
        m_ReadFunctions.pop(functionCxt);

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(m_Memory + functionCxt.obj.offset,
                                                                                args...);

            m_FreePtrSet.emplace(functionCxt.obj.offset, functionCxt.obj.size);
            m_RemainingClean.fetch_add(1, std::memory_order_release);
        } else {

            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    m_Memory + functionCxt.obj.offset, args...);

            m_FreePtrSet.emplace(functionCxt.obj.offset, functionCxt.obj.size);
            m_RemainingClean.fetch_add(1, std::memory_order_release);

            return std::move(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;

        MemCxt const memCxt = getCallableStorage<Callable>();

        if (!memCxt.size)
            return false;

        new(align<Callable>(m_Memory + memCxt.offset)) Callable{std::forward<T>(function)};
        m_ReadFunctions.push(
                {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke<Callable> ) - fp_base), memCxt});

        m_Remaining.fetch_add(1, std::memory_order_release);
        m_RemainingRead.fetch_add(1, std::memory_order_release);

        return true;
    }
};

#endif //FUNCTIONQUEUE_CONCURRENTFUNCTIONQUEUE_H
