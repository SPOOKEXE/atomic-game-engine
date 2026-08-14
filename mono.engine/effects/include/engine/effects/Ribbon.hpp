#pragma once

// Beams and trails: two things that are one ribbon.
//
// **They are the same geometry and that is not a coincidence worth ignoring.** A
// beam is a strip between two attachments with a Bezier curve through it; a trail
// is a strip between two attachments *sampled over time*. Both come out as a
// run of quads with a width, a colour and a texture coordinate along their
// length, so there is one builder and two sources of points - rather than two
// builders that agree until somebody fixes a winding bug in one of them.
//
// **Neither is a `scene::DrawInstance` and neither has a mesh.** A ribbon's
// geometry is a function of where its endpoints are *this frame*, so a mesh would
// be a mesh rebuilt every frame - which is a mesh table entry churning at frame
// rate for something that is four vertices per segment. The builder writes a
// vertex stream the renderer uploads directly, the same way the particle stream
// is uploaded.
//
// **A trail's history is on the trail's own row and is a fixed-capacity ring.**
// A `std::vector` per trail is an allocation per trail and a pointer chase per
// segment, and - the rule that actually forbids it - a component holding one
// cannot survive being memcpy'd across a process boundary. Sixteen points is
// enough for a sword swipe or a tyre mark at any frame rate worth drawing, and
// `Trail::Lifetime` is what decides how far back in time those sixteen reach.
//
// @tier L8 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::core {
	class ByteReader;
	class ByteWriter;
}

namespace engine::ecs {
	class Store;
}

namespace engine::effects {

	// How many points of history a trail keeps.
	//
	// Sixteen, for the reason the header gives. A power of two, so the ring's
	// index is a mask rather than a modulo - this is walked per trail per frame.
	inline constexpr uint32_t TRAIL_POINTS = 16;

	// How many segments a beam is drawn with.
	//
	// **Ten, and a beam with no curvature is still drawn with ten.** Collapsing a
	// straight beam to one segment would be right for the geometry and wrong for
	// the texture: a beam scrolls its texture along its length, and a single quad
	// interpolates that scroll linearly across the whole span, so a curved
	// texture-space would shear. Ten is cheap enough that the branch is not worth
	// having.
	inline constexpr uint32_t BEAM_SEGMENTS = 10;

	// One vertex of a ribbon, in the layout the pass binds.
	//
	// Twenty-four bytes. Flat and trivially copyable, for `ParticleInstance`'s
	// reason.
	//
	// @since v0.10
	struct RibbonVertex {
		// Where it is, in world space.
		core::Vector3 Position;

		// How far along the ribbon it is, 0 at the start and 1 at the end, and
		// which side of the strip it is on.
		//
		// **Both in one `Vector2` because both are texture coordinates**, and the
		// side is a coordinate rather than a sign so that a texture with a
		// gradient across the ribbon works without the shader knowing which
		// vertex it has.
		core::Vector2 Coordinate;

		// Its colour and alpha, RGBA8.
		//
		// Per vertex rather than per ribbon, because a beam's colour is a
		// `ColorSequence` along its length and a trail's fades towards its tail -
		// which is the whole visual point of both.
		uint32_t Colour = 0xFFFFFFFF;
	};

	// A strip drawn between two attachments.
	//
	// The property surface is Roblox's `Beam`, minus what is not implemented:
	// `LightEmission` and `LightInfluence` are absent rather than declared and
	// ignored, because the pass is unlit - `SurfaceAppearance`'s rule.
	//
	// @since v0.10
	struct Beam {
		// How wide and what colour it is along its length.
		core::ColorSequence Colour{core::Color3{1.0f, 1.0f, 1.0f}};

		// How see-through it is along its length, 0 solid to 1 gone.
		core::NumberSequence Transparency{0.0f};

		// The texture drawn along it. Invalid draws a flat colour.
		core::Name Texture;

		// Which attachment it leaves from.
		//
		// **An entity handle and not a name**, which is `PropertyType::Reference`
		// and is meaningless outside this world - stated because it is the one
		// field here that does not survive a wire on its own. `replication` turns
		// a handle into its own identifier; a beam on a replica whose attachments
		// have not arrived draws nothing, which is the honest state.
		ecs::Entity Attachment0;

		// Which attachment it arrives at.
		ecs::Entity Attachment1;

		// How far the curve control points are pushed along each attachment's own
		// axis, in metres.
		//
		// **Two numbers rather than two `Vector3`s**, which is Roblox's design and
		// is the better one: a control point that could go anywhere would make a
		// beam an authored spline, and what an author actually wants is a beam
		// that leaves along the attachment it is on. The direction comes from the
		// attachment's frame, which is what an attachment carries an orientation
		// for.
		float CurveSize0 = 0.0f;

		// The same, at the other end.
		float CurveSize1 = 0.0f;

		// How wide it is at the start, in metres.
		float Width0 = 1.0f;

		// How wide it is at the end.
		float Width1 = 1.0f;

		// How fast the texture scrolls along it, in texture lengths per second.
		float TextureSpeed = 1.0f;

		// How long one repeat of the texture is, in metres.
		float TextureLength = 1.0f;

		// How far towards the camera the ribbon is nudged, in metres.
		float ZOffset = 0.0f;

		// Whether it faces the camera or keeps the attachments' own up.
		//
		// **The default is to face the camera**, which is what a laser or a lens
		// flare wants and is what makes a beam readable from any angle. A beam
		// that kept a fixed plane vanishes edge-on, which reads as the beam having
		// switched off.
		bool FaceCamera = true;

		// Whether its colour is added to the target rather than blended.
		//
		// Order-independent, exactly as `ParticleEmitter::Additive` is, and here
		// it matters for the same reason: an additive beam needs no sort.
		bool Additive = false;

		// Whether it is drawn.
		bool Enabled = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved = 0;
	};

	// A strip that follows two attachments through the world.
	//
	// **The history is on the row and the row is fixed size**, which is what makes
	// this a component rather than an object with a buffer. See the header.
	//
	// @since v0.10
	struct Trail {
		// What colour it is from head to tail.
		core::ColorSequence Colour{core::Color3{1.0f, 1.0f, 1.0f}};

		// How see-through it is from head to tail.
		core::NumberSequence Transparency{0.0f};

		// Where the top edge has been, newest first.
		//
		// **A ring with the newest at `Head`**, so recording a point is one write
		// and one increment rather than a shift of sixteen positions per trail per
		// frame.
		core::Vector3 Top[TRAIL_POINTS] = {};

		// Where the bottom edge has been, indexed the same way.
		core::Vector3 Bottom[TRAIL_POINTS] = {};

		// How old each recorded point is, in seconds.
		float Age[TRAIL_POINTS] = {};

		// The texture drawn along it.
		core::Name Texture;

		// The attachment tracing the top edge.
		ecs::Entity Attachment0;

		// The attachment tracing the bottom edge.
		ecs::Entity Attachment1;

		// How long a recorded point survives, in seconds.
		float Lifetime = 1.0f;

		// The narrowest angle between two segments that is still drawn, in
		// degrees.
		//
		// Roblox's `MinLength` is a distance and this is an angle, which is the
		// more useful of the two: a trail that doubles back on itself produces a
		// bow-tie of crossed quads, and the thing that identifies it is the turn
		// rather than the length.
		float MinimumAngle = 5.0f;

		// How long one repeat of the texture is, in metres.
		float TextureLength = 1.0f;

		// Where the newest point sits in the ring.
		uint32_t Head = 0;

		// How many points are recorded, up to `TRAIL_POINTS`.
		uint32_t Recorded = 0;

		// Whether the trail is recording new points.
		//
		// **Disabling stops recording and does not clear what is there**, which is
		// `ParticleEmitter::Enabled`'s rule: a swipe trail is enabled for the
		// length of a swing, and a version that cleared on disable would erase the
		// swipe at the moment it finished.
		bool Enabled = true;

		// Whether it is added to the target rather than blended.
		bool Additive = false;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// Every ribbon vertex a world produces this frame, and where each ribbon sits.
	//
	// **One buffer and a run per ribbon**, exactly as the particle pool is one
	// array and a run per emitter. A vertex buffer per beam is a buffer per beam.
	//
	// @since v0.10
	struct RibbonRun {
		// Where this ribbon's vertices start.
		uint32_t First = 0;

		// How many there are. Always even - a strip is pairs.
		uint32_t Count = 0;

		// Which texture, by name. Invalid draws a flat colour.
		core::Name Texture;

		// How far towards the camera it is nudged.
		float ZOffset = 0.0f;

		// Whether it is added rather than blended.
		bool Additive = false;

		// Explicit padding.
		uint8_t Reserved[3] = {};
	};

	// What a world's ribbons came out as.
	//
	// A resource, for `ParticleSystem`'s reason: there is one of it and nothing
	// iterates it.
	//
	// @since v0.10
	struct RibbonBuffer {
		// The vertices, in strip order: top, bottom, top, bottom.
		std::vector<RibbonVertex> Vertices;

		// One per ribbon that produced anything.
		std::vector<RibbonRun> Runs;
	};

	// Records where every enabled trail's attachments are now, and ages what is
	// already recorded.
	//
	// **In the simulation and not in `PreRender`, unlike everything else here**,
	// and the difference is what a trail *is*: a record of where something has
	// been. Sampling it at frame rate would make a trail's length depend on the
	// machine drawing it, so two players swinging the same sword would leave
	// different arcs - which is a desync arriving through a decoration.
	//
	// @param store The world.
	// @param delta How much time passed, in seconds.
	// @return How many trails recorded a point.
	size_t RecordTrails(ecs::Store &store, float delta);

	// Builds every beam's and every trail's vertices into the world's buffer.
	//
	// **`PreRender`, because it needs the eye.** A camera-facing ribbon's vertices
	// are a function of where it is looked at from, which is the same reason the
	// blended pass's sort lives per view.
	//
	// @param store   The world.
	// @param eye     Where the view is, in world space.
	// @param elapsed The world's clock, for texture scroll.
	// @return How many ribbons produced vertices.
	size_t BuildRibbons(ecs::Store &store, const core::Vector3 &eye, float elapsed);

	// The vertices `BuildRibbons` produced.
	//
	// @param store The world.
	// @return The stream, empty when the world has no buffer.
	std::span<const RibbonVertex> RibbonStream(const ecs::Store &store);

	// The runs those vertices are divided into.
	//
	// @param store The world.
	// @return The runs, empty when the world has no buffer.
	std::span<const RibbonRun> RibbonRuns(const ecs::Store &store);

	// Serialisation for the two components, exported so `Registration.cpp` can
	// name them without this file's internals.
	//@{
	void WriteBeams(core::ByteWriter &writer, const void *source, size_t count);
	void ReadBeams(core::ByteReader &reader, void *destination, size_t count);
	void WriteTrails(core::ByteWriter &writer, const void *source, size_t count);
	void ReadTrails(core::ByteReader &reader, void *destination, size_t count);
	//@}
}
