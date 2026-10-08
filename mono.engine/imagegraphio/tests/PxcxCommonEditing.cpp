#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcxcommonediting")

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
	"Common local authoring roundtrips mixed owners without runtime socket payloads", "[imagegraphio][common]"
) {
	const auto imported = Import(Source());
	auto edited = imported.Graph;
	REQUIRE(edited.SourceCommonOwners.size() == 4);
	auto &collectionOwner = edited.SourceCommonOwners[1];
	collectionOwner.ShowUpdateTrigger = true;
	collectionOwner.OutMeta = true;
	collectionOwner.UpdateGraph = false;
	collectionOwner.UpdateOverrideInstance = true;
	collectionOwner.UpdateExpression =
		SourceInputExpression{"pxcx.update_in_trigger", "retained disabled code", false};
	auto collection = std::find_if(edited.Groups.begin(), edited.Groups.end(), [](const auto &group) {
		return group.Id == "collection";
	});
	REQUIRE(collection != edited.Groups.end());
	collection->SourcePosition = {101, 103};
	collection->SourceInternalName = "renamed_namespace";
	collection->Name = "";
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, edited, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto reopened = Import(Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1));
	const auto &owner = reopened.Graph.SourceCommonOwners[1];
	CHECK(owner.SourceOwnerId == "collection");
	CHECK(owner.ShowUpdateTrigger);
	CHECK(owner.OutMeta);
	CHECK_FALSE(owner.UpdateGraph);
	CHECK(owner.UpdateOverrideInstance);
	CHECK(owner.UpdateExpression == collectionOwner.UpdateExpression);
	CHECK(owner.DisplayNamePresent);
	CHECK_FALSE(reopened.Graph.SourceCommonOwners[2].DisplayNamePresent);
	const auto group =
		std::find_if(reopened.Graph.Groups.begin(), reopened.Graph.Groups.end(), [](const auto &value) {
			return value.Id == "collection";
		});
	REQUIRE(group != reopened.Graph.Groups.end());
	CHECK(group->SourcePosition == Vector2{101, 103});
	CHECK(group->SourceInternalName == "renamed_namespace");
	CHECK(group->Name.empty());
	const auto &keys = Animator(reopened.Graph, owner).Keys;
	REQUIRE(keys.size() == 2);
	CHECK(GetFrameTime(keys[0]) == FrameTime{});
	CHECK(keys[0].Data == Value{false});
	CHECK(GetFrameTime(keys[1]) == FrameTime{4, 0, false});
	CHECK(keys[1].Data == Value{true});
	const auto raw = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	CHECK(raw["nodes"][1]["inspectInputs"].size() == 3);
	CHECK_FALSE(raw["nodes"][1].contains("updatedInTrigger"));
	CHECK_FALSE(raw["nodes"][1].contains("outMeta"));
	CHECK_FALSE(raw["nodes"][1].contains("outputs"));
}

TEST_CASE(
	"Common route editing admits native getters and removes only Update inspector links",
	"[imagegraphio][common]"
) {
	const auto imported = Import(Json{{"nodes", Json::array({Number("from"), Number("to")})}});
	auto edited = imported.Graph;
	edited.Links.push_back({"from", "pxcx.updated_out_trigger", "to", "pxcx.update_in_trigger"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(CompileSourceCommonRuntime(edited, plan, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, edited, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	REQUIRE(archive.Links.size() == 1);
	CHECK(archive.Links[0].FromTag == -3);
	CHECK(archive.Links[0].SourceTriggerIndexMinusOne);
	CHECK(archive.Links[0].DestinationUpdateTrigger);
	auto reopened = Import(Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1));
	CHECK(reopened.Graph.Links == edited.Links);
	CHECK(std::all_of(reopened.Graph.Nodes.begin(), reopened.Graph.Nodes.end(), [](const auto &node) {
		return !node.Type.starts_with("pxcx.opaque/");
	}));
	REQUIRE(CompileSourceCommonRuntime(reopened.Graph, plan, diagnostic) == Status::Ok);
	auto removed = reopened.Graph;
	removed.Links.clear();
	REQUIRE(WritePxcxProjection(reopened, removed, {}, bytes, diagnostic));
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	CHECK(archive.Links.empty());
	const auto raw = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	CHECK_FALSE(raw["nodes"][1]["inspectInputs"][2].contains("from_node"));
	CHECK(raw["nodes"][1]["inputs"][0]["r"]["d"] == 2.5);
}

TEST_CASE(
	"Common append preserves source order remaps Update endpoints and keeps local physical writers",
	"[imagegraphio][common]"
) {
	auto source = Source();
	source["nodes"][2]["inspectInputs"] = Update(
		Json{
			{"anim", true},
			{"r", Json::array({Key(7, false)})},
			{"from_node", "first"},
			{"from_index", -1},
			{"from_tag", -2}
		}
	);
	const auto destination = Archive(Json{{"nodes", Json::array({Number("existing")})}});
	const auto incoming = Archive(source);
	PxcxAppendOptions options;
	options.Namespace = "common-copy";
	options.Offset = {11, 13};
	PxcxAppendResult result;
	Diagnostic diagnostic;
	const bool appended = AppendPxcxProject(destination, incoming, options, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(appended);
	const auto &owners = result.Project.Graph.SourceCommonOwners;
	REQUIRE(owners.size() == 5);
	CHECK(owners[0].SourceOwnerId == "existing");
	REQUIRE(result.Nodes.size() == 4);
	for (size_t index = 0; index < result.Nodes.size(); ++index) {
		CHECK(owners[index + 1].SourceOwnerId == result.Nodes[index].NodeId);
		CHECK(owners[index + 1].UpdateAnimatorOwnerId == result.Nodes[index].NodeId);
		CHECK(owners[index + 1].UpdateAnimatorPort != owners[0].UpdateAnimatorPort);
	}
	const auto &last = owners[3];
	const auto &payload = Animator(result.Project.Graph, last);
	REQUIRE(payload.Keys.size() == 1);
	CHECK(payload.Keys[0].NodeId == last.SourceOwnerId);
	CHECK(payload.Keys[0].Port == last.UpdateAnimatorPort);
	CHECK(GetFrameTime(payload.Keys[0]) == FrameTime{7, 0, false});
	CHECK(payload.Keys[0].Data == Value{false});
	const auto route = std::find_if(
		result.Project.Source.Links.begin(), result.Project.Source.Links.end(), [](const auto &link) {
			return link.DestinationUpdateTrigger;
		}
	);
	REQUIRE(route != result.Project.Source.Links.end());
	CHECK(route->FromNode == owners[1].SourceOwnerId);
	CHECK(route->ToNode == last.SourceOwnerId);
	CHECK(route->FromTag == -2);
	CHECK(route->SourceTriggerIndexMinusOne);
	const auto group = std::find_if(
		result.Project.Graph.Groups.begin(), result.Project.Graph.Groups.end(), [&](const auto &value) {
			return value.Id == owners[2].NativeOwnerId;
		}
	);
	REQUIRE(group != result.Project.Graph.Groups.end());
	CHECK(group->SourcePosition == Vector2{28, 36});
	CHECK(group->SourceInternalName == "source_collection_namespace");
	CHECK(owners[2].DisplayNamePresent);
	CHECK(group->Name.empty());
}

TEST_CASE("Unsupported common lifecycle edits preserve the previous archive", "[imagegraphio][common]") {
	const auto imported = Import(Source());
	auto edited = imported.Graph;
	edited.SourceCommonOwners[1].Active = false;
	const std::vector<std::byte> sentinel{std::byte{0x77}};
	auto bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(imported, edited, {}, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK(diagnostic.Message == "PXC common owner requires its active source constructor");
}

TEST_CASE(
	"PXC common writer reconstruction refuses unrelated captured generations", "[imagegraphio][common]"
) {
	const auto imported = Import(Json{{"nodes", Json::array({Number("number")})}});
	auto edited = imported.Graph;
	REQUIRE(bool(edited.SourceAnimators));
	DetachedSourceAnimator retired;
	retired.Id = "native:animator:900";
	retired.OwnerId = "number";
	retired.OriginalPort = "value";
	retired.Writer = GroupSubtypeAnimator::Static;
	retired.Type = ValueType::Scalar;
	edited.SourceAnimators->Detached.push_back(retired);
	GroupSubtypeOverlay payload;
	payload.NodeId = "number";
	payload.Port = retired.Id;
	payload.Fixed = 17.0;
	edited.SourceAnimators->DetachedValues.push_back(payload);
	const std::vector<std::byte> sentinel{std::byte{0x77}};
	auto bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(imported, edited, {}, bytes, diagnostic));
	CHECK(bytes == sentinel);
	CHECK(
		diagnostic.Message == "PXC cannot reconstruct captured animator generations; save the native document"
	);
}

TEST_CASE(
	"Existing structural Group common sockets survive native route edits and reopen", "[imagegraphio][common]"
) {
	auto group = Json{
		{"id", "group"},
		{"type", "Node_Group"},
		{"x", 17},
		{"y", 23},
		{"name", ""},
		{"iname", "group_namespace"},
		{"inputs", Json::array()}
	};
	const auto imported = Import(Json{{"nodes", Json::array({group, Number("to")})}});
	REQUIRE(imported.Graph.Groups.size() == 1);
	auto edited = imported.Graph;
	edited.Links.push_back({"group", "pxcx.updated_out_trigger", "to", "pxcx.update_in_trigger"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(CompileSourceCommonRuntime(edited, plan, diagnostic) == Status::Ok);
	std::vector<std::byte> bytes;
	const bool saved = WritePxcxProjection(imported, edited, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto reopened = Import(Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1));
	CHECK(reopened.Graph.Links == edited.Links);
	CHECK(reopened.Graph.Nodes.size() == 1);
	CHECK(reopened.Graph.Groups.size() == 1);
	CHECK(reopened.Graph.Groups.front().SourcePosition == Vector2{17, 23});
	CHECK(reopened.Graph.Groups.front().SourceInternalName == "group_namespace");
	CHECK(reopened.Graph.SourceCommonOwners.front().NativeOwnerKind == SourceCommonNativeOwnerKind::Group);
	CHECK(reopened.Graph.SourceCommonOwners.front().DisplayNamePresent);
	REQUIRE(CompileSourceCommonRuntime(reopened.Graph, plan, diagnostic) == Status::Ok);
	CHECK(archive.Nodes.size() == 2);
}

TEST_CASE(
	"Common route retargeting preserves reserved tag precedence and its authored raw index",
	"[imagegraphio][common]"
) {
	auto target = Number("old");
	target["inspectInputs"] = Update(Json{{"from_node", "from"}, {"from_index", 1008}, {"from_tag", -2.0}});
	const auto imported = Import(Json{{"nodes", Json::array({Number("from"), target, Number("new")})}});
	REQUIRE(imported.Graph.Links.size() == 1);
	CHECK(imported.Graph.Links.front().FromPort == "pxcx.update_in_trigger");
	auto edited = imported.Graph;
	edited.Links.front().ToNode = "new";
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, edited, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	REQUIRE(archive.Links.size() == 1);
	CHECK(archive.Links.front().FromTag == -2);
	CHECK(archive.Links.front().FromIndex == 1008);
	CHECK_FALSE(archive.Links.front().SourceTriggerIndexMinusOne);
	CHECK(archive.Links.front().DestinationUpdateTrigger);
	const auto raw = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	CHECK(raw["nodes"][2]["inspectInputs"][2]["from_tag"].is_number_float());
	CHECK(raw["nodes"][2]["inspectInputs"][2]["from_index"] == 1008);
	CHECK_FALSE(raw["nodes"][1]["inspectInputs"][2].contains("from_node"));
	const auto reopened = Import(raw);
	CHECK(reopened.Graph.Links == edited.Links);
}

TEST_CASE(
	"Mixed common owner source rows survive unrelated position edits without rewriting local writers",
	"[imagegraphio][common]"
) {
	const auto source = Source();
	const auto imported = Import(source);
	auto edited = imported.Graph;
	const auto node = std::find_if(edited.Nodes.begin(), edited.Nodes.end(), [](const auto &row) {
		return row.Id == "first";
	});
	REQUIRE(node != edited.Nodes.end());
	node->Position = {31, 42};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, edited, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(saved);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto raw = Json::parse(archive.GraphJson.begin(), archive.GraphJson.end() - 1);
	auto expected = source;
	expected["nodes"][0]["x"] = 31;
	expected["nodes"][0]["y"] = 42;
	REQUIRE(raw["nodes"].size() == expected["nodes"].size());
	for (size_t index = 0; index < raw["nodes"].size(); ++index) {
		CHECK(raw["nodes"][index]["id"] == expected["nodes"][index]["id"]);
		CHECK(raw["nodes"][index]["x"] == expected["nodes"][index]["x"]);
		CHECK(raw["nodes"][index]["y"] == expected["nodes"][index]["y"]);
		CHECK(
			raw["nodes"][index].contains("inspectInputs") ==
			expected["nodes"][index].contains("inspectInputs")
		);
		if (expected["nodes"][index].contains("inspectInputs"))
			CHECK(raw["nodes"][index]["inspectInputs"] == expected["nodes"][index]["inspectInputs"]);
	}
	const auto reopened = Import(raw);
	REQUIRE(reopened.Graph.SourceCommonOwners.size() == 4);
	for (size_t index = 0; index < 4; ++index) {
		CHECK(
			reopened.Graph.SourceCommonOwners[index].SourceOwnerId ==
			imported.Graph.SourceCommonOwners[index].SourceOwnerId
		);
		CHECK(
			Animator(reopened.Graph, reopened.Graph.SourceCommonOwners[index]) ==
			Animator(imported.Graph, imported.Graph.SourceCommonOwners[index])
		);
	}
}

TEST_CASE(
	"Common inverse bounds long shared owner identities without replacing saved bytes",
	"[imagegraphio][common]"
) {
	Json source{{"nodes", Json::array()}};
	const std::string prefix(960, 'a');
	for (size_t index = 0; index < 128; ++index) {
		const std::string id = prefix + std::to_string(index);
		source["nodes"].push_back(Number(id.c_str()));
	}
	const auto imported = Import(source);
	REQUIRE(imported.Graph.SourceCommonOwners.size() == 128);
	const auto baseline = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	auto edited = baseline;
	edited.Nodes.front().Position.X += 1;
	const auto wanted = edited;
	std::vector<std::byte> bytes{std::byte{0x17}, std::byte{0x39}};
	const auto retained = bytes;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(imported, edited, {}, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC common authoring exceeds its transaction work limit");
	CHECK(bytes == retained);
	CHECK(edited == wanted);
	CHECK(imported.Graph == baseline);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}
