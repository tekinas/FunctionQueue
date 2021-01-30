#include <thread>
#include <string>

#include "FunctionQueue.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"


using namespace util;

using ComputeFunctionSig = size_t(size_t);
using LockFreeQueue = FunctionQueue<true, true, ComputeFunctionSig>;

int main(int argc, char **argv) {
    size_t const rawQueueMemSize =
            [&] { return (argc >= 2) ? atof(argv[1]) : 10000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 10);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total num_threads :", num_threads);

    FunctionQueue<true, true, ComputeFunctionSig> rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    std::vector<size_t> result_vector;
    std::mutex result_mut;
    std::vector<std::thread> reader_threads;

    for (auto t = num_threads; t--;)
        reader_threads.emplace_back([&, str{"thread " + std::to_string(t + 1)}] {
            std::vector<size_t> res_vec;
            {
                Timer timer{str};

                while (true) {
                    while (!rawComputeQueue) std::this_thread::yield();
                    auto const res = rawComputeQueue.callAndPop(seed);
                    if (res == std::numeric_limits<size_t>::max()) break;
                    else res_vec.push_back(res);
                }
            }

            std::lock_guard lock{result_mut};
            println("numbers computed : ", res_vec.size());
            result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
        });

    [=, &rawComputeQueue] {
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

        for (auto t = num_threads; t--;)
            while (!rawComputeQueue.push_back([](auto) { return std::numeric_limits<size_t>::max(); }));
    }();

    for (auto &&t:reader_threads)
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
