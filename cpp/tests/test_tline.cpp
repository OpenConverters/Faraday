#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <faraday/Tline.hpp>

using Catch::Approx;
namespace tl = faraday::tline;

TEST_CASE("microstrip: air line w/h=1 pins the Hammerstad closed form", "[tline]") {
    // eps_r = 1 -> eps_eff = 1, Z0 = 60 ln(8 + 0.25) = 126.61 ohm (textbook value)
    CHECK(tl::microstrip_eps_eff(1.0, 1.0, 1.0) == Approx(1.0).margin(1e-9));
    CHECK(tl::microstrip_z0(1.0, 1.0, 1.0) == Approx(126.61).margin(0.05));
}

TEST_CASE("microstrip: the classic ~50-ohm FR4 geometry", "[tline]") {
    // w = 3.0 mm on h = 1.6 mm FR4 (eps_r 4.5): the well-known w ~ 2h rule of
    // thumb for 50 ohm. Hammerstad gives 50.3 ohm here.
    double z0 = tl::microstrip_z0(3.0, 1.6, 4.5);
    CHECK(z0 == Approx(50.3).margin(1.0));
    // monotonic sanity: wider -> lower Z0, higher eps -> lower Z0
    CHECK(tl::microstrip_z0(4.0, 1.6, 4.5) < z0);
    CHECK(tl::microstrip_z0(3.0, 1.6, 4.8) < z0);
}

TEST_CASE("stripline: IPC-2141 form, monotonic and in the physical range", "[tline]") {
    // 0.5 mm trace centred in a 1.0 mm FR4 gap: formula value 41.7 ohm.
    double z0 = tl::stripline_z0(0.5, 1.0, 0.035, 4.5);
    CHECK(z0 == Approx(41.7).margin(0.5));
    CHECK(tl::stripline_z0(0.3, 1.0, 0.035, 4.5) > z0);   // narrower -> higher
    CHECK(tl::stripline_z0(0.5, 1.2, 0.035, 4.5) > z0);   // bigger gap -> higher
    // stripline of the same w/h is always lower-Z than microstrip (fully
    // embedded field) — cross-formula physical consistency
    CHECK(z0 < tl::microstrip_z0(0.5, 0.5, 4.5));
    CHECK_THROWS(tl::stripline_z0(3.0, 1.0, 0.035, 4.5));  // out of validity range
}

TEST_CASE("coupling: Johnson & Graham screening estimate", "[tline]") {
    // d = 0 hits the saturated bound; d = h halves it; d = 4h is ~ -36.6 dB
    CHECK(tl::next_sat_edge(0.0, 1.0) == Approx(0.25));
    CHECK(tl::next_sat_edge(1.0, 1.0) == Approx(0.125));
    CHECK(tl::to_db(tl::next_sat_edge(4.0, 1.0)) == Approx(-36.6).margin(0.1));
    CHECK(tl::to_db(0.25) == Approx(-12.04).margin(0.01));
    // broadside with zero lateral offset also saturates
    CHECK(tl::next_sat_broadside(0.0, 0.2) == Approx(0.25));
    CHECK_THROWS(tl::next_sat_edge(1.0, 0.0));
    CHECK_THROWS(tl::to_db(0.0));
}
