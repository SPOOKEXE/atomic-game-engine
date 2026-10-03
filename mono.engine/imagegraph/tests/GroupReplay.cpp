#include "../src/TimelineOverrides.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraph/SourceModeTransition.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.group_replay")
using namespace engine::imagegraph;
namespace {
	GroupRefreshEvent Event(std::string nodeId, GroupRefreshReason reason = GroupRefreshReason::Load) {
		GroupRefreshEvent event;
		event.NodeId = std::move(nodeId);
		event.Reason = reason;
		return event;
	}
	Document Boundary(double raw = .5) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{2}},
			  {"subtype", EnumValue{0}},
			  {"vector_size", EnumValue{0}},
			  {"parent_value", raw}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		Group group{"group", "Group"};
		group.Ports = {
			{"input", "input/parent", PortDirection::Input, "input"},
			{"output", "output/parent", PortDirection::Output, "output"}
		};
		document.Groups.push_back(std::move(group));
		document.Junctions = {
			{"input/parent", "group", ValueType::Any, raw},
			{"output/parent", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {
			{"input/parent", "value", "input", "parent_value"},
			{"input", "value", "output", "value"},
			{"output", "value", "output/parent", "value"}
		};
		document.Outputs = {{"result", "output", "value"}};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		{
			const auto status = Compile(document, plan, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		return plan;
	}
	EvaluatedValue Sample(
		const Document &document,
		const Plan &plan,
		const GroupReplayState &state,
		uint64_t tick,
		uint64_t revision = 1
	) {
		EvaluationRequest request;
		request.Tick = tick;
		request.GroupReplay = &state;
		request.GroupAuthoringRevision = revision;
		EvaluatedValue result;
		Diagnostic diagnostic;
		{
			const auto status = EvaluateValue(document, plan, "result", request, result, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
		}
		return result;
	}
} // namespace
TEST_CASE(
	"Group Trigger button records exact frame pulses without a reset seed key",
	"[imagegraph][groups][group_replay][trigger]"
) {
	auto document = Boundary(false);
	document.Links.erase(document.Links.begin());
	document.Nodes.front().Values[0].Data = EnumValue{19};
	document.Nodes.front().SourceAnimatedInputs = {"parent_value"};
	Diagnostic diagnostic;
	GroupReplayState empty, loaded, edited;
	const GroupBootstrapTarget order[] = {{"input"}};
	EvaluationRequest clock;
	REQUIRE(
		ReplayGroupBootstrap(document, Checked(document), order, clock, empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	Value pressed = true;
	auto event = Event("input", GroupRefreshReason::ParentEdit);
	event.EditedPort = "parent_value";
	event.LocalValue = &pressed;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	REQUIRE(
		ReplayGroupRefresh(document, Checked(document), {&event, 1}, loaded, 1, edited, diagnostic) ==
		Status::Ok
	);
	REQUIRE(edited.Find("input")->ParentKeys.size() == 1);
	CHECK(edited.Find("input")->ParentKeys[0].Tick == 5);
	for (const uint64_t tick : {0, 4, 5, 6, 5}) {
		const auto result = Sample(document, Checked(document), edited, tick);
		CHECK(std::get<bool>(result.Data) == (tick == 5));
	}
	Document projected;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
	projected.Keyframes[0].Data = false;
	GroupReplayState restored;
	REQUIRE(
		RestoreGroupDeclarations(
			projected, Checked(projected), order, clock, empty, 2, restored, diagnostic
		) == Status::Ok
	);
	CHECK(std::get<bool>(Sample(projected, Checked(projected), restored, 5, 2).Data));
	Document disabled, enabled;
	REQUIRE(
		ToggleSourceInputMode(
			projected, restored, 2, {"input", "parent_value", false, {5, 0, false}}, disabled, diagnostic
		) == Status::Ok
	);
	REQUIRE(disabled.Keyframes.size() == 1);
	CHECK(GetFrameTime(disabled.Keyframes[0]) == FrameTime{});
	CHECK(disabled.Keyframes[0].Data == Value{false});
	GroupReplayState disabledState;
	REQUIRE(
		RestoreGroupDeclarations(
			disabled, Checked(disabled), order, clock, empty, 3, disabledState, diagnostic
		) == Status::Ok
	);
	CHECK_FALSE(std::get<bool>(Sample(disabled, Checked(disabled), disabledState, 0, 3).Data));
	REQUIRE(
		ToggleSourceInputMode(
			disabled, disabledState, 3, {"input", "parent_value", true, {7, 0, false}}, enabled, diagnostic
		) == Status::Ok
	);
	REQUIRE(enabled.Keyframes.size() == 1);
	CHECK(GetFrameTime(enabled.Keyframes[0]) == FrameTime{7, 0, false});
}
TEST_CASE(
	"Ordered group callbacks change declarations while timeline sampling "
	"does not",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary();
	document.Keyframes = {
		{"input", "input_type", 0, EnumValue{2}, "step"}, {"input", "input_type", 10, EnumValue{1}, "step"}
	};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	const auto plan = Checked(restored);
	const auto before = restored;
	GroupReplayState empty, loaded, edited;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(restored, plan, {&load, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	REQUIRE(loaded.Find("input"));
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	const auto sampled = Sample(restored, plan, loaded, 10);
	REQUIRE(sampled.Domain);
	CHECK(sampled.Domain->Kind == SourceSocketKind::Boolean);
	CHECK(std::get<double>(sampled.Data) == .5);
	auto edit = Event("input", GroupRefreshReason::Edit);
	edit.At.Tick = 10;
	REQUIRE(ReplayGroupRefresh(restored, plan, {&edit, 1}, loaded, 1, edited, diagnostic) == Status::Ok);
	CHECK(edited.Find("input")->Domain.Kind == SourceSocketKind::Float);
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	CHECK(restored == before);
	REQUIRE(Sample(restored, plan, edited, 0).Domain);
	CHECK(Sample(restored, plan, edited, 0).Domain->Kind == SourceSocketKind::Float);
}
TEST_CASE(
	"Group refresh replaces a local animator and retains last good state "
	"on refusal",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary(7);
	document.Links.erase(document.Links.begin());
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{1};
	document.Keyframes = {
		{"input", "parent_value", 0, 7.0, "linear"}, {"input", "parent_value", 10, 9.0, "linear"}
	};
	auto plan = Checked(document);
	Diagnostic diagnostic;
	GroupReplayState empty, state;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&load, 1}, empty, 1, state, diagnostic) == Status::Ok);
	REQUIRE(state.Find("input")->ParentReset);
	const auto value = Sample(document, plan, state, 5);
	REQUIRE(std::holds_alternative<ArrayValue>(value.Data));
	const auto &elements = std::get<ArrayValue>(value.Data).Elements;
	REQUIRE(elements.size() == 2);
	CHECK(std::get<double>(elements[0]) == 0);
	CHECK(std::get<double>(elements[1]) == 0);
	const auto retained = state.RetainedBytes();
	REQUIRE(retained > 1);
	CHECK(
		ReplayGroupRefresh(document, plan, {&load, 1}, state, 1, state, diagnostic, retained - 1) ==
		Status::LimitExceeded
	);
	CHECK(state.RetainedBytes() == retained);
	CHECK(Sample(document, plan, state, 5).Data == value.Data);
	auto stale = Event("missing");
	CHECK(
		ReplayGroupRefresh(document, plan, {&stale, 1}, state, 1, state, diagnostic) == Status::InvalidGroup
	);
	CHECK(Sample(document, plan, state, 5).Data == value.Data);
	EvaluationRequest request;
	request.GroupReplay = &state;
	request.GroupAuthoringRevision = 2;
	EvaluatedValue previous = value;
	CHECK(EvaluateValue(document, plan, "result", request, previous, diagnostic) == Status::InvalidValue);
	CHECK(previous == value);
}
TEST_CASE(
	"A local edit after a refresh never revives the destroyed original "
	"animator",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary(7);
	document.Links.erase(document.Links.begin());
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{1};
	document.Keyframes = {
		{"input", "parent_value", 0, 7.0, "linear"}, {"input", "parent_value", 10, 99.0, "linear"}
	};
	const auto plan = Checked(document);
	const auto before = document;
	Diagnostic diagnostic;
	GroupReplayState empty, loaded, edited;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&load, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	ArrayValue local;
	local.ElementType = ValueType::Scalar;
	local.Elements = {2.0, 4.0};
	const Value value = local;
	auto edit = Event("input", GroupRefreshReason::ParentEdit);
	edit.At.Tick = 5;
	edit.LocalValue = &value;
	edit.LocalAnimated = true;
	REQUIRE(ReplayGroupRefresh(document, plan, {&edit, 1}, loaded, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.Find("input"));
	const auto &entry = *edited.Find("input");
	CHECK_FALSE(entry.ParentReset);
	REQUIRE(entry.ParentKeys.size() == 2);
	CHECK(entry.ParentKeys[0].Tick == 0);
	CHECK(entry.ParentKeys[1].Tick == 5);
	CHECK(entry.ParentKeys[1].Kind == KeyframeKind::Normal);
	CHECK_FALSE(entry.ParentKeys[1].SourceDriver);
	CHECK(std::get<ArrayValue>(Sample(document, plan, edited, 10).Data) == local);
	CHECK(document == before);
	std::vector<AuthoredValue> resolved;
	EvaluationRequest request;
	request.Tick = 10;
	request.GroupReplay = &edited;
	request.GroupAuthoringRevision = 1;
	REQUIRE(
		ResolveNodeValues(document, plan, "result", "input", request, resolved, diagnostic) == Status::Ok
	);
	const auto parent = std::find_if(resolved.begin(), resolved.end(), [](const auto &row) {
		return row.Port == "parent_value";
	});
	REQUIRE(parent != resolved.end());
	CHECK(parent->Data == value);
}
TEST_CASE(
	"Refresh reset preserves exact-time subtype metadata and honors key "
	"replacement preference",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary();
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{1};
	Keyframe key{"input", "subtype", 3, 1.0, "source"};
	key.Kind = KeyframeKind::Adder;
	key.Ease = KeyframeEase{};
	key.SourceDriver = KeyframeLinearDriver{2};
	document.Keyframes = {key};
	document.Tracks = {{"input", "subtype", "hold"}};
	const auto plan = Checked(document);
	Diagnostic diagnostic;
	GroupReplayState empty, loaded, edited;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&load, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	// A type edit requires the source subtype reset at the exact signed authoring
	// clock.
	document.Nodes.front().Values[0].Data = EnumValue{0};
	const auto changedPlan = Checked(document);
	auto edit = Event("input", GroupRefreshReason::Edit);
	edit.At.Tick = 3;
	edit.SubtypeAnimator = GroupSubtypeAnimator::Animated;
	REQUIRE(
		ReplayGroupRefresh(document, changedPlan, {&edit, 1}, loaded, 2, edited, diagnostic) == Status::Ok
	);
	REQUIRE(edited.Find("input")->SubtypeKeys.size() == 1);
	auto expected = key;
	expected.Data = EnumValue{0};
	CHECK(edited.Find("input")->SubtypeKeys.front() == expected);
	GroupReplayState refusedOverride;
	edit.ReplaceExistingKey = false;
	REQUIRE(
		ReplayGroupRefresh(document, changedPlan, {&edit, 1}, loaded, 2, refusedOverride, diagnostic) ==
		Status::Ok
	);
	REQUIRE(refusedOverride.Find("input")->SubtypeKeys.empty());
	CHECK_FALSE(refusedOverride.Find("input")->SubtypeStatic);
}
TEST_CASE(
	"Distinct replay destination residency is admitted before replacement",
	"[imagegraph][groups][group_replay]"
) {
	const auto document = Boundary();
	const auto plan = Checked(document);
	Diagnostic diagnostic;
	GroupReplayState empty, previous, destination;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&load, 1}, empty, 1, previous, diagnostic) == Status::Ok);
	ArrayValue large;
	large.ElementType = ValueType::Scalar;
	large.Elements.assign(Limits::MaximumArrayElements, ElementValue{.25});
	const Value editedValue = std::move(large);
	auto edit = Event("input", GroupRefreshReason::ParentEdit);
	edit.LocalValue = &editedValue;
	REQUIRE(
		ReplayGroupRefresh(document, plan, {&edit, 1}, previous, 1, destination, diagnostic) == Status::Ok
	);
	const uint64_t destinationBytes = destination.RetainedBytes();
	const uint64_t previousBytes = previous.RetainedBytes();
	const auto before = Sample(document, plan, destination, 0);
	// The old destination alone consumes this cap. A small previous state must
	// not hide it.
	CHECK(
		ReplayGroupRefresh(
			document, plan, {&load, 1}, previous, 1, destination, diagnostic, destinationBytes
		) == Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == destinationBytes);
	CHECK(previous.RetainedBytes() == previousBytes);
	CHECK(Sample(document, plan, destination, 0) == before);
	uint64_t lower = destinationBytes, upper = Limits::MaximumEvaluationBytes;
	while (upper - lower > 1) {
		const uint64_t cap = lower + (upper - lower) / 2;
		const Status status =
			ReplayGroupRefresh(document, plan, {&load, 1}, previous, 1, destination, diagnostic, cap);
		if (status == Status::Ok) {
			upper = cap;
			REQUIRE(
				ReplayGroupRefresh(document, plan, {&edit, 1}, previous, 1, destination, diagnostic) ==
				Status::Ok
			);
		} else {
			REQUIRE(status == Status::LimitExceeded);
			lower = cap;
		}
		REQUIRE(destination.RetainedBytes() == destinationBytes);
	}
	CHECK(
		ReplayGroupRefresh(document, plan, {&load, 1}, previous, 1, destination, diagnostic, upper - 1) ==
		Status::LimitExceeded
	);
	CHECK(Sample(document, plan, destination, 0) == before);
	REQUIRE(
		ReplayGroupRefresh(document, plan, {&load, 1}, previous, 1, destination, diagnostic, upper) ==
		Status::Ok
	);
	CHECK(std::get<double>(Sample(document, plan, destination, 0).Data) == .5);
	CHECK(previous.RetainedBytes() == previousBytes);
}
TEST_CASE(
	"An animated parent callback with a simultaneous declaration change "
	"keeps its input alive",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary(7);
	document.Links.erase(document.Links.begin());
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{1};
	document.Keyframes = {
		{"input", "input_type", 0, EnumValue{1}, "step"}, {"input", "input_type", 10, EnumValue{0}, "step"}
	};
	const auto plan = Checked(document);
	const auto before = document;
	Diagnostic diagnostic;
	GroupReplayState empty, loaded, edited;
	auto load = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&load, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	REQUIRE(loaded.Find("input")->ParentReset);
	REQUIRE(std::holds_alternative<ArrayValue>(*loaded.Find("input")->ParentReset));
	ArrayValue array;
	array.ElementType = ValueType::Scalar;
	array.Elements = {2.0, 4.0};
	const Value local = array;
	auto edit = Event("input", GroupRefreshReason::ParentEdit);
	edit.At.Tick = 10;
	edit.LocalAnimated = true;
	edit.LocalValue = &local;
	REQUIRE(edit.EditedPort.empty());
	REQUIRE(ReplayGroupRefresh(document, plan, {&edit, 1}, loaded, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.Find("input"));
	const auto &entry = *edited.Find("input");
	CHECK(entry.Domain.Kind == SourceSocketKind::Integer);
	CHECK(entry.Subtype == 0);
	REQUIRE(entry.ParentReset);
	// Default numeric display destroys the newly edited array animator and starts
	// scalar zero.
	CHECK(std::get<double>(*entry.ParentReset) == 0);
	CHECK(entry.ParentKeys.empty());
	CHECK(std::get<double>(Sample(document, plan, edited, 10).Data) == 0);
	CHECK(document == before);
	CHECK(std::holds_alternative<ArrayValue>(*loaded.Find("input")->ParentReset));
}
TEST_CASE(
	"Bootstrap preserves explicit callback order and signed caller clock",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary();
	auto second = document.Nodes.front();
	second.Id = "second";
	second.Values[0].Data = EnumValue{1};
	document.Nodes.push_back(second);
	document.Groups.front().Ports.push_back({"second", "second/parent", PortDirection::Input, "second"});
	document.Junctions.push_back({"second/parent", "group", ValueType::Any, .5});
	document.Links.push_back({"second/parent", "value", "second", "parent_value"});
	document.Keyframes = {
		{"input", "input_type", 1, EnumValue{2}, "step"}, {"input", "input_type", 1, EnumValue{1}, "step"}
	};
	document.Keyframes[0].NegativeFrame = true;
	document.Keyframes[0].Subframe = .5;
	document.Keyframes[1].Subframe = .25;
	const auto plan = Checked(document);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.Subframe = .5;
	clock.NegativeFrame = true;
	const GroupBootstrapTarget order[] = {
		{"second", GroupSubtypeAnimator::Animated}, {"input", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, loaded;
	Diagnostic diagnostic;
	const auto status = ReplayGroupBootstrap(document, plan, order, clock, empty, 1, loaded, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	REQUIRE(loaded.Entries().size() == 2);
	CHECK(loaded.Entries()[0].NodeId == "second");
	CHECK(loaded.Entries()[1].NodeId == "input");
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	CHECK(loaded.Find("second")->Domain.Kind == SourceSocketKind::Float);
	CHECK(loaded.Find("second")->SubtypeKeys.empty());
	clock.NegativeFrame = false;
	clock.Subframe = .25;
	GroupReplayState later;
	REQUIRE(ReplayGroupBootstrap(document, plan, order, clock, empty, 1, later, diagnostic) == Status::Ok);
	CHECK(later.Find("input")->Domain.Kind == SourceSocketKind::Float);
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	// A populated distinct destination remains intact when callback scratch
	// leaves insufficient room for the old and new replay owners.
	const auto before = loaded.RetainedBytes();
	CHECK(
		ReplayGroupBootstrap(document, plan, order, clock, empty, 1, loaded, diagnostic, before) ==
		Status::LimitExceeded
	);
	CHECK(loaded.RetainedBytes() == before);
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
}

TEST_CASE(
	"Unrelated authored edits rebind cached declarations without "
	"refreshing their sampled type",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary();
	document.Keyframes = {
		{"input", "input_type", 0, EnumValue{2}, "step"}, {"input", "input_type", 10, EnumValue{1}, "step"}
	};
	const auto plan = Checked(document);
	GroupReplayState empty, loaded, rebound;
	Diagnostic diagnostic;
	auto event = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&event, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	REQUIRE(loaded.Find("input"));
	CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	document.Nodes.front().Position = {20, 30};
	const auto reboundStatus = RebindGroupReplay(document, loaded, 2, rebound, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(reboundStatus == Status::Ok);
	CHECK(rebound.AuthoringRevision() == 2);
	CHECK(rebound.Find("input")->Domain.Kind == SourceSocketKind::Boolean);
	CHECK(loaded.AuthoringRevision() == 1);
	const auto bytes = rebound.RetainedBytes();
	CHECK(RebindGroupReplay(document, loaded, 3, rebound, diagnostic, bytes) == Status::LimitExceeded);
	CHECK(rebound.AuthoringRevision() == 2);
	CHECK(rebound.RetainedBytes() == bytes);
	std::erase_if(document.Nodes, [](const auto &node) { return node.Id == "input"; });
	REQUIRE(RebindGroupReplay(document, rebound, 3, rebound, diagnostic) == Status::Ok);
	CHECK(rebound.Entries().empty());
	CHECK(rebound.AuthoringRevision() == 3);
	CHECK(rebound.RetainedBytes() < bytes);
	CHECK(loaded.Find("input"));
}

namespace {
	Document AliasedBoundaries() {
		auto document = Boundary();
		document.Nodes.front().Values[0].Data = EnumValue{1};
		for (const auto &id : {std::string("alpha"), std::string("beta")}) {
			auto node = document.Nodes.front();
			node.Id = id;
			node.Values[1].Data = EnumValue{id == "alpha" ? 4 : 6};
			document.Nodes.push_back(std::move(node));
			document.Groups.front().Ports.push_back({id, id + "/parent", PortDirection::Input, id});
			document.Junctions.push_back({id + "/parent", "group", ValueType::Any, .5});
			document.Links.push_back({id + "/parent", "value", id, "parent_value"});
		}
		Keyframe key;
		key.NodeId = "input";
		key.Port = "subtype";
		key.Data = EnumValue{2};
		key.Interpolation = "source";
		key.Ease = KeyframeEase{};
		key.Kind = KeyframeKind::Adder;
		key.SourceDriver = KeyframeLinearDriver{0};
		key.Tick = 1;
		key.NegativeFrame = true;
		key.Subframe = .5;
		document.Keyframes.push_back(key);
		document.Tracks.push_back({"input", "subtype", "hold", -1});
		return document;
	}
	EvaluationRequest BindingClock(const GroupReplayState *state = nullptr, uint64_t revision = 1) {
		EvaluationRequest clock;
		clock.Tick = 1;
		clock.NegativeFrame = true;
		clock.Subframe = .5;
		clock.GroupReplay = state;
		clock.GroupAuthoringRevision = revision;
		return clock;
	}
	double ResolvedSubtype(
		const Document &document,
		const Plan &plan,
		std::string_view id,
		const GroupReplayState &state,
		uint64_t revision = 1
	) {
		EvaluationSnapshot snapshot;
		Diagnostic diagnostic;
		const auto status =
			EvaluateNodeInputs(document, plan, id, BindingClock(&state, revision), snapshot, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "subtype";
			});
		REQUIRE(value != snapshot.Values().end());
		if (const auto *choice = std::get_if<EnumValue>(&value->Data))
			return static_cast<double>(choice->Value);
		REQUIRE(std::holds_alternative<double>(value->Data));
		return std::get<double>(value->Data);
	}
}
TEST_CASE(
	"Delayed binding shares animator writes while declarations stay target-local",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Animated},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, bootstrapped, bound, refreshed;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(
			local, Checked(local), order, BindingClock(), empty, 1, bootstrapped, diagnostic
		) == Status::Ok
	);
	CHECK(bootstrapped.Find("alpha")->Subtype == 4);
	CHECK(bootstrapped.Find("beta")->Subtype == 6);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	authored.Nodes[3].InstanceOverrides = {"subtype"};
	const auto unchanged = authored;
	const auto plan = Checked(authored);
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Animated}
	};
	REQUIRE(BindGroupReplay(authored, bindings, bootstrapped, 1, bound, diagnostic) == Status::Ok);
	CHECK(bound.InstancesBound());
	CHECK(bound.Find("alpha")->Subtype == 4);
	CHECK(bound.Find("beta")->Subtype == 6);
	GroupReplayState reboundBindings;
	auto replacementBindings = std::to_array(bindings);
	replacementBindings[0].Getter = GroupSubtypeAnimator::Static;
	REQUIRE(
		BindGroupReplay(authored, replacementBindings, bound, 1, reboundBindings, diagnostic) == Status::Ok
	);
	CHECK(reboundBindings.Binding("alpha")->Getter == GroupSubtypeAnimator::Static);
	CHECK(reboundBindings.Find("alpha")->Domain == bound.Find("alpha")->Domain);
	CHECK(reboundBindings.Find("alpha")->Subtype == 4);
	CHECK(bound.Binding("alpha")->Getter == GroupSubtypeAnimator::Animated);
	const auto retainedBindings = reboundBindings.RetainedBytes();
	CHECK(
		BindGroupReplay(authored, bindings, bound, 1, reboundBindings, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(reboundBindings.RetainedBytes() == retainedBindings);
	CHECK(reboundBindings.Binding("alpha")->Getter == GroupSubtypeAnimator::Static);
	CHECK(ResolvedSubtype(authored, plan, "alpha", bound) == 2);
	CHECK(ResolvedSubtype(authored, plan, "beta", bound) == 2);
	Value integerType = EnumValue{0};
	auto event = Event("alpha", GroupRefreshReason::Edit);
	event.At = BindingClock();
	event.EditedPort = "input_type";
	event.LocalValue = &integerType;
	event.SubtypeAnimator = GroupSubtypeAnimator::Static;
	REQUIRE(ReplayGroupRefresh(authored, plan, {&event, 1}, bound, 1, refreshed, diagnostic) == Status::Ok);
	REQUIRE(refreshed.SharedSubtype("input"));
	REQUIRE(refreshed.SharedSubtype("input")->Keys.size() == 1);
	auto expected = authored.Keyframes.front();
	expected.Data = EnumValue{0};
	CHECK(refreshed.SharedSubtype("input")->Keys.front() == expected);
	CHECK_FALSE(refreshed.SharedSubtype("input")->Fixed.has_value());
	CHECK(refreshed.Find("alpha")->Subtype == 0);
	CHECK(refreshed.Find("beta")->Subtype == 6);
	CHECK(refreshed.Find("input")->Subtype == 2);
	CHECK(ResolvedSubtype(authored, plan, "alpha", refreshed) == 0);
	CHECK(ResolvedSubtype(authored, plan, "beta", refreshed) == 0);
	CHECK(authored == unchanged);
	CHECK(bound.SharedSubtypes().empty());
	const auto oldShared = refreshed.SharedSubtype("input")->Keys;
	const auto oldBytes = refreshed.RetainedBytes();
	CHECK(
		ReplayGroupRefresh(
			authored, plan, {&event, 1}, bound, 1, refreshed, diagnostic, bound.RetainedBytes() + oldBytes
		) == Status::LimitExceeded
	);
	CHECK(refreshed.RetainedBytes() == oldBytes);
	CHECK(refreshed.SharedSubtype("input")->Keys == oldShared);
	CHECK(refreshed.Find("beta")->Subtype == 6);
	GroupReplayState rebound;
	auto unrelated = authored;
	unrelated.Groups.front().Name = "renamed";
	REQUIRE(RebindGroupReplay(unrelated, refreshed, 2, rebound, diagnostic) == Status::Ok);
	CHECK(rebound.InstancesBound());
	CHECK(rebound.Find("beta")->Subtype == 6);
	CHECK(ResolvedSubtype(unrelated, Checked(unrelated), "alpha", rebound, 2) == 0);
	CHECK(ResolvedSubtype(unrelated, Checked(unrelated), "beta", rebound, 2) == 0);
}
TEST_CASE(
	"Bound reset creates source default keys and linked getters retain priority",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Animated},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, bootstrapped, bound, reset;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(
			local, Checked(local), order, BindingClock(), empty, 1, bootstrapped, diagnostic
		) == Status::Ok
	);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	authored.Nodes.push_back({"selector", "pc.number", "group", {}, {{"value", 2.0}}});
	for (const auto &id : {"input", "alpha", "beta"})
		authored.Links.push_back({"selector", "number", id, "subtype"});
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated},
		{"beta", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated}
	};
	REQUIRE(BindGroupReplay(authored, bindings, bootstrapped, 1, bound, diagnostic) == Status::Ok);
	Value integerType = EnumValue{0};
	auto event = Event("alpha", GroupRefreshReason::Edit);
	event.EditedPort = "input_type";
	event.LocalValue = &integerType;
	event.At.Tick = 2;
	event.At.NegativeFrame = true;
	event.At.Subframe = .25;
	const auto plan = Checked(authored);
	REQUIRE(ReplayGroupRefresh(authored, plan, {&event, 1}, bound, 1, reset, diagnostic) == Status::Ok);
	REQUIRE(reset.SharedSubtype("input"));
	REQUIRE(reset.SharedSubtype("input")->Keys.size() == 2);
	const auto &newKey = reset.SharedSubtype("input")->Keys.front();
	CHECK(newKey.Tick == 2);
	CHECK(newKey.NegativeFrame);
	CHECK(newKey.Subframe == .25);
	CHECK(newKey.Kind == KeyframeKind::Normal);
	CHECK(newKey.Ease == std::optional<KeyframeEase>{KeyframeEase{}});
	CHECK_FALSE(newKey.SourceDriver.has_value());
	CHECK(reset.SharedSubtype("input")->Keys.back() == authored.Keyframes.front());
	CHECK(reset.Find("alpha")->Subtype == 0);
	CHECK(ResolvedSubtype(authored, plan, "alpha", reset) == 2);
	CHECK(ResolvedSubtype(authored, plan, "beta", reset) == 2);
	CHECK(reset.Find("beta")->Subtype == 6);
}
TEST_CASE(
	"Binding validates original owner and preserves populated destinations on refusal",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Animated},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, localState, destination;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(
			local, Checked(local), order, BindingClock(), empty, 1, localState, diagnostic
		) == Status::Ok
	);
	REQUIRE(localState.Find("input"));
	const int64_t localInputSubtype = localState.Find("input")->Subtype;
	REQUIRE(RebindGroupReplay(local, localState, 1, destination, diagnostic) == Status::Ok);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated},
		{"beta", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated}
	};
	const auto retained = destination.RetainedBytes();
	const auto documentBytes = DocumentRetainedPayloadBytes(authored);
	REQUIRE(documentBytes);
	GroupReplayState admitted;
	REQUIRE(BindGroupReplay(authored, bindings, localState, 1, admitted, diagnostic) == Status::Ok);
	const uint64_t threshold = *documentBytes + admitted.RetainedBytes() + localState.RetainedBytes() +
							   retained + authored.Nodes.size() * sizeof(const Node *) +
							   std::size(bindings) * sizeof(const GroupSubtypeBinding *);
	CHECK(
		BindGroupReplay(authored, bindings, localState, 1, destination, diagnostic, threshold - 1) ==
		Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == retained);
	CHECK_FALSE(destination.InstancesBound());
	REQUIRE(
		BindGroupReplay(authored, bindings, localState, 1, destination, diagnostic, threshold) == Status::Ok
	);
	CHECK(destination.InstancesBound());
	REQUIRE(RebindGroupReplay(local, localState, 1, destination, diagnostic) == Status::Ok);
	CHECK(
		BindGroupReplay(authored, bindings, localState, 1, destination, diagnostic, retained) ==
		Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == retained);
	CHECK_FALSE(destination.InstancesBound());
	CHECK(destination.Find("beta")->Subtype == 6);
	const auto localBytes = localState.RetainedBytes();
	auto oversizedNodes = authored;
	oversizedNodes.Nodes.resize(Limits::MaximumNodes + 1);
	CHECK(
		BindGroupReplay(oversizedNodes, bindings, localState, 1, destination, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(destination.RetainedBytes() == retained);
	CHECK_FALSE(destination.InstancesBound());
	CHECK(destination.Find("beta")->Subtype == 6);
	CHECK(localState.RetainedBytes() == localBytes);
	CHECK(localState.AuthoringRevision() == 1);
	CHECK_FALSE(localState.InstancesBound());
	CHECK(localState.Find("input")->Subtype == localInputSubtype);
	bindings[1].OwnerId = "alpha";
	CHECK(
		BindGroupReplay(authored, bindings, localState, 1, destination, diagnostic) == Status::InvalidGroup
	);
	CHECK_FALSE(destination.InstancesBound());
	CHECK(destination.Find("alpha")->Subtype == 4);
	CHECK_FALSE(localState.InstancesBound());
}

TEST_CASE(
	"Static shared writers preserve the first key and leave later keys intact",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	auto second = local.Keyframes.front();
	second.Tick = 9;
	second.NegativeFrame = false;
	second.Data = EnumValue{7};
	local.Keyframes.push_back(second);
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Static},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, loaded, bound, edited;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(local, Checked(local), order, BindingClock(), empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Static},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static}
	};
	auto plan = Checked(authored);
	REQUIRE(BindGroupReplay(authored, bindings, loaded, 1, bound, diagnostic) == Status::Ok);
	CHECK(ResolvedSubtype(authored, plan, "alpha", bound) == 2);
	CHECK(ResolvedSubtype(authored, plan, "beta", bound) == 2);
	Value integer = EnumValue{0};
	auto event = Event("alpha", GroupRefreshReason::Edit);
	event.At = BindingClock();
	event.EditedPort = "input_type";
	event.LocalValue = &integer;
	REQUIRE(ReplayGroupRefresh(authored, plan, {&event, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.SharedSubtype("input"));
	REQUIRE(edited.SharedSubtype("input")->Keys.size() == 2);
	auto expected = authored.Keyframes.front();
	expected.Data = EnumValue{0};
	CHECK(edited.SharedSubtype("input")->Keys.front() == expected);
	CHECK(edited.SharedSubtype("input")->Keys.back() == second);
	CHECK_FALSE(edited.SharedSubtype("input")->Fixed);
	CHECK(ResolvedSubtype(authored, plan, "alpha", edited) == 0);
	CHECK(ResolvedSubtype(authored, plan, "beta", edited) == 0);
}
TEST_CASE(
	"Owned replay names use sized construction across the SSO growth interval",
	"[imagegraph][groups][group_replay]"
) {
	for (size_t length = 16; length <= 29; ++length) {
		INFO(length);
		auto local = AliasedBoundaries();
		const std::string owner(length, 'o'), target(length, 't');
		auto rename = [&](std::string &id) {
			if (id == "input")
				id = owner;
			else if (id == "alpha")
				id = target;
		};
		for (auto &node : local.Nodes)
			rename(node.Id);
		for (auto &key : local.Keyframes)
			rename(key.NodeId);
		for (auto &track : local.Tracks)
			rename(track.NodeId);
		for (auto &link : local.Links) {
			rename(link.FromNode);
			rename(link.ToNode);
		}
		for (auto &port : local.Groups.front().Ports)
			rename(port.ControlNodeId);
		const GroupBootstrapTarget order[] = {
			{owner, GroupSubtypeAnimator::Animated},
			{target, GroupSubtypeAnimator::Static},
			{"beta", GroupSubtypeAnimator::Static}
		};
		GroupReplayState empty, loaded, bound, edited;
		Diagnostic diagnostic;
		REQUIRE(
			ReplayGroupBootstrap(
				local, Checked(local), order, BindingClock(), empty, 1, loaded, diagnostic
			) == Status::Ok
		);
		auto authored = local;
		authored.Nodes[2].InstanceBase = owner;
		authored.Nodes[3].InstanceBase = owner;
		const GroupSubtypeBinding bindings[] = {
			{target, owner, GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated},
			{"beta", owner, GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Animated}
		};
		auto plan = Checked(authored);
		REQUIRE(BindGroupReplay(authored, bindings, loaded, 1, bound, diagnostic) == Status::Ok);
		CHECK(bound.Binding(target)->NodeId.capacity() == std::string(target).capacity());
		CHECK(bound.Binding(target)->OwnerId.capacity() == std::string(owner).capacity());
		Value integer = EnumValue{0};
		auto event = Event(target, GroupRefreshReason::Edit);
		event.At = BindingClock();
		event.EditedPort = "input_type";
		event.LocalValue = &integer;
		REQUIRE(ReplayGroupRefresh(authored, plan, {&event, 1}, bound, 1, edited, diagnostic) == Status::Ok);
		REQUIRE(edited.SharedSubtype(owner));
		CHECK(edited.SharedSubtype(owner)->NodeId.capacity() == std::string(owner).capacity());
		CHECK(edited.SharedSubtype(owner)->Keys.front().NodeId.capacity() == std::string(owner).capacity());
	}
}
TEST_CASE(
	"A lone shared key runs its driver for an animated getter with a static writer",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	auto &key = local.Keyframes.front();
	key.Data = 2.0;
	key.Tick = 0;
	key.Subframe = 0;
	key.NegativeFrame = false;
	key.Kind = KeyframeKind::Normal;
	key.SourceDriver = KeyframeLinearDriver{2};
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Static},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, loaded, bound;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(local, Checked(local), order, BindingClock(), empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Static},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static}
	};
	auto plan = Checked(authored);
	REQUIRE(BindGroupReplay(authored, bindings, loaded, 1, bound, diagnostic) == Status::Ok);
	EvaluationRequest clock;
	clock.Tick = 3;
	clock.GroupReplay = &bound;
	clock.GroupAuthoringRevision = 1;
	for (const auto &id : {"input", "alpha", "beta"}) {
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(authored, plan, id, clock, snapshot, diagnostic) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "subtype";
			});
		REQUIRE(value != snapshot.Values().end());
		REQUIRE(std::holds_alternative<double>(value->Data));
		CHECK(std::get<double>(value->Data) == (std::string_view(id) == "alpha" ? 8.0 : 2.0));
	}
}
TEST_CASE(
	"Authored projection retains local static writer keys and exact edit-time source metadata",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = AliasedBoundaries();
	auto later = authored.Keyframes.front();
	later.Tick = 9;
	later.NegativeFrame = false;
	later.Data = EnumValue{7};
	authored.Keyframes.push_back(later);
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Static},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, loaded, edited;
	Diagnostic diagnostic;
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), empty, 1, loaded, diagnostic
		) == Status::Ok
	);
	CHECK(loaded.SharedSubtypes().empty());
	CHECK_FALSE(loaded.Find("input")->ParentReset);
	Value integer = EnumValue{0};
	auto event = Event("input", GroupRefreshReason::Edit);
	event.At = BindingClock();
	event.EditedPort = "input_type";
	event.LocalValue = &integer;
	event.SubtypeAnimator = GroupSubtypeAnimator::Static;
	REQUIRE(
		ReplayGroupRefresh(authored, Checked(authored), {&event, 1}, loaded, 1, edited, diagnostic) ==
		Status::Ok
	);
	REQUIRE(edited.Find("input")->SubtypeKeys.size() == 2);
	auto expected = authored.Keyframes.front();
	expected.Data = EnumValue{0};
	CHECK(edited.Find("input")->SubtypeKeys.front() == expected);
	CHECK(edited.Find("input")->SubtypeKeys.back() == later);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	REQUIRE(projected.Keyframes.size() == 2);
	CHECK(projected.Keyframes.front() == expected);
	CHECK(projected.Keyframes.back() == later);
	CHECK(projected.Nodes == authored.Nodes);
	CHECK(projected.Groups == authored.Groups);
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	const auto serialized = Write(restored);
	auto clock = BindingClock();
	clock.Tick = 40;
	clock.NegativeFrame = false;
	clock.Subframe = .125;
	GroupReplayState restoredState;
	REQUIRE(
		RestoreGroupDeclarations(
			restored, Checked(restored), order, clock, empty, 2, restoredState, diagnostic
		) == Status::Ok
	);
	Document again;
	REQUIRE(ProjectGroupReplay(restored, restoredState, 2, again, diagnostic) == Status::Ok);
	CHECK(Write(again) == serialized);
}
TEST_CASE(
	"Authored projection writes animated alias keys to the original owner and refuses atomically",
	"[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	const GroupBootstrapTarget order[] = {
		{"input", GroupSubtypeAnimator::Animated},
		{"alpha", GroupSubtypeAnimator::Static},
		{"beta", GroupSubtypeAnimator::Static}
	};
	GroupReplayState empty, loaded, bound, edited;
	Diagnostic diagnostic;
	REQUIRE(
		RestoreGroupDeclarations(
			local, Checked(local), order, BindingClock(), empty, 1, loaded, diagnostic
		) == Status::Ok
	);
	auto authored = local;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Animated}
	};
	REQUIRE(BindGroupReplay(authored, bindings, loaded, 1, bound, diagnostic) == Status::Ok);
	Value integer = EnumValue{0};
	auto event = Event("alpha", GroupRefreshReason::Edit);
	event.At = BindingClock();
	event.At.Tick = 2;
	event.At.Subframe = .25;
	event.EditedPort = "input_type";
	event.LocalValue = &integer;
	REQUIRE(
		ReplayGroupRefresh(authored, Checked(authored), {&event, 1}, bound, 1, edited, diagnostic) ==
		Status::Ok
	);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	REQUIRE(projected.Keyframes.size() == 2);
	const auto created =
		std::find_if(projected.Keyframes.begin(), projected.Keyframes.end(), [](const auto &key) {
			return GetFrameTime(key) == FrameTime{2, .25, true};
		});
	REQUIRE(created != projected.Keyframes.end());
	CHECK(created->NodeId == "input");
	CHECK(created->Kind == KeyframeKind::Normal);
	CHECK_FALSE(created->SourceDriver);
	CHECK(projected.Nodes == authored.Nodes);
	CHECK(
		std::find(projected.Keyframes.begin(), projected.Keyframes.end(), authored.Keyframes.front()) !=
		projected.Keyframes.end()
	);
	const auto beforeText = Write(authored), afterText = Write(projected);
	Document undone, redone;
	REQUIRE(Read(beforeText, undone, diagnostic) == Status::Ok);
	REQUIRE(Read(afterText, redone, diagnostic) == Status::Ok);
	CHECK(undone == authored);
	CHECK(redone == projected);
	CHECK(std::find(redone.Keyframes.begin(), redone.Keyframes.end(), *created) != redone.Keyframes.end());
	// Exact operation admission is discovered with a populated prior destination.
	const auto prior = Boundary(.75);
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		auto output = prior;
		if (ProjectGroupReplay(authored, edited, 1, output, diagnostic, middle) == Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	auto output = prior;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, output, diagnostic, low) == Status::Ok);
	CHECK(output == projected);
	output = prior;
	CHECK(ProjectGroupReplay(authored, edited, 1, output, diagnostic, low - 1) == Status::LimitExceeded);
	CHECK(output == prior);
	CHECK(authored.Nodes[2].InstanceBase == "input");
	CHECK(ProjectGroupReplay(authored, edited, 2, output, diagnostic) == Status::InvalidValue);
	CHECK(output == prior);
	CHECK(ProjectGroupReplay(authored, edited, 1, output, diagnostic, 0) == Status::LimitExceeded);
	CHECK(output == prior);
}

TEST_CASE(
	"Projected replay acceptance drops persisted effects but retains declaration and bindings",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = Boundary();
	authored.Nodes.front().Values[0].Data = EnumValue{1};
	GroupReplayState empty, loaded, edited, committed;
	Diagnostic diagnostic;
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), empty, 1, loaded, diagnostic
		) == Status::Ok
	);
	Value subtype = EnumValue{4};
	auto event = Event("input", GroupRefreshReason::Edit);
	event.EditedPort = "subtype";
	event.LocalValue = &subtype;
	event.At = BindingClock();
	REQUIRE(
		ReplayGroupRefresh(authored, Checked(authored), {&event, 1}, loaded, 1, edited, diagnostic) ==
		Status::Ok
	);
	CHECK(RebindProjectedGroupReplay(authored, edited, 2, committed, diagnostic) == Status::InvalidValue);
	CHECK(committed.AuthoringRevision() == 0);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	REQUIRE(RebindProjectedGroupReplay(projected, edited, 2, committed, diagnostic) == Status::Ok);
	REQUIRE(committed.Find("input"));
	CHECK(committed.Find("input")->Subtype == edited.Find("input")->Subtype);
	CHECK_FALSE(committed.Find("input")->ParentReset);
	CHECK_FALSE(committed.Find("input")->SubtypeStatic);
	CHECK(committed.Find("input")->ParentKeys.empty());
	CHECK(committed.Find("input")->SubtypeKeys.empty());
	Document stable;
	REQUIRE(ProjectGroupReplay(projected, committed, 2, stable, diagnostic) == Status::Ok);
	CHECK(stable == projected);
}

TEST_CASE(
	"Native declaration restore recomputes vector size without creating animator writes",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = Boundary();
	authored.Nodes.front().Values[0].Data = EnumValue{1};
	authored.Nodes.front().Values[1].Data = EnumValue{7};
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	GroupReplayState empty, first, second;
	Diagnostic diagnostic;
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), empty, 1, first, diagnostic
		) == Status::Ok
	);
	CHECK(first.Find("input")->VectorSize == 0);
	authored.Nodes.front().Values[2].Data = EnumValue{2};
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), first, 1, second, diagnostic
		) == Status::Ok
	);
	CHECK(second.Find("input")->VectorSize == 2);
	CHECK_FALSE(second.Find("input")->ParentReset);
	CHECK(second.Find("input")->ParentKeys.empty());
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, second, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected == authored);
}
TEST_CASE(
	"Local static parent writes preserve serialized keys and driver metadata",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = Boundary();
	Keyframe first;
	first.NodeId = "input";
	first.Port = "parent_value";
	first.Data = .5;
	first.Interpolation = "source";
	first.Ease = KeyframeEase{};
	first.Kind = KeyframeKind::Adder;
	first.SourceDriver = KeyframeLinearDriver{0};
	first.Tick = 1;
	first.NegativeFrame = true;
	first.Subframe = .5;
	auto later = first;
	later.Tick = 8;
	later.NegativeFrame = false;
	later.Data = .25;
	authored.Keyframes = {first, later};
	authored.Tracks = {{"input", "parent_value", "hold", -1}};
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	GroupReplayState empty, loaded, edited;
	Diagnostic diagnostic;
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), empty, 1, loaded, diagnostic
		) == Status::Ok
	);
	Value replacement = .75;
	auto event = Event("input", GroupRefreshReason::ParentEdit);
	event.At = BindingClock();
	event.EditedPort = "parent_value";
	event.LocalValue = &replacement;
	event.LocalAnimated = false;
	REQUIRE(
		ReplayGroupRefresh(authored, Checked(authored), {&event, 1}, loaded, 1, edited, diagnostic) ==
		Status::Ok
	);
	auto expected = first;
	expected.Data = replacement;
	REQUIRE(edited.Find("input")->ParentKeys.size() == 2);
	CHECK(edited.Find("input")->ParentKeys.front() == expected);
	CHECK(edited.Find("input")->ParentKeys.back() == later);
	CHECK_FALSE(edited.Find("input")->ParentReset);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Keyframes == std::vector<Keyframe>{expected, later});
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
}
TEST_CASE(
	"Public group rebind rejects over-limit documents before effect matching",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = Boundary();
	const GroupBootstrapTarget order[] = {{"input", GroupSubtypeAnimator::Static}};
	GroupReplayState empty, previous, destination;
	Diagnostic diagnostic;
	REQUIRE(
		RestoreGroupDeclarations(
			authored, Checked(authored), order, BindingClock(), empty, 1, previous, diagnostic
		) == Status::Ok
	);
	REQUIRE(RebindGroupReplay(authored, previous, 2, destination, diagnostic) == Status::Ok);
	const auto priorBytes = destination.RetainedBytes();
	auto oversized = authored;
	oversized.Nodes.resize(Limits::MaximumNodes + 1);
	CHECK(RebindGroupReplay(oversized, previous, 3, destination, diagnostic) == Status::LimitExceeded);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	CHECK(
		RebindProjectedGroupReplay(oversized, previous, 3, destination, diagnostic) == Status::LimitExceeded
	);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	oversized = authored;
	oversized.Keyframes.resize(Limits::MaximumKeyframes + 1);
	CHECK(
		RebindProjectedGroupReplay(oversized, previous, 3, destination, diagnostic) == Status::LimitExceeded
	);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	oversized = authored;
	oversized.Nodes.front().SourceAnimatedInputs.resize(Limits::MaximumArrayElements + 1);
	CHECK(RebindGroupReplay(oversized, previous, 3, destination, diagnostic) == Status::LimitExceeded);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	CHECK(
		RebindProjectedGroupReplay(oversized, previous, 3, destination, diagnostic) == Status::LimitExceeded
	);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	auto invalid = authored;
	invalid.Nodes.front().Values.front().Data = std::numeric_limits<double>::infinity();
	CHECK(RebindProjectedGroupReplay(invalid, previous, 3, destination, diagnostic) == Status::LimitExceeded);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
	const auto docBytes = DocumentRetainedPayloadBytes(authored);
	REQUIRE(docBytes);
	CHECK(
		RebindProjectedGroupReplay(
			authored,
			previous,
			3,
			destination,
			diagnostic,
			*docBytes + previous.RetainedBytes() + destination.RetainedBytes()
		) == Status::LimitExceeded
	);
	CHECK(destination.AuthoringRevision() == 2);
	CHECK(destination.RetainedBytes() == priorBytes);
}

namespace {
	Value ResolvedPort(
		const Document &document,
		const Plan &plan,
		const GroupReplayState &state,
		std::string_view node,
		std::string_view port,
		bool authored = false
	) {
		Diagnostic diagnostic;
		EvaluationRequest request = BindingClock(&state);
		request.Tick = 5;
		request.NegativeFrame = false;
		request.Subframe = 0;
		if (authored) {
			std::vector<AuthoredValue> values;
			const auto selected =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &output) {
					return output.NodeId == node;
				});
			REQUIRE(selected != document.Outputs.end());
			const auto status = ResolveNodeValues(
				document, plan, selected->Id, std::string(node), request, values, diagnostic
			);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
			for (const auto &value : values)
				if (value.Port == port) return value.Data;
		} else {
			EvaluationSnapshot snapshot;
			const auto status = EvaluateNodeInputs(document, plan, node, request, snapshot, diagnostic);
			INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
			REQUIRE(status == Status::Ok);
			for (const auto &value : snapshot.Values())
				if (value.Port == port) return value.Data;
		}
		FAIL("resolved source alias port is absent");
		return 0.0;
	}
}
TEST_CASE(
	"Group child input type and range alias independent owner animators", "[imagegraph][groups][group_replay]"
) {
	auto local = AliasedBoundaries();
	local.Keyframes.clear();
	local.Tracks.clear();
	for (auto &node : local.Nodes)
		if (node.Type == "pc.group_input") node.Values.push_back({"range", Vector2{2, 8}});
	Keyframe first{"input", "input_type", 1, EnumValue{1}, "source", KeyframeEase{}};
	first.NegativeFrame = true;
	first.Subframe = .5;
	first.Kind = KeyframeKind::Adder;
	first.SourceDriver = KeyframeLinearDriver{0};
	auto later = first;
	later.Tick = 9;
	later.NegativeFrame = false;
	later.Data = EnumValue{2};
	auto range = first;
	range.Port = "range";
	range.Kind = KeyframeKind::Normal;
	range.SourceDriver = KeyframeLinearDriver{0};
	range.Data = Vector2{2, 8};
	local.Keyframes = {first, later, range};
	local.Tracks = {{"input", "input_type", "hold", -1}, {"input", "range", "hold", -1}};
	local.Nodes.front().SourceAnimatedInputs = {"range"};
	const GroupBootstrapTarget order[] = {{"input"}, {"alpha"}, {"beta"}};
	GroupReplayState empty, loaded, bound, edited;
	Diagnostic diagnostic;
	REQUIRE(
		ReplayGroupBootstrap(local, Checked(local), order, BindingClock(), empty, 1, loaded, diagnostic) ==
		Status::Ok
	);
	ArrayValue bootstrapPadding;
	bootstrapPadding.ElementType = ValueType::Scalar;
	bootstrapPadding.Elements = {0.0, 0.0, 0.0, 0.0};
	REQUIRE(loaded.Find("beta"));
	REQUIRE(loaded.Find("beta")->ParentReset);
	REQUIRE(*loaded.Find("beta")->ParentReset == Value{bootstrapPadding});
	auto authored = local;
	// Padding bootstrap replaces only the local parent animator before these edits.
	const auto betaParent =
		std::find_if(authored.Nodes[3].Values.begin(), authored.Nodes[3].Values.end(), [](const auto &value) {
			return value.Port == "parent_value";
		});
	REQUIRE(betaParent != authored.Nodes[3].Values.end());
	betaParent->Data = bootstrapPadding;
	authored.Nodes[2].InstanceBase = "input";
	authored.Nodes[3].InstanceBase = "input";
	authored.Nodes[2].InstanceOverrides = {"input_type", "range"};
	authored.Nodes[2].SourceAnimatedInputs = {"input_type", "range"};
	authored.Outputs.push_back({"alpha-result", "alpha", "value"});
	const GroupSubtypeBinding bindings[] = {
		{"alpha", "input"},
		{"beta", "input"},
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Static, "input_type"},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "input_type"},
		{"alpha", "input", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "range"},
		{"beta", "input", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Animated, "range"}
	};
	REQUIRE(BindGroupReplay(authored, bindings, loaded, 1, bound, diagnostic) == Status::Ok);
	Value type = EnumValue{2}, rangeValue = Vector2{4, 12};
	auto typeEdit = Event("alpha", GroupRefreshReason::Edit);
	typeEdit.EditedPort = "input_type";
	typeEdit.LocalValue = &type;
	typeEdit.LocalAnimated = true;
	typeEdit.At = BindingClock();
	auto rangeEdit = typeEdit;
	rangeEdit.EditedPort = "range";
	rangeEdit.LocalValue = &rangeValue;
	const GroupRefreshEvent edits[] = {typeEdit, rangeEdit};
	REQUIRE(ReplayGroupAnimatorEdits(authored, edits, bound, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.SharedSubtype("input", "input_type"));
	REQUIRE(edited.SharedSubtype("input", "range"));
	auto expected = first;
	expected.Data = type;
	CHECK(edited.SharedSubtype("input", "input_type")->Keys == std::vector<Keyframe>{expected, later});
	CHECK(edited.SharedSubtype("input", "range")->Keys.front().NodeId == "input");
	CHECK(edited.SharedSubtype("input", "range")->Keys.front().Data == rangeValue);
	const auto plan = Checked(authored);
	CHECK(ResolvedPort(authored, plan, edited, "beta", "input_type") == type);
	CHECK(ResolvedPort(authored, plan, edited, "alpha", "range", true) == rangeValue);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Nodes[2].Values == authored.Nodes[2].Values);
	CHECK(projected.Nodes[3].Values == authored.Nodes[3].Values);
	CHECK(projected.Keyframes[0] == expected);
	CHECK(projected.Keyframes[1] == later);
	GroupReplayState retired;
	REQUIRE(RebindProjectedGroupReplay(projected, edited, 2, retired, diagnostic) == Status::Ok);
	CHECK(retired.SharedSubtypes().empty());
	REQUIRE(retired.Binding("alpha", "range"));
	CHECK(retired.Binding("alpha", "range")->Getter == GroupSubtypeAnimator::Animated);
}
TEST_CASE(
	"Cloned native Invert edits share original static animator despite animated override",
	"[imagegraph][groups][group_replay]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.invert", "", {}, {{"mix", 1.0}}},
		{"copy", "pc.invert", "", {}, {{"mix", .25}}},
		{"peer", "pc.invert", "", {}, {{"mix", .5}}}
	};
	document.Nodes[1].InstanceBase = "owner";
	document.Nodes[2].InstanceBase = "owner";
	document.Nodes[1].InstanceOverrides = {"mix"};
	document.Nodes[1].SourceAnimatedInputs = {"mix"};
	document.Outputs = {{"copy-result", "copy", "surface_out"}, {"peer-result", "peer", "surface_out"}};
	Keyframe first{"owner", "mix", 1, .75, "source", KeyframeEase{}};
	first.NegativeFrame = true;
	first.Subframe = .5;
	first.Kind = KeyframeKind::Adder;
	first.SourceDriver = KeyframeLinearDriver{0};
	auto later = first;
	later.Tick = 9;
	later.NegativeFrame = false;
	later.Data = .5;
	document.Keyframes = {first, later};
	document.Tracks = {{"owner", "mix", "hold", -1}};
	Diagnostic diagnostic;
	GroupReplayState empty, local, bound, edited;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"copy", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Static, "mix"},
		{"peer", "owner", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "mix"}
	};
	REQUIRE(BindGroupReplay(document, bindings, local, 1, bound, diagnostic) == Status::Ok);
	const auto plan = Checked(document);
	CHECK(ResolvedPort(document, plan, bound, "copy", "mix", true) == Value{.75});
	Value mix = .125;
	auto edit = Event("copy", GroupRefreshReason::Edit);
	edit.EditedPort = "mix";
	edit.LocalValue = &mix;
	edit.LocalAnimated = true;
	edit.At = BindingClock();
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	auto expected = first;
	expected.Data = mix;
	CHECK(edited.SharedSubtype("owner", "mix")->Keys == std::vector<Keyframe>{expected, later});
	CHECK(ResolvedPort(document, plan, edited, "copy", "mix", true) == mix);
	CHECK(ResolvedPort(document, plan, edited, "peer", "mix", true) == mix);
	Document projected;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Keyframes == std::vector<Keyframe>{expected, later});
	CHECK(projected.Nodes[1] == document.Nodes[1]);
	CHECK(projected.Nodes[2] == document.Nodes[2]);
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	GroupReplayState populated;
	REQUIRE(RebindGroupReplay(document, edited, 1, populated, diagnostic) == Status::Ok);
	const auto oldBytes = populated.RetainedBytes();
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		GroupReplayState candidate;
		if (ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, candidate, diagnostic, middle) ==
			Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	GroupReplayState admitted;
	REQUIRE(
		ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, admitted, diagnostic, low) == Status::Ok
	);
	CHECK(
		ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, populated, diagnostic, low - 1) ==
		Status::LimitExceeded
	);
	CHECK(populated.RetainedBytes() == oldBytes);
	CHECK(populated.SharedSubtype("owner", "mix")->Keys == edited.SharedSubtype("owner", "mix")->Keys);
	edit.EditedPort = "attribute_process";
	CHECK(
		ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, populated, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(populated.RetainedBytes() == oldBytes);
	Value wrongType = Vector2{1, 2};
	edit.EditedPort = "mix";
	edit.LocalValue = &wrongType;
	CHECK(
		ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, populated, diagnostic) ==
		Status::TypeMismatch
	);
	CHECK(populated.RetainedBytes() == oldBytes);
}

TEST_CASE(
	"Instance input lookup follows the nearest override through a bounded base chain",
	"[imagegraph][groups][group_replay]"
) {
	for (const bool childOverrides : {false, true}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"surface",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}}},
			{"owner", "pc.invert", "", {}, {{"mix", .1}}},
			{"middle", "pc.invert", "", {}, {{"mix", .2}}},
			{"child", "pc.invert", "", {}, {{"mix", .9}}}
		};
		document.Nodes[2].InstanceBase = "owner";
		document.Nodes[2].InstanceOverrides = {"mix"};
		document.Nodes[2].SourceAnimatedInputs = {"mix"};
		document.Nodes[3].InstanceBase = "middle";
		if (childOverrides) document.Nodes[3].InstanceOverrides = {"mix"};
		document.Links = {
			{"surface", "image", "owner", "surface_in"},
			{"owner", "surface_out", "middle", "surface_in"},
			{"middle", "surface_out", "child", "surface_in"}
		};
		document.Outputs = {{"result", "child", "surface_out"}};
		document.Keyframes = {{"middle", "mix", 0, .25, "linear"}, {"middle", "mix", 10, .75, "linear"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		std::vector<AuthoredValue> authored;
		REQUIRE(
			ResolveNodeValues(document, plan, "result", "child", {.Tick = 2}, authored, diagnostic) ==
			Status::Ok
		);
		const auto value = std::find_if(authored.begin(), authored.end(), [](const auto &entry) {
			return entry.Port == "mix";
		});
		REQUIRE(value != authored.end());
		CHECK(value->Data == Value{childOverrides ? .9 : .35});
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(document, plan, "child", {.Tick = 2}, snapshot, diagnostic) == Status::Ok);
		const auto input =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &entry) {
				return entry.Port == "mix";
			});
		REQUIRE(input != snapshot.Values().end());
		CHECK(input->Data == value->Data);
	}
}

TEST_CASE(
	"Inherited source static keys follow their nearest instance owner", "[imagegraph][groups][group_replay]"
) {
	auto document = AliasedBoundaries();
	document.Nodes[2].InstanceBase = "input";
	document.Nodes[2].InstanceOverrides = {"subtype"};
	document.Nodes[2].SourceStaticInputs = {"subtype"};
	document.Nodes[3].InstanceBase = "alpha";
	document.Keyframes.push_back(
		{"alpha", "subtype", 1, EnumValue{3}, "source", KeyframeEase{"linear", "linear", {0, 1}, {0, 0}}}
	);
	document.Tracks.push_back({"alpha", "subtype", "hold", -1});
	const auto plan = Checked(document);
	EvaluationSnapshot snapshot;
	Diagnostic diagnostic;
	REQUIRE(EvaluateNodeInputs(document, plan, "beta", BindingClock(), snapshot, diagnostic) == Status::Ok);
	const auto subtype =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "subtype";
		});
	REQUIRE(subtype != snapshot.Values().end());
	CHECK(subtype->Data == Value{EnumValue{3}});
}

TEST_CASE(
	"Explicit source static parent reads first raw key and preserves metadata through edits",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary(7);
	document.Links.erase(document.Links.begin());
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().SourceStaticInputs = {"parent_value"};
	Keyframe first{"input", "parent_value", 1, 7.0, "source", KeyframeEase{}};
	first.NegativeFrame = true;
	first.Subframe = .5;
	first.Kind = KeyframeKind::Adder;
	first.SourceDriver = KeyframeLinearDriver{2};
	auto later = first;
	later.Tick = 9;
	later.NegativeFrame = false;
	later.Data = 19.0;
	document.Keyframes = {first, later};
	document.Tracks = {{"input", "parent_value", "hold", -1}};
	Diagnostic diagnostic;
	const auto plan = Checked(document);
	EvaluationSnapshot snapshot;
	auto request = BindingClock();
	REQUIRE(SetFrameTime(request, {4, .25, true}));
	REQUIRE(EvaluateNodeInputs(document, plan, "input", request, snapshot, diagnostic) == Status::Ok);
	const auto parent =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "parent_value";
		});
	REQUIRE(parent != snapshot.Values().end());
	CHECK(parent->Data == Value{7.0});
	GroupReplayState empty, loaded, edited;
	const GroupBootstrapTarget order[] = {{"input"}};
	REQUIRE(ReplayGroupBootstrap(document, plan, order, request, empty, 1, loaded, diagnostic) == Status::Ok);
	Value replacement = 11.0;
	auto event = Event("input", GroupRefreshReason::ParentEdit);
	event.LocalValue = &replacement;
	event.LocalAnimated = false;
	event.At = request;
	REQUIRE(ReplayGroupRefresh(document, plan, {&event, 1}, loaded, 1, edited, diagnostic) == Status::Ok);
	auto expected = first;
	expected.Data = replacement;
	CHECK(edited.Find("input")->ParentKeys == std::vector<Keyframe>{expected, later});
	Document projected, restored;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	REQUIRE(
		EvaluateNodeInputs(restored, Checked(restored), "input", request, snapshot, diagnostic) == Status::Ok
	);
	const auto persisted =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "parent_value";
		});
	REQUIRE(persisted != snapshot.Values().end());
	CHECK(persisted->Data == replacement);
	CHECK(restored.Keyframes == std::vector<Keyframe>{expected, later});
	auto invalid = restored;
	invalid.Nodes.front().SourceAnimatedInputs = {"parent_value"};
	Plan rejected;
	CHECK(Compile(invalid, rejected, diagnostic) == Status::InvalidValue);
	invalid = restored;
	invalid.Nodes.front().SourceStaticInputs = {"attribute_process"};
	CHECK(Compile(invalid, rejected, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Static parent projection budgets keyed and fixed controls without partial output",
	"[imagegraph][groups][group_replay]"
) {
	auto authored = Boundary(8.0);
	authored.Links.erase(authored.Links.begin());
	authored.Nodes.front().Values[0].Data = EnumValue{1};
	authored.Nodes.front().SourceStaticInputs = {"parent_value"};
	Keyframe first{"input", "parent_value", 1, 8.0, "source", KeyframeEase{}};
	first.Kind = KeyframeKind::Adder;
	first.SourceDriver = KeyframeLinearDriver{0};
	authored.Keyframes = {first};
	authored.Tracks = {{"input", "parent_value", "hold", -1}};
	const GroupBootstrapTarget order[] = {{"input"}};
	GroupReplayState empty, loaded, parentEdited, edited;
	Diagnostic diagnostic;
	const auto plan = Checked(authored);
	const auto request = BindingClock();
	REQUIRE(ReplayGroupBootstrap(authored, plan, order, request, empty, 1, loaded, diagnostic) == Status::Ok);
	Value parent = 19.0;
	auto parentEvent = Event("input", GroupRefreshReason::ParentEdit);
	parentEvent.LocalValue = &parent;
	parentEvent.LocalAnimated = false;
	parentEvent.At = request;
	REQUIRE(
		ReplayGroupRefresh(authored, plan, {&parentEvent, 1}, loaded, 1, parentEdited, diagnostic) ==
		Status::Ok
	);
	Value subtype = EnumValue{4};
	auto subtypeEvent = Event("input", GroupRefreshReason::Edit);
	subtypeEvent.EditedPort = "subtype";
	subtypeEvent.LocalValue = &subtype;
	subtypeEvent.LocalAnimated = false;
	subtypeEvent.At = request;
	REQUIRE(
		ReplayGroupRefresh(authored, plan, {&subtypeEvent, 1}, parentEdited, 1, edited, diagnostic) ==
		Status::Ok
	);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	const auto findValue = [](const Node &node, std::string_view port) {
		return std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
			return value.Port == port;
		});
	};
	const auto parentValue = findValue(projected.Nodes.front(), "parent_value");
	const auto subtypeValue = findValue(projected.Nodes.front(), "subtype");
	REQUIRE(parentValue != projected.Nodes.front().Values.end());
	REQUIRE(subtypeValue != projected.Nodes.front().Values.end());
	CHECK(parentValue->Data == parent);
	CHECK(subtypeValue->Data == subtype);
	REQUIRE(projected.Keyframes.size() == 1);
	CHECK(projected.Keyframes.front().Data == parent);

	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		Document candidate;
		if (ProjectGroupReplay(authored, edited, 1, candidate, diagnostic, middle) == Status::Ok)
			high = middle;
		else
			low = middle + 1;
	}
	Document admitted;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, admitted, diagnostic, low) == Status::Ok);
	CHECK(admitted == projected);
	Document prior = Boundary(.125);
	CHECK(ProjectGroupReplay(authored, edited, 1, prior, diagnostic, low - 1) == Status::LimitExceeded);
	CHECK(prior == Boundary(.125));
}

TEST_CASE(
	"Group shape refresh preserves recursive Any outer array length", "[imagegraph][groups][group_replay]"
) {
	auto document = Boundary();
	ArrayValue parent;
	parent.ElementType = ValueType::Any;
	parent.Items = {
		SourceArrayItem{ElementValue{2.0}},
		SourceArrayItem{std::vector<SourceArrayItem>{
			SourceArrayItem{ElementValue{4.0}}, SourceArrayItem{ElementValue{6.0}}
		}}
	};
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{1};
	document.Nodes.front().Values[3].Data = parent;
	document.Junctions.front().Default = parent;
	const auto plan = Checked(document);
	GroupReplayState empty, loaded;
	Diagnostic diagnostic;
	auto event = Event("input");
	REQUIRE(ReplayGroupRefresh(document, plan, {&event, 1}, empty, 1, loaded, diagnostic) == Status::Ok);
	REQUIRE(loaded.Find("input"));
	CHECK_FALSE(loaded.Find("input")->ParentReset);
	CHECK(std::get<ArrayValue>(Sample(document, plan, loaded, 0).Data) == parent);
}

TEST_CASE(
	"Source parent animator reset persists a new zero-frame key and preserves settings",
	"[imagegraph][groups][group_replay]"
) {
	for (bool animated : {false, true}) {
		auto document = Boundary(.5);
		document.Links.erase(document.Links.begin());
		document.Nodes.front().Values[0].Data = EnumValue{1};
		document.Nodes.front().Values[1].Data = EnumValue{7};
		(animated ? document.Nodes.front().SourceAnimatedInputs
				  : document.Nodes.front().SourceStaticInputs) = {"parent_value"};
		document.Keyframes = {{"input", "parent_value", 3, .5, "source", KeyframeEase{}}};
		document.Tracks = {{"input", "parent_value", "hold", 7}};
		Diagnostic diagnostic;
		const auto plan = Checked(document);
		GroupReplayState empty, loaded, edited;
		const GroupBootstrapTarget order[] = {{"input"}};
		REQUIRE(
			ReplayGroupBootstrap(document, plan, order, BindingClock(), empty, 1, loaded, diagnostic) ==
			Status::Ok
		);
		REQUIRE(loaded.Find("input"));
		CHECK_FALSE(loaded.Find("input")->ParentReset);
		REQUIRE(loaded.Find("input")->ParentKeys.size() == 1);
		const auto reset = loaded.Find("input")->ParentKeys.front();
		CHECK(GetFrameTime(reset) == FrameTime{});
		REQUIRE(std::holds_alternative<ArrayValue>(reset.Data));
		CHECK(std::get<ArrayValue>(reset.Data).Elements == std::vector<ElementValue>{0.0, 0.0});
		Document projected, restored;
		REQUIRE(ProjectGroupReplay(document, loaded, 1, projected, diagnostic) == Status::Ok);
		CHECK(projected.Keyframes == std::vector<Keyframe>{reset});
		CHECK(projected.Tracks == document.Tracks);
		CHECK(projected.Nodes.front().SourceAnimatedInputs == document.Nodes.front().SourceAnimatedInputs);
		CHECK(projected.Nodes.front().SourceStaticInputs == document.Nodes.front().SourceStaticInputs);
		REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
		CHECK(restored == projected);
		const auto restoredPlan = Checked(restored);
		GroupReplayState rebound;
		REQUIRE(RebindProjectedGroupReplay(restored, loaded, 2, rebound, diagnostic) == Status::Ok);
		Value replacement = ArrayValue{ValueType::Scalar, {2.0, 3.0}};
		auto event = Event("input", GroupRefreshReason::ParentEdit);
		event.LocalAnimated = animated;
		event.LocalValue = &replacement;
		event.At = BindingClock();
		event.At.Tick = 5;
		REQUIRE(
			ReplayGroupRefresh(restored, restoredPlan, {&event, 1}, rebound, 2, edited, diagnostic) ==
			Status::Ok
		);
		REQUIRE(ProjectGroupReplay(restored, edited, 2, projected, diagnostic) == Status::Ok);
		REQUIRE(projected.Keyframes.size() == (animated ? 2 : 1));
		if (animated) {
			const auto retainedReset = std::find_if(
				projected.Keyframes.begin(), projected.Keyframes.end(), [&](const Keyframe &key) {
					return key.NodeId == reset.NodeId && key.Port == reset.Port &&
						   GetFrameTime(key) == GetFrameTime(reset);
				}
			);
			REQUIRE(retainedReset != projected.Keyframes.end());
			CHECK(*retainedReset == reset);
			const auto signedEdit = std::find_if(
				projected.Keyframes.begin(), projected.Keyframes.end(), [&](const Keyframe &key) {
					return key.NodeId == reset.NodeId && key.Port == reset.Port &&
						   GetFrameTime(key) == GetFrameTime(event.At);
				}
			);
			REQUIRE(signedEdit != projected.Keyframes.end());
			CHECK(signedEdit->Data == replacement);
			CHECK(signedEdit->NegativeFrame);
			CHECK(signedEdit->Tick == 5);
			CHECK(signedEdit->Subframe == .5);
		} else
			CHECK(projected.Keyframes.front().Data == replacement);
		CHECK(projected.Tracks == document.Tracks);
	}
}

TEST_CASE(
	"Source Trigger parent reset removes keys and persists its false empty getter",
	"[imagegraph][groups][group_replay]"
) {
	for (bool animated : {false, true}) {
		auto document = Boundary(.5);
		document.Links.erase(document.Links.begin());
		document.Nodes.front().Values[0].Data = EnumValue{19};
		(animated ? document.Nodes.front().SourceAnimatedInputs
				  : document.Nodes.front().SourceStaticInputs) = {"parent_value"};
		document.Keyframes = {{"input", "parent_value", 3, .5, "source", KeyframeEase{}}};
		document.Tracks = {{"input", "parent_value", "hold", 7}};
		Diagnostic diagnostic;
		GroupReplayState empty, loaded;
		const GroupBootstrapTarget order[] = {{"input"}};
		REQUIRE(
			ReplayGroupBootstrap(
				document, Checked(document), order, BindingClock(), empty, 1, loaded, diagnostic
			) == Status::Ok
		);
		REQUIRE(loaded.Find("input"));
		CHECK(loaded.Find("input")->Domain.Kind == SourceSocketKind::Trigger);
		CHECK(loaded.Find("input")->ParentKeys.empty());
		REQUIRE(loaded.Find("input")->ParentReset);
		CHECK(*loaded.Find("input")->ParentReset == Value{false});
		Document projected, restored;
		REQUIRE(ProjectGroupReplay(document, loaded, 1, projected, diagnostic) == Status::Ok);
		CHECK(projected.Keyframes.empty());
		CHECK(projected.Tracks == document.Tracks);
		CHECK(projected.Nodes.front().SourceAnimatedInputs == document.Nodes.front().SourceAnimatedInputs);
		CHECK(projected.Nodes.front().SourceStaticInputs == document.Nodes.front().SourceStaticInputs);
		REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
		CHECK(restored == projected);
		EvaluationSnapshot snapshot;
		REQUIRE(
			EvaluateNodeInputs(restored, Checked(restored), "input", BindingClock(), snapshot, diagnostic) ==
			Status::Ok
		);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &entry) {
				return entry.Port == "parent_value";
			});
		REQUIRE(value != snapshot.Values().end());
		CHECK(value->Data == Value{false});
		GroupReplayState rebound, nativeRestored;
		REQUIRE(RebindProjectedGroupReplay(restored, loaded, 2, rebound, diagnostic) == Status::Ok);
		auto clock = BindingClock();
		clock.Tick = 31;
		REQUIRE(
			RestoreGroupDeclarations(
				restored, Checked(restored), order, clock, rebound, 2, nativeRestored, diagnostic
			) == Status::Ok
		);
		Document stable;
		REQUIRE(ProjectGroupReplay(restored, nativeRestored, 2, stable, diagnostic) == Status::Ok);
		CHECK(stable == restored);
		Value pressed = true;
		auto press = Event("input", GroupRefreshReason::ParentEdit);
		press.LocalAnimated = animated;
		press.LocalValue = &pressed;
		press.At = clock;
		GroupReplayState unchanged;
		CHECK(
			ReplayGroupRefresh(
				restored, Checked(restored), {&press, 1}, nativeRestored, 2, unchanged, diagnostic
			) == Status::UnsupportedExecution
		);
		CHECK(diagnostic.Message == "source Trigger map requires a nonnegative integer frame");
	}
}

TEST_CASE("Timeline clones long instance names with admitted copy capacity", "[imagegraph][timeline]") {
	Document document;
	document.FormatVersion = 9;
	const std::string originalName(21, 'o');
	document.Nodes = {
		{originalName, "pc.invert", "", {}, {{"mix", .25}}}, {"copy", "pc.invert", "", {}, {{"mix", .75}}}
	};
	document.Nodes.back().InstanceBase = originalName;
	document.Nodes.back().InstanceOverrides = {"mix"};
	document.Keyframes = {{"copy", "mix", 1, .5, "linear"}, {"copy", "mix", 3, .75, "linear"}};
	document.Outputs = {{"result", "copy", "surface_out"}};
	Diagnostic diagnostic;
	const auto plan = Checked(document);
	(void)plan;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides overrides;
	const uint8_t needed[] = {1, 1};
	EvaluationRequest request;
	request.Tick = 2;
	REQUIRE(
		detail::ResolveTimelineOverrides(document, needed, request, budget, overrides, diagnostic) ==
		Status::Ok
	);
	const auto &sampled = overrides.Find(1, document.Nodes.back());
	CHECK(sampled.InstanceBase == originalName);
	CHECK(sampled.InstanceBase.capacity() == std::string(originalName).capacity());
	CHECK(std::get<double>(sampled.Values.front().Data) == .625);
}

TEST_CASE(
	"Declared animated mode does not exempt legacy one-key range validation", "[imagegraph][timeline]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "pc.number_simple", "", {}, {{"value", 2.0}}}};
	document.Nodes.front().SourceAnimatedInputs = {"value"};
	document.Keyframes = {{"number", "value", 0, 2.0, "linear"}};
	document.Tracks = {{"number", "value", "wrap", 999}};
	document.Outputs = {{"result", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Keyframes.front().Interpolation = "source";
	document.Keyframes.front().Ease = KeyframeEase{};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
}

TEST_CASE(
	"Effective shared wrap tracks diagnose missing timeline after a second key",
	"[imagegraph][groups][group_replay]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.invert", "", {}, {{"mix", .5}}}, {"copy", "pc.invert", "", {}, {{"mix", .25}}}
	};
	document.Nodes.back().InstanceBase = "owner";
	document.Nodes.front().SourceAnimatedInputs = {"mix"};
	document.Nodes.back().SourceAnimatedInputs = {"mix"};
	document.Keyframes = {{"owner", "mix", 0, .5, "source", KeyframeEase{}}};
	document.Tracks = {{"owner", "mix", "wrap", -1}};
	document.Outputs = {{"result", "copy", "surface_out"}};
	Diagnostic diagnostic;
	const auto plan = Checked(document);
	GroupReplayState empty, local, bound, edited;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"copy", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"}
	};
	REQUIRE(BindGroupReplay(document, bindings, local, 1, bound, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.GroupReplay = &bound;
	request.GroupAuthoringRevision = 1;
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "copy", request, snapshot, diagnostic) == Status::Ok);
	const size_t retainedCount = snapshot.Values().size();
	Value replacement = .75;
	auto edit = Event("copy");
	edit.EditedPort = "mix";
	edit.LocalValue = &replacement;
	edit.LocalAnimated = true;
	REQUIRE(SetFrameTime(edit.At, {1, .25, true}));
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.SharedSubtype("owner", "mix"));
	CHECK(edited.SharedSubtype("owner", "mix")->Keys.size() == 2);
	const auto retained = edited.RetainedBytes();
	request.GroupReplay = &edited;
	REQUIRE(SetFrameTime(request, {2, .5, true}));
	CHECK(
		EvaluateNodeInputs(document, plan, "copy", request, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "owner");
	CHECK(diagnostic.Port == "mix");
	CHECK(snapshot.Values().size() == retainedCount);
	const auto mix = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "mix";
	});
	REQUIRE(mix != snapshot.Values().end());
	CHECK(mix->Data == Value{.5});
	CHECK(edited.RetainedBytes() == retained);
	CHECK(bound.SharedSubtypes().empty());
}

TEST_CASE(
	"Activated shared source loop ranges below minus one are diagnosed", "[imagegraph][groups][group_replay]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"owner", "pc.invert", "", {}, {{"mix", .5}}}, {"copy", "pc.invert", "", {}, {{"mix", .25}}}
	};
	document.Nodes.back().InstanceBase = "owner";
	document.Nodes.front().SourceAnimatedInputs = {"mix"};
	document.Nodes.back().SourceAnimatedInputs = {"mix"};
	document.Keyframes = {{"owner", "mix", 0, .5, "source", KeyframeEase{}}};
	document.Tracks = {{"owner", "mix", "hold", -7}};
	document.Outputs = {{"result", "copy", "surface_out"}};
	Diagnostic diagnostic;
	const auto plan = Checked(document);
	GroupReplayState empty, local, bound, edited;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"copy", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "mix"}
	};
	REQUIRE(BindGroupReplay(document, bindings, local, 1, bound, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.GroupReplay = &bound;
	request.GroupAuthoringRevision = 1;
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "copy", request, snapshot, diagnostic) == Status::Ok);
	const size_t retainedCount = snapshot.Values().size();
	Value replacement = .75;
	auto edit = Event("copy");
	edit.EditedPort = "mix";
	edit.LocalValue = &replacement;
	edit.LocalAnimated = true;
	REQUIRE(SetFrameTime(edit.At, {1, .25, true}));
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	REQUIRE(edited.SharedSubtype("owner", "mix"));
	CHECK(edited.SharedSubtype("owner", "mix")->Keys.size() == 2);
	const auto retained = edited.RetainedBytes();
	request.GroupReplay = &edited;
	REQUIRE(SetFrameTime(request, {2, .5, true}));
	CHECK(
		EvaluateNodeInputs(document, plan, "copy", request, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "owner");
	CHECK(diagnostic.Port == "mix");
	CHECK(snapshot.Values().size() == retainedCount);
	const auto mix = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "mix";
	});
	REQUIRE(mix != snapshot.Values().end());
	CHECK(mix->Data == Value{.5});
	CHECK(edited.RetainedBytes() == retained);
	CHECK(bound.SharedSubtypes().empty());
}

TEST_CASE(
	"Pure local Group parent Set key samples linked GET and preserves static or animated writer keys",
	"[imagegraph][groups][group_replay]"
) {
	for (const bool animated : {false, true}) {
		auto document = Boundary(73.0);
		document.Nodes.front().Values[0].Data = EnumValue{1};
		document.Nodes.front().Values[3].Data = 8.0;
		(animated ? document.Nodes.front().SourceAnimatedInputs
				  : document.Nodes.front().SourceStaticInputs) = {"parent_value"};
		Keyframe first{"input", "parent_value", 1, 11.0, "source", KeyframeEase{}};
		first.NegativeFrame = true;
		first.Subframe = .5;
		first.Kind = KeyframeKind::Adder;
		first.SourceDriver = KeyframeLinearDriver{0};
		auto later = first;
		later.Tick = 10;
		later.NegativeFrame = false;
		later.Subframe = 0;
		later.Data = 27.0;
		document.Keyframes = {first, later};
		document.Tracks = {{"input", "parent_value", "hold", -1}};
		const auto unchanged = document;
		auto clock = BindingClock();
		REQUIRE(SetFrameTime(clock, {4, .25, true}));
		Diagnostic diagnostic;
		GroupReplayState empty, restored, bound, edited;
		const GroupBootstrapTarget order[] = {{"input"}};
		const auto plan = Checked(document);
		REQUIRE(
			RestoreGroupDeclarations(document, plan, order, clock, empty, 1, restored, diagnostic) ==
			Status::Ok
		);
		REQUIRE(BindGroupReplay(document, {}, restored, 1, bound, diagnostic) == Status::Ok);
		clock.GroupReplay = &bound;
		clock.GroupAuthoringRevision = 1;
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(document, plan, "input", clock, snapshot, diagnostic) == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &input) {
				return input.Port == "parent_value";
			});
		REQUIRE(value != snapshot.Values().end());
		REQUIRE(value->Data == Value{73.0});
		auto edit = Event("input", GroupRefreshReason::Edit);
		edit.At = clock;
		edit.EditedPort = "parent_value";
		edit.LocalValue = &value->Data;
		edit.LocalAnimated = animated;
		REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic) == Status::Ok);
		REQUIRE(edited.Find("input"));
		CHECK(edited.Find("input")->InputType == bound.Find("input")->InputType);
		CHECK(edited.Find("input")->Subtype == bound.Find("input")->Subtype);
		CHECK(edited.Bindings().empty());
		Document projected, reopened;
		REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
		if (animated) {
			REQUIRE(projected.Keyframes.size() == 3);
			CHECK(
				std::find(projected.Keyframes.begin(), projected.Keyframes.end(), first) !=
				projected.Keyframes.end()
			);
			CHECK(
				std::find(projected.Keyframes.begin(), projected.Keyframes.end(), later) !=
				projected.Keyframes.end()
			);
			const auto inserted =
				std::find_if(projected.Keyframes.begin(), projected.Keyframes.end(), [&](const auto &key) {
					return GetFrameTime(key) == GetFrameTime(clock);
				});
			REQUIRE(inserted != projected.Keyframes.end());
			CHECK(inserted->NodeId == "input");
			CHECK(inserted->Port == "parent_value");
			CHECK(inserted->Data == value->Data);
		} else {
			auto expected = first;
			expected.Data = value->Data;
			CHECK(projected.Keyframes == std::vector<Keyframe>{expected, later});
		}
		REQUIRE(Read(Write(projected), reopened, diagnostic) == Status::Ok);
		CHECK(reopened == projected);
		CHECK(document == unchanged);
		const auto bytes = edited.RetainedBytes();
		CHECK(
			ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic, 1) ==
			Status::LimitExceeded
		);
		CHECK(edited.RetainedBytes() == bytes);
	}
}

TEST_CASE(
	"Routed Group parent GET survives durable local edits and bootstrap animator resets",
	"[imagegraph][groups][group_replay]"
) {
	auto document = Boundary(73.0);
	document.Nodes.front().Values[0].Data = EnumValue{1};
	document.Nodes.front().Values[1].Data = EnumValue{6};
	document.Nodes.front().Values[3].Data = 8.0;
	document.Nodes.front().SourceStaticInputs = {"parent_value"};
	const auto unchanged = document;
	const auto plan = Checked(document);
	Diagnostic diagnostic;
	GroupReplayState empty, loaded, edited;
	const GroupBootstrapTarget order[] = {{"input"}};
	auto clock = BindingClock();
	const auto bootstrap = ReplayGroupBootstrap(document, plan, order, clock, empty, 1, loaded, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(bootstrap == Status::Ok);
	REQUIRE(loaded.Find("input"));
	CHECK_FALSE(loaded.Find("input")->ParentReset);
	REQUIRE(loaded.Find("input")->ParentKeys.size() == 1);
	Keyframe expected;
	expected.NodeId = "input";
	expected.Port = "parent_value";
	expected.Tick = 0;
	expected.Data = ArrayValue{ValueType::Scalar, {0.0, 0.0, 0.0, 0.0}};
	expected.Interpolation = "source";
	expected.Ease = KeyframeEase{};
	CHECK(loaded.Find("input")->ParentKeys == std::vector<Keyframe>{expected});
	const auto bootstrapKey = expected;
	const auto checkRouted = [&](const GroupReplayState &state) {
		clock.GroupReplay = &state;
		clock.GroupAuthoringRevision = 1;
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(document, plan, "input", clock, snapshot, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		const auto parent =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &input) {
				return input.Port == "parent_value";
			});
		REQUIRE(parent != snapshot.Values().end());
		CHECK(parent->Data == Value{73.0});
	};
	checkRouted(loaded);
	const Value local = 19.0;
	auto edit = Event("input", GroupRefreshReason::ParentEdit);
	edit.At = clock;
	edit.LocalValue = &local;
	edit.LocalAnimated = false;
	const auto status = ReplayGroupRefresh(document, plan, {&edit, 1}, loaded, 1, edited, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(status == Status::Ok);
	REQUIRE(edited.Find("input"));
	CHECK_FALSE(edited.Find("input")->ParentReset);
	expected.Data = local;
	CHECK(edited.Find("input")->ParentKeys == std::vector<Keyframe>{expected});
	checkRouted(edited);
	Document projected, reopened;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
	const auto parent = std::find_if(
		projected.Nodes.front().Values.begin(), projected.Nodes.front().Values.end(), [](const auto &value) {
			return value.Port == "parent_value";
		}
	);
	REQUIRE(parent != projected.Nodes.front().Values.end());
	CHECK(parent->Data == local);
	REQUIRE(Read(Write(projected), reopened, diagnostic) == Status::Ok);
	CHECK(reopened == projected);
	CHECK(document == unchanged);
	CHECK(loaded.Find("input")->ParentKeys == std::vector<Keyframe>{bootstrapKey});
}

TEST_CASE(
	"Static source projection preserves dynamic declarations through native reload and rebind",
	"[imagegraph][groups][group_replay][dynamic_projection]"
) {
	Document authored;
	authored.FormatVersion = 9;
	authored.Nodes = {{"lua", "pc.lua_compute", {}, {}, {}}, {"invert", "pc.invert", {}, {}, {{"mix", .5}}}};
	authored.Nodes.front().DynamicInputs = {
		{"argument_name_0", ValueType::Text, Value{std::string("gain")}},
		{"argument_type_0", ValueType::Enum, Value{EnumValue{0}}},
		{"argument_value_0", ValueType::Scalar, Value{9.5}},
		{"argument_name_1", ValueType::Text, Value{std::string("keyed_gain")}},
		{"argument_type_1", ValueType::Enum, Value{EnumValue{0}}},
		{"argument_value_1", ValueType::Scalar, Value{19.5}}
	};
	authored.Nodes.front().DynamicInputs[1].SourceInputId = "pxc:input:4";
	authored.Nodes.front().DynamicInputs[2].SourceInputId = "pxc:input:5";
	authored.Nodes.front().DynamicInputs[5].SourceInputId = "pxc:input:8";
	authored.Nodes.front().SourceStaticInputs = {"argument_value_0", "argument_value_1"};
	authored.Keyframes = {{"lua", "argument_value_1", 0, 19.5, "source", KeyframeEase{}}};
	authored.Keyframes.front().SourceKeyId = "original-value";
	authored.Tracks = {{"lua", "argument_value_1", "hold", -1}};
	authored.Outputs = {{"result", "invert", "surface_out"}};
	const auto unchanged = authored;
	Checked(authored);
	Diagnostic diagnostic;
	GroupReplayState empty, local, loaded, edited;
	REQUIRE(RebindGroupReplay(authored, empty, 1, local, diagnostic) == Status::Ok);
	const auto bindStatus = BindGroupReplay(authored, {}, local, 1, loaded, diagnostic);
	INFO(diagnostic.NodeId << "/" << diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(bindStatus == Status::Ok);
	const Value dynamicValue = 12.5, keyedValue = 22.5, fixedValue = .75, selector = EnumValue{0};
	GroupRefreshEvent edits[4];
	edits[0] = Event("lua", GroupRefreshReason::Edit);
	edits[0].EditedPort = "argument_value_0";
	edits[0].LocalValue = &dynamicValue;
	edits[1] = Event("invert", GroupRefreshReason::Edit);
	edits[1].EditedPort = "mix";
	edits[1].LocalValue = &fixedValue;
	edits[2] = Event("lua", GroupRefreshReason::Edit);
	edits[2].EditedPort = "argument_type_0";
	edits[2].LocalValue = &selector;
	edits[3] = Event("lua", GroupRefreshReason::Edit);
	edits[3].EditedPort = "argument_value_1";
	edits[3].LocalValue = &keyedValue;
	for (auto &edit : edits)
		edit.LocalAnimated = false;
	REQUIRE(ReplayGroupAnimatorEdits(authored, edits, loaded, 1, edited, diagnostic) == Status::Ok);
	Document projected, restored;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(authored == unchanged);
	CHECK(projected.Nodes.front().Values.empty());
	CHECK(projected.Nodes.front().DynamicInputs[1].Default == std::optional<Value>{selector});
	CHECK(projected.Nodes.front().DynamicInputs[2].Default == std::optional<Value>{dynamicValue});
	CHECK(projected.Nodes.front().DynamicInputs[1].SourceInputId == "pxc:input:4");
	CHECK(projected.Nodes.front().DynamicInputs[2].SourceInputId == "pxc:input:5");
	CHECK(projected.Nodes.back().Values == std::vector<AuthoredValue>{{"mix", fixedValue}});
	auto expectedKey = authored.Keyframes.front();
	expectedKey.Data = keyedValue;
	CHECK(projected.Keyframes == std::vector<Keyframe>{expectedKey});
	CHECK(
		projected.Nodes.front().DynamicInputs[5].Default == authored.Nodes.front().DynamicInputs[5].Default
	);
	CHECK(projected.Nodes.front().DynamicInputs[5].SourceInputId == "pxc:input:8");
	CHECK(projected.Tracks == authored.Tracks);
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	const auto plan = Checked(restored);
	GroupReplayState rebound;
	REQUIRE(RebindProjectedGroupReplay(restored, edited, 2, rebound, diagnostic) == Status::Ok);
	CHECK(rebound.SharedSubtypes().empty());
	EvaluationRequest request;
	request.GroupReplay = &rebound;
	request.GroupAuthoringRevision = 2;
	EvaluationSnapshot inputs;
	REQUIRE(EvaluateNodeInputs(restored, plan, "lua", request, inputs, diagnostic) == Status::Ok);
	const auto input = std::find_if(inputs.Values().begin(), inputs.Values().end(), [](const auto &value) {
		return value.Port == "argument_value_0";
	});
	REQUIRE(input != inputs.Values().end());
	CHECK(input->Data == dynamicValue);
	const auto keyedInput =
		std::find_if(inputs.Values().begin(), inputs.Values().end(), [](const auto &value) {
			return value.Port == "argument_value_1";
		});
	REQUIRE(keyedInput != inputs.Values().end());
	CHECK(keyedInput->Data == keyedValue);
	auto mismatched = restored;
	mismatched.Nodes.front().DynamicInputs[2].Default = Value{13.5};
	const auto priorBytes = rebound.RetainedBytes();
	CHECK(RebindProjectedGroupReplay(mismatched, edited, 3, rebound, diagnostic) != Status::Ok);
	CHECK(rebound.AuthoringRevision() == 2);
	CHECK(rebound.RetainedBytes() == priorBytes);
}
