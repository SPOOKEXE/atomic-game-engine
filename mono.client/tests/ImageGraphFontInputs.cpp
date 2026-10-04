#include "../src/ImageGraphTransform3DAdapter.hpp"

#include <engine/assets/ContentPolicy.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <fstream>

TEST_SUITE_ID("client.imagegraph.font_inputs")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraphfont.graphfont_host")

namespace {
	using namespace engine::imagegraph;
	constexpr std::string_view BITMAP_FONT = R"(STARTFONT 2.1
FONT -engine-test-medium-r-normal--10-100-72-72-c-80-iso10646-1
SIZE 10 72 72
FONTBOUNDINGBOX 5 7 -1 -2
STARTPROPERTIES 4
FONT_ASCENT 8
FONT_DESCENT 2
CHARSET_REGISTRY "ISO10646"
CHARSET_ENCODING "1"
ENDPROPERTIES
CHARS 3
STARTCHAR A
ENCODING 65
SWIDTH 800 0
DWIDTH 8 0
BBX 5 7 -1 -2
BITMAP
70
88
88
F8
88
88
88
ENDCHAR
STARTCHAR Eacute
ENCODING 201
SWIDTH 900 0
DWIDTH 9 0
BBX 5 7 -1 -2
BITMAP
20
70
88
F8
80
80
F8
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 400 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
ENDFONT
)";
	struct FontGraph {
		std::filesystem::path Directory;
		std::filesystem::path FontPath;
		engine::core::Name Graph{"font-input-test"};
		FontGraph() {
			Directory = std::filesystem::temp_directory_path() /
						("client-font-input-" +
						 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(Directory / "imagegraphs");
			FontPath = Directory / "font.bdf";
			std::ofstream font(FontPath, std::ios::binary);
			font.write(BITMAP_FONT.data(), static_cast<std::streamsize>(BITMAP_FONT.size()));
			font.close();
			REQUIRE(font.good());
		}
		~FontGraph() {
			std::error_code ignored;
			std::filesystem::remove_all(Directory, ignored);
		}
		Document DocumentWithText(bool transformed = false, bool transform = false) const {
			Document document;
			document.FormatVersion = 9;
			document.Nodes.push_back({"font_data", "pc.font_data", "", {}, {{"font", FontPath.string()}}});
			Node text{
				"text",
				"pc.text",
				"",
				{},
				{{"text", std::string("A\xC3\xA9")},
				 {"size", int64_t{10}},
				 {"use_sdf", true},
				 {"color", Colour{255, 0, 0, 255}}}
			};
			if (transformed) text.Values.push_back({"change_case", EnumValue{2}});
			document.Nodes.push_back(std::move(text));
			document.Links.push_back({"font_data", "font", "text", "font"});
			if (transform) {
				document.Nodes.push_back(
					{"transform",
					 "image.transform_3d",
					 "",
					 {},
					 {{"position", Vector3{}},
					  {"anchor", Vector3{}},
					  {"rotation", Quaternion{}},
					  {"scale", Vector3{1, 1, 1}},
					  {"texture_tiling", Vector2{1, 1}},
					  {"projection", EnumValue{1}},
					  {"fov", 45.0},
					  {"view_range", Vector2{.001, 10}},
					  {"depth_range", Vector2{0, 1}}}}
				);
				document.Links.push_back({"text", "surface_out", "transform", "surface"});
				document.Outputs.push_back({"out", "transform", "rendered"});
			} else {
				document.Outputs.push_back({"out", "text", "surface_out"});
			}
			return document;
		}
		void Write(const Document &document) const {
			std::ofstream file(client::ImageGraphDocumentPath(Directory, Graph), std::ios::binary);
			file << engine::imagegraph::Write(document);
		}
		engine::imagegraphfont::GraphFontConfiguration Configuration() const {
			engine::imagegraphfont::GraphFontConfiguration configuration;
			configuration.Context.AliasMapKnown = true;
			configuration.Context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
			configuration.Context.Playing = false;
			configuration.ReadGrants.push_back({"text", FontPath, false, "font"});
			return configuration;
		}
	};
} // namespace

TEST_CASE(
	"client frame loader carries exact BDF grants through Unicode Text "
	"evaluation",
	"[client][imagegraph]"
) {
	FontGraph file;
	file.Write(file.DocumentWithText(true));
	engine::imagegraphfont::GraphFontInputs fonts;
	const auto policy = engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);
	Diagnostic diagnostic;
	REQUIRE(fonts.Replace(file.Configuration(), policy, Limits::MaximumEvaluationBytes, diagnostic));
	const auto frame = client::LoadImageGraphFrame(
		file.Directory, file.Graph, engine::core::Name("out"), 4, 9, nullptr, &fonts
	);
	INFO(diagnostic.Message << ':' << frame.Diagnostic.Message);
	REQUIRE(frame.Status == Status::Ok);
	CHECK(frame.Image.Width > 0);
	CHECK(frame.Image.Height > 0);
	bool visibleRed = false;
	for (size_t index = 0; index + 3 < frame.Image.Pixels.size(); index += 4) {
		visibleRed |= frame.Image.Pixels[index] != 0;
		CHECK(frame.Image.Pixels[index + 1] == 0);
		CHECK(frame.Image.Pixels[index + 2] == 0);
	}
	CHECK(visibleRed);
	file.Write(file.DocumentWithText(false));
	const auto untransformed = client::LoadImageGraphFrame(
		file.Directory, file.Graph, engine::core::Name("out"), 4, 9, nullptr, &fonts
	);
	INFO(untransformed.Diagnostic.Message);
	REQUIRE(untransformed.Status == Status::Ok);
	CHECK(frame.Image.Width > untransformed.Image.Width);
	CHECK(fonts.Revision() == 1);
	auto incomplete = file.Configuration();
	incomplete.Context.Playing.reset();
	engine::imagegraphfont::GraphFontInputs missingPlayback;
	REQUIRE(missingPlayback.Replace(incomplete, policy, Limits::MaximumEvaluationBytes, diagnostic));
	const auto refused = client::LoadImageGraphFrame(
		file.Directory, file.Graph, engine::core::Name("out"), 4, 9, nullptr, &missingPlayback
	);
	CHECK(refused.Status == Status::UnsupportedExecution);
}

TEST_CASE(
	"client transform builder binds the exact font owner before "
	"capturing linked Text",
	"[client][imagegraph]"
) {
	FontGraph file;
	const Document document = file.DocumentWithText(false, true);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphfont::GraphFontInputs fonts;
	REQUIRE(fonts.Replace(
		file.Configuration(),
		engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
		Limits::MaximumEvaluationBytes,
		diagnostic
	));
	engine::render::imagegraph::TransformImage3DRequest request;
	const bool built = client::detail::BuildTransformRequest(
		document, plan, document.Nodes.back(), 0, 0, false, request, diagnostic, nullptr, nullptr, 1, &fonts
	);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
	REQUIRE(built);
	CHECK(request.Front.Width > 0);
	CHECK(request.Front.Height > 0);
	CHECK_FALSE(request.Front.Pixels.empty());
}

TEST_CASE("client font generation advances only after a bounded owner replacement", "[client][imagegraph]") {
	FontGraph file;
	file.Write(file.DocumentWithText(true));
	client::ImageGraphRuntime runtime;
	engine::render::Renderer renderer;
	Diagnostic diagnostic;
	const auto policy = engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);
	REQUIRE(runtime.FontGeneration() == 0);
	REQUIRE(runtime.PrepareFonts(file.Configuration(), policy, renderer, diagnostic) == Status::Ok);
	const uint64_t installed = runtime.FontGeneration();
	REQUIRE(installed == 1);
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("client-font-fixed-tick");
	const auto entity = store.Create();
	engine::scene::ImageGraphBinding binding;
	binding.Graph = file.Graph;
	binding.Output = engine::core::Name("out");
	binding.Texture = engine::core::Name("font-fixed-tick");
	binding.TickPolicy = engine::scene::ImageGraphTickPolicy::Fixed;
	binding.FixedTick = 37;
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, binding));
	(void)runtime.Refresh(store, renderer, engine::core::Name("font-world"), file.Directory);
	REQUIRE(runtime.CachedDocumentCount() == 1);
	CHECK(
		runtime.PrepareFonts(file.Configuration(), policy, renderer, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(runtime.FontGeneration() == installed);
	CHECK(runtime.CachedDocumentCount() == 1);
	CHECK(runtime.PrepareFonts(file.Configuration(), policy, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.FontGeneration() == installed + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
}
