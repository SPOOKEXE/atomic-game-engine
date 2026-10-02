#include "../src/ProcessorBatch.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.stateful_data_nodes")
using namespace engine::imagegraph;
namespace {
	struct Run {
		bool Ok = false;
		Status Code = Status::Ok;
		std::string Message;
		std::vector<AuthoredValue> Outputs;
		DataReplayState History;
	};
	Run Sample(
		std::string type,
		uint64_t tick,
		double subframe,
		bool negative,
		std::vector<AuthoredValue> controls,
		const DataReplayState *previous = nullptr,
		uint64_t budget = Limits::MaximumEvaluationBytes
	) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		Node node{"data", type, "", {}, std::move(controls)};
		EvaluationRequest request;
		request.Tick = tick;
		request.Subframe = subframe;
		request.NegativeFrame = negative;
		request.DataReplay = previous;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = budget;
		for (const auto &input : entry->Inputs) {
			const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &v) {
				return v.Port == input.Id;
			});
			if (found != node.Values.end())
				context.Values.emplace_back(input.Id, found->Data);
			else if (auto fallback = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, std::move(*fallback));
		}
		Run run;
		run.Ok = engine::imagegraph::detail::RunProcessorBatch(
			context, engine::imagegraph::detail::FindExecutor(type)
		);
		run.Code = context.FailureCode;
		run.Message = context.FailureMessage;
		run.Outputs = std::move(context.OutputValues);
		run.History.Entries = std::move(context.DataUpdates);
		return run;
	}
}
TEST_CASE(
	"Differential preserves signed fractional frame and repeated sample source history",
	"[imagegraph][stateful_data]"
) {
	auto first = Sample("pc.differential", 0, 0, false, {{"value", 4.0}});
	INFO(first.Message);
	REQUIRE(first.Ok);
	CHECK(std::get<double>(first.Outputs[0].Data) == 0);
	auto second = Sample("pc.differential", 0, .5, false, {{"value", 7.0}}, &first.History);
	REQUIRE(second.Ok);
	CHECK(std::get<double>(second.Outputs[0].Data) == 6);
	auto edit = Sample("pc.differential", 0, .5, false, {{"value", 20.0}}, &second.History);
	REQUIRE(edit.Ok);
	CHECK(std::get<double>(edit.Outputs[0].Data) == 0);
	auto rewind = Sample("pc.differential", 0, .5, true, {{"value", 18.0}}, &edit.History);
	REQUIRE(rewind.Ok);
	CHECK(std::get<double>(rewind.Outputs[0].Data) == 2);
	CHECK(rewind.History.Entries[0].PreviousFrame == -.5);
	auto bounded = Sample("pc.differential", 1, 0, false, {{"value", 1.0}}, nullptr, 1);
	CHECK_FALSE(bounded.Ok);
	CHECK(bounded.Code == Status::LimitExceeded);
	CHECK(bounded.History.Entries.empty());
}
TEST_CASE("Differential keeps processor rows independent", "[imagegraph][stateful_data]") {
	auto first =
		Sample("pc.differential", 1, 0, false, {{"value", ArrayValue{ValueType::Scalar, {2.0, 8.0}}}});
	INFO(first.Message);
	REQUIRE(first.Ok);
	REQUIRE(first.History.Entries.size() == 2);
	auto second = Sample(
		"pc.differential", 2, 0, false, {{"value", ArrayValue{ValueType::Scalar, {5.0, 9.0}}}}, &first.History
	);
	INFO(second.Message);
	REQUIRE(second.Ok);
	REQUIRE(second.History.Entries.size() == 2);
	const auto &out = std::get<ArrayValue>(second.Outputs[0].Data);
	REQUIRE(out.Elements.size() == 2);
	CHECK(std::get<double>(out.Elements[0]) == 3);
	CHECK(std::get<double>(out.Elements[1]) == 1);
}
TEST_CASE(
	"Boolean trigger retains previous values across modes and same-frame edits", "[imagegraph][stateful_data]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		DataReplayState history;
		const std::array<bool, 4> input{false, true, true, false};
		const std::array<std::array<bool, 4>, 4> expected{
			{{false, true, true, false},
			 {false, true, false, false},
			 {false, false, false, true},
			 {false, true, false, true}}
		};
		for (size_t index = 0; index < input.size(); ++index) {
			auto run = Sample(
				"pc.trigger_bool",
				0,
				0,
				false,
				{{"boolean", input[index]}, {"trigger_condition", EnumValue{mode}}},
				&history
			);
			INFO(run.Message);
			REQUIRE(run.Ok);
			CHECK(std::get<bool>(run.Outputs[0].Data) == expected[size_t(mode)][index]);
			history = std::move(run.History);
		}
	}
	auto trueFrame = Sample("pc.trigger_bool", 0, 0, false, {{"boolean", true}});
	REQUIRE(trueFrame.Ok);
	auto invalid = Sample(
		"pc.trigger_bool",
		1,
		0,
		false,
		{{"boolean", false}, {"trigger_condition", EnumValue{99}}},
		&trueFrame.History
	);
	REQUIRE(invalid.Ok);
	CHECK(std::get<bool>(invalid.Outputs[0].Data));
	CHECK(invalid.History.Entries[0].PreviousValue == 0);
}
