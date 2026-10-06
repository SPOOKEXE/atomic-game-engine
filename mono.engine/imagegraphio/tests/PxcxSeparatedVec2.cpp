#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_separated_vec2")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::json;
	PxcxImport Checked(const Json &root) {
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
		return imported;
	}
}
TEST_CASE("PXC separated Vec2 IVec2 and Range edits preserve inactive tracks", "[pxcx_vec2_axes]") {
	for (const auto &[type, port] : std::array<std::pair<const char *, const char *>, 4>{
			 {{"pc.solid", "dimension"},
			  {"pc.box_pattern", "position"},
			  {"pc.matrix", "size"},
			  {"pc.colorize", "color_range"}}
		 }) {
		INFO(type << ':' << port);
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const auto *input = FindCatalogueInput(*entry, port);
		REQUIRE(input);
		REQUIRE(input->SourceIndex >= 0);
		Json inputs = Json::array();
		for (int32_t i = 0; i <= input->SourceIndex; ++i)
			inputs.push_back(Json::object());
		inputs[input->SourceIndex] = {
			{"sep_axis", true},
			{"anim", false},
			{"r", {{"d", Json::array({2, 3})}}},
			{"animators", Json::array({Json{{"d", 2.25}}, Json{{"d", 3.5}}, Json{{"opaque_axis", "keep"}}})},
			{"opaque_input", "keep"}
		};
		Json root = {
			{"nodes",
			 Json::array(
				 {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		};
		auto imported = Checked(root);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		INFO((imported.Diagnostics.empty() ? "" : imported.Diagnostics.front().Message));
		REQUIRE(imported.Graph.Nodes[0].Type == type);
		REQUIRE(imported.Graph.Nodes[0].SourceSeparatedVec2Animators);
		const auto &before = imported.Graph.Nodes[0].SourceSeparatedVec2Animators->Inputs[0];
		CHECK(before.Port == port);
		CHECK(before.Separated);
		CHECK(before.Axes[0].Keys[0].Data == Value{2.25});
		CHECK(before.Axes[1].Keys[0].Data == Value{3.5});
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		auto desired = imported.Graph;
		auto &axes = desired.Nodes[0].SourceSeparatedVec2Animators->Inputs[0];
		axes.Separated = false;
		axes.Axes[0].Keys[0].Data = .875;
		std::vector<std::byte> bytes;
		const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport reopened;
		REQUIRE(ImportPxcxImageGraph(checked, reopened, failure));
		CHECK(reopened.Graph == desired);
		const auto saved =
			Json::parse(std::string_view(checked.GraphJson.data(), checked.GraphJson.size() - 1));
		const auto &record = saved["nodes"][0]["inputs"][input->SourceIndex];
		CHECK(record["sep_axis"] == false);
		CHECK(record["animators"][2]["opaque_axis"] == "keep");
		CHECK(record["opaque_input"] == "keep");
		CHECK(record["r"] == inputs[input->SourceIndex]["r"]);
	}
}

TEST_CASE("PXC IVec2 preserves only represented constant unit modes", "[pxcx_vec2_axes]") {
	const auto *entry = FindCatalogueEntry("pc.matrix");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "size");
	REQUIRE(input);
	Json record = {{"r", {{"d", Json::array({2, 3})}}}, {"anim", false}};
	Json inputs = Json::array();
	for (int32_t i = 0; i <= input->SourceIndex; ++i)
		inputs.push_back(Json::object());
	inputs[input->SourceIndex] = record;
	Json root = {
		{"nodes",
		 Json::array(
			 {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
		 )}
	};
	CHECK(Checked(root).Graph.Nodes.front().Type == "pc.matrix");
	root["nodes"][0]["inputs"][input->SourceIndex]["unit"] = 0;
	CHECK(Checked(root).Graph.Nodes.front().Type == "pc.matrix");
	for (const Json &unit : std::array<Json, 3>{Json(1), Json("reference"), Json(true)}) {
		root["nodes"][0]["inputs"][input->SourceIndex]["unit"] = unit;
		const auto imported = Checked(root);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes.front().Type != "pc.matrix");
		REQUIRE_FALSE(imported.Diagnostics.empty());
		CHECK(imported.Diagnostics.front().Message.find("unit") != std::string::npos);
		CHECK(imported.Source.GraphJson == root.dump() + '\0');
	}
}
