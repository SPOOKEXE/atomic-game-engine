#include "SourceStrandState.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_strand_state")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace {
	SourceStrandState State(bool free = false) {
		SourceStrandState state;
		SourceStrandHair hair;
		hair.SourceId = 123456;
		hair.Free = free;
		hair.Tension = 0;
		hair.AngularTension = 0;
		hair.Spring = 1;
		hair.Points = {{.Position = {0, 0}, .Previous = {0, 0}}, {.Position = {4, 0}, .Previous = {4, 0}}};
		hair.Lengths = {4, 4};
		hair.RestAngles = {0, 0};
		state.Hairs.push_back(hair);
		return state;
	}
} // namespace
TEST_CASE(
	"Source strand gravity repeats captured motion and preserves attached root", "[imagegraph][source_strand]"
) {
	auto state = State();
	state.Hairs[0].Points[0].Delta = {100, 100};
	state.Hairs[0].Points[1].Delta = {2, 3};
	const auto before = state;
	Diagnostic diagnostic;
	SourceStrandState result;
	REQUIRE(
		SourceStrandGravity(state, 2, -90, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok
	);
	CHECK(result.Hairs[0].Points[0] == before.Hairs[0].Points[0]);
	CHECK(result.Hairs[0].Points[1].Position[0] == Catch::Approx(6));
	CHECK(result.Hairs[0].Points[1].Position[1] == Catch::Approx(5));
	CHECK(result.Hairs[0].Points[1].Delta == before.Hairs[0].Points[1].Delta);
	REQUIRE(
		SourceStrandGravity(result, 2, -90, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok
	);
	CHECK(result.Hairs[0].Points[1].Position[0] == Catch::Approx(8));
	CHECK(result.Hairs[0].Points[1].Position[1] == Catch::Approx(10));
	CHECK(state == before);
}
TEST_CASE("Source strand gravity moves every free root and fails atomically", "[imagegraph][source_strand]") {
	auto state = State(true);
	state.Hairs[0].Points[0].Delta = {1, 2};
	Diagnostic diagnostic;
	SourceStrandState result;
	REQUIRE(
		SourceStrandGravity(state, 3, 0, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok
	);
	CHECK(result.Hairs[0].Points[0].Position == std::array<double, 2>{4, 2});
	const auto checkpoint = result;
	CHECK(SourceStrandGravity(state, 3, 0, 1, result, diagnostic) == Status::LimitExceeded);
	CHECK(result == checkpoint);
	CHECK(
		SourceStrandGravity(
			state,
			std::numeric_limits<double>::infinity(),
			0,
			Limits::MaximumEvaluationBytes,
			result,
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(result == checkpoint);
}
TEST_CASE("Source strand propagation keeps axis skip and mesh Step arity", "[imagegraph][source_strand]") {
	auto state = State(true);
	state.Hairs[0].Points[1].Delta = {2, 0};
	Diagnostic diagnostic;
	SourceStrandState result;
	REQUIRE(SourceStrandUpdate(state, 3, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok);
	CHECK(result.Hairs[0].Points[1].Position == std::array<double, 2>{4, 0});
	CHECK(result.Hairs[0].Points[1].Delta == std::array<double, 2>{0, 0});
	state.Hairs[0].Points[0].Delta = {2, 3};
	state.Hairs[0].Points[1].Delta = {2, 3};
	REQUIRE(SourceStrandUpdate(state, 3, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok);
	CHECK(result.Hairs[0].Points[0].Position[0] == Catch::Approx(2));
	CHECK(result.Hairs[0].Points[0].Position[1] == Catch::Approx(3));
	CHECK(result.Hairs[0].Points[1].Position[0] == Catch::Approx(6));
	CHECK(result.Hairs[0].Points[1].Position[1] == Catch::Approx(3));
}
TEST_CASE(
	"Source strand reset rootForce precedes detachment and curl refusal "
	"is atomic",
	"[imagegraph][source_strand]"
) {
	auto state = State();
	state.Hairs[0].RootStrength = .1;
	state.Hairs[0].RootForce = 99;
	Diagnostic diagnostic;
	SourceStrandState result;
	REQUIRE(SourceStrandUpdate(state, 0, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok);
	CHECK(result.Hairs[0].RootForce == 0);
	CHECK_FALSE(result.Hairs[0].Free);
	state.Hairs[0].RootStrength = -.5;
	REQUIRE(SourceStrandUpdate(state, 0, Limits::MaximumEvaluationBytes, result, diagnostic) == Status::Ok);
	CHECK(result.Hairs[0].Free);
	const auto checkpoint = result;
	state.Hairs[0].CurlFrequency = 3;
	CHECK(
		SourceStrandUpdate(state, 1, Limits::MaximumEvaluationBytes, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result == checkpoint);
}

TEST_CASE(
	"Source strand constructor consumes length then ID and equal root "
	"needs no random call",
	"[imagegraph][source_strand]"
) {
	SourceStrandCreateSettings settings;
	settings.Hairs = 2;
	settings.Segments = 1;
	settings.Position = {1, 2};
	std::vector<SourceBuiltinRandomDraw> draws{
		{SourceBuiltinRandomOperation::RandomRange, 4, 4, 4},
		{SourceBuiltinRandomOperation::IRandomRange, 100000, 999999, 123456},
		{SourceBuiltinRandomOperation::RandomRange, 4, 4, 4},
		{SourceBuiltinRandomOperation::IRandomRange, 100000, 999999, 987654}
	};
	Diagnostic diagnostic;
	SourceStrandState state;
	REQUIRE(
		SourceStrandCreateUniformPoint(settings, draws, Limits::MaximumEvaluationBytes, state, diagnostic) ==
		Status::Ok
	);
	REQUIRE(state.Hairs.size() == 2);
	CHECK(state.Loop);
	CHECK(state.Hairs[0].SourceId == 123456);
	CHECK(state.Hairs[1].SourceId == 987654);
	CHECK(state.Hairs[0].Points[1].Position == std::array<double, 2>{5, 2});
	CHECK(state.Hairs[1].Points[1].Position[0] == Catch::Approx(-3));
	CHECK(state.Hairs[1].Points[1].Position[1] == Catch::Approx(2));
	CHECK(state.Hairs[0].RootStrength == -1);
	const auto checkpoint = state;
	REQUIRE(
		SourceStrandGravity(state, 1, -90, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	REQUIRE(SourceStrandUpdate(state, 1, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok);
	CHECK(state != checkpoint);
	REQUIRE(
		SourceStrandCreateUniformPoint(settings, draws, Limits::MaximumEvaluationBytes, state, diagnostic) ==
		Status::Ok
	);
	CHECK(state == checkpoint);
	CHECK(
		SourceStrandCreateUniformPoint(settings, {}, Limits::MaximumEvaluationBytes, state, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(state == checkpoint);
	CHECK(SourceStrandCreateUniformPoint(settings, draws, 1, state, diagnostic) == Status::LimitExceeded);
	CHECK(state == checkpoint);
	draws[1].Result = 1000000;
	CHECK(
		SourceStrandCreateUniformPoint(settings, draws, Limits::MaximumEvaluationBytes, state, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(state == checkpoint);
}
TEST_CASE(
	"Source strand default constraints match pinned GML with official "
	"HTML5 math",
	"[imagegraph][source_strand]"
) {
	auto state = State();
	state.Hairs[0].Tension = .95;
	state.Hairs[0].Spring = .8;
	state.Hairs[0].AngularTension = .2;
	state.Hairs[0].RootStrength = .1;
	Diagnostic diagnostic;
	REQUIRE(
		SourceStrandGravity(state, 1, -90, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	REQUIRE(SourceStrandUpdate(state, 1, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok);
	const auto &point = state.Hairs[0].Points[1];
	CHECK(point.Position[0] == Catch::Approx(3.920384922506184).margin(1e-12));
	CHECK(point.Position[1] == Catch::Approx(.7942404177642729).margin(1e-12));
	CHECK(point.Delta[0] == Catch::Approx(-.0796150774938158).margin(1e-12));
	CHECK(point.Delta[1] == Catch::Approx(.7942404177642729).margin(1e-12));
	CHECK(state.Hairs[0].RootForce == 0);
	CHECK_FALSE(state.Hairs[0].Free);
}
TEST_CASE(
	"Strand constructor accounts prior nested clone storage without advancing on refusal", "[source_strand]"
) {
	SourceStrandCreateSettings settings;
	settings.Hairs = 1;
	settings.Segments = 1;
	const std::array<SourceBuiltinRandomDraw, 2> draws{
		{{SourceBuiltinRandomOperation::RandomRange, 4, 4, 4},
		 {SourceBuiltinRandomOperation::IRandomRange, 100000, 999999, 123456}}
	};
	SourceStrandState prior, output;
	Diagnostic diagnostic;
	REQUIRE(
		SourceStrandCreateUniformPoint(settings, draws, Limits::MaximumEvaluationBytes, prior, diagnostic) ==
		Status::Ok
	);
	const uint64_t previous = SourceStrandBytes(prior), candidate = previous;
	const auto unchanged = output;
	for (uint32_t density : {1u, 0u}) {
		settings.Hairs = density;
		CHECK(
			SourceStrandCreateUniformPoint(
				settings, {}, previous + candidate + SourceStrandBytes(output) - 1, output, diagnostic, &prior
			) == Status::LimitExceeded
		);
		CHECK(output == unchanged);
		CHECK(prior.Hairs.size() == 1);
	}
	settings.Hairs = 1;
	const auto checkpoint = prior;
	CHECK(
		SourceStrandCreateUniformPoint(settings, {}, 2 * previous - 1, prior, diagnostic, &prior) ==
		Status::LimitExceeded
	);
	CHECK(prior == checkpoint);
	REQUIRE(
		SourceStrandCreateUniformPoint(settings, {}, 2 * previous, prior, diagnostic, &prior) == Status::Ok
	);
	CHECK(prior == checkpoint);
	settings.Hairs = 0;
	settings.Segments = 65535;
	REQUIRE(
		SourceStrandCreateUniformPoint(
			settings, {}, Limits::MaximumEvaluationBytes, output, diagnostic, &prior
		) == Status::Ok
	);
	CHECK(output.Hairs == prior.Hairs);
}
