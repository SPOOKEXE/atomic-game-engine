#include "../src/SourcePathWaveRandom.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_path_wave_random")
TEST_DEPENDS("engine.imagegraph.source_random")
using engine::imagegraph::detail::SourcePathWaveFrac;
using engine::imagegraph::detail::SourcePathWaveRandom;
using engine::imagegraph::detail::SourcePathWaveSeedBits;

// These values come from the pinned HTML5 InitRandom/randReal and Composer random_function.gml.
// The following stream draws distinguish the source reseed order from independent range helpers.
TEST_CASE("Wave seeded ranges preserve source interpolation and the final stream", "[path_wave_random]") {
	struct Fixture {
		double Seed, Result, Next, After;
	};
	const std::array fixtures{
		Fixture{0., 1.037165290693364, .8061879765271153, .7674706847255447},
		Fixture{.25, .5066141627061245, .8061879765271153, .7674706847255447},
		Fixture{-.25, -1.6299691112385921, .9255384131919305, .5099332265136453},
		Fixture{1.75, -1.750636895653157, .6848448131628543, .9770680027907099},
		Fixture{12345.5, .0019104748507543512, .0056279744047801825, .774337738647283},
		Fixture{2147483647.25, 2.1669688853048577, .6054015371973633, .992209569547423},
		Fixture{4294967296.25, -9114798973.737795, .8061879765271153, .7674706847255447}
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Seed);
		SourcePathWaveRandom random;
		const auto result = random.SeededRange(-2, 3, fixture.Seed);
		REQUIRE(result);
		CHECK(*result == Catch::Approx(fixture.Result).epsilon(1e-14));
		CHECK(random.Unit() == fixture.Next);
		CHECK(random.Unit() == fixture.After);
	}
}

TEST_CASE("Wave noise preserves both source draws even at integer coordinates", "[path_wave_random]") {
	struct Fixture {
		double Coordinate, Result, Next;
	};
	const std::array fixtures{
		Fixture{0., .6074330581386728, .5099332265136453},
		Fixture{.25, .6571370198657442, .5099332265136453},
		Fixture{-.25, .18170046768230408, .8567445170398543},
		Fixture{1.75, .7088136295305162, .7674706847255447},
		Fixture{12345.5, .5253124085838499, .015874267097504002}
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Coordinate);
		SourcePathWaveRandom random;
		const auto result = random.Noise(fixture.Coordinate);
		REQUIRE(result);
		CHECK(*result == Catch::Approx(fixture.Result).epsilon(1e-14));
		CHECK(random.Unit() == fixture.Next);
	}
}

TEST_CASE("Wave equal ranges still reseed and wiggle retains its last source draw", "[path_wave_random]") {
	SourcePathWaveRandom random;
	REQUIRE(random.SeededRange(7, 7, 0) == 7.);
	CHECK(random.Unit() == .8061879765271153);
	const auto reversed = random.SeededRange(3, -2, .25);
	REQUIRE(reversed);
	CHECK(*reversed == Catch::Approx(.4933858372938755).epsilon(1e-14));
	CHECK(random.Unit() == .8061879765271153);
	const auto forward = random.Wiggle(-2, 3, 2, .25, 0);
	REQUIRE(forward);
	CHECK(*forward == Catch::Approx(1.832428678326508).epsilon(1e-14));
	CHECK(random.Unit() == .5099332265136453);
	const auto backward = random.Wiggle(-2, 3, -2, .25, 0);
	REQUIRE(backward);
	CHECK(*backward == Catch::Approx(-1.0734813427894756).epsilon(1e-14));
	CHECK(random.Unit() == .8567445170398543);
	REQUIRE(random.Wiggle(7, 7, 0, .5, 12345) == 7.);
	CHECK(random.Unit() == .015874267097504002);
}

TEST_CASE("Wave source fractions and seed casts preserve signed wrapped integers", "[path_wave_random]") {
	CHECK(SourcePathWaveFrac(-.25) == -.25);
	CHECK(SourcePathWaveFrac(-1.25) == -.25);
	CHECK(SourcePathWaveFrac(1.25) == .25);
	CHECK(SourcePathWaveFrac(2147483648.25) == 4294967296.25);
	CHECK(SourcePathWaveFrac(4294967296.25) == 4294967296.25);
	CHECK(SourcePathWaveFrac(-4294967296.25) == -4294967296.25);
	CHECK(SourcePathWaveSeedBits(-1.) == UINT32_MAX);
	CHECK(SourcePathWaveSeedBits(4294967297.) == 1);
	CHECK(SourcePathWaveSeedBits(-4294967297.) == UINT32_MAX);
	CHECK(SourcePathWaveSeedBits(std::numeric_limits<double>::max()) == 0);
	SourcePathWaveRandom random;
	REQUIRE(random.Seed(4294967297.));
	CHECK(random.Unit() == .18299215574888147);
	REQUIRE(random.Seed(-4294967297.));
	CHECK(random.Unit() == .18069155382955984);
}

TEST_CASE("Wave random refuses nonfinite inputs and finite arithmetic overflow", "[path_wave_random]") {
	SourcePathWaveRandom random;
	for (double invalid :
		 {std::numeric_limits<double>::infinity(),
		  -std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::quiet_NaN()}) {
		REQUIRE(random.Seed(12345));
		CHECK_FALSE(SourcePathWaveSeedBits(invalid));
		CHECK_FALSE(SourcePathWaveFrac(invalid));
		CHECK_FALSE(random.Seed(invalid));
		CHECK_FALSE(random.SeededRange(0, 1, invalid));
		CHECK_FALSE(random.Noise(invalid));
		CHECK_FALSE(random.Wiggle(0, 1, invalid, 0, 0));
		CHECK(random.Unit() == .7990098701785364);
	}
	const double largest = std::numeric_limits<double>::max();
	CHECK_FALSE(random.SeededRange(-largest, largest, .5));
	CHECK_FALSE(random.Noise(largest));
	CHECK_FALSE(random.Wiggle(0, 1, largest, largest, 0));
}
