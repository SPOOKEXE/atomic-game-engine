#include "../src/ProcessorBatch.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/RandomReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.random_nodes")
using namespace engine::imagegraph;
namespace {
	struct Run {
		bool Ok = false;
		Status Code = Status::Ok;
		std::string Message;
		double Value = 0;
		RandomReplayState Replay;
	};
	Run Random(
		uint64_t tick,
		std::initializer_list<AuthoredValue> controls = {},
		const RandomReplayState *previous = nullptr,
		std::span<const RandomEntropyCapture> entropy = {}
	) {
		const auto *entry = FindCatalogueEntry("pc.random");
		REQUIRE(entry);
		Node node{"random", "pc.random", "", {}, std::vector<AuthoredValue>(controls)};
		node.Values.push_back({"seed", int64_t{12345}});
		EvaluationRequest request;
		request.Tick = tick;
		request.RandomReplay = previous;
		request.RandomEntropy = entropy;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		TimelineSettings timeline;
		timeline.Frames = 3;
		timeline.Last = 2;
		context.Timeline = &timeline;
		for (const auto &input : entry->Inputs) {
			const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
				return value.Port == input.Id;
			});
			if (found != node.Values.end())
				context.Values.emplace_back(input.Id, found->Data);
			else if (auto fallback = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, std::move(*fallback));
		}
		Run result;
		result.Ok = engine::imagegraph::detail::RunProcessorBatch(
			context, engine::imagegraph::detail::FindExecutor(node.Type)
		);
		result.Code = context.FailureCode;
		result.Message = context.FailureMessage;
		if (result.Ok) {
			REQUIRE(context.OutputValues.size() == 1);
			result.Value = std::get<double>(context.OutputValues[0].Data);
			result.Replay.Entries = std::move(context.RandomUpdates);
		}
		return result;
	}
}
TEST_CASE("Random distributions match inspected official HTML5 seed vectors", "[imagegraph][random_nodes]") {
	const auto uniform = Random(0);
	INFO(uniform.Message);
	REQUIRE(uniform.Ok);
	CHECK(uniform.Value == Catch::Approx(.7990098701785364));
	const auto gaussian = Random(0, {{"distribution", EnumValue{1}}});
	REQUIRE(gaussian.Ok);
	CHECK(gaussian.Value == Catch::Approx(-.006797350629150205));
	const auto bernoulli = Random(0, {{"distribution", EnumValue{2}}, {"p", .5}});
	REQUIRE(bernoulli.Ok);
	CHECK(bernoulli.Value == 0);
	const auto binomial = Random(0, {{"distribution", EnumValue{3}}, {"t", 10.0}});
	REQUIRE(binomial.Ok);
	CHECK(binomial.Value == 5);
	const auto custom = Random(0, {{"distribution", EnumValue{4}}});
	REQUIRE(custom.Ok);
	CHECK(custom.Value == Catch::Approx(.7990098701785364 * 128 / 127));
	const auto integer = Random(0, {{"integer", true}});
	REQUIRE(integer.Ok);
	CHECK(integer.Value == 1);
}
TEST_CASE(
	"Random smoothing retains source reset and future-zero convolution semantics",
	"[imagegraph][random_nodes]"
) {
	constexpr double value = .7990098701785364;
	const auto average = Random(0, {{"smoothing", EnumValue{1}}});
	REQUIRE(average.Ok);
	const auto nextAverage = Random(1, {{"smoothing", EnumValue{1}}}, &average.Replay);
	REQUIRE(nextAverage.Ok);
	CHECK(nextAverage.Value == Catch::Approx(value / 2));
	const auto lerp = Random(0, {{"smoothing", EnumValue{3}}});
	REQUIRE(lerp.Ok);
	CHECK(lerp.Value == Catch::Approx(value / 2));
	const auto nextLerp = Random(1, {{"smoothing", EnumValue{3}}}, &lerp.Replay);
	REQUIRE(nextLerp.Ok);
	CHECK(nextLerp.Value == Catch::Approx(value * .75));
	const auto convolution = Random(0, {{"smoothing", EnumValue{2}}});
	INFO(convolution.Message);
	REQUIRE(convolution.Ok);
	CHECK(convolution.Value == Catch::Approx(value / 3));
	const auto nextConvolution = Random(1, {{"smoothing", EnumValue{2}}}, &convolution.Replay);
	REQUIRE(nextConvolution.Ok);
	CHECK(nextConvolution.Value == Catch::Approx(value * 2 / 3));
}
TEST_CASE(
	"Random reshuffle modes consume recorded host seeds and refuse missing entropy",
	"[imagegraph][random_nodes]"
) {
	RandomEntropyCapture capture;
	capture.NodeId = "random";
	capture.ReshuffleSeed = 123456;
	const std::span<const RandomEntropyCapture> captures{&capture, 1};
	for (int64_t mode = 0; mode < 6; ++mode) {
		const auto run = Random(
			0,
			{{"shuffle", true},
			 {"mode", EnumValue{mode}},
			 {"trigger", true},
			 {"average_period", .1},
			 {"period_variance", 0.0}},
			nullptr,
			captures
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		REQUIRE(run.Replay.Entries.size() == 1);
		CHECK(run.Replay.Entries[0].StoredSeed == 123456);
	}
	CHECK(Random(0, {{"shuffle", true}}).Code == Status::UnsupportedExecution);
	CHECK(Random(0, {{"deterministic", false}}).Code == Status::UnsupportedExecution);
	capture.CurrentTimeMilliseconds = 100007;
	const auto recorded = Random(0, {{"deterministic", false}}, nullptr, captures);
	REQUIRE(recorded.Ok);
	CHECK(recorded.Replay.Entries[0].StoredSeed == 12352);
	CHECK(
		Random(0, {{"mode", EnumValue{1}}, {"shuffle", true}, {"period", int64_t{0}}}, nullptr, captures)
			.Code == Status::InvalidValue
	);
}
