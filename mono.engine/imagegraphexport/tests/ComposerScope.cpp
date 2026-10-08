#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphexport.composer_scope")
using namespace engine::imagegraph;
using namespace engine::imagegraphexport;

TEST_CASE(
	"Image composer refuses video and WAV before resolving files or starting encoders",
	"[imagegraph][composer_scope]"
) {
	GraphExportSettings settings;
	settings.Scope = ComposerScope::ImageOnly;
	settings.Input = "/ungranted/missing.graph";
	settings.OutputId = "image";
	settings.Animation = true;
	settings.VideoEncoder = "/ungranted/encoder";
	for (const auto extension : {".mp4", ".MP4", ".webm", ".wav"}) {
		settings.Output = std::string{"/ungranted/missing/output"} + extension;
		std::filesystem::path retained = "sentinel";
		std::string failure;
		CHECK_FALSE(ExportGraph(settings, failure, &retained));
		CHECK(failure == "Video and audio exports are disabled in the image composer");
		CHECK(retained == "sentinel");
	}
}

TEST_CASE(
	"Encoder admission checks actual target extension and retains generic video support",
	"[imagegraph][composer_scope]"
) {
	GraphExportSettings settings;
	settings.Scope = ComposerScope::ImageOnly;
	settings.Output = "claimed.gif";
	settings.Animation = true;
	settings.ImageEncoder = "/explicit/image";
	settings.VideoEncoder = "/explicit/video";
	std::filesystem::path executable;
	std::vector<std::string> arguments;
	std::string failure;
	CHECK_FALSE(BuildGraphEncoderArguments(
		settings, "/frames", "/actual/output.mp4", 2, executable, arguments, failure
	));
	CHECK(arguments.empty());
	CHECK(executable.empty());
	REQUIRE(BuildGraphEncoderArguments(
		settings, "/frames", "/actual/output.gif", 2, executable, arguments, failure
	));
	settings.Scope = ComposerScope::Unrestricted;
	settings.Output = "output.mp4";
	REQUIRE(BuildGraphEncoderArguments(
		settings, "/frames", "/actual/output.mp4", 2, executable, arguments, failure
	));
	CHECK(executable == settings.VideoEncoder);
}
