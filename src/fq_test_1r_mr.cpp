#include <thread>

#include "SyncFuctionQueue.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"


using namespace util;

using ComputeFunctionSig = size_t(size_t);
using LockFreeQueue = SyncFunctionQueue</*true, true,*/ ComputeFunctionSig>;

int main(int argc, char **argv) {
    size_t const rawQueueMemSize =
            [&] { return (argc >= 2) ? atof(argv[1]) : 1000; }() * 1024 * 1024;

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 20);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const numWriterThreads = [&] { return (argc >= 4) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total writer threads :", numWriterThreads);

    size_t const numReaderThreads = [&] {
        return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency();
    }();
    println("total reader threads :", numReaderThreads);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    { /// writing functions to the queue concurrently ::::
        std::vector<std::thread> writer_threads;
        Timer timer{"data write time :"};

        for (auto t = numWriterThreads; t--;) {
            writer_threads.emplace_back([=, &rawComputeQueue] {
                auto func = 3'100'000;
                CallbackGenerator callbackGenerator{seed};
                while (func) {
                    callbackGenerator.addCallback(
                            [&]<typename T>(T &&t) {
                                while (!rawComputeQueue.push_back(std::forward<T>(t))) {
                                    std::this_thread::yield();
                                }
                                --func;
                            });
                }

                println("thread ", t, " finished");
            });
        }

        for (auto &&t:writer_threads)
            t.join();
    }
    println(numWriterThreads, " writer threads joined\n");


    std::vector<size_t> result_vector;
    std::mutex res_vec_mut;

    { /// reading functions from the queue concurrently ::::
        std::vector<std::thread> reader_threads;
        Timer timer{"data read time :"};

        for (auto t = numReaderThreads; t--;) {
            reader_threads.emplace_back([=, &rawComputeQueue, &result_vector, &res_vec_mut] {
                uint32_t func{0};
                std::vector<size_t> data;
                while (rawComputeQueue) {
                    auto const res = rawComputeQueue.callAndPop(seed);
                    data.push_back(res);
                    ++func;
                }

                println("thread ", t, " read ", func, " functions");

                std::lock_guard lock{res_vec_mut};
                result_vector.insert(result_vector.end(), data.begin(), data.end());
            });
        }

        for (auto &&t:reader_threads)
            t.join();
    }
    println(numReaderThreads, " reader threads joined\n");

    println("result vector size : ", result_vector.size());
    print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    println("result vector sorted");

    size_t hash = seed;
    for (auto r : result_vector)
        hash ^= r + r * hash;
    println("result : ", hash);
}
