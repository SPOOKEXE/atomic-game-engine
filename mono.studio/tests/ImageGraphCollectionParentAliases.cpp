#include "../src/ImageGraphAppendGroups.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("studio.imagegraph.collection_parent_aliases")
TEST_DEPENDS("studio.imagegraph.group_host")
TEST_DEPENDS("studio.imagegraph.append_groups")
TEST_DEPENDS("engine.imagegraph.group_replay")

using namespace engine::imagegraph;

namespace {
	Document ParentAliases() {
		Document document;
		document.FormatVersion = 10;
		for (const auto *id : {"owner", "copy", "child"}) {
			const std::string group = std::string(id) + "-group";
			const std::string junction = std::string(id) + "/parent-value";
			Node input{
				id,
				"pc.group_input",
				group,
				{},
				{{"input_type", EnumValue{1}},
				 {"subtype", EnumValue{0}},
				 {"vector_size", EnumValue{0}},
				 {"parent_value", .5}}
			};
			input.SourceStaticInputs = {"parent_value"};
			if (input.Id == "copy") input.SourceParentInputBase = "owner";
			if (input.Id == "child") input.SourceParentInputBase = "copy";
			document.Nodes.push_back(std::move(input));
			document.Groups.push_back({group, group, {}, {{"input", junction, PortDirection::Input, id}}});
			document.Junctions.push_back({junction, group, ValueType::Any, .5});
			document.Outputs.push_back({id, id, "value"});
		}
		return document;
	}

	void Prepare(Document &document, studio::ImageGraphGroupHost &host, uint64_t revision) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		const bool prepared = host.Prepare(document, plan, revision, request, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(prepared);
	}
	void CheckChildValue(
		Document &document, studio::ImageGraphGroupHost &host, uint64_t revision, double expected
	) {
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluationRequest request;
		request.Tick = 10;
		REQUIRE(host.Prepare(document, plan, revision, request, diagnostic));
		EvaluatedValue observed;
		const auto status = EvaluateValue(document, plan, "child", request, observed, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(std::get<double>(observed.Data) == expected);
	}

}

TEST_CASE(
	"Studio Collection parent aliases bind only parent values across rebind and save",
	"[studio][imagegraph][collection]"
) {
	auto document = ParentAliases();
	document.Nodes[0].SourceStaticInputs.clear();
	document.Nodes[0].SourceAnimatedInputs = {"parent_value"};
	document.Keyframes = {
		{"owner", "parent_value", 0, .25, "source", KeyframeEase{}},
		{"owner", "parent_value", 10, .75, "source", KeyframeEase{}}
	};
	document.Tracks = {{"owner", "parent_value", "hold", -1}};
	studio::ImageGraphGroupHost host;
	Prepare(document, host, 1);
	for (const auto *id : {"copy", "child"}) {
		const auto *binding = host.Replay.Binding(id, "parent_value");
		REQUIRE(binding);
		CHECK(binding->OwnerId == "owner");
		CHECK(binding->Getter == GroupSubtypeAnimator::Animated);
		CHECK_FALSE(host.Replay.Binding(id, "input_type"));
		CHECK_FALSE(host.Replay.Binding(id, "subtype"));
	}
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	for (const auto tick : {uint64_t{0}, uint64_t{10}, uint64_t{0}}) {
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(host.Prepare(document, plan, 1, request, diagnostic));
		EvaluatedValue evaluated;
		const auto status = EvaluateValue(document, plan, "child", request, evaluated, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(std::get<double>(evaluated.Data) == (tick ? .75 : .25));
	}
	Document saved;
	REQUIRE(host.ProjectForSave(document, 1, saved, diagnostic));
	CHECK(saved.Nodes[1].SourceParentInputBase == "owner");
	CHECK(saved.Nodes[2].SourceParentInputBase == "copy");
	CHECK(saved.Nodes[1].InstanceBase.empty());
	Document reopened;
	REQUIRE(Read(Write(saved), reopened, diagnostic) == Status::Ok);
	CHECK(reopened.Nodes[1].SourceParentInputBase == "owner");
	CHECK(reopened.Nodes[2].SourceParentInputBase == "copy");
	saved = std::move(reopened);
	studio::ImageGraphGroupHost restored;
	Prepare(saved, restored, 2);
	REQUIRE(restored.Replay.Binding("child", "parent_value"));
	CHECK(restored.Replay.Binding("child", "parent_value")->OwnerId == "owner");
	Prepare(saved, restored, 3);
	CHECK(restored.Replay.Bindings().size() == 2);
}

TEST_CASE(
	"Studio Collection detached parent getter survives history without aliasing declarations",
	"[studio][imagegraph][collection]"
) {
	auto document = ParentAliases();
	document.Nodes[0].SourceAnimatedInputs = {"parent_value"};
	document.Nodes[0].SourceStaticInputs.clear();
	document.Keyframes = {
		{"owner", "parent_value", 0, .25, "source", KeyframeEase{}},
		{"owner", "parent_value", 10, .75, "source", KeyframeEase{}}
	};
	document.Tracks = {{"owner", "parent_value", "hold", -1}};
	document.Nodes[1].InstanceOverrides = {"parent_value"};
	studio::ImageGraphGroupHost host;
	Prepare(document, host, 1);
	REQUIRE(host.Replay.Binding("child", "parent_value"));
	CHECK(host.Replay.Binding("child", "parent_value")->Getter == GroupSubtypeAnimator::Static);
	CHECK(host.Replay.Binding("child", "parent_value")->OwnerId == "owner");
	CHECK(host.Replay.Binding("child", "parent_value")->Writer == GroupSubtypeAnimator::Animated);
	// grug keeps the shared animator; a static getter reads its first key.
	CheckChildValue(document, host, 1, .25);
	CHECK(document.Nodes[1].Values.back().Data == Value{.5});
	REQUIRE(document.Keyframes.size() == 2);
	CHECK(document.Keyframes.front().Data == Value{.25});
	const auto before = document;
	auto changed = document;
	changed.Nodes[1].InstanceOverrides.clear();
	studio::ImageGraphHistory history;
	REQUIRE(history.TryRecord(before, changed));
	document = changed;
	Prepare(document, host, 2);
	CHECK(host.Replay.Binding("child", "parent_value")->Getter == GroupSubtypeAnimator::Animated);
	CheckChildValue(document, host, 2, .75);
	REQUIRE(history.Undo(document));
	host.Clear();
	Prepare(document, host, 3);
	CHECK(host.Replay.Binding("child", "parent_value")->Getter == GroupSubtypeAnimator::Static);
	CheckChildValue(document, host, 3, .25);
	REQUIRE(history.Redo(document));
	host.Clear();
	Prepare(document, host, 4);
	CHECK(host.Replay.Binding("child", "parent_value")->Getter == GroupSubtypeAnimator::Animated);
	CheckChildValue(document, host, 4, .75);
}

TEST_CASE(
	"Studio append builds incoming Collection parent bindings from durable remapped IDs",
	"[studio][imagegraph][collection][append]"
) {
	auto document = ParentAliases();
	studio::ImageGraphGroupHost previous;
	Prepare(document, previous, 1);
	engine::imagegraphio::PxcxAppendResult append;
	append.Project.Graph = document;
	Node input = document.Nodes[1];
	input.Id = "incoming-copy";
	input.GroupId = "incoming-group";
	input.SourceParentInputBase = "child";
	append.Project.Graph.Nodes.push_back(input);
	append.Project.Graph.Groups.push_back(
		{"incoming-group",
		 "Incoming",
		 {},
		 {{"incoming-input", "incoming-junction", PortDirection::Input, "incoming-copy"}}}
	);
	append.Project.Graph.Junctions.push_back({"incoming-junction", "incoming-group", ValueType::Any, .5});
	append.Nodes = {{"source-copy", "incoming-copy", false}};
	append.Project.GroupBootstrap = {{"incoming-copy", GroupSubtypeAnimator::Static}};
	studio::detail::ImageGraphAppendGroups candidate;
	Diagnostic diagnostic;
	const bool accepted = candidate.Prepare(append, document, previous, 2, {}, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(accepted);
	const auto *binding = candidate.Host.Replay.Binding("incoming-copy", "parent_value");
	REQUIRE(binding);
	CHECK(binding->OwnerId == "owner");
	CHECK(candidate.Authored.Nodes.back().SourceParentInputBase == "child");
	REQUIRE(candidate.Host.Replay.Binding("copy", "parent_value"));
	CHECK(candidate.Host.Replay.Binding("copy", "parent_value")->OwnerId == "owner");
	const auto retained = candidate.Host.Replay.RetainedBytes();
	CHECK_FALSE(candidate.Prepare(append, document, previous, 3, {}, diagnostic, 1));
	CHECK(candidate.Host.Revision == 2);
	CHECK(candidate.Host.Replay.RetainedBytes() == retained);
}
