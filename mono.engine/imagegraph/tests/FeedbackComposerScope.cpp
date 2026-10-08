#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.feedback_composer_scope")
using namespace engine::imagegraph;
namespace {
	struct CountingHost final : HostNodeProvider {
		uint32_t Calls = 0;
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override {
			++Calls;
			failure = "unexpected excluded capture";
			return false;
		}
	};
	Document Mixed() {
		Document document;
		document.FormatVersion = 9;
		document.Project.emplace();
		document.Project->SurfaceWidth = document.Project->SurfaceHeight = 2;
		document.Nodes = {
			{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:image"}}}},
			{"invert", "image.invert", "", {}, {{"include_alpha", false}, {"mix", 1.}}},
			{"audio", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}},
			{"cube", "pc.3_d_mesh_cube", "", {}, {}}
		};
		document.Links = {{"prior", "image", "invert", "image"}};
		document.Outputs = {
			{"image", "invert", "image"}, {"audio", "audio", "samples"}, {"mesh", "cube", "mesh"}
		};
		return document;
	}
	Plan Checked(const Document &document) {
		Plan plan;
		Diagnostic error;
		const auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
}
TEST_CASE(
	"feedback memo cannot return excluded cached resources after a scope change", "[composer_scope][feedback]"
) {
	const auto document = Mixed();
	const auto plan = Checked(document);
	const std::array<AudioCaptureFrame, 1> audio{{{"mono", 0, {.25, -.5}}}};
	for (const auto output : {std::string_view{"audio"}, std::string_view{"mesh"}}) {
		INFO(output);
		CapturedFeedbackHost host;
		EvaluationRequest request;
		request.AudioFrames = audio;
		Diagnostic error;
		REQUIRE(host.Prepare(document, plan, 1, 1, request, error, Limits::MaximumEvaluationBytes, output));
		REQUIRE(host.Value(output));
		const auto saved = std::get<EvaluatedValue>(host.Value(output)->Output);
		const auto image = *host.Output("image");
		const auto frame = host.PreparedFrame(1, 1);
		CountingHost provider;
		EvaluationRequest restricted;
		restricted.Scope = ComposerScope::ImageOnly;
		restricted.HostProvider = &provider;
		CHECK_FALSE(
			host.Prepare(document, plan, 1, 1, restricted, error, Limits::MaximumEvaluationBytes, output)
		);
		CHECK(error.Code == Status::UnsupportedExecution);
		CHECK(error.NodeId == (output == "audio" ? "audio" : "cube"));
		CHECK(
			error.Message == (output == "audio" ? "Audio is disabled in the image composer"
												: "3D meshes are disabled in the image composer")
		);
		CHECK(provider.Calls == 0);
		CHECK(restricted.ImageSources.empty());
		CHECK(restricted.DataReplay == nullptr);
		REQUIRE(host.Value(output));
		CHECK(std::get<EvaluatedValue>(host.Value(output)->Output) == saved);
		CHECK(*host.Output("image") == image);
		CHECK(host.PreparedFrame(1, 1) == frame);
		CHECK_FALSE(host.PrepareNodeInputs(
			document, plan, 1, 1, output == "audio" ? "audio" : "cube", restricted, error
		));
		CHECK(error.Code == Status::UnsupportedExecution);
		CHECK(provider.Calls == 0);
	}
}
TEST_CASE(
	"image scope replaces feedback generation while ignoring disconnected excluded nodes",
	"[composer_scope][feedback]"
) {
	const auto document = Mixed();
	const auto plan = Checked(document);
	CapturedFeedbackHost host;
	Diagnostic error;
	EvaluationRequest unrestricted;
	REQUIRE(host.Prepare(document, plan, 1, 1, unrestricted, error, Limits::MaximumEvaluationBytes, "image"));
	const auto saved = *host.Output("image");
	CountingHost provider;
	EvaluationRequest restricted;
	restricted.Scope = ComposerScope::ImageOnly;
	restricted.HostProvider = &provider;
	REQUIRE(host.Prepare(document, plan, 1, 1, restricted, error, Limits::MaximumEvaluationBytes, "image"));
	CHECK(*host.Output("image") == saved);
	CHECK(provider.Calls == 0);
	CHECK_FALSE(host.Value("audio"));
	CHECK_FALSE(host.Value("mesh"));
	EvaluationRequest invalid;
	invalid.Scope = static_cast<ComposerScope>(255);
	CHECK_FALSE(host.Prepare(document, plan, 1, 1, invalid, error, Limits::MaximumEvaluationBytes, "image"));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(*host.Output("image") == saved);
}
