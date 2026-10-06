#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("PXC Vec2 constructor defaults stay separate from the current animator value", "[pxcx_vec2_axes]") {
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "dimension");
	REQUIRE(input);
	const auto importRecord = [&](Json defaultValue, bool separated = false) {
		Json inputs = Json::array();
		for (int32_t i = 0; i <= input->SourceIndex; ++i)
			inputs.push_back(Json::object());
		Json record = {
			{"anim", false},
			{"sep_axis", separated},
			{"def_val", std::move(defaultValue)},
			{"r", {{"d", Json::array({99, 88})}}}
		};
		if (separated) record["animators"] = Json::array({Json::array(), Json::array()});
		inputs[input->SourceIndex] = std::move(record);
		return Checked(
			{{"nodes",
			  Json::array(
				  {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			  )}}
		);
	};
	const auto currentDimension = [](Document &document) -> AuthoredValue * {
		auto &values = document.Nodes.front().Values;
		const auto found = std::find_if(values.begin(), values.end(), [](const auto &value) {
			return value.Port == "dimension";
		});
		return found == values.end() ? nullptr : &*found;
	};

	auto imported = importRecord(Json::array({7, 8}));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	const auto &node = imported.Graph.Nodes.front();
	REQUIRE(node.Type == "pc.solid");
	REQUIRE(node.SourceVec2Defaults);
	REQUIRE(node.SourceVec2Defaults->Inputs.size() == 1);
	CHECK(node.SourceVec2Defaults->Inputs.front() == SourceVec2Default{"dimension", Vector2{7, 8}});
	CHECK_FALSE(node.SourceSeparatedVec2Animators);
	REQUIRE(currentDimension(imported.Graph));
	CHECK(currentDimension(imported.Graph)->Data == Value{Vector2{99, 88}});
	auto separated = importRecord(Json::array({7, 8}), true);
	REQUIRE(separated.Graph.Nodes.size() == 1);
	const auto &separatedNode = separated.Graph.Nodes.front();
	REQUIRE(separatedNode.Type == "pc.solid");
	REQUIRE(separatedNode.SourceVec2Defaults);
	CHECK(separatedNode.SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	REQUIRE(separatedNode.SourceSeparatedVec2Animators);
	REQUIRE(separatedNode.SourceSeparatedVec2Animators->Inputs.size() == 1);
	const auto &separatedInput = separatedNode.SourceSeparatedVec2Animators->Inputs.front();
	CHECK(separatedInput.Initialized);
	const auto &axes = separatedInput.Axes;
	REQUIRE(axes[0].Keys.size() == 1);
	REQUIRE(axes[1].Keys.size() == 1);
	CHECK(axes[0].Keys.front().Data == Value{7.0});
	CHECK(axes[1].Keys.front().Data == Value{8.0});
	REQUIRE(currentDimension(separated.Graph));
	CHECK(currentDimension(separated.Graph)->Data == Value{Vector2{99, 88}});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);

	Document edited = imported.Graph;
	auto *dimension = currentDimension(edited);
	REQUIRE(dimension);
	dimension->Data = Vector2{101, 102};
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, edited, {}, bytes, diagnostic));
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(checked, reopened, failure));
	REQUIRE(reopened.Graph.Nodes.front().SourceVec2Defaults);
	CHECK(reopened.Graph.Nodes.front().SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	REQUIRE(currentDimension(reopened.Graph));
	CHECK(currentDimension(reopened.Graph)->Data == Value{Vector2{101, 102}});
	const auto saved = Json::parse(std::string_view(checked.GraphJson.data(), checked.GraphJson.size() - 1));
	const auto &savedDimension = saved["nodes"][0]["inputs"][input->SourceIndex];
	CHECK(savedDimension["def_val"] == Json::array({7, 8}));
	CHECK(savedDimension["r"]["d"] == Json::array({101, 102}));

	for (const auto &mismatched : std::array<Json, 3>{Json::array({7}), Json::array({7, 8, 9}), Json(7)}) {
		auto ignored = importRecord(mismatched);
		REQUIRE(ignored.Graph.Nodes.size() == 1);
		CHECK(ignored.Graph.Nodes.front().Type == "pc.solid");
		CHECK_FALSE(ignored.Graph.Nodes.front().SourceVec2Defaults);
		REQUIRE(currentDimension(ignored.Graph));
		CHECK(currentDimension(ignored.Graph)->Data == Value{Vector2{99, 88}});
	}
	const auto unsupported = importRecord(Json::array({7, "unknown"}));
	REQUIRE(unsupported.Graph.Nodes.size() == 1);
	CHECK(unsupported.Graph.Nodes.front().Type != "pc.solid");
	CHECK_FALSE(unsupported.Diagnostics.empty());
	CHECK(unsupported.Diagnostics.front().Message.find("constructor default pair") != std::string::npos);

	const auto priorGraph = separated.Graph;
	const auto priorSource = separated.Source.OriginalBytes;
	Document coldDesired = separated.Graph;
	auto &coldAxes = coldDesired.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	coldAxes.Initialized = false;
	coldAxes.Axes[0].Keys.clear();
	coldAxes.Axes[1].Keys.clear();
	currentDimension(coldDesired)->Data = Vector2{101, 102};
	std::vector<std::byte> result{std::byte{0x77}};
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	CHECK(diagnostic.Message.find("cold scalar storage") != std::string::npos);
	currentDimension(coldDesired)->Data = Vector2{99, 88};
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	CHECK(separated.Source.OriginalBytes == priorSource);
	CHECK(separated.Graph == priorGraph);
	coldDesired.Nodes.front().Id.assign(Limits::MaximumTextBytes + 1, 'x');
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(diagnostic.NodeId.empty());
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	coldDesired.Nodes.front().Id = "node";
	coldDesired.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Port.assign(
		Limits::MaximumTextBytes + 1, 'x'
	);
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(diagnostic.Port.empty());
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
}
