// CPU planning for a mirror scene with every root in view. No device work or
// texture allocation is included; terminal depth still owes every root capture.

#include "../src/SurfaceCapturePlan.hpp"

#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <stdexcept>

TEST_SUITE_ID("engine.render.bench.surface-capture-plan")
TEST_DEPENDS("engine.render.surfacecaptureplan")

namespace {
	struct VisibleMirrors {
		std::array<engine::render::SurfaceView, 320> Mirrors;
		engine::render::SurfaceCaptureRequest Request;
		engine::render::SurfaceCapturePlan Plan;

		VisibleMirrors() {
			using namespace engine;
			for (size_t index = 0; index < Mirrors.size(); ++index) {
				auto &mirror = Mirrors[index];
				mirror.Index = static_cast<int16_t>(index);
				mirror.PaneCentre = {0, 0, -3};
				mirror.PaneNormal = {0, 0, 1};
				mirror.PaneFirst = {2, 0, 0};
				mirror.PaneSecond = {0, 2, 0};
				mirror.Width = 16;
				mirror.Height = 8;
			}
			Request.Mirrors = Mirrors;
			Request.Projection = scene::ResolveCamera({}, {}, 2).Projection;
			Request.Width = 32;
			Request.Height = 16;
			Request.Depth = 1;
			Request.PixelBudget = Mirrors.size() * 16 * 8;
			if (render::PlanSurfaceCaptures(Request, Plan) != render::SurfaceCaptureStatus::Ok ||
				Plan.Entries.size() != Mirrors.size()) {
				throw std::runtime_error("320 visible mirror roots did not produce complete captures");
			}
		}
	};

	VisibleMirrors &Fixture() {
		static VisibleMirrors fixture;
		return fixture;
	}
}

BENCH("surface captures · 320 visible roots · one bounce · retained buffers", 10) {
	auto &fixture = Fixture();
	for (size_t iteration = 0; iteration < 10; ++iteration) {
		engine::testing::Consume(engine::render::PlanSurfaceCaptures(fixture.Request, fixture.Plan));
		engine::testing::Consume(fixture.Plan.Entries.size());
	}
}
