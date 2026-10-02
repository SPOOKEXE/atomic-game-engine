#include "NodeHarness.hpp"

#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_sparkle")
using namespace engine::imagegraph;

namespace {
	struct SparkleRun {
		bool Ok = false;
		Status Code = Status::Ok;
		std::string Message;
		std::vector<std::pair<std::string, Image>> Images;
		std::vector<std::pair<std::string, ImageArray>> Arrays;
	};
	SparkleRun DrawSparkle(
		bool array = false, int alteration = 0, int64_t loop = 0, int64_t loopLength = 4, FrameTime time = {}
	) {
		Node node{
			"sparkle",
			"pc.mk_sparkle",
			"",
			{},
			{{"seed", 1.},
			 {"size", int64_t{8}},
			 {"scatter", 1.},
			 {"array", array},
			 {"array_length", int64_t{3}},
			 {"loop", EnumValue{loop}},
			 {"loop_length", loopLength}}
		};
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		EvaluationRequest request;
		request.Tick = time.Tick;
		request.Subframe = time.Subframe;
		request.NegativeFrame = time.NegativeFrame;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &input : entry->Inputs) {
			const auto authored =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
					return value.Port == input.Id;
				});
			if (authored != node.Values.end())
				context.Values.emplace_back(input.Id, authored->Data);
			else if (auto value = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, std::move(*value));
		}
		SourceBuiltinRandomCapture capture;
		capture.Authored = node;
		capture.Tick = request.Tick;
		capture.Subframe = request.Subframe;
		capture.NegativeFrame = request.NegativeFrame;
		for (const auto &[port, value] : context.Values)
			capture.Inputs.push_back({std::string(port), value});
		capture.Draws.push_back({SourceBuiltinRandomOperation::IRandom, 0, 2, 0});
		for (int i = 0; i < 3; ++i) {
			capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .5});
			capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .5});
			capture.Draws.push_back({SourceBuiltinRandomOperation::IRandomRange, 1, 1, 1});
			capture.Draws.push_back({SourceBuiltinRandomOperation::IRandomRange, 1, 1, 1});
			capture.Draws.push_back({SourceBuiltinRandomOperation::IRandomRange, 1, 2, 2});
			capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .9});
			capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .9});
		}
		if (alteration == 1) capture.Draws.pop_back();
		if (alteration == 2) capture.Draws.front().Upper = 3;
		if (alteration == 3) capture.Inputs.front().Data = false;
		if (alteration == 4) capture.Tick = 1;
		if (alteration == 5) capture.Draws.push_back({});
		if (alteration != 6) request.BuiltinRandomCaptures = {&capture, 1};
		if (alteration == 7) request.RequireSourceGpuRasterCoverage = true;
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(executor);
		SparkleRun result;
		result.Ok = executor(context);
		result.Code = context.FailureCode;
		result.Message = context.FailureMessage;
		result.Images = std::move(context.OutputImages);
		result.Arrays = std::move(context.OutputImageArrays);
		return result;
	}
	std::vector<uint8_t> Expected(size_t armLength) {
		std::vector<uint8_t> image(8 * 8 * 4);
		for (size_t y = 0; y < 8; ++y)
			for (size_t x = 0; x < 8; ++x) {
				const bool nearEnd = armLength == 4 ? (x >= 2 && x <= 5) : (x == 2 || x == 5);
				const bool nearEndY = armLength == 4 ? (y >= 2 && y <= 5) : (y == 2 || y == 5);
				if (((y == 1 || y == 6) && nearEnd) || ((x == 1 || x == 6) && nearEndY))
					for (size_t c = 0; c < 4; ++c)
						image[(y * 8 + x) * 4 + c] = 255;
			}
		return image;
	}
}
TEST_CASE(
	"MK Sparkle mirrors recorded source line geometry and resets draws for each array frame",
	"[imagegraph][source_2d]"
) {
	const auto single = DrawSparkle();
	INFO(single.Message);
	REQUIRE(single.Ok);
	REQUIRE(single.Images.size() == 1);
	CHECK(single.Images.front().second.Pixels == Expected(4));
	const auto array = DrawSparkle(true);
	INFO(array.Message);
	REQUIRE(array.Ok);
	REQUIRE(array.Arrays.size() == 1);
	const auto &frames = array.Arrays.front().second;
	REQUIRE(frames.Images.size() == 3);
	REQUIRE(frames.Items.size() == 3);
	CHECK(frames.Images[0].Pixels == Expected(4));
	CHECK(frames.Images[1].Pixels == Expected(2));
	CHECK(frames.Images[2].Pixels == std::vector<uint8_t>(8 * 8 * 4));
}
TEST_CASE(
	"MK Sparkle validates the recorded source call stream and refuses unresolved runner state",
	"[imagegraph][source_2d]"
) {
	for (int alteration = 1; alteration <= 7; ++alteration) {
		const auto result = DrawSparkle(false, alteration);
		INFO(result.Message);
		CHECK_FALSE(result.Ok);
		CHECK(
			result.Code == (alteration == 1 || alteration == 4 || alteration == 6 || alteration == 7
								? Status::UnsupportedExecution
								: Status::InvalidValue)
		);
	}
}

TEST_CASE("MK Sparkle preserves source signed and zero-divisor safe modulo", "[source_2d]") {
	const auto negative = DrawSparkle(false, 0, 1, 4, {1, 0, true});
	const auto unlooped = DrawSparkle(false, 0, 0, 4, {1, 0, true});
	REQUIRE(negative.Ok);
	REQUIRE(unlooped.Ok);
	CHECK(negative.Images.front().second.Pixels == unlooped.Images.front().second.Pixels);
	CHECK(negative.Images.front().second.Pixels != std::vector<uint8_t>(8 * 8 * 4));
	const auto zeroPing = DrawSparkle(false, 0, 2, 0, {1, 0, false});
	const auto minusTwo = DrawSparkle(false, 0, 0, 4, {2, 0, true});
	REQUIRE(zeroPing.Ok);
	REQUIRE(minusTwo.Ok);
	CHECK(zeroPing.Images.front().second.Pixels == minusTwo.Images.front().second.Pixels);
	const auto negativePeriod = DrawSparkle(false, 0, 1, -4, {1, 0, false});
	const auto positive = DrawSparkle(false, 0, 0, 4, {1, 0, false});
	REQUIRE(negativePeriod.Ok);
	REQUIRE(positive.Ok);
	CHECK(negativePeriod.Images.front().second.Pixels == positive.Images.front().second.Pixels);
}
