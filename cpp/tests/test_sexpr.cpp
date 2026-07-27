#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <faraday/SExpr.hpp>

using faraday::SExpr;
using faraday::SExprError;

TEST_CASE("sexpr: basic parse and navigation", "[sexpr]") {
    SExpr root = SExpr::parse("(kicad_pcb (version 20221018) (net 1 \"GND\"))");
    CHECK(root.name() == "kicad_pcb");
    REQUIRE(root.find("version") != nullptr);
    CHECK(root.number_of("version") == 20221018);
    const SExpr* net = root.find("net");
    REQUIRE(net != nullptr);
    CHECK(net->number_at(1) == 1);
    CHECK(net->atom_at(2) == "GND");
}

TEST_CASE("sexpr: quoted strings with escapes", "[sexpr]") {
    SExpr root = SExpr::parse(R"((a (b "hello \"world\"") (c "line\nbreak") (d "back\\slash")))");
    CHECK(root.find("b")->atom_at(1) == "hello \"world\"");
    CHECK(root.find("c")->atom_at(1) == "line\nbreak");
    CHECK(root.find("d")->atom_at(1) == "back\\slash");
}

TEST_CASE("sexpr: find_all returns every named child in order", "[sexpr]") {
    SExpr root = SExpr::parse("(r (net 0) (x 1) (net 1) (net 2))");
    auto nets = root.find_all("net");
    REQUIRE(nets.size() == 3);
    CHECK(nets[2]->number_at(1) == 2);
}

TEST_CASE("sexpr: negative and decimal numbers", "[sexpr]") {
    SExpr root = SExpr::parse("(at -0.7875 10.5 90)");
    CHECK(root.number_at(1) == -0.7875);
    CHECK(root.number_at(2) == 10.5);
}

TEST_CASE("sexpr: malformed input throws, never a silent fallback", "[sexpr]") {
    CHECK_THROWS_AS(SExpr::parse("(unterminated"), SExprError);
    CHECK_THROWS_AS(SExpr::parse("(a) trailing"), SExprError);
    CHECK_THROWS_AS(SExpr::parse("atom-only"), SExprError);
    CHECK_THROWS_AS(SExpr::parse("(a (b \"unclosed))"), SExprError);
    SExpr root = SExpr::parse("(a (b not-a-number))");
    CHECK_THROWS_AS(root.number_of("b"), SExprError);
    CHECK_THROWS_AS(root.value_of("missing"), SExprError);
}
