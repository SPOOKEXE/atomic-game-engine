#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_mirror_path_ratio")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::json;
	PxcxImport Imported(bool animated = false, bool empty = false) {
		Json inputs = Json::array();
		for (size_t i = 0; i < 16; ++i)
			inputs.push_back(Json::object());
		inputs[0] = {{"from_node", "noise"}, {"from_index", 0}};
		inputs[1] = {
			{"from_node", "number"},
			{"from_index", 0},
			{"unit", 1},
			{"anim", animated},
			{"future", "keep-local"}
		};
		if (animated) {
			inputs[1]["r"] = Json::array(
				{Json::array(
					 {Json::array({0, 0}),
					  Json::array({.25, .1}),
					  Json::array({0, 1}),
					  Json::array({0, 0}),
					  0,
					  0,
					  true,
					  0,
					  16777215}
				 ),
				 Json::array(
					 {Json::array({0, 2}),
					  Json::array({.75, .9}),
					  Json::array({0, 1}),
					  Json::array({0, 0}),
					  0,
					  0,
					  true,
					  0,
					  16777215}
				 )}
			);
		} else
			inputs[1]["r"] = {{"d", Json::array({.25, .9})}};
		if (empty) inputs[1]["r"] = Json::array();
		Json root = {
			{"animator", {{"frames_total", 30}, {"playback", 1}, {"framerate", 30}}},
			{"nodes",
			 Json::array(
				 {{{"id", "number"},
				   {"type", "Node_Number_Simple"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs", Json::array({Json{{"r", {{"d", .5}}}}})}},
				  {{"id", "noise"},
				   {"type", "Node_Noise_Simplex"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs", Json::array()}},
				  {{"id", "mirror"}, {"type", "Node_Mirror_Polar"}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		};
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = root.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		const auto node =
			std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &n) {
				return n.Id == "mirror";
			});
		REQUIRE(node != imported.Graph.Nodes.end());
		INFO(failure);
		REQUIRE(node->Type == "pc.mirror_polar");
		return imported;
	}
	PxcxImport ReadEdited(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
	const Node &Mirror(const PxcxImport &project) {
		const auto node =
			std::find_if(project.Graph.Nodes.begin(), project.Graph.Nodes.end(), [](const auto &n) {
				return n.Id == "mirror";
			});
		REQUIRE(node != project.Graph.Nodes.end());
		return *node;
	}
	Json Center(const PxcxImport &project) {
		const auto text =
			std::string_view(project.Source.GraphJson.data(), project.Source.GraphJson.size() - 1);
		const Json root = Json::parse(text);
		for (const auto &node : root["nodes"])
			if (node["id"] == "mirror") return node["inputs"][1];
		FAIL("source Mirror is missing");
		return {};
	}
}
TEST_CASE("PXC linked Mirror preserves editable local ratio and source connection", "[pxcx_mirror_ratio]") {
	const auto imported = Imported();
	const auto &node = Mirror(imported);
	const auto raw = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &v) {
		return v.Port == "center";
	});
	REQUIRE(raw != node.Values.end());
	CHECK(raw->Data == Value{Vector2{.25, .9}});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(imported.Graph), restored, diag) == Status::Ok);
	CHECK(restored == imported.Graph);
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"mirror", "center", Vector2{.75, .1}}};
	std::vector<std::byte> bytes;
	const auto ok = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diag);
	INFO(diag.Message);
	REQUIRE(ok);
	const auto replay = ReadEdited(bytes);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["from_index"] == 0);
	CHECK(center["unit"] == 1);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"]["d"] == Json::array({.75, .1}));
	CHECK(replay.Graph.Links == imported.Graph.Links);
}
TEST_CASE("PXC linked Mirror animator key inverse retains ordered raw tuples", "[pxcx_mirror_ratio]") {
	const auto imported = Imported(true);
	const auto first =
		std::find_if(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "mirror" && key.Port == "center";
		});
	REQUIRE(first != imported.Graph.Keyframes.end());
	Keyframe replacement = *first;
	replacement.Data = Vector2{.125, .8};
	const std::array<PxcxEdit, 1> edits{
		PxcxKeyframeEdit{"mirror", "center", GetFrameTime(*first), replacement}
	};
	Diagnostic diag;
	std::vector<std::byte> bytes;
	const auto ok = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, diag);
	INFO(diag.Message);
	REQUIRE(ok);
	const auto replay = ReadEdited(bytes);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["anim"] == true);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"][0][1] == Json::array({.125, .8}));
	CHECK(center["r"][1][1] == Json::array({.75, .9}));
	CHECK(replay.Graph.Links == imported.Graph.Links);
}

TEST_CASE("PXC empty saved Mirror animator reloads the source constructor ratio", "[pxcx_mirror_ratio]") {
	const auto imported = Imported(true, true);
	const auto &node = Mirror(imported);
	CHECK(std::none_of(node.Values.begin(), node.Values.end(), [](const auto &value) {
		return value.Port == "center";
	}));
	CHECK(std::none_of(imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "mirror" && key.Port == "center";
	}));
	CHECK(
		std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), "center") !=
		node.SourceAnimatedInputs.end()
	);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);
	CHECK(Center(imported)["r"] == Json::array());
}
TEST_CASE(
	"PXC full projection rewrites linked Mirror animator without disconnecting", "[pxcx_mirror_ratio]"
) {
	const auto imported = Imported(true);
	auto desired = imported.Graph;
	const auto key = std::find_if(desired.Keyframes.begin(), desired.Keyframes.end(), [](const auto &k) {
		return k.NodeId == "mirror" && k.Port == "center";
	});
	REQUIRE(key != desired.Keyframes.end());
	key->Data = Vector2{.125, .875};
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool saved = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message << " " << diagnostic.NodeId << ":" << diagnostic.Port);
	REQUIRE(saved);
	const auto replay = ReadEdited(bytes);
	CHECK(replay.Graph == desired);
	const auto center = Center(replay);
	CHECK(center["from_node"] == "number");
	CHECK(center["unit"] == 1);
	CHECK(center["future"] == "keep-local");
	CHECK(center["r"][0][1] == Json::array({.125, .875}));
}
