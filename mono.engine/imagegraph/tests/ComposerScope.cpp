#include <engine/imagegraph/ComposerScope.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.composer_scope")
using namespace engine::imagegraph;

TEST_CASE(
	"Image composer excludes resource workflows without excluding image data", "[imagegraph][composer_scope]"
) {
	for (const auto type :
		 {"pc.3_d_mesh_cube",
		  "pc.3_d_mesh_obj",
		  "pc.3_d_particle",
		  "pc.p_system_3_d_acceleration",
		  "pc.audio_window",
		  "pc.wav_file_read",
		  "image.audio_recording",
		  "pc.image_mp4"}) {
		INFO(type);
		REQUIRE(FindSchema(type));
		CHECK_FALSE(ComposerNodeEnabled(std::string_view{type}, ComposerScope::ImageOnly));
		CHECK(ComposerNodeEnabled(std::string_view{type}, ComposerScope::Unrestricted));
	}
	for (const auto type :
		 {"pc.particle",
		  "pc.vfx_spawner",
		  "pc.vfx_renderer",
		  "pc.flip_to_vfx",
		  "pc.mesh_create_path",
		  "pc.mesh_warp",
		  "pc.surface_project_3_d",
		  "pc.3_d_transform_image",
		  "image.transform_3d",
		  "pc.point_3_d_camera",
		  "value.noise_field",
		  "value.sample_noise",
		  "pc.array",
		  "pc.fft",
		  "pc.image_gif"}) {
		INFO(type);
		REQUIRE(FindSchema(type));
		CHECK(ComposerNodeEnabled(std::string_view{type}, ComposerScope::ImageOnly));
	}
}

TEST_CASE(
	"Scope refusal names the saved node and retains its authored graph", "[imagegraph][composer_scope]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"cube", "pc.3_d_mesh_cube", "", {}, {}}, {"mirror", "pc.3_d_mirror", "", {}, {}}};
	document.Links = {{"cube", "mesh", "mirror", "mesh"}};
	document.Outputs = {{"mesh-output", "mirror", "mesh"}};
	const auto saved = Write(document);
	Diagnostic diagnostic;
	CHECK(
		CheckComposerNodeScope(document.Nodes.front(), ComposerScope::ImageOnly, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "cube");
	CHECK(diagnostic.Message == "3D meshes are disabled in the image composer");
	CHECK(Write(document) == saved);
	Document restored;
	REQUIRE(Read(saved, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Dynamic resource ports obey scope while planar particle ports remain enabled",
	"[imagegraph][composer_scope]"
) {
	Node node{"router", "pc.group_input", "", {}, {}};
	node.DynamicOutputs = {{"value", ValueType::Particle}};
	CHECK(ComposerNodeEnabled(node, ComposerScope::ImageOnly));
	node.DynamicOutputs.front().Type = ValueType::Mesh2D;
	CHECK(ComposerNodeEnabled(node, ComposerScope::ImageOnly));
	node.DynamicOutputs.front().Type = ValueType::Mesh;
	CHECK_FALSE(ComposerNodeEnabled(node, ComposerScope::ImageOnly));
	node.DynamicOutputs.clear();
	node.DynamicInputs = {{"samples", ValueType::AudioBit, {}}};
	Diagnostic diagnostic;
	CHECK(CheckComposerNodeScope(node, ComposerScope::ImageOnly, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "router");
	CHECK(diagnostic.Message == "Audio is disabled in the image composer");
	CHECK(ComposerNodeEnabled(node, ComposerScope::Unrestricted));
}

TEST_CASE(
	"Image scope permits animated images but refuses video and audio sinks", "[imagegraph][composer_scope]"
) {
	for (const auto extension : {".mp4", ".MP4", ".webm", ".WeBm", ".wav", ".WAV"}) {
		CHECK_FALSE(ComposerExportEnabled(extension, ComposerScope::ImageOnly));
		CHECK(ComposerExportEnabled(extension, ComposerScope::Unrestricted));
	}
	for (const auto extension : {".png", ".gif", ".GIF", ".apng", ".json", ".csv"})
		CHECK(ComposerExportEnabled(extension, ComposerScope::ImageOnly));
}
