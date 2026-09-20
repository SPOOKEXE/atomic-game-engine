// The rectangles and quads the passes work in.
//
// **`render/AGENTS.md` promised this file for four versions and it did not
// exist**, which `docs/ARCH_REVIEW.md` B recorded. The two things it checks are
// the two the module had written out by hand and never asserted: the shadow
// atlas quadrant a portal beam draws into, which was three expressions of one
// rectangle, and the quad a spatial canvas occupies, whose normal is not the
// cross product of its own axes.
//
// Headless. A quadrant is arithmetic and a quad is four corners; neither needs
// a device, which is the whole reason they were moved somewhere a suite can
// reach them.

#include "Primitives.hpp"

#include "PortalBeamSelection.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

TEST_SUITE_ID("engine.render.primitives")

using Catch::Approx;
using engine::render::AtlasQuadrant;
using engine::render::BeamQuadrant;
using engine::render::BillboardQuad;
using engine::render::CanvasFacesViewer;
using engine::render::CanvasPixelsPerStud;
using engine::render::PortalBeamInfluenceDistanceSquared;
using engine::render::PortalBeamMappedBounds;
using engine::render::PortalBeamProjector;
using engine::render::PortalBeamRank;
using engine::render::PortalBeamRanksBefore;
using engine::render::SpatialQuad;
using Vector2 = engine::core::Vector2;
using Vector3 = engine::core::Vector3;

namespace {
	constexpr uint32_t TILE = 1024;
	constexpr uint32_t ATLAS_WIDTH = TILE * 2;
	constexpr uint32_t ATLAS_HEIGHT = TILE * 3;

	// Whether two texel rectangles share any area.
	bool Overlaps(const AtlasQuadrant &left, const AtlasQuadrant &right) {
		return left.X < right.X + right.Width && right.X < left.X + left.Width &&
			   left.Y < right.Y + right.Height && right.Y < left.Y + left.Height;
	}

	Vector3 Cross(const Vector3 &left, const Vector3 &right) {
		return Vector3{
			left.Y * right.Z - left.Z * right.Y,
			left.Z * right.X - left.X * right.Z,
			left.X * right.Y - left.Y * right.X,
		};
	}
}

// --- the beam atlas ------------------------------------------------------

TEST_CASE("the six beam tiles tile the atlas and do not overlap", "[render][primitives]") {
	float area = 0.0f;
	for (uint32_t index = 0; index < 6; index++) {
		const AtlasQuadrant quadrant = BeamQuadrant(index, TILE);

		REQUIRE(quadrant.Width == Approx(static_cast<float>(TILE)));
		REQUIRE(quadrant.Height == Approx(static_cast<float>(TILE)));
		REQUIRE(quadrant.X >= 0.0f);
		REQUIRE(quadrant.Y >= 0.0f);
		REQUIRE(quadrant.X + quadrant.Width <= static_cast<float>(ATLAS_WIDTH));
		REQUIRE(quadrant.Y + quadrant.Height <= static_cast<float>(ATLAS_HEIGHT));
		area += quadrant.Width * quadrant.Height;

		for (uint32_t other = 0; other < index; other++) {
			REQUIRE_FALSE(Overlaps(quadrant, BeamQuadrant(other, TILE)));
		}
	}

	// Six tiles fill the whole atlas: nothing is left unwritten, and
	// a quadrant nobody wrote stays at the far plane, which the beam lookup
	// reads as lit.
	REQUIRE(area == Approx(static_cast<float>(ATLAS_WIDTH) * static_cast<float>(ATLAS_HEIGHT)));
}

// **The check the three hand-written copies could not make.** The viewport says
// where a beam draws and the window says where the shader looks; they used to
// be separate expressions of `index % 2` and `index / 2`, and a beam that drew
// into one quadrant while sampling another shadows through the wrong doorway.
TEST_CASE("a beam quadrant's lookup window is its own viewport", "[render][primitives]") {
	for (uint32_t index = 0; index < 6; index++) {
		const AtlasQuadrant quadrant = BeamQuadrant(index, TILE);

		REQUIRE(quadrant.Window.x == Approx(quadrant.Width / static_cast<float>(ATLAS_WIDTH)));
		REQUIRE(quadrant.Window.y == Approx(quadrant.Height / static_cast<float>(ATLAS_HEIGHT)));
		REQUIRE(quadrant.Window.z == Approx(quadrant.X / static_cast<float>(ATLAS_WIDTH)));
		REQUIRE(quadrant.Window.w == Approx(quadrant.Y / static_cast<float>(ATLAS_HEIGHT)));
	}
}

TEST_CASE("beam tiles go in reading order", "[render][primitives]") {
	const auto tile = static_cast<float>(TILE);

	for (uint32_t index = 0; index < 6; index++) {
		REQUIRE(BeamQuadrant(index, TILE).X == Approx(static_cast<float>(index % 2u) * tile));
		REQUIRE(BeamQuadrant(index, TILE).Y == Approx(static_cast<float>(index / 2u) * tile));
	}
}

// A seventh beam is refused by the caller, which logs the ones it dropped. If one
// arrives anyway it takes a quadrant rather than a viewport off the edge of the
// texture, which is a driver validation error on some backends and a silent
// read of unmapped memory on others.
TEST_CASE("a beam index past the sixth stays inside the atlas", "[render][primitives]") {
	const AtlasQuadrant seventh = BeamQuadrant(6, TILE);
	const AtlasQuadrant first = BeamQuadrant(0, TILE);

	REQUIRE(seventh.X == Approx(first.X));
	REQUIRE(seventh.Y == Approx(first.Y));
}

TEST_CASE("a portal beam keeps an offscreen doorway when it reaches visible ground", "[render][primitives]") {
	std::array<engine::scene::DrawInstance, 2> rows;
	rows[0].Frame.Position = {0.0f, 0.0f, 0.5f};
	rows[0].HalfExtent = {0.2f, 0.2f, 0.2f};
	rows[1].Frame.Position = {8.0f, 0.0f, 0.5f};
	rows[1].HalfExtent = {0.2f, 0.2f, 0.2f};
	const std::array<uint32_t, 1> visible{0};
	const PortalBeamProjector projector{
		.Back = {},
		.PlaneNormal = {0.0f, 0.0f, 1.0f},
		.PlaneOffset = -0.5f,
		.Light = glm::mat4{1.0f},
	};

	const float influence =
		PortalBeamInfluenceDistanceSquared(projector, rows, visible, Vector3{0.0f, 0.0f, 2.0f});

	// The pane itself may be offscreen. Only the receiver set decides whether
	// this beam earns a bounded atlas slot.
	REQUIRE(std::isfinite(influence));
	REQUIRE(influence == Approx(1.69f));
}

TEST_CASE("portal beam receiver bounds rotate once through a seam", "[render][primitives]") {
	engine::scene::DrawInstance receiver;
	receiver.Frame = engine::core::CFrame::Angles(0.0f, 0.5f, 0.0f);
	receiver.HalfExtent = {3.0f, 1.0f, 0.5f};
	PortalBeamProjector projector;
	projector.Back.Frame = engine::core::CFrame::Angles(0.0f, 0.7f, 0.0f);
	projector.Back.Scale = 1.5f;

	const engine::core::AABB mapped = PortalBeamMappedBounds(projector, receiver);
	const engine::core::AABB expected = engine::core::OrientedBoxBounds(
		projector.Back.Place(receiver.Frame), receiver.HalfExtent * projector.Back.Scale
	);

	REQUIRE(mapped.Minimum.X == Approx(expected.Minimum.X));
	REQUIRE(mapped.Minimum.Y == Approx(expected.Minimum.Y));
	REQUIRE(mapped.Minimum.Z == Approx(expected.Minimum.Z));
	REQUIRE(mapped.Maximum.X == Approx(expected.Maximum.X));
	REQUIRE(mapped.Maximum.Y == Approx(expected.Maximum.Y));
	REQUIRE(mapped.Maximum.Z == Approx(expected.Maximum.Z));
}

TEST_CASE("seven portal beams retain the offscreen aperture that reaches the view", "[render][primitives]") {
	std::array<engine::scene::DrawInstance, 1> rows;
	rows[0].Frame.Position = {0.0f, 0.0f, 0.5f};
	rows[0].HalfExtent = {0.1f, 0.1f, 0.1f};
	const std::array<uint32_t, 1> visible{0};
	std::array<PortalBeamRank, 7> ranks;
	for (uint32_t slot = 0; slot < ranks.size(); slot++) {
		PortalBeamProjector projector;
		projector.PlaneNormal = {0.0f, 0.0f, 1.0f};
		projector.PlaneOffset = -0.5f;
		// The first six apertures' beams lie outside the receiver. The seventh
		// can be outside the eye yet its beam lands on visible ground.
		projector.Light = glm::translate(glm::mat4{1.0f}, glm::vec3{slot < 6 ? 4.0f : 0.0f, 0.0f, 0.0f});
		ranks[slot] = {
			slot, PortalBeamInfluenceDistanceSquared(projector, rows, visible, {0.0f, 0.0f, 2.0f})
		};
	}

	std::sort(ranks.begin(), ranks.end(), PortalBeamRanksBefore);
	REQUIRE(ranks[0].Slot == 6);
	REQUIRE(std::isfinite(ranks[0].InfluenceDistance));
	for (size_t index = 1; index < ranks.size(); index++) {
		REQUIRE_FALSE(std::isfinite(ranks[index].InfluenceDistance));
	}
}

// --- the spatial canvas quad ---------------------------------------------

TEST_CASE("a canvas facing the eye is drawn and one facing away is not", "[render][primitives]") {
	const Vector3 normal{0.0f, 0.0f, 1.0f};

	REQUIRE(CanvasFacesViewer(normal, Vector3{0.0f, 0.0f, 4.0f}));
	REQUIRE_FALSE(CanvasFacesViewer(normal, Vector3{0.0f, 0.0f, -4.0f}));

	// Exactly edge-on is behind. A canvas with no area on screen is not worth a
	// draw, and answering "in front" here would put a zero-width quad through
	// the whole interface pipeline once a frame.
	REQUIRE_FALSE(CanvasFacesViewer(normal, Vector3{5.0f, 5.0f, 0.0f}));
}

TEST_CASE("a billboard quad is centred on its anchor", "[render][primitives]") {
	const Vector3 anchor{3.0f, 4.0f, 5.0f};
	const SpatialQuad quad = BillboardQuad(
		anchor,
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{0.0f, 0.0f, 10.0f},
		Vector2{2.0f, 6.0f},
		Vector2{0.0f, 0.0f},
		100.0f,
		Vector3{0.0f, 0.0f, 1.0f}
	);

	const std::array<Vector3, 4> corners = quad.Corners();
	Vector3 middle{0.0f, 0.0f, 0.0f};
	for (const Vector3 &corner : corners) {
		middle = middle + corner;
	}
	middle = middle / 4.0f;

	REQUIRE(middle.X == Approx(anchor.X));
	REQUIRE(middle.Y == Approx(anchor.Y));
	REQUIRE(middle.Z == Approx(anchor.Z));

	// Two studs across and six down, which is what was asked for.
	REQUIRE((corners[1] - corners[0]).Magnitude() == Approx(2.0f));
	REQUIRE((corners[2] - corners[0]).Magnitude() == Approx(6.0f));

	// The top edge is above the bottom one. `AxisY` runs *down* the image
	// because a canvas is laid out in interface pixels, and getting that
	// backwards presents as every billboard in the world being upside down.
	REQUIRE(corners[0].Y > corners[2].Y);
}

// **The fact `SpatialQuad::Normal` exists for.** A canvas is laid out top-down,
// so `AxisX x AxisY` points away from the viewer - and a caller that derived
// the normal from the axes would light every billboard from behind and cull the
// ones it should draw.
TEST_CASE("a billboard's normal is not the cross product of its axes", "[render][primitives]") {
	const SpatialQuad quad = BillboardQuad(
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{0.0f, 0.0f, 8.0f},
		Vector2{2.0f, 2.0f},
		Vector2{0.0f, 0.0f},
		100.0f,
		Vector3{0.0f, 0.0f, 1.0f}
	);

	REQUIRE(quad.Normal.Z == Approx(1.0f));
	REQUIRE(Cross(quad.AxisX, quad.AxisY).Z < 0.0f);
	REQUIRE(quad.Normal.Dot(Cross(quad.AxisX, quad.AxisY)) < 0.0f);
}

TEST_CASE("a billboard's quad is flat", "[render][primitives]") {
	const SpatialQuad quad = BillboardQuad(
		Vector3{1.0f, 2.0f, 3.0f},
		Vector3{0.0f, 0.0f, -1.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{-4.0f, 0.0f, 0.0f},
		Vector2{3.0f, 1.5f},
		Vector2{0.0f, 0.0f},
		100.0f,
		Vector3{1.0f, 0.0f, 0.0f}
	);

	const std::array<Vector3, 4> corners = quad.Corners();
	const Vector3 plane = Cross(corners[1] - corners[0], corners[2] - corners[0]).Unit();
	for (const Vector3 &corner : corners) {
		REQUIRE(std::abs(plane.Dot(corner - corners[0])) == Approx(0.0f).margin(1e-5));
	}
}

// The pixel half of a billboard's size is a `UDim2` offset, so it is divided by
// however many canvas pixels a stud covers. A studs-only billboard must not
// move when that measurement changes, which is what says the two halves are
// genuinely independent.
TEST_CASE("only the pixel half of a billboard's size scales with the projection", "[render][primitives]") {
	const auto sized = [](float perStud) {
		return BillboardQuad(
			Vector3{0.0f, 0.0f, 0.0f},
			Vector3{1.0f, 0.0f, 0.0f},
			Vector3{0.0f, 1.0f, 0.0f},
			Vector3{0.0f, 0.0f, 6.0f},
			Vector2{4.0f, 4.0f},
			Vector2{200.0f, 0.0f},
			perStud,
			Vector3{0.0f, 0.0f, 1.0f}
		);
	};

	REQUIRE(sized(100.0f).AxisX.Magnitude() == Approx(6.0f));
	REQUIRE(sized(200.0f).AxisX.Magnitude() == Approx(5.0f));

	// The stud half is untouched by either.
	REQUIRE(sized(100.0f).AxisY.Magnitude() == Approx(4.0f));
	REQUIRE(sized(200.0f).AxisY.Magnitude() == Approx(4.0f));
}

// **A pixels-per-stud of zero is the caller's ordinary state, not a mistake.**
// A billboard exactly at the eye projects to nothing, and dividing by what that
// measures would put an infinity into a vertex buffer.
TEST_CASE("a billboard survives a projection that measures nothing", "[render][primitives]") {
	const SpatialQuad quad = BillboardQuad(
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
		Vector2{1.0f, 1.0f},
		Vector2{10.0f, 10.0f},
		0.0f,
		Vector3{0.0f, 0.0f, 1.0f}
	);

	REQUIRE(std::isfinite(quad.AxisX.Magnitude()));
	REQUIRE(std::isfinite(quad.AxisY.Magnitude()));
	REQUIRE(std::isfinite(quad.Origin.X));
}

// With the eye exactly on the anchor there is no direction to face, so the
// caller's fallback is used rather than a zero normal - which `CanvasFacesViewer`
// would read as "behind" and the shading would read as unlit.
TEST_CASE("a billboard at the eye faces the way the caller said", "[render][primitives]") {
	const SpatialQuad quad = BillboardQuad(
		Vector3{7.0f, 7.0f, 7.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{0.0f, 0.0f, 0.0f},
		Vector2{1.0f, 1.0f},
		Vector2{0.0f, 0.0f},
		100.0f,
		Vector3{0.0f, -1.0f, 0.0f}
	);

	REQUIRE(quad.Normal.Y == Approx(-1.0f));
}

TEST_CASE("pixels per stud grows as a canvas comes closer", "[render][primitives]") {
	const glm::mat4 projection = glm::perspective(1.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	const glm::mat4 lookAt =
		glm::lookAt(glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
	const glm::mat4 viewProjection = projection * lookAt;
	const Vector3 up{0.0f, 1.0f, 0.0f};

	const float near = CanvasPixelsPerStud(viewProjection, Vector3{0.0f, 0.0f, -5.0f}, up, 1080.0f);
	const float far = CanvasPixelsPerStud(viewProjection, Vector3{0.0f, 0.0f, -50.0f}, up, 1080.0f);

	REQUIRE(near > far);
	REQUIRE(far > 0.0f);

	// Twice the canvas is twice the pixels for the same stud, which is the whole
	// reason this takes the canvas' height rather than the attachment's.
	const float taller = CanvasPixelsPerStud(viewProjection, Vector3{0.0f, 0.0f, -5.0f}, up, 2160.0f);
	REQUIRE(taller == Approx(near * 2.0f));
}

// A point behind the eye projects with a negative w. Dividing by it unguarded
// flips the sign of a height that is about to be divided into a size, and a
// negative pixels-per-stud makes a billboard's pixel half grow the further away
// it gets.
TEST_CASE("pixels per stud is never negative behind the eye", "[render][primitives]") {
	const glm::mat4 projection = glm::perspective(1.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	const glm::mat4 lookAt =
		glm::lookAt(glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});

	const float behind = CanvasPixelsPerStud(
		projection * lookAt, Vector3{0.0f, 0.0f, 20.0f}, Vector3{0.0f, 1.0f, 0.0f}, 1080.0f
	);

	REQUIRE(behind > 0.0f);
	REQUIRE(std::isfinite(behind));
}
