#include <thread>
#include <string>

#include <boost/asio.hpp>
#include "util.h"
#include "ComputeCallbackGenerator.h"


using namespace util;

using ComputeFunctionSig = void();
using LockFreeQueue = boost::asio::io_context;

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
//                        std::cout << computeCxt->num << '\n';

                        if (++computeCxt->func != computeCxt->num_functions)
                            ComputeCxt::addComputeTask(std::move(computeCxt), functionQueue);
                        else println("result :", computeCxt->num);
                    };

                    boost::asio::post(*functionQueue, std::move(compute));
                });
    }
};

int main(int argc, char **argv) {
    if (argc == 1) {
        println("usage : ./fq_test_nr_nw_asio <buffer_size> <seed> <functions> <threads> <compute_chains>");
    }

    size_t const seed = [&] { return (argc >= 3) ? atol(argv[2]) : 100; }();
    println("using seed :", seed);

    size_t const functions = [&] { return (argc >= 4) ? atol(argv[3]) : 12639182; }();
    println("total functions :", functions);

    size_t const num_threads = [&] { return (argc >= 5) ? atol(argv[4]) : std::thread::hardware_concurrency(); }();
    println("total num_threads :", num_threads);

    size_t const compute_chains = [&] {
        return (argc >= 6) ? atol(argv[5]) : std::thread::hardware_concurrency();
    }();
    println("total compute chains :", compute_chains);

    LockFreeQueue rawComputeQueue;

    for (auto t = compute_chains; t--;)
        ComputeCxt::addComputeTask(
                std::make_unique<ComputeCxt>(seed, functions, "compute chain " + std::to_string(t + 1)),
                &rawComputeQueue);

    std::vector<std::thread> threads;
    for (auto t = num_threads; t--;)
        threads.emplace_back([&rawComputeQueue] {
            rawComputeQueue.run();
        });

    for (auto &&t:threads)
        t.join();
}