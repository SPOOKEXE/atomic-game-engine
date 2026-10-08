#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraphexport/GraphExportSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

TEST_SUITE_ID("engine.imagegraphexport.composer_scope_session")
using namespace engine::imagegraph;
using namespace engine::imagegraphexport;
namespace {
	struct ScopeHost final : GraphExportSessionHost {
		std::vector<ComposerScope> Captured;
		bool Capture(const HostNodeInvocation &call, HostNodeCapture &out, std::string &failure) override {
			Captured.push_back(call.Request.Scope);
			REQUIRE(call.Authored.Type == "pc.hlsl");
			HostNodeCapture candidate;
			Diagnostic diagnostic;
			uint64_t bytes = 0;
			if (PrepareResolvedHostCapture(call, call.MaximumOperationBytes, candidate, bytes, diagnostic) !=
				Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			const auto image = std::find_if(call.Images.begin(), call.Images.end(), [](const auto &binding) {
				return binding.Port == "base_texture";
			});
			REQUIRE(image != call.Images.end());
			REQUIRE(image->Data);
			candidate.Images.push_back({"surface", *image->Data});
			out = std::move(candidate);
			return true;
		}
		bool Pending() const noexcept override {
			return false;
		}
		void Cancel() noexcept override {}
	};
	struct Fixture {
		inline static std::atomic<uint64_t> Sequence{0};
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("atomic-composer-scope-session-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
			 std::to_string(Sequence.fetch_add(1)));
		Document Graph;
		Plan Compiled;
		EvaluationSnapshot Inputs;
		EvaluationRequest Request;
		GraphExportSettings Grants;
		GraphExportGeneration Generation{1, 2, 3, 4, 5};
		ScopeHost Host;
		Fixture(bool video) {
			std::filesystem::create_directories(Root);
			Graph.FormatVersion = 9;
			Graph.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
			Graph.Nodes = {
				{"image", "image.solid", "", {}, {{"width", int64_t{1}}, {"height", int64_t{1}}}},
				{"shader", "pc.hlsl", "", {}, {{"main", std::string{"output.color=1;"}}}},
				{"export",
				 "pc.export",
				 "",
				 {},
				 {{"directory", Root.string()},
				  {"file_name", std::string{"image"}},
				  {"template", std::string{"%d%n%f"}},
				  {"type", EnumValue{video ? 2 : 1}},
				  {"format", EnumValue{video ? 3 : 0}}}}
			};
			Graph.Links = {
				{"image", "image", "shader", "base_texture"}, {"shader", "surface", "export", "surface"}
			};
			Graph.Outputs = {{"preview", "export", "preview"}};
			Diagnostic diagnostic;
			REQUIRE(Compile(Graph, Compiled, diagnostic) == Status::Ok);
			Request.HostProvider = &Host;
			REQUIRE(EvaluateNodeInputs(Graph, Compiled, "export", Request, Inputs, diagnostic) == Status::Ok);
			Grants.Input = Root / "unsaved.graph";
			Grants.Output = Root;
			Grants.OutputId = "preview";
			Grants.Scope = ComposerScope::ImageOnly;
			Host.Captured.clear();
		}
		~Fixture() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};
}

TEST_CASE("Scoped animation video admission refuses before staging", "[imagegraph][composer_scope]") {
	Fixture fixture(true);
	GraphExportSession session;
	std::string failure;
	CHECK_FALSE(session.BeginAuthored(
		fixture.Graph, fixture.Inputs, fixture.Request, fixture.Grants, "export", fixture.Generation, failure
	));
	CHECK(failure == "Video and audio exports are disabled in the image composer");
	CHECK(std::filesystem::is_empty(fixture.Root));
	CHECK(fixture.Host.Captured.empty());
	CHECK_FALSE(session.NextFrame());
}

TEST_CASE(
	"Image export session retains admitted scope through unrestricted resume observations",
	"[imagegraph][composer_scope]"
) {
	Fixture fixture(false);
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(
		fixture.Graph, fixture.Inputs, fixture.Request, fixture.Grants, "export", fixture.Generation, failure
	));
	REQUIRE(fixture.Request.Scope == ComposerScope::Unrestricted);
	GraphExportProgress progress = GraphExportProgress::Progress;
	for (size_t attempt = 0; attempt < 4 && progress == GraphExportProgress::Progress; ++attempt)
		progress = session.Resume(fixture.Request, fixture.Generation, fixture.Host, failure);
	INFO(failure);
	REQUIRE(progress == GraphExportProgress::Complete);
	REQUIRE_FALSE(fixture.Host.Captured.empty());
	for (const auto scope : fixture.Host.Captured)
		CHECK(scope == ComposerScope::ImageOnly);
	CHECK(std::filesystem::exists(fixture.Root / "image1.png"));
	CHECK(std::filesystem::exists(fixture.Root / "image2.png"));
}
