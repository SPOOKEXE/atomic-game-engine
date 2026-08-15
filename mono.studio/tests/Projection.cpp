// The viewport's arithmetic, which is the half of picking and the grid a test
// can reach.
//
// **`Projection.hpp` names two traps and this file exists to catch them.** Both
// are the same shape - an error small enough to look like something else:
//
//   - projecting the overlay with the *current* camera instead of the one that
//     made the displayed texture, which looks correct until you move, and
//   - mapping panel space onto the panel rather than onto the sub-rect the
//     renderer actually drew into, which is a constant few-pixel offset and
//     reads as "sometimes it selects the wrong thing".
//
// Neither shows up in a screenshot of a stationary camera, which is exactly why
// they belong here rather than in a look at the studio.
//
// The matrices below are built with `scene::ResolveCamera` - the same call the
// renderer makes - rather than hand-written, because a test against a
// hand-written projection proves the test's arithmetic and not the engine's.

#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <studio/Projection.hpp>

#include <cmath>
#include <glm/gtc/quaternion.hpp>

TEST_SUITE_ID("studio.projection")

using Catch::Matchers::WithinAbs;
using engine::core::CFrame;
using engine::core::Ray;
using engine::core::Vector3;
using engine::scene::Camera;
using engine::scene::CameraMatrices;
using studio::PanelProjection;

namespace {
	constexpr float PIXEL = 0.5f;
	constexpr float TIGHT = 1e-3f;

	// A panel showing a camera at `eye` looking down -Z, drawn into `size`
	// pixels at `origin` within the panel.
	//
	// The offset origin is the point: a projection tested only at (0,0) passes
	// whether or not it respects the image rect.
	PanelProjection Panel(
		Vector3 eye = Vector3{0.0f, 0.0f, 0.0f},
		glm::vec2 origin = glm::vec2(0.0f, 0.0f),
		glm::vec2 size = glm::vec2(800.0f, 400.0f)
	) {
		Camera camera;

		const CFrame frame(eye);
		const CameraMatrices matrices = engine::scene::ResolveCamera(frame, camera, size.x / size.y);

		PanelProjection projection;
		projection.Matrix = matrices.ViewProjection;
		projection.Eye = eye;
		projection.ImageMin = origin;
		projection.ImageSize = size;
		projection.Near = camera.NearPlane;
		return projection;
	}
}

TEST_CASE("a point straight ahead lands in the middle of the image", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 point{};
	REQUIRE(panel.WorldToPanel(Vector3{0.0f, 0.0f, -10.0f}, point));

	CHECK_THAT(point.x, WithinAbs(400.0f, PIXEL));
	CHECK_THAT(point.y, WithinAbs(200.0f, PIXEL));
}

TEST_CASE("the image rect is what panel space maps onto, not the panel", "[studio][projection]") {
	// **The trap.** The same camera, the same world point, drawn into a
	// sub-rect that starts 60 across and 25 down - the projected point has to
	// move by exactly that, and a mapping written against the panel does not
	// move at all.
	const PanelProjection panel = Panel(Vector3{}, glm::vec2(60.0f, 25.0f));

	glm::vec2 point{};
	REQUIRE(panel.WorldToPanel(Vector3{0.0f, 0.0f, -10.0f}, point));

	CHECK_THAT(point.x, WithinAbs(460.0f, PIXEL));
	CHECK_THAT(point.y, WithinAbs(225.0f, PIXEL));
}

TEST_CASE("up in the world is up the panel", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 low{};
	glm::vec2 high{};
	REQUIRE(panel.WorldToPanel(Vector3{0.0f, -1.0f, -10.0f}, low));
	REQUIRE(panel.WorldToPanel(Vector3{0.0f, 1.0f, -10.0f}, high));

	// A panel counts downwards from its top edge and NDC counts upwards from
	// the centre, so a sign error here is a world drawn upside down - which is
	// obvious in a screenshot and completely invisible in a projected point.
	CHECK(high.y < low.y);
	CHECK_THAT(high.x, WithinAbs(low.x, PIXEL));
}

TEST_CASE("right in the world is right along the panel", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 left{};
	glm::vec2 right{};
	REQUIRE(panel.WorldToPanel(Vector3{-1.0f, 0.0f, -10.0f}, left));
	REQUIRE(panel.WorldToPanel(Vector3{1.0f, 0.0f, -10.0f}, right));

	CHECK(right.x > left.x);
}

TEST_CASE("a point behind the camera is refused rather than mirrored", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 point{};

	// Behind, and level with the eye. Both divide by a non-positive `w`, and
	// the mirrored ghost that produces is a grid line drawn across the sky.
	CHECK_FALSE(panel.WorldToPanel(Vector3{0.0f, 0.0f, 10.0f}, point));
	CHECK_FALSE(panel.WorldToPanel(Vector3{0.0f, 0.0f, 0.0f}, point));
}

TEST_CASE("a ray through the middle of the image points where the camera looks", "[studio][projection]") {
	const PanelProjection panel = Panel();
	const Ray ray = panel.PanelToRay(glm::vec2(400.0f, 200.0f));

	CHECK_THAT(ray.Origin.X, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(ray.Origin.Y, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(ray.Origin.Z, WithinAbs(0.0f, TIGHT));

	CHECK_THAT(ray.Direction.X, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(ray.Direction.Y, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(ray.Direction.Z, WithinAbs(-1.0f, TIGHT));
}

TEST_CASE("a ray starts at the eye wherever the camera is", "[studio][projection]") {
	const Vector3 eye{12.0f, 3.0f, -7.0f};
	const PanelProjection panel = Panel(eye);

	const Ray ray = panel.PanelToRay(glm::vec2(120.0f, 310.0f));

	CHECK_THAT(ray.Origin.X, WithinAbs(eye.X, TIGHT));
	CHECK_THAT(ray.Origin.Y, WithinAbs(eye.Y, TIGHT));
	CHECK_THAT(ray.Origin.Z, WithinAbs(eye.Z, TIGHT));
}

TEST_CASE("a ray has unit length, so a hit distance is a distance", "[studio][projection]") {
	const PanelProjection panel = Panel();

	// The corners, where a direction taken from a single near-plane point
	// rather than from two would be furthest wrong.
	for (const glm::vec2 at :
		 {glm::vec2(0.0f, 0.0f), glm::vec2(800.0f, 0.0f), glm::vec2(0.0f, 400.0f),
		  glm::vec2(800.0f, 400.0f), glm::vec2(400.0f, 200.0f)}) {
		const Ray ray = panel.PanelToRay(at);
		CHECK_THAT(ray.Direction.Magnitude(), WithinAbs(1.0f, TIGHT));
	}
}

TEST_CASE("projecting a point and raying its pixel finds that point again", "[studio][projection]") {
	// The round trip is the property picking actually depends on: click where
	// something is drawn, and the ray has to pass through it. A sign error, an
	// aspect-ratio error or an image-rect error all break this and each of them
	// individually looks plausible.
	const PanelProjection panel = Panel(Vector3{2.0f, 1.0f, 4.0f}, glm::vec2(17.0f, 9.0f));

	for (const Vector3 target :
		 {Vector3{2.0f, 1.0f, -6.0f}, Vector3{5.0f, 3.0f, -2.0f}, Vector3{-4.0f, -2.0f, -11.0f}}) {
		glm::vec2 point{};
		REQUIRE(panel.WorldToPanel(target, point));

		const Ray ray = panel.PanelToRay(point);

		// The target lies along the ray: the vector from the eye to it, made
		// unit length, is the ray's direction.
		const Vector3 toTarget = (target - ray.Origin).Unit();
		CHECK_THAT(ray.Direction.X, WithinAbs(toTarget.X, TIGHT));
		CHECK_THAT(ray.Direction.Y, WithinAbs(toTarget.Y, TIGHT));
		CHECK_THAT(ray.Direction.Z, WithinAbs(toTarget.Z, TIGHT));
	}
}

TEST_CASE("the aspect ratio is honoured rather than assumed square", "[studio][projection]") {
	// A wide panel and a tall one, each offset by the same world angle. If the
	// aspect ratio were dropped, the two would agree - and a world stretched
	// sideways is the single most common way to get this wrong.
	const PanelProjection wide = Panel(Vector3{}, glm::vec2(0.0f, 0.0f), glm::vec2(1000.0f, 250.0f));
	const PanelProjection tall = Panel(Vector3{}, glm::vec2(0.0f, 0.0f), glm::vec2(250.0f, 1000.0f));

	glm::vec2 onWide{};
	glm::vec2 onTall{};
	REQUIRE(wide.WorldToPanel(Vector3{1.0f, 0.0f, -10.0f}, onWide));
	REQUIRE(tall.WorldToPanel(Vector3{1.0f, 0.0f, -10.0f}, onTall));

	// Measured as a fraction of each panel's width, the wide one spreads the
	// same angle over less of its width than the tall one does.
	const float wideFraction = (onWide.x - 500.0f) / 1000.0f;
	const float tallFraction = (onTall.x - 125.0f) / 250.0f;

	CHECK(wideFraction < tallFraction);
}

TEST_CASE("a segment wholly in front projects like its endpoints", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 a{};
	glm::vec2 b{};
	REQUIRE(panel.ProjectSegment(Vector3{-2.0f, 0.0f, -10.0f}, Vector3{2.0f, 0.0f, -10.0f}, a, b));

	glm::vec2 directA{};
	glm::vec2 directB{};
	REQUIRE(panel.WorldToPanel(Vector3{-2.0f, 0.0f, -10.0f}, directA));
	REQUIRE(panel.WorldToPanel(Vector3{2.0f, 0.0f, -10.0f}, directB));

	CHECK_THAT(a.x, WithinAbs(directA.x, TIGHT));
	CHECK_THAT(b.x, WithinAbs(directB.x, TIGHT));
}

TEST_CASE("a segment wholly behind the camera is dropped", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 a{};
	glm::vec2 b{};
	CHECK_FALSE(panel.ProjectSegment(Vector3{-2.0f, 0.0f, 10.0f}, Vector3{2.0f, 0.0f, 10.0f}, a, b));
}

TEST_CASE("a segment crossing behind the camera is clipped, not dropped", "[studio][projection]") {
	// **The case the grid is made of.** A line running under the viewer starts
	// in front and ends behind; dropping it leaves a hole exactly where you are
	// standing, which is most of the screen. Both halves are wrong in a way a
	// stationary screenshot does not show.
	const PanelProjection panel = Panel();

	glm::vec2 a{};
	glm::vec2 b{};
	REQUIRE(panel.ProjectSegment(Vector3{0.0f, -1.0f, -10.0f}, Vector3{0.0f, -1.0f, 10.0f}, a, b));

	// The visible end keeps its place; the clipped end lands at the eye plane
	// rather than somewhere off in the millions.
	glm::vec2 front{};
	REQUIRE(panel.WorldToPanel(Vector3{0.0f, -1.0f, -10.0f}, front));

	CHECK_THAT(a.x, WithinAbs(front.x, PIXEL));
	CHECK_THAT(a.y, WithinAbs(front.y, PIXEL));

	// Finite, and within a sane multiple of the panel - the mirrored-ghost bug
	// produces coordinates in the millions and imgui rasterises them as a
	// stripe across the whole viewport.
	CHECK(std::isfinite(b.x));
	CHECK(std::isfinite(b.y));
	CHECK(std::abs(b.y) < 100000.0f);
}

TEST_CASE("clipping is symmetric in the order the endpoints are given", "[studio][projection]") {
	const PanelProjection panel = Panel();

	glm::vec2 a{};
	glm::vec2 b{};
	glm::vec2 ra{};
	glm::vec2 rb{};

	const Vector3 front{1.0f, -1.0f, -8.0f};
	const Vector3 behind{1.0f, -1.0f, 8.0f};

	REQUIRE(panel.ProjectSegment(front, behind, a, b));
	REQUIRE(panel.ProjectSegment(behind, front, ra, rb));

	// **Sub-pixel rather than `TIGHT`.** A clipped endpoint near the near plane
	// lands thousands of pixels outside the panel, and at that magnitude a
	// float carries about a hundredth of a pixel of precision - so interpolating
	// from either end agrees to far less than a pixel and not to 1e-3 absolute.
	// Asserting the tighter bound would be asserting that floats are exact.
	CHECK_THAT(a.x, WithinAbs(rb.x, PIXEL));
	CHECK_THAT(a.y, WithinAbs(rb.y, PIXEL));
	CHECK_THAT(b.x, WithinAbs(ra.x, PIXEL));
	CHECK_THAT(b.y, WithinAbs(ra.y, PIXEL));
}

TEST_CASE("a panel with no area refuses rather than dividing by it", "[studio][projection]") {
	PanelProjection panel = Panel();
	panel.ImageSize = glm::vec2(0.0f, 0.0f);

	// A window reports zero height while it is minimised, which
	// `scene::ResolveCamera` already documents as an ordinary frame.
	CHECK_FALSE(panel.IsValid());

	glm::vec2 point{};
	CHECK_FALSE(panel.WorldToPanel(Vector3{0.0f, 0.0f, -10.0f}, point));
	CHECK_FALSE(panel.ContainsPanel(glm::vec2(0.0f, 0.0f)));
}

TEST_CASE("a point beside the drawn world is not over it", "[studio][projection]") {
	const PanelProjection panel = Panel(Vector3{}, glm::vec2(60.0f, 25.0f));

	CHECK(panel.ContainsPanel(glm::vec2(60.0f, 25.0f)));
	CHECK(panel.ContainsPanel(glm::vec2(460.0f, 225.0f)));
	CHECK(panel.ContainsPanel(glm::vec2(860.0f, 425.0f)));

	// Inside the panel, outside the rect the world was drawn into - which is
	// the region a click must not be treated as pointing at the world.
	CHECK_FALSE(panel.ContainsPanel(glm::vec2(59.0f, 200.0f)));
	CHECK_FALSE(panel.ContainsPanel(glm::vec2(861.0f, 200.0f)));
	CHECK_FALSE(panel.ContainsPanel(glm::vec2(400.0f, 426.0f)));
}

TEST_CASE("dragging an axis reads the point on it nearest the cursor", "[studio][projection]") {
	// A ray straight down -Z from the origin, and the world X axis passing
	// three metres in front of it. The nearest point on that axis to the ray is
	// directly above the ray, which is x = 0.
	const Ray ray(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	float along = 123.0f;
	REQUIRE(studio::ClosestPointOnAxis(
		Vector3{0.0f, 0.0f, -3.0f}, Vector3{1.0f, 0.0f, 0.0f}, ray, along
	));

	CHECK_THAT(along, WithinAbs(0.0f, TIGHT));
}

TEST_CASE("an offset cursor reads an offset distance along the axis", "[studio][projection]") {
	// Aimed two metres to the right of straight ahead: the nearest point on the
	// X axis is two metres along it. This is the number a drag subtracts its
	// starting value from, so a sign error here drags the wrong way.
	const Ray ray(Vector3{2.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	float along = 0.0f;
	REQUIRE(studio::ClosestPointOnAxis(
		Vector3{0.0f, 0.0f, -3.0f}, Vector3{1.0f, 0.0f, 0.0f}, ray, along
	));

	CHECK_THAT(along, WithinAbs(2.0f, TIGHT));
}

TEST_CASE("an axis pointing at the eye is refused rather than flung", "[studio][projection]") {
	// The axis is the ray. Every cursor position maps to a wildly different
	// distance, and a gizmo that kept dragging here throws the selection off
	// into the distance - which is the failure this refusal exists to stop.
	const Ray ray(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	float along = 0.0f;
	CHECK_FALSE(studio::ClosestPointOnAxis(
		Vector3{0.0f, 0.0f, -3.0f}, Vector3{0.0f, 0.0f, -1.0f}, ray, along
	));

	// And the near-parallel case, which is the one somebody actually reaches by
	// turning the camera rather than by aiming exactly.
	const Vector3 nearlyAligned = Vector3{0.004f, 0.0f, -1.0f}.Unit();
	CHECK_FALSE(studio::ClosestPointOnAxis(
		Vector3{0.0f, 0.0f, -3.0f}, nearlyAligned, ray, along
	));
}

TEST_CASE("a ray meets a plane in front of it", "[studio][projection]") {
	// Straight down -Z at a plane five metres ahead facing back at the camera.
	const Ray ray(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	Vector3 point;
	REQUIRE(studio::IntersectRayPlane(
		Vector3{0.0f, 0.0f, -5.0f}, Vector3{0.0f, 0.0f, 1.0f}, ray, point
	));

	CHECK_THAT(point.X, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(point.Y, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(point.Z, WithinAbs(-5.0f, TIGHT));
}

TEST_CASE("a ray meets a ground plane it is angled at", "[studio][projection]") {
	// From two metres up, angled down and forwards at 45 degrees: it lands two
	// metres ahead on the ground.
	const Ray ray(Vector3{0.0f, 2.0f, 0.0f}, Vector3{0.0f, -1.0f, -1.0f}.Unit());

	Vector3 point;
	REQUIRE(studio::IntersectRayPlane(
		Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, ray, point
	));

	CHECK_THAT(point.Y, WithinAbs(0.0f, TIGHT));
	CHECK_THAT(point.Z, WithinAbs(-2.0f, TIGHT));
}

TEST_CASE("a ray along a plane is refused rather than crossing far away", "[studio][projection]") {
	// Edge-on. The crossing is at infinity and swings for a pixel of movement,
	// which is a rotate gizmo that spins the selection when nudged.
	const Ray ray(Vector3{0.0f, 1.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	Vector3 point;
	CHECK_FALSE(studio::IntersectRayPlane(
		Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 1.0f, 0.0f}, ray, point
	));
}

TEST_CASE("a plane behind the eye is not what the cursor is pointing at", "[studio][projection]") {
	const Ray ray(Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, -1.0f});

	Vector3 point;
	CHECK_FALSE(studio::IntersectRayPlane(
		Vector3{0.0f, 0.0f, 5.0f}, Vector3{0.0f, 0.0f, 1.0f}, ray, point
	));
}

TEST_CASE("a box reaches to its corner along a slope's normal", "[studio][projection]") {
	// **What "resting on it" needs.** Placing a part by half its height works
	// only while it is axis-aligned; a turned box touches a surface at a corner,
	// and the distance to that corner is what stops it sinking in.
	const Vector3 half{1.0f, 2.0f, 3.0f};

	// Unturned, the reach along each axis is that axis's half-extent.
	const CFrame flat;
	CHECK_THAT(studio::SupportAlong(flat, half, Vector3::YAxis), WithinAbs(2.0f, 0.0001f));
	CHECK_THAT(studio::SupportAlong(flat, half, Vector3::XAxis), WithinAbs(1.0f, 0.0001f));

	// Negative is the same distance: a box reaches as far down as it does up,
	// and a signed answer would place a part underneath a floor half the time.
	CHECK_THAT(studio::SupportAlong(flat, half, Vector3::YAxis * -1.0f), WithinAbs(2.0f, 0.0001f));

	// Turned a quarter about Z, the world's up is the box's own X.
	const CFrame turned(Vector3{}, glm::angleAxis(1.5707963f, glm::vec3(0.0f, 0.0f, 1.0f)));
	CHECK_THAT(studio::SupportAlong(turned, half, Vector3::YAxis), WithinAbs(1.0f, 0.0001f));

	// And at forty-five degrees it is the corner, which is the case that made
	// this a function rather than a half-extent lookup.
	const CFrame tilted(Vector3{}, glm::angleAxis(0.7853982f, glm::vec3(0.0f, 0.0f, 1.0f)));
	CHECK_THAT(
		studio::SupportAlong(tilted, half, Vector3::YAxis), WithinAbs((1.0f + 2.0f) * 0.7071068f, 0.0005f)
	);
}

TEST_CASE("aligning to a surface keeps the facing it can", "[studio][projection]") {
	// **The handedness is the whole risk here.** `LookVector` is `-Z`, so the
	// basis's third column is the back - building it the other way round mirrors
	// every part that is dropped, which reads as the model being wrong rather
	// than the maths, and is invisible on anything symmetrical.
	const CFrame flat;

	// A surface whose normal is already up leaves the rotation alone.
	const CFrame same(Vector3{}, studio::AlignedTo(flat, Vector3::YAxis));
	CHECK_THAT(same.UpVector().Y, WithinAbs(1.0f, 0.0005f));
	CHECK_THAT(same.LookVector().Z, WithinAbs(-1.0f, 0.0005f));

	// A wall facing +X: up becomes +X, and the old facing survives as far as it
	// can - it was already perpendicular to the new up, so it survives intact.
	const CFrame wall(Vector3{}, studio::AlignedTo(flat, Vector3::XAxis));
	CHECK_THAT(wall.UpVector().X, WithinAbs(1.0f, 0.0005f));
	CHECK_THAT(wall.LookVector().Z, WithinAbs(-1.0f, 0.0005f));

	// Right-handed throughout: right cross up is the back, which is what a
	// mirrored basis would get wrong and nothing else here would notice.
	const Vector3 back = wall.VectorToWorldSpace(Vector3::ZAxis);
	const Vector3 crossed = wall.RightVector().Cross(wall.UpVector());
	CHECK_THAT(crossed.X, WithinAbs(back.X, 0.0005f));
	CHECK_THAT(crossed.Y, WithinAbs(back.Y, 0.0005f));
	CHECK_THAT(crossed.Z, WithinAbs(back.Z, 0.0005f));

	// A part looking straight down at a floor has a facing that says nothing
	// about the floor's plane, so its right-hand side is used instead - and the
	// answer is still a rotation rather than a degenerate basis.
	const CFrame looking(Vector3{}, glm::angleAxis(1.5707963f, glm::vec3(1.0f, 0.0f, 0.0f)));
	const CFrame onFloor(Vector3{}, studio::AlignedTo(looking, Vector3::YAxis));
	CHECK_THAT(onFloor.UpVector().Y, WithinAbs(1.0f, 0.0005f));
	CHECK_THAT(onFloor.LookVector().Magnitude(), WithinAbs(1.0f, 0.0005f));
}
