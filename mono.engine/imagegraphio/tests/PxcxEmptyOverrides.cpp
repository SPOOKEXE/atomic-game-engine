#include "SourceCommonAdmission.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_empty_overrides")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	Json Update(Json payload) {
		return Json::array({Json::object(), Json::object(), std::move(payload)});
	}
	Json Owner(const char *id, const char *type, int x) {
		return {
			{"id", id},
			{"type", type},
			{"x", x},
			{"y", 7},
			{"name", id},
			{"inputs", Json::array()},
			{"attri", {{"show_update_trigger", true}, {"outp_meta", true}}},
			{"future_canvas", "preserved"}
		};
	}
	Json Project(bool metadata = false) {
		auto frame = Owner("frame", "Node_Frame", 3);
		frame["inspectInputs"] = Update(Json{{"r", {{"d", true}}}});
		auto text = Owner("text", "Node_Display_Text", 11);
		text["inspectInputs"] = Update(Json{{"from_node", "frame"}, {"from_index", -1}, {"from_tag", -2}});
		if (metadata) {
			text["inputs"] =
				Json::array({Json::object(), {{"from_node", "frame"}, {"from_index", 0}, {"from_tag", -4}}});
		}
		return {{"nodes", Json::array({frame, text})}};
	}
	engine::bake::PxcxArchive Archive(const Json &project) {
		engine::bake::PxcxArchive packet;
		packet.MetadataNumber = 121092;
		packet.MetadataText = "1.22.10.201";
		packet.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(packet, bytes, failure));
		engine::bake::PxcxArchive archive;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		return archive;
	}
	PxcxImport Import(const Json &project) {
		PxcxImport result;
		std::string failure;
		const bool accepted = ImportPxcxImageGraph(Archive(project), result, failure);
		INFO(failure);
		REQUIRE(accepted);
		return result;
	}
	Plan CommonPlan(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = CompileSourceCommonRuntime(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(plan.SourceCommonRuntimeOnly);
		return plan;
	}
	void Run(const Document &document) {
		const auto plan = CommonPlan(document);
		GroupRenderSession session;
		Diagnostic diagnostic;
		REQUIRE(
			InitializeNativeSourceCommonRuntime(
				document, plan, {}, SourceNodeInitialState::Loaded, session, diagnostic
			) == Status::Ok
		);
		REQUIRE(session.Common.Owners.size() == 2);
		for (const auto &owner : session.Common.Owners)
			CHECK_FALSE(owner.Updated);
		for (const auto &node : session.Outputs.Nodes)
			CHECK(node.Outputs.empty());
		const auto status = NativeSourceStepBounded(document, plan, {}, {}, session, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		for (const auto &owner : session.Common.Owners)
			CHECK_FALSE(owner.Updated);
		for (const auto &node : session.Outputs.Nodes)
			CHECK(node.Outputs.empty());
		EvaluatedValue name;
		REQUIRE(
			ReadNativeSourceCommonGetter(
				document, plan, "frame", SourceCommonSelector::Name, {}, session, name, diagnostic
			) == Status::Ok
		);
		CHECK(std::get<std::string>(name.Data) == "frame");
	}
}

TEST_CASE(
	"Frame and Display Text preserve empty source callbacks through actual common routes",
	"[imagegraphio][common]"
) {
	for (const bool metadata : {false, true}) {
		const auto source = Project(metadata);
		const auto imported = Import(source);
		REQUIRE(imported.Graph.SourceCommonOwners.size() == 2);
		for (const auto &node : imported.Graph.Nodes) {
			CHECK_FALSE(HasNativeExecutor(node.Type));
			CHECK(engine::imagegraphio::detail::SourceCommonNativeAvailable(imported.Graph, node.Id));
		}
		const auto plan = CommonPlan(imported.Graph);
		REQUIRE(plan.SourceCommonRoutes.size() == (metadata ? 2 : 1));
		CHECK(
			std::any_of(
				plan.SourceCommonRoutes.begin(), plan.SourceCommonRoutes.end(), [](const auto &route) {
					return route.OwnerId == "frame" && route.Selector == SourceCommonSelector::Update &&
						   route.DestinationUpdate;
				}
			)
		);
		if (metadata)
			CHECK(
				std::any_of(
					plan.SourceCommonRoutes.begin(), plan.SourceCommonRoutes.end(), [](const auto &route) {
						return route.OwnerId == "frame" && route.Selector == SourceCommonSelector::Name;
					}
				)
			);
		Run(imported.Graph);
		Document native;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), native, diagnostic) == Status::Ok);
		Run(native);
		std::vector<std::byte> saved;
		REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, diagnostic));
		CHECK(saved == imported.Source.OriginalBytes);
		engine::bake::PxcxArchive reopened;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(saved, reopened, failure));
		PxcxImport loaded;
		REQUIRE(ImportPxcxImageGraph(reopened, loaded, failure));
		CHECK(loaded.Graph.SourceCommonOwners == imported.Graph.SourceCommonOwners);
		CHECK(loaded.Graph.Links == imported.Graph.Links);
		Run(loaded.Graph);
	}
}

TEST_CASE(
	"Empty common admission requires exact opaque source identity and no invented outputs",
	"[imagegraphio][common]"
) {
	const auto imported = Import(Project());
	for (const auto &[nativeId, sourceType] :
		 {std::pair{"frame", "Node_Frame"}, std::pair{"text", "Node_Display_Text"}}) {
		auto document = imported.Graph;
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
			return candidate.Id == nativeId;
		});
		REQUIRE(node != document.Nodes.end());
		node->Type = "pxcx.opaque/" + std::string(sourceType);
		CHECK(engine::imagegraphio::detail::SourceCommonNativeAvailable(document, nativeId));
		node->Type = "pxcx.opaque/Node_Future_Empty";
		CHECK_FALSE(engine::imagegraphio::detail::SourceCommonNativeAvailable(document, nativeId));
		node->Type = "pxcx.opaque/" + std::string(sourceType);
		node->DynamicOutputs.push_back({"invented", ValueType::Scalar});
		CHECK_FALSE(engine::imagegraphio::detail::SourceCommonNativeAvailable(document, nativeId));
	}
	auto malformed = Project();
	malformed["nodes"][0]["type"] = "Node_Future_Empty";
	const auto unknown = Import(malformed);
	CHECK_FALSE(engine::imagegraphio::detail::SourceCommonNativeAvailable(unknown.Graph, "frame"));
	CHECK(unknown.Graph.Nodes[0].Type == "pxcx.opaque/Node_Future_Empty");
	CHECK_FALSE(HasNativeExecutor(unknown.Graph.Nodes[0].Type));
}

TEST_CASE(
	"Unsupported Frame canvas controls stay opaque while their exact empty callback remains admitted",
	"[imagegraphio][common]"
) {
	auto project = Project();
	project["nodes"][0]["inputs"] = Json::array({{{"r", {{"d", "future frame size"}}}}});
	const auto imported = Import(project);
	REQUIRE(imported.Graph.Nodes.size() == 2);
	const auto &frame = imported.Graph.Nodes[0];
	CHECK(frame.Type == "pxcx.opaque/Node_Frame");
	CHECK_FALSE(HasNativeExecutor(frame.Type));
	CHECK(engine::imagegraphio::detail::SourceCommonNativeAvailable(imported.Graph, "frame"));
	CHECK(frame.DynamicOutputs.empty());
	CHECK(frame.DynamicInputs.empty());
	CHECK(Json::parse(imported.Source.GraphJson.begin(), imported.Source.GraphJson.end() - 1) == project);
	Run(imported.Graph);
	std::vector<std::byte> saved;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, saved, diagnostic));
	CHECK(saved == imported.Source.OriginalBytes);
	const auto retained = imported.Graph;
	auto malformed = imported.Graph;
	malformed.SourceCommonOwners[0].SourceType = "Node_Display_Text";
	CHECK_FALSE(engine::imagegraphio::detail::SourceCommonNativeAvailable(malformed, "frame"));
	const auto malformedPlan = CommonPlan(malformed);
	GroupRenderSession session;
	REQUIRE(
		InitializeNativeSourceCommonRuntime(
			malformed, malformedPlan, {}, SourceNodeInitialState::Loaded, session, diagnostic
		) == Status::Ok
	);
	const auto before = session;
	REQUIRE(before.Replay.Outputs.empty());
	CHECK(
		NativeSourceStepBounded(malformed, malformedPlan, {}, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(diagnostic.NodeId == "frame");
	CHECK(session.Outputs == before.Outputs);
	CHECK(session.Nodes == before.Nodes);
	CHECK(session.Purities == before.Purities);
	CHECK(session.Common == before.Common);
	CHECK(session.CommonAnimators == before.CommonAnimators);
	CHECK(session.SourceCommonWrites == before.SourceCommonWrites);
	CHECK(session.SourceCommonBindings == before.SourceCommonBindings);
	CHECK(session.SourceCommonInputs == before.SourceCommonInputs);
	CHECK(session.Replay.Outputs.empty());
	CHECK(session.Replay.Simulation == before.Replay.Simulation);
	CHECK(session.Replay.Surfaces == before.Replay.Surfaces);
	CHECK(session.Replay.Random == before.Replay.Random);
	CHECK(session.Replay.Data == before.Replay.Data);
	CHECK(session.Replay.Rigid == before.Replay.Rigid);
	CHECK(imported.Graph == retained);
}
