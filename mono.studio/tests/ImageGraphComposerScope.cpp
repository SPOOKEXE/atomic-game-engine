#include "../src/ImageGraphComposerScope.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nodegraph/Graph.hpp>
#include <nodegraph/Registry.hpp>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph.composer_scope")

TEST_CASE(
	"Excluded saved mesh workflows retain canvas nodes and connections",
	"[studio][imagegraph][composer_scope]"
) {
	using namespace engine::imagegraph;
	Document source;
	source.FormatVersion = 9;
	source.Nodes = {{"cube", "pc.3_d_mesh_cube", "", {}, {}}, {"mirror", "pc.3_d_mirror", "", {}, {}}};
	source.Links = {{"cube", "mesh", "mirror", "mesh"}};
	source.Outputs = {{"mesh-output", "mirror", "mesh"}};
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	CHECK(canvas.LinkInto(ids.ToCanvas.at("mirror"), "mesh"));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved == source);
}

TEST_CASE(
	"Composer palette hides excluded resources while retaining saved node definitions",
	"[studio][imagegraph][composer_scope]"
) {
	studio::RegisterImageGraphNodeTypes();
	for (const auto id :
		 {"pc.3_d_mesh_cube",
		  "pc.3_d_particle",
		  "pc.wav_file_read",
		  "pc.audio_window",
		  "pc.image_mp4",
		  "image.audio_volume"}) {
		INFO(id);
		const auto *type = nodegraph::NodeTypes::Find(id);
		REQUIRE(type);
		CHECK(type->Hidden);
		CHECK_FALSE(studio::detail::ImageGraphComposerTypeVisible(id));
		CHECK(engine::imagegraph::FindSchema(id));
	}
	for (const auto id :
		 {"pc.particle",
		  "pc.vfx_renderer",
		  "pc.mesh_warp",
		  "pc.surface_project_3_d",
		  "value.noise_field",
		  "pc.fft",
		  "pc.image_gif"}) {
		INFO(id);
		const auto *type = nodegraph::NodeTypes::Find(id);
		REQUIRE(type);
		CHECK_FALSE(type->Hidden);
	}
	CHECK(studio::detail::ImageGraphComposerExportEnabled(".gif"));
	CHECK(studio::detail::ImageGraphComposerExportEnabled(".apng"));
	CHECK_FALSE(studio::detail::ImageGraphComposerExportEnabled(".mp4"));
	CHECK_FALSE(studio::detail::ImageGraphComposerExportEnabled(".wav"));
}
