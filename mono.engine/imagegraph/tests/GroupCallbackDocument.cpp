#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.group_callback_document")
TEST_DEPENDS("engine.imagegraph.group_replay")

using namespace engine::imagegraph;

TEST_CASE(
	"Group callback scratch keeps known nodes and maps unknown records to refusing output markers",
	"[imagegraph][group_callback_document]"
) {
	Document source;
	source.FormatVersion = 9;
	Node known{"known", "pc.number", "nested", {3, 4}, {{"value", 7.0}}};
	Node unknown{"future", "Vendor_Future", "nested", {-5, 11}, {}};
	unknown.SourceDisplayName = "Future node";
	unknown.SourceInternalName = "future";
	unknown.DynamicInputs = {
		{"future-input", ValueType::Any, Value{std::string{"preserved only in source"}}}
	};
	unknown.DynamicOutputs = {{"future-output", ValueType::Any}};
	unknown.SourceInputExpressions = {{"future-input", "19", true}};
	source.Nodes = {known, unknown};
	source.Groups = {{"nested", "Nested", {}, {}}};
	source.Outputs = {{"opaque-result", "future", "future-output"}};
	const auto before = source;

	Document scratch;
	Diagnostic diagnostic;
	const auto prepared = PrepareGroupCallbackDocument(source, scratch, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(prepared == Status::Ok);
	CHECK(source == before);
	REQUIRE(scratch.Nodes.size() == 2);
	CHECK(scratch.Nodes.front() == known);
	const auto &marker = scratch.Nodes.back();
	CHECK(marker.Id == unknown.Id);
	CHECK(marker.Type == "internal.group_opaque");
	CHECK(marker.GroupId == unknown.GroupId);
	CHECK(marker.Position == unknown.Position);
	CHECK(marker.SourceDisplayName == unknown.SourceDisplayName);
	CHECK(marker.SourceInternalName == unknown.SourceInternalName);
	CHECK((marker.Values == std::vector<AuthoredValue>{{"source_type", std::string{"Vendor_Future"}}}));
	CHECK(marker.DynamicInputs.empty());
	CHECK(marker.DynamicOutputs.empty());
	CHECK(marker.SourceInputExpressions.empty());
	CHECK(scratch.Outputs == source.Outputs);

	Plan plan;
	REQUIRE(Compile(scratch, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	CHECK(
		EvaluateValue(scratch, plan, "opaque-result", {}, result, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "future");

	Document inPlace = source;
	REQUIRE(PrepareGroupCallbackDocument(inPlace, inPlace, diagnostic) == Status::Ok);
	REQUIRE(inPlace.Nodes.size() == 2);
	CHECK(inPlace.Nodes.back().Type == "internal.group_opaque");
}

TEST_CASE(
	"Group callback scratch byte refusal preserves the caller result",
	"[imagegraph][group_callback_document][atomic]"
) {
	Document source;
	source.FormatVersion = 9;
	source.Nodes = {{"future", "Vendor_Future", "group", {1, 2}, {}}};
	Document result;
	result.FormatVersion = 9;
	result.Nodes = {{"sentinel", "pc.number", {}, {8, 9}, {{"value", 42.0}}}};
	const auto prior = result;
	Diagnostic diagnostic;
	CHECK(PrepareGroupCallbackDocument(source, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == prior);

	Document inPlace = source;
	const auto inPlacePrior = inPlace;
	CHECK(PrepareGroupCallbackDocument(inPlace, inPlace, diagnostic, 1) == Status::LimitExceeded);
	CHECK(inPlace == inPlacePrior);
}
