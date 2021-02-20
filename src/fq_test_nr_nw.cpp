#include <thread>
#include <string>

#include "MRMW_FunctionQueue.h"
#include "util.h"
#include "ComputeCallbackGenerator.h"


using namespace util;

using ComputeFunctionSig = void();
using LockFreeQueue = MRMW_FunctionQueue</*true, true, */ComputeFunctionSig>;

void test_lockFreeQueue(LockFreeQueue &rawComputeQueue, CallbackGenerator &callbackGenerator, size_t functions);

struct ComputeCxt {
private:
    size_t num{};
    size_t func{};
    size_t const num_functions;
    CallbackGenerator callbackGenerator;
    Timer timer;

public:
    explicit ComputeCxt(size_t seed, size_t num_functions, std::string_view timer_str) : num_functions{num_functions},
                                                                                         callbackGenerator{seed},
                                                                                         timer{timer_str} {}

    static void addComputeTask(std::unique_ptr<ComputeCxt> computeCxt, LockFreeQueue *functionQueue) noexcept {
        auto const cxtPtr = computeCxt.get();
        cxtPtr->callbackGenerator.addCallback(
                [computeCxt{std::move(computeCxt)}, functionQueue]<typename T>(T &&t) mutable {
                    auto compute = [computeCxt{std::move(computeCxt)}, t{std::forward<T>(t)}, functionQueue]() mutable {
                        computeCxt->num = t(computeCxt->num);
                        println(computeCxt->num);

                        if (++computeCxt->func != computeCxt->num_functions)
                            ComputeCxt::addComputeTask(std::move(computeCxt), functionQueue);
                        else println("result :", computeCxt->num);
                    };

                    while (!functionQueue->push_back(std::move(compute))) {
                        std::this_thread::yield();
                    }
                });
    }
};

int main(int argc, char **argv) {
    size_t const rawQueueMemSize =
            [&] { return (argc >= 2) ? atof(argv[1]) : 10000 / 1024.0 / 1024.0; }() * 1024 * 1024;

    auto const rawQueueMem = std::make_unique<uint8_t[]>(rawQueueMemSize + 10);
    println("using buffer of size :", rawQueueMemSize);

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : 2/*std::thread::hardware_concurrency()*/; }();
    println("total num_threads :", num_threads);

    size_t const compute_chains = [&] {
        return (argc >= 6) ? atol(argv[5]) : 2/*std::thread::hardware_concurrency()*/;
    }();
    println("total compute chains :", compute_chains);

    LockFreeQueue rawComputeQueue{rawQueueMem.get(), rawQueueMemSize};

    for (auto t = compute_chains; t--;)
        ComputeCxt::addComputeTask(
                std::make_unique<ComputeCxt>(seed, functions, "compute chain " + std::to_string(t + 1)),
                &rawComputeQueue);

    std::vector<std::thread> threads;
    for (auto t = num_threads; t--;)
        threads.emplace_back([&rawComputeQueue] {
            while (rawComputeQueue) rawComputeQueue.callAndPop();
        });

    for (auto &&t:threads)
        t.join();
}
