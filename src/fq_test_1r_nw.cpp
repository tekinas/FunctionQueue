#include <thread>

#include "MRMW_FunctionQueue.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"


using namespace util;

using ComputeFunctionSig = size_t(size_t);
using LockFreeQueue = MRMW_FunctionQueue</*true, true,*/ ComputeFunctionSig>;

int main(int argc, char **argv) {
    size_t const rawQueueMemSize =
            [&] { return (argc >= 2) ? atof(argv[1]) : 300 / 1024.0 / 1024.0; }() * 1024 * 1024;

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 10);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 23423; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : 2/*std::thread::hardware_concurrency()*/; }();
    println("total num_threads :", num_threads);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    std::vector<size_t> result_vector;
    std::vector<std::thread> writer_threads;

    for (auto t = num_threads; t--;)
        writer_threads.emplace_back([=, &rawComputeQueue] {
            auto func = functions;
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

            while (!rawComputeQueue.push_back([](auto) { return std::numeric_limits<size_t>::max(); }));
        });

    {
        Timer timer{"reader thread "};
        auto threads = num_threads;

        while (threads) {
            while (!rawComputeQueue) std::this_thread::yield();
            auto const res = rawComputeQueue.callAndPop(seed);
//            printf("%lu\n", res);
            if (res == std::numeric_limits<size_t>::max()) --threads;
            else result_vector.push_back(res);
        }
    }

    for (auto &&t:writer_threads)
        t.join();

    println("result vector size : ", result_vector.size());
    print("sorting result vector .... ");
    std::sort(result_vector.begin(), result_vector.end());
    println("result vector sorted");

    size_t hash = seed;
    for (auto r : result_vector)
        hash ^= r;
    println("result : ", hash);
}
