/*******************************************************************
 * Copyright (C) 2022 Intel Corporation
 * Copyright (C) 2021 Serge Sans Paille
 * SPDX-License-Identifier: BSD-3-Clause
 * Authors: Raghuveer Devulapalli <raghuveer.devulapalli@intel.com>
 *          Serge Sans Paille <serge.guelton@telecom-bretagne.eu>
 * ****************************************************************/

#ifndef AVX512_QSORT_COMMON
#define AVX512_QSORT_COMMON

/*
 * Quicksort using AVX-512. The ideas and code are based on these two research
 * papers [1] and [2]. On a high level, the idea is to vectorize quicksort
 * partitioning using AVX-512 compressstore instructions. If the array size is
 * < 128, then use Bitonic sorting network implemented on 512-bit registers.
 * The precise network definitions depend on the dtype and are defined in
 * separate files: avx512-16bit-qsort.hpp, avx512-32bit-qsort.hpp and
 * avx512-64bit-qsort.hpp. Article [4] is a good resource for bitonic sorting
 * network. The core implementations of the vectorized qsort functions
 * avx512_qsort<T>(T*, int64_t) are modified versions of avx2 quicksort
 * presented in the paper [2] and source code associated with that paper [3].
 *
 * [1] Fast and Robust Vectorized In-Place Sorting of Primitive Types
 *     https://drops.dagstuhl.de/opus/volltexte/2021/13775/
 *
 * [2] A Novel Hybrid Quicksort Algorithm Vectorized using AVX-512 on Intel
 * Skylake https://arxiv.org/pdf/1704.08579.pdf
 *
 * [3] https://github.com/simd-sorting/fast-and-robust: SPDX-License-Identifier: MIT
 *
 * [4] http://mitp-content-server.mit.edu:18180/books/content/sectbyfn?collid=books_pres_0&fn=Chapter%2027.pdf&id=8030
 *
 */

#include "avx512-zmm-classes.h"

template <typename T>
void avx512_qsort(T *arr, int64_t arrsize);
void avx512_qsort_fp16(uint16_t *arr, int64_t arrsize);

template <typename T>
void avx512_qselect(T *arr, int64_t k, int64_t arrsize);
void avx512_qselect_fp16(uint16_t *arr, int64_t k, int64_t arrsize);

template <typename T>
inline void avx512_partial_qsort(T *arr, int64_t k, int64_t arrsize)
{
    avx512_qselect<T>(arr, k - 1, arrsize);
    avx512_qsort<T>(arr, k - 1);
}
inline void avx512_partial_qsort_fp16(uint16_t *arr, int64_t k, int64_t arrsize)
{
    avx512_qselect_fp16(arr, k - 1, arrsize);
    avx512_qsort_fp16(arr, k - 1);
}

template <typename vtype, typename T = typename vtype::type_t>
bool comparison_func(const T &a, const T &b)
{
    return a < b;
}

/*
 * COEX == Compare and Exchange two registers by swapping min and max values
 */
template <typename vtype, typename mm_t>
static void COEX(mm_t &a, mm_t &b)
{
    mm_t temp = a;
    a = vtype::min(a, b);
    b = vtype::max(temp, b);
}

template <typename vtype,
          typename zmm_t = typename vtype::zmm_t,
          typename opmask_t = typename vtype::opmask_t>
static inline zmm_t cmp_merge(zmm_t in1, zmm_t in2, opmask_t mask)
{
    zmm_t min = vtype::min(in2, in1);
    zmm_t max = vtype::max(in2, in1);
    return vtype::mask_mov(min, mask, max); // 0 -> min, 1 -> max
}
/*
 * Parition one ZMM register based on the pivot and returns the
 * number of elements that are greater than or equal to the pivot.
 */
template <typename vtype, typename type_t, typename zmm_t>
static inline int32_t partition_vec(type_t *arr,
                                    int64_t left,
                                    int64_t right,
                                    const zmm_t curr_vec,
                                    const zmm_t pivot_vec,
                                    zmm_t *smallest_vec,
                                    zmm_t *biggest_vec)
{
    /* which elements are larger than or equal to the pivot */
    typename vtype::opmask_t ge_mask = vtype::ge(curr_vec, pivot_vec);
    int32_t amount_ge_pivot = _mm_popcnt_u32((int32_t)ge_mask);
    vtype::mask_compressstoreu(
            arr + left, vtype::knot_opmask(ge_mask), curr_vec);
    vtype::mask_compressstoreu(
            arr + right - amount_ge_pivot, ge_mask, curr_vec);
    *smallest_vec = vtype::min(curr_vec, *smallest_vec);
    *biggest_vec = vtype::max(curr_vec, *biggest_vec);
    return amount_ge_pivot;
}
/*
 * Parition an array based on the pivot and returns the index of the
 * first element that is greater than or equal to the pivot.
 */
template <typename vtype, typename type_t>
static inline int64_t partition_avx512(type_t *arr,
                                       int64_t left,
                                       int64_t right,
                                       type_t pivot,
                                       type_t *smallest,
                                       type_t *biggest)
{
    /* make array length divisible by vtype::numlanes , shortening the array */
    for (int32_t i = (right - left) % vtype::numlanes; i > 0; --i) {
        *smallest = std::min(*smallest, arr[left], comparison_func<vtype>);
        *biggest = std::max(*biggest, arr[left], comparison_func<vtype>);
        if (!comparison_func<vtype>(arr[left], pivot)) {
            std::swap(arr[left], arr[--right]);
        }
        else {
            ++left;
        }
    }

    if (left == right)
        return left; /* less than vtype::numlanes elements in the array */

    using zmm_t = typename vtype::zmm_t;
    zmm_t pivot_vec = vtype::set1(pivot);
    zmm_t min_vec = vtype::set1(*smallest);
    zmm_t max_vec = vtype::set1(*biggest);

    if (right - left == vtype::numlanes) {
        zmm_t vec = vtype::loadu(arr + left);
        int32_t amount_ge_pivot = partition_vec<vtype>(arr,
                                                       left,
                                                       left + vtype::numlanes,
                                                       vec,
                                                       pivot_vec,
                                                       &min_vec,
                                                       &max_vec);
        *smallest = vtype::reducemin(min_vec);
        *biggest = vtype::reducemax(max_vec);
        return left + (vtype::numlanes - amount_ge_pivot);
    }

    // first and last vtype::numlanes values are partitioned at the end
    zmm_t vec_left = vtype::loadu(arr + left);
    zmm_t vec_right = vtype::loadu(arr + (right - vtype::numlanes));
    // store points of the vectors
    int64_t r_store = right - vtype::numlanes;
    int64_t l_store = left;
    // indices for loading the elements
    left += vtype::numlanes;
    right -= vtype::numlanes;
    while (right - left != 0) {
        zmm_t curr_vec;
        /*
         * if fewer elements are stored on the right side of the array,
         * then next elements are loaded from the right side,
         * otherwise from the left side
         */
        if ((r_store + vtype::numlanes) - right < left - l_store) {
            right -= vtype::numlanes;
            curr_vec = vtype::loadu(arr + right);
        }
        else {
            curr_vec = vtype::loadu(arr + left);
            left += vtype::numlanes;
        }
        // partition the current vector and save it on both sides of the array
        int32_t amount_ge_pivot
                = partition_vec<vtype>(arr,
                                       l_store,
                                       r_store + vtype::numlanes,
                                       curr_vec,
                                       pivot_vec,
                                       &min_vec,
                                       &max_vec);
        ;
        r_store -= amount_ge_pivot;
        l_store += (vtype::numlanes - amount_ge_pivot);
    }

    /* partition and save vec_left and vec_right */
    int32_t amount_ge_pivot = partition_vec<vtype>(arr,
                                                   l_store,
                                                   r_store + vtype::numlanes,
                                                   vec_left,
                                                   pivot_vec,
                                                   &min_vec,
                                                   &max_vec);
    l_store += (vtype::numlanes - amount_ge_pivot);
    amount_ge_pivot = partition_vec<vtype>(arr,
                                           l_store,
                                           l_store + vtype::numlanes,
                                           vec_right,
                                           pivot_vec,
                                           &min_vec,
                                           &max_vec);
    l_store += (vtype::numlanes - amount_ge_pivot);
    *smallest = vtype::reducemin(min_vec);
    *biggest = vtype::reducemax(max_vec);
    return l_store;
}

template <typename vtype,
          int num_unroll,
          typename type_t = typename vtype::type_t>
static inline int64_t partition_avx512_unrolled(type_t *arr,
                                                int64_t left,
                                                int64_t right,
                                                type_t pivot,
                                                type_t *smallest,
                                                type_t *biggest)
{
    if (right - left <= 2 * num_unroll * vtype::numlanes) {
        return partition_avx512<vtype>(
                arr, left, right, pivot, smallest, biggest);
    }
    /* make array length divisible by 8*vtype::numlanes , shortening the array */
    for (int32_t i = ((right - left) % (num_unroll * vtype::numlanes)); i > 0;
         --i) {
        *smallest = std::min(*smallest, arr[left], comparison_func<vtype>);
        *biggest = std::max(*biggest, arr[left], comparison_func<vtype>);
        if (!comparison_func<vtype>(arr[left], pivot)) {
            std::swap(arr[left], arr[--right]);
        }
        else {
            ++left;
        }
    }

    if (left == right)
        return left; /* less than vtype::numlanes elements in the array */

    using zmm_t = typename vtype::zmm_t;
    zmm_t pivot_vec = vtype::set1(pivot);
    zmm_t min_vec = vtype::set1(*smallest);
    zmm_t max_vec = vtype::set1(*biggest);

    // We will now have atleast 16 registers worth of data to process:
    // left and right vtype::numlanes values are partitioned at the end
    zmm_t vec_left[num_unroll], vec_right[num_unroll];
#pragma GCC unroll 8
    for (int ii = 0; ii < num_unroll; ++ii) {
        vec_left[ii] = vtype::loadu(arr + left + vtype::numlanes * ii);
        vec_right[ii] = vtype::loadu(
                arr + (right - vtype::numlanes * (num_unroll - ii)));
    }
    // store points of the vectors
    int64_t r_store = right - vtype::numlanes;
    int64_t l_store = left;
    // indices for loading the elements
    left += num_unroll * vtype::numlanes;
    right -= num_unroll * vtype::numlanes;
    while (right - left != 0) {
        zmm_t curr_vec[num_unroll];
        /*
         * if fewer elements are stored on the right side of the array,
         * then next elements are loaded from the right side,
         * otherwise from the left side
         */
        if ((r_store + vtype::numlanes) - right < left - l_store) {
            right -= num_unroll * vtype::numlanes;
#pragma GCC unroll 8
            for (int ii = 0; ii < num_unroll; ++ii) {
                curr_vec[ii] = vtype::loadu(arr + right + ii * vtype::numlanes);
            }
        }
        else {
#pragma GCC unroll 8
            for (int ii = 0; ii < num_unroll; ++ii) {
                curr_vec[ii] = vtype::loadu(arr + left + ii * vtype::numlanes);
            }
            left += num_unroll * vtype::numlanes;
        }
// partition the current vector and save it on both sides of the array
#pragma GCC unroll 8
        for (int ii = 0; ii < num_unroll; ++ii) {
            int32_t amount_ge_pivot
                    = partition_vec<vtype>(arr,
                                           l_store,
                                           r_store + vtype::numlanes,
                                           curr_vec[ii],
                                           pivot_vec,
                                           &min_vec,
                                           &max_vec);
            l_store += (vtype::numlanes - amount_ge_pivot);
            r_store -= amount_ge_pivot;
        }
    }

/* partition and save vec_left[8] and vec_right[8] */
#pragma GCC unroll 8
    for (int ii = 0; ii < num_unroll; ++ii) {
        int32_t amount_ge_pivot
                = partition_vec<vtype>(arr,
                                       l_store,
                                       r_store + vtype::numlanes,
                                       vec_left[ii],
                                       pivot_vec,
                                       &min_vec,
                                       &max_vec);
        l_store += (vtype::numlanes - amount_ge_pivot);
        r_store -= amount_ge_pivot;
    }
#pragma GCC unroll 8
    for (int ii = 0; ii < num_unroll; ++ii) {
        int32_t amount_ge_pivot
                = partition_vec<vtype>(arr,
                                       l_store,
                                       r_store + vtype::numlanes,
                                       vec_right[ii],
                                       pivot_vec,
                                       &min_vec,
                                       &max_vec);
        l_store += (vtype::numlanes - amount_ge_pivot);
        r_store -= amount_ge_pivot;
    }
    *smallest = vtype::reducemin(min_vec);
    *biggest = vtype::reducemax(max_vec);
    return l_store;
}
#endif // AVX512_QSORT_COMMON
