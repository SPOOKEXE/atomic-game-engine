#include "ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
TEST_SUITE_ID("engine.render.source_sdf_host_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.source_sdf_capture")
TEST_CASE(
	"four RM GPU hosts feed downstream filters with exact pending ownership", "[render][gpu][sdf-host-gpu][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	using Access = render::test_support::TransformImage3DResidentTestAccess;
	struct Provider final : imagegraph::HostNodeProvider {
		render::Renderer &Render;
		const core::Name Owner{"sdf-host-gpu"};
		const core::Name Name{"sdf-host-gpu/rm"};
		bool Pending = false;
		imagegraph::HostNodeCapture Receipt;
		explicit Provider(render::Renderer &render) : Render(render) {}
		bool Capture(
			const imagegraph::HostNodeInvocation &invocation,
			imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			const bool ready =
				Render.CaptureSourceSdfAsync(invocation, Owner, Name, Receipt, failure, &Pending);
			if (ready) output = Receipt;
			return ready;
		}
	} provider(fixture.Render);
	for (const std::string type : {"pc.rm_render", "pc.rm_render_scatter", "pc.rm_cloud", "pc.rm_terrain"}) {
		DYNAMIC_SECTION(type) {
			imagegraph::Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"shape", "pc.rm_primitive", "", {}, {}},
				{"rm",
				 type,
				 "",
				 {},
				 {{"dimension", imagegraph::Vector2{8, 6}}, {"dimension_unit", imagegraph::EnumValue{0}}}},
				{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
			};
			if (type == "pc.rm_render" || type == "pc.rm_render_scatter")
				document.Links.push_back({"shape", "sdf_object", "rm", "sdf_object"});
			document.Links.push_back({"rm", "surface_out", "invert", "image"});
			document.Outputs = {{"out", "invert", "image"}};
			imagegraph::Plan plan;
			imagegraph::Diagnostic diagnostic;
			REQUIRE(imagegraph::Compile(document, plan, diagnostic) == imagegraph::Status::Ok);
			imagegraph::EvaluationRequest clock{.Seed = 7, .HostProvider = &provider};
			imagegraph::Image result;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			bool ready = false;
			while (!ready && std::chrono::steady_clock::now() < deadline) {
				Access::Poll(fixture.Render);
				ready = imagegraph::Evaluate(document, plan, "out", clock, result, diagnostic) ==
						imagegraph::Status::Ok;
				INFO(diagnostic.Message);
				REQUIRE((ready || provider.Pending));
				if (ready) break;
				const auto slots = Access::Slots(fixture.Render);
				if (std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
						return slot.Phase == render::test_support::TransformImage3DQueuePhase::Queued;
					}))
					REQUIRE(Access::RecordAndSubmit(fixture.Render));
				SDL_Delay(1);
			}
			REQUIRE(ready);
			REQUIRE(provider.Receipt.Images.size() == 1);
			const auto &raw = provider.Receipt.Images[0].Data;
			CHECK(provider.Receipt.Images[0].Port == "surface_out");
			CHECK(raw.Width == 8);
			CHECK(raw.Height == 6);
			REQUIRE(raw.Pixels.size() == 192);
			REQUIRE(result.Pixels.size() == raw.Pixels.size());
			for (size_t pixel = 0; pixel < raw.Pixels.size(); ++pixel)
				CHECK(result.Pixels[pixel] == (pixel % 4 == 3 ? raw.Pixels[pixel] : 255 - raw.Pixels[pixel]));
			imagegraph::HostNodeCapture prepared;
			REQUIRE(
				imagegraph::PrepareHostCapture(document, plan, "rm", clock, prepared, diagnostic) ==
				imagegraph::Status::Ok
			);
			imagegraph::HostNodeInvocation invocation{
				document.Nodes[1],
				clock,
				prepared.Inputs,
				{},
				64ull * 1024 * 1024,
				nullptr,
				prepared.CameraPolicy->InheritedSurfaceFormat,
				1,
				prepared.CameraPolicy
			};
			imagegraph::HostNodeCapture blocking;
			std::string failure;
			REQUIRE(fixture.Render.CaptureSourceSdf(invocation, provider.Owner, blocking, failure));
			REQUIRE(blocking.Images.size() == 1);
			CHECK(blocking.Images[0].Data == raw);
			const auto previous = provider.Receipt.Images[0].Data;
			clock.Seed = 8;
			CHECK(
				imagegraph::Evaluate(document, plan, "out", clock, result, diagnostic) ==
				imagegraph::Status::UnsupportedExecution
			);
			REQUIRE(provider.Pending);
			CHECK(provider.Receipt.Images[0].Data == previous);
			clock.Tick = 1;
			CHECK(
				imagegraph::Evaluate(document, plan, "out", clock, result, diagnostic) ==
				imagegraph::Status::UnsupportedExecution
			);
			REQUIRE(provider.Pending);
			const auto slots = Access::Slots(fixture.Render);
			CHECK(std::count_if(slots.begin(), slots.end(), [](const auto &slot) {
					  return slot.Phase == render::test_support::TransformImage3DQueuePhase::Queued;
				  }) == 1);
			fixture.Render.CancelComposerCapture(provider.Owner, provider.Name);
			for (const auto &slot : Access::Slots(fixture.Render))
				CHECK(slot.Phase == render::test_support::TransformImage3DQueuePhase::Free);
			provider.Receipt = {};
		}
	}
}
