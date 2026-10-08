// Source Tile Random sockets, keys and dimension modes remain editable across PXC saves.
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_tile_random")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}};
	}
	Json Wire(std::string id, int index = 0) {
		return {{"from_node", std::move(id)}, {"from_index", index}, {"from_tag", 0}};
	}
	Json Record(std::string id, const char *type, Json inputs) {
		return {
			{"id", std::move(id)},
			{"type", type},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Row(double time, double value, const char *identity) {
		return Json::array(
			{Json::array({0, time, "marker"}),
			 value,
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 16777215,
			 Json{{"future_key", identity}}}
		);
	}
	Json Project(int64_t unit = 0, bool linked = false, bool grouped = false, bool animated = false) {
		// Pinned Node_UV_Cartesian creates an asymmetric source surface without asset hosts.
		Json uv = std::vector<Json>(12, Json::object());
		uv[0] = Fixed(Json::array({3, 2}));
		uv[0]["attri"] = {{"use_project_dimension", 0}};
		Json tile = Json::array(
			{Wire("uv"), Fixed(Json::array({unit == 0 ? 12.0 : 1.0, unit == 0 ? 6.0 : 1.0})), Fixed(.5)}
		);
		tile[1]["attri"] = {{"use_project_dimension", unit}, {"future_attribute", "keep"}};
		if (animated)
			tile[2] = {{"anim", true}, {"r", Json::array({Row(0, 0, "first"), Row(10, 1, "last")})}};
		if (linked) {
			tile[1]["from_node"] = "size";
			tile[1]["from_index"] = 0;
			tile[1]["from_tag"] = 0;
			tile[2]["from_node"] = "amount";
			tile[2]["from_index"] = 0;
			tile[2]["from_tag"] = 0;
		}
		for (auto &input : tile)
			input["future_input"] = "keep";
		Json size = std::vector<Json>(12, Json::object());
		size[0] = Fixed(9);
		size[1] = Fixed(5);
		Json nodes = Json::array(
			{Record("uv", "Node_UV_Cartesian", uv),
			 Record("tile", "Node_Tile_Random", tile),
			 Record("size", "Node_Vector2", size),
			 Record("amount", "Node_Number_Simple", Json::array({Fixed(.25)}))}
		);
		if (grouped) {
			for (auto &node : nodes)
				node["group"] = "group";
			auto group = Record("group", "Node_Group", Json::array());
			group["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}};
			nodes.push_back(std::move(group));
		}
		return {
			{"attributes", {{"surface_dimension", Json::array({12, 6})}}},
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", "keep"},
			{"nodes", std::move(nodes)}
		};
	}
	engine::bake::PxcxArchive Archive(const Json &project) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		return archive;
	}
	PxcxImport Import(const Json &project) {
		PxcxImport imported;
		std::string failure;
		const bool accepted = ImportPxcxImageGraph(Archive(project), imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Reopen(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		return imported;
	}
	Node &Native(Document &document, std::string_view id) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
			return value.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const Value &Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return value.Data;
		FAIL("missing mapped Tile Random input: " << port);
		return node.Values.front().Data;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	Image Sample(Document document, int64_t tick = 0) {
		document.Outputs = {{"image", "tile", "surface_out"}};
		Plan plan;
		Diagnostic error;
		auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		Image image;
		status = Evaluate(document, plan, "image", request, image, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	Document Edit(const PxcxImport &imported, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		return Reopen(bytes).Graph;
	}
	PxcxImport Save(const PxcxImport &imported, const Document &desired) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		return Reopen(bytes);
	}
	void NativeRoundtrip(const Document &document, int64_t tick = 0) {
		Document reopened;
		Diagnostic error;
		REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
		CHECK(reopened == document);
		CHECK(Sample(reopened, tick) == Sample(document, tick));
	}
}

TEST_CASE(
	"PXC Tile Random retains Pixel and Project dimensions and complete authored controls",
	"[pxcx_tile_random]"
) {
	for (int64_t unit : {0, 1}) {
		CAPTURE(unit);
		const auto project = Project(unit);
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "tile").Type == "pc.tile_random");
		CHECK(Authored(Native(imported.Graph, "tile"), "dimension_unit") == Value{EnumValue{unit}});
		CHECK(Authored(Native(imported.Graph, "tile"), "randomness") == Value{.5});
		CHECK(imported.Graph.Links == std::vector<Link>{{"uv", "surface_out", "tile", "surface_in"}});
		const auto before = Sample(imported.Graph);
		CHECK(before.Width == 12);
		CHECK(before.Height == 6);
		const std::array<PxcxEdit, 2> edits{
			PxcxInputValueEdit{"tile", "randomness", .8},
			PxcxInputValueEdit{
				"tile", "dimension", unit == 0 ? Value{Vector2{18, 9}} : Value{Vector2{1.5, 1.5}}
			}
		};
		auto desired = Edit(imported, edits);
		const auto expected = Sample(desired);
		CHECK(expected.Width == 18);
		CHECK(expected.Height == 9);
		CHECK(expected != before);
		NativeRoundtrip(desired);
		auto reopened = Save(imported, desired);
		CHECK(Sample(reopened.Graph) == expected);
		CHECK(Authored(Native(reopened.Graph, "tile"), "dimension_unit") == Value{EnumValue{unit}});
		const auto source = Source(reopened);
		CHECK(source["nodes"][1]["inputs"][1]["attri"] == project["nodes"][1]["inputs"][1]["attri"]);
		CHECK(source["nodes"][0] == project["nodes"][0]);
		CHECK(source["future_project"] == "keep");
		for (const auto &input : source["nodes"][1]["inputs"])
			CHECK(input["future_input"] == "keep");
		std::vector<std::byte> unchanged;
		Diagnostic error;
		REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
		CHECK(unchanged == reopened.Source.OriginalBytes);
	}
}

TEST_CASE("PXC Tile Random default and linked dimension modes retain source routes", "[pxcx_tile_random]") {
	auto defaults = Project();
	defaults["nodes"][1]["inputs"][1] = Json::object();
	defaults["nodes"][1]["inputs"][2] = Json::object();
	auto importedDefault = Import(defaults);
	CHECK(Native(importedDefault.Graph, "tile").Type == "pc.tile_random");
	CHECK(Sample(importedDefault.Graph) == Sample(Import(Project(1)).Graph));
	for (int64_t unit : {0, 1, 2}) {
		auto grouped = Import(Project(unit, true, true));
		auto ordinary = Import(Project(unit, true));
		CHECK(Native(grouped.Graph, "tile").GroupId == "group");
		REQUIRE(grouped.Graph.Links.size() == 3);
		CHECK(Sample(grouped.Graph) == Sample(ordinary.Graph));
		CHECK(Sample(grouped.Graph).Width == 9);
		CHECK(Sample(grouped.Graph).Height == 5);
		const std::array<PxcxEdit, 3> edits{
			PxcxInputValueEdit{"size", "x", 15.0},
			PxcxInputValueEdit{"size", "y", 7.0},
			PxcxInputValueEdit{"amount", "value", .9}
		};
		auto desired = Edit(grouped, edits);
		const auto expected = Sample(desired);
		CHECK(expected.Width == 15);
		CHECK(expected.Height == 7);
		NativeRoundtrip(desired);
		auto reopened = Save(grouped, desired);
		CHECK(Sample(reopened.Graph) == expected);
		CHECK(reopened.Graph.Links == grouped.Graph.Links);
		CHECK(Native(reopened.Graph, "tile").GroupId == "group");
		CHECK(Source(reopened)["nodes"][1] == Source(grouped)["nodes"][1]);
		CHECK(Source(reopened)["nodes"][4] == Source(grouped)["nodes"][4]);
	}
}

TEST_CASE("PXC Tile Random animated randomness retains edited keys and pixel results", "[pxcx_tile_random]") {
	const auto project = Project(0, false, true, true);
	auto imported = Import(project);
	auto desired = imported.Graph;
	unsigned changed = 0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "tile" && key.Port == "randomness" && key.Tick == 10) {
			key.Tick = 12;
			key.Data = .8;
			++changed;
		}
	REQUIRE(changed == 1);
	auto reopened = Save(imported, desired);
	for (int64_t tick : {0, 5, 12}) {
		CAPTURE(tick);
		CHECK(Sample(reopened.Graph, tick) == Sample(desired, tick));
		NativeRoundtrip(desired, tick);
	}
	CHECK(Sample(reopened.Graph, 0) != Sample(reopened.Graph, 12));
	const auto source = Source(reopened);
	const auto &keys = source["nodes"][1]["inputs"][2]["r"];
	REQUIRE(keys.size() == 2);
	CHECK(keys[1][0][1] == 12);
	CHECK(keys[1][1] == .8);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][2]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][2]["r"][1][9]);
}

TEST_CASE(
	"PXC Tile Random retains unsupported Mask mode and refuses malformed projection atomically",
	"[pxcx_tile_random]"
) {
	auto mask = Import(Project(2));
	REQUIRE(Native(mask.Graph, "tile").Type == "pc.tile_random");
	Native(mask.Graph, "tile").Position.X = 20;
	auto reopened = Save(Import(Project(2)), mask.Graph);
	CHECK(Authored(Native(reopened.Graph, "tile"), "dimension_unit") == Value{EnumValue{2}});
	CHECK(Source(reopened)["nodes"][1]["inputs"][1]["attri"]["use_project_dimension"] == 2);
	auto unsupported = reopened.Graph;
	unsupported.Outputs = {{"image", "tile", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(unsupported, plan, error) == Status::Ok);
	Image sentinel;
	sentinel.Width = 1;
	sentinel.Height = 1;
	const auto previousImage = sentinel;
	CHECK(Evaluate(unsupported, plan, "image", sentinel, error) == Status::UnsupportedExecution);
	CHECK(sentinel == previousImage);
	const auto imported = Import(Project());
	const auto previousGraph = imported.Graph;
	const auto previousBytes = imported.Source.OriginalBytes;
	const std::vector<std::byte> outputSentinel{std::byte{0x61}};
	for (Value invalid : {Value{std::string("wrong")}, Value{Vector2{1, 2}}}) {
		auto desired = imported.Graph;
		for (auto &value : Native(desired, "tile").Values)
			if (value.Port == "randomness") value.Data = invalid;
		auto bytes = outputSentinel;
		CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
		CHECK(bytes == outputSentinel);
	}
	auto bytes = outputSentinel;
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"tile", "randomness", .9}};
	CHECK_FALSE(WritePxcxEdits(imported, outputSentinel, edits, bytes, error));
	CHECK(bytes == outputSentinel);
	CHECK(imported.Graph == previousGraph);
	CHECK(imported.Source.OriginalBytes == previousBytes);
}

TEST_CASE("PXC Tile Random malformed source layouts remain opaque and lossless", "[pxcx_tile_random]") {
	auto project = Project();
	SECTION("Unrepresented randomness") {
		project["nodes"][1]["inputs"][2] = Fixed("unrepresented");
	}
	SECTION("Malformed dimension") {
		project["nodes"][1]["inputs"][1] = Fixed(Json::array({1, 2, 3}));
	}
	SECTION("Unknown physical input") {
		project["nodes"][1]["inputs"].push_back(Fixed(1));
	}
	SECTION("Unknown output index") {
		auto input = Wire("tile", 1);
		project["nodes"].push_back(Record(
			"consumer",
			"Node_Tile_Random",
			Json::array({std::move(input), Fixed(Json::array({4, 2})), Fixed(.5)})
		));
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "tile").Type == "pxcx.opaque/Node_Tile_Random");
	CHECK_FALSE(imported.Diagnostics.empty());
	std::vector<std::byte> bytes;
	Diagnostic error;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, error));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto desired = imported.Graph;
	Native(desired, "tile").Type = "pc.tile_random";
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	bytes = sentinel;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(bytes == sentinel);
}
