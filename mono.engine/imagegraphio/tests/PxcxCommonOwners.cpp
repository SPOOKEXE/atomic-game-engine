#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcxcommonowners")

using namespace engine::imagegraph;
using namespace engine::imagegraphio;

namespace {
	using Json = nlohmann::json;
	Json Number(const char *id) {
		return {
			{"id", id},
			{"type", "Node_Number_Simple"},
			{"x", 3},
			{"y", 4},
			{"inputs", Json::array({Json{{"r", {{"d", 2.5}}}}})}
		};
	}
	Json Key(double frame, bool data) {
		return Json::array(
			{Json::array({0, frame}), data, Json::array({0, 1}), Json::array({0, 0}), 0, 0, true, 0}
		);
	}
	Json Update(Json record) {
		return Json::array({Json::object(), Json::object(), std::move(record)});
	}
	Json Source() {
		auto first = Number("first");
		first["name"] = "";
		first["inspectInputs"] = Update(
			Json{
				{"r", Json::array({Key(0, true)})},
				{"attri", {{"override_instance", true}}},
				{"global_key", "disabled code"},
				{"global_use", false}
			}
		);
		auto collection = Json{
			{"id", "collection"},
			{"type", "Node_Collection"},
			{"name", ""},
			{"iname", "source_collection_namespace"},
			{"x", 17},
			{"y", 23},
			{"inputs", Json::array()}
		};
		collection["inspectInputs"] =
			Update(Json{{"anim", true}, {"r", Json::array({Key(0, false), Key(4, true)})}});
		auto last = Number("last");
		auto builder = Json{
			{"id", "builder"}, {"type", "Node_Pixel_Builder"}, {"x", 31}, {"y", 37}, {"inputs", Json::array()}
		};
		return {{"nodes", Json::array({first, collection, last, builder})}};
	}
	engine::bake::PxcxArchive Archive(const Json &source) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = source.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		return archive;
	}
	PxcxImport Import(const Json &source) {
		PxcxImport result;
		std::string failure;
		const bool accepted = ImportPxcxImageGraph(Archive(source), result, failure);
		INFO(failure);
		REQUIRE(accepted);
		return result;
	}
	const GroupSubtypeOverlay &Animator(const Document &document, const SourceCommonOwnerRecord &owner) {
		REQUIRE(bool(document.SourceAnimators));
		const auto &values = document.SourceAnimators->DetachedValues;
		const auto value = std::find_if(values.begin(), values.end(), [&](const auto &row) {
			return row.NodeId == owner.UpdateAnimatorOwnerId && row.Port == owner.UpdateAnimatorPort;
		});
		REQUIRE(value != values.end());
		return *value;
	}
}

TEST_CASE(
	"Common owner projection preserves source order names and unique mixed bindings", "[imagegraphio][common]"
) {
	const auto imported = Import(Source());
	const auto &document = imported.Graph;
	const auto &owners = document.SourceCommonOwners;
	REQUIRE(owners.size() == 4);
	CHECK(owners[0].SourceOwnerId == "first");
	CHECK(owners[1].SourceOwnerId == "collection");
	CHECK(owners[2].SourceOwnerId == "last");
	CHECK(owners[3].SourceOwnerId == "builder");
	CHECK(owners[0].NativeOwnerKind == SourceCommonNativeOwnerKind::Node);
	CHECK(owners[1].NativeOwnerKind == SourceCommonNativeOwnerKind::Group);
	CHECK(owners[3].NativeOwnerKind == SourceCommonNativeOwnerKind::Node);
	CHECK(owners[0].DisplayNamePresent);
	CHECK(owners[1].DisplayNamePresent);
	CHECK_FALSE(owners[2].DisplayNamePresent);
	const auto collection =
		std::find_if(document.Groups.begin(), document.Groups.end(), [](const auto &group) {
			return group.Id == "collection";
		});
	REQUIRE(collection != document.Groups.end());
	CHECK(collection->Name.empty());
	CHECK(collection->SourcePosition == Vector2{17, 23});
	CHECK(collection->SourceInternalName == "source_collection_namespace");
	REQUIRE(bool(document.SourceAnimators));
	CHECK(document.SourceAnimators->Detached.size() == owners.size());
	CHECK(document.SourceAnimators->DetachedValues.size() == owners.size());
	for (const auto &owner : owners) {
		CHECK(owner.Active);
		CHECK(owner.UpdateAnimatorOwnerId == owner.SourceOwnerId);
		CHECK(owner.UpdateAnimatorPort.starts_with("native:animator:"));
	}
	CHECK(owners[0].UpdateOverrideInstance);
	REQUIRE(owners[0].UpdateExpression.has_value());
	CHECK_FALSE(owners[0].UpdateExpression->Enabled);
	CHECK(owners[0].UpdateExpression->Code == "disabled code");
	const auto &animated = Animator(document, owners[1]);
	REQUIRE(animated.Keys.size() == 2);
	CHECK(animated.Keys[0].Port == owners[1].UpdateAnimatorPort);
	CHECK(animated.Keys[0].NodeId == "collection");
	CHECK(animated.Keys[0].Data == Value{false});
	CHECK(animated.Keys[1].Data == Value{true});
	CHECK_FALSE(Animator(document, owners[2]).Fixed.has_value());
	CHECK(Animator(document, owners[2]).Keys.empty());
}

TEST_CASE("Linked inherited common Update retains its own local setter animator", "[imagegraphio][common]") {
	auto source = Source();
	auto &last = source["nodes"][2];
	last["instanceBase"] = "first";
	last["attri"] = {{"show_update_trigger", true}, {"outp_meta", true}};
	last["inspectInputs"] =
		Update(Json{{"r", {{"d", false}}}, {"from_node", "first"}, {"from_tag", -2}, {"from_index", -1}});
	const auto imported = Import(source);
	const auto &owner = imported.Graph.SourceCommonOwners[2];
	CHECK(owner.InstanceBase == "first");
	CHECK(owner.ShowUpdateTrigger);
	CHECK(owner.OutMeta);
	CHECK_FALSE(owner.UpdateOverrideInstance);
	CHECK(owner.UpdateAnimatorOwnerId == "last");
	CHECK_FALSE(Animator(imported.Graph, owner).Fixed.has_value());
	REQUIRE(Animator(imported.Graph, owner).Keys.size() == 1);
	CHECK(Animator(imported.Graph, owner).Keys.front().Data == Value{false});
	REQUIRE(imported.Graph.Links.size() == 1);
	CHECK(imported.Graph.Links.front().FromPort == "pxcx.update_in_trigger");
	CHECK(imported.Graph.Links.front().ToPort == "pxcx.update_in_trigger");
	for (const auto &node : imported.Graph.Nodes)
		if (node.Id == "first" || node.Id == "last") CHECK(node.Type == "pc.number_simple");
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = CompileSourceCommonRuntime(imported.Graph, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	CHECK(plan.SourceCommonRuntimeOnly);
	REQUIRE(plan.SourceCommonRoutes.size() == 1);
	CHECK(plan.SourceCommonRoutes.front().OwnerId == "first");
	CHECK(plan.SourceCommonRoutes.front().Selector == SourceCommonSelector::Update);
	CHECK(plan.SourceCommonRoutes.front().NodeId == "last");
	CHECK(plan.SourceCommonRoutes.front().Port == "pxcx.update_in_trigger");
	CHECK(plan.SourceCommonRoutes.front().DestinationUpdate);
}

TEST_CASE("Malformed common animator flags do not replace the prior projection", "[imagegraphio][common]") {
	auto imported = Import(Source());
	const auto previous = imported.Graph;
	const auto previousBytes = imported.Source.OriginalBytes;
	auto source = Source();
	source["nodes"][0]["inspectInputs"][2]["anim"] = "yes";
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(Archive(source), imported, failure));
	CHECK(failure == "common source flag is not boolean: anim");
	CHECK(imported.Graph == previous);
	CHECK(imported.Source.OriginalBytes == previousBytes);
}

TEST_CASE(
	"Compact and empty Trigger records preserve exactly one physical animator payload",
	"[imagegraphio][common]"
) {
	auto source = Source();
	source["nodes"][0]["inspectInputs"][2] = Json{{"r", {{"d", false}}}};
	source["nodes"][2]["inspectInputs"] = Update(Json{{"anim", true}, {"r", Json::array()}});
	const auto imported = Import(source);
	const auto &compact = Animator(imported.Graph, imported.Graph.SourceCommonOwners[0]);
	CHECK_FALSE(compact.Fixed.has_value());
	REQUIRE(compact.Keys.size() == 1);
	CHECK(compact.Keys.front().Data == Value{false});
	const auto &empty = Animator(imported.Graph, imported.Graph.SourceCommonOwners[2]);
	CHECK_FALSE(empty.Fixed.has_value());
	CHECK(empty.Keys.empty());
}

TEST_CASE(
	"Group common visibility and wrapper flags are retained locally without inheritance",
	"[imagegraphio][common]"
) {
	auto source = Source();
	source["nodes"][1]["attri"] = {
		{"show_update_trigger", true}, {"outp_meta", true}, {"update_graph", false}
	};
	const auto imported = Import(source);
	const auto &owner = imported.Graph.SourceCommonOwners[1];
	CHECK(owner.SourceType == "Node_Collection");
	CHECK(owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Group);
	CHECK(owner.ShowUpdateTrigger);
	CHECK(owner.OutMeta);
	CHECK_FALSE(owner.UpdateGraph);
	CHECK_FALSE(imported.Graph.SourceCommonOwners[0].ShowUpdateTrigger);
	CHECK_FALSE(imported.Graph.SourceCommonOwners[0].OutMeta);
	CHECK(imported.Graph.SourceCommonOwners[0].UpdateGraph);
	auto malformed = source;
	malformed["nodes"][1]["attri"]["show_update_trigger"] = 1;
	auto candidate = imported;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(Archive(malformed), candidate, failure));
	CHECK(failure == "source group common attribute show_update_trigger is not boolean");
	CHECK(candidate.Graph == imported.Graph);
}

TEST_CASE("Common Trigger projection retains numeric physical key payloads", "[imagegraphio][common]") {
	auto node = Number("numeric");
	node["inspectInputs"] = Update(Json{{"anim", true}, {"r", Json::array({Key(3, false)})}});
	node["inspectInputs"][2]["r"][0][1] = 0.0;
	const auto imported = Import(Json{{"nodes", Json::array({node})}});
	const auto &owner = imported.Graph.SourceCommonOwners.front();
	const auto &payload = Animator(imported.Graph, owner);
	REQUIRE(payload.Keys.size() == 1);
	CHECK(std::holds_alternative<double>(payload.Keys.front().Data));
	CHECK(payload.Keys.front().Data == Value{0.0});
}

TEST_CASE(
	"Unsupported common owners stay opaque but malformed native common routes refuse atomically",
	"[imagegraphio][common]"
) {
	auto producer = Number("from");
	producer["type"] = "Unknown_Future_Node";
	auto target = Number("to");
	target["inspectInputs"] = Update(Json{{"from_node", "from"}, {"from_index", -1}, {"from_tag", -3}});
	const auto retained = Import(Json{{"nodes", Json::array({producer, target})}});
	CHECK(retained.Graph.Nodes[0].Type.starts_with("pxcx.opaque/"));
	CHECK(retained.Graph.Links.front().FromPort == "pxcx.updated_out_trigger");
	CHECK(retained.Graph.Links.front().ToPort == "pxcx.update_in_trigger");
	auto prior = retained;
	target["inspectInputs"][2]["from_tag"] = -4;
	target["inspectInputs"][2]["from_index"] = 2;
	const auto malformed = Archive(Json{{"nodes", Json::array({Number("from"), target})}});
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(malformed, prior, failure));
	CHECK(prior.Graph == retained.Graph);
	CHECK(prior.Source.OriginalBytes == retained.Source.OriginalBytes);
	CHECK_FALSE(failure.empty());
}
