#include <engine/imagegraph/VerletReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.verlet_replay")
using namespace engine::imagegraph;
namespace {
	VerletReplayState Reset(std::span<const VerletPoint> points, std::span<const VerletEdge> edges = {}) {
		VerletReplayState state;
		Diagnostic diagnostic;
		REQUIRE(
			ResetVerletReplay(points, edges, 0, 7, Limits::MaximumEvaluationBytes, state, diagnostic) ==
			Status::Ok
		);
		return state;
	}
}
TEST_CASE("source inline verlet gravity and quartic drag preserve previous positions", "[imagegraph]") {
	const std::array points{VerletPoint{{2, 3}, {1, 1}, {0, 0}, .5}};
	const auto initial = Reset(points);
	VerletStepSettings settings;
	settings.Substeps = 1;
	settings.Gravity = {0, 10};
	VerletReplayState next;
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(initial, 1, 7, settings, next, diagnostic) == Status::Ok);
	CHECK(next.Mesh.Points[0].Position.X == Catch::Approx(2.9375));
	CHECK(next.Mesh.Points[0].Position.Y == Catch::Approx(5.8125));
	CHECK(next.Mesh.Points[0].Previous == Vector2{2, 3});
	CHECK(next.Mesh.Points[0].BeforePrevious == Vector2{1, 1});
	CHECK(initial.Mesh.Points[0] == points[0]);
}
TEST_CASE("source simple verlet has separate gravity scale and pinned history", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}}, VerletPoint{{5, 6}, {2, 3}, {}, 0, true}};
	auto state = Reset(points);
	VerletStepSettings settings;
	settings.Simple = true;
	settings.Substeps = 2;
	settings.Gravity = {0, 2};
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK(state.Mesh.Points[0].Position == Vector2{0, 3});
	CHECK(state.Mesh.Points[1].Position == Vector2{5, 6});
	CHECK(state.Mesh.Points[1].Previous == Vector2{5, 6});
}
TEST_CASE("source constraints preserve target flexibility and fixed endpoint ordering", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}, {}, 0, true}, VerletPoint{{4, 0}, {4, 0}}};
	const std::array edges{VerletEdge{0, 1, 2, .5}};
	auto state = Reset(points, edges);
	VerletStepSettings settings;
	settings.Substeps = 1;
	settings.Gravity = {};
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK(state.Mesh.Points[0].Position == Vector2{0, 0});
	CHECK(state.Mesh.Points[1].Position.X == Catch::Approx(3));
}
TEST_CASE("source constraints with two fixed endpoints restore previous positions", "[imagegraph]") {
	const std::array points{
		VerletPoint{{1, 2}, {0, 0}, {}, 0, true}, VerletPoint{{4, 5}, {3, 3}, {}, 0, false, true}
	};
	const std::array edges{VerletEdge{0, 1, 2}};
	auto state = Reset(points, edges);
	VerletStepSettings settings;
	settings.Substeps = 1;
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK(state.Mesh.Points[0].Position == Vector2{0, 0});
	CHECK(state.Mesh.Points[1].Position == Vector2{3, 3});
}
TEST_CASE("source inline verlet disables edges with inactive endpoints", "[imagegraph]") {
	const std::array points{
		VerletPoint{{0, 0}, {0, 0}, {}, 0, false, false, false}, VerletPoint{{4, 0}, {4, 0}}
	};
	const std::array edges{VerletEdge{0, 1, 2}};
	auto state = Reset(points, edges);
	VerletStepSettings settings;
	settings.Substeps = 1;
	settings.Gravity = {};
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK_FALSE(state.Mesh.Edges[0].Active);
	CHECK(state.Mesh.Points[1].Position == Vector2{4, 0});
}
TEST_CASE("source inline angular drag retains orientation independently of length", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}, {}, 0, true}, VerletPoint{{0, -4}, {0, -4}}};
	const std::array edges{VerletEdge{0, 1, 2, 0, 0, .5}};
	auto state = Reset(points, edges);
	VerletStepSettings settings;
	settings.Substeps = 1;
	settings.Gravity = {};
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK(state.Mesh.Edges[0].DirectionDegrees == Catch::Approx(45));
	CHECK(state.Mesh.Points[1].Position.X == Catch::Approx(std::sqrt(2.0)));
	CHECK(state.Mesh.Points[1].Position.Y == Catch::Approx(-std::sqrt(2.0)));
}
TEST_CASE("verlet reset and long fixed tick replay have identical snapshots", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}}, VerletPoint{{2, 0}, {2, 0}}};
	const std::array edges{VerletEdge{0, 1, 2}};
	auto first = Reset(points, edges);
	auto second = Reset(points, edges);
	Diagnostic diagnostic;
	VerletStepSettings settings;
	for (uint64_t tick = 1; tick <= 512; ++tick) {
		REQUIRE(StepVerletReplay(first, tick, 7, settings, first, diagnostic) == Status::Ok);
		REQUIRE(StepVerletReplay(second, tick, 7, settings, second, diagnostic) == Status::Ok);
	}
	CHECK(first == second);
	REQUIRE(
		ResetVerletReplay(points, edges, 0, 7, Limits::MaximumEvaluationBytes, second, diagnostic) ==
		Status::Ok
	);
	for (uint64_t tick = 1; tick <= 512; ++tick)
		REQUIRE(StepVerletReplay(second, tick, 7, settings, second, diagnostic) == Status::Ok);
	CHECK(first == second);
}
TEST_CASE("verlet failed step or reset cannot publish partial state", "[imagegraph]") {
	const std::array points{VerletPoint{{0, 0}, {0, 0}}};
	auto state = Reset(points);
	const auto original = state;
	Diagnostic diagnostic;
	VerletStepSettings settings;
	CHECK(StepVerletReplay(state, 2, 7, settings, state, diagnostic) == Status::InvalidValue);
	CHECK(StepVerletReplay(state, 1, 8, settings, state, diagnostic) == Status::InvalidValue);
	settings.MaximumBytes = 0;
	CHECK(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::LimitExceeded);
	settings.MaximumBytes = Limits::MaximumEvaluationBytes;
	settings.MaximumWork = 0;
	CHECK(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::LimitExceeded);
	const std::array invalid{VerletEdge{0, 1}};
	CHECK(
		ResetVerletReplay(points, invalid, 0, 7, Limits::MaximumEvaluationBytes, state, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(state == original);
}
TEST_CASE("verlet rejects nonfinite arithmetic after bounded candidate work", "[imagegraph]") {
	const std::array points{
		VerletPoint{{std::numeric_limits<double>::max(), 0}, {-std::numeric_limits<double>::max(), 0}}
	};
	auto state = Reset(points);
	const auto original = state;
	Diagnostic diagnostic;
	VerletStepSettings settings;
	settings.Substeps = 1;
	CHECK(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::InvalidValue);
	CHECK(state == original);
}
TEST_CASE("source inline single walls collide before and after constraints", "[imagegraph]") {
	const std::array points{VerletPoint{{1, -2}, {1, -2}}, VerletPoint{{1, 2}, {1, 2}}};
	auto state = Reset(points);
	VerletStepSettings settings;
	settings.Substeps = 1;
	settings.Gravity = {};
	settings.Wall = 1;
	Diagnostic diagnostic;
	REQUIRE(StepVerletReplay(state, 1, 7, settings, state, diagnostic) == Status::Ok);
	CHECK(state.Mesh.Points[0].Position == Vector2{1, 0});
	CHECK(state.Mesh.Points[0].Rest);
	CHECK_FALSE(state.Mesh.Points[1].Rest);
	CHECK(state.Mesh.Points[0].Previous == Vector2{1, -2});
	const auto unchanged = state;
	settings.Wall = 3;
	CHECK(StepVerletReplay(state, 2, 7, settings, state, diagnostic) == Status::UnsupportedExecution);
	CHECK(state == unchanged);
}
