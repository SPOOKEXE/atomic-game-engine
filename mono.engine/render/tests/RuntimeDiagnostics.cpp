// The diagnostic geometry is deliberate approximation, so its contract is
// pinned without a GPU: probes cross noncasting bounds, stop at opaque casting
// bounds, and a frozen visibility camera can differ from the inspection camera.

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
	const engine::render::LightProbeSegment *
	Find(std::span<const engine::render::LightProbeSegment> paths, LightProbeEvent event, Vector3 direction) {
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

TEST_CASE(
	"light influence probes end at range when no render bound is met", "[render][runtime-diagnostics]"
) {
	SceneLight light;
	light.Position = Vector3{};
	light.Range = 7.0f;

	LightPathGeometry paths;
	paths.Build(std::span<const SceneLight>(&light, 1), {});

	const auto *travel = Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(travel != nullptr);
	CHECK(travel->Line.To.Z == Catch::Approx(-7.0f));
}

TEST_CASE(
	"light probes pass through noncasting bounds before an opaque stop", "[render][runtime-diagnostics]"
) {
	SceneLight light;
	light.Range = 10.0f;
	DrawInstance pane;
	pane.Frame = CFrame(Vector3{0.0f, 0.0f, -3.0f});
	pane.HalfExtent = Vector3{1.0f, 1.0f, 0.5f};
	DrawInstance wall;
	wall.Frame = CFrame(Vector3{0.0f, 0.0f, -6.0f});
	wall.HalfExtent = Vector3{1.0f, 1.0f, 1.0f};
	std::array<DrawInstance, 2> instances{pane, wall};
	LightPathGeometry paths;
	const auto build = [&] { paths.Build(std::span(&light, 1), instances); };
	const auto pass = [&] {
		return Find(paths.Segments(), LightProbeEvent::PassThrough, Vector3{0.0f, 0.0f, -1.0f});
	};

	instances[0].Transparency = 0.5f;
	build();
	REQUIRE(pass() != nullptr);
	CHECK(pass()->Line.From.Z == Catch::Approx(-2.5f));
	CHECK(pass()->Line.To.Z == Catch::Approx(-3.5f));
	bool travelAfterPane = false;
	bool stopAtWall = false;
	for (const auto &segment : paths.Segments()) {
		if (segment.Event == LightProbeEvent::EmptySpace && segment.Line.From.Z == Catch::Approx(-3.5f) &&
			segment.Line.To.Z == Catch::Approx(-5.0f))
			travelAfterPane = true;
		if (segment.Event != LightProbeEvent::Termination) continue;
		stopAtWall |= ((segment.Line.From.Z + segment.Line.To.Z) * 0.5f == Catch::Approx(-5.0f));
	}
	CHECK(travelAfterPane);
	CHECK(stopAtWall);

	instances[0].Transparency = 0.0f;
	instances[0].TransmissionFactor = 0.5f;
	build();
	CHECK(pass() != nullptr);

	instances[0].TransmissionFactor = 0.0f;
	instances[0].CastShadow = false;
	build();
	CHECK(pass() != nullptr);

	instances[0].CastShadow = true;
	instances[0].Alpha = engine::scene::AlphaMode::Transparency;
	build();
	CHECK(pass() == nullptr);
	const auto *opaqueTravel =
		Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(opaqueTravel != nullptr);
	CHECK(opaqueTravel->Line.To.Z == Catch::Approx(-2.5f));
}

TEST_CASE(
	"light probe pass-through intervals remain bounded and nonoverlapping", "[render][runtime-diagnostics]"
) {
	SceneLight light;
	light.Range = 30.0f;
	std::array<DrawInstance, 18> panes{};
	for (size_t index = 0; index < panes.size(); ++index) {
		panes[index].Frame = CFrame(Vector3{0.0f, 0.0f, -1.0f - static_cast<float>(index)});
		panes[index].HalfExtent = Vector3{0.5f, 0.5f, 0.25f};
		panes[index].Transparency = 0.5f;
	}
	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), panes);
	size_t passes = 0;
	float furthest = 0.0f;
	for (const auto &segment : paths.Segments()) {
		if (segment.Event != LightProbeEvent::PassThrough ||
			(segment.Line.To - segment.Line.From).Unit() != Vector3{0.0f, 0.0f, -1.0f})
			continue;
		++passes;
		CHECK(segment.Line.From.Z <= furthest);
		furthest = segment.Line.To.Z;
	}
	CHECK(passes == 16);
	CHECK(furthest == Catch::Approx(-16.25f));
}

TEST_CASE("overlapping pass-through bounds stop at the light range", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Range = 4.5f;
	std::array<DrawInstance, 2> panes{};
	panes[0].Frame = CFrame(Vector3{0.0f, 0.0f, -3.0f});
	panes[1].Frame = CFrame(Vector3{0.0f, 0.0f, -4.0f});
	for (DrawInstance &pane : panes) {
		pane.HalfExtent = Vector3{0.5f, 0.5f, 1.0f};
		pane.Transparency = 0.5f;
	}
	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), panes);
	float orangeDistance = 0.0f;
	float previousEnd = 0.0f;
	for (const auto &segment : paths.Segments()) {
		if (segment.Event != LightProbeEvent::PassThrough ||
			(segment.Line.To - segment.Line.From).Unit() != Vector3{0.0f, 0.0f, -1.0f})
			continue;
		CHECK(segment.Line.From.Z <= previousEnd);
		orangeDistance += segment.Line.From.Z - segment.Line.To.Z;
		previousEnd = segment.Line.To.Z;
	}
	CHECK(orangeDistance == Catch::Approx(2.5f));
	CHECK(previousEnd == Catch::Approx(-4.5f));
}

TEST_CASE("light probes reflect from a surface camera before terminating", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Range = 10.0f;
	DrawInstance mirror;
	mirror.Frame = CFrame(Vector3{0.0f, 0.0f, -3.0f});
	mirror.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	mirror.Surface = 0;
	DrawInstance wall;
	wall.Frame = CFrame(Vector3{0.0f, 0.0f, 2.0f});
	wall.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	const std::array<DrawInstance, 2> instances{mirror, wall};

	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), instances);

	const auto *incoming = Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	const auto *reflection = Find(paths.Segments(), LightProbeEvent::Reflection, Vector3{0.0f, 0.0f, 1.0f});
	REQUIRE(incoming != nullptr);
	REQUIRE(reflection != nullptr);
	CHECK(incoming->Line.To.Z == Catch::Approx(-2.9f));
	CHECK(reflection->Line.From.Z == Catch::Approx(-2.9f));
	CHECK(reflection->Line.To.Z == Catch::Approx(1.9f));
	CHECK(reflection->Line.Colour.R == Catch::Approx(1.0f));
	CHECK(reflection->Line.Colour.G == Catch::Approx(0.48f));
	CHECK(reflection->Line.Colour.B == Catch::Approx(0.08f));
}

TEST_CASE("light probes do not reflect through an opaque bound", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Range = 10.0f;
	DrawInstance wall;
	wall.Frame = CFrame(Vector3{0.0f, 0.0f, -2.0f});
	wall.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	DrawInstance mirror;
	mirror.Frame = CFrame(Vector3{0.0f, 0.0f, -4.0f});
	mirror.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	mirror.Surface = 0;
	const std::array<DrawInstance, 2> instances{wall, mirror};

	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), instances);

	const auto *incoming = Find(paths.Segments(), LightProbeEvent::EmptySpace, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(incoming != nullptr);
	CHECK(incoming->Line.To.Z == Catch::Approx(-1.9f));
	CHECK(std::none_of(paths.Segments().begin(), paths.Segments().end(), [](const auto &segment) {
		return segment.Event == LightProbeEvent::Reflection;
	}));
}

TEST_CASE("light probe reflections remain within the source range", "[render][runtime-diagnostics]") {
	SceneLight light;
	light.Range = 5.0f;
	DrawInstance mirror;
	mirror.Frame = CFrame(Vector3{0.0f, 0.0f, -3.0f});
	mirror.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	mirror.Surface = 0;

	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), std::span(&mirror, 1));

	const auto *reflection = Find(paths.Segments(), LightProbeEvent::Reflection, Vector3{0.0f, 0.0f, 1.0f});
	REQUIRE(reflection != nullptr);
	CHECK(reflection->Line.From.Z == Catch::Approx(-2.9f));
	CHECK(reflection->Line.To.Z == Catch::Approx(-0.8f));
}

TEST_CASE(
	"light probes pass through linked portal surfaces without reflecting", "[render][runtime-diagnostics]"
) {
	SceneLight light;
	light.Range = 10.0f;
	DrawInstance portal;
	portal.Frame = CFrame(Vector3{0.0f, 0.0f, -3.0f});
	portal.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	portal.Surface = 0;
	portal.SurfaceIsPortal = true;
	DrawInstance wall;
	wall.Frame = CFrame(Vector3{0.0f, 0.0f, -6.0f});
	wall.HalfExtent = Vector3{1.0f, 1.0f, 0.1f};
	const std::array<DrawInstance, 2> instances{portal, wall};

	LightPathGeometry paths;
	paths.Build(std::span(&light, 1), instances);

	const auto *passThrough =
		Find(paths.Segments(), LightProbeEvent::PassThrough, Vector3{0.0f, 0.0f, -1.0f});
	REQUIRE(passThrough != nullptr);
	CHECK(passThrough->Line.From.Z == Catch::Approx(-2.9f));
	CHECK(passThrough->Line.To.Z == Catch::Approx(-3.1f));
	CHECK(std::none_of(paths.Segments().begin(), paths.Segments().end(), [](const auto &segment) {
		return segment.Event == LightProbeEvent::Reflection;
	}));
}

TEST_CASE(
	"a frozen culling camera can select a different draw set than the inspection camera",
	"[render][runtime-diagnostics]"
) {
	std::array<DrawInstance, 2> instances{};
	instances[0].Frame = CFrame(Vector3{0.0f, 0.0f, -5.0f});
	instances[1].Frame = CFrame(Vector3{0.0f, 0.0f, 5.0f});
	for (DrawInstance &instance : instances)
		instance.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};

	std::vector<uint32_t> visible;
	const Camera camera;
	CullForCamera(instances, CFrame::Angles(0.0f, std::numbers::pi_v<float>, 0.0f), camera, 1.0f, visible);

	REQUIRE(visible.size() == 1);
	CHECK(visible[0] == 1);
}

TEST_CASE(
	"a visibility pose stays separate from the inspection projection pose", "[render][runtime-diagnostics]"
) {
	View view;
	const Vector3 inspection{1.0f, 2.0f, 3.0f};
	const Vector3 visibility{7.0f, 8.0f, 9.0f};
	view.CameraFrame = CFrame(inspection);
	view.VisibilityFrame = CFrame(visibility);

	CHECK(view.CameraFrame.Position == inspection);
	CHECK(view.VisibilityCameraFrame().Position == visibility);
}
