//
// Created by tekinas on 1/5/21.
//

#ifndef FUNCTIONQUEUE_COMPUTECALLBACKGENERATOR_H
#define FUNCTIONQUEUE_COMPUTECALLBACKGENERATOR_H

#include "util.h"

using util::Random;

size_t compute_1(size_t num) {
    num >>= 2u;
    num <<= 3u;
    num ^= 34234235ul;
    return num;
}

size_t compute_2(size_t num) {
    num ^= num % 24234235ul;
    return num;
}

size_t compute_3(size_t num) {
    num &= num ^ 24234235ul;
    return num;
}

template<size_t fields>
class ComputeFunctor {
private:
    size_t data[fields];
public:
    explicit ComputeFunctor(Random<std::mt19937_64> &rng) {
        rng.fillRand<size_t>(0, std::numeric_limits<size_t>::max(), std::begin(data), std::end(data));
    }

    size_t operator()(size_t num) const {
        for (auto d : data) {
            num ^= d;
        }

        return num;
    }
};

template<size_t fields>
class ComputeFunctor2 {
private:
    size_t data[fields];
    uint16_t data2[fields];
public:
    explicit ComputeFunctor2(Random<std::mt19937_64> &rng) {
        rng.fillRand<size_t>(0, std::numeric_limits<size_t>::max(), std::begin(data), std::end(data));
        rng.fillRand<uint16_t>(0, std::numeric_limits<uint16_t>::max(), std::begin(data2), std::end(data2));
    }

    size_t operator()(size_t num) const {
        for (unsigned i = 0; i != fields; ++i) {
            num ^= data[i];
            num ^= data2[i];
        }
        return num;
    }
};

class CallbackGenerator {
    Random<std::mt19937_64> random;
public:
    explicit CallbackGenerator(size_t seed) : random{seed} {}

    void setSeed(uint32_t seed) {
        random.setSeed(seed);
    }

    template<typename T>
    void addCallback(T &&push_back) noexcept {
        switch (random.getRand(0, 12)) {
            case 0: {
                auto constexpr max_ = std::numeric_limits<uint32_t>::max();
                auto a = random.getRand<uint32_t>(0, max_);
                auto b = random.getRand<uint32_t>(0, max_);
                auto c = random.getRand<uint32_t>(0, max_);
                push_back([=](size_t num) {
                    return (num ^ a) & (b ^ c);
                });
            }
                break;
            case 1: {
                auto constexpr max_ = std::numeric_limits<uint32_t>::max();
                auto a = random.getRand<uint32_t>(0, max_);
                auto b = random.getRand<uint32_t>(0, max_);
                auto c = random.getRand<size_t>(0, max_);
                auto d = random.getRand<size_t>(0, max_);
                auto e = random.getRand<size_t>(0, max_);
                auto f = random.getRand<size_t>(0, max_);
                auto g = random.getRand<size_t>(0, max_);
                push_back([=](size_t num) {
                    return (num ^ a) & (b ^ c) >> (d % 5) ^ e << (f % 3) ^ g;
                });
            }
                break;
            case 2:
                push_back(compute_1);
                break;
            case 3:
                push_back(compute_2);
                break;
            case 4:
                push_back(compute_3);
                break;
            case 5: {
                push_back(ComputeFunctor<10>{random});
            }
                break;
            case 6: {
                push_back(ComputeFunctor2<10>{random});
            }
                break;
            case 7: {
                push_back(ComputeFunctor<7>{random});
            }
                break;
            case 8: {
                push_back(ComputeFunctor2<5>{random});
            }
                break;
            case 9: {
                push_back(ComputeFunctor<2>{random});
            }
                break;
            case 10: {
                push_back(ComputeFunctor<3>{random});
            }
                break;

            case 11: {
                push_back([a = random.getRand<uint16_t>(0, std::numeric_limits<uint16_t>::max())](size_t num) {
                    return num ^ a;
                });
            }
                break;
            case 12: {
                push_back([a = random.getRand<uint16_t>(0, 255)](size_t num) {
                    return num ^ a;
                });
            }
                break;
                /*case 13:
                    rawComputeQueue.push_back(compute_1);
                    vectorComputeQueue.emplace_back(compute_1);
                    vectorStdComputeQueue.emplace_back(compute_1);
                    break;*/
        }
    }
};

#endif //FUNCTIONQUEUE_COMPUTECALLBACKGENERATOR_H
