#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/sort/spreadsort/spreadsort.hpp>

#include "libmorton/morton.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>

template <std::size_t NDim, typename Out>
struct morton_encoder {
};

template <>
struct morton_encoder<3u, std::uint64_t> {
    std::uint64_t operator()(std::uint64_t x, std::uint64_t y, std::uint64_t z) const
    {
        assert(x < (1ul << 21));
        assert(y < (1ul << 21));
        assert(z < (1ul << 21));
        assert(!(::m3D_e_sLUT<std::uint64_t, std::uint32_t>(x, y, z) >> 63u));
        assert((::morton3D_64_encode(x, y, z) == ::m3D_e_sLUT<std::uint64_t, std::uint32_t>(x, y, z)));
        return ::m3D_e_sLUT<std::uint64_t, std::uint32_t>(x, y, z);
    }
};

template <>
struct morton_encoder<2u, std::uint64_t> {
    std::uint64_t operator()(std::uint64_t x, std::uint64_t y) const
    {
        // TODO fix.
        assert(x < (1ul << 31));
        assert(y < (1ul << 31));
        assert(!(::morton2D_64_encode(x, y) >> 62u));
        return ::morton2D_64_encode(x, y);
    }
};

template <unsigned Start, unsigned End, typename UInt>
inline UInt extract_bits(UInt n)
{
    static_assert(std::conjunction_v<std::is_integral<UInt>, std::is_unsigned<UInt>>,
                  "UInt must be an unsigned integral type.");
    static_assert(End > Start, "End must be greater than Start.");
    static_assert(End <= unsigned(std::numeric_limits<UInt>::digits), "End must not be greater than the bit width.");
    if constexpr (End - Start == unsigned(std::numeric_limits<UInt>::digits)) {
        return n;
    } else {
        constexpr auto mask = static_cast<UInt>(((UInt(1) << (End - Start)) - 1u) << Start);
        return static_cast<UInt>((n & mask) >> Start);
    }
}

template <typename UInt, std::size_t NDim>
constexpr unsigned get_cbits()
{
    constexpr unsigned nbits = std::numeric_limits<UInt>::digits;
    static_assert(nbits > NDim, "The number of bits must be greater than the number of dimensions.");
    return static_cast<unsigned>(nbits / NDim - !(nbits % NDim));
}

template <typename F, typename PIt, typename MIt, typename... CIts>
inline void get_particle_codes(F size, std::size_t nparts, PIt p_it, MIt m_it, CIts... c_its)
{
    using code_t = typename std::iterator_traits<PIt>::value_type;
    constexpr std::size_t ndim = sizeof...(CIts);
    static_assert(get_cbits<code_t, ndim>() < std::numeric_limits<code_t>::digits);
    constexpr code_t factor = code_t(1) << get_cbits<code_t, ndim>();
    // Make a copy of the original mass/coord iterators.
    auto its_orig = std::make_tuple(m_it, c_its...);
    // Function to discretise the input floating-point coordinate x
    // into a box of a given size.
    auto disc_coord = [size](const auto &x) {
        // Translate and rescale the coordinate so that -size/2 becomes zero
        // and size/2 becomes 1.
        auto tmp = (x + size / F(2)) / size;
        // Rescale by factor.
        tmp *= factor;
        // Check: don't end up with a nonfinite value.
        if (!std::isfinite(tmp)) {
            throw std::invalid_argument("Not finite!");
        }
        // Check: don't end up outside the [0, factor[ range.
        if (tmp < F(0) || tmp >= F(factor)) {
            throw std::invalid_argument("Out of bounds!");
        }
        // Cast to code_t.
        auto retval = static_cast<code_t>(tmp);
        // Last check, make sure we don't overflow.
        if (retval >= factor) {
            throw std::invalid_argument("Out of bounds! (after cast)");
        }
        return retval;
    };
    // Do the encoding. We will store code, mass and coordinates
    // in a temporary structure which we will use for sorting.
    static thread_local std::vector<std::pair<code_t, decltype(std::make_tuple(*m_it, *c_its...))>> tmp;
    tmp.resize(nparts);
    // The encoder object.
    morton_encoder<ndim, code_t> me;
    for (auto &p : tmp) {
        // Compute the code.
        p.first = me(disc_coord(*c_its)...);
        // Store the mass and the real coordinates.
        p.second = std::tie(*m_it, *c_its...);
        // Increase the other iterators.
        ++m_it, (..., ++c_its);
    }
    // Now let's sort.
    boost::sort::spreadsort::integer_sort(
        tmp.begin(), tmp.end(), [](const auto &p, unsigned offset) { return static_cast<code_t>(p.first >> offset); },
        [](const auto &p1, const auto &p2) { return p1.first < p2.first; });
    // Reset the original iterators.
    std::tie(m_it, c_its...) = its_orig;
    // Copy over the data.
    for (const auto &p : tmp) {
        *p_it = p.first;
        std::tie(*m_it, *c_its...) = p.second;
        // Increase the iterators.
        ++p_it, ++m_it, (..., ++c_its);
    }
}

template <unsigned ParentLevel, std::size_t NDim>
struct l_comparer {
    template <typename UInt>
    bool operator()(UInt code, UInt b) const
    {
        static_assert(NDim < std::numeric_limits<UInt>::digits);
        assert(b <= (UInt(1) << NDim));
        constexpr auto cbits = get_cbits<UInt, NDim>();
        static_assert(cbits > ParentLevel);
        static_assert((cbits - ParentLevel) / NDim <= std::numeric_limits<unsigned>::max());
        return extract_bits<(NDim * (cbits - ParentLevel - 1u)), (NDim * (cbits - ParentLevel))>(code) < b;
    }
};

template <unsigned ParentLevel, std::size_t NDim, typename UInt,
          typename = std::integral_constant<bool, (get_cbits<UInt, NDim>() == ParentLevel)>>
struct tree_builder {
    template <typename Tree, typename PIt, typename MIt, typename... CIts>
    void operator()(Tree &tree, UInt parent_code, PIt cbegin, PIt cend, MIt m_it, CIts... c_its) const
    {
        static_assert(NDim == sizeof...(CIts));
        using code_t = typename std::iterator_traits<PIt>::value_type;
        using mass_t = typename std::iterator_traits<MIt>::value_type;
        static_assert(std::is_same_v<code_t, UInt>);
        static_assert(NDim < std::numeric_limits<code_t>::digits);
        l_comparer<ParentLevel, NDim> lc;
        for (code_t i = 0; i < (code_t(1) << NDim); ++i) {
            assert(cbegin == std::lower_bound(cbegin, cend, i, lc));
            auto it_end = std::lower_bound(cbegin, cend, static_cast<code_t>(i + 1u), lc);
            const auto npart = std::distance(cbegin, it_end);
            if (npart) {
                const auto cur_code = static_cast<code_t>((parent_code << NDim) + i);
                const auto M = std::accumulate(m_it, m_it + npart, mass_t(0));
                tree.emplace_back(
                    cur_code, M,
                    std::inner_product(m_it, m_it + npart, c_its, static_cast<decltype(*c_its * mass_t(0))>(0)) / M...);
                if (npart > 1) {
                    tree_builder<ParentLevel + 1u, NDim, UInt>{}(tree, cur_code, cbegin, it_end, m_it, c_its...);
                }
            }
            std::advance(cbegin, npart);
            std::advance(m_it, npart);
            (..., std::advance(c_its, npart));
        }
    }
};

template <unsigned ParentLevel, std::size_t NDim, typename UInt>
struct tree_builder<ParentLevel, NDim, UInt, std::true_type> {
    template <typename Tree, typename PIt, typename MIt, typename... CIts>
    void operator()(Tree &, UInt, PIt, PIt, MIt, CIts...) const
    {
    }
};

template <typename Tree, typename PIt, typename MIt, typename... CIts>
inline void build_tree(Tree &tree, std::size_t nparts, PIt p_it, MIt m_it, CIts... c_its)
{
    using code_t = typename std::iterator_traits<PIt>::value_type;
    tree_builder<0, sizeof...(CIts), code_t>{}(tree, code_t(1), p_it, p_it + nparts, m_it, c_its...);
}

static std::mt19937 rng;

template <std::size_t D, typename F>
inline std::vector<F> get_uniform_particles(std::size_t n, F size)
{
    std::vector<F> retval(n * (D + 1u));
    // Mass.
    std::uniform_real_distribution<F> mdist(F(0), F(1));
    std::generate(retval.begin(), retval.begin() + n, [&mdist]() { return mdist(rng); });
    // Positions.
    std::uniform_real_distribution<F> rdist(-size / F(2), size / F(2));
    std::generate(retval.begin() + n, retval.end(), [&rdist]() { return rdist(rng); });
    return retval;
}

static constexpr unsigned nparts = 600'000;
static constexpr double bsize = 10.;

template <std::size_t NDim>
struct node_comparer {
    template <typename UInt>
    bool operator()(UInt n1, UInt n2) const
    {
        constexpr unsigned cbits = get_cbits<UInt, NDim>();
        constexpr unsigned ndigits = std::numeric_limits<UInt>::digits;
        // TODO clzl, assert nonzero.
        assert(!((ndigits - 1u - unsigned(__builtin_clzl(n1))) % NDim));
        assert(!((ndigits - 1u - unsigned(__builtin_clzl(n2))) % NDim));
        const auto tl1 = static_cast<unsigned>((ndigits - 1u - unsigned(__builtin_clzl(n1))) / NDim);
        const auto tl2 = static_cast<unsigned>((ndigits - 1u - unsigned(__builtin_clzl(n2))) / NDim);
        assert(cbits >= tl1);
        assert(cbits >= tl2);
        assert((cbits - tl1) * NDim < ndigits);
        assert((cbits - tl2) * NDim < ndigits);
        const auto s_n1 = n1 << ((cbits - tl1) * NDim);
        const auto s_n2 = n2 << ((cbits - tl2) * NDim);
        return s_n1 < s_n2 || (s_n1 == s_n2 && tl1 < tl2);
    }
};

int main()
{
    std::cout.precision(40);
    auto parts = get_uniform_particles<3>(nparts, bsize);
    std::vector<std::uint64_t> codes(nparts);
    get_particle_codes(bsize, nparts, codes.begin(), parts.begin(), parts.begin() + nparts, parts.begin() + 2u * nparts,
                       parts.begin() + 3u * nparts);
    std::cout << "Done sorting.\n\n";
    std::cout.flush();
    std::cout << "Code: " << std::bitset<64>(codes.back()) << '\n';
    std::cout << "mass: " << *(parts.begin() + nparts - 1) << '\n';
    std::cout << "x coord: " << *(parts.begin() + 2 * nparts - 1) << '\n';
    std::cout << "y coord: " << *(parts.begin() + 3 * nparts - 1) << '\n';
    std::cout << "z coord: " << *(parts.begin() + 4 * nparts - 1) << '\n';
    std::cout.flush();
    std::vector<std::tuple<std::uint64_t, double, double, double, double>> tree;
    build_tree(tree, nparts, codes.cbegin(), parts.cbegin(), parts.cbegin() + nparts, parts.cbegin() + 2u * nparts,
               parts.cbegin() + 3u * nparts);
    std::cout << "Tree size: " << tree.size() << '\n';
    std::cout << "First few: \n";
    for (auto it = tree.cbegin(); it != tree.cbegin() + 10; ++it) {
        std::cout << std::bitset<64>(std::get<0>(*it)) << '\n';
    }
    std::cout << "....\n\n";
    std::cout << "Tree sorted: " << std::is_sorted(tree.begin(), tree.end(), [](const auto &n1, const auto &n2) {
        return node_comparer<3>{}(std::get<0>(n1), std::get<0>(n2));
    }) << '\n';

    // Manual.
    // Let's pick a "random" particle.
    const std::size_t pidx = nparts / 2;
    const auto code = std::get<0>(tree[pidx]);
    std::cout << "Code:" << std::bitset<64>(code) << '\n';
    auto get_tl = [](auto c) {
        return (unsigned(std::numeric_limits<std::uint64_t>::digits) - 1u - unsigned(__builtin_clzl(c))) / 3u;
    };
    const auto tl = get_tl(code);
    std::cout << "Tree level: " << tl << '\n';
    const auto f_oct = ((code >> ((tl - 1u) * 3u))) & 7u;
    std::cout << "First octant: " << std::bitset<3>(f_oct) << '\n';
    auto begin = tree.begin();
    auto end = tree.end();
    for (auto i = 1u; i < 8u; ++i) {
        const auto bab = ((f_oct + i) % 8u);
        auto it = std::lower_bound(begin, end, bab, [get_tl](const auto &t, const auto b) {
            const auto c = std::get<0>(t);
            const auto tl = get_tl(c);
            return ((c >> ((tl - 1u) * 3u)) & 7u) < b;
        });
        std::cout << std::bitset<64>(std::get<0>(*it)) << '\n';
        std::cout << std::get<1>(*it) << '\n';
    }
}
