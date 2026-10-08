// Point SDF physical controls and mapped ranges stay attached to their source records.
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_point_sdf")
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
	Json VectorRecord(std::string id, Vector2 value) {
		Json inputs = std::vector<Json>(12, Json::object());
		inputs[0] = Fixed(value.X);
		inputs[1] = Fixed(value.Y);
		return Record(std::move(id), "Node_Vector2", inputs);
	}
	Json Project(int64_t depth = 5, bool mapped = false, bool linked = false, bool grouped = false) {
		Json inputs = Json::array(
			{Fixed(Json::array({3, 3})),
			 Fixed(Json::array({Json::array({1.5, 1.5})})),
			 Fixed(mapped ? Json(Json::array({2, 4})) : Json(2)),
			 Fixed(false),
			 Wire("map")}
		);
		inputs[0]["attri"] = {{"use_project_dimension", 0}, {"future_dimension", "keep"}};
		inputs[2]["attri"] = {{"mapped", mapped}, {"future_map", "keep"}};
		if (linked) {
			inputs[0]["from_node"] = "size";
			inputs[0]["from_index"] = 0;
			inputs[0]["from_tag"] = 0;
			inputs[2]["from_node"] = "distance";
			inputs[2]["from_index"] = 0;
			inputs[2]["from_tag"] = 0;
			inputs[3] = Wire("invert");
		}
		for (auto &input : inputs)
			input["future_input"] = "keep";
		auto sdf = Record("sdf", "Node_Point_SDF", inputs);
		sdf["attri"] = {
			{"color_depth", depth}, {"process", true}, {"array_process", 0}, {"future_attribute", "keep"}
		};
		Json uvInputs = std::vector<Json>(12, Json::object());
		uvInputs[0] = Fixed(Json::array({2, 2}));
		uvInputs[0]["attri"] = {{"use_project_dimension", 0}};
		auto map = Record("map", "Node_UV_Cartesian", uvInputs);
		map["attri"] = {{"color_depth", 5}};
		Json flagInputs = std::vector<Json>(3, Json::object());
		flagInputs[0] = Fixed(false);
		Json nodes = Json::array(
			{std::move(sdf),
			 std::move(map),
			 VectorRecord("size", {3, 3}),
			 Record("distance", "Node_Number_Simple", Json::array({Fixed(2)})),
			 Record("invert", "Node_Boolean", flagInputs)}
		);
		if (grouped) {
			for (auto &node : nodes)
				node["group"] = "group";
			auto group = Record("group", "Node_Group", Json::array());
			group["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}};
			nodes.push_back(std::move(group));
		}
		return {
			{"attributes", {{"surface_dimension", Json::array({3, 3})}}},
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
		FAIL("missing mapped Point SDF input: " << port);
		return node.Values.front().Data;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	Image Sample(Document document, std::string_view node = "sdf") {
		document.Outputs = {{"image", std::string(node), "surface_out"}};
		Plan plan;
		Diagnostic error;
		auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
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
	void NativeRoundtrip(const Document &document) {
		Document reopened;
		Diagnostic error;
		REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
		CHECK(reopened == document);
		CHECK(Sample(reopened) == Sample(document));
	}
	constexpr std::array<SurfaceFormat, 7> FORMATS{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	ArrayValue Points(Vector2 point) {
		ArrayValue points{ValueType::Vector2, {}};
		points.Elements.emplace_back(point);
		return points;
	}
	Document
	Reference(uint32_t width, uint32_t height, Vector2 point, double distance, bool inverted, int64_t depth) {
		Document document;
		document.FormatVersion = 6;
		document.Nodes.push_back(
			{"sdf",
			 "pc.point_sdf",
			 "",
			 {},
			 {{"dimension", Vector2{double(width), double(height)}},
			  {"dimension_unit", EnumValue{0}},
			  {"points", Points(point)},
			  {"max_distance", distance},
			  {"inverted", inverted},
			  {"attribute_color_depth", EnumValue{depth}}}}
		);
		Document reopened;
		Diagnostic error;
		const auto status = Read(Write(document), reopened, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		CHECK(reopened == document);
		return reopened;
	}
	Image Oracle(
		uint32_t width,
		uint32_t height,
		Vector2 point,
		double distance,
		bool inverted,
		SurfaceFormat format,
		const Image *map = nullptr,
		Vector2 range = {2, 4}
	) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		Image expected{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const float u = (float(x) + .5f) / float(width), v = (float(y) + .5f) / float(height);
				float maximum = float(distance);
				if (map) {
					const auto mx =
						uint32_t(std::clamp(std::floor(double(u) * map->Width), 0., double(map->Width - 1)));
					const auto my = uint32_t(
						std::clamp(std::floor(double(v) * map->Height), 0., double(map->Height - 1))
					);
					SurfacePixel value;
					REQUIRE(LoadSurfacePixel(*map, mx, my, value));
					const float mean = (float(value[0]) + float(value[1]) + float(value[2])) / 3.f;
					maximum = float(range.X) * (1.f - mean) + float(range.Y) * mean;
				}
				const float dx = u * float(width) - float(point.X), dy = v * float(height) - float(point.Y);
				float shade = std::min(9999.f, std::sqrt(dx * dx + dy * dy)) / maximum;
				if (inverted) shade = 1.f - shade;
				REQUIRE(StoreSurfacePixel(expected, x, y, {shade, shade, shade, 1}));
			}
		return expected;
	}
	void SamePixels(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
	}
}

TEST_CASE("PXC Point SDF physical controls edit and reopen in every surface format", "[pxcx_point_sdf]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		CAPTURE(depth);
		const auto project = Project(depth);
		auto imported = Import(project);
		const auto &node = Native(imported.Graph, "sdf");
		REQUIRE(node.Type == "pc.point_sdf");
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		for (const auto &[port, index] : std::array<std::pair<const char *, int64_t>, 5>{
				 {{"dimension", 0},
				  {"points", 1},
				  {"max_distance", 2},
				  {"inverted", 3},
				  {"max_distance_map", 4}}
			 }) {
			const auto *input = FindCatalogueInput(*entry, port);
			REQUIRE(input);
			CHECK(input->SourceIndex == index);
		}
		CHECK(Authored(node, "dimension") == Value{Vector2{3, 3}});
		CHECK(Authored(node, "dimension_unit") == Value{EnumValue{0}});
		CHECK(Authored(node, "max_distance") == Value{2.});
		CHECK(Authored(node, "inverted") == Value{false});
		CHECK(Authored(node, "attribute_color_depth") == Value{EnumValue{depth}});
		const auto actual = Sample(imported.Graph);
		SamePixels(actual, Oracle(3, 3, {1.5, 1.5}, 2, false, FORMATS[size_t(depth - 2)]));
		CHECK(actual == Sample(Reference(3, 3, {1.5, 1.5}, 2, false, depth)));
		const std::array<PxcxEdit, 4> edits{
			PxcxInputValueEdit{"sdf", "dimension", Vector2{5, 3}},
			PxcxInputValueEdit{"sdf", "points", Points({2.5, 1.5})},
			PxcxInputValueEdit{"sdf", "max_distance", 4.},
			PxcxInputValueEdit{"sdf", "inverted", true}
		};
		const auto desired = Edit(imported, edits);
		NativeRoundtrip(desired);
		auto reopened = Save(imported, desired);
		SamePixels(Sample(reopened.Graph), Oracle(5, 3, {2.5, 1.5}, 4, true, FORMATS[size_t(depth - 2)]));
		CHECK(Sample(reopened.Graph) == Sample(Reference(5, 3, {2.5, 1.5}, 4, true, depth)));
		const auto source = Source(reopened);
		CHECK(source["nodes"][0]["inputs"][0]["r"]["d"] == Json::array({5, 3}));
		CHECK(source["nodes"][0]["inputs"][1]["r"]["d"] == Json::array({Json::array({2.5, 1.5})}));
		CHECK(source["nodes"][0]["inputs"][2]["r"]["d"] == 4);
		CHECK(source["nodes"][0]["inputs"][3]["r"]["d"] == true);
		CHECK(source["nodes"][0]["inputs"][4] == project["nodes"][0]["inputs"][4]);
		CHECK(source["nodes"][0]["attri"] == project["nodes"][0]["attri"]);
		for (const auto &input : source["nodes"][0]["inputs"])
			CHECK(input["future_input"] == "keep");
		CHECK(source["nodes"][1] == project["nodes"][1]);
		std::vector<std::byte> unchanged;
		Diagnostic error;
		REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, unchanged, error));
		CHECK(unchanged == reopened.Source.OriginalBytes);
	}
}

TEST_CASE("PXC Point SDF mapped range edits retain slot metadata and rendered pixels", "[pxcx_point_sdf]") {
	const auto project = Project(5, true);
	auto imported = Import(project);
	const auto &node = Native(imported.Graph, "sdf");
	REQUIRE(node.Type == "pc.point_sdf");
	CHECK(Authored(node, "max_distance_mapped") == Value{true});
	CHECK(Authored(node, "max_distance_map_range") == Value{Vector2{2, 4}});
	const auto map = Sample(imported.Graph, "map");
	SamePixels(Sample(imported.Graph), Oracle(3, 3, {1.5, 1.5}, 2, false, SurfaceFormat::RGBA32Float, &map));
	auto desired = imported.Graph;
	for (auto &value : Native(desired, "sdf").Values)
		if (value.Port == "max_distance_map_range") value.Data = Vector2{4, 8};
	auto reopened = Save(imported, desired);
	CHECK(Authored(Native(reopened.Graph, "sdf"), "max_distance_map_range") == Value{Vector2{4, 8}});
	SamePixels(
		Sample(reopened.Graph), Oracle(3, 3, {1.5, 1.5}, 4, false, SurfaceFormat::RGBA32Float, &map, {4, 8})
	);
	CHECK(Source(reopened)["nodes"][0]["inputs"][2]["r"]["d"] == Json::array({4, 8}));
	CHECK(Source(reopened)["nodes"][0]["inputs"][2]["attri"] == project["nodes"][0]["inputs"][2]["attri"]);
	CHECK(Source(reopened)["nodes"][0]["inputs"][4] == project["nodes"][0]["inputs"][4]);
	NativeRoundtrip(reopened.Graph);
}

TEST_CASE("PXC Point SDF grouped physical links preserve native analytical results", "[pxcx_point_sdf]") {
	auto grouped = Import(Project(5, false, true, true));
	auto ordinary = Import(Project(5, false, true));
	REQUIRE(Native(grouped.Graph, "sdf").Type == "pc.point_sdf");
	CHECK(Native(grouped.Graph, "sdf").GroupId == "group");
	CHECK(grouped.Graph.Links.size() == 4);
	CHECK(Sample(grouped.Graph) == Sample(ordinary.Graph));
	const std::array<PxcxEdit, 4> edits{
		PxcxInputValueEdit{"size", "x", 5.},
		PxcxInputValueEdit{"size", "y", 3.},
		PxcxInputValueEdit{"distance", "value", 4.},
		PxcxInputValueEdit{"invert", "value", true}
	};
	const auto desired = Edit(grouped, edits);
	auto reopened = Save(grouped, desired);
	SamePixels(Sample(reopened.Graph), Oracle(5, 3, {1.5, 1.5}, 4, true, SurfaceFormat::RGBA32Float));
	CHECK(Sample(reopened.Graph) == Sample(Edit(ordinary, edits)));
	CHECK(reopened.Graph.Links == grouped.Graph.Links);
	CHECK(Native(reopened.Graph, "sdf").GroupId == "group");
	CHECK(Source(reopened)["nodes"][0] == Source(grouped)["nodes"][0]);
	CHECK(Source(reopened)["nodes"][5] == Source(grouped)["nodes"][5]);
	NativeRoundtrip(reopened.Graph);
}

TEST_CASE("PXC Point SDF format projection edits survive physical source save", "[pxcx_point_sdf]") {
	auto imported = Import(Project());
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto desired = imported.Graph;
		for (auto &value : Native(desired, "sdf").Values)
			if (value.Port == "attribute_color_depth") value.Data = EnumValue{depth};
		auto reopened = Save(imported, desired);
		CHECK(Source(reopened)["nodes"][0]["attri"]["color_depth"] == depth);
		SamePixels(Sample(reopened.Graph), Oracle(3, 3, {1.5, 1.5}, 2, false, FORMATS[size_t(depth - 2)]));
	}
}
