// Copyright 2018 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the rakau library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <rakau/tree.hpp>

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <random>
#include <vector>

#include <boost/iterator/zip_iterator.hpp>
#include <boost/tuple/tuple.hpp>

#include <rakau/config.hpp>

#include "test_utils.hpp"

using namespace rakau;
using namespace rakau::kwargs;
using namespace rakau_test;

using fp_types = std::tuple<float, double>;

static std::mt19937 rng(0);

static const std::vector<double> sp =
#if defined(RAKAU_WITH_ROCM) || defined(RAKAU_WITH_CUDA)
    {0.5, 0.5}
#else
    {}
#endif
;

TEST_CASE("g constant accelerations/potentials")
{
    tuple_for_each(fp_types{}, [](auto x) {
        using fp_type = decltype(x);
        constexpr auto bsize = static_cast<fp_type>(1);
        constexpr auto s = 10000u;
        constexpr auto theta = fp_type(0.75);
        auto parts = get_uniform_particles<3>(s, bsize, rng);
        octree<fp_type> t({parts.begin() + s, parts.begin() + 2u * s, parts.begin() + 3u * s, parts.begin()}, s,
                          box_size = bsize);
        std::array<std::vector<fp_type>, 4> accpots;
        t.accs_pots_u(accpots, theta, kwargs::split = sp);
        auto accs_pots_u_orig(accpots);
        t.accs_pots_o(accpots, theta);
        auto accs_pots_o_orig(accpots);
        t.accs_pots_u(accpots, theta, kwargs::G = fp_type(0), kwargs::split = sp);
        REQUIRE(std::all_of(accpots[0].begin(), accpots[0].end(), [](fp_type x) { return x == fp_type(0); }));
        REQUIRE(std::all_of(accpots[1].begin(), accpots[1].end(), [](fp_type x) { return x == fp_type(0); }));
        REQUIRE(std::all_of(accpots[2].begin(), accpots[2].end(), [](fp_type x) { return x == fp_type(0); }));
        REQUIRE(std::all_of(accpots[3].begin(), accpots[3].end(), [](fp_type x) { return x == fp_type(0); }));
        t.accs_pots_u(accpots, theta, kwargs::G = fp_type(2), kwargs::split = sp);
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[0].begin(), accs_pots_u_orig[0].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[0].end(), accs_pots_u_orig[0].end())),
                        [](auto t) { return boost::get<0>(t) == fp_type(2) * boost::get<1>(t); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[1].begin(), accs_pots_u_orig[1].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[1].end(), accs_pots_u_orig[1].end())),
                        [](auto t) { return boost::get<0>(t) == fp_type(2) * boost::get<1>(t); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[2].begin(), accs_pots_u_orig[2].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[2].end(), accs_pots_u_orig[2].end())),
                        [](auto t) { return boost::get<0>(t) == fp_type(2) * boost::get<1>(t); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[3].begin(), accs_pots_u_orig[3].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[3].end(), accs_pots_u_orig[3].end())),
                        [](auto t) { return boost::get<0>(t) == fp_type(2) * boost::get<1>(t); }));
        t.accs_pots_o(accpots, theta, kwargs::G = fp_type(1) / fp_type(2));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[0].begin(), accs_pots_o_orig[0].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[0].end(), accs_pots_o_orig[0].end())),
                        [](auto t) { return boost::get<0>(t) == boost::get<1>(t) / fp_type(2); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[1].begin(), accs_pots_o_orig[1].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[1].end(), accs_pots_o_orig[1].end())),
                        [](auto t) { return boost::get<0>(t) == boost::get<1>(t) / fp_type(2); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[2].begin(), accs_pots_o_orig[2].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[2].end(), accs_pots_o_orig[2].end())),
                        [](auto t) { return boost::get<0>(t) == boost::get<1>(t) / fp_type(2); }));
        REQUIRE(
            std::all_of(boost::make_zip_iterator(boost::make_tuple(accpots[3].begin(), accs_pots_o_orig[3].begin())),
                        boost::make_zip_iterator(boost::make_tuple(accpots[3].end(), accs_pots_o_orig[3].end())),
                        [](auto t) { return boost::get<0>(t) == boost::get<1>(t) / fp_type(2); }));
        // Check the exact accs/pots as well.
        const auto eacc_u_orig = t.exact_acc_u(42);
        const auto epot_u_orig = t.exact_pot_u(42);
        const auto eacc_o_orig = t.exact_acc_o(42);
        const auto epot_o_orig = t.exact_pot_o(42);
        const auto eacc_u = t.exact_acc_u(42, kwargs::G = fp_type(2));
        const auto epot_u = t.exact_pot_u(42, kwargs::G = fp_type(2));
        const auto eacc_o = t.exact_acc_o(42, kwargs::G = fp_type(1) / fp_type(2));
        const auto epot_o = t.exact_pot_o(42, kwargs::G = fp_type(1) / fp_type(2));
        REQUIRE(eacc_u_orig[0] == eacc_u[0] / fp_type(2));
        REQUIRE(eacc_u_orig[1] == eacc_u[1] / fp_type(2));
        REQUIRE(eacc_u_orig[2] == eacc_u[2] / fp_type(2));
        REQUIRE(epot_u_orig == epot_u / fp_type(2));
        REQUIRE(eacc_o_orig[0] == eacc_o[0] * fp_type(2));
        REQUIRE(eacc_o_orig[1] == eacc_o[1] * fp_type(2));
        REQUIRE(eacc_o_orig[2] == eacc_o[2] * fp_type(2));
        REQUIRE(epot_o_orig == epot_o * fp_type(2));
    });
}
