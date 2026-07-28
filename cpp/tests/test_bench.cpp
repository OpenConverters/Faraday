// The bench end to end: the JSON contract the browser depends on, the noise
// verdict, and the auto-fix. Also the speed budget — this runs while a slider
// is moving, so a regression that makes it slow is a regression that breaks
// the feature, and a test is the only thing that will notice.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <faraday/Bench.hpp>

#include <chrono>

using namespace faraday;
using Catch::Matchers::WithinRel;

namespace {

nlohmann::json base_request() {
    return nlohmann::json{{"mode", "microstrip"}, {"w1Mm", 0.25}, {"w2Mm", 0.25},
                          {"gapMm", 0.2},        {"hMm", 0.2},   {"tMm", 0.035},
                          {"epsR", 4.3},         {"lengthMm", 60.0},
                          {"riseNs", 0.5},       {"amplitudeV", 3.3},
                          {"family", "lvcmos33"}};
}

}  // namespace

TEST_CASE("a bench run returns every section the UI reads", "[bench]") {
    const nlohmann::json out = bench::run(bench::request_from_json(base_request()));
    REQUIRE_FALSE(out.contains("error"));

    for (const char* k : {"geometry", "rlgc", "spice", "verdict", "field", "timingMs"}) {
        INFO("section " << k);
        REQUIRE(out.contains(k));
    }

    CHECK(out["rlgc"]["z0"].get<double>() > 20.0);
    CHECK(out["rlgc"]["z0"].get<double>() < 200.0);
    // effective permittivity of a microstrip lies between air and the laminate
    const double ee = out["rlgc"]["epsEff"].get<double>();
    CHECK(ee > 1.0);
    CHECK(ee < 4.3);

    const auto& sp = out["spice"];
    CHECK(sp["t"].size() == sp["vicNear"].size());
    CHECK(sp["t"].size() == sp["aggFar"].size());
    CHECK(sp["t"].size() > 100);
    CHECK(sp["delayNs"].get<double>() > 0.0);

    const auto& f = out["field"];
    const int nx = f["nx"].get<int>(), ny = f["ny"].get<int>();
    CHECK(nx * ny > 0);
    // base64 of one byte per cell, padded to a multiple of four
    CHECK(f["v"].get<std::string>().size() == (size_t)((nx * ny + 2) / 3 * 4));
}

TEST_CASE("the verdict is measured against the receiver, not against a dB figure",
          "[bench]") {
    nlohmann::json req = base_request();
    req["gapMm"] = 0.1;                  // tight: should consume real budget
    const nlohmann::json tight = bench::run(bench::request_from_json(req));
    req["gapMm"] = 1.5;                  // generous
    const nlohmann::json loose = bench::run(bench::request_from_json(req));

    CHECK(tight["verdict"]["peakMv"].get<double>() >
          loose["verdict"]["peakMv"].get<double>());
    CHECK_THAT(tight["verdict"]["budgetV"].get<double>(), WithinRel(0.8, 1e-9));

    // the percentage and the level must agree with each other
    for (const auto& r : {tight, loose}) {
        const double pct = r["verdict"]["pctOfBudget"].get<double>();
        const std::string level = r["verdict"]["level"].get<std::string>();
        if (pct >= 50) CHECK(level == "fail");
        else if (pct >= 25) CHECK(level == "watch");
        else CHECK(level == "ok");
    }
}

TEST_CASE("a lower-voltage family has less room for the same noise", "[bench]") {
    nlohmann::json req = base_request();
    req["gapMm"] = 0.12;
    req["family"] = "lvcmos33";
    const double pct33 = bench::run(bench::request_from_json(req))["verdict"]
                             ["pctOfBudget"].get<double>();
    req["family"] = "lvcmos12";
    const double pct12 = bench::run(bench::request_from_json(req))["verdict"]
                             ["pctOfBudget"].get<double>();
    CHECK(pct12 > pct33);
}

TEST_CASE("the suggested gap actually meets the budget it promises", "[bench]") {
    nlohmann::json req = base_request();
    req["gapMm"] = 0.08;
    req["lengthMm"] = 120.0;
    const nlohmann::json out = bench::run(bench::request_from_json(req));
    REQUIRE(out["verdict"]["pctOfBudget"].get<double>() >= 25.0);
    REQUIRE(out["fix"].is_object());

    const double gap = out["fix"]["gapMm"].get<double>();
    CHECK(gap > req["gapMm"].get<double>());
    CHECK(out["fix"]["pctAfter"].get<double>() < 27.0);

    // re-run at the suggested gap: the promise has to survive an independent
    // solve, not just the bisection's own bookkeeping
    req["gapMm"] = gap;
    req["fix"] = false;
    const nlohmann::json again = bench::run(bench::request_from_json(req));
    CHECK_THAT(again["verdict"]["peakMv"].get<double>(),
               WithinRel(out["fix"]["peakMvAfter"].get<double>(), 0.02));
}

TEST_CASE("stripline and broadside sections both solve", "[bench]") {
    nlohmann::json req = base_request();
    req["mode"] = "stripline";
    req["bMm"] = 0.7;
    const nlohmann::json sl = bench::run(bench::request_from_json(req));
    REQUIRE_FALSE(sl.contains("error"));
    // buried between planes, the whole field is in the laminate
    CHECK_THAT(sl["rlgc"]["epsEff"].get<double>(), WithinRel(4.3, 0.02));

    req = base_request();
    req["mode"] = "broadside";
    req["hvMm"] = 0.2;
    const nlohmann::json bs = bench::run(bench::request_from_json(req));
    REQUIRE_FALSE(bs.contains("error"));
    CHECK(bs["rlgc"]["kb"].get<double>() > 0.0);
}

TEST_CASE("missing inputs are refused by name", "[bench]") {
    nlohmann::json req = base_request();
    req.erase("epsR");
    CHECK_THROWS_AS(bench::request_from_json(req), std::invalid_argument);

    req = base_request();
    req["family"] = "ttl-74series";
    CHECK_THROWS_AS(bench::run(bench::request_from_json(req)), std::invalid_argument);
}

TEST_CASE("a full bench run fits in an interactive budget", "[bench]") {
    // The slider re-runs extraction plus transient on every input event. Those
    // two together are what must stay fast; the field map and the auto-fix are
    // deliberately excluded here because the UI defers them.
    nlohmann::json req = base_request();
    req["field"] = false;
    req["fix"] = false;
    const bench::Request r = bench::request_from_json(req);

    bench::run(r);                                     // warm
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) bench::run(r);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count() / 10;
    INFO("extraction + transient took " << ms << " ms");
    CHECK(ms < 25.0);
}

TEST_CASE("copper loss follows sqrt(f) above the skin crossover", "[bench]") {
    // DC below, one skin-depth sheet above: R_ac(4f) = 2 R_ac(f) once delta is
    // inside the foil, and the DC floor holds below it.
    const double w = 0.25e-3, t = 35e-6;
    const double r_dc = bench::r_ac_per_m(w, t, 0);
    CHECK_THAT(r_dc, WithinRel(1.724e-8 / (w * t), 1e-12));
    // at 10 kHz delta (~660 um) >> t: still DC
    CHECK_THAT(bench::r_ac_per_m(w, t, 1e4), WithinRel(r_dc, 1e-12));
    // deep in skin: sqrt(f) scaling, exactly
    const double a = bench::r_ac_per_m(w, t, 1e8);
    const double b = bench::r_ac_per_m(w, t, 4e8);
    CHECK_THAT(b, WithinRel(2 * a, 1e-9));
    CHECK(a > r_dc);
    // and the bench reports it
    nlohmann::json req = base_request();
    req["field"] = false; req["fix"] = false;
    const auto out = bench::run(bench::request_from_json(req));
    CHECK(out["rlgc"]["rAcOhmPerM"].get<double>() > 0.0);
    CHECK(out["rlgc"]["fKneeMhz"].get<double>() > 0.0);
}

TEST_CASE("two in-phase aggressors superpose exactly on a symmetric victim",
          "[bench][triple]") {
    // Linearity is the check: with identical terminations everywhere, zeroing
    // one source leaves the identical network, so the two-aggressor response
    // is EXACTLY the sum of the single-aggressor ones — and by symmetry, twice
    // one of them, at every instant. If this fails, the multi-drive stamp is
    // wrong; there is no tolerance to hide in.
    bem::PairSection s;
    s.triple = true;
    s.w1 = s.w2 = s.w3 = 0.25e-3;
    s.gap = s.gap2 = 0.25e-3;
    s.h = 0.2e-3; s.t = 35e-6; s.eps_r = 4.3;
    const bench::Extraction ex = bench::extract(s);
    REQUIRE(ex.p.n == 3);

    mna::DriveOptions one, two;
    one.length_m = two.length_m = 0.06;
    one.rise_s = two.rise_s = 0.5e-9;
    // identical impedances at every port, so superposition is exact
    const double z = ex.p.z0(1);
    for (auto* o : {&one, &two}) {
        o->z_src = z; o->z_term = z; o->z_victim_near = z;
        o->sections = 24; o->max_steps = 4000;
    }
    one.aggressors = {0};
    two.aggressors = {0, 2};

    const mna::Waveforms w1 = mna::simulate(ex.p, one);
    const mna::Waveforms w2 = mna::simulate(ex.p, two);
    REQUIRE(w1.vic_near.size() == w2.vic_near.size());
    // victim is line 1 in both runs (first non-aggressor)
    CHECK_THAT(w2.next_peak_v, WithinRel(2.0 * w1.next_peak_v, 1e-6));
    // and not just the peak: the whole waveform doubles
    for (size_t i = 0; i < w1.vic_near.size(); i += 7)
        if (std::abs(w1.vic_near[i]) > 1e-5)
            CHECK_THAT((double)w2.vic_near[i],
                       WithinRel(2.0 * (double)w1.vic_near[i], 1e-4));
}

TEST_CASE("the triple request runs end to end through the bench", "[bench][triple]") {
    nlohmann::json req = base_request();
    req["mode"] = "triple";
    req["gap2Mm"] = 0.2;
    req["field"] = false;
    req["fix"] = false;
    const auto out = bench::run(bench::request_from_json(req));
    REQUIRE_FALSE(out.contains("error"));
    // more attackers, more noise: the triple must exceed the same pair
    nlohmann::json pj = base_request();
    pj["field"] = false; pj["fix"] = false;
    const auto pair = bench::run(bench::request_from_json(pj));
    CHECK(out["verdict"]["peakMv"].get<double>() >
          pair["verdict"]["peakMv"].get<double>());
    // and a non-microstrip triple is refused, not guessed
    req["mode"] = "triple";
    req["hMm"] = 0;
    CHECK_THROWS_AS(bench::run(bench::request_from_json(req)), std::invalid_argument);
}
