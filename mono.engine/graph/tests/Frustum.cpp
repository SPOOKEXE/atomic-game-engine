#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <vector>

TEST_SUITE_ID("engine.graph.frustum")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::graph::Cull;
using engine::graph::Frustum;
using engine::scene::Camera;
using engine::scene::DrawInstance;

namespace {
	// A camera at the origin looking down -Z, which is this engine's forward.
	//
	// Built through `scene::ResolveCamera` rather than by composing a
	// projection here - that function is the one place the engine decides what
	// a camera's matrices are, and a frustum derived from a second answer would
	// be testing agreement with something nothing else uses.
	//
	// The same camera as a matrix, which is what `VisibleSurfaces` takes: it
	// answers "how much of the screen" as well as "any of it", and a frustum
	// cannot say the first.
	glm::mat4 LookingMatrix(const Vector3 &from = Vector3::Zero, float aspect = 16.0f / 9.0f) {
		Camera camera;
		camera.NearPlane = 0.1f;
		camera.FarPlane = 100.0f;

		return engine::scene::ResolveCamera(CFrame{from}, camera, aspect).ViewProjection;
	}

	Frustum Looking(const Vector3 &from = Vector3::Zero, float aspect = 16.0f / 9.0f) {
		return Frustum::FromViewProjection(LookingMatrix(from, aspect));
	}

	DrawInstance At(const Vector3 &position, float halfExtent = 0.5f) {
		DrawInstance instance;
		instance.Frame = CFrame{position};
		instance.HalfExtent = Vector3{halfExtent, halfExtent, halfExtent};
		return instance;
	}

	struct RunningJobs {
		RunningJobs() {
			engine::parallel::Jobs::Start(4);
		}
		~RunningJobs() {
			engine::parallel::Jobs::Stop();
		}
	};
}

TEST_CASE("a point in front is inside and one behind is not", "[graph][frustum]") {
	// The near plane is the one a wrong clip convention breaks: Vulkan's volume
	// is `0 <= z <= w` and OpenGL's is `-w <= z <= w`, and using the second here
	// would put the near plane behind the camera and pass everything.
	const Frustum frustum = Looking();

	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -10.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, 10.0f}));
}

TEST_CASE("the near and far planes bound what is visible", "[graph][frustum]") {
	const Frustum frustum = Looking();

	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, -0.05f}));
	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -0.2f}));
	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -99.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{0.0f, 0.0f, -101.0f}));
}

TEST_CASE("the side planes widen with distance", "[graph][frustum]") {
	// The property that makes it a frustum rather than a box: a point far off
	// the axis is inside when it is far away and outside when it is close.
	const Frustum frustum = Looking();

	CHECK(frustum.Contains(Vector3{10.0f, 0.0f, -50.0f}));
	CHECK_FALSE(frustum.Contains(Vector3{10.0f, 0.0f, -1.0f}));
}

TEST_CASE("every plane's normal points inward", "[graph][frustum]") {
	// The sign convention the whole file rests on. A plane extracted with its
	// normal outward would reject everything it should accept, and the symptom
	// is a black screen rather than an obviously wrong test.
	const Frustum frustum = Looking();
	const Vector3 inside{0.0f, 0.0f, -10.0f};

	for (const auto &plane : frustum.Planes) {
		INFO("plane normal " << plane.Normal.X << ", " << plane.Normal.Y << ", " << plane.Normal.Z);
		CHECK(plane.SignedDistance(inside) >= 0.0f);
	}
}

TEST_CASE("plane normals are unit length, so distances are metres", "[graph][frustum]") {
	// A sign test would not need this. `SignedDistance` reports a real
	// distance, and the sphere test compares it against a radius - so an
	// unnormalised plane would cull spheres at an arbitrary scale.
	const Frustum frustum = Looking();

	for (const auto &plane : frustum.Planes) {
		CHECK(plane.Normal.Magnitude() == Approx(1.0f).margin(1e-4));
	}
}

TEST_CASE("a box straddling a plane is visible", "[graph][frustum]") {
	// The direction the error has to go: a `true` costs a draw call, and a
	// `false` is a hole in the world.
	const Frustum frustum = Looking();

	// Centred behind the near plane, but big enough to reach past it.
	CHECK(frustum.Intersects(AABB::FromCentre(Vector3{0.0f, 0.0f, 1.0f}, Vector3{0.0f, 0.0f, 5.0f})));

	// And one wholly behind is not.
	CHECK_FALSE(frustum.Intersects(AABB::FromCentre(Vector3{0.0f, 0.0f, 20.0f}, Vector3{1.0f, 1.0f, 1.0f})));
}

TEST_CASE("a sphere is culled by its radius", "[graph][frustum]") {
	const Frustum frustum = Looking();

	CHECK(frustum.Intersects(Vector3{0.0f, 0.0f, 5.0f}, 10.0f));
	CHECK_FALSE(frustum.Intersects(Vector3{0.0f, 0.0f, 5.0f}, 1.0f));
}

TEST_CASE("a degenerate matrix accepts everything rather than nothing", "[graph][frustum]") {
	// The conservative direction. A projection with a zero field of view is a
	// caller's bug, and culling the world away would hide it behind a symptom
	// that looks like a renderer fault.
	const Frustum frustum = Frustum::FromViewProjection(glm::mat4{0.0f});

	CHECK(frustum.Contains(Vector3{0.0f, 0.0f, -10.0f}));
	CHECK(frustum.Contains(Vector3{1000.0f, 1000.0f, 1000.0f}));
}

TEST_CASE("a rotated cube is bounded by what it actually reaches", "[graph][frustum]") {
	// `OrientedBoxBounds`, not the centre and the half-extent. A unit cube turned
	// forty-five degrees reaches root two, and a bound smaller than the shape
	// is a part that vanishes as it turns near the screen edge.
	DrawInstance turned;
	turned.Frame = CFrame::Angles(0.0f, 0.7853982f, 0.0f);
	turned.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};

	const AABB bound = engine::graph::BoundsOf(turned);
	CHECK(bound.Size().X == Approx(1.41421356f).margin(1e-3));
	CHECK(bound.Size().Y == Approx(1.0f).margin(1e-3));
}

TEST_CASE("culling keeps what is in front and drops what is behind", "[graph][frustum]") {
	const Frustum frustum = Looking();

	const std::vector<DrawInstance> instances{
		At(Vector3{0.0f, 0.0f, -10.0f}),
		At(Vector3{0.0f, 0.0f, 10.0f}),
		At(Vector3{0.0f, 0.0f, -20.0f}),
		At(Vector3{500.0f, 0.0f, -10.0f}),
	};

	std::vector<uint32_t> visible;
	CHECK(Cull(instances, frustum, visible) == 2);
	CHECK(visible == std::vector<uint32_t>{0, 2});
}

TEST_CASE("culling preserves the order it was given", "[graph][frustum]") {
	// Ordering is `scene::OrderForDrawing`'s job and this must not disturb it:
	// the two compose, cull first and order second over the survivors.
	const Frustum frustum = Looking();

	std::vector<DrawInstance> instances;
	for (int index = 0; index < 32; index++) {
		instances.push_back(At(Vector3{0.0f, 0.0f, -1.0f - static_cast<float>(index)}));
	}

	std::vector<uint32_t> visible;
	CHECK(Cull(instances, frustum, visible) == instances.size());

	for (uint32_t index = 0; index < visible.size(); index++) {
		CHECK(visible[index] == index);
	}
}

TEST_CASE("a parallel camera whitelist preserves entity order across chunks", "[graph][frustum]") {
	const RunningJobs jobs;
	const Frustum frustum = Looking();
	std::vector<DrawInstance> instances;
	instances.reserve(20'000);
	for (uint32_t index = 0; index < 20'000; index++) {
		const float x = (index % 3) == 0 ? 0.0f : 500.0f;
		instances.push_back(At(Vector3{x, 0.0f, -10.0f}));
	}

	std::vector<uint32_t> visible;
	CHECK(Cull(instances, frustum, visible) == 6'667);
	REQUIRE(visible.size() == 6'667);
	for (uint32_t kept = 0; kept < visible.size(); kept++) {
		CHECK(visible[kept] == kept * 3);
	}
}

TEST_CASE("culling an empty list is empty", "[graph][frustum]") {
	std::vector<uint32_t> visible;
	CHECK(Cull({}, Looking(), visible) == 0);
	CHECK(visible.empty());
}

TEST_CASE("a surface whose pane nothing can see is not visible", "[graph][cull]") {
	// **The half of "should this mirror redraw" that used not to be asked.** A
	// content signature says whether the image changed; it cannot say whether
	// anybody is looking, so a room of eight mirrors redrew all eight on every
	// frame anything in the world moved - including the ones behind the viewer.
	//
	// Two panes, one in front of a camera at the origin and one well behind it.
	DrawInstance ahead = At(Vector3{0.0f, 0.0f, -10.0f}, 2.0f);
	ahead.Surface = 0;

	DrawInstance behind = At(Vector3{0.0f, 0.0f, 40.0f}, 2.0f);
	behind.Surface = 1;

	const std::vector<DrawInstance> instances{ahead, behind};

	// Each pane's own camera. Neither matters for this case - what decides it
	// is where the *pane* is - and giving them matrices that see nothing is
	// what makes that claim rather than assumes it.
	engine::graph::SurfaceEye eyes[2];
	eyes[0].Index = 0;
	eyes[1].Index = 1;

	bool visible[2] = {true, true};
	const size_t seen = engine::graph::VisibleSurfaces(
		instances,
		LookingMatrix(),
		std::span<const engine::graph::SurfaceEye>(eyes, 2),
		std::span<bool>(visible, 2)
	);

	CHECK(seen == 1);
	CHECK(visible[0]);
	CHECK_FALSE(visible[1]);
}

TEST_CASE("a surface visible only inside another surface still counts", "[graph][cull]") {
	// **Two facing mirrors, and a portal seen through a portal.** A pane off the
	// screen but inside one that is on it has to keep refreshing, or the picture
	// in the mirror freezes while the mirror itself moves - which is a much
	// louder artefact than the redraw it saves.
	//
	// Pane A is in front of the viewer. Pane B is behind the viewer and so is
	// not on screen, but A's camera looks the other way and has B in it.
	DrawInstance ahead = At(Vector3{0.0f, 0.0f, -10.0f}, 2.0f);
	ahead.Surface = 0;

	DrawInstance behind = At(Vector3{0.0f, 0.0f, 40.0f}, 2.0f);
	behind.Surface = 1;

	const std::vector<DrawInstance> instances{ahead, behind};

	Camera looking;
	looking.NearPlane = 0.1f;
	looking.FarPlane = 200.0f;

	engine::graph::SurfaceEye eyes[2];
	eyes[0].Index = 0;

	// A's camera stands where A is and looks back down +Z, so pane B at z = 40
	// is squarely in front of it.
	eyes[0].ViewProjection =
		engine::scene::ResolveCamera(
			CFrame::LookAt(Vector3{0.0f, 0.0f, -10.0f}, Vector3{0.0f, 0.0f, 40.0f}), looking, 16.0f / 9.0f
		)
			.ViewProjection;
	eyes[1].Index = 1;

	bool visible[2] = {};
	const size_t seen = engine::graph::VisibleSurfaces(
		instances,
		LookingMatrix(),
		std::span<const engine::graph::SurfaceEye>(eyes, 2),
		std::span<bool>(visible, 2)
	);

	CHECK(seen == 2);
	CHECK(visible[0]);
	CHECK(visible[1]);
}

TEST_CASE("a surface two bounces deep counts when the pass will draw it", "[graph][cull]") {
	// **The bug a screenshot showed and no test did, and it was a mismatch
	// between two numbers rather than a fault in either.** The grant sweep
	// followed exactly one level of surface-seen-in-surface. `D00112` made the
	// surface pass run `Renderer::SetSurfaceBounces` times and resolve a chain
	// that deep *inside* the frame. So the pass drew level two and this marked it
	// invisible, and a mirror's deeper reflections were culled rather than late.
	//
	// **It showed as a camera angle**, which is what made it hard to place: a
	// pane directly on screen reveals what it sees for free, so the chain is only
	// two deep once the first pane has *left* the frustum. Turning far enough to
	// do that made reflections disappear with the geometry untouched.
	//
	// Three panes in a line. Only A is on screen. A's camera sees B, B's camera
	// sees C. C is therefore two grants away and needs two rounds.
	DrawInstance a = At(Vector3{0.0f, 0.0f, -10.0f}, 2.0f);
	a.Surface = 0;

	DrawInstance b = At(Vector3{0.0f, 0.0f, 40.0f}, 2.0f);
	b.Surface = 1;

	DrawInstance c = At(Vector3{0.0f, 0.0f, 90.0f}, 2.0f);
	c.Surface = 2;

	const std::vector<DrawInstance> instances{a, b, c};

	Camera looking;
	looking.NearPlane = 0.1f;
	looking.FarPlane = 200.0f;

	// **A's far plane stops short of C on purpose**, and the first version of
	// this case did not do that: with everything on one axis and a two-hundred
	// unit reach, A saw C directly and the chain was one deep rather than two.
	// The test passed at one round for a reason that had nothing to do with what
	// it was written to check.
	Camera nearSighted;
	nearSighted.NearPlane = 0.1f;
	nearSighted.FarPlane = 60.0f;

	engine::graph::SurfaceEye eyes[3];

	// A stands at its own pane and looks down +Z, so B at 40 is in front of it
	// and C at 90 is past where it can see.
	eyes[0].Index = 0;
	eyes[0].ViewProjection =
		engine::scene::ResolveCamera(
			CFrame::LookAt(Vector3{0.0f, 0.0f, -10.0f}, Vector3{0.0f, 0.0f, 40.0f}), nearSighted, 16.0f / 9.0f
		)
			.ViewProjection;

	// B looks further down +Z, so C is in front of it - and C is in front of
	// nothing else, which is what makes it exactly two grants away.
	eyes[1].Index = 1;
	eyes[1].ViewProjection =
		engine::scene::ResolveCamera(
			CFrame::LookAt(Vector3{0.0f, 0.0f, 40.0f}, Vector3{0.0f, 0.0f, 90.0f}), looking, 16.0f / 9.0f
		)
			.ViewProjection;

	// C looks away from all of them, so it grants nothing and cannot rescue
	// itself - without which this would pass for the wrong reason.
	eyes[2].Index = 2;
	eyes[2].ViewProjection =
		engine::scene::ResolveCamera(
			CFrame::LookAt(Vector3{0.0f, 0.0f, 90.0f}, Vector3{0.0f, 100.0f, 90.0f}), looking, 16.0f / 9.0f
		)
			.ViewProjection;

	SECTION("one round reaches one level, which is what it always did") {
		bool visible[3] = {};
		const size_t seen = engine::graph::VisibleSurfaces(
			instances,
			LookingMatrix(),
			std::span<const engine::graph::SurfaceEye>(eyes, 3),
			std::span<bool>(visible, 3),
			{},
			1
		);

		CHECK(seen == 2);
		CHECK(visible[0]);
		CHECK(visible[1]);
		INFO("one round cannot reach a pane two grants away, and must not pretend to");
		CHECK_FALSE(visible[2]);
	}

	SECTION("two rounds reach the level a two-bounce pass will draw") {
		bool visible[3] = {};
		const size_t seen = engine::graph::VisibleSurfaces(
			instances,
			LookingMatrix(),
			std::span<const engine::graph::SurfaceEye>(eyes, 3),
			std::span<bool>(visible, 3),
			{},
			2
		);

		CHECK(seen == 3);
		CHECK(visible[0]);
		CHECK(visible[1]);
		CHECK(visible[2]);
	}

	SECTION("asking for more rounds than the scene has depth changes nothing") {
		// The early exit's guarantee, stated rather than assumed: a round that
		// grants nothing stops the loop, so a caller may pass its bounce count
		// without knowing how deep the scene actually goes.
		bool visible[3] = {};
		const size_t seen = engine::graph::VisibleSurfaces(
			instances,
			LookingMatrix(),
			std::span<const engine::graph::SurfaceEye>(eyes, 3),
			std::span<bool>(visible, 3),
			{},
			64
		);

		CHECK(seen == 3);
	}
}

TEST_CASE("a surface with no pane in the draw list is not visible", "[graph][cull]") {
	// A slot nothing samples is a texture nothing reads, and rendering into it
	// is work with no consumer. `SurfaceSlotState::Ready` already knows how to
	// show a pane that has never been drawn - it falls back to its own tint - so
	// the honest answer costs nothing downstream.
	DrawInstance plain = At(Vector3{0.0f, 0.0f, -10.0f}, 2.0f);
	plain.Surface = -1;

	const std::vector<DrawInstance> instances{plain};

	engine::graph::SurfaceEye eyes[1];
	eyes[0].Index = 0;

	bool visible[1] = {true};
	CHECK(
		engine::graph::VisibleSurfaces(
			instances,
			LookingMatrix(),
			std::span<const engine::graph::SurfaceEye>(eyes, 1),
			std::span<bool>(visible, 1)
		) == 0
	);
	CHECK_FALSE(visible[0]);
}

TEST_CASE("a surface's rate cap drops frames rather than queueing them", "[graph][cull]") {
	// **A surface is a whole scene render and there is no reason it should keep
	// the screen's rate.** A room of mirrors at 165 hertz is a room of full
	// scene passes at 165 hertz, and a reflection is already a frame behind by
	// construction - so `scene::SurfaceCamera::FPS` bounds the staleness instead
	// of leaving it at whatever the display does.
	//
	// The rule lives in the renderer, where a device is needed to exercise it.
	// What can be checked here is the decision itself, which is arithmetic:
	// `render::DueToDraw` in `Renderer.cpp` is this, and the three ways of being
	// uncapped are what this case is for.
	const auto due = [](double drawn, float fps, double now) {
		if (!(fps > 0.0f) || drawn < 0.0 || !(now > drawn)) {
			return true;
		}
		return now - drawn >= 1.0 / static_cast<double>(fps);
	};

	// Never drawn: draws now, whatever the cap says. A pane walked up to must
	// not show its own tint for an interval before the picture arrives.
	CHECK(due(-1.0, 120.0f, 0.0));

	// Inside the interval, and past it.
	//
	// **Not measured at the boundary itself**, which is a coin flip and rightly
	// so: `(1 + 1/120) - 1` is not `1/120` in a double, and a cap that cared
	// about which side of that it landed on would be a cap with an opinion about
	// float rounding. What it promises is a bound, not a phase.
	CHECK_FALSE(due(1.0, 120.0f, 1.0 + 1.0 / 240.0));
	CHECK(due(1.0, 120.0f, 1.0 + 1.0 / 60.0));

	// **Three ways to be uncapped, all of them "draw".** Zero is the documented
	// way to ask for every frame; a negative rate is a script that computed
	// something silly and must not black the surface out; and a clock that has
	// not advanced is a host that never set one, where capping would freeze
	// every surface in the world after its first frame.
	CHECK(due(1.0, 0.0f, 1.0));
	CHECK(due(1.0, -30.0f, 1.0));
	CHECK(due(1.0, 120.0f, 1.0));
}

TEST_CASE("a surface's coverage is what its resolution is chosen from", "[graph][cull]") {
	// **A surface camera is fitted to its pane, so its texture maps one to one
	// onto the pane's screen footprint.** A pane covering half the screen wants
	// half the screen's pixels; handing it a fixed size whatever it covers is
	// what makes a portal go coarse as you walk up to it, because the texels are
	// all still there and simply spread over a much larger rectangle.
	DrawInstance small = At(Vector3{0.0f, 0.0f, -40.0f}, 1.0f);
	small.Surface = 0;

	DrawInstance close = At(Vector3{0.0f, 0.0f, -2.0f}, 1.0f);
	close.Surface = 1;

	const std::vector<DrawInstance> instances{small, close};

	engine::graph::SurfaceEye eyes[2];
	eyes[0].Index = 0;
	eyes[1].Index = 1;

	bool visible[2] = {};
	float coverage[2] = {};
	REQUIRE(
		engine::graph::VisibleSurfaces(
			instances,
			LookingMatrix(),
			std::span<const engine::graph::SurfaceEye>(eyes, 2),
			std::span<bool>(visible, 2),
			std::span<float>(coverage, 2)
		) == 2
	);

	// The far one is a sliver of the screen; the near one is most of it.
	CHECK(coverage[0] > 0.0f);
	CHECK(coverage[0] < 0.15f);
	CHECK(coverage[1] > coverage[0] * 4.0f);
	CHECK(coverage[1] <= 1.0f);

	// **A pane the camera is inside gives one rather than a number from a
	// negative divide.** A box straddling the camera's own plane has no bounded
	// projection, and "as much as you have" is the honest answer.
	DrawInstance around = At(Vector3::Zero, 5.0f);
	around.Surface = 0;

	const std::vector<DrawInstance> swallowed{around};
	engine::graph::SurfaceEye one[1];
	one[0].Index = 0;

	bool alsoVisible[1] = {};
	float alsoCoverage[1] = {};
	(void)engine::graph::VisibleSurfaces(
		swallowed,
		LookingMatrix(),
		std::span<const engine::graph::SurfaceEye>(one, 1),
		std::span<bool>(alsoVisible, 1),
		std::span<float>(alsoCoverage, 1)
	);

	CHECK(alsoCoverage[0] == 1.0f);
}

TEST_CASE("a portal pane is culled per level rather than once for the eye", "[graph][cull]") {
	// **The whole reason `VisiblePane` is not `VisibleSurfaces`.** A surface
	// camera is placed from the eye, so its pane's visibility is one answer for
	// the frame; a portal's sub-camera is derived from whichever camera the
	// recursion is at, and the same rectangle is on screen at one level and
	// behind the camera at the next.
	const Vector3 first{2.0f, 0.0f, 0.0f};
	const Vector3 second{0.0f, 2.0f, 0.0f};

	const glm::mat4 eye = LookingMatrix();

	// Ten metres down -Z, square in the middle of the view.
	CHECK(engine::graph::VisiblePane(eye, Vector3{0.0f, 0.0f, -10.0f}, first, second));

	// The same rectangle behind the camera, which is where a sub-camera that
	// stepped through it stands.
	CHECK_FALSE(engine::graph::VisiblePane(eye, Vector3{0.0f, 0.0f, 10.0f}, first, second));

	// Far off to the side, outside the field of view at that depth.
	CHECK_FALSE(engine::graph::VisiblePane(eye, Vector3{200.0f, 0.0f, -10.0f}, first, second));
}

TEST_CASE("a pane edge-on to the camera is still culled by what it reaches", "[graph][cull]") {
	// A rectangle in the plane of the view direction has no thickness on one
	// axis, and the box built from it is degenerate there. That is exact rather
	// than a case to guard: what matters is that the other two axes still bound
	// it, so a pane whose corners reach into the frustum is kept and one whose
	// corners do not is dropped.
	const glm::mat4 eye = LookingMatrix();

	// Lying in the XZ plane, stretching down the view axis. Zero thickness on Y.
	CHECK(
		engine::graph::VisiblePane(
			eye, Vector3{0.0f, 0.0f, -10.0f}, Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, 5.0f}
		)
	);

	// The same flat rectangle, moved a long way above the frustum.
	CHECK_FALSE(
		engine::graph::VisiblePane(
			eye, Vector3{0.0f, 400.0f, -10.0f}, Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, 5.0f}
		)
	);
}

TEST_CASE("the oblique clip drops what is behind its plane at any angle", "[graph][cull]") {
	// **`scene::ObliqueProjection` against the screen's own frustum**, which is
	// what the recursive portal pass skews and what `SurfaceProjection` no
	// longer holds a second copy of. The property that matters is the one
	// Lengyel's method is for: the near plane *becomes* the given plane, so a
	// point behind it leaves the clip volume however far off-axis it is.
	Camera lens;
	lens.NearPlane = 0.1f;
	lens.FarPlane = 100.0f;

	const CFrame at{Vector3::Zero};
	const glm::mat4 plain = engine::scene::ResolveCamera(at, lens, 16.0f / 9.0f).Projection;

	// A plane five metres down -Z, its normal pointing away from the camera.
	const Vector3 normal{0.0f, 0.0f, -1.0f};
	const float distance = normal.Dot(Vector3{0.0f, 0.0f, -5.0f});
	const glm::mat4 skewed = engine::scene::ObliqueProjection(plain, at, normal, distance);

	const auto depthOf = [](const glm::mat4 &projection, const Vector3 &point) {
		const glm::vec4 clip = projection * glm::vec4(point.X, point.Y, point.Z, 1.0f);
		return clip.z / clip.w;
	};

	// In front of the plane and inside the volume; behind it and outside, which
	// under `GLM_FORCE_DEPTH_ZERO_TO_ONE` means a negative depth.
	CHECK(depthOf(skewed, Vector3{0.0f, 0.0f, -6.0f}) > 0.0f);
	CHECK(depthOf(skewed, Vector3{0.0f, 0.0f, -4.0f}) < 0.0f);

	// Off to the side, where a near plane merely pushed out parallel to the
	// face would still have kept it. Two metres across at four metres deep is
	// well inside a sixteen-by-nine frustum.
	CHECK(depthOf(skewed, Vector3{2.0f, 1.0f, -4.0f}) < 0.0f);
	CHECK(depthOf(skewed, Vector3{2.0f, 1.0f, -6.0f}) > 0.0f);

	// A camera standing on the plane has no half to keep, so it gets its
	// frustum back rather than a matrix with no volume in it.
	const CFrame onIt{Vector3{0.0f, 0.0f, -5.0f}};
	CHECK(engine::scene::ObliqueProjection(plain, onIt, normal, distance) == plain);
}
