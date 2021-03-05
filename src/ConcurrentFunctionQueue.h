#ifndef FUNCTIONQUEUE_ConcurrentFunctionQueue_H
#define FUNCTIONQUEUE_ConcurrentFunctionQueue_H

#include <atomic>
#include <type_traits>
#include <tuple>
#include <cassert>
#include <iostream>

#include <boost/lockfree/queue.hpp>
#include <tbb/concurrent_hash_map.h>

#include <absl/hash/hash.h>

template<typename FunctionSignature>
class ConcurrentFunctionQueue {
};

template<typename R, typename ...Args>
class ConcurrentFunctionQueue<R(Args...)> {
private:
    struct MemCxt {
        uint32_t offset: 24 {};
        uint32_t size: 8 {};

        MemCxt() noexcept = default;

        inline void set(std::byte *base, std::byte *mem, std::byte *obj_mem, size_t s) noexcept {
            offset = static_cast<uint32_t>(mem - base);
            size = static_cast<uint32_t>(obj_mem - mem + s);
        }
    };

    struct FunctionCxt {
        uint32_t fp_offset{};
        MemCxt obj{};
    };

    struct MemPos {
    private:
        uint32_t hash{};
        uint32_t offset{};

        MemPos(uint32_t hash, uint32_t offset) : hash{hash}, offset{offset} {}

    public:
        MemPos() noexcept = default;

        template<typename H>
        friend H AbslHashValue(H h, const MemPos &c) {
            return H::combine(std::move(h), c.hash, c.offset);
        }

        MemPos getNext(uint32_t next_offset) const noexcept {
            return {static_cast<uint32_t>(absl::Hash<MemPos>{}(*this)), next_offset};
        }

        std::byte *memory(std::byte *base) const noexcept {
            return base + offset;
        }
    };

    std::atomic<size_t> m_RemainingRead;

    std::atomic<MemPos> m_InputPos;
    std::atomic<std::byte *> m_OutPosFollow;
    std::atomic<size_t> m_Remaining;
    std::atomic<size_t> m_Writing;

    boost::lockfree::queue<FunctionCxt/*, boost::lockfree::capacity<100>*/> m_ReadFunctions;

    tbb::concurrent_hash_map<uint32_t, uint32_t> m_FreePtrSet;

    std::atomic_flag outPosUpdateFlag;

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

    template<typename Callable>
    MemCxt getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *ptr, size_t space) noexcept -> std::byte * {
            const auto intptr = reinterpret_cast<uintptr_t>(ptr);
            const auto aligned = (intptr - 1u + alignof(Callable)) & -alignof(Callable);
            const auto diff = aligned - intptr;
            if ((sizeof(Callable) + diff) > space)
                return nullptr;
            else
                return reinterpret_cast<std::byte *>(aligned);
        };

        MemCxt memCxt;
        MemPos input_mem = m_InputPos.load(std::memory_order_acquire);

        bool search_ahead;
        m_Writing.fetch_add(1, std::memory_order_relaxed);

        do {
            m_Writing.fetch_sub(1, std::memory_order_relaxed);

            memCxt = {};
            auto const out_pos = m_OutPosFollow.load(std::memory_order_acquire);
            auto const rem = m_Remaining.load(std::memory_order_relaxed);

            auto const input_pos = input_mem.memory(m_Memory);

            if (out_pos == input_pos) {
                if (rem || m_Writing.load(std::memory_order_acquire))
                    return {};
            }

            search_ahead = input_pos >= out_pos;

            if (size_t const buffer_size = m_Memory + m_MemorySize - input_pos; search_ahead && buffer_size) {
                if (auto obj_buff = getAlignedStorage(input_pos, buffer_size)) {
                    search_ahead = false;
                    memCxt.set(m_Memory, input_pos, obj_buff, sizeof(Callable));
                    m_Writing.fetch_add(1, std::memory_order_release);
                    continue;
                }
            }

            {
                auto const mem = search_ahead ? m_Memory : input_pos;
                if (size_t const buffer_size = out_pos - mem) {
                    if (auto obj_buff = getAlignedStorage(mem, buffer_size)) {
                        memCxt.set(m_Memory, mem, obj_buff, sizeof(Callable));
                    }
                }
            }

            if (!memCxt.size) {
//                cleanMemory();
                return {};
            }
            m_Writing.fetch_add(1, std::memory_order_release);

        } while (!m_InputPos.compare_exchange_weak(input_mem, input_mem.getNext(memCxt.offset + memCxt.size),
                                                   std::memory_order_release, std::memory_order_acquire));

        if (search_ahead) {
            m_FreePtrSet.emplace(std::distance(m_Memory, input_mem.memory(m_Memory)), 0);
        }

        return memCxt;
    }

    void cleanMemory() noexcept {
        if (!outPosUpdateFlag.test_and_set(std::memory_order_acquire)) {

            uint32_t output_offset = std::distance(m_Memory, m_OutPosFollow.load(std::memory_order_acquire));

            auto cleaned = 0;
            while (!m_FreePtrSet.empty()) {
                tbb::concurrent_hash_map<uint32_t, uint32_t>::accessor stride_iter;
                bool found = m_FreePtrSet.find(stride_iter, output_offset);

                if (!found) break;
                else if (stride_iter->second) {
                    output_offset += stride_iter->second;
                    ++cleaned;
                } else {
                    output_offset = 0;
                }

                m_FreePtrSet.erase(stride_iter);
            }

            if (cleaned) m_Remaining.fetch_sub(cleaned, std::memory_order_relaxed);
            auto const next_out_pos = m_Memory + output_offset;
            m_OutPosFollow.store(next_out_pos, std::memory_order_release);

            outPosUpdateFlag.clear(std::memory_order_release);
        }
    }

    void cleanMemory(MemCxt lastMem) noexcept {
        if (!outPosUpdateFlag.test_and_set(std::memory_order_acquire)) {

            uint32_t output_offset = std::distance(m_Memory, m_OutPosFollow.load(std::memory_order_acquire));
            auto cleaned = 0;

            if (output_offset == lastMem.offset) {
                ++cleaned;
                output_offset += lastMem.size;
            } else {
                m_FreePtrSet.emplace(uint32_t{lastMem.offset}, uint32_t{lastMem.size});
            }

            while (!m_FreePtrSet.empty()) {
                tbb::concurrent_hash_map<uint32_t, uint32_t>::accessor stride_iter;
                bool found = m_FreePtrSet.find(stride_iter, output_offset);

                if (!found) break;
                else if (stride_iter->second) {
                    output_offset += stride_iter->second;
                    ++cleaned;
                } else {
                    output_offset = 0;
                }

                m_FreePtrSet.erase(stride_iter);
            }

            if (cleaned) m_Remaining.fetch_sub(cleaned, std::memory_order_relaxed);
            auto const next_out_pos = m_Memory + output_offset;
            m_OutPosFollow.store(next_out_pos, std::memory_order_release);

            outPosUpdateFlag.clear(std::memory_order_release);
        } else { m_FreePtrSet.emplace(uint32_t{lastMem.offset}, uint32_t{lastMem.size}); }
    }


public:

    ConcurrentFunctionQueue(void *mem, size_t size) : m_Memory{static_cast<std::byte *const>(mem)}, m_MemorySize{size},
                                                      m_RemainingRead{0}, m_Remaining{0}, m_ReadFunctions{1000},
                                                      m_InputPos{} {
        m_OutPosFollow = m_Memory;

        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() {
        size_t rem = m_RemainingRead.load(std::memory_order_relaxed);
        if (!rem) return false;

        while (rem && !m_RemainingRead.compare_exchange_weak(rem, rem - 1, std::memory_order_acquire,
                                                             std::memory_order_relaxed));
        return rem;
    }

    inline R callAndPop(Args ... args) noexcept {
        FunctionCxt functionCxt;

        while (m_ReadFunctions.empty()) std::this_thread::yield();
        m_ReadFunctions.pop(functionCxt);

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(m_Memory + functionCxt.obj.offset,
                                                                                args...);

            cleanMemory(functionCxt.obj);

        } else {
            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    m_Memory + functionCxt.obj.offset, args...);

            cleanMemory(functionCxt.obj);

            return std::move(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;

        MemCxt const memCxt = getCallableStorage<Callable>();

        if (!memCxt.size)
            return false;

        m_Remaining.fetch_add(1, std::memory_order_release);

        std::construct_at(align<Callable>(m_Memory + memCxt.offset), std::forward<T>(function));

        m_ReadFunctions.push(
                {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke<Callable> ) - fp_base), memCxt});

        m_RemainingRead.fetch_add(1, std::memory_order_release);

        m_Writing.fetch_sub(1, std::memory_order_release);

        return true;
    }

};

#endif //FUNCTIONQUEUE_ConcurrentFunctionQueue_H
