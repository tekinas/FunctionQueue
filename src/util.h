#ifndef CPP_TEST_UTIL_H
#define CPP_TEST_UTIL_H

#include "uniform_distribution.h"

#include <iostream>
#include <mutex>
#include <random>
#include <memory_resource>
#include <condition_variable>

namespace util {

    inline std::mutex _cout_mutex;

    template<typename...T>
    void print(T &&...t) {
        std::scoped_lock _cout_lock{_cout_mutex};
        (std::cout << ... << std::forward<T>(t));
    }

    template<typename...T>
    void println(T &&...t) {
        print(std::forward<T>(t)..., '\n');
    }

    template<typename... Types>
    void print_error(Types &&... args) {
        std::cerr << "ERROR :";
        (std::cerr << ... << std::forward<Types>(args)) << std::endl;
        std::exit(EXIT_FAILURE);
    }

    template<size_t count, typename T, std::enable_if_t<std::is_invocable_v<T>, int> = 0>
    void repeat(T &&t) {
        for (size_t i = 0; i != count; ++i) {
            std::forward<T>(t)();
        }
    }

    template<size_t count, typename T, std::enable_if_t<std::is_invocable_v<T, size_t>, int> = 0>
    void repeat(T &&t) {
        for (size_t i = 0; i != count; ++i) {
            std::forward<T>(t)(i);
        }
    }

    template<typename T, std::enable_if_t<std::is_invocable_v<T>, int> = 0>
    void repeat(size_t count, T &&t) {
        for (size_t i = 0; i != count; ++i) {
            std::forward<T>(t)();
        }
    }

    template<typename T, std::enable_if_t<std::is_invocable_v<T, size_t>, int> = 0>
    void repeat(size_t count, T &&t) {
        for (size_t i = 0; i != count; ++i) {
            std::forward<T>(t)(i);
        }
    }


    template<typename RNG_Type = std::mt19937>
    class Random {
    private:
        RNG_Type rng;

    public:
        Random() : rng{std::random_device{}()} {}

        template<typename T>
        Random(T t):rng{t} {
            static_assert(std::is_integral_v<T>);
        }

        template<typename T>
        void setSeed(T t) {
            static_assert(std::is_integral_v<T>);
            rng.seed(t);
        }

        template<typename T = float>
        T getRand() {
            return util::uniform_real_distribution<T>{T{0.0}, T{1.0}}(rng);
        }

        template<typename T>
        T getRand(T lower, T upper) {
            if constexpr (std::is_integral_v<T>) {
                return util::uniform_int_distribution<T>{lower, upper}(rng);
            } else if constexpr (std::is_floating_point_v<T>) {
                return util::uniform_real_distribution<T>{lower, upper}(rng);
            } else {
                static_assert(
                        std::is_integral<T>::value | std::is_floating_point<T>::value,
                        "T is neither integral result_type nor floating point result_type.");
            }
        }

        template<typename T>
        void fillRand(T lower, T upper, std::vector<T> &dest, uint64_t num) {
            if constexpr (std::is_integral<T>::value) {
                util::uniform_int_distribution<T> dist{lower, upper};
                while (num--) {
                    dest.emplace_back(dist(rng));
                }
            } else if constexpr (std::is_floating_point<T>::value) {
                util::uniform_real_distribution<T> dist{lower, upper};
                while (num--) {
                    dest.emplace_back(dist(rng));
                }
            } else {
                static_assert(
                        std::is_integral<T>::value | std::is_floating_point<T>::value,
                        "T is neither integral result_type nor floating point result_type.");
            }
        }

        template<typename T, typename ItStart, typename ItEnd>
        void fillRand(T lower, T upper, ItStart start, ItEnd end) {
            if constexpr (std::is_integral_v<T>) {
                util::uniform_int_distribution<T> dist{lower, upper};
                while (start != end) {
                    *start = dist(rng);
                    ++start;
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                util::uniform_real_distribution<T> dist{lower, upper};
                while (start != end) {
                    *start = dist(rng);
                    ++start;
                }
            } else {
                static_assert(
                        std::is_integral<T>::value | std::is_floating_point<T>::value,
                        "T is neither integral result_type nor floating point result_type.");
            }
        }
    };

    template<typename T>
    class pmr_deleter {
    private:
        std::pmr::memory_resource *memoryResource;
    public:
        pmr_deleter() noexcept: memoryResource{nullptr} {}

        /*explicit*/ pmr_deleter(std::pmr::memory_resource *memoryResource) noexcept: memoryResource{memoryResource} {}

        void operator()(T *const ptr) const noexcept {
            ptr->~T();
            memoryResource->deallocate(ptr, sizeof(T), alignof(T));
        }
    };

    template<typename T>
    class pmr_deleter<T[]> {
    private:
        std::pmr::memory_resource *memoryResource;
        size_t size;
    public:
        pmr_deleter() noexcept: memoryResource{nullptr}, size{0} {}

        pmr_deleter(std::pmr::memory_resource *memoryResource, size_t size) noexcept: memoryResource{
                memoryResource}, size{size} {}

        [[nodiscard]] size_t getSize() const noexcept { return size; }

        [[nodiscard]] std::pmr::memory_resource *getResource() const noexcept { return memoryResource; }

        void operator()(T *const ptr) const noexcept {
            std::destroy_n(ptr, size);

            memoryResource->deallocate(ptr, size * sizeof(T), alignof(T));
        }
    };

    struct ConditionFlag {
    public:
        explicit ConditionFlag(bool condition) noexcept: condition(condition) {}

        void wait(bool con) noexcept {
            std::unique_lock lock{mutex};
            cv.wait(lock, [&, con] {
                return condition == con;
            });
        }

        template<typename Duration>
        void wait_for(bool con, Duration &&duration) noexcept {
            std::unique_lock lock{mutex};
            cv.wait_for(lock, std::forward<Duration>(duration), [&, con] {
                return condition == con;
            });
        }

        template<typename Timepoint>
        void wait_until(bool con, Timepoint &&timepoint) noexcept {
            std::unique_lock lock{mutex};
            cv.wait_until(lock, std::forward<Timepoint>(timepoint), [&, con] {
                return condition == con;
            });
        }

        void set() noexcept {
            {
                std::lock_guard lock{mutex};
                condition = true;
            }
            cv.notify_one();
        }

        void set(bool con) noexcept {
            {
                std::lock_guard lock{mutex};
                condition = con;
            }
            cv.notify_one();
        }

        void unset() noexcept {
            {
                std::lock_guard lock{mutex};
                condition = false;
            }
            cv.notify_one();
        }

    private:
        std::condition_variable cv;
        std::mutex mutex;
        bool condition;
    };

    template<typename IntType>
    struct ReferenceCount {
    public:
        explicit ReferenceCount(IntType count) noexcept: count{count} {}

        void wait(IntType _count) noexcept {
            std::unique_lock lock{mutex};
            cv.wait(lock, [&, _count] {
                return count == _count;
            });
        }

        template<typename Duration>
        void wait_for(IntType _count, Duration &&duration) noexcept {
            std::unique_lock lock{mutex};
            cv.wait_for(lock, std::forward<Duration>(duration), [&, _count] {
                return count == _count;
            });
        }

        template<typename Timepoint>
        void wait_until(IntType _count, Timepoint &&timepoint) noexcept {
            std::unique_lock lock{mutex};
            cv.wait_until(lock, std::forward<Timepoint>(timepoint), [&, _count] {
                return count == _count;
            });
        }

        void incr() noexcept {
            {
                std::lock_guard lock{mutex};
                ++count;
            }
            cv.notify_all();
        }

        void decr() noexcept {
            {
                std::lock_guard lock{mutex};
                --count;
            }
            cv.notify_all();
        }

    private:
        std::condition_variable cv;
        std::mutex mutex;

        static_assert(std::is_integral_v<IntType>);
        IntType count;
    };

}  // namespace util

namespace std::pmr {
    template<typename T>
    using unique_ptr = std::unique_ptr<T, util::pmr_deleter<T>>;
}

namespace util {
    template<typename T>
    struct UniquePointerType {
        using single_object = std::pmr::unique_ptr<T>;
    };

    template<typename T>
    struct UniquePointerType<T[]> {
        using array = std::pmr::unique_ptr<T[]>;
    };

    template<typename T, size_t Bound>
    struct UniquePointerType<T[Bound]> {
        struct invalid_type {
        };
    };

    template<typename T, typename... ArgTypes>
    inline typename UniquePointerType<T>::single_object
    make_unique(std::pmr::memory_resource *memoryResource, ArgTypes &&... args) noexcept {
        std::pmr::polymorphic_allocator<T> alloc{memoryResource};
        auto ptr = alloc.allocate(1);
        alloc.construct(ptr, std::forward<ArgTypes>(args)...);

        return std::pmr::unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource}};
    }

    template<typename T>
    inline typename UniquePointerType<T>::array
    make_unique(std::pmr::memory_resource *memoryResource, size_t const size) noexcept {
        using type = std::remove_extent_t<T>;

        std::pmr::polymorphic_allocator<type> alloc{memoryResource};
        auto const ptr = alloc.allocate(size);

        for (size_t i = 0; i != size; ++i) {
            alloc.construct(ptr + i);
        }

        return std::pmr::unique_ptr<T>{ptr, pmr_deleter<T>{memoryResource, size}};
    }

    template<typename T>
    inline typename UniquePointerType<T>::invalid_type
    make_unique(std::pmr::memory_resource *memoryResource, size_t size) = delete;
}

namespace util {
    class Timer {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;
        using print_duartion = std::chrono::duration<double>;
    private:
        std::string name;
        time_point start;
    public:
        explicit Timer(std::string_view str) noexcept: name{str} {
            start = clock::now();
        }

        ~Timer() noexcept {
            println(name, " :", std::chrono::duration_cast<print_duartion>(clock::now() - start).count(), " seconds");
        }
    };
}

#endif  // CPP_TEST_UTIL_H
