#include "ImageGraphHlslGroups.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.hlsl_groups")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Timeline = TimelineSettings{24};
		doc.Nodes = {{"hlsl", "pc.hlsl", {}, {}, {}}};
		for (size_t group = 0; group < 3; ++group) {
			const auto suffix = std::to_string(group);
			for (auto input : std::vector<DynamicInput>{
					 {"argument_name_" + suffix, ValueType::Text, std::string("gain" + suffix)},
					 {"argument_type_" + suffix, ValueType::Enum, EnumValue{0}},
					 {"argument_value_" + suffix, ValueType::Scalar, double(group)}
				 }) {
				input.SourceInputId = "pxc:input:" + std::to_string(5 + doc.Nodes[0].DynamicInputs.size());
				doc.Nodes[0].DynamicInputs.push_back(std::move(input));
			}
			doc.Nodes[0].SourceAnimatedInputs.push_back("argument_value_" + suffix);
			doc.Tracks.push_back({"hlsl", "argument_value_" + suffix, "wrap", -1});
			if (group != 1) {
				doc.Keyframes.push_back(
					{"hlsl", "argument_value_" + suffix, 8 + group, double(group), "source", KeyframeEase{}}
				);
				doc.Keyframes.back().SourceKeyId = "original-key-" + suffix;
			}
		}
		doc.Outputs = {{"out", "hlsl", "surface"}};
		return doc;
	}
}
TEST_CASE(
	"Removing a middle shader triple preserves original empty and keyed record identities",
	"[studio][hlsl_groups]"
) {
	auto doc = Graph();
	const auto original = doc;
	Diagnostic error;
	studio::ImageGraphHistory history;
	REQUIRE(studio::detail::StageHlslGroupRemoval(doc, "hlsl", 1, error));
	REQUIRE(doc.Nodes[0].DynamicInputs.size() == 6);
	for (size_t field = 0; field < 3; ++field) {
		CHECK(
			doc.Nodes[0].DynamicInputs[3 + field].SourceInputId ==
			original.Nodes[0].DynamicInputs[6 + field].SourceInputId
		);
	}
	REQUIRE(doc.Keyframes.size() == 2);
	CHECK(doc.Keyframes[1].Port == "argument_value_1");
	CHECK(doc.Keyframes[1].SourceKeyId == "original-key-2");
	CHECK(doc.Keyframes[1].Tick == 10);
	CHECK(std::get<double>(doc.Keyframes[1].Data) == 2.0);
	CHECK(doc.Tracks.back().Port == "argument_value_1");
	CHECK(
		doc.Nodes[0].SourceAnimatedInputs == std::vector<std::string>{"argument_value_0", "argument_value_1"}
	);
	REQUIRE(history.TryRecord(original, doc));
	const auto removed = doc;
	REQUIRE(history.Undo(doc));
	CHECK_FALSE(history.CanUndo());
	CHECK(doc == original);
	REQUIRE(history.Redo(doc));
	CHECK(doc == removed);
	SECTION("incoming and outgoing bypass routes retain surviving physical records") {
		doc = original;
		doc.Links = {
			{"upstream", "value", "hlsl", "argument_value_1"},
			{"upstream", "value", "hlsl", "argument_value_2"},
			{"hlsl", "argument_value_1.bypass", "sink", "deleted"},
			{"hlsl", "argument_value_2.bypass", "sink", "survivor"},
			{"hlsl", "argument_name_2.bypass", "sink", "name"}
		};
		doc.Outputs.push_back({"deleted", "hlsl", "argument_type_1.bypass"});
		doc.Outputs.push_back({"survivor", "hlsl", "argument_type_2.bypass"});
		REQUIRE(studio::detail::StageHlslGroupRemoval(doc, "hlsl", 1, error));
		REQUIRE(doc.Links.size() == 3);
		CHECK(doc.Links[0].ToPort == "argument_value_1");
		CHECK(doc.Links[1].FromPort == "argument_value_1.bypass");
		CHECK(doc.Links[1].ToPort == "survivor");
		CHECK(doc.Links[2].FromPort == "argument_name_1.bypass");
		REQUIRE(doc.Outputs.size() == 2);
		CHECK(doc.Outputs[1].Id == "survivor");
		CHECK(doc.Outputs[1].Port == "argument_type_1.bypass");
	}
	SECTION("empty original survives moving into the first physical group") {
		doc = original;
		REQUIRE(studio::detail::StageHlslGroupRemoval(doc, "hlsl", 0, error));
		CHECK(
			doc.Nodes[0].DynamicInputs[0].SourceInputId == original.Nodes[0].DynamicInputs[3].SourceInputId
		);
		CHECK(std::none_of(doc.Keyframes.begin(), doc.Keyframes.end(), [](const auto &key) {
			return key.Port == "argument_value_0";
		}));
		REQUIRE(doc.Keyframes.size() == 1);
		CHECK(doc.Keyframes[0].Port == "argument_value_1");
		CHECK(doc.Keyframes[0].SourceKeyId == "original-key-2");
	}
}
TEST_CASE(
	"Canvas-created shader copy preserves original input origins and creates fresh cloned sockets",
	"[studio][hlsl_groups]"
) {
	const auto original = Graph();
	nodegraph::Graph graph;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(original, graph, ids, error));
	const auto *source = graph.Find(ids.ToCanvas.at("hlsl"));
	REQUIRE(source);
	auto copy = *source;
	copy.Id = 2;
	copy.X += 100;
	graph.Adopt(copy);
	REQUIRE(graph.Find(copy.Id));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(graph, original, ids, saved, error));
	REQUIRE(saved.Nodes.size() == 2);
	const auto sourceNode = std::find_if(saved.Nodes.begin(), saved.Nodes.end(), [](const auto &node) {
		return node.Id == "hlsl";
	});
	REQUIRE(sourceNode != saved.Nodes.end());
	CHECK(sourceNode->DynamicInputs == original.Nodes[0].DynamicInputs);
	const auto clone = std::find_if(saved.Nodes.begin(), saved.Nodes.end(), [](const auto &node) {
		return node.Id != "hlsl";
	});
	REQUIRE(clone != saved.Nodes.end());
	REQUIRE(clone->DynamicInputs.size() == 9);
	for (const auto &input : clone->DynamicInputs)
		CHECK(input.SourceInputId.empty());
	CHECK(std::none_of(saved.Keyframes.begin(), saved.Keyframes.end(), [&](const auto &key) {
		return key.NodeId == clone->Id && !key.SourceKeyId.empty();
	}));
	studio::ImageGraphHistory history;
	REQUIRE(history.TryRecord(original, saved));
	const auto copied = saved;
	REQUIRE(history.Undo(saved));
	CHECK(saved == original);
	REQUIRE(history.Redo(saved));
	CHECK(saved == copied);
}

namespace {
	engine::imagegraph::Document AliasedShaderGraph() {
		auto doc = Graph();
		doc.Nodes[0].Id = "base";
		for (auto &key : doc.Keyframes)
			key.NodeId = "base";
		for (auto &track : doc.Tracks)
			track.NodeId = "base";
		for (size_t n = 0; n < 3; ++n) {
			doc.Nodes[0].DynamicInputs[3 * n + 2].Default = double(10 * (n + 1));
		}
		for (auto &track : doc.Tracks)
			track.End = "hold";
		doc.Keyframes = {
			{"base", "argument_value_0", 0, 10., "source", KeyframeEase{}},
			{"base", "argument_value_1", 0, 20., "source", KeyframeEase{}},
			{"base", "argument_value_2", 0, 30., "source", KeyframeEase{}}
		};
		for (size_t n = 0; n < 3; ++n)
			doc.Keyframes[n].SourceKeyId = "original-" + std::to_string(n);
		for (const auto *id : {"copy", "sibling"}) {
			auto copy = doc.Nodes[0];
			copy.Id = id;
			copy.InstanceBase = "base";
			for (auto &input : copy.DynamicInputs)
				input.SourceInputId.clear();
			doc.Nodes.push_back(std::move(copy));
		}
		doc.Nodes.push_back(
			{"solid",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}}
		);
		doc.Outputs = {{"out", "solid", "image"}};
		return doc;
	}
	studio::ImageGraphGroupHost BoundShaderHost(const engine::imagegraph::Document &doc) {
		using namespace engine::imagegraph;
		studio::ImageGraphGroupHost host;
		GroupReplayState initial;
		Diagnostic error;
		REQUIRE(RebindGroupReplay(doc, host.Replay, 1, initial, error) == Status::Ok);
		std::vector<GroupSubtypeBinding> bindings;
		for (const auto *id : {"copy", "sibling"})
			for (size_t n = 0; n < 3; ++n)
				bindings.push_back(
					{id,
					 "base",
					 GroupSubtypeAnimator::Animated,
					 GroupSubtypeAnimator::Animated,
					 "argument_value_" + std::to_string(n)}
				);
		const auto status = BindGroupReplay(doc, bindings, initial, 1, host.Replay, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		host.Revision = 1;
		return host;
	}
	engine::imagegraph::Value ShaderInput(
		const engine::imagegraph::Document &doc,
		const studio::ImageGraphGroupHost &host,
		std::string_view node,
		std::string_view port
	) {
		using namespace engine::imagegraph;
		Plan plan;
		Diagnostic error;
		const auto compiled = Compile(doc, plan, error);
		INFO(error.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.GroupReplay = &host.Replay;
		request.GroupAuthoringRevision = host.Replay.AuthoringRevision();
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, node, request, snapshot, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &v) {
				return v.Port == port;
			});
		REQUIRE(value != snapshot.Values().end());
		return value->Data;
	}
}
TEST_CASE(
	"Shader middle deletion publishes original writers and one undo transition atomically",
	"[studio][hlsl_groups]"
) {
	using namespace engine::imagegraph;
	auto doc = AliasedShaderGraph();
	auto host = BoundShaderHost(doc);
	const auto original = doc;
	const auto oldHost = BoundShaderHost(doc);
	studio::ImageGraphHistory history;
	Diagnostic error;
	SECTION("history and live byte refusals preserve the existing writers") {
		history = studio::ImageGraphHistory{128, 1};
		CHECK_FALSE(studio::detail::ApplyHlslGroupRemoval(doc, history, host, 1, "copy", 1, error));
		CHECK(doc == original);
		CHECK(
			host.Replay.Binding("copy", "argument_value_2")->AnimatorPort ==
			oldHost.Replay.Binding("copy", "argument_value_2")->AnimatorPort
		);
		CHECK(ShaderInput(doc, host, "copy", "argument_value_2") == Value{30.});
		CHECK(!history.CanUndo());
		history = studio::ImageGraphHistory{128, 1024 * 1024};
		CHECK_FALSE(
			studio::detail::ApplyHlslGroupRemoval(doc, history, host, 1, "copy", 1, error, uint64_t{1})
		);
		CHECK(doc == original);
		CHECK(!history.CanUndo());
		return;
	}
	SECTION("the host callback cannot spend beyond a smaller transaction ceiling") {
		constexpr uint64_t ceiling = 1024 * 1024;
		bool callbackSeen = false;
		CHECK_FALSE(
			studio::detail::ApplyHlslGroupRemoval(
				doc,
				history,
				host,
				1,
				"copy",
				1,
				error,
				[&](auto &candidate, auto &candidateHost) {
					callbackSeen = true;
					const auto remaining = candidateHost.Budget(error, {&candidate}, {&candidateHost.Replay});
					CHECK(remaining < ceiling);
					const auto overBudget =
						candidateHost.Budget(error, {&candidate}, {&candidateHost.Replay}, ceiling);
					CHECK(overBudget == 0);
					return overBudget != 0;
				},
				ceiling
			)
		);
		REQUIRE(callbackSeen);
		CHECK(doc == original);
		CHECK_FALSE(history.CanUndo());
		CHECK(ShaderInput(doc, host, "copy", "argument_value_2") == Value{30.});
		return;
	}
	SECTION("a refused host reconciliation preserves staged writer identities") {
		CHECK_FALSE(
			studio::detail::ApplyHlslGroupRemoval(
				doc, history, host, 1, "copy", 1, error, [&](auto &candidate, auto &candidateHost) {
					CHECK(candidate.Nodes[1].DynamicInputs.size() == 6);
					CHECK(
						candidateHost.Replay.Binding("copy", "argument_value_1")->AnimatorPort ==
						"argument_value_2"
					);
					error = {Status::InvalidValue, "copy", {}, "reconciliation refused"};
					return false;
				}
			)
		);
		CHECK(doc == original);
		CHECK_FALSE(history.CanUndo());
		CHECK(ShaderInput(doc, host, "copy", "argument_value_2") == Value{30.});
		return;
	}
	SECTION("decreasing the group count retires multiple physical triples in one transition") {
		REQUIRE(
			studio::detail::ApplyHlslGroupRangeRemoval(
				doc, history, host, 1, "copy", 1, 2, error, [](auto &, auto &) { return true; }
			)
		);
		CHECK(doc.Nodes[1].DynamicInputs.size() == 3);
		CHECK(host.Replay.Binding("copy", "argument_value_1") == nullptr);
		CHECK(host.Replay.Binding("copy", "argument_value_2") == nullptr);
		CHECK(ShaderInput(doc, host, "sibling", "argument_value_2") == Value{30.});
		REQUIRE(history.Undo(doc));
		CHECK_FALSE(history.CanUndo());
		CHECK(doc == original);
		REQUIRE(history.Redo(doc));
		CHECK(doc.Nodes[1].DynamicInputs.size() == 3);
		return;
	}
	REQUIRE(studio::detail::ApplyHlslGroupRemoval(doc, history, host, 1, "copy", 1, error));
	REQUIRE(history.CanUndo());
	CHECK(ShaderInput(doc, host, "copy", "argument_value_1") == Value{20.});
	CHECK(host.Replay.Binding("copy", "argument_value_1")->AnimatorPort == "argument_value_2");
	Value replacement = 99.;
	GroupRefreshEvent event;
	event.NodeId = "copy";
	event.EditedPort = "argument_value_1";
	event.LocalValue = &replacement;
	event.LocalAnimated = true;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(doc, {&event, 1}, host.Replay, 2, edited, error) == Status::Ok);
	host.Replay = std::move(edited);
	CHECK(ShaderInput(doc, host, "sibling", "argument_value_2") == replacement);
	CHECK(ShaderInput(doc, host, "copy", "argument_value_1") == Value{20.});
	const auto removed = doc;
	REQUIRE(history.Undo(doc));
	CHECK_FALSE(history.CanUndo());
	CHECK(doc == original);
	REQUIRE(history.Redo(doc));
	CHECK(doc == removed);
	Document projected, reloaded;
	REQUIRE(ProjectGroupReplay(doc, host.Replay, 2, projected, error) == Status::Ok);
	REQUIRE(Read(Write(projected), reloaded, error) == Status::Ok);
	CHECK(reloaded == projected);
}
TEST_CASE(
	"Deleting a shader writer retains its alias through paired mode and native reload",
	"[studio][hlsl_groups]"
) {
	using namespace engine::imagegraph;
	auto doc = AliasedShaderGraph();
	auto host = BoundShaderHost(doc);
	const auto oldHost = BoundShaderHost(doc);
	const auto original = doc;
	studio::ImageGraphHistory history;
	Diagnostic error;
	const auto status = studio::detail::ApplyHlslGroupRemoval(doc, history, host, 1, "base", 1, error);
	INFO(error.Message);
	REQUIRE(status);
	CHECK(ShaderInput(doc, host, "copy", "argument_value_1") == Value{30.});
	const auto *binding = host.Replay.Binding("copy", "argument_value_1");
	REQUIRE(binding);
	CHECK(binding->AnimatorPort != "argument_value_1");
	CHECK(ShaderInput(original, oldHost, "copy", "argument_value_1") == Value{20.});
	Document fixed;
	GroupReplayState fixedReplay;
	REQUIRE(
		ToggleSourceInputMode(
			doc, host.Replay, 2, {"copy", "argument_value_1", false, {}}, fixed, fixedReplay, error
		) == Status::Ok
	);
	Plan plan;
	REQUIRE(Compile(fixed, plan, error) == Status::Ok);
	studio::ImageGraphGroupHost fixedHost;
	fixedHost.Replay = std::move(fixedReplay);
	fixedHost.Revision = 2;
	// Without an override the getter still follows the current source index.
	CHECK(ShaderInput(fixed, fixedHost, "copy", "argument_value_1") == Value{30.});
	const auto *retainedWriter = fixedHost.Replay.SharedSubtype("base", binding->AnimatorPort);
	REQUIRE(retainedWriter);
	REQUIRE(retainedWriter->Keys.size() == 1);
	CHECK(retainedWriter->Keys.front().Data == Value{20.});
	REQUIRE(fixedHost.Replay.DetachedAnimators().size() == 1);
	CHECK(fixedHost.Replay.DetachedAnimators().front().Writer == GroupSubtypeAnimator::Animated);
	CHECK(ShaderInput(fixed, fixedHost, "base", "argument_value_1") == Value{30.});
	Document projected, reloaded;
	REQUIRE(ProjectGroupReplay(fixed, fixedHost.Replay, 2, projected, error) == Status::Ok);
	REQUIRE(Read(Write(projected), reloaded, error) == Status::Ok);
	CHECK(reloaded == projected);
	CHECK(
		reloaded.Nodes[0].DynamicInputs[3].SourceInputId == original.Nodes[0].DynamicInputs[6].SourceInputId
	);
	const auto removed = doc;
	REQUIRE(history.Undo(doc));
	CHECK_FALSE(history.CanUndo());
	CHECK(doc == original);
	REQUIRE(history.Redo(doc));
	CHECK(doc == removed);
}
