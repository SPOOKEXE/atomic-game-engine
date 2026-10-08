#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.source_group_parent_alias")
using namespace engine::imagegraph;

namespace {
	Document ParentAliases() {
		Document document;
		document.FormatVersion = 10;
		for (const auto *id : {"owner", "target", "leaf"}) {
			document.Nodes.push_back(
				{id,
				 "pc.group_input",
				 "scope",
				 {},
				 {{"input_type", EnumValue{1}},
				  {"subtype", EnumValue{0}},
				  {"vector_size", EnumValue{0}},
				  {"parent_value", .125}}}
			);
		}
		document.Nodes[0].Values.back().Data = .75;
		document.Nodes[1].SourceParentInputBase = "owner";
		document.Nodes[2].SourceParentInputBase = "target";
		Group scope{"scope", "Scope"};
		for (const auto &node : document.Nodes) {
			scope.Ports.push_back({node.Id, node.Id + "/parent", PortDirection::Input, node.Id});
			document.Junctions.push_back({node.Id + "/parent", "scope", ValueType::Any, .125});
		}
		document.Groups.push_back(std::move(scope));
		document.Outputs = {{"target", "target", "value"}, {"leaf", "leaf", "value"}};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	EvaluatedValue Sample(const Document &document, std::string output, EvaluationRequest request = {}) {
		EvaluatedValue value;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(document, Checked(document), output, request, value, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return value;
	}
}

TEST_CASE("Nested Collection parent aliases preserve local Group controls", "[imagegraph][groups]") {
	Document document = ParentAliases();
	document.Nodes[1].Values[0].Data = EnumValue{2};
	const auto target = Sample(document, "target");
	CHECK(std::get<double>(target.Data) == .75);
	REQUIRE(target.Domain);
	CHECK(target.Domain->Type == ValueType::Boolean);
	CHECK(target.Domain->Kind == SourceSocketKind::Boolean);
	CHECK(std::get<double>(Sample(document, "leaf").Data) == .75);
	CHECK(document.Nodes[1].InstanceBase.empty());
	CHECK(document.Nodes[1].Values[0].Data == Value{EnumValue{2}});
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	CHECK(std::get<double>(Sample(parsed, "leaf").Data) == .75);
	document.Nodes[1].InstanceOverrides = {"parent_value"};
	CHECK(std::get<double>(Sample(document, "target").Data) == .125);
	CHECK(std::get<double>(Sample(document, "leaf").Data) == .125);
	document.Nodes[1].InstanceOverrides.clear();
	document.Nodes[0].Values.back().Data = true;
	const auto booleanTarget = Sample(document, "target");
	CHECK(booleanTarget.Data == Value{true});
	REQUIRE(booleanTarget.Domain);
	CHECK(booleanTarget.Domain->Type == ValueType::Boolean);
	CHECK(booleanTarget.Domain->Kind == SourceSocketKind::Boolean);
	CHECK(Sample(document, "leaf").Data == Value{true});
}

TEST_CASE("Parent alias getters follow their owner's source animation", "[imagegraph][groups]") {
	Document document = ParentAliases();
	document.Nodes[0].SourceAnimatedInputs = {"parent_value"};
	document.Keyframes = {
		{"owner", "parent_value", 0, .25, "source", KeyframeEase{}},
		{"owner", "parent_value", 4, .75, "source", KeyframeEase{}}
	};
	document.Tracks = {{"owner", "parent_value", "hold", -1}};
	CHECK(std::get<double>(Sample(document, "leaf", {.Tick = 0}).Data) == .25);
	CHECK(std::get<double>(Sample(document, "leaf", {.Tick = 4}).Data) == .75);
	GroupReplayState empty, local, bound;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(document, empty, 1, local, diagnostic) == Status::Ok);
	const GroupSubtypeBinding bindings[] = {
		{"target", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "parent_value"},
		{"leaf", "owner", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "parent_value"}
	};
	REQUIRE(BindGroupReplay(document, bindings, local, 1, bound, diagnostic) == Status::Ok);
	CHECK(
		std::get<double>(
			Sample(document, "leaf", {.Tick = 4, .GroupReplay = &bound, .GroupAuthoringRevision = 1}).Data
		) == .75
	);
	Value editedValue = .875;
	GroupRefreshEvent edit;
	edit.NodeId = "target";
	edit.EditedPort = "parent_value";
	edit.LocalValue = &editedValue;
	edit.LocalAnimated = true;
	edit.At.Tick = 4;
	GroupReplayState edited;
	REQUIRE(ReplayGroupAnimatorEdits(document, {&edit, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	CHECK(
		std::get<double>(
			Sample(document, "leaf", {.Tick = 4, .GroupReplay = &edited, .GroupAuthoringRevision = 1}).Data
		) == .875
	);
	REQUIRE(edited.SharedSubtype("owner", "parent_value"));
	CHECK(document.Keyframes.back().Data == Value{.75});
	Document projected;
	REQUIRE(ProjectGroupReplay(document, edited, 1, projected, diagnostic) == Status::Ok);
	Document parsed;
	REQUIRE(Read(Write(projected), parsed, diagnostic) == Status::Ok);
	GroupReplayState restored;
	REQUIRE(RestoreSourceAnimatorBindings(parsed, empty, 2, restored, diagnostic) == Status::Ok);
	REQUIRE(restored.Binding("leaf", "parent_value"));
	CHECK(restored.Binding("leaf", "parent_value")->OwnerId == "owner");
	CHECK(
		std::get<double>(
			Sample(parsed, "leaf", {.Tick = 4, .GroupReplay = &restored, .GroupAuthoringRevision = 2}).Data
		) == .875
	);
}

TEST_CASE("Parent aliases retain inherited linked producer routes", "[imagegraph][groups]") {
	Document document = ParentAliases();
	document.Nodes.push_back({"number", "pc.number_simple", "scope", {}, {{"value", .625}}});
	document.Links = {{"number", "number", "owner", "parent_value"}};
	const auto plan = Checked(document);
	CHECK(std::any_of(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [](const auto &link) {
		return link.FromNode == "number" && link.ToNode == "leaf" && link.ToPort == "parent_value";
	}));
	CHECK(std::get<double>(Sample(document, "leaf").Data) == .625);
}

TEST_CASE("Parent input aliases refuse invalid durable ancestry", "[imagegraph][groups]") {
	const Document original = ParentAliases();
	Plan plan;
	Diagnostic diagnostic;
	for (const auto *invalidOwner : {"missing", "target"}) {
		Document document = original;
		document.Nodes[1].SourceParentInputBase = invalidOwner;
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidGroup);
		CHECK(diagnostic.NodeId == "target");
		CHECK(diagnostic.Port == "parent_value");
	}
	Document wrongClass = original;
	wrongClass.Nodes.push_back({"number", "pc.number_simple", "scope", {}, {{"value", .5}}});
	wrongClass.Nodes[1].SourceParentInputBase = "number";
	CHECK(Compile(wrongClass, plan, diagnostic) == Status::InvalidGroup);
	Document oldFormat = original;
	oldFormat.FormatVersion = 9;
	CHECK(Compile(oldFormat, plan, diagnostic) == Status::InvalidGroup);
	CHECK(Write(oldFormat).empty());
	Document wrongTarget = original;
	wrongTarget.Nodes.push_back({"number", "pc.number_simple", "scope", {}, {{"value", .5}}});
	wrongTarget.Nodes.back().SourceParentInputBase = "owner";
	CHECK(Compile(wrongTarget, plan, diagnostic) == Status::InvalidGroup);
}
