#include "ImageGraphSourceEdit.hpp"

#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.hlsl_inputs")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.group_replay")

namespace {
	using namespace engine::imagegraph;
	struct CountedLua final : HostNodeProvider {
		std::unique_ptr<ComposerLuaHost> Lua = engine::script::MakeComposerLuaHost();
		size_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
		) override {
			++Calls;
			return Lua->Capture(invocation, output, failure);
		}
	};
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{11};
		document.Nodes = {
			{"lua",
			 "pc.lua_compute",
			 {},
			 {},
			 {{"lua_code", std::string("counter=(counter or 0)+1 return counter")},
			  {"function_name", std::string("argument")},
			  {"execute_on_frame", true}}},
			{"hlsl", "pc.hlsl", {}, {}, {}}
		};
		document.Nodes.back().DynamicInputs = {
			{"argument_name_0", ValueType::Text, std::string("gain")},
			{"argument_type_0", ValueType::Enum, EnumValue{0}},
			{"argument_value_0", ValueType::Scalar, 9.5}
		};
		document.Nodes.back().SourceAnimatedInputs = {"argument_value_0"};
		document.Keyframes = {
			{"hlsl", "argument_value_0", 0, 9.5, "source", KeyframeEase{}},
			{"hlsl", "argument_value_0", 10, 7.5, "source", KeyframeEase{}}
		};
		document.Keyframes[0].SourceKeyId = "original-first";
		document.Keyframes[1].SourceKeyId = "original-second";
		document.Tracks = {{"hlsl", "argument_value_0", "wrap", -1}};
		document.Links = {{"lua", "return_value", "hlsl", "argument_value_0"}};
		document.Outputs = {{"out", "hlsl", "surface"}};
		return document;
	}
}
TEST_CASE(
	"HLSL selector normalization uses one prepared producer generation and one undo", "[studio][hlsl_inputs]"
) {
	auto document = Graph();
	const auto original = document;
	CountedLua producer;
	EvaluationRequest request;
	request.Tick = 5;
	request.HostProvider = &producer;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 3, request, error));
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(document, plan, "hlsl", request, inputs, error) == Status::Ok);
	REQUIRE(producer.Calls == 1);
	const auto replayBytes = host.Replay.RetainedBytes();
	const auto hostRevision = host.Revision;
	auto selector = document.Nodes.back().DynamicInputs[1];
	selector.Default = EnumValue{2};
	studio::ImageGraphHistory refused(0), history;
	const auto edit = [&](studio::ImageGraphHistory &selectedHistory, const EvaluationSnapshot *prepared) {
		return studio::ApplyImageGraphSourceDynamicInput(
			document,
			selectedHistory,
			host,
			3,
			"hlsl",
			selector,
			request,
			error,
			[](auto &, auto &) { return true; },
			prepared
		);
	};
	CHECK_FALSE(edit(history, nullptr));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(document == original);
	CHECK_FALSE(edit(refused, &inputs));
	INFO(error.Message);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(document == original);
	CHECK(host.Revision == hostRevision);
	CHECK(host.Replay.RetainedBytes() == replayBytes);
	CHECK(producer.Calls == 1);
	const bool changed = edit(history, &inputs);
	INFO(error.Message);
	REQUIRE(changed);
	CHECK(producer.Calls == 1);
	CHECK(document.Links.empty());
	CHECK(document.Nodes.back().DynamicInputs.back().Type == ValueType::Array);
	CHECK(document.Nodes.back().SourceAnimatedInputs == original.Nodes.back().SourceAnimatedInputs);
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(document.Keyframes.front().Tick == 0);
	CHECK(document.Keyframes.front().SourceKeyId.empty());
	CHECK(
		std::get<ArrayValue>(document.Keyframes.front().Data).Elements == std::vector<ElementValue>{0.0, 0.0}
	);
	const auto edited = document;
	CHECK(history.Undo(document));
	CHECK(document == original);
	CHECK_FALSE(history.CanUndo());
	CHECK(history.Redo(document));
	CHECK(document == edited);
	Document reopened;
	REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
	CHECK(reopened == document);
	const auto reopenedStatus = Compile(reopened, plan, error);
	INFO(error.NodeId);
	INFO(error.Port);
	INFO(error.Message);
	REQUIRE(reopenedStatus == Status::Ok);
}
TEST_CASE(
	"HLSL current shape decides resets while Int keeps fractional raw values", "[studio][hlsl_inputs]"
) {
	using studio::detail::HlslArgumentOverride;
	ArrayValue two;
	two.ElementType = ValueType::Scalar;
	two.Elements = {2.25, 7.75};
	CHECK_FALSE(HlslArgumentOverride(2, Value{two}));
	for (const int64_t kind : {0, 1, 7, 8}) {
		CHECK_FALSE(HlslArgumentOverride(kind, Value{2.5}));
		REQUIRE(HlslArgumentOverride(kind, Value{two}));
	}
	CHECK(std::get<int64_t>(*HlslArgumentOverride(7, Value{two})) == -4);
	CHECK(std::get<int64_t>(*HlslArgumentOverride(8, Value{two})) == 0);
	for (const int64_t kind : {2, 3, 4, 5, 6}) {
		const auto reset = HlslArgumentOverride(kind, Value{2.5});
		REQUIRE(reset);
		constexpr size_t lengths[]{2, 3, 4, 9, 16};
		CHECK(std::get<ArrayValue>(*reset).Elements.size() == lengths[kind - 2]);
	}
}

TEST_CASE("HLSL argument rename preserves compatible source keys and identity", "[studio][hlsl_inputs]") {
	auto document = Graph();
	const auto keys = document.Keyframes;
	CountedLua producer;
	EvaluationRequest request;
	request.Tick = 5;
	request.HostProvider = &producer;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 3, request, error));
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(document, plan, "hlsl", request, inputs, error) == Status::Ok);
	auto name = document.Nodes.back().DynamicInputs.front();
	name.Default = std::string("renamed_gain");
	studio::ImageGraphHistory history;
	const bool renamed = studio::ApplyImageGraphSourceDynamicInput(
		document, history, host, 3, "hlsl", name, request, error, [](auto &, auto &) { return true; }, &inputs
	);
	INFO(error.Message);
	REQUIRE(renamed);
	CHECK(document.Keyframes == keys);
	CHECK(producer.Calls == 1);
	CHECK(document.Links == Graph().Links);
	CHECK(history.CanUndo());
	REQUIRE(history.Undo(document));
	CHECK(document == Graph());
}

TEST_CASE("HLSL shape reset retires only its staged cached source animator effect", "[studio][hlsl_inputs]") {
	auto document = Graph();
	CountedLua producer;
	EvaluationRequest request;
	request.Tick = 5;
	request.HostProvider = &producer;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(host.Prepare(document, plan, 3, request, error));
	const Value editedValue = 8.5;
	GroupRefreshEvent event;
	event.NodeId = "hlsl";
	event.EditedPort = "argument_value_0";
	event.LocalValue = &editedValue;
	event.LocalAnimated = true;
	event.At = request;
	GroupReplayState editedReplay;
	REQUIRE(
		ReplayGroupAnimatorEdits(document, {&event, 1}, host.Replay, 3, editedReplay, error) == Status::Ok
	);
	Document projected;
	REQUIRE(ProjectGroupReplay(document, editedReplay, 3, projected, error) == Status::Ok);
	document = std::move(projected);
	host.Replay = std::move(editedReplay);
	REQUIRE_FALSE(host.Replay.SharedSubtypes().empty());
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	request.GroupReplay = &host.Replay;
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(document, plan, "hlsl", request, inputs, error) == Status::Ok);
	REQUIRE(producer.Calls == 1);
	const auto original = document;
	const auto retained = host.Replay.RetainedBytes();
	auto selector = document.Nodes.back().DynamicInputs[1];
	selector.Default = EnumValue{2};
	studio::ImageGraphHistory refused(0), history;
	const auto edit = [&](auto &selectedHistory) {
		return studio::ApplyImageGraphSourceDynamicInput(
			document,
			selectedHistory,
			host,
			3,
			"hlsl",
			selector,
			request,
			error,
			[](auto &, auto &) { return true; },
			&inputs
		);
	};
	CHECK_FALSE(edit(refused));
	CHECK(document == original);
	CHECK(host.Replay.RetainedBytes() == retained);
	CHECK_FALSE(host.Replay.SharedSubtypes().empty());
	const bool changed = edit(history);
	INFO(error.Message);
	REQUIRE(changed);
	CHECK(producer.Calls == 1);
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(
		std::get<ArrayValue>(document.Keyframes.front().Data).Elements == std::vector<ElementValue>{0.0, 0.0}
	);
	const auto editedStatus = Compile(document, plan, error);
	INFO(error.NodeId);
	INFO(error.Port);
	INFO(error.Message);
	REQUIRE(editedStatus == Status::Ok);
	REQUIRE(history.Undo(document));
	CHECK(document == original);
}
