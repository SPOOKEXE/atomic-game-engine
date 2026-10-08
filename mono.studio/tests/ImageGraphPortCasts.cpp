#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Graph.hpp>
#include <nodegraph/Registry.hpp>
#include <string>
#include <studio/ImageGraph.hpp>
#include <utility>

TEST_SUITE_ID("studio.imagegraph.port_casts")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("studio.nodegraph.graph")

namespace {
	using namespace engine::imagegraph;

	Document NumberToBoolean(double number) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"number", "pc.number_simple", {}, {}, {{"value", number}}},
			{"alternate", "pc.number_simple", {}, {}, {{"value", 0.0}}},
			{"boolean", "pc.boolean", {}, {}, {{"value", false}}}
		};
		document.Links = {{"number", "number", "boolean", "value"}};
		document.Outputs = {{"result", "boolean", "boolean"}};
		return document;
	}

	bool EvaluateBoolean(const Document &document) {
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue result;
		REQUIRE(EvaluateValue(document, plan, "result", {}, result, diagnostic) == Status::Ok);
		REQUIRE(std::holds_alternative<bool>(result.Data));
		return std::get<bool>(result.Data);
	}

	Document Reopen(const Document &document) {
		Document reopened;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), reopened, diagnostic) == Status::Ok);
		return reopened;
	}
}

TEST_CASE(
	"Catalogue junction casts stay visible and editable after save and reopen", "[studio][imagegraph]"
) {
	auto authored = NumberToBoolean(3.0);
	CHECK(EvaluateBoolean(authored));

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(authored, canvas, ids, error));
	REQUIRE(ids.UnmappedLinks.empty());
	REQUIRE(canvas.LinkInto(ids.ToCanvas.at("boolean"), "value"));

	REQUIRE(canvas.Disconnect(ids.ToCanvas.at("boolean"), "value"));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("alternate"), "number", ids.ToCanvas.at("boolean"), "value") ==
		nodegraph::LinkResult::Made
	);
	canvas.Attach({ids.ToCanvas.at("alternate"), "number", ids.ToCanvas.at("boolean"), "value"});

	Document edited;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, authored, ids, edited, error));
	REQUIRE(edited.Links.size() == 1);
	CHECK(edited.Links.front() == (Link{"alternate", "number", "boolean", "value"}));
	CHECK(edited.Nodes == authored.Nodes);
	CHECK_FALSE(EvaluateBoolean(Reopen(edited)));

	nodegraph::Graph reopenedCanvas;
	studio::ImageGraphCanvasIds reopenedIds;
	const auto reopened = Reopen(edited);
	REQUIRE(studio::LoadImageGraphCanvas(reopened, reopenedCanvas, reopenedIds, error));
	REQUIRE(reopenedIds.UnmappedLinks.empty());
	CHECK(reopenedCanvas.LinkInto(reopenedIds.ToCanvas.at("boolean"), "value"));
	Document resaved;
	REQUIRE(studio::SaveImageGraphCanvas(reopenedCanvas, reopened, reopenedIds, resaved, error));
	CHECK(resaved == reopened);
}

TEST_CASE("Catalogue casts are directional and native endpoints stay strict", "[studio][imagegraph]") {
	studio::RegisterImageGraphNodeTypes();
	nodegraph::Graph canvas;
	const auto numberNode = canvas.Add("pc.number_simple", 0.0f, 0.0f);
	const auto booleanNode = canvas.Add("pc.boolean", 200.0f, 0.0f);
	REQUIRE(numberNode != nodegraph::NO_NODE);
	REQUIRE(booleanNode != nodegraph::NO_NODE);
	CHECK(canvas.CanConnect(numberNode, "number", booleanNode, "value") == nodegraph::LinkResult::Made);

	const auto textNode = canvas.Add("pc.string", 400.0f, 0.0f);
	REQUIRE(textNode != nodegraph::NO_NODE);
	CHECK(canvas.CanConnect(numberNode, "number", textNode, "text") == nodegraph::LinkResult::Made);
	CHECK(canvas.CanConnect(textNode, "text", numberNode, "value") == nodegraph::LinkResult::TypeMismatch);

	const auto sample = canvas.Add("value.sample_noise", 600.0f, 0.0f);
	const auto vector2 = canvas.Add("pc.vector2", 800.0f, 0.0f);
	REQUIRE(sample != nodegraph::NO_NODE);
	REQUIRE(vector2 != nodegraph::NO_NODE);
	CHECK(canvas.CanConnect(numberNode, "number", sample, "position") == nodegraph::LinkResult::Made);
	CHECK(canvas.CanConnect(vector2, "vector", sample, "position") == nodegraph::LinkResult::Made);
	CHECK(canvas.CanConnect(booleanNode, "boolean", sample, "position") != nodegraph::LinkResult::Made);

	Document native;
	native.FormatVersion = 9;
	native.Nodes = {
		{"comparison", "value.compare", {}, {}, {{"a", 2.0}, {"b", 1.0}}},
		{"noise", "value.noise_field", {}, {}, {}}
	};
	native.Links = {{"comparison", "result", "noise", "seed"}};
	native.Outputs = {{"result", "comparison", "result"}};
	Diagnostic nativeDiagnostic;
	Plan nativePlan;
	const auto nativeStatus = Compile(native, nativePlan, nativeDiagnostic);
	INFO(nativeDiagnostic.Message);
	CHECK(nativeStatus == Status::TypeMismatch);

	Document mixed;
	mixed.FormatVersion = 9;
	mixed.Nodes = {
		{"source", "pc.boolean", {}, {}, {{"value", true}}}, {"noise", "value.noise_field", {}, {}, {}}
	};
	mixed.Outputs = {{"result", "source", "boolean"}};
	Document mixedLinked = mixed;
	mixedLinked.Links = {{"source", "boolean", "noise", "seed"}};
	Diagnostic diagnostic;
	Plan plan;
	const auto mixedStatus = Compile(mixedLinked, plan, diagnostic);
	INFO(diagnostic.Message);
	CHECK(mixedStatus == Status::Ok);
	nodegraph::Graph mixedCanvas;
	studio::ImageGraphCanvasIds mixedIds;
	std::string mixedError;
	REQUIRE(studio::LoadImageGraphCanvas(mixed, mixedCanvas, mixedIds, mixedError));
	CHECK(mixedIds.UnmappedLinks.empty());
	CHECK(
		mixedCanvas.CanConnect(
			mixedIds.ToCanvas.at("source"), "boolean", mixedIds.ToCanvas.at("noise"), "seed"
		) == nodegraph::LinkResult::Made
	);
}

TEST_CASE("Noise coordinates keep strict scalar unions despite catalogue casts", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.boolean", {}, {}, {{"value", true}}},
		{"noise", "value.noise_field", {}, {}, {{"mode", EnumValue{1}}, {"position", 0.0}}}
	};
	document.Links = {{"source", "boolean", "noise", "position"}};
	document.Outputs = {{"result", "source", "boolean"}};

	Diagnostic diagnostic;
	Plan plan;
	const auto coordinateStatus = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	CHECK(coordinateStatus == Status::TypeMismatch);

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("source"), "boolean", ids.ToCanvas.at("noise"), "position") !=
		nodegraph::LinkResult::Made
	);
}

TEST_CASE("Malformed dynamic input types remain unconnectable on the canvas", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	Node dynamic;
	dynamic.Id = "dynamic";
	dynamic.Type = "pc.pxc";
	dynamic.DynamicInputs = {{"invalid", static_cast<ValueType>(255), std::nullopt}};
	document.Nodes = {{"number", "pc.number_simple", {}, {}, {{"value", 1.0}}}, std::move(dynamic)};
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	const auto *node = canvas.Find(ids.ToCanvas.at("dynamic"));
	REQUIRE(node);
	REQUIRE(node->DynamicInputs.size() == 1);
	CHECK_FALSE(nodegraph::DataTypes::Find(node->DynamicInputs.front().Type));
	CHECK(
		canvas.CanConnect(ids.ToCanvas.at("number"), "number", node->Id, "invalid") ==
		nodegraph::LinkResult::TypeMismatch
	);
	Document saved;
	CHECK_FALSE(studio::SaveImageGraphCanvas(canvas, document, ids, saved, error));
}
