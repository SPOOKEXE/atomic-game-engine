// The diagnostic geometry is deliberate approximation, so its contract is
// pinned without a GPU: finite influence probes stop at render bounds and a
// frozen visibility camera can differ from the inspection camera.

#include <engine/render/RuntimeDiagnostics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <numbers>
#include <vector>

TEST_SUITE_ID("engine.render.runtime_diagnostics")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::render::CullForCamera;
using engine::render::LightPathGeometry;
using engine::render::LightProbeEvent;
using engine::render::SceneLight;
using engine::render::View;
using engine::scene::Camera;
using engine::scene::DrawInstance;

namespace {
	const engine::render::LightProbeSegment *Find(
		std::span<const engine::render::LightProbeSegment> paths, LightProbeEvent event, Vector3 direction
	) {
		for (const auto &path : paths) {
			if (path.Event == event && (path.Line.To - path.Line.From).Unit() == direction) return &path;
		}
		return nullptr;
	}
}

TEST_CASE("light influence probes terminate on the first render bound", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Position = Vector3{};
	light.Range = 10.0f;
	DrawInstance wall;
	wall.Frame = CFrame(Vector3{0.0f, 0.0f, -5.0f});
	wall.HalfExtent = Vector3{1.0f, 1.0f, 1.0f};
	DrawInstance lamp;
	lamp.Frame = CFrame{};
	lamp.HalfExtent = Vector3{0.25f, 0.25f, 0.25f};
	const std::array<DrawInstance, 2> instances{lamp, wall};

	LightPathGeometry paths;
	paths.Build(std::span<const SceneLight>(&light, 1), instances);

	const auto *travel = Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(travel != nullptr);
	CHECK(travel->Line.To.Z == Catch::Approx(-4.0f));
	bool terminationAtWall = false;
	for (const auto &segment : paths.Segments()) {
		if (segment.Event != LightProbeEvent::Termination) continue;
		const Vector3 middle = (segment.Line.From + segment.Line.To) * 0.5f;
		terminationAtWall = terminationAtWall || middle.Z == Catch::Approx(-4.0f);
	}
	CHECK(terminationAtWall);
}

TEST_CASE("light influence probes end at range when no render bound is met", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Position = Vector3{};
	light.Range = 7.0f;

	LightPathGeometry paths;
	paths.Build(std::span<const SceneLight>(&light, 1), {});

	const auto *travel = Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(travel != nullptr);
	CHECK(travel->Line.To.Z == Catch::Approx(-7.0f));
}

TEST_CASE("a frozen culling camera can select a different draw set than the inspection camera", "[render][runtime-diagnostics]") {
	std::array<DrawInstance, 2> instances{};
	instances[0].Frame = CFrame(Vector3{0.0f, 0.0f, -5.0f});
	instances[1].Frame = CFrame(Vector3{0.0f, 0.0f, 5.0f});
	for (DrawInstance &instance : instances) instance.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};

	std::vector<uint32_t> visible;
	const Camera camera;
	CullForCamera(instances, CFrame::Angles(0.0f, std::numbers::pi_v<float>, 0.0f), camera, 1.0f, visible);

	REQUIRE(visible.size() == 1);
	CHECK(visible[0] == 1);
}

TEST_CASE("a visibility pose stays separate from the inspection projection pose", "[render][runtime-diagnostics]") {
	View view;
	const Vector3 inspection{1.0f, 2.0f, 3.0f};
	const Vector3 visibility{7.0f, 8.0f, 9.0f};
	view.CameraFrame = CFrame(inspection);
	view.VisibilityFrame = CFrame(visibility);

	CHECK(view.CameraFrame.Position == inspection);
	CHECK(view.VisibilityCameraFrame().Position == visibility);
}
