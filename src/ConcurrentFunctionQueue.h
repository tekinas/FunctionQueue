#ifndef FUNCTIONQUEUE_ConcurrentFunctionQueue_H
#define FUNCTIONQUEUE_ConcurrentFunctionQueue_H

#include <atomic>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <iostream>

#include <boost/lockfree/queue.hpp>
#include <tbb/concurrent_hash_map.h>
#include <queue>

#include <absl/hash/hash.h>

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

    struct MemPos {
    private:
        uint32_t hash{};
        uint32_t offset{};

    public:
        template<typename H>
        friend H AbslHashValue(H h, const MemPos &c) {
            return H::combine(std::move(h), c.hash, c.offset);
        }

        MemPos getNext(uint32_t next_offset) const noexcept {
            MemPos memPos;
            memPos.hash = absl::Hash<MemPos>{}(*this);
            memPos.offset = next_offset;
            return memPos;
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

    boost::lockfree::queue<FunctionCxt/*, boost::lockfree::capacity<100>*/>
    /*std::queue<FunctionCxt>*/ m_ReadFunctions;

    tbb::concurrent_hash_map<uint32_t, uint32_t>
    /*std::unordered_map<uint32_t, uint32_t>*/ m_FreePtrSet/*, m_WritingSet*/;

    std::shared_mutex outPosMut;
    std::mutex outPosUpdateMut;

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

    template<typename Callable>
    MemCxt getCallableStorage() noexcept {
        auto getAlignedStorage = [](void *buffer, size_t size) noexcept {
            return static_cast<std::byte *>(align<Callable>(buffer, size));
        };

        MemCxt memCxt;
        MemPos input_mem = m_InputPos.load();;

        bool search_ahead;
        m_Writing.fetch_add(1);

        do {
            memCxt = {};
            auto const[out_pos, rem] = checkNAdvanceOutPos();
            auto const input_pos = input_mem.memory(m_Memory);

            m_Writing.fetch_sub(1);

            if (out_pos == input_pos) {
                if (rem || m_Writing.load())
                    return {};
            }

            search_ahead = input_pos >= out_pos;

            if (size_t const buffer_size = m_Memory + m_MemorySize - input_pos; search_ahead && buffer_size) {
                if (auto obj_buff = getAlignedStorage(input_pos, buffer_size)) {
                    search_ahead = false;
                    memCxt = {.offset = static_cast<uint32_t>(std::distance(m_Memory, input_pos)),
                            .size = static_cast<uint32_t>(std::distance(input_pos, obj_buff + sizeof(Callable)))};
                    goto INCR_WRITING;
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

            INCR_WRITING:
            /*{
                tbb::concurrent_hash_map<uint32_t, uint32_t>::accessor write_offset;
                if (m_WritingSet.find(write_offset, memCxt.offset)) {
                    std::cout << "error : someone else is writing at " << memCxt.offset << std::endl;
                    return {};
                }
            }*/

            if (m_Writing.fetch_add(1) == UINT64_MAX) {
                std::cout << "error : writing is now zero" << std::endl;
            }

        } while (!m_InputPos.compare_exchange_weak(input_mem, input_mem.getNext(memCxt.offset + memCxt.size)));

        /*if (!m_WritingSet.emplace(memCxt.offset, memCxt.size)) {
            std::cout << "error : someone else is writing at " << memCxt.offset << std::endl;
            return {};
        }*/

        if (search_ahead) {
            m_FreePtrSet.emplace(std::distance(m_Memory, input_mem.memory(m_Memory)), 0);
        }

        return memCxt;
    }

    std::pair<std::byte *, size_t> checkNAdvanceOutPos() noexcept {
        std::byte *out_pos;
        size_t rem;
        {
            std::shared_lock lock{outPosMut};
            out_pos = m_OutPosFollow.load();
            rem = m_Remaining.load();
        }

        if (!rem || !outPosUpdateMut.try_lock()) {
            return {out_pos, rem};
        }

        std::lock_guard lock{outPosUpdateMut, std::adopt_lock};
        out_pos = m_OutPosFollow.load();

        uint32_t output_offset = std::distance(m_Memory, out_pos);

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


        {
            std::lock_guard outPosUpdateLock{outPosMut};

            if (cleaned) {
                rem = m_Remaining.fetch_sub(cleaned) - cleaned;
            } else rem = m_Remaining.load();

            out_pos = m_Memory + output_offset;
            m_OutPosFollow.store(out_pos);
        }

        return {out_pos, rem};
    }


public:

    ConcurrentFunctionQueue(void *mem, size_t size) : m_Memory{static_cast<std::byte *const>(mem)}, m_MemorySize{size},
                                                      m_RemainingRead{0}, m_Remaining{0}, m_ReadFunctions{1000},
                                                      m_InputPos{} {
        m_OutPosFollow = m_Memory;
        printf("init input :%u\n", std::distance(m_Memory, m_InputPos.load().memory(m_Memory)));
        printf("init output :%u\n", std::distance(m_Memory, m_OutPosFollow.load()));

        memset(m_Memory, 0, m_MemorySize);
    }

    explicit operator bool() {
        size_t rem = m_RemainingRead.load();
        if (!rem) return false;

        while (rem && !m_RemainingRead.compare_exchange_weak(rem, rem - 1));
        return rem;
    }

    inline R callAndPop(Args ... args) noexcept {
        FunctionCxt functionCxt;

        while (m_ReadFunctions.empty()) std::this_thread::yield();
        m_ReadFunctions.pop(functionCxt);

        /*tbb::concurrent_hash_map<uint32_t, uint32_t>::accessor write_offset;
        if (!m_WritingSet.find(write_offset, functionCxt.obj.offset)) {
            std::cout << "error : no data written here " << functionCxt.obj.offset << std::endl;
        }*/

        if constexpr (std::is_same_v<R, void>) {
            reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(m_Memory + functionCxt.obj.offset,
                                                                                args...);
            std::atomic_thread_fence(std::memory_order_release);

            m_FreePtrSet.emplace(functionCxt.obj.offset, functionCxt.obj.size);
//            m_WritingSet.erase(write_offset);

        } else {
            auto &&result = reinterpret_cast<InvokeAndDestroy>(fp_base + functionCxt.fp_offset)(
                    m_Memory + functionCxt.obj.offset, args...);

            std::atomic_thread_fence(std::memory_order_release);

            m_FreePtrSet.emplace(functionCxt.obj.offset, functionCxt.obj.size);
//            m_WritingSet.erase(write_offset);

            return std::move(result);
        }
    }

    template<typename T>
    bool push_back(T &&function) noexcept {
        using Callable = std::decay_t<T>;

        MemCxt const memCxt = getCallableStorage<Callable>();

        if (!memCxt.size)
            return false;

        m_Remaining.fetch_add(1);

        new(align<Callable>(m_Memory + memCxt.offset)) Callable{std::forward<T>(function)};

        m_ReadFunctions.push(
                {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&invoke < Callable > ) - fp_base), memCxt});

        m_RemainingRead.fetch_add(1);

        m_Writing.fetch_sub(1);

        return true;
    }

};

#endif //FUNCTIONQUEUE_ConcurrentFunctionQueue_H
