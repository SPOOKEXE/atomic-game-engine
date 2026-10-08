#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_modified_image_project")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	constexpr std::string_view CACHE = R"({"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"})";
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}, {"future_input", "keep"}};
	}
	Json Wire(std::string id) {
		return {{"from_node", std::move(id)}, {"from_index", 0}, {"from_tag", 0}, {"future_input", "keep"}};
	}
	Json Record(const char *id, const char *type, Json inputs) {
		return {
			{"id", id}, {"type", type}, {"x", 0}, {"y", 0}, {"inputs", std::move(inputs)}, {"future_node", id}
		};
	}
	Json Project() {
		const auto hash = engine::bake::SpriteCacheDataHash(CACHE);
		REQUIRE(hash);
		auto image = Record("image", "Node_Image", Json::array({Fixed("missing.png")}));
		image["attri"] = {{"cache_use", true}, {"cache_data", CACHE}, {"future_cache_attribute", 19}};
		image["atomic_game_engine"] = {
			{"version", 1},
			{"future_engine", 23},
			{"sprite_cache",
			 {{"layout", "rgba8-top-down"},
			  {"data_hash", std::string(hash->data(), hash->size())},
			  {"future_layout", 29}}}
		};
		auto repeat = Record(
			"repeat",
			"Node_Repeat_Texture",
			Json::array({Wire("image"), Fixed(Json::array({2, 1})), Fixed(0), Fixed(17), Fixed(.5)})
		);
		repeat["inputs"][1]["attri"] = {{"use_project_dimension", 0}, {"future_dimension", 31}};
		repeat["attri"] = {{"color_depth", 2}, {"process", true}, {"array_process", 0}};
		auto section = Record(
			"section",
			"Node_Cross_Section",
			Json::array(
				{Wire("repeat"),
				 Fixed(0),
				 Fixed(.25),
				 Fixed(false),
				 Fixed(1),
				 Fixed(-4),
				 Fixed(false),
				 Fixed(Json::array({0, 1}))}
			)
		);
		section["attri"] = {{"color_depth", 5}, {"process", true}, {"array_process", 0}};
		auto markov = Record(
			"markov",
			"Node_Markov_Gradient",
			Json::array(
				{Wire("section"),
				 Fixed(17),
				 Fixed(true),
				 Fixed(Json::array({4294967295ULL, 4278190335ULL})),
				 Fixed(2),
				 Fixed(1),
				 Fixed(-4)}
			)
		);
		markov["inputs"][5]["attri"] = {{"mapped", false}, {"future_mapping", 37}};
		markov["attri"] = {{"process", true}, {"array_process", 0}};
		return {
			{"attributes", {{"surface_dimension", Json::array({8, 4})}}},
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", Json::array({41, "keep"})},
			{"nodes",
			 Json::array(
				 {image,
				  repeat,
				  section,
				  markov,
				  Record("seed", "Node_Number_Simple", Json::array({Fixed(29)}))}
			 )}
		};
	}
	PxcxImport Reimport(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		const auto accepted = ImportPxcxImageGraph(archive, imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Imported() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = Project().dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		return Reimport(bytes);
	}
	Node &Native(Document &document, std::string_view id) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
			return candidate.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	void Set(Document &document, std::string_view id, std::string_view port, Value value) {
		auto &node = Native(document, id);
		for (auto &input : node.Values)
			if (input.Port == port) {
				input.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::string(port), std::move(value)});
	}
	Document Modified(const PxcxImport &imported) {
		auto document = imported.Graph;
		Diagnostic diagnostic;
		REQUIRE(Migrate(document, diagnostic) == Status::Ok);
		Set(document, "repeat", "target_dimension", Vector2{3, 2});
		Set(document, "section", "axis", EnumValue{1});
		Set(document, "section", "position", .75);
		ArrayValue palette;
		palette.ElementType = ValueType::Colour;
		palette.Elements = {Colour{255, 255, 255, 255}, Colour{7, 113, 229, 61}};
		Set(document, "markov", "colors", palette);
		Set(document, "markov", "seed", 29.);
		document.Links.push_back({"seed", "number", "markov", "seed"});
		Native(document, "image").Position = {12, -4};
		return document;
	}
	Image Sample(Document document) {
		document.Outputs = {{"out", "markov", "surface_out"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		HostNodeCapture capture;
		REQUIRE(PrepareHostCapture(document, plan, "image", request, capture, diagnostic) == Status::Ok);
		const auto &sourceImage = Native(document, "image");
		const std::string *cacheText = nullptr, *layoutText = nullptr;
		for (const auto &property : sourceImage.SourceProperties) {
			if (property.Port == "cache_data") cacheText = std::get_if<std::string>(&property.Data);
			if (property.Port == "composer_sprite_cache_layout")
				layoutText = std::get_if<std::string>(&property.Data);
		}
		REQUIRE(cacheText);
		REQUIRE(layoutText);
		const auto layout = engine::bake::ParseSpriteCacheLayoutName(*layoutText);
		REQUIRE(layout);
		std::vector<engine::bake::SpriteCacheFrame> frames;
		std::string failure;
		REQUIRE(
			engine::bake::ReadSpriteCache(
				*cacheText,
				*layout,
				frames,
				failure,
				Limits::MaximumEvaluationBytes,
				engine::bake::SpriteCacheShape::Sprite
			)
		);
		REQUIRE(frames.size() == 1);
		CHECK(frames[0].Rgba == std::vector<uint8_t>{12, 34, 56, 78});
		const auto *entry = FindCatalogueEntry("pc.image");
		REQUIRE(entry);
		REQUIRE_FALSE(entry->Outputs.empty());
		CHECK(entry->Outputs[0].SourceIndex == 0);
		REQUIRE(entry->Outputs[0].Type == ValueType::Image);
		capture.Images = {
			{std::string(entry->Outputs[0].Id), Image{frames[0].Width, frames[0].Height, frames[0].Rgba}}
		};
		request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
		Image output;
		const auto status = Evaluate(document, plan, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output;
	}
	void Uniform(const Image &image, uint32_t width, uint32_t height, std::array<uint8_t, 4> pixel) {
		CHECK(image.Width == width);
		CHECK(image.Height == height);
		CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
		std::vector<uint8_t> expected;
		for (uint32_t i = 0; i < width * height; ++i)
			expected.insert(expected.end(), pixel.begin(), pixel.end());
		CHECK(image.Pixels == expected);
	}
}

TEST_CASE(
	"Compound image edits reopen with deterministic source-derived pixels and retained cache records",
	"[imagegraphio][pxcx_modified_image_project]"
) {
	const auto source = Imported();
	const auto original = source.Graph;
	const auto originalBytes = source.Source.OriginalBytes;
	for (const auto &[id, type] : std::array<std::pair<const char *, const char *>, 4>{
			 {{"image", "pc.image"},
			  {"repeat", "pc.repeat_texture"},
			  {"section", "pc.cross_section"},
			  {"markov", "pc.markov_gradient"}}
		 }) {
		auto graph = source.Graph;
		CHECK(Native(graph, id).Type == type);
	}
	// Uniform input survives Tile and Color; threshold 2 matches the first white RGB,
	// and chance 1 selects its next full RGBA without chaining to another palette entry.
	Uniform(Sample(source.Graph), 2, 1, {255, 0, 0, 255});
	const auto desired = Modified(source);
	Document native;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(desired), native, diagnostic) == Status::Ok);
	CHECK(native == desired);
	Uniform(Sample(native), 3, 2, {7, 113, 229, 61});
	std::vector<std::byte> bytes;
	const auto accepted = WritePxcxProjection(source, native, {}, bytes, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reimport(bytes);
	REQUIRE(Migrate(reopened.Graph, diagnostic) == Status::Ok);
	CHECK(reopened.Graph == native);
	Uniform(Sample(reopened.Graph), 3, 2, {7, 113, 229, 61});
	const auto saved = Json::parse(reopened.Source.GraphJson.c_str());
	const auto project = Project();
	CHECK(saved["future_project"] == project["future_project"]);
	for (size_t index = 0; index < project["nodes"].size(); ++index)
		CHECK(saved["nodes"][index]["future_node"] == project["nodes"][index]["future_node"]);
	CHECK(saved["nodes"][0]["attri"] == project["nodes"][0]["attri"]);
	CHECK(saved["nodes"][0]["atomic_game_engine"] == project["nodes"][0]["atomic_game_engine"]);
	CHECK(saved["nodes"][1]["inputs"][1]["attri"] == project["nodes"][1]["inputs"][1]["attri"]);
	CHECK(saved["nodes"][3]["inputs"][5]["attri"] == project["nodes"][3]["inputs"][5]["attri"]);
	CHECK(saved["nodes"][3]["inputs"][1]["from_node"] == "seed");
	std::vector<std::byte> again;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, again, diagnostic));
	CHECK(again == bytes);
	CHECK(source.Graph == original);
	CHECK(source.Source.OriginalBytes == originalBytes);
}

TEST_CASE(
	"An unsupported final image edit refuses the whole PXC transaction without publication",
	"[imagegraphio][pxcx_modified_image_project]"
) {
	const auto source = Imported();
	auto desired = Modified(source);
	Native(desired, "markov").Values.push_back({"unsupported_future_socket", 1.});
	const auto previous = desired;
	const auto original = source.Graph;
	const auto originalBytes = source.Source.OriginalBytes;
	const std::vector<std::byte> sentinel{std::byte{0x61}, std::byte{0x62}};
	auto bytes = sentinel;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(source, desired, {}, bytes, diagnostic));
	CHECK_FALSE(diagnostic.Message.empty());
	CHECK(bytes == sentinel);
	CHECK(desired == previous);
	CHECK(source.Graph == original);
	CHECK(source.Source.OriginalBytes == originalBytes);
	Uniform(Sample(source.Graph), 2, 1, {255, 0, 0, 255});
}
