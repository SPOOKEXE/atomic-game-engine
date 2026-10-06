#include "../src/ImageGraphAppendGroups.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <studio/ImageGraph.hpp>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.append_groups")
TEST_DEPENDS("engine.imagegraph.group_replay")
TEST_DEPENDS("engine.imagegraph.group_callback_document")
TEST_DEPENDS("engine.imagegraphio.pxcxappend")
TEST_DEPENDS("studio.imagegraph.group_host")

using namespace engine::imagegraph;

namespace {
	Document GroupDocument() {
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
			  {"parent_value", .5}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		document.Groups = {
			{"group",
			 "Group",
			 {},
			 {{"input", "in", PortDirection::Input, "input"},
			  {"output", "out", PortDirection::Output, "output"}}}
		};
		document.Junctions = {
			{"in", "group", ValueType::Any, .5}, {"out", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {{"input", "value", "output", "value"}, {"output", "value", "out", "value"}};
		document.Outputs = {{"result", "output", "value"}};
		return document;
	}

	engine::imagegraphio::PxcxAppendResult IncomingGroup(const Document &live) {
		using namespace engine::imagegraphio;
		PxcxAppendResult append;
		append.Project.Graph = live;
		auto &graph = append.Project.Graph;
		Node input{
			"new-input",
			"pc.group_input",
			"new-group",
			{},
			{{"input_type", EnumValue{2}},
			 {"subtype", EnumValue{0}},
			 {"vector_size", EnumValue{0}},
			 {"parent_value", .5}}
		};
		input.SourceAnimatedInputs = {"input_type"};
		graph.Nodes.push_back(std::move(input));
		graph.Nodes.push_back({"new-output", "pc.group_output", "new-group", {}, {}});
		graph.Groups.push_back(
			{"new-group",
			 "New group",
			 {},
			 {{"new-input-port", "new-in", PortDirection::Input, "new-input"},
			  {"new-output-port", "new-out", PortDirection::Output, "new-output"}}}
		);
		graph.Junctions.push_back({"new-in", "new-group", ValueType::Any, .5});
		graph.Junctions.push_back({"new-out", "new-group", ValueType::Any, std::nullopt});
		graph.Links.push_back({"new-input", "value", "new-output", "value"});
		graph.Links.push_back({"new-output", "value", "new-out", "value"});
		graph.Keyframes = {
			{"new-input", "input_type", 2, EnumValue{1}, "step", std::nullopt},
			{"new-input", "input_type", 2, EnumValue{2}, "step", std::nullopt}
		};
		graph.Keyframes[0].NegativeFrame = true;
		graph.Tracks = {{"new-input", "input_type", "hold", -1}};
		append.Project.GroupPrebinding = graph;
		append.Project.GroupBootstrap = {{"new-input", GroupSubtypeAnimator::Animated}};
		append.Nodes = {{"new-input", "new-input", false}, {"new-output", "new-output", false}};
		return append;
	}

	void KeepOnlyIncomingCallbacks(engine::imagegraphio::PxcxAppendResult &append, const Document &live) {
		auto &saved = *append.Project.GroupPrebinding;
		const auto isLiveNode = [&](std::string_view id) {
			return std::any_of(live.Nodes.begin(), live.Nodes.end(), [&](const auto &node) {
				return node.Id == id;
			});
		};
		const auto isLiveTopology = [&](std::string_view id) {
			return isLiveNode(id) ||
				   std::any_of(
					   live.Groups.begin(),
					   live.Groups.end(),
					   [&](const auto &group) { return group.Id == id; }
				   ) ||
				   std::any_of(live.Junctions.begin(), live.Junctions.end(), [&](const auto &junction) {
					   return junction.Id == id;
				   });
		};
		std::erase_if(saved.Nodes, [&](const auto &node) { return isLiveNode(node.Id); });
		std::erase_if(saved.Groups, [&](const auto &group) {
			return std::any_of(live.Groups.begin(), live.Groups.end(), [&](const auto &old) {
				return old.Id == group.Id;
			});
		});
		std::erase_if(saved.Junctions, [&](const auto &junction) {
			return std::any_of(live.Junctions.begin(), live.Junctions.end(), [&](const auto &old) {
				return old.Id == junction.Id;
			});
		});
		std::erase_if(saved.Links, [&](const auto &link) {
			return isLiveTopology(link.FromNode) || isLiveTopology(link.ToNode);
		});
		std::erase_if(saved.Keyframes, [&](const auto &key) { return isLiveNode(key.NodeId); });
		std::erase_if(saved.Tracks, [&](const auto &track) { return isLiveNode(track.NodeId); });
	}

	Document GroupWithLuaAliases() {
		auto document = GroupDocument();
		for (const auto *id : {"base", "copy", "sibling"}) {
			Node node{id, "pc.lua_compute", {}, {}, {}};
			for (size_t index = 0; index < 3; ++index) {
				const auto suffix = std::to_string(index);
				node.DynamicInputs.push_back(
					{"argument_name_" + suffix, ValueType::Text, Value{std::string("v") + suffix}}
				);
				node.DynamicInputs.push_back(
					{"argument_type_" + suffix, ValueType::Enum, Value{EnumValue{0}}}
				);
				node.DynamicInputs.push_back(
					{"argument_value_" + suffix, ValueType::Scalar, Value{double(index + 1) * 10}}
				);
				node.SourceAnimatedInputs.push_back("argument_value_" + suffix);
			}
			if (node.Id != "base") node.InstanceBase = "base";
			document.Nodes.push_back(std::move(node));
		}
		for (size_t index = 0; index < 3; ++index) {
			const auto port = "argument_value_" + std::to_string(index);
			document.Keyframes.push_back({"base", port, 0, double(index + 1) * 10, "source", KeyframeEase{}});
			document.Tracks.push_back({"base", port, "hold", -1});
		}
		document.Nodes.push_back(
			{"solid",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}}
		);
		document.Outputs.push_back({"solid-output", "solid", "image"});
		return document;
	}

	void StageBaseMove(Document &document) {
		auto &base = *std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
			return node.Id == "base";
		});
		base.DynamicInputs.erase(base.DynamicInputs.begin() + 3, base.DynamicInputs.begin() + 6);
		for (size_t index = 3; index < 6; ++index)
			base.DynamicInputs[index].Id.back() = '1';
		std::erase(base.SourceAnimatedInputs, "argument_value_1");
		for (auto &port : base.SourceAnimatedInputs)
			if (port == "argument_value_2") port = "argument_value_1";
		std::erase_if(document.Keyframes, [](const auto &key) {
			return key.NodeId == "base" && key.Port == "argument_value_1";
		});
		for (auto &key : document.Keyframes)
			if (key.NodeId == "base" && key.Port == "argument_value_2") key.Port = "argument_value_1";
		std::erase_if(document.Tracks, [](const auto &track) {
			return track.NodeId == "base" && track.Port == "argument_value_1";
		});
		for (auto &track : document.Tracks)
			if (track.NodeId == "base" && track.Port == "argument_value_2") track.Port = "argument_value_1";
	}

	Value InputValue(const Document &document, const GroupReplayState &replay, std::string_view nodeId) {
		Diagnostic diagnostic;
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.GroupReplay = &replay;
		request.GroupAuthoringRevision = replay.AuthoringRevision();
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic) == Status::Ok);
		const auto found =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
				return value.Port == "argument_value_1";
			});
		REQUIRE(found != snapshot.Values().end());
		return found->Data;
	}
}

TEST_CASE(
	"appended Group callbacks use the signed fractional caller clock and keep destination capture",
	"[studio][append_groups]"
) {
	const auto live = GroupDocument();
	Plan plan;
	Diagnostic diagnostic;
	const auto compile = Compile(live, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compile == Status::Ok);
	studio::ImageGraphGroupHost previous;
	EvaluationRequest initialClock;
	REQUIRE(previous.Prepare(live, plan, 1, initialClock, diagnostic));
	REQUIRE(previous.Replay.Find("input"));
	const auto oldDomain = previous.Replay.Find("input")->Domain;

	auto appended = IncomingGroup(live);
	studio::detail::ImageGraphAppendGroups candidate;
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.Subframe = .5;
	clock.NegativeFrame = true;
	const bool accepted = candidate.Prepare(appended, live, previous, 2, clock, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(accepted);
	REQUIRE(candidate.Host.Replay.Find("input"));
	CHECK(candidate.Host.Replay.Find("input")->Domain == oldDomain);
	REQUIRE(candidate.Host.Replay.Find("new-input"));
	CHECK(candidate.Host.Replay.Find("new-input")->Domain.Kind == SourceSocketKind::Float);
	CHECK(candidate.Host.Revision == 2);
}

TEST_CASE(
	"appended Group refusal preserves the prior prepared document and host", "[studio][append_groups][atomic]"
) {
	const auto live = GroupDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(live, plan, diagnostic) == Status::Ok);
	studio::ImageGraphGroupHost previous;
	EvaluationRequest clock;
	REQUIRE(previous.Prepare(live, plan, 1, clock, diagnostic));

	studio::detail::ImageGraphAppendGroups candidate;
	auto appended = IncomingGroup(live);
	REQUIRE(candidate.Prepare(appended, live, previous, 2, clock, diagnostic));
	const auto priorDocument = candidate.Authored;
	const auto priorRevision = candidate.Host.Revision;
	const auto priorObservation = candidate.Host.Replay.ObservationRevision();

	CHECK_FALSE(candidate.Prepare(appended, live, previous, 3, clock, diagnostic, 1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(candidate.Authored == priorDocument);
	CHECK(candidate.Host.Revision == priorRevision);
	CHECK(candidate.Host.Replay.ObservationRevision() == priorObservation);
	CHECK(candidate.Host.Replay.Find("input"));
	CHECK(candidate.Host.Replay.Find("new-input"));

	auto opaque = IncomingGroup(live);
	opaque.Project.Graph.Nodes.push_back({"future", "Vendor_Future", {}, {}, {}});
	opaque.Project.GroupPrebinding = opaque.Project.Graph;
	opaque.Nodes.push_back({"future", "future", false});
	REQUIRE(candidate.Prepare(opaque, live, previous, 3, clock, diagnostic));
	const auto future =
		std::find_if(candidate.Authored.Nodes.begin(), candidate.Authored.Nodes.end(), [](const auto &node) {
			return node.Id == "future";
		});
	REQUIRE(future != candidate.Authored.Nodes.end());
	CHECK(future->Type == "Vendor_Future");
	const auto opaqueDocument = candidate.Authored;
	const auto opaqueRevision = candidate.Host.Revision;
	const auto opaqueObservation = candidate.Host.Replay.ObservationRevision();
	for (const bool viaExpression : {false, true}) {
		INFO("opaque dependency viaExpression=" << viaExpression);
		auto dependency = IncomingGroup(live);
		Node futureNode{"future", "Vendor_Future", {}, {}, {}};
		futureNode.GroupId = "new-group";
		futureNode.SourceInternalName = "future";
		futureNode.DynamicOutputs = {{"value", ValueType::Any}};
		dependency.Project.Graph.Nodes.push_back(std::move(futureNode));
		if (viaExpression) {
			auto input = std::find_if(
				dependency.Project.Graph.Nodes.begin(),
				dependency.Project.Graph.Nodes.end(),
				[](const auto &node) { return node.Id == "new-input"; }
			);
			REQUIRE(input != dependency.Project.Graph.Nodes.end());
			input->SourceInputExpressions = {{"input_type", "future.outputs.value", true}};
		} else {
			dependency.Project.Graph.Links.push_back({"future", "value", "new-input", "input_type"});
		}
		dependency.Project.GroupPrebinding = dependency.Project.Graph;
		dependency.Nodes.push_back({"future", "future", false});
		CHECK_FALSE(candidate.Prepare(dependency, live, previous, 4, clock, diagnostic));
		CHECK(diagnostic.Code == Status::UnsupportedExecution);
		CHECK(diagnostic.NodeId == "future");
		CHECK(candidate.Authored == opaqueDocument);
		CHECK(candidate.Host.Revision == opaqueRevision);
		CHECK(candidate.Host.Replay.ObservationRevision() == opaqueObservation);
	}
	auto junction = IncomingGroup(live);
	Node futureNode{"future", "Vendor_Future", "new-group", {}, {}};
	futureNode.SourceInternalName = "future";
	futureNode.DynamicOutputs = {{"value", ValueType::Any}};
	junction.Project.Graph.Nodes.push_back(std::move(futureNode));
	junction.Project.Graph.Junctions.push_back({"opaque-route", "new-group", ValueType::Any, std::nullopt});
	junction.Project.Graph.Links.push_back({"future", "value", "opaque-route", "value"});
	junction.Project.Graph.Links.push_back({"opaque-route", "value", "new-input", "input_type"});
	junction.Project.GroupPrebinding = junction.Project.Graph;
	junction.Nodes.push_back({"future", "future", false});
	CHECK_FALSE(candidate.Prepare(junction, live, previous, 4, clock, diagnostic));
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "future");
	CHECK(candidate.Authored == opaqueDocument);
	CHECK(candidate.Host.Revision == opaqueRevision);
	CHECK(candidate.Host.Replay.ObservationRevision() == opaqueObservation);
}

TEST_CASE(
	"append keeps live detached writers and restores callback topology absent from the saved prebinding",
	"[studio][append_groups][source_identity]"
) {
	auto live = GroupWithLuaAliases();
	Diagnostic diagnostic;
	Plan plan;
	const auto compile = Compile(live, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compile == Status::Ok);
	studio::ImageGraphGroupHost prepared;
	EvaluationRequest clock;
	const auto group = GroupDocument();
	Plan groupPlan;
	REQUIRE(Compile(group, groupPlan, diagnostic) == Status::Ok);
	REQUIRE(prepared.Prepare(group, groupPlan, 1, clock, diagnostic));
	GroupReplayState expanded;
	REQUIRE(RebindGroupReplay(live, prepared.Replay, 1, expanded, diagnostic) == Status::Ok);
	std::vector<GroupSubtypeBinding> bindings;
	for (const auto *id : {"copy", "sibling"})
		for (size_t index = 0; index < 3; ++index)
			bindings.push_back(
				{id,
				 "base",
				 GroupSubtypeAnimator::Animated,
				 GroupSubtypeAnimator::Animated,
				 "argument_value_" + std::to_string(index)}
			);
	GroupReplayState bound;
	REQUIRE(BindGroupReplay(live, bindings, expanded, 1, bound, diagnostic) == Status::Ok);
	auto staged = live;
	StageBaseMove(staged);
	const SourceInputMove moves[] = {
		{"base", "argument_name_1", ""},
		{"base", "argument_type_1", ""},
		{"base", "argument_value_1", ""},
		{"base", "argument_name_2", "argument_name_1"},
		{"base", "argument_type_2", "argument_type_1"},
		{"base", "argument_value_2", "argument_value_1"}
	};
	GroupReplayState detached;
	REQUIRE(
		RebindGroupReplayWithInputMoves(live, staged, moves, bound, 2, detached, diagnostic) == Status::Ok
	);
	REQUIRE(detached.DetachedAnimators().size() == 1);
	const std::string writerId = detached.DetachedAnimators().front().Id;
	REQUIRE(detached.Binding("copy", "argument_value_1"));
	CHECK(detached.Binding("copy", "argument_value_1")->AnimatorPort == writerId);
	const auto *priorWriter = detached.SharedSubtype("base", writerId);
	REQUIRE(priorWriter);
	const auto priorKeys = priorWriter->Keys;
	const Value copyValue = InputValue(staged, detached, "copy");
	const Value siblingValue = InputValue(staged, detached, "sibling");

	studio::ImageGraphGroupHost previous;
	previous.Replay = std::move(detached);
	previous.Revision = 2;
	REQUIRE(previous.Replay.Find("input"));
	const auto priorGroupDomain = previous.Replay.Find("input")->Domain;
	auto appended = IncomingGroup(staged);
	KeepOnlyIncomingCallbacks(appended, staged);
	appended.Project.GroupBindings.push_back(
		{"copy",
		 "archive-base-that-is-stale",
		 GroupSubtypeAnimator::Animated,
		 GroupSubtypeAnimator::Static,
		 "argument_value_1",
		 "argument_value_2"}
	);
	studio::detail::ImageGraphAppendGroups candidate;
	clock.Tick = 1;
	clock.Subframe = .5;
	clock.NegativeFrame = true;
	const bool accepted = candidate.Prepare(appended, staged, previous, 3, clock, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(accepted);
	REQUIRE(candidate.Host.Replay.DetachedAnimators().size() == 1);
	CHECK(candidate.Host.Replay.DetachedAnimators().front().Id == writerId);
	REQUIRE(candidate.Host.Replay.Binding("copy", "argument_value_1"));
	CHECK(candidate.Host.Replay.Binding("copy", "argument_value_1")->AnimatorPort == writerId);
	REQUIRE(candidate.Host.Replay.SharedSubtype("base", writerId));
	CHECK(candidate.Host.Replay.SharedSubtype("base", writerId)->Keys == priorKeys);
	REQUIRE(candidate.Host.Replay.Find("input"));
	CHECK(candidate.Host.Replay.Find("input")->Domain == priorGroupDomain);
	CHECK(InputValue(candidate.Authored, candidate.Host.Replay, "copy") == copyValue);
	CHECK(InputValue(candidate.Authored, candidate.Host.Replay, "sibling") == siblingValue);
	CHECK(
		std::any_of(
			candidate.Authored.Groups.begin(), candidate.Authored.Groups.end(), [](const auto &group) {
				return group.Id == "group";
			}
		)
	);
	CHECK(
		std::any_of(
			candidate.Authored.Junctions.begin(),
			candidate.Authored.Junctions.end(),
			[](const auto &junction) { return junction.Id == "in"; }
		)
	);
	CHECK(std::any_of(candidate.Authored.Nodes.begin(), candidate.Authored.Nodes.end(), [](const auto &node) {
		return node.Id == "output";
	}));
}
