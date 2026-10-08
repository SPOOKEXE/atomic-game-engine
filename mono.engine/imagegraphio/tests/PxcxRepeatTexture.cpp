// Source Repeat Texture sockets, keys and dimension modes remain editable across PXC saves.
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

TEST_SUITE_ID("engine.imagegraphio.pxcx_repeat_texture")
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
	Json Project(
		int64_t unit = 0,
		bool linked = false,
		bool grouped = false,
		bool animated = false,
		int64_t mode = 1,
		int64_t depth = 3
	) {
		Json uv = std::vector<Json>(12, Json::object());
		uv[0] = Fixed(Json::array({3, 2}));
		uv[0]["attri"] = {{"use_project_dimension", 0}};
		Json tile = Json::array(
			{Wire("uv"),
			 Fixed(Json::array({unit == 0 ? 12.0 : 1.0, unit == 0 ? 6.0 : 1.0})),
			 Fixed(mode),
			 Fixed(17),
			 Fixed(.5)}
		);
		tile[1]["attri"] = {{"use_project_dimension", unit}, {"future_attribute", "keep"}};
		if (animated)
			tile[4] = {{"anim", true}, {"r", Json::array({Row(0, 0, "first"), Row(10, 1, "last")})}};
		if (linked) {
			tile[1]["from_node"] = "size";
			tile[1]["from_index"] = 0;
			tile[1]["from_tag"] = 0;
			tile[4]["from_node"] = "amount";
			tile[4]["from_index"] = 0;
			tile[4]["from_tag"] = 0;
			tile[3]["from_node"] = "seed";
			tile[3]["from_index"] = 0;
			tile[3]["from_tag"] = 0;
		}
		for (auto &input : tile)
			input["future_input"] = "keep";
		Json size = std::vector<Json>(12, Json::object());
		size[0] = Fixed(9);
		size[1] = Fixed(5);
		auto repeat = Record("tile", "Node_Repeat_Texture", tile);
		repeat["attri"] = {
			{"color_depth", depth}, {"process", true}, {"array_process", 0}, {"future_attribute", "keep"}
		};
		Json nodes = Json::array(
			{Record("uv", "Node_UV_Cartesian", uv),
			 std::move(repeat),
			 Record("size", "Node_Vector2", size),
			 Record("amount", "Node_Number_Simple", Json::array({Fixed(.25)})),
			 Record("seed", "Node_Number_Simple", Json::array({Fixed(17)}))}
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
		FAIL("missing mapped Repeat Texture input: " << port);
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
	"PXC Repeat Texture retains all modes and formats through physical edits", "[pxcx_repeat_texture]"
) {
	for (int64_t mode : {0, 1, 2})
		for (int64_t depth : {3, 4, 6})
			for (int64_t unit : {0, 1}) {
				CAPTURE(mode, depth, unit);
				const auto project = Project(unit, false, false, false, mode, depth);
				auto imported = Import(project);
				const auto &node = Native(imported.Graph, "tile");
				REQUIRE(node.Type == "pc.repeat_texture");
				const auto *entry = FindCatalogueEntry("pc.repeat_texture");
				REQUIRE(entry);
				CHECK(std::count_if(entry->Inputs.begin(), entry->Inputs.end(), [](const auto &input) {
						  return input.SourceIndex >= 0;
					  }) == 5);
				CHECK(Authored(node, "target_dimension_unit") == Value{EnumValue{unit}});
				CHECK(Authored(node, "type") == Value{EnumValue{mode}});
				CHECK(Authored(node, "seed") == Value{17.0});
				CHECK(Authored(node, "randomness") == Value{.5});
				CHECK(Authored(node, "attribute_color_depth") == Value{EnumValue{depth}});
				CHECK(Authored(node, "attribute_array_process") == Value{EnumValue{0}});
				CHECK(imported.Graph.Links == std::vector<Link>{{"uv", "surface_out", "tile", "surface_in"}});
				const auto before = Sample(imported.Graph);
				CHECK(before.Width == 12);
				CHECK(before.Height == 6);
				CHECK(before.Format == *SourceSurfaceFormat(depth));
				const std::array<PxcxEdit, 4> edits{
					PxcxInputValueEdit{"tile", "randomness", .8},
					PxcxInputValueEdit{"tile", "seed", 23.0},
					PxcxInputValueEdit{"tile", "type", EnumValue{(mode + 1) % 3}},
					PxcxInputValueEdit{
						"tile",
						"target_dimension",
						unit == 0 ? Value{Vector2{18, 9}} : Value{Vector2{1.5, 1.5}}
					}
				};
				auto desired = Edit(imported, edits);
				const auto expected = Sample(desired);
				CHECK(expected.Width == 18);
				CHECK(expected.Height == 9);
				CHECK(expected.Format == before.Format);
				NativeRoundtrip(desired);
				auto reopened = Save(imported, desired);
				CHECK(Sample(reopened.Graph) == expected);
				CHECK(
					Authored(Native(reopened.Graph, "tile"), "target_dimension_unit") ==
					Value{EnumValue{unit}}
				);
				const auto source = Source(reopened);
				CHECK(source["nodes"][1]["inputs"][1]["attri"] == project["nodes"][1]["inputs"][1]["attri"]);
				CHECK(source["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
				CHECK(source["nodes"][0] == project["nodes"][0]);
				CHECK(source["future_project"] == "keep");
				REQUIRE(source["nodes"][1]["inputs"].size() == 5);
				for (const auto &input : source["nodes"][1]["inputs"])
					CHECK(input["future_input"] == "keep");
				std::vector<std::byte> unchanged;
				Diagnostic error;
				REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
				CHECK(unchanged == reopened.Source.OriginalBytes);
			}
}

TEST_CASE(
	"PXC Repeat Texture defaults and linked dimensions preserve grouped routes", "[pxcx_repeat_texture]"
) {
	auto defaults = Project();
	defaults["nodes"][1]["inputs"][1] = Json::object();
	defaults["nodes"][1]["inputs"][2] = Json::object();
	defaults["nodes"][1]["inputs"][4] = Json::object();
	auto explicitDefaults = Project(1);
	explicitDefaults["nodes"][1]["inputs"][4] = Fixed(1);
	CHECK(Sample(Import(defaults).Graph) == Sample(Import(explicitDefaults).Graph));
	for (int64_t unit : {0, 1, 2}) {
		auto grouped = Import(Project(unit, true, true));
		auto ordinary = Import(Project(unit, true));
		CHECK(Native(grouped.Graph, "tile").GroupId == "group");
		REQUIRE(grouped.Graph.Links.size() == 4);
		CHECK(Sample(grouped.Graph) == Sample(ordinary.Graph));
		CHECK(Sample(grouped.Graph).Width == 9);
		CHECK(Sample(grouped.Graph).Height == 5);
		const std::array<PxcxEdit, 4> edits{
			PxcxInputValueEdit{"size", "x", 15.0},
			PxcxInputValueEdit{"size", "y", 7.0},
			PxcxInputValueEdit{"amount", "value", .9},
			PxcxInputValueEdit{"seed", "value", 41.0}
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
		CHECK(Source(reopened)["nodes"][5] == Source(grouped)["nodes"][5]);
	}
}

TEST_CASE("PXC Repeat Texture animated randomness retains keys and replay pixels", "[pxcx_repeat_texture]") {
	const auto project = Project(0, false, true, true, 2, 4);
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
		CHECK(Sample(reopened.Graph, tick) == Sample(reopened.Graph, tick));
		NativeRoundtrip(desired, tick);
	}
	const auto source = Source(reopened);
	const auto &keys = source["nodes"][1]["inputs"][4]["r"];
	REQUIRE(keys.size() == 2);
	CHECK(keys[1][0][1] == 12);
	CHECK(keys[1][1] == .8);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][4]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][4]["r"][1][9]);
}

TEST_CASE(
	"PXC Repeat Texture processor arrays preserve exact seeds and native image order", "[pxcx_repeat_texture]"
) {
	auto project = Project(0, false, true, false, 1, 4);
	project["nodes"][1]["inputs"][3] = Fixed(Json::array({17, 41}));
	project["nodes"][1]["inputs"][4] = Fixed(Json::array({.25, .75}));
	auto imported = Import(project);
	REQUIRE(Native(imported.Graph, "tile").Type == "pc.repeat_texture");
	auto sampleArray = [](Document document) {
		document.Outputs = {{"image", "tile", "surface_out"}};
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		ImageArray array;
		const auto status = EvaluateArray(document, plan, "image", {}, array, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return array;
	};
	const auto expected = sampleArray(imported.Graph);
	REQUIRE(expected.Images.size() == 2);
	for (size_t index = 0; index < expected.Images.size(); ++index) {
		auto scalar = Project(0, false, true, false, 1, 4);
		scalar["nodes"][1]["inputs"][3] = Fixed(index == 0 ? 17 : 41);
		scalar["nodes"][1]["inputs"][4] = Fixed(index == 0 ? .25 : .75);
		CHECK(expected.Images[index] == Sample(Import(scalar).Graph));
	}
	Document nativeReload;
	Diagnostic error;
	REQUIRE(Read(Write(imported.Graph), nativeReload, error) == Status::Ok);
	const auto nativeArray = sampleArray(nativeReload);
	CHECK(nativeArray.Images == expected.Images);
	CHECK(nativeArray.Items == expected.Items);
	auto desired = imported.Graph;
	Native(desired, "tile").Position.X = 20;
	auto reopened = Save(imported, desired);
	const auto reopenedArray = sampleArray(reopened.Graph);
	CHECK(reopenedArray.Images == expected.Images);
	CHECK(reopenedArray.Items == expected.Items);
	CHECK(Source(reopened)["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
	CHECK(Source(reopened)["nodes"][1]["inputs"] == project["nodes"][1]["inputs"]);
}

TEST_CASE(
	"PXC Repeat Texture preserves synthetic attributes and refuses unsupported edits atomically",
	"[pxcx_repeat_texture]"
) {
	auto imported = Import(Project());
	const auto previousGraph = imported.Graph;
	const auto previousBytes = imported.Source.OriginalBytes;
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	for (const auto &[port, value] : std::array<AuthoredValue, 3>{
			 {{"target_dimension_unit", EnumValue{1}},
			  {"attribute_color_depth", EnumValue{4}},
			  {"attribute_array_process", EnumValue{1}}}
		 }) {
		CAPTURE(port);
		const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"tile", port, value}};
		auto bytes = sentinel;
		Diagnostic error;
		CHECK_FALSE(WritePxcxEdits(imported, previousBytes, edits, bytes, error));
		CHECK(bytes == sentinel);
	}
	auto mask = Import(Project(2));
	Native(mask.Graph, "tile").Position.X = 20;
	auto reopened = Save(Import(Project(2)), mask.Graph);
	CHECK(Authored(Native(reopened.Graph, "tile"), "target_dimension_unit") == Value{EnumValue{2}});
	auto unsupported = reopened.Graph;
	unsupported.Outputs = {{"image", "tile", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(unsupported, plan, error) == Status::Ok);
	Image unchanged;
	const auto previousImage = unchanged;
	CHECK(Evaluate(unsupported, plan, "image", unchanged, error) == Status::UnsupportedExecution);
	CHECK(error.Port == "target_dimension_unit");
	CHECK(unchanged == previousImage);
	for (Value invalid : {Value{std::string("wrong")}, Value{Vector2{1, 2}}}) {
		auto desired = imported.Graph;
		for (auto &value : Native(desired, "tile").Values)
			if (value.Port == "randomness") value.Data = invalid;
		auto bytes = sentinel;
		CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
		CHECK(bytes == sentinel);
	}
	auto staleOutput = sentinel;
	const std::array<PxcxEdit, 1> staleEdit{PxcxInputValueEdit{"tile", "seed", 29.0}};
	CHECK_FALSE(WritePxcxEdits(imported, sentinel, staleEdit, staleOutput, error));
	CHECK(staleOutput == sentinel);
	CHECK(imported.Graph == previousGraph);
	CHECK(imported.Source.OriginalBytes == previousBytes);
}

TEST_CASE(
	"PXC Repeat Texture malformed source controls remain opaque and lossless", "[pxcx_repeat_texture]"
) {
	auto project = Project();
	SECTION("Unrepresented numeric seed") {
		project["nodes"][1]["inputs"][3] = Fixed("unrepresented");
	}
	SECTION("Malformed target dimensions") {
		project["nodes"][1]["inputs"][1] = Fixed(Json::array({1, 2, 3}));
	}
	SECTION("Unknown physical input") {
		project["nodes"][1]["inputs"].push_back(Fixed(1));
	}
	SECTION("Unsupported output index") {
		project["nodes"].push_back(Record(
			"consumer",
			"Node_Repeat_Texture",
			Json::array({Wire("tile", 1), Fixed(Json::array({4, 2})), Fixed(0), Fixed(17), Fixed(.5)})
		));
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "tile").Type == "pxcx.opaque/Node_Repeat_Texture");
	CHECK_FALSE(imported.Diagnostics.empty());
	std::vector<std::byte> bytes;
	Diagnostic error;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, error));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto desired = imported.Graph;
	Native(desired, "tile").Type = "pc.repeat_texture";
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	bytes = sentinel;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(bytes == sentinel);
}
