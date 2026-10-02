#include "../src/SourceTilesetCodec.hpp"
#include "../src/nodes/SourceTileRule.hpp"
#include "../src/nodes/SourceTileTerrain.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sstream>
TEST_SUITE_ID("engine.imagegraph.source_tiles")
using namespace engine::imagegraph;
namespace {
	Image
	TileTestImage(uint32_t w, uint32_t h, SurfaceFormat format, const std::vector<detail::Rgba> &pixels) {
		Image image;
		image.Width = w;
		image.Height = h;
		image.Format = format;
		const auto layout = CheckedSurfaceLayout(w, h, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		image.Pixels.resize(size_t(layout->Bytes));
		REQUIRE(pixels.size() == size_t(w) * h);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				REQUIRE(detail::WritePixel(image, x, y, pixels[y * w + x]));
		return image;
	}
	TilesetValue TileTestResource() {
		TilesetValue result;
		auto &data = result.Data.emplace();
		data.TileSize = {2, 2};
		data.Texture = TileTestImage(
			4,
			2,
			SurfaceFormat::RGBA8Unorm,
			{{1, 0, 0, 1},
			 {0, 1, 0, 1},
			 {1, 1, 0, 1},
			 {0, 1, 1, 1},
			 {0, 0, 1, 1},
			 {1, 1, 1, 1},
			 {1, 0, 1, 1},
			 {0, 0, 0, 1}}
		);
		return result;
	}
	SourceArrayItem TileTestItem(const Value &value) {
		SourceArrayItem item;
		std::visit(
			[&](const auto &data) {
				using T = std::decay_t<decltype(data)>;
				if constexpr (std::is_same_v<T, ArrayValue>) {
					std::vector<SourceArrayItem> children = data.Items;
					if (children.empty())
						for (const auto &leaf : data.Elements)
							children.push_back({leaf});
					item.Data = std::move(children);
				} else
					item.Data = ElementValue{data};
			},
			value
		);
		return item;
	}
	ArrayValue TileTestRecord(std::initializer_list<std::pair<std::string, Value>> fields) {
		std::vector<SourceArrayItem> record{{ElementValue{std::string("tile_record")}}};
		for (const auto &[key, value] : fields)
			record.push_back({std::vector<SourceArrayItem>{{ElementValue{key}}, TileTestItem(value)}});
		ArrayValue rows;
		rows.ElementType = ValueType::Any;
		rows.Items.push_back({std::move(record)});
		return rows;
	}
	Document TileTestGraph(std::string operation = "pc.tile_render") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"texture",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{12, 34, 56, 255}}}},
			{"map",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}},
			{"tileset", "pc.tile_tileset", "", {}, {{"tile_size", Vector2{1, 1}}}},
			{"render", operation, "", {}, {}}
		};
		document.Nodes[2].SourceDisplayName = "fixture";
		if (operation == "pc.tile_rule" || operation == "pc.tile_convert")
			document.Nodes.back().Values.push_back({"seed", 0.0});
		document.Links = {
			{"texture", "image", "tileset", "texture"},
			{"tileset", "tileset", "render", operation == "pc.tile_convert" ? "input_1" : "input_0"},
			{"map", "image", "render", operation == "pc.tile_convert" ? "surface" : "tilemap"}
		};
		document.Outputs = {{"rendered", "render", "rendered"}};
		return document;
	}
}
TEST_CASE(
	"Source tileset to render graph survives persistence and typed capture", "[imagegraph][source_tiles]"
) {
	auto document = TileTestGraph();
	document.Nodes[2].SourceProperties = {
		{"animatedTiles",
		 TileTestRecord(
			 {{"name", std::string("step")},
			  {"index", ArrayValue{ValueType::Scalar, {0.0, 1.0}}},
			  {"size", 2.0}}
		 )},
		{"autoterrain",
		 TileTestRecord(
			 {{"name", std::string("ground")},
			  {"index", ArrayValue{ValueType::Scalar, {0.0, 1.0}}},
			  {"type", 4.0}}
		 )},
		{"ruleTiles", TileTestRecord({{"name", std::string("inactive")}, {"active", false}})}
	};
	Plan plan;
	Diagnostic diagnostic;
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "rendered", image, diagnostic) == Status::Ok);
	CHECK(image.Width == 2);
	CHECK(image.Height == 2);
	CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
	CHECK(
		image.Pixels ==
		std::vector<uint8_t>{12, 34, 56, 255, 12, 34, 56, 255, 12, 34, 56, 255, 12, 34, 56, 255}
	);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(restored, plan, "render", {}, snapshot, diagnostic) == Status::Ok);
	const auto found =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &input) {
			return input.Port == "input_0";
		});
	REQUIRE(found != snapshot.Values().end());
	const auto &tiles = std::get<TilesetValue>(found->Data);
	REQUIRE(tiles.Data);
	CHECK(tiles.Data->DisplayName == "fixture");
	REQUIRE(tiles.Data->Animations.size() == 1);
	CHECK(tiles.Data->Animations[0].Indices == std::vector<int32_t>{0, 1});
	CHECK(tiles.Data->Animations[0].Length == 2);
	REQUIRE(tiles.Data->Terrains.size() == 1);
	CHECK(tiles.Data->Terrains[0].Type == 4);
	CHECK(tiles.Data->Terrains[0].PreviewIndex == 0);
	REQUIRE(tiles.Data->Rules.size() == 1);
	CHECK_FALSE(tiles.Data->Rules[0].Active);
	restored.Junctions.push_back({"retained", "", ValueType::Tileset, tiles});
	restored.Links[1] = {"retained", "value", "render", "input_0"};
	const auto saved = Write(restored);
	Document reparsed;
	REQUIRE(Read(saved, reparsed, diagnostic) == Status::Ok);
	CHECK(reparsed == restored);
	REQUIRE(Compile(reparsed, plan, diagnostic) == Status::Ok);
	Image replay;
	const auto replayStatus = Evaluate(reparsed, plan, "rendered", replay, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(replayStatus == Status::Ok);
	CHECK(replay == image);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest tiny;
	Image untouched{1, 1, {1, 2, 3, 4}, 0};
	const Image sentinel = untouched;
	CHECK(Evaluate(document, plan, "rendered", tiny, untouched, diagnostic, 128) == Status::LimitExceeded);
	CHECK(untouched == sentinel);
}
TEST_CASE(
	"Source tile flags flip before rotation and retain signed animated indices", "[imagegraph][source_tiles]"
) {
	const auto tiles = TileTestResource();
	// Literal order of the source shader's four texels for flags 0..15.
	constexpr std::array<std::array<int, 4>, 16> order{
		{{0, 1, 2, 3},
		 {2, 0, 3, 1},
		 {3, 2, 1, 0},
		 {1, 3, 0, 2},
		 {1, 0, 3, 2},
		 {0, 2, 1, 3},
		 {2, 3, 0, 1},
		 {3, 1, 2, 0},
		 {2, 3, 0, 1},
		 {3, 1, 2, 0},
		 {1, 0, 3, 2},
		 {0, 2, 1, 3},
		 {3, 2, 1, 0},
		 {1, 3, 0, 2},
		 {0, 1, 2, 3},
		 {2, 0, 3, 1}}
	};
	const std::array<detail::Rgba, 4> colours{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}}};
	for (int flag = 0; flag < 16; flag++) {
		Image map = TileTestImage(
			2, 2, SurfaceFormat::RGBA16Float, std::vector<detail::Rgba>(4, {1, double(flag), 0, 1})
		);
		const auto run =
			imagegraph_test::RunNode("pc.tile_render", {{"tilemap", &map}}, {{"input_0", tiles}});
		INFO(run.Message);
		REQUIRE(run.Ok);
		for (uint32_t y = 0; y < 2; y++)
			for (uint32_t x = 0; x < 2; x++)
				CHECK(
					detail::ReadPixel(run.Output("rendered"), x, y) ==
					colours[size_t(order[size_t(flag)][y * 2 + x])]
				);
	}
	auto animated = tiles;
	animated.Data->Animations = {{"first", {0, 1}, 2}};
	Image map = TileTestImage(2, 2, SurfaceFormat::RGBA16Float, std::vector<detail::Rgba>(4, {-1, 0, 0, 1}));
	const auto initial =
		imagegraph_test::RunNode("pc.tile_render", {{"tilemap", &map}}, {{"input_0", animated}}, 0);
	const auto next =
		imagegraph_test::RunNode("pc.tile_render", {{"tilemap", &map}}, {{"input_0", animated}}, 1);
	REQUIRE(initial.Ok);
	REQUIRE(next.Ok);
	CHECK(detail::ReadPixel(initial.Output("rendered"), 0, 0) == detail::Rgba{1, 0, 0, 1});
	CHECK(detail::ReadPixel(next.Output("rendered"), 0, 0) == detail::Rgba{1, 1, 0, 1});
	CHECK(
		map.Pixels ==
		TileTestImage(2, 2, SurfaceFormat::RGBA16Float, std::vector<detail::Rgba>(4, {-1, 0, 0, 1})).Pixels
	);
	// Length indexes the source's concatenated frame uniform, including the next animation.
	animated.Data->Animations = {{"first", {0}, 2}, {"second", {1}, 1}};
	const auto cross =
		imagegraph_test::RunNode("pc.tile_render", {{"tilemap", &map}}, {{"input_0", animated}}, 1);
	REQUIRE(cross.Ok);
	CHECK(detail::ReadPixel(cross.Output("rendered"), 0, 0) == detail::Rgba{1, 1, 0, 1});
}
TEST_CASE(
	"Source tile rules refuse undefined terrain bands and retain R16 group rounding",
	"[imagegraph][source_tiles]"
) {
	auto tiles = TileTestResource();
	tiles.Data->Terrains = {{"zero", 0, {0}, 0}, {"one", 0, {1}, 0}};
	TileRuleData rule;
	rule.Replacements = {{0}};
	rule.Selection = {{1, true}};
	detail::PreparedTileRule prepared;
	bool undefined = false;
	CHECK_FALSE(detail::PrepareSourceTileRule(rule, *tiles.Data, prepared, undefined));
	CHECK(undefined);
	rule.Selection = {{10000, false}};
	CHECK_FALSE(detail::PrepareSourceTileRule(rule, *tiles.Data, prepared, undefined));
	CHECK(undefined);
	rule.Selection = {{0, true}, {1, true}};
	REQUIRE(detail::PrepareSourceTileRule(rule, *tiles.Data, prepared, undefined));
	CHECK_FALSE(undefined);
	CHECK(detail::SourceTileRuleGroup(prepared, 2) == 10001);
	Image groups =
		TileTestImage(2, 2, SurfaceFormat::R16Float, std::vector<detail::Rgba>(4, {10001, 0, 0, 1}));
	CHECK(detail::ReadPixel(groups, 0, 0)[0] == 10000);
	// Numeric selectors use GLSL int truncation and retain their original group value.
	rule.Selection = {{10000.5, false}, {0, true}};
	REQUIRE(detail::PrepareSourceTileRule(rule, *tiles.Data, prepared, undefined));
	CHECK_FALSE(undefined);
	CHECK(detail::SourceTileRuleGroup(prepared, 1) == 10000.5);
	rule.Selection = {{10001.5, false}, {0, true}};
	CHECK_FALSE(detail::PrepareSourceTileRule(rule, *tiles.Data, prepared, undefined));
	CHECK(undefined);
	// Actual graph route uses the same source rule attributes and rejects the sparse group atomically.
	auto document = TileTestGraph("pc.tile_rule");
	ArrayValue selection;
	selection.ElementType = ValueType::Vector2;
	selection.Elements = {Vector2{0, 1}};
	ArrayValue replacement = TileTestRecord({{"index", ArrayValue{ValueType::Scalar, {0.0}}}});
	document.Nodes.back().SourceProperties = {
		{"ruleTiles", TileTestRecord({{"selection_rules", selection}, {"replacements", replacement}})}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {7, 8, 9, 10}, 0};
	Image image = sentinel;
	CHECK(Evaluate(document, plan, "rendered", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(image == sentinel);
	CHECK(diagnostic.Message.find("never uploaded") != std::string::npos);
}
TEST_CASE(
	"Source conversion retains plain pass ordering and ordered terrain topology", "[imagegraph][source_tiles]"
) {
	auto document = TileTestGraph("pc.tile_convert");
	document.Nodes.back().SourceProperties = {
		{"colorList", ArrayValue{ValueType::Scalar, {255.0}}},
		{"colorMap", TileTestRecord({{"color_id", 255.0}, {"target", 0.0}})}
	};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "rendered", image, diagnostic) == Status::Ok);
	CHECK(std::all_of(image.Pixels.begin(), image.Pixels.end(), [](uint8_t byte) { return byte == 0; }));
	CHECK(detail::SourceTerrainIndex(0, {0, 0, 0, 0, 1, 1, 0, 1, 1}) == 0);
	CHECK(detail::SourceTerrainIndex(0, {1, 1, 1, 1, 1, 1, 1, 1, 1}) == 4);
	CHECK(detail::SourceTerrainIndex(1, {0, 1, 1, 1, 1, 1, 1, 1, 1}) == 6);
	CHECK(detail::SourceTerrainIndex(3, {1, 1, 1, 1, 1, 1, 1, 1, 1}) == 33);
	CHECK(detail::SourceTerrainIndex(4, {1, 1, 1, 1, 1, 1, 1, 1, 1}) == 12);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Tileset codec is atomic and admits its surface and nested buffers before allocation",
	"[imagegraph][source_tiles]"
) {
	auto tiles = TileTestResource();
	tiles.Data->DisplayName = "tiles \"named\"";
	tiles.Data->Animations = {{"step", {0, 1}, 2}};
	tiles.Data->Terrains = {{"terrain", 4, {0, 1}, 0}};
	TileRuleData rule;
	rule.Replacements = {{0}};
	tiles.Data->Rules = {rule};
	std::ostringstream encoded;
	detail::WriteTilesetValue(encoded, tiles);
	std::istringstream stream(encoded.str());
	TilesetValue decoded;
	uint64_t admitted = 0;
	auto admit = [&](uint64_t bytes) {
		admitted += bytes;
		return true;
	};
	REQUIRE(detail::ReadTilesetValue(stream, decoded, admit));
	CHECK(decoded == tiles);
	CHECK(admitted >= detail::TilesetStorageBytes<true>(decoded));
	TilesetValue sentinel = decoded;
	std::istringstream truncated(encoded.str().substr(0, encoded.str().size() / 2));
	CHECK_FALSE(detail::ReadTilesetValue(truncated, decoded, admit));
	CHECK(decoded == sentinel);
	std::istringstream refusal(encoded.str());
	auto reject = [](uint64_t) { return false; };
	CHECK_FALSE(detail::ReadTilesetValue(refusal, decoded, reject));
	CHECK(decoded == sentinel);
	std::istringstream malformed("1 2 2 0 \"\" 4 2 rgba8_unorm 0 18446744073709551615");
	CHECK_FALSE(detail::ReadTilesetValue(malformed, decoded, admit));
	CHECK(decoded == sentinel);
	auto invalid = tiles;
	invalid.Data->Animations[0].Indices.resize(257);
	CHECK_FALSE(detail::ValidTilesetPayload(invalid));
	CHECK(sizeof(TilesetValue) == 8);
	CHECK(sizeof(Value) <= 88);
}

TEST_CASE(
	"Owned tileset arrays retain order through persisted Any group boundaries and budget refusal",
	"[imagegraph][source_tiles]"
) {
	auto first = TileTestResource(), second = first;
	first.Data->DisplayName = "first";
	second.Data->DisplayName = "second";
	ArrayValue flat{ValueType::Tileset, {first, second}};
	ArrayValue nested;
	nested.ElementType = ValueType::Tileset;
	nested.Nested = {{first}, {second}};
	ArrayValue general;
	general.ElementType = ValueType::Any;
	general.Items = {
		{{ElementValue{first}}}, {std::vector<SourceArrayItem>{{ElementValue{second}}, {ElementValue{first}}}}
	};
	for (const auto &array : {flat, nested, general}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{11}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}},
			{"output", "pc.group_output", "group", {}, {}}
		};
		Group group{"group", "Tiles"};
		group.Ports = {
			{"input", "input/parent-value", PortDirection::Input, "input"},
			{"output", "output/parent-value", PortDirection::Output, "output"}
		};
		document.Groups.push_back(group);
		document.Junctions = {
			{"input/parent-value", "group", ValueType::Any, array},
			{"output/parent-value", "group", ValueType::Any, std::nullopt}
		};
		document.Links = {
			{"input/parent-value", "value", "input", "parent_value"},
			{"input", "value", "output", "value"},
			{"output", "value", "output/parent-value", "value"}
		};
		document.Outputs = {{"out", "output", "value"}};
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto compiled = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue value;
		const auto evaluated = EvaluateValue(restored, plan, "out", {}, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		CHECK(value.Data == Value{array});
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(restored, plan, "output", {}, snapshot, diagnostic) == Status::Ok);
		REQUIRE(snapshot.Values().size() == 1);
		const Value captured = snapshot.Values()[0].Data;
		CHECK(
			EvaluateNodeInputs(restored, plan, "output", {}, snapshot, diagnostic, 128) ==
			Status::LimitExceeded
		);
		CHECK(snapshot.Values()[0].Data == captured);
	}
}
