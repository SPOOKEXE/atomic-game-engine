// Source Cross Section controls, keys and depth metadata remain editable across PXC saves.
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

TEST_SUITE_ID("engine.imagegraphio.source_cross_section")
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
	Json UvRecord(std::string id, int width, int height, int64_t depth) {
		Json inputs = std::vector<Json>(12, Json::object());
		inputs[0] = Fixed(Json::array({width, height}));
		inputs[0]["attri"] = {{"use_project_dimension", 0}};
		auto node = Record(std::move(id), "Node_UV_Cartesian", inputs);
		node["attri"] = {{"color_depth", depth}};
		return node;
	}
	Json NumberRecord(std::string id, double value) {
		return Record(std::move(id), "Node_Number_Simple", Json::array({Fixed(value)}));
	}
	Json BooleanRecord(std::string id, bool value) {
		Json inputs = std::vector<Json>(3, Json::object());
		inputs[0] = Fixed(value);
		return Record(std::move(id), "Node_Boolean", inputs);
	}
	Json Project(
		bool linked = false,
		bool grouped = false,
		bool animated = false,
		int64_t depth = 3,
		int64_t arrayMode = 0,
		bool process = true,
		bool masked = true
	) {
		Json section = Json::array(
			{Wire("uv"),
			 Fixed(0),
			 Fixed(.375),
			 Fixed(true),
			 Fixed(1),
			 masked ? Wire("mask") : Fixed(-4),
			 Fixed(true),
			 Fixed(Json::array({.1, .9}))}
		);
		section[5]["attri"] = {{"mask_alpha_only", true}, {"future_mask", "keep"}};
		if (animated)
			section[2] = {{"anim", true}, {"r", Json::array({Row(0, .125, "first"), Row(10, .875, "last")})}};
		if (linked) {
			for (const auto &[index, id] : std::array<std::pair<size_t, const char *>, 6>{
					 {{1, "axis"}, {2, "position"}, {3, "aa"}, {4, "mode"}, {6, "alpha"}, {7, "level"}}
				 })
				section[index] = Wire(id);
		}
		for (auto &input : section)
			input["future_input"] = "keep";
		auto node = Record("tile", "Node_Cross_Section", section);
		node["attri"] = {
			{"color_depth", depth},
			{"process", process},
			{"array_process", arrayMode},
			{"future_attribute", "keep"}
		};
		Json level = std::vector<Json>(12, Json::object());
		level[0] = Fixed(.1);
		level[1] = Fixed(.9);
		Json nodes = Json::array(
			{UvRecord("uv", 8, 4, 4),
			 std::move(node),
			 UvRecord("mask", 3, 2, 3),
			 NumberRecord("axis", 0),
			 NumberRecord("position", .375),
			 BooleanRecord("aa", true),
			 NumberRecord("mode", 1),
			 BooleanRecord("alpha", true),
			 Record("level", "Node_Vector2", level)}
		);
		if (grouped) {
			for (auto &child : nodes)
				child["group"] = "group";
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
		FAIL("missing mapped Cross Section input: " << port);
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
	"PXC Cross Section retains all physical controls and ignores stored output depth",
	"[source_cross_section]"
) {
	const auto depthReference = Sample(Import(Project()).Graph);
	for (int64_t depth = 0; depth <= 8; ++depth) {
		CAPTURE(depth);
		const auto project = Project(false, false, false, depth);
		auto imported = Import(project);
		const auto &node = Native(imported.Graph, "tile");
		REQUIRE(node.Type == "pc.cross_section");
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		CHECK(std::count_if(entry->Inputs.begin(), entry->Inputs.end(), [](const auto &input) {
				  return input.SourceIndex >= 0;
			  }) == 8);
		CHECK(Authored(node, "axis") == Value{EnumValue{0}});
		CHECK(Authored(node, "position") == Value{.375});
		CHECK(Authored(node, "anti_aliasing") == Value{true});
		CHECK(Authored(node, "mode") == Value{EnumValue{1}});
		CHECK(Authored(node, "to_alpha") == Value{true});
		CHECK(Authored(node, "level") == Value{Vector2{.1, .9}});
		CHECK(Authored(node, "attribute_color_depth") == Value{EnumValue{depth}});
		CHECK(Authored(node, "mask_alpha_only") == Value{true});
		REQUIRE(imported.Graph.Links.size() == 2);
		const auto before = Sample(imported.Graph);
		CHECK(before.Width == 8);
		CHECK(before.Height == 4);
		CHECK(before.Format == SurfaceFormat::RGBA8Unorm);
		CHECK(before == depthReference);
		const std::array<PxcxEdit, 6> edits{
			PxcxInputValueEdit{"tile", "axis", EnumValue{1}},
			PxcxInputValueEdit{"tile", "position", .625},
			PxcxInputValueEdit{"tile", "anti_aliasing", false},
			PxcxInputValueEdit{"tile", "mode", EnumValue{0}},
			PxcxInputValueEdit{"tile", "to_alpha", false},
			PxcxInputValueEdit{"tile", "level", Vector2{.2, .8}}
		};
		auto desired = Edit(imported, edits);
		NativeRoundtrip(desired);
		auto reopened = Save(imported, desired);
		CHECK(Sample(reopened.Graph) == Sample(desired));
		CHECK(Sample(reopened.Graph).Format == SurfaceFormat::RGBA8Unorm);
		CHECK(reopened.Graph.Links == imported.Graph.Links);
		const auto source = Source(reopened);
		CHECK(source["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
		CHECK(source["nodes"][1]["inputs"][5] == project["nodes"][1]["inputs"][5]);
		CHECK(source["nodes"][0] == project["nodes"][0]);
		CHECK(source["nodes"][2] == project["nodes"][2]);
		for (const auto &input : source["nodes"][1]["inputs"])
			CHECK(input["future_input"] == "keep");
		std::vector<std::byte> unchanged;
		Diagnostic error;
		REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
		CHECK(unchanged == reopened.Source.OriginalBytes);
	}
}

TEST_CASE(
	"PXC Cross Section links every physical control through grouped native saves", "[source_cross_section]"
) {
	auto grouped = Import(Project(true, true));
	auto ordinary = Import(Project(true));
	REQUIRE(Native(grouped.Graph, "tile").Type == "pc.cross_section");
	REQUIRE(grouped.Graph.Links.size() == 8);
	CHECK(Native(grouped.Graph, "tile").GroupId == "group");
	CHECK(Sample(grouped.Graph) == Sample(ordinary.Graph));
	const std::array<PxcxEdit, 7> edits{
		PxcxInputValueEdit{"axis", "value", 1.0},
		PxcxInputValueEdit{"position", "value", .625},
		PxcxInputValueEdit{"aa", "value", false},
		PxcxInputValueEdit{"mode", "value", 0.0},
		PxcxInputValueEdit{"alpha", "value", false},
		PxcxInputValueEdit{"level", "x", .25},
		PxcxInputValueEdit{"level", "y", .75}
	};
	auto desired = Edit(grouped, edits);
	NativeRoundtrip(desired);
	auto reopened = Save(grouped, desired);
	CHECK(Sample(reopened.Graph) == Sample(desired));
	CHECK(Sample(reopened.Graph) == Sample(Edit(ordinary, edits)));
	CHECK(reopened.Graph.Links == grouped.Graph.Links);
	CHECK(Native(reopened.Graph, "tile").GroupId == "group");
	CHECK(Source(reopened)["nodes"][1] == Source(grouped)["nodes"][1]);
	CHECK(Source(reopened)["nodes"][9] == Source(grouped)["nodes"][9]);
}

TEST_CASE("PXC Cross Section position animation retains keys and replay pixels", "[source_cross_section]") {
	const auto project = Project(false, true, true);
	auto imported = Import(project);
	auto desired = imported.Graph;
	unsigned changed = 0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "tile" && key.Port == "position" && key.Tick == 10) {
			key.Tick = 12;
			key.Data = .625;
			++changed;
		}
	REQUIRE(changed == 1);
	auto reopened = Save(imported, desired);
	for (int64_t tick : {0, 5, 12}) {
		CAPTURE(tick);
		CHECK(Sample(reopened.Graph, tick) == Sample(desired, tick));
		NativeRoundtrip(desired, tick);
	}
	const auto source = Source(reopened);
	const auto &keys = source["nodes"][1]["inputs"][2]["r"];
	REQUIRE(keys.size() == 2);
	CHECK(keys[1][0][1] == 12);
	CHECK(keys[1][1] == .625);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][2]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][2]["r"][1][9]);
}

TEST_CASE(
	"PXC Cross Section preserves all four processor array modes and disabled scalar processing",
	"[source_cross_section]"
) {
	for (int64_t arrayMode : {0, 1, 2, 3}) {
		CAPTURE(arrayMode);
		auto project = Project(false, true, false, 5, arrayMode);
		project["nodes"][1]["inputs"][2] = Fixed(Json::array({.125, .875}));
		project["nodes"][1]["inputs"][7] = Fixed(Json::array({Json::array({0, 1}), Json::array({.25, .75})}));
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "tile").Type == "pc.cross_section");
		auto sampleArray = [](Document document) {
			document.Outputs = {{"image", "tile", "surface_out"}};
			Plan plan;
			Diagnostic error;
			REQUIRE(Compile(document, plan, error) == Status::Ok);
			ImageArray images;
			const auto status = EvaluateArray(document, plan, "image", {}, images, error);
			INFO(error.Message);
			REQUIRE(status == Status::Ok);
			return images;
		};
		const auto expected = sampleArray(imported.Graph);
		CHECK(expected.Images.size() == (arrayMode < 2 ? 2 : 4));
		for (const auto &image : expected.Images)
			CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
		Document nativeReload;
		Diagnostic error;
		REQUIRE(Read(Write(imported.Graph), nativeReload, error) == Status::Ok);
		CHECK(sampleArray(nativeReload).Images == expected.Images);
		auto desired = imported.Graph;
		Native(desired, "tile").Position.X = 20;
		auto reopened = Save(imported, desired);
		const auto actual = sampleArray(reopened.Graph);
		CHECK(actual.Images == expected.Images);
		CHECK(actual.Items == expected.Items);
		CHECK(Source(reopened)["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
	}
	auto disabled = Import(Project(false, false, false, 4, 0, false));
	auto enabled = Import(Project(false, false, false, 4));
	CHECK(Sample(disabled.Graph) == Sample(enabled.Graph));
	NativeRoundtrip(disabled.Graph);
	CHECK(Sample(Save(disabled, disabled.Graph).Graph) == Sample(disabled.Graph));
}

TEST_CASE(
	"PXC Cross Section refuses unrepresented controls without replacing retained source",
	"[source_cross_section]"
) {
	auto project = Project();
	SECTION("Malformed level") {
		project["nodes"][1]["inputs"][7] = Fixed(Json::array({1, 2, 3}));
	}
	SECTION("Unrepresented position") {
		project["nodes"][1]["inputs"][2] = Fixed("unrepresented");
	}
	SECTION("Unknown physical input") {
		project["nodes"][1]["inputs"].push_back(Fixed(1));
	}
	SECTION("Unknown output index") {
		auto input = Wire("tile", 1);
		project["nodes"].push_back(Record(
			"consumer",
			"Node_Cross_Section",
			Json::array(
				{input,
				 Fixed(0),
				 Fixed(.5),
				 Fixed(false),
				 Fixed(0),
				 Fixed(-4),
				 Fixed(false),
				 Fixed(Json::array({0, 1}))}
			)
		));
	}
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "tile").Type == "pxcx.opaque/Node_Cross_Section");
	CHECK_FALSE(imported.Diagnostics.empty());
	std::vector<std::byte> bytes;
	Diagnostic error;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, error));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto desired = imported.Graph;
	Native(desired, "tile").Type = "pc.cross_section";
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	bytes = sentinel;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(bytes == sentinel);
}

TEST_CASE(
	"PXC Cross Section physical edit refusals leave source and output bytes intact", "[source_cross_section]"
) {
	const auto imported = Import(Project());
	const auto previousGraph = imported.Graph;
	const auto previousBytes = imported.Source.OriginalBytes;
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	Diagnostic error;
	for (const auto &[port, value] : std::array<AuthoredValue, 3>{
			 {{"mask_alpha_only", false},
			  {"attribute_color_depth", EnumValue{4}},
			  {"attribute_array_process", EnumValue{1}}}
		 }) {
		CAPTURE(port);
		const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"tile", port, value}};
		auto bytes = sentinel;
		CHECK_FALSE(WritePxcxEdits(imported, previousBytes, edits, bytes, error));
		CHECK(bytes == sentinel);
	}
	const std::array<PxcxEdit, 1> validEdit{PxcxInputValueEdit{"tile", "position", .625}};
	auto staleOutput = sentinel;
	CHECK_FALSE(WritePxcxEdits(imported, sentinel, validEdit, staleOutput, error));
	CHECK(staleOutput == sentinel);
	auto desired = imported.Graph;
	for (auto &value : Native(desired, "tile").Values)
		if (value.Port == "position") value.Data = std::string("unrepresented");
	auto invalidOutput = sentinel;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, invalidOutput, error));
	CHECK(invalidOutput == sentinel);
	CHECK(imported.Graph == previousGraph);
	CHECK(imported.Source.OriginalBytes == previousBytes);
}
