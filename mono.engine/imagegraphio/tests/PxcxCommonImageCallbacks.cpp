#include "SourceCommonAdmission.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_common_image_callbacks")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	Json Solid(bool lifecycle, bool empty = false) {
		Json node = {
			{"id", "solid"},
			{"type", "Node_Solid"},
			{"x", 3},
			{"y", 7},
			{"name", "paint"},
			{"inputs",
			 Json::array(
				 {{{"r", {{"d", Json::array({2, 1})}}}, {"attri", {{"use_project_dimension", 0}}}},
				  {{"r", {{"d", 4278850590ULL}}}},
				  {{"r", {{"d", empty}}}},
				  {{"r", {{"d", -4}}}, {"attri", {{"mask_alpha_only", true}}}},
				  {{"r", {{"d", false}}}},
				  {{"r", {{"d", -4}}}}}
			 )},
			{"future_canvas", "preserved"}
		};
		if (lifecycle) {
			node["attri"] = {{"show_update_trigger", true}, {"outp_meta", true}};
			node["inspectInputs"] = Json::array({Json::object(), Json::object(), {{"r", {{"d", true}}}}});
		}
		return node;
	}
	PxcxImport Import(Json node, bool project = false) {
		engine::bake::PxcxArchive packet;
		packet.MetadataNumber = 121092;
		packet.MetadataText = "1.22.10.201";
		packet.GraphJson = (project ? node : Json{{"nodes", Json::array({std::move(node)})}}).dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(packet, bytes, failure));
		engine::bake::PxcxArchive archive;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		const bool accepted = ImportPxcxImageGraph(archive, imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	void Run(const Document &document, bool empty) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = CompileSourceCommonRuntime(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		GroupRenderSession session;
		REQUIRE(
			InitializeNativeSourceCommonRuntime(
				document, plan, {}, SourceNodeInitialState::Loaded, session, diagnostic
			) == Status::Ok
		);
		const auto stepped = NativeSourceStepBounded(document, plan, {}, {}, session, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(stepped == Status::Ok);
		REQUIRE(session.Outputs.Nodes.size() == 1);
		const auto &outputs = session.Outputs.Nodes[0].Outputs;
		const auto found = std::find_if(outputs.begin(), outputs.end(), [](const auto &value) {
			return value.Port == "surface_out";
		});
		REQUIRE(found != outputs.end());
		REQUIRE(found->Data);
		REQUIRE(std::holds_alternative<SurfaceValue>(*found->Data));
		const auto &image = std::get<SurfaceValue>(*found->Data).Data;
		CHECK(image.Width == 2);
		CHECK(image.Height == 1);
		const std::vector<uint8_t> expected =
			empty ? std::vector<uint8_t>(8, 0) : std::vector<uint8_t>{30, 20, 10, 255, 30, 20, 10, 255};
		CHECK(image.Pixels == expected);
		REQUIRE(session.Common.Owners.size() == 1);
		CHECK_FALSE(session.Common.Owners[0].Updated);
		CHECK_FALSE(session.Ready("solid"));
		EvaluatedValue name;
		REQUIRE(
			ReadNativeSourceCommonGetter(
				document, plan, "solid", SourceCommonSelector::Name, {}, session, name, diagnostic
			) == Status::Ok
		);
		CHECK(std::get<std::string>(name.Data) == "paint");
	}
}
TEST_CASE(
	"Solid common update executes complete source controls across native and PXC reopen",
	"[imagegraphio][common]"
) {
	for (const bool empty : {false, true}) {
		const auto imported = Import(Solid(true, empty));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes[0].Type == "pc.solid");
		CHECK(HasNativeExecutor(imported.Graph.Nodes[0].Type));
		CHECK(engine::imagegraphio::detail::SourceCommonNativeAvailable(imported.Graph, "solid"));
		Run(imported.Graph, empty);
		Document native;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), native, diagnostic) == Status::Ok);
		Run(native, empty);
		std::vector<std::byte> saved;
		REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, diagnostic));
		CHECK(saved == imported.Source.OriginalBytes);
		engine::bake::PxcxArchive reopened;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(saved, reopened, failure));
		PxcxImport loaded;
		REQUIRE(ImportPxcxImageGraph(reopened, loaded, failure));
		CHECK(loaded.Graph.SourceCommonOwners == imported.Graph.SourceCommonOwners);
		Run(loaded.Graph, empty);
	}
}
TEST_CASE("Solid without common lifecycle preserves its legacy image projection", "[imagegraphio][common]") {
	const auto imported = Import(Solid(false));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	CHECK(imported.Graph.Nodes[0].Type == "image.solid");
}
TEST_CASE("Solid metadata refusal preserves the complete common session", "[imagegraphio][common]") {
	auto source = Solid(true);
	source.erase("name");
	const auto imported = Import(std::move(source));
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(CompileSourceCommonRuntime(imported.Graph, plan, diagnostic) == Status::Ok);
	GroupRenderSession session;
	REQUIRE(
		InitializeNativeSourceCommonRuntime(
			imported.Graph, plan, {}, SourceNodeInitialState::Loaded, session, diagnostic
		) == Status::Ok
	);
	const auto before = session;
	CHECK(
		NativeSourceStepBounded(imported.Graph, plan, {}, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "solid");
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(session.Outputs == before.Outputs);
	CHECK(session.Common == before.Common);
	CHECK(session.Nodes == before.Nodes);
	CHECK(session.CommonAnimators == before.CommonAnimators);
	CHECK(session.Replay.Surfaces == before.Replay.Surfaces);
	CHECK(session.Replay.Data == before.Replay.Data);
}

TEST_CASE(
	"Solid reserved Update getter routes select its full source callback projection", "[imagegraphio][common]"
) {
	auto producer = Solid(false);
	producer["id"] = "producer";
	auto consumer = Solid(true);
	consumer["inspectInputs"][2] = {{"from_node", "producer"}, {"from_index", -1}, {"from_tag", -2}};
	const auto imported = Import(Json{{"nodes", Json::array({producer, consumer})}}, true);
	REQUIRE(imported.Graph.Nodes.size() == 2);
	for (const auto &node : imported.Graph.Nodes)
		CHECK(node.Type == "pc.solid");
	Plan plan;
	Diagnostic diagnostic;
	const auto status = CompileSourceCommonRuntime(imported.Graph, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(plan.SourceCommonRoutes.size() == 1);
	CHECK(plan.SourceCommonRoutes[0].OwnerId == "producer");
	CHECK(plan.SourceCommonRoutes[0].Selector == SourceCommonSelector::Update);
	CHECK(plan.SourceCommonRoutes[0].DestinationUpdate);
	std::vector<std::byte> saved;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, diagnostic));
	CHECK(saved == imported.Source.OriginalBytes);
}
