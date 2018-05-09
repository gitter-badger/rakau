#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__AVX2__) || defined(__AVX__)

#include <immintrin.h>

#endif

#include <boost/numeric/conversion/cast.hpp>
#include <boost/sort/spreadsort/spreadsort.hpp>

#include <xsimd/xsimd.hpp>

#include "libmorton/morton.h"

inline namespace detail
{

template <typename... Args>
inline void ignore_args(const Args &...)
{
}

class simple_timer
{
public:
    simple_timer(const std::string &desc) : m_desc(desc), m_start(std::chrono::high_resolution_clock::now()) {}
    double elapsed() const
    {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - m_start)
                .count());
    }
    ~simple_timer()
    {
        std::cout << "Elapsed time for '" + m_desc + "': "
                  << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now()
                                                                           - m_start)
                         .count()
                  << u8"μs\n";
    }

private:
    const std::string m_desc;
    const std::chrono::high_resolution_clock::time_point m_start;
};

template <std::size_t NDim, typename Out>
struct morton_encoder {
};

template <>
struct morton_encoder<3, std::uint64_t> {
    template <typename It>
    std::uint64_t operator()(It it) const
    {
        const auto x = *it;
        const auto y = *(it + 1);
        const auto z = *(it + 2);
        assert(x < (1ul << 21));
        assert(y < (1ul << 21));
        assert(z < (1ul << 21));
        assert(!(::m3D_e_sLUT<std::uint64_t, std::uint32_t>(x, y, z) >> 63u));
        assert((::morton3D_64_encode(x, y, z) == ::m3D_e_sLUT<std::uint64_t, std::uint32_t>(x, y, z)));
        return ::m3D_e_sLUT<std::uint64_t, std::uint32_t>(std::uint32_t(x), std::uint32_t(y), std::uint32_t(z));
    }
};

template <>
struct morton_encoder<2, std::uint64_t> {
    std::uint64_t operator()(std::uint64_t x, std::uint64_t y) const
    {
        // TODO fix.
        assert(x < (1ul << 31));
        assert(y < (1ul << 31));
        assert(!(::morton2D_64_encode(x, y) >> 62u));
        return ::morton2D_64_encode(x, y);
    }
};

template <typename UInt, std::size_t NDim>
constexpr unsigned get_cbits()
{
    constexpr unsigned nbits = std::numeric_limits<UInt>::digits;
    static_assert(nbits > NDim, "The number of bits must be greater than the number of dimensions.");
    return static_cast<unsigned>(nbits / NDim - !(nbits % NDim));
}

template <typename UInt, std::size_t NDim>
inline constexpr unsigned cbits_v = get_cbits<UInt, NDim>();

template <typename T, std::size_t... N>
inline void increase_children_count(T &tup, const std::index_sequence<N...> &)
{
    auto inc = [](auto &n) {
        if (n == std::numeric_limits<std::remove_reference_t<decltype(n)>>::max()) {
            throw std::overflow_error(
                "overflow error when incrementing the children count during the construction of a tree");
        }
        ++n;
    };
    ignore_args(inc);
    (..., inc(get<N>(tup)));
}

// Small helper to get the tree level of a nodal code.
template <std::size_t NDim, typename UInt>
inline unsigned tree_level(UInt n)
{
    // TODO clzl wrappers.
#if !defined(NDEBUG)
    constexpr unsigned cbits = cbits_v<UInt, NDim>;
#endif
    constexpr unsigned ndigits = std::numeric_limits<UInt>::digits;
    assert(n);
    assert(!((ndigits - 1u - unsigned(__builtin_clzl(n))) % NDim));
    auto retval = static_cast<unsigned>((ndigits - 1u - unsigned(__builtin_clzl(n))) / NDim);
    assert(cbits >= retval);
    assert((cbits - retval) * NDim < ndigits);
    return retval;
}

// Small functor to compare nodal codes.
template <std::size_t NDim>
struct node_comparer {
    template <typename UInt>
    bool operator()(UInt n1, UInt n2) const
    {
        constexpr unsigned cbits = cbits_v<UInt, NDim>;
        const auto tl1 = tree_level<NDim>(n1);
        const auto tl2 = tree_level<NDim>(n2);
        const auto s_n1 = n1 << ((cbits - tl1) * NDim);
        const auto s_n2 = n2 << ((cbits - tl2) * NDim);
        return s_n1 < s_n2 || (s_n1 == s_n2 && tl1 < tl2);
    }
};

inline constexpr unsigned avx_version =
#if defined(__AVX2__)
    2
#elif defined(__AVX__)
    1
#else
    0
#endif
    ;

#if defined(__AVX2__)

// Rotate a vector of AVX single-precision floats using AVX2.
inline __m256 rotate_m256(__m256 x)
{
    // NOTE: this is an AVX2 specific intrinsic. We can emulate it in terms
    // of AVX with 3 instructions (see below).
    return _mm256_permutevar8x32_ps(x, _mm256_set_epi32(0, 7, 6, 5, 4, 3, 2, 1));
}

#elif defined(__AVX__)

// Rotate a vector of AVX single-precision floats using AVX.
//
// See the second answer here:
// https://stackoverflow.com/questions/19516585/shifting-sse-avx-registers-32-bits-left-and-right-while-shifting-in-zeros
//
// This is slightly modified in the second line as we don't want the zeroing
// feature provided by _mm256_permute2f128_ps.
inline __m256 rotate_m256(__m256 x)
{
    // NOTE: 0x39 == 0011 1001, 0x88 == 1000 1000.
    // NOTE: x = [x7 x6 ... x0].
    const __m256 t0 = _mm256_permute_ps(x, 0x39);        // [x4  x7  x6  x5  x0  x3  x2  x1]
    const __m256 t1 = _mm256_permute2f128_ps(t0, t0, 1); // [x0  x3  x2  x1  x4  x7  x6  x5]
    return _mm256_blend_ps(t0, t1, 0x88);                // [x0  x7  x6  x5  x4  x3  x2  x1]
}

#endif

#if defined(__AVX__)

// Rotate a vector of AVX double-precision floats using AVX.
// NOTE: it seems like there's no AVX2-specific way of doing this,
// so we do it the vanilla AVX way.
inline __m256d rotate_m256(__m256d x)
{
    // NOTE: same idea as above, just different constants.
    // NOTE: x = [x3 x2 x1 x0].
    const __m256d t0 = _mm256_permute_pd(x, 5);           // [x2 x3 x0 x1]
    const __m256d t1 = _mm256_permute2f128_pd(t0, t0, 1); // [x0 x1 x2 x3]
    return _mm256_blend_pd(t0, t1, 10);                   // [x0 x3 x2 x1]
}

// Compute 1/sqrt(x)**3 using the AVX intrinsic _mm256_rsqrt_ps, refined with a Newton iteration.
inline xsimd::batch<float, 8> inv_sqrt_3(xsimd::batch<float, 8> x)
{
    // Newton iteration: y1 = y0/2 * (3 - x*y0**2).
    const xsimd::batch<float, 8> three(3.f);
    const xsimd::batch<float, 8> y0 = _mm256_rsqrt_ps(x);
    const xsimd::batch<float, 8> xy0 = x * y0;
    const xsimd::batch<float, 8> half_y0 = y0 * .5f;
    const xsimd::batch<float, 8> three_minus_muls = xsimd::fnma(xy0, y0, three);
    const xsimd::batch<float, 8> res = half_y0 * three_minus_muls;
    return res * res * res;
}

#endif

// Apply the indirect sort defined by the vector of indices 'perm'
// into the 'values' vector. E.g., if in input
//
// values = [a, c, d, b]
// perm = [0, 3, 1, 2]
//
// then in output
//
// values = [a, b, c, d]
//
// and perm is unchanged (but it will be subject to writes inside this function).
template <typename VVec, typename PVec>
inline void apply_isort(VVec &values, PVec &perm)
{
    using std::swap;
    using idx_t = typename PVec::value_type;
    assert(values.size() == perm.size());
#if !defined(NDEBUG)
    const auto orig_perm = perm;
#endif
    const auto size = perm.size();
    for (decltype(perm.size()) i = 0; i < size; ++i) {
        if (perm[i] >= (idx_t(1) << (std::numeric_limits<idx_t>::digits - 1))) {
            // The value was swapped in the correct position
            // in an earlier iteration. Flip back the permutation
            // index and move to the next value.
            perm[i] = static_cast<idx_t>(-perm[i] - 1u);
            continue;
        }
        auto j = perm[i];
        if (i != j) {
            // The current value is not at its correct position. The idea
            // is then to keep on swapping the current value with other
            // values to the right, until it eventually falls into the correct
            // place. The other values swapped in this process are all put
            // into the correct position.
            auto k = i;
            while (true) {
                // Move into the current position the correct value,
                // swapping out the incorrect value.
                swap(values[k], values[j]);
                // The current position now contains the correct value.
                // Mark the permutation index with the negative of the original index.
                perm[k] = static_cast<idx_t>(idx_t(-1) - perm[k]);
                if (perm[j] == i) {
                    // The previously incorrect value was swapped into its correct
                    // position (note that the original index of the incorrect value
                    // is i). Mark the corresponding index and break out.
                    perm[j] = static_cast<idx_t>(idx_t(-1) - perm[j]);
                    break;
                }
                // The previously incorrect value was swapped into another
                // incorrect position. Iterate the procedure.
                k = j;
                j = perm[k];
            }
            // NOTE: during the first iteration of the cycle above we certainly
            // negated the i-th slot of perm at the first swapout of the incorrect
            // value. Since at the next iteration we will move to i + 1, we won't
            // have any chance to restore the original permutation index, so we do
            // it here.
            perm[i] = static_cast<idx_t>(-perm[i] - 1u);
        }
    }
    assert(perm == orig_perm);
}

} // namespace detail

template <typename UInt, typename F, std::size_t NDim>
class tree
{
    static_assert(NDim);
    // cbits shortcut.
    static constexpr unsigned cbits = cbits_v<UInt, NDim>;
    // Main vector type. Use the SIMD-aware allocator in order
    // to enable aligned loads/stores where possible.
    template <typename T>
    using v_type = std::vector<T, XSIMD_DEFAULT_ALLOCATOR(T)>;

public:
    using size_type = typename v_type<F>::size_type;

private:
    static auto &get_v_ind()
    {
        static thread_local v_type<size_type> v_ind;
        return v_ind;
    }
    template <unsigned ParentLevel, typename CTuple, typename CIt>
    void build_tree_impl(std::deque<size_type> &children_count, CTuple &ct, UInt parent_code, CIt begin, CIt end)
    {
        if constexpr (ParentLevel < cbits) {
            // We should never be invoking this on an empty range.
            assert(begin != end);
            // On entry, the range [begin, end) contains the codes
            // of all the particles belonging to the parent node.
            // parent_code is the nodal code of the parent node.
            //
            // We want to iterate over the current children nodes
            // (of which there might be up to 2**NDim). A child exists if
            // it contains at least 1 particle. If it contains > m_max_leaf_n particles,
            // it is an internal (i.e., non-leaf) node and we go deeper. If it contains <= m_max_leaf_n
            // particles, it is a leaf node, we stop going deeper and move to its sibling.
            //
            // This is the node prefix: it is the nodal code of the parent without the most significant bit.
            const auto node_prefix = parent_code - (UInt(1) << (ParentLevel * NDim));
            for (UInt i = 0; i < (UInt(1) << NDim); ++i) {
                // Compute the first and last possible codes for the current child node.
                // They both start with (from MSB to LSB):
                // - current node prefix,
                // - i.
                // The first possible code is then right-padded with all zeroes, the last possible
                // code is right-padded with ones.
                const auto p_first = static_cast<UInt>((node_prefix << ((cbits - ParentLevel) * NDim))
                                                       + (i << ((cbits - ParentLevel - 1u) * NDim)));
                const auto p_last
                    = static_cast<UInt>(p_first + ((UInt(1) << ((cbits - ParentLevel - 1u) * NDim)) - 1u));
                // Verify that begin contains the first value equal to or greater than p_first.
                assert(begin == std::lower_bound(begin, end, p_first));
                // Determine the end of the child node: it_end will point to the first value greater
                // than the largest possible code for the current child node.
                const auto it_end = std::upper_bound(begin, end, p_last);
                // Compute the number of particles.
                const auto npart = std::distance(begin, it_end);
                assert(npart >= 0);
                if (npart) {
                    // npart > 0, we have a node. Compute its nodal code by moving up the
                    // parent nodal code by NDim and adding the current child node index i.
                    const auto cur_code = static_cast<UInt>((parent_code << NDim) + i);
                    // Add the node to the tree.
                    m_tree.emplace_back(
                        cur_code,
                        std::array<size_type, 3>{static_cast<size_type>(std::distance(m_codes.begin(), begin)),
                                                 static_cast<size_type>(std::distance(m_codes.begin(), it_end)),
                                                 // NOTE: the children count gets inited to zero. It
                                                 // will be filled in later.
                                                 size_type(0)},
                        // NOTE: make sure mass and coords are initialised in a known state (i.e.,
                        // zero for C++ floating-point).
                        0, std::array<F, NDim>{});
                    // Add a counter to the deque. The newly-added node has zero children initially.
                    children_count.emplace_back(0);
                    // Increase the children count of the parents.
                    increase_children_count(ct, std::make_index_sequence<std::tuple_size_v<CTuple>>{});
                    if (static_cast<std::make_unsigned_t<decltype(std::distance(begin, it_end))>>(npart)
                        > m_max_leaf_n) {
                        // Add a new element to the children counter tuple, pointing to
                        // the value we just added to the deque.
                        auto new_ct = std::tuple_cat(ct, std::tie(children_count.back()));
                        // The node is an internal one, go deeper.
                        build_tree_impl<ParentLevel + 1u>(children_count, new_ct, cur_code, begin, it_end);
                    }
                }
                // Move to the next child node.
                begin += npart;
            }
        } else {
            // NOTE: if we end up here, it means we walked through all the recursion levels
            // and we cannot go any deeper. This will be a children with a number of particles
            // greater than m_max_leaf_n.
            // GCC warnings about unused params.
            ignore_args(parent_code, end);
        }
    }
    void build_tree()
    {
        simple_timer st("node building");
        // Make sure this is not invoked on an empty tree.
        assert(m_codes.size());
        // NOTE: in the tree builder code, we will be moving around in the codes
        // vector using random access iterators. Thus, we must ensure the difference
        // type of the iterator can represent the size of the codes vector.
        using it_diff_t = typename std::iterator_traits<decltype(m_codes.begin())>::difference_type;
        using it_udiff_t = std::make_unsigned_t<it_diff_t>;
        if (m_codes.size() > static_cast<it_udiff_t>(std::numeric_limits<it_diff_t>::max())) {
            throw std::overflow_error("the number of particles (" + std::to_string(m_codes.size())
                                      + ") is too large, and it results in an overflow condition");
        }
        // The structure to keep track of the children count. This
        // has to be a deque (rather than a vector) because we need
        // to keep references to elements in it as we grow it.
        static thread_local std::deque<size_type> children_count;
        children_count.resize(0);
        // Add the counter for the root node.
        children_count.emplace_back(0);
        // The initial tuple containing the children counters for the parents
        // of a node.
        auto c_tuple = std::tie(children_count.back());
        // Add the root node.
        m_tree.emplace_back(1,
                            std::array<size_type, 3>{size_type(0), size_type(m_codes.size()),
                                                     // NOTE: the children count gets inited to zero. It
                                                     // will be filled in later.
                                                     size_type(0)},
                            // NOTE: make sure mass and coords are initialised in a known state (i.e.,
                            // zero for C++ floating-point).
                            0, std::array<F, NDim>{});
        // Build the rest.
        build_tree_impl<0>(children_count, c_tuple, 1, m_codes.begin(), m_codes.end());
        // Check the result.
        assert(children_count.size() == m_tree.size());
        // Check the tree is sorted according to the nodal code comparison.
        assert(std::is_sorted(m_tree.begin(), m_tree.end(), [](const auto &t1, const auto &t2) {
            node_comparer<NDim> nc;
            return nc(get<0>(t1), get<0>(t2));
        }));
        // Check that all the nodes contain at least 1 element.
        assert(
            std::all_of(m_tree.begin(), m_tree.end(), [](const auto &tup) { return get<1>(tup)[1] > get<1>(tup)[0]; }));
        // Copy over the children count.
        auto tree_it = m_tree.begin();
        for (auto it = children_count.begin(); it != children_count.end(); ++it, ++tree_it) {
            get<1>(*tree_it)[2] = *it;
        }
        // Check that size_type can represent the size of the tree.
        if (m_tree.size() > std::numeric_limits<size_type>::max()) {
            throw std::overflow_error("the size of the tree (" + std::to_string(m_tree.size())
                                      + ") is too large, and it results in an overflow condition");
        }
    }
    void build_tree_properties()
    {
        simple_timer st("tree properties");
        for (auto &tup : m_tree) {
            // Get the indices and the size for the current node.
            const auto begin = get<1>(tup)[0];
            const auto end = get<1>(tup)[1];
            assert(end > begin);
            const auto size = end - begin;
            // Compute the total mass.
            const auto tot_mass = std::accumulate(m_masses.data() + begin, m_masses.data() + end, F(0));
            // Compute the COM for the coordinates.
            for (std::size_t j = 0; j < NDim; ++j) {
                F acc(0);
                auto m_ptr = m_masses.data() + begin;
                auto c_ptr = m_coords[j].data() + begin;
                for (std::remove_const_t<decltype(size)> i = 0; i < size; ++i) {
                    acc += m_ptr[i] * c_ptr[i];
                }
                get<3>(tup)[j] = acc / tot_mass;
            }
            // Store the total mass.
            get<2>(tup) = tot_mass;
        }
    }

public:
    template <typename It>
    explicit tree(const F &box_size, It m_it, std::array<It, NDim> c_it, size_type N, size_type max_leaf_n,
                  size_type ncrit)
        : m_box_size(box_size), m_max_leaf_n(max_leaf_n), m_ncrit(ncrit)
    {
        simple_timer st("overall tree construction");
        // Check the box size.
        if (!std::isfinite(box_size) || box_size <= F(0)) {
            throw std::invalid_argument("the box size must be a finite positive value, but it is "
                                        + std::to_string(box_size) + " instead");
        }
        // Check the max_leaf_n param.
        if (!max_leaf_n) {
            throw std::invalid_argument("the maximum number of particles per leaf must be nonzero");
        }
        // Check the ncrit param.
        if (!ncrit) {
            throw std::invalid_argument(
                "the critical number of particles for the vectorised computation of the accelerations must be nonzero");
        }
        // Prepare the vectors.
        m_masses.resize(N);
        for (auto &vc : m_coords) {
            vc.resize(N);
        }
        // Get out soon if there's nothing to do.
        if (!N) {
            return;
        }
        // NOTE: this ensures that, from now on, we can just cast
        // freely between the size types of the masses/coords and codes vectors.
        m_codes.resize(boost::numeric_cast<decltype(m_codes.size())>(N));
        // Function to discretise the input NDim floating-point coordinates starting at 'it'
        // into a box of a given size box_size.
        auto disc_coords = [&box_size](auto it) {
            constexpr UInt factor = UInt(1) << cbits;
            std::array<UInt, NDim> retval;
            for (std::size_t i = 0; i < NDim; ++i, ++it) {
                const auto &x = *it;
                // Translate and rescale the coordinate so that -box_size/2 becomes zero
                // and box_size/2 becomes 1.
                auto tmp = (x + box_size / F(2)) / box_size;
                // Rescale by factor.
                tmp *= factor;
                // Check: don't end up with a nonfinite value.
                if (!std::isfinite(tmp)) {
                    throw std::invalid_argument("Not finite!");
                }
                // Check: don't end up outside the [0, factor) range.
                if (tmp < F(0) || tmp >= F(factor)) {
                    throw std::invalid_argument("Out of bounds!");
                }
                // Cast to UInt and write to retval.
                retval[i] = static_cast<UInt>(tmp);
                // Last check, make sure we don't overflow.
                if (retval[i] >= factor) {
                    throw std::invalid_argument("Out of bounds! (after cast)");
                }
            }
            return retval;
        };
        // Temporary structure used in the encoding.
        std::array<F, NDim> tmp_coord;
        // The encoder object.
        morton_encoder<NDim, UInt> me;
        // Determine the particles' codes, and fill in the particles' data.
        auto &v_ind = get_v_ind();
        v_ind.resize(boost::numeric_cast<decltype(v_ind.size())>(N));
        for (size_type i = 0; i < N; ++i, ++m_it) {
            // Write the coords in the temp structure and in the data members.
            for (std::size_t j = 0; j < NDim; ++j) {
                tmp_coord[j] = *c_it[j];
                m_coords[j][i] = *c_it[j];
            }
            // Store the mass.
            m_masses[i] = *m_it;
            // Compute and store the code.
            m_codes[i] = me(disc_coords(tmp_coord.begin()).begin());
            // Store the index.
            v_ind[i] = i;
            // Increase the coordinates iterators.
            for (auto &_ : c_it) {
                ++_;
            }
        }
        // Do the sorting of v_ind.
        boost::sort::spreadsort::integer_sort(
            v_ind.begin(), v_ind.end(),
            [this](const size_type &idx, unsigned offset) { return this->m_codes[idx] >> offset; },
            [this](const size_type &idx1, const size_type &idx2) { return this->m_codes[idx1] < this->m_codes[idx2]; });
        // Apply the permutation to the data members.
        // NOTE: the indices range is [0, N - 1]. The apply_isort() function requires the maximum
        // value in the indices vector (N - 1, in this case) to be less than 2**(nbits - 1), as it
        // uses the highest bit internally for a special purpose.
        if (N > (size_type(1) << (std::numeric_limits<size_type>::digits - 1))) {
            throw std::overflow_error("the number of particles (" + std::to_string(N)
                                      + ") is too large, and it results in an overflow condition");
        }
        apply_isort(m_codes, v_ind);
        // Make sure the sort worked as intended.
        assert(std::is_sorted(m_codes.begin(), m_codes.end()));
        for (std::size_t j = 0; j < NDim; ++j) {
            apply_isort(m_coords[j], v_ind);
        }
        apply_isort(m_masses, v_ind);
        // Now let's proceed to the tree construction.
        build_tree();
        // Now move to the computation of the COM of the nodes.
        build_tree_properties();
    }
    friend std::ostream &operator<<(std::ostream &os, const tree &t)
    {
        static_assert(unsigned(std::numeric_limits<UInt>::digits) <= std::numeric_limits<std::size_t>::max());
        os << "Total number of particles: " << t.m_codes.size() << '\n';
        os << "Total number of nodes    : " << t.m_tree.size() << "\n\n";
        os << "First few nodes:\n";
        constexpr unsigned max_nodes = 20;
        auto i = 0u;
        for (const auto &tup : t.m_tree) {
            if (i > max_nodes) {
                break;
            }
            os << std::bitset<std::numeric_limits<UInt>::digits>(get<0>(tup)) << '|' << get<1>(tup)[0] << ','
               << get<1>(tup)[1] << ',' << get<1>(tup)[2] << "|" << get<2>(tup) << "|[";
            for (std::size_t j = 0; j < NDim; ++j) {
                os << get<3>(tup)[j];
                if (j < NDim - 1u) {
                    os << ", ";
                }
            }
            os << "]\n";
            ++i;
        }
        if (i > max_nodes) {
            std::cout << "...\n";
        }
        return os;
    }

private:
    // Add to out the acceleration induced by the particles in the [begin, end) range onto
    // the particle at index pidx.
    void add_acc_from_range(std::array<F, NDim> &out, size_type begin, size_type end, size_type pidx) const
    {
#if 0
        using b_type = xsimd::simd_type<F>;
        constexpr std::size_t inc = b_type::size;
        const auto size = end - begin;
        const auto vec_size = static_cast<size_type>(size - size % inc);
        const auto vec_end = begin + vec_size;
        auto xdata = m_coords[0].data();
        auto ydata = m_coords[1].data();
        auto zdata = m_coords[2].data();
        auto mdata = m_masses.data();
        const auto x0 = xdata[pidx], y0 = ydata[pidx], z0 = zdata[pidx];
        for (; begin != vec_end; begin += inc) {
            b_type xvec = xsimd::load_unaligned(xdata + begin);
            b_type diff_x = xvec - x0;
            b_type yvec = xsimd::load_unaligned(ydata + begin);
            b_type diff_y = yvec - y0;
            b_type zvec = xsimd::load_unaligned(zdata + begin);
            b_type diff_z = zvec - z0;
            b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
            b_type dist = xsimd::sqrt(dist2);
            b_type dist3 = dist2 * dist;
            b_type mvec = xsimd::load_unaligned(mdata + begin);
            b_type m_dist3 = mvec / dist3;
            b_type xacc = diff_x * m_dist3;
            b_type yacc = diff_y * m_dist3;
            b_type zacc = diff_z * m_dist3;
            out[0] += xsimd::hadd(xacc);
            out[1] += xsimd::hadd(yacc);
            out[2] += xsimd::hadd(zacc);
        }
#endif
        // Local arrays to store the target particle's coords
        // and the diff between the range's particles coordinates and the
        // target particle coordinates.
        std::array<F, NDim> pcoords, diffs;
        for (std::size_t j = 0; j < NDim; ++j) {
            pcoords[j] = m_coords[j][pidx];
        }
        for (; begin != end; ++begin) {
            assert(begin != pidx);
            F dist2(0);
            for (std::size_t j = 0; j < NDim; ++j) {
                diffs[j] = m_coords[j][begin] - pcoords[j];
                dist2 += diffs[j] * diffs[j];
            }
            const F dist = std::sqrt(dist2);
            const F dist3 = dist * dist2;
            const F m_dist3 = m_masses[begin] / dist3;
            for (std::size_t j = 0; j < NDim; ++j) {
                out[j] += m_dist3 * diffs[j];
            }
        }
    }
    // NOTE: this is templatised over Level (in addition to SLevel) because
    // like that we get better performance (~4% or so). This probably has to
    // do with the fact that, with 2 templ. params, we get a unique version
    // of this function each time it is invoked, whereas with 1 templ. param
    // we have multiple calls to the same function throughout a tree traversal.
    // Probably this impacts inlining etc.
    template <unsigned Level, unsigned SLevel>
    void scalar_acc_from_node(std::array<F, NDim> &out, const F &theta2, UInt code, size_type pidx, size_type begin,
                              size_type end, const F &node_size2) const
    {
        if constexpr (SLevel <= cbits) {
            // SLevel cannot be zero: the root node has no siblings and, even if it is a leaf node,
            // that case will be handled elsewhere.
            assert(SLevel);
            // Make sure node_size2 is ok.
            assert(node_size2 == m_box_size / (UInt(1) << SLevel) * m_box_size / (UInt(1) << SLevel));
            // Local array to store the difference between the node's COM coordinates
            // and the particle coordinates.
            std::array<F, NDim> diffs;
            // Determine the distance**2 between the particle and the COM of the node.
            F dist2(0);
            for (std::size_t j = 0; j < NDim; ++j) {
                // Current node COM coordinate.
                const F &node_x = get<3>(m_tree[begin])[j];
                // Current part coordinate.
                const F &part_x = m_coords[j][pidx];
                diffs[j] = node_x - part_x;
                dist2 += diffs[j] * diffs[j];
            }
            // Check the BH acceptance criterion.
            if (node_size2 < theta2 * dist2) {
                // We can approximate the acceleration with the COM of the
                // current node.
                const F dist = std::sqrt(dist2);
                const F dist3 = dist2 * dist;
                // NOTE: this is the mass of the COM divided by dist**3.
                const F com_mass_dist3 = get<2>(m_tree[begin]) / dist3;
                for (std::size_t j = 0; j < NDim; ++j) {
                    out[j] += com_mass_dist3 * diffs[j];
                }
                return;
            }
            // The node does *not* satisfy the BH acceptance criterion.
            // Check if the current node is a leaf: if that's the case,
            // we cannot go deeper and we need to compute the interaction
            // from all the particles in the node.
            const auto n_children = get<1>(m_tree[begin])[2];
            if (!n_children) {
                // Leaf node, compute the acceleration.
                add_acc_from_range(out, get<1>(m_tree[begin])[0], get<1>(m_tree[begin])[1], pidx);
                return;
            }
            // We can go deeper in the tree. Bump up begin to move to the first child.
            // Determine the size of the node at the next level.
            const F next_node_size = m_box_size / (UInt(1) << (SLevel + 1u));
            const F next_node_size2 = next_node_size * next_node_size;
            for (++begin; begin != end; begin += get<1>(m_tree[begin])[2] + 1u) {
                scalar_acc_from_node<Level, SLevel + 1u>(out, theta2, code, pidx, begin,
                                                         begin + get<1>(m_tree[begin])[2] + 1u, next_node_size2);
            }
        } else {
            // GCC warnings.
            ignore_args(code, pidx, begin, end);
            // We should never get here: if we use all cbits, the deepest node will be a leaf
            // and we will have handled the case earlier.
            assert(false);
        }
    }
    template <unsigned Level>
    void scalar_acc(std::array<F, NDim> &out, const F &theta2, UInt code, size_type pidx, size_type begin,
                    size_type end) const
    {
        assert(code == m_codes[pidx]);
        if constexpr (Level <= cbits) {
            // This is the nodal code of the node in which the particle is at the current level.
            // We compute it via the following:
            // - add an extra 1 bit in the MSB direction,
            // - shift down the result depending on the current level.
            const auto part_node_code
                = static_cast<UInt>(((UInt(1) << (cbits * NDim)) + code) >> ((cbits - Level) * NDim));
            // Determine the size of the node at the current level.
            const F node_size = m_box_size / (UInt(1) << Level);
            const F node_size2 = node_size * node_size;
            for (auto idx = begin; idx != end;
                 // NOTE: when incrementing idx, we need to add 1 to the
                 // total number of children in order to point to the next sibling.
                 idx += get<1>(m_tree[idx])[2] + 1u) {
                // Get the nodal code of the current sibling.
                const auto cur_node_code = get<0>(m_tree[idx]);
                // Get the number of children of the current sibling.
                const auto n_children = get<1>(m_tree[idx])[2];
                if (part_node_code == cur_node_code) {
                    // We are in the sibling that contains the particle. Check if it is a leaf
                    // or an internal node.
                    if (n_children) {
                        // Internal node, go one level deeper. The new indices range must
                        // start from the position immediately past idx (i.e., the first children node) and have a size
                        // equal to the number of children.
                        scalar_acc<Level + 1u>(out, theta2, code, pidx, idx + 1u, idx + 1u + n_children);
                    } else {
                        // The particle's node has no children, hence it is *the* leaf node
                        // containing the target particle.
                        const auto leaf_begin = get<1>(m_tree[idx])[0];
                        const auto leaf_end = get<1>(m_tree[idx])[1];
                        // Double check the particle's code is indeed in the leaf node.
                        assert(pidx >= leaf_begin && pidx < leaf_end);
                        // Add the acceleration from the leaf node. We split it
                        // in two parts as we want to avoid adding the acceleration
                        // from the particle itself.
                        add_acc_from_range(out, leaf_begin, pidx, pidx);
                        add_acc_from_range(out, pidx + 1u, leaf_end, pidx);
                    }
                } else {
                    // Compute the acceleration from the current sibling node.
                    scalar_acc_from_node<Level, Level>(out, theta2, code, pidx, idx, idx + 1u + n_children, node_size2);
                }
            }
        } else {
            // GCC warnings.
            ignore_args(code, pidx, begin, end);
            // We would end up here only if the leaf node containing the target
            // particle had children, but that is impossible.
            assert(false);
        }
    }

public:
    template <typename OutIt>
    void scalar_accs(OutIt out_it, const F &theta) const
    {
        simple_timer st("scalar acc computation");
        const auto theta2 = theta * theta;
        std::array<F, NDim> tmp;
        for (size_type i = 0; i < m_codes.size(); ++i) {
            // Init the acc vector to zero.
            for (auto &c : tmp) {
                c = F(0);
            }
            // Write the acceleration into tmp.
            scalar_acc<0>(tmp, theta2, m_codes[i], i, size_type(0), size_type(m_tree.size()));
            // Copy the result to the output iterator.
            for (const auto &c : tmp) {
                *out_it++ = c;
            }
        }
    }

private:
    // Temporary storage used to store the distances between the particles
    // of a node and the COM of another node while traversing the tree.
    static auto &vec_acc_tmp_vecs()
    {
        static thread_local std::array<v_type<F>, NDim + 1u> tmp_vecs;
        return tmp_vecs;
    }
    // Temporary storage to accumulate the accelerations induced on the
    // particles of a critical node. Data in here will be copied to
    // the output array after the accelerations from all the other
    // particles/nodes in the domain have been computed.
    static auto &vec_acc_tmp_res()
    {
        static thread_local std::array<v_type<F>, NDim> tmp_res;
        return tmp_res;
    }
    // Compute the accelerations on the particles of a target node induced by the particles of another
    // node (the source). pidx and size are the starting index (in the particles arrays) and the size of the target
    // node. begin/end is the range, in the tree structure, encompassing the source node and its children.
    // node_size2 is the size of the source node. The accelerations will be written into the
    // temporary storage provided by vec_acc_tmp_res().
    template <unsigned Level, unsigned SLevel>
    void vec_acc_from_node(const F &theta2, size_type pidx, size_type size, size_type begin, size_type end,
                           const F &node_size2) const
    {
        if constexpr (SLevel <= cbits) {
            // Check that node_size2 is correct.
            assert(node_size2 == m_box_size / (UInt(1) << SLevel) * m_box_size / (UInt(1) << SLevel));
            // Prepare pointers to the input and output data.
            auto &tmp_res = vec_acc_tmp_res();
            std::array<F *, NDim> res_ptrs;
            std::array<const F *, NDim> c_ptrs;
            for (std::size_t j = 0; j < NDim; ++j) {
                res_ptrs[j] = tmp_res[j].data();
                c_ptrs[j] = m_coords[j].data() + pidx;
            }
            // Temporary vectors to store the pos differences and dist3 below.
            // We will store the data generated in the BH criterion check because
            // we can re-use it later to compute the accelerations.
            auto &tmp_vecs = vec_acc_tmp_vecs();
            std::array<F *, NDim + 1u> tmp_ptrs;
            for (std::size_t j = 0; j < NDim + 1u; ++j) {
                tmp_vecs[j].resize(size);
                tmp_ptrs[j] = tmp_vecs[j].data();
            }
            // Copy locally the COM coords of the source.
            const auto com_pos = get<3>(m_tree[begin]);
            // Check the distances of all the particles of the target
            // node from the COM of the source.
            bool bh_flag = true;
            size_type i = 0;
            if constexpr (NDim == 3u) {
                // The SIMD-accelerated part.
                using b_type = xsimd::simd_type<F>;
                constexpr auto inc = b_type::size;
                const auto vec_size = static_cast<size_type>(size - size % inc);
                const b_type node_size2_vec = xsimd::set_simd(node_size2);
                auto [x_ptr, y_ptr, z_ptr] = c_ptrs;
                const auto [x_com, y_com, z_com] = com_pos;
                auto [tmp_x, tmp_y, tmp_z, tmp_dist3] = tmp_ptrs;
                for (; i < vec_size; i += inc, x_ptr += inc, y_ptr += inc, z_ptr += inc, tmp_x += inc, tmp_y += inc,
                                     tmp_z += inc, tmp_dist3 += inc) {
                    const b_type xvec = xsimd::load_unaligned(x_ptr);
                    const b_type yvec = xsimd::load_unaligned(y_ptr);
                    const b_type zvec = xsimd::load_unaligned(z_ptr);
                    const b_type diffx = x_com - xvec;
                    const b_type diffy = y_com - yvec;
                    const b_type diffz = z_com - zvec;
                    const b_type dist2 = diffx * diffx + diffy * diffy + diffz * diffz;
                    if (xsimd::any(node_size2_vec >= theta2 * dist2)) {
                        // At least one particle in the current batch fails the BH criterion
                        // check. Mark the bh_flag as false, and set i to size in order
                        // to skip the scalar calculation later. Then break out.
                        bh_flag = false;
                        i = size;
                        break;
                    }
                    xsimd::store_aligned(tmp_x, diffx);
                    xsimd::store_aligned(tmp_y, diffy);
                    xsimd::store_aligned(tmp_z, diffz);
                    if constexpr (avx_version && std::is_same_v<F, float>) {
                        xsimd::store_aligned(tmp_dist3, inv_sqrt_3(dist2));
                    } else {
                        xsimd::store_aligned(tmp_dist3, xsimd::sqrt(dist2) * dist2);
                    }
                }
            }
            for (; i < size; ++i) {
                F dist2(0);
                for (std::size_t j = 0; j < NDim; ++j) {
                    // Store the differences for later use.
                    tmp_ptrs[j][i] = com_pos[j] - c_ptrs[j][i];
                    dist2 += tmp_ptrs[j][i] * tmp_ptrs[j][i];
                }
                if (node_size2 >= theta2 * dist2) {
                    // At least one of the particles in the target
                    // node is too close to the COM. Set the flag
                    // to false and exit.
                    bh_flag = false;
                    break;
                }
                // Store dist3 (or 1/dist3) for later use.
                tmp_ptrs[NDim][i] = (avx_version && std::is_same_v<F, float>) ? F(1) / (std::sqrt(dist2) * dist2)
                                                                              : std::sqrt(dist2) * dist2;
            }
            if (bh_flag) {
                // The source node satisfies the BH criterion for
                // all the particles of the target node. Add the accelerations.
                //
                // Load the mass of the COM of the sibling node.
                const auto m_com = get<2>(m_tree[begin]);
                i = 0;
                if constexpr (NDim == 3u) {
                    // The SIMD-accelerated part.
                    using b_type = xsimd::simd_type<F>;
                    constexpr auto inc = b_type::size;
                    const auto vec_size = static_cast<size_type>(size - size % inc);
                    auto [tmp_x, tmp_y, tmp_z, tmp_dist3] = tmp_ptrs;
                    auto [res_x, res_y, res_z] = res_ptrs;
                    for (; i < vec_size; i += inc, tmp_x += inc, tmp_y += inc, tmp_z += inc, tmp_dist3 += inc,
                                         res_x += inc, res_y += inc, res_z += inc) {
                        const b_type m_com_dist3_vec = (avx_version && std::is_same_v<F, float>)
                                                           ? m_com * xsimd::load_aligned(tmp_dist3)
                                                           : m_com / xsimd::load_aligned(tmp_dist3);
                        const b_type xdiff = xsimd::load_aligned(tmp_x);
                        const b_type ydiff = xsimd::load_aligned(tmp_y);
                        const b_type zdiff = xsimd::load_aligned(tmp_z);
                        xsimd::store_aligned(res_x, xsimd::fma(xdiff, m_com_dist3_vec, xsimd::load_aligned(res_x)));
                        xsimd::store_aligned(res_y, xsimd::fma(ydiff, m_com_dist3_vec, xsimd::load_aligned(res_y)));
                        xsimd::store_aligned(res_z, xsimd::fma(zdiff, m_com_dist3_vec, xsimd::load_aligned(res_z)));
                    }
                }
                for (; i < size; ++i) {
                    const auto m_com_dist3 = (avx_version && std::is_same_v<F, float>) ? m_com * tmp_ptrs[NDim][i]
                                                                                       : m_com / tmp_ptrs[NDim][i];
                    for (std::size_t j = 0; j < NDim; ++j) {
                        res_ptrs[j][i] += tmp_ptrs[j][i] * m_com_dist3;
                    }
                }
                return;
            }
            // At least one particle in the target node is too close to the
            // COM of the source node. If we can, we go deeper, otherwise we must compute
            // all the pairwise interactions between all the particles in the
            // target and source nodes.
            const auto n_children = get<1>(m_tree[begin])[2];
            if (!n_children) {
                // The source node is a leaf, compute all the accelerations induced by its
                // particles on the particles of the target node.
                //
                // NOTE: we do this here, rather than earlier, as it might be that the node
                // is far enough to satisfy the BH criterion. In such a case we save a lot
                // of operations, as we are avoiding all the pairwise interactions.
                //
                // Establish the range of the source node.
                const auto leaf_begin = get<1>(m_tree[begin])[0];
                const auto leaf_end = get<1>(m_tree[begin])[1];
                size_type i1 = 0;
                if constexpr (NDim == 3u && avx_version) {
                    // The size of the source node.
                    const auto size_leaf = leaf_end - leaf_begin;
                    using b_type = xsimd::simd_type<F>;
                    constexpr auto inc = b_type::size;
                    // Vector size of the target node.
                    const auto vec_size1 = static_cast<size_type>(size - size % inc);
                    // Vector size of the source node.
                    const auto vec_size2 = static_cast<size_type>(size_leaf - size_leaf % inc);
                    auto [x_ptr1, y_ptr1, z_ptr1] = c_ptrs;
                    auto [res_x, res_y, res_z] = res_ptrs;
                    for (; i1 < vec_size1; i1 += inc, x_ptr1 += inc, y_ptr1 += inc, z_ptr1 += inc, res_x += inc,
                                           res_y += inc, res_z += inc) {
                        // Load the current batch of target data.
                        const b_type xvec1 = xsimd::load_unaligned(x_ptr1);
                        const b_type yvec1 = xsimd::load_unaligned(y_ptr1);
                        const b_type zvec1 = xsimd::load_unaligned(z_ptr1);
                        // Init the pointers to the source data.
                        auto x_ptr2 = m_coords[0].data() + leaf_begin;
                        auto y_ptr2 = m_coords[1].data() + leaf_begin;
                        auto z_ptr2 = m_coords[2].data() + leaf_begin;
                        auto m_ptr = m_masses.data() + leaf_begin;
                        // Init the batches for computing the accelerations, loading the
                        // accumulated acceleration for the current batch.
                        b_type res_x_vec = xsimd::load_aligned(res_x);
                        b_type res_y_vec = xsimd::load_aligned(res_y);
                        b_type res_z_vec = xsimd::load_aligned(res_z);
                        size_type i2 = 0;
                        for (; i2 < vec_size2; i2 += inc, x_ptr2 += inc, y_ptr2 += inc, z_ptr2 += inc, m_ptr += inc) {
                            b_type xvec2 = xsimd::load_unaligned(x_ptr2);
                            b_type yvec2 = xsimd::load_unaligned(y_ptr2);
                            b_type zvec2 = xsimd::load_unaligned(z_ptr2);
                            b_type mvec = xsimd::load_unaligned(m_ptr);
                            b_type diff_x = xvec2 - xvec1;
                            b_type diff_y = yvec2 - yvec1;
                            b_type diff_z = zvec2 - zvec1;
                            b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                            b_type m_dist3;
                            if constexpr (avx_version && std::is_same_v<F, float>) {
                                m_dist3 = inv_sqrt_3(dist2) * mvec;
                            } else {
                                b_type dist = xsimd::sqrt(dist2);
                                b_type dist3 = dist * dist2;
                                m_dist3 = mvec / dist3;
                            }
                            res_x_vec = xsimd::fma(diff_x, m_dist3, res_x_vec);
                            res_y_vec = xsimd::fma(diff_y, m_dist3, res_y_vec);
                            res_z_vec = xsimd::fma(diff_z, m_dist3, res_z_vec);
                            for (std::size_t j = 1; j < inc; ++j) {
                                // Above we computed the element-wise accelerations of a source batch
                                // onto a target batch. We need to rotate the source batch
                                // inc - 1 times and perform again the computation in order
                                // to compute all possible particle-particle interactions.
                                xvec2 = rotate_m256(xvec2);
                                yvec2 = rotate_m256(yvec2);
                                zvec2 = rotate_m256(zvec2);
                                mvec = rotate_m256(mvec);
                                diff_x = xvec2 - xvec1;
                                diff_y = yvec2 - yvec1;
                                diff_z = zvec2 - zvec1;
                                dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                                if constexpr (avx_version && std::is_same_v<F, float>) {
                                    m_dist3 = inv_sqrt_3(dist2) * mvec;
                                } else {
                                    b_type dist = xsimd::sqrt(dist2);
                                    b_type dist3 = dist * dist2;
                                    m_dist3 = mvec / dist3;
                                }
                                res_x_vec = xsimd::fma(diff_x, m_dist3, res_x_vec);
                                res_y_vec = xsimd::fma(diff_y, m_dist3, res_y_vec);
                                res_z_vec = xsimd::fma(diff_z, m_dist3, res_z_vec);
                            }
                        }
                        // Iterate over the remaining particles of the source node, one by one.
                        for (; i2 < size_leaf; ++i2, ++x_ptr2, ++y_ptr2, ++z_ptr2, ++m_ptr) {
                            const b_type diff_x = *x_ptr2 - xvec1;
                            const b_type diff_y = *y_ptr2 - yvec1;
                            const b_type diff_z = *z_ptr2 - zvec1;
                            const b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                            b_type m_dist3;
                            if constexpr (avx_version && std::is_same_v<F, float>) {
                                m_dist3 = inv_sqrt_3(dist2) * *m_ptr;
                            } else {
                                const b_type dist = xsimd::sqrt(dist2);
                                const b_type dist3 = dist2 * dist;
                                m_dist3 = *m_ptr / dist3;
                            }
                            res_x_vec = xsimd::fma(diff_x, m_dist3, res_x_vec);
                            res_y_vec = xsimd::fma(diff_y, m_dist3, res_y_vec);
                            res_z_vec = xsimd::fma(diff_z, m_dist3, res_z_vec);
                        }
                        // Store the updated accelerations in the temporary vectors.
                        xsimd::store_aligned(res_x, res_x_vec);
                        xsimd::store_aligned(res_y, res_y_vec);
                        xsimd::store_aligned(res_z, res_z_vec);
                    }
                }
                // Local variables for the scalar computation.
                std::array<F, NDim> pos1, diffs;
                for (; i1 < size; ++i1) {
                    // Load the coordinates of the current particle
                    // in the target node.
                    for (std::size_t j = 0; j < NDim; ++j) {
                        pos1[j] = c_ptrs[j][i1];
                    }
                    // Iterate over the particles in the sibling node.
                    for (size_type i2 = leaf_begin; i2 < leaf_end; ++i2) {
                        F dist2(0);
                        for (std::size_t j = 0; j < NDim; ++j) {
                            diffs[j] = m_coords[j][i2] - pos1[j];
                            dist2 += diffs[j] * diffs[j];
                        }
                        const auto dist = std::sqrt(dist2);
                        const auto dist3 = dist * dist2;
                        const auto m_dist3 = m_masses[i2] / dist3;
                        for (std::size_t j = 0; j < NDim; ++j) {
                            tmp_res[j][i1] += diffs[j] * m_dist3;
                        }
                    }
                }
                return;
            }
            // We can go deeper in the tree.
            //
            // Determine the size of the node at the next level.
            const F next_node_size = m_box_size / (UInt(1) << (SLevel + 1u));
            const F next_node_size2 = next_node_size * next_node_size;
            // Bump up begin to move to the first child.
            for (++begin; begin != end; begin += get<1>(m_tree[begin])[2] + 1u) {
                vec_acc_from_node<Level, SLevel + 1u>(theta2, pidx, size, begin, begin + get<1>(m_tree[begin])[2] + 1u,
                                                      next_node_size2);
            }
        } else {
            ignore_args(pidx, size, begin, end);
            // NOTE: we cannot go deeper than the maximum level of the tree.
            // The n_children check above will prevent reaching this point at runtime.
            assert(false);
        }
    }
    // Compute the accelerations on the particles in a node due to the particles themselves.
    // node_begin is the starting index of the node in the particles arrays. npart is the
    // number of particles in the node. The self accelerations will be added to the accelerations
    // in the temporary storage.
    void vec_node_self_interactions(size_type node_begin, size_type npart) const
    {
        // Prepare common pointers to the input and output data.
        auto &tmp_res = vec_acc_tmp_res();
        const auto m_ptr = m_masses.data() + node_begin;
        if constexpr (NDim == 3u && avx_version) {
            // Shortcuts to the node coordinates/masses.
            const auto x_ptr = m_coords[0].data() + node_begin;
            const auto y_ptr = m_coords[1].data() + node_begin;
            const auto z_ptr = m_coords[2].data() + node_begin;
            // Shortcuts to the result vectors.
            auto res_x = tmp_res[0].data();
            auto res_y = tmp_res[1].data();
            auto res_z = tmp_res[2].data();
            using b_type = xsimd::simd_type<F>;
            constexpr auto inc = b_type::size;
            const auto vec_size = static_cast<size_type>(npart - npart % inc);
            auto [x_ptr1, y_ptr1, z_ptr1] = std::make_tuple(x_ptr, y_ptr, z_ptr);
            size_type i1 = 0;
            for (; i1 < vec_size;
                 i1 += inc, x_ptr1 += inc, y_ptr1 += inc, z_ptr1 += inc, res_x += inc, res_y += inc, res_z += inc) {
                // Load the current accelerations from the temporary result vectors.
                b_type res_x_vec = xsimd::load_aligned(res_x);
                b_type res_y_vec = xsimd::load_aligned(res_y);
                b_type res_z_vec = xsimd::load_aligned(res_z);
                // Load the data for the particles under consideration.
                const b_type xvec1 = xsimd::load_unaligned(x_ptr1);
                const b_type yvec1 = xsimd::load_unaligned(y_ptr1);
                const b_type zvec1 = xsimd::load_unaligned(z_ptr1);
                // Iterate over all the particles in the node and compute the accelerations
                // on the particles under consideration.
                auto [x_ptr2, y_ptr2, z_ptr2, m_ptr2] = std::make_tuple(x_ptr, y_ptr, z_ptr, m_ptr);
                size_type i2 = 0;
                for (; i2 < vec_size; i2 += inc, x_ptr2 += inc, y_ptr2 += inc, z_ptr2 += inc, m_ptr2 += inc) {
                    // Load the current batch of particles exerting gravity.
                    b_type xvec2 = xsimd::load_unaligned(x_ptr2);
                    b_type yvec2 = xsimd::load_unaligned(y_ptr2);
                    b_type zvec2 = xsimd::load_unaligned(z_ptr2);
                    b_type mvec2 = xsimd::load_unaligned(m_ptr2);
                    if (i2 != i1) {
                        // NOTE: if i2 == i1, we want to skip the first batch-batch
                        // permutation, as we don't want to compute self-accelerations.
                        b_type diff_x = xvec2 - xvec1;
                        b_type diff_y = yvec2 - yvec1;
                        b_type diff_z = zvec2 - zvec1;
                        b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                        b_type m2_dist3;
                        if constexpr (avx_version && std::is_same_v<F, float>) {
                            m2_dist3 = mvec2 * inv_sqrt_3(dist2);
                        } else {
                            b_type dist = xsimd::sqrt(dist2);
                            b_type dist3 = dist * dist2;
                            m2_dist3 = mvec2 / dist3;
                        }
                        res_x_vec = xsimd::fma(diff_x, m2_dist3, res_x_vec);
                        res_y_vec = xsimd::fma(diff_y, m2_dist3, res_y_vec);
                        res_z_vec = xsimd::fma(diff_z, m2_dist3, res_z_vec);
                    }
                    // Iterate over all the other possible batch-batch permutations
                    // by rotating the data in xvec2, yvec2, zvec2.
                    for (std::size_t j = 1; j < inc; ++j) {
                        xvec2 = rotate_m256(xvec2);
                        yvec2 = rotate_m256(yvec2);
                        zvec2 = rotate_m256(zvec2);
                        mvec2 = rotate_m256(mvec2);
                        b_type diff_x = xvec2 - xvec1;
                        b_type diff_y = yvec2 - yvec1;
                        b_type diff_z = zvec2 - zvec1;
                        b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                        b_type m2_dist3;
                        if constexpr (avx_version && std::is_same_v<F, float>) {
                            m2_dist3 = mvec2 * inv_sqrt_3(dist2);
                        } else {
                            b_type dist = xsimd::sqrt(dist2);
                            b_type dist3 = dist * dist2;
                            m2_dist3 = mvec2 / dist3;
                        }
                        res_x_vec = xsimd::fma(diff_x, m2_dist3, res_x_vec);
                        res_y_vec = xsimd::fma(diff_y, m2_dist3, res_y_vec);
                        res_z_vec = xsimd::fma(diff_z, m2_dist3, res_z_vec);
                    }
                }
                // Do the remaining scalar part.
                for (; i2 < npart; ++i2, ++x_ptr2, ++y_ptr2, ++z_ptr2, ++m_ptr2) {
                    if (i2 != i1) {
                        // Avoid self-interactions.
                        b_type diff_x = *x_ptr2 - xvec1;
                        b_type diff_y = *y_ptr2 - yvec1;
                        b_type diff_z = *z_ptr2 - zvec1;
                        b_type dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                        b_type m2_dist3;
                        if constexpr (avx_version && std::is_same_v<F, float>) {
                            m2_dist3 = *m_ptr2 * inv_sqrt_3(dist2);
                        } else {
                            b_type dist = xsimd::sqrt(dist2);
                            b_type dist3 = dist * dist2;
                            m2_dist3 = *m_ptr2 / dist3;
                        }
                        res_x_vec = xsimd::fma(diff_x, m2_dist3, res_x_vec);
                        res_y_vec = xsimd::fma(diff_y, m2_dist3, res_y_vec);
                        res_z_vec = xsimd::fma(diff_z, m2_dist3, res_z_vec);
                    }
                }
                // Write out the updated accelerations.
                xsimd::store_aligned(res_x, res_x_vec);
                xsimd::store_aligned(res_y, res_y_vec);
                xsimd::store_aligned(res_z, res_z_vec);
            }
            // Do the remaining scalar part.
            for (; i1 < npart; ++i1, ++x_ptr1, ++y_ptr1, ++z_ptr1, ++res_x, ++res_y, ++res_z) {
                auto [x_ptr2, y_ptr2, z_ptr2, m_ptr2] = std::make_tuple(x_ptr, y_ptr, z_ptr, m_ptr);
                const F x1 = *x_ptr1;
                const F y1 = *y_ptr1;
                const F z1 = *z_ptr1;
                for (size_type i2 = 0; i2 < npart; ++i2, ++x_ptr2, ++y_ptr2, ++z_ptr2, ++m_ptr2) {
                    if (i1 != i2) {
                        // Avoid self interactions.
                        F diff_x = *x_ptr2 - x1;
                        F diff_y = *y_ptr2 - y1;
                        F diff_z = *z_ptr2 - z1;
                        F dist2 = diff_x * diff_x + diff_y * diff_y + diff_z * diff_z;
                        F dist = std::sqrt(dist2);
                        F dist3 = dist * dist2;
                        F m2_dist3 = *m_ptr2 / dist3;
                        *res_x += diff_x * m2_dist3;
                        *res_y += diff_y * m2_dist3;
                        *res_z += diff_z * m2_dist3;
                    }
                }
            }
        } else {
            // Shortcuts to the input coordinates.
            std::array<const F *, NDim> c_ptrs;
            for (std::size_t j = 0; j < NDim; ++j) {
                c_ptrs[j] = m_coords[j].data() + node_begin;
            }
            // Temporary vectors to be used in the loops below.
            std::array<F, NDim> diffs, pos1;
            for (size_type i1 = 0; i1 < npart; ++i1) {
                // Load the coords of the current particle.
                for (std::size_t j = 0; j < NDim; ++j) {
                    pos1[j] = c_ptrs[j][i1];
                }
                // Load the mass of the current particle.
                const auto m1 = m_ptr[i1];
                // The acceleration vector on the current particle
                // (inited to zero).
                std::array<F, NDim> a1{};
                for (size_type i2 = i1 + 1u; i2 < npart; ++i2) {
                    // Determine dist2, dist and dist3.
                    F dist2(0);
                    for (std::size_t j = 0; j < NDim; ++j) {
                        diffs[j] = c_ptrs[j][i2] - pos1[j];
                        dist2 += diffs[j] * diffs[j];
                    }
                    const F dist = std::sqrt(dist2);
                    const F dist3 = dist2 * dist;
                    // Divide both masses by dist3.
                    const F m2_dist3 = m_ptr[i2] / dist3;
                    const F m1_dist3 = m1 / dist3;
                    // Accumulate the accelerations, both in the local
                    // accumulator for the current particle and in the global
                    // acc vector for the opposite acceleration.
                    for (std::size_t j = 0; j < NDim; ++j) {
                        a1[j] += m2_dist3 * diffs[j];
                        tmp_res[j][i2] -= m1_dist3 * diffs[j];
                    }
                }
                // Update the acceleration on the first particle
                // in the temporary storage.
                for (std::size_t j = 0; j < NDim; ++j) {
                    tmp_res[j][i1] += a1[j];
                }
            }
        }
    }
    // Compute the total acceleration on the particles in a target node. node_begin is the starting index
    // of the target in the particles' arrays, npart the number of particles in the node. [sib_begin, sib_end)
    // is the index range, in the tree structure, encompassing the target, its parent and its parent's siblings at the
    // tree level CurLevel. NodeLevel is the level of the node itself. nodal_code is the nodal code of the
    // target.
    template <unsigned CurLevel, unsigned NodeLevel>
    void vec_acc_on_node(const F &theta2, size_type node_begin, size_type npart, UInt nodal_code, size_type sib_begin,
                         size_type sib_end) const
    {
        // We proceed breadth-first examining all the siblings of the target's parent
        // (or of the node itself, at the last iteration) at the current level.
        //
        // Compute the shifted code. This is the nodal code of the target's parent
        // at the current level (or the nodal code of the target itself at the last
        // iteration).
        const auto s_code = nodal_code >> ((NodeLevel - CurLevel) * NDim);
        auto new_sib_begin = sib_end;
        // Determine the size of the target at the current level.
        const F node_size = m_box_size / (UInt(1) << CurLevel);
        const F node_size2 = node_size * node_size;
        for (auto idx = sib_begin; idx != sib_end; idx += get<1>(m_tree[idx])[2] + 1u) {
            if (get<0>(m_tree[idx]) == s_code) {
                // We are in the target's parent, or the target itself.
                if (CurLevel == NodeLevel) {
                    // Last iteration, we are in the target itself. Compute the
                    // self-interactions within the target.
                    vec_node_self_interactions(node_begin, npart);
                } else {
                    // We identified the parent of the target at the current
                    // level. Store its starting index for later.
                    new_sib_begin = idx;
                }
            } else {
                // Compute the accelerations from the current sibling.
                vec_acc_from_node<CurLevel, CurLevel>(theta2, node_begin, npart, idx, idx + 1u + get<1>(m_tree[idx])[2],
                                                      node_size2);
            }
        }
        if (CurLevel != NodeLevel) {
            // If we are not at the last iteration, we must have changed
            // new_sib_begin in the loop above.
            assert(new_sib_begin != sib_end);
        } else {
            ignore_args(new_sib_begin);
        }
        if constexpr (CurLevel < NodeLevel) {
            // We are not at the level of the target yet. Recurse down.
            vec_acc_on_node<CurLevel + 1u, NodeLevel>(theta2, node_begin, npart, nodal_code, new_sib_begin + 1u,
                                                      new_sib_begin + 1u + get<1>(m_tree[new_sib_begin])[2]);
        }
    }
    // Top level function fo the vectorised computation of the accelerations. This function does
    // a depth-first tree traversal until it finds a target node with a number of particles npart <= m_ncrit.
    // When it finds one, it will compute the accelerations on the target's particles by all the other
    // particles in the domain. When that is done, it will continue the traversal (except that all the
    // children of the target node will be skipped) and repeat the procedure for the next target node.
    template <unsigned Level>
    void vec_accs_impl(std::vector<F> &out, const F &theta2, size_type begin, size_type end) const
    {
        if constexpr (Level <= cbits) {
            for (auto idx = begin; idx != end;
                 // NOTE: when incrementing idx, we need to add 1 to the
                 // total number of children in order to point to the next sibling.
                 idx += get<1>(m_tree[idx])[2] + 1u) {
                const auto [node_begin, node_end, n_children] = get<1>(m_tree[idx]);
                const auto npart = node_end - node_begin;
                if (Level == cbits || npart <= m_ncrit || !n_children) {
                    // Either:
                    // - this is the last possible recursion level, or
                    // - the number of particles is low enough, or
                    // - this is a leaf node.
                    // Then, proceed to the vectorised calculation of the accelerations
                    // on the particles belonging to this target node.
                    //
                    // Prepare the temporary vectors containing the result.
                    auto &tmp_res = vec_acc_tmp_res();
                    for (auto &v : tmp_res) {
                        // Resize and fill with zeroes.
                        v.resize(boost::numeric_cast<decltype(v.size())>(npart));
                        std::fill(v.begin(), v.end(), F(0));
                    }
                    // Do the computation.
                    vec_acc_on_node<0, Level>(theta2, node_begin, npart, get<0>(m_tree[idx]), size_type(0),
                                              size_type(m_tree.size()));
                    // Write out the result.
                    auto out_ptr = out.data() + node_begin * NDim;
                    for (size_type i = 0; i < npart; ++i, out_ptr += NDim) {
                        for (std::size_t j = 0; j != NDim; ++j) {
                            out_ptr[j] = tmp_res[j][i];
                        }
                    }
                } else {
                    // We are not at the last recursion level, npart > m_ncrit and
                    // the node has at least one children. Go deeper.
                    vec_accs_impl<Level + 1u>(out, theta2, idx + 1u, idx + 1u + n_children);
                }
            }
        } else {
            ignore_args(begin, end);
            // NOTE: we can never end up here, as we have reached the maximum tree level
            // and we are prevented from reaching this point at runtime by the Level == cbits
            // check above.
            assert(false);
        }
    }

public:
    void vec_accs(std::vector<F> &out, const F &theta) const
    {
        simple_timer st("vector acc computation");
        const auto theta2 = theta * theta;
        // Prepare out.
        out.resize(m_masses.size() * NDim);
        std::fill(out.begin(), out.end(), F(0));
        vec_accs_impl<0>(out, theta2, size_type(0), size_type(m_tree.size()));
    }
    std::array<F, NDim> exact_accs(size_type idx) const
    {
        simple_timer st("exact acc computation");
        const auto size = m_masses.size();
        std::array<F, NDim> retval{};
        for (size_type i = 0; i < size; ++i) {
            if (i == idx) {
                continue;
            }
            F dist2(0);
            for (std::size_t j = 0; j < NDim; ++j) {
                dist2 += (m_coords[j][i] - m_coords[j][idx]) * (m_coords[j][i] - m_coords[j][idx]);
            }
            const auto dist = std::sqrt(dist2);
            const auto dist3 = dist * dist2;
            for (std::size_t j = 0; j < NDim; ++j) {
                retval[j] += (m_coords[j][i] - m_coords[j][idx]) * m_masses[i] / dist3;
            }
        }
        return retval;
    }

private:
    F m_box_size;
    size_type m_max_leaf_n;
    // ncrit: if the number of particles in a node is ncrit or less,
    // then we will compute the accelerations on the particles in that
    // node in a vectorised fashion.
    size_type m_ncrit;
    v_type<F> m_masses;
    std::array<v_type<F>, NDim> m_coords;
    v_type<UInt> m_codes;
    v_type<std::tuple<UInt, std::array<size_type, 3>, F, std::array<F, NDim>>> m_tree;
};

#include <random>

#include <boost/math/constants/constants.hpp>

static constexpr unsigned nparts = 1'000'000;
static constexpr float bsize = 10.f;

static std::mt19937 rng;

template <std::size_t D, typename F>
inline std::vector<F> get_uniform_particles(std::size_t n, F size)
{
    std::vector<F> retval(n * (D + 1u));
    // Mass.
    std::uniform_real_distribution<F> mdist(F(0), F(1));
    std::generate(retval.begin(), retval.begin() + n, [&mdist]() { return mdist(rng); });
    // Positions.
    std::uniform_real_distribution<F> rdist(-size / F(2) /*+ size * F(0.01)*/, size / F(2) /*- size * F(0.01)*/);
    std::generate(retval.begin() + n, retval.end(), [&rdist]() { return rdist(rng); });
    return retval;
}

// See: http://www.artcompsci.org/kali/vol/plummer/ch03.html
template <typename F>
inline std::vector<F> get_plummer_sphere(std::size_t n, F size)
{
    std::vector<F> retval(n * 4u);
    // Uniform [0, 1) dist.
    std::uniform_real_distribution<F> udist(F(0), F(1));
    // Uniform [0.1, 1.9) dist.
    std::uniform_real_distribution<F> mdist(F(0.1), F(1.9));
    // Average mass of 1.
    std::generate(retval.begin(), retval.begin() + n, [&mdist]() { return mdist(rng); });
    for (std::size_t i = 0; i < n;) {
        // Generate a random radius.
        const F r = F(1) / std::sqrt(std::pow(udist(rng), F(-2) / F(3)) - F(1));
        // Generate random u, v for sphere picking.
        const F u = udist(rng), v = udist(rng);
        // Longitude.
        const F lon
            = std::clamp(F(2) * boost::math::constants::pi<F>() * u, F(0), F(2) * boost::math::constants::pi<F>());
        // Colatitude.
        const F colat = std::acos(std::clamp(F(2) * v - F(1), F(-1), F(1)));
        // Compute x, y, z.
        const F x = r * std::cos(lon) * std::sin(colat);
        const F y = r * std::sin(lon) * std::sin(colat);
        const F z = r * std::cos(colat);
        if (x >= -size / F(2) && x < size / F(2) && y >= -size / F(2) && y < size / F(2) && z >= -size / F(2)
            && z < size / F(2)) {
            // Assign coordinates only if we fall into the domain. Otherwise, try again.
            *(retval.begin() + n + i) = x;
            *(retval.begin() + 2 * n + i) = y;
            *(retval.begin() + 3 * n + i) = z;
            ++i;
        }
    }
    return retval;
}

#include <boost/lexical_cast.hpp>

int main(int argc, char **argv)
{
    if (argc < 2) {
        throw std::runtime_error("Need at least 4 arguments, but only " + std::to_string(argc) + " was/were provided");
    }
    const auto idx = boost::lexical_cast<std::size_t>(argv[1]);
    const auto max_leaf_n = boost::lexical_cast<std::size_t>(argv[2]);
    const auto ncrit = boost::lexical_cast<std::size_t>(argv[3]);
    // auto parts = get_uniform_particles<3>(nparts, bsize);
    auto parts = get_plummer_sphere(nparts, bsize);
    tree<std::uint64_t, float, 3> t(bsize, parts.begin(),
                                    {parts.begin() + nparts, parts.begin() + 2 * nparts, parts.begin() + 3 * nparts},
                                    nparts, max_leaf_n, ncrit);
    std::cout << t << '\n';
    std::vector<float> accs(nparts * 3);
    // t.scalar_accs(accs.begin(), 0.75f);
    // std::cout << accs[idx * 3] << ", " << accs[idx * 3 + 1] << ", " << accs[idx * 3 + 2] << '\n';
    t.vec_accs(accs, 0.75f);
    std::cout << accs[idx * 3] << ", " << accs[idx * 3 + 1] << ", " << accs[idx * 3 + 2] << '\n';
    auto eacc = t.exact_accs(idx);
    std::cout << eacc[0] << ", " << eacc[1] << ", " << eacc[2] << '\n';
}
