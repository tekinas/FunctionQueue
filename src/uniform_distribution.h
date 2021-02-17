// Class template uniform_int_distribution -*- C++ -*-

// Copyright (C) 2009-2021 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License and
// a copy of the GCC Runtime Library Exception along with this program;
// see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
// <http://www.gnu.org/licenses/>.

/**
 * @file bits/uniform_int_dist.h
 *  This is an internal header file, included by other library headers.
 *  Do not attempt to use it directly. @headername{random}
 */


#include <type_traits>
#include <cassert>
#include <random>

namespace util {
    namespace detail {
        // Determine whether number is a power of two.
        // This is true for zero, which is OK because we want powerOf2(n+1)
        // to be true if n==numeric_limits<_Tp>::max() and so n+1 wraps around.
        template<typename Tp>
        constexpr bool
        powerOf2(Tp x) {
            return ((x - 1) & x) == 0;
        }
    }

    /**
     * @brief Uniform discrete distribution for random numbers.
     * A discrete random distribution on the range @f$[min, max]@f$ with equal
     * probability throughout the range.
     */
    template<typename IntType = int>
    class uniform_int_distribution {
        static_assert(std::is_integral<IntType>::value,
                      "template argument must be an integral type");

    public:
        /** The type of the range of the distribution. */
        typedef IntType result_type;

        /** Parameter type. */
        struct param_type {
            typedef uniform_int_distribution<IntType> distribution_type;

            param_type() : param_type(0) {}

            explicit
            param_type(IntType a,
                       IntType b = std::numeric_limits<IntType>::max())
                    : M_a(a), M_b(b) {
                assert(M_a <= M_b);
            }

            result_type
            a() const { return M_a; }

            result_type
            b() const { return M_b; }

            friend bool
            operator==(const param_type &p1, const param_type &p2) {
                return p1.M_a == p2.M_a && p1.M_b == p2.M_b;
            }

            friend bool
            operator!=(const param_type &p1, const param_type &p2) { return !(p1 == p2); }

        private:
            IntType M_a;
            IntType M_b;
        };

    public:
        /**
         * @brief Constructs a uniform distribution object.
         */
        uniform_int_distribution() : uniform_int_distribution(0) {}

        /**
         * @brief Constructs a uniform distribution object.
         */
        explicit
        uniform_int_distribution(IntType a,
                                 IntType b
                                 = std::numeric_limits<IntType>::max())
                : M_param(a, b) {}

        explicit
        uniform_int_distribution(const param_type &p)
                : M_param(p) {}

        /**
         * @brief Resets the distribution state.
         *
         * Does nothing for the uniform integer distribution.
         */
        void
        reset() {}

        result_type
        a() const { return M_param.a(); }

        result_type
        b() const { return M_param.b(); }

        /**
         * @brief Returns the parameter set of the distribution.
         */
        param_type
        param() const { return M_param; }

        /**
         * @brief Sets the parameter set of the distribution.
         * @param param The new parameter set of the distribution.
         */
        void
        param(const param_type &param) { M_param = param; }

        /**
         * @brief Returns the inclusive lower bound of the distribution range.
         */
        result_type
        min() const { return this->a(); }

        /**
         * @brief Returns the inclusive upper bound of the distribution range.
         */
        result_type
        max() const { return this->b(); }

        /**
         * @brief Generating functions.
         */
        template<typename UniformRandomBitGenerator>
        result_type
        operator()(UniformRandomBitGenerator &urng) { return this->operator()(urng, M_param); }

        template<typename UniformRandomBitGenerator>
        result_type
        operator()(UniformRandomBitGenerator &urng,
                   const param_type &p);

        template<typename ForwardIterator,
                typename UniformRandomBitGenerator>
        void
        generate(ForwardIterator f, ForwardIterator t,
                 UniformRandomBitGenerator &urng) { this->generate(f, t, urng, M_param); }

        template<typename ForwardIterator,
                typename UniformRandomBitGenerator>
        void
        generate(ForwardIterator f, ForwardIterator t,
                 UniformRandomBitGenerator &urng,
                 const param_type &p) { this->generate_impl(f, t, urng, p); }

        template<typename UniformRandomBitGenerator>
        void
        generate(result_type *f, result_type *t,
                 UniformRandomBitGenerator &urng,
                 const param_type &p) { this->generate_impl(f, t, urng, p); }

        /**
         * @brief Return true if two uniform integer distributions have
         *        the same parameters.
         */
        friend bool
        operator==(const uniform_int_distribution &d1,
                   const uniform_int_distribution &d2) { return d1.M_param == d2.M_param; }

    private:
        template<typename ForwardIterator,
                typename UniformRandomBitGenerator>
        void
        generate_impl(ForwardIterator f, ForwardIterator t,
                      UniformRandomBitGenerator &urng,
                      const param_type &p);

        param_type M_param;

        // Lemire's nearly divisionless algorithm.
        // Returns an unbiased random number from g downscaled to [0,range)
        // using an unsigned type _Wp twice as wide as unsigned type _Up.
        template<typename Wp, typename Urbg, typename Up>
        static Up
        S_nd(Urbg &g, Up range) {
            static_assert(!std::is_signed_v<Up>, "U must be unsigned");
            static_assert(!std::is_signed_v<Wp>, "W must be unsigned");
            static_assert(std::numeric_limits<Wp>::digits == (2 * std::numeric_limits<Up>::digits),
                          "W must be twice as wide as U");

            // reference: Fast Random Integer Generation in an Interval
            // ACM Transactions on Modeling and Computer Simulation 29 (1), 2019
            // https://arxiv.org/abs/1805.10941
            Wp product = Wp(g()) * Wp(range);
            Up low = Up(product);
            if (low < range) {
                Up threshold = -range % range;
                while (low < threshold) {
                    product = Wp(g()) * Wp(range);
                    low = Up(product);
                }
            }
            return product >> std::numeric_limits<Up>::digits;
        }
    };

    template<typename IntType>
    template<typename UniformRandomBitGenerator>
    typename uniform_int_distribution<IntType>::result_type
    uniform_int_distribution<IntType>::
    operator()(UniformRandomBitGenerator &urng,
               const param_type &param) {
        typedef typename UniformRandomBitGenerator::result_type Gresult_type;
        typedef typename std::make_unsigned<result_type>::type utype;
        typedef typename std::common_type<Gresult_type, utype>::type uctype;

        constexpr uctype urngmin = UniformRandomBitGenerator::min();
        constexpr uctype urngmax = UniformRandomBitGenerator::max();
        static_assert(urngmin < urngmax,
                      "Uniform random bit generator must define min() < max()");
        constexpr uctype urngrange = urngmax - urngmin;

        const uctype urange
                = uctype(param.b()) - uctype(param.a());

        uctype ret;
        if (urngrange > urange) {
            // downscaling

            const uctype uerange = urange + 1; // urange can be zero

#if defined UINT64_TYPE && defined UINT32_TYPE
#if SIZEOF_INT128
            if _GLIBCXX17_CONSTEXPR (urngrange == UINT64_MAX) {
                // urng produces values that use exactly 64-bits,
                // so use 128-bit integers to downscale to desired range.
                UINT64_TYPE u64erange = uerange;
                ret = _S_nd<unsigned int128>(urng, u64erange);
            } else
#endif
            if _GLIBCXX17_CONSTEXPR (urngrange == UINT32_MAX) {
                // urng produces values that use exactly 32-bits,
                // so use 64-bit integers to downscale to desired range.
                UINT32_TYPE u32erange = uerange;
                ret = _S_nd<UINT64_TYPE>(urng, u32erange);
            } else
#endif
            {
                // fallback case (2 divisions)
                const uctype scaling = urngrange / uerange;
                const uctype past = uerange * scaling;
                do
                    ret = uctype(urng()) - urngmin;
                while (ret >= past);
                ret /= scaling;
            }
        } else if (urngrange < urange) {
            // upscaling
            /*
              Note that every value in [0, urange]
              can be written uniquely as
    
              (urngrange + 1) * high + low
    
              where
    
              high in [0, urange / (urngrange + 1)]
    
              and
    
              low in [0, urngrange].
            */
            uctype tmp; // wraparound control
            do {
                const uctype uerngrange = urngrange + 1;
                tmp = (uerngrange * operator()
                        (urng, param_type(0, urange / uerngrange)));
                ret = tmp + (uctype(urng()) - urngmin);
            } while (ret > urange || ret < tmp);
        } else
            ret = uctype(urng()) - urngmin;

        return ret + param.a();
    }


    template<typename IntType>
    template<typename ForwardIterator,
            typename UniformRandomBitGenerator>
    void
    uniform_int_distribution<IntType>::
    generate_impl(ForwardIterator f, ForwardIterator t,
                  UniformRandomBitGenerator &urng,
                  const param_type &param) {
        typedef typename UniformRandomBitGenerator::result_type Gresult_type;
        typedef typename std::make_unsigned<result_type>::type utype;
        typedef typename std::common_type<Gresult_type, utype>::type uctype;

        static_assert(urng.min() < urng.max(),
                      "Uniform random bit generator must define min() < max()");

        constexpr uctype urngmin = urng.min();
        constexpr uctype urngmax = urng.max();
        constexpr uctype urngrange = urngmax - urngmin;
        const uctype urange
                = uctype(param.b()) - uctype(param.a());

        uctype ret;

        if (urngrange > urange) {
            if (detail::powerOf2(urngrange + 1)
                && detail::powerOf2(urange + 1)) {
                while (f != t) {
                    ret = uctype(urng()) - urngmin;
                    *f++ = (ret & urange) + param.a();
                }
            } else {
                // downscaling
                const uctype uerange = urange + 1; // urange can be zero
                const uctype scaling = urngrange / uerange;
                const uctype past = uerange * scaling;
                while (f != t) {
                    do
                        ret = uctype(urng()) - urngmin;
                    while (ret >= past);
                    *f++ = ret / scaling + param.a();
                }
            }
        } else if (urngrange < urange) {
            // upscaling
            /*
              Note that every value in [0, urange]
              can be written uniquely as
    
              (urngrange + 1) * high + low
    
              where
    
              high in [0, urange / (urngrange + 1)]
    
              and
    
              low in [0, urngrange].
            */
            uctype tmp; // wraparound control
            while (f != t) {
                do {
                    constexpr uctype uerngrange = urngrange + 1;
                    tmp = (uerngrange * operator()
                            (urng, param_type(0, urange / uerngrange)));
                    ret = tmp + (uctype(urng()) - urngmin);
                } while (ret > urange || ret < tmp);
                *f++ = ret;
            }
        } else
            while (f != t)
                *f++ = uctype(urng()) - urngmin + param.a();
    }
}


namespace util {
    template<typename realType = double>
    class uniform_real_distribution {
        static_assert(std::is_floating_point<realType>::value,
                      "result_type must be a floating point type");

    public:
        /** The type of the range of the distribution. */
        typedef realType result_type;

        /** Parameter type. */
        struct param_type {
            typedef uniform_real_distribution<realType> distribution_type;

            param_type() : param_type(0) {}

            explicit
            param_type(realType a, realType b = realType(1))
                    : mA(a), mB(b) {
                glibcxx_assert(mA <= mB);
            }

            result_type
            a() const { return mA; }

            result_type
            b() const { return mB; }

            friend bool
            operator==(const param_type &p1, const param_type &p2) {
                return p1.mA == p2.mA && p1.mB == p2.mB;
            }

            friend bool
            operator!=(const param_type &p1, const param_type &p2) { return !(p1 == p2); }

        private:
            realType mA;
            realType mB;
        };

    public:
        /**
         * @brief Constructs a uniform_real_distribution object.
         *
         * The lower bound is set to 0.0 and the upper bound to 1.0
         */
        uniform_real_distribution() : uniform_real_distribution(0.0) {}

        /**
         * @brief Constructs a uniform_real_distribution object.
         *
         * @param a [IN]  The lower bound of the distribution.
         * @param b [IN]  The upper bound of the distribution.
         */
        explicit
        uniform_real_distribution(realType a, realType b = realType(1))
                : mParam(a, b) {}

        explicit
        uniform_real_distribution(const param_type &p)
                : mParam(p) {}

        /**
         * @brief Resets the distribution state.
         *
         * Does nothing for the uniform real distribution.
         */
        void
        reset() {}

        result_type
        a() const { return mParam.a(); }

        result_type
        b() const { return mParam.b(); }

        /**
         * @brief Returns the parameter set of the distribution.
         */
        param_type
        param() const { return mParam; }

        /**
         * @brief Sets the parameter set of the distribution.
         * @param param The new parameter set of the distribution.
         */
        void
        param(const param_type &param) { mParam = param; }

        /**
         * @brief Returns the inclusive lower bound of the distribution range.
         */
        result_type
        min() const { return this->a(); }

        /**
         * @brief Returns the inclusive upper bound of the distribution range.
         */
        result_type
        max() const { return this->b(); }

        /**
         * @brief Generating functions.
         */
        template<typename UniformRandomNumberGenerator>
        result_type
        operator()(UniformRandomNumberGenerator &urng) { return this->operator()(urng, mParam); }

        template<typename UniformRandomNumberGenerator>
        result_type
        operator()(UniformRandomNumberGenerator &urng, const param_type &p) {
            /*detail::_Adaptor<_UniformRandomNumberGenerator, result_type>
                    aurng(urng);*/
            return (/*aurng() **/ (p.b() - p.a())) + p.a();
        }

        template<typename ForwardIterator,
                typename UniformRandomNumberGenerator>
        void
        generate(ForwardIterator _f, ForwardIterator _t,
                 UniformRandomNumberGenerator &urng) { this->generate(_f, _t, urng, mParam); }

        template<typename _ForwardIterator,
                typename _UniformRandomNumberGenerator>
        void
        generate(_ForwardIterator f, _ForwardIterator t,
                 _UniformRandomNumberGenerator &urng,
                 const param_type &p) { this->generateImpl(f, t, urng, p); }

        template<typename _UniformRandomNumberGenerator>
        void
        generate(result_type *f, result_type *t,
                 _UniformRandomNumberGenerator &urng,
                 const param_type &p) { this->generateImpl(f, t, urng, p); }

        /**
         * @brief Return true if two uniform real distributions have
         *        the same parameters.
         */
        friend bool
        operator==(const uniform_real_distribution &d1,
                   const uniform_real_distribution &d2) { return d1.mParam == d2.mParam; }

    private:
        template<typename ForwardIterator,
                typename UniformRandomNumberGenerator>
        void
        generateImpl(ForwardIterator f, ForwardIterator t,
                     UniformRandomNumberGenerator &urng,
                     const param_type &p);

        param_type mParam;
    };

    /**
     * @brief Return true if two uniform real distributions have
     *        different parameters.
     */
    template<typename IntType>
    inline bool
    operator!=(const uniform_real_distribution<IntType> &d1,
               const uniform_real_distribution<IntType> &d2) { return !(d1 == d2); }
}
