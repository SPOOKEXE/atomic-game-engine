#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

TEST_SUITE_ID("engine.scene.components")
// Collider's two layer fields are spatial::LayerMask, so both its size and its
// defaults move with that type.
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector3;
using engine::scene::BodyKind;
using engine::scene::Bounds;
using engine::scene::Camera;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::PreviousTransform;
using engine::scene::QuickHash;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Surface;
using engine::scene::SurfaceCamera;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::scene::WorldBounds;
using engine::spatial::LayerMask;

// The storage takes the memcpy path for a trivially copyable component — a
// whole column in one call instead of a call per row — and `replication` copies
// runs of adjacent changed rows the same way. A component that quietly stopped
// being trivially copyable would still work and would cost a function call per
// row on the engine's hottest path.
TEST_CASE("every component is trivially copyable", "[scene][components]") {
	CHECK(std::is_trivially_copyable_v<Transform>);
	CHECK(std::is_trivially_copyable_v<PreviousTransform>);
	CHECK(std::is_trivially_copyable_v<Bounds>);
	CHECK(std::is_trivially_copyable_v<Motion>);
	CHECK(std::is_trivially_copyable_v<RigidBody>);
	CHECK(std::is_trivially_copyable_v<Collider>);
	CHECK(std::is_trivially_copyable_v<Surface>);
	CHECK(std::is_trivially_copyable_v<Visual>);
	CHECK(std::is_trivially_copyable_v<Camera>);
	CHECK(std::is_trivially_copyable_v<QuickHash>);
}

// **The one that catches a real bug rather than a design opinion.** A
// trivially copyable component is serialised as its object representation,
// padding included, and padding is never initialised — so a hole here makes two
// runs of one scene produce different snapshot bytes, and `just determinism`
// reports it from `mono.server` with no clue which type is at fault.
//
// Every component's size is checked against the sum of its members, so a field
// added in the middle fails here instead.
TEST_CASE("no component carries unnamed padding", "[scene][components]") {
	CHECK(sizeof(Transform) == sizeof(CFrame));
	CHECK(sizeof(PreviousTransform) == sizeof(CFrame));
	CHECK(sizeof(Bounds) == sizeof(Vector3));
	CHECK(sizeof(Motion) == 2 * sizeof(Vector3));
	CHECK(sizeof(Surface) == sizeof(Name));
	CHECK(sizeof(QuickHash) == sizeof(uint64_t));
	CHECK(sizeof(Camera) == 3 * sizeof(float));
	CHECK(sizeof(SurfaceCamera) == 2 * sizeof(uint16_t) + sizeof(int8_t) + 3);
	CHECK(offsetof(SurfaceCamera, Reserved) + 3 == sizeof(SurfaceCamera));

	CHECK(sizeof(RigidBody) == 3 * sizeof(float) + sizeof(BodyKind) + 3);
	CHECK(
		sizeof(Collider) ==
		sizeof(Vector3) + 2 * sizeof(LayerMask) + sizeof(ShapeKind) + sizeof(bool) + sizeof(uint16_t)
	);
	// **Two v0.6 fields, and only one of them widened the struct.**
	// `Transparency` is a float and needed four-byte alignment, so it could not
	// live in the three named bytes after `Visible` and the row got wider.
	// `Surface` is an `int8_t` and fits, so it took one of those bytes and cost
	// nothing — which is what named padding is *for*, and what this check is
	// here to keep honest.
	CHECK(
		sizeof(Visual) ==
		sizeof(Color3) + 2 * sizeof(Name) + sizeof(float) + sizeof(bool) + sizeof(int8_t) + 2
	);

	// The named padding is the last thing in each, so a member appended after
	// it would reopen the hole silently.
	CHECK(offsetof(RigidBody, Reserved) + 3 == sizeof(RigidBody));
	CHECK(offsetof(Collider, Reserved) + sizeof(uint16_t) == sizeof(Collider));
	CHECK(offsetof(Visual, Reserved) + 2 == sizeof(Visual));
}

TEST_CASE("a default transform is the identity at the origin", "[scene][components]") {
	// The prototype row a class copies from is default-constructed, so these
	// defaults are what `Instance.new` hands back before anything writes to it.
	const Transform transform;
	CHECK(transform.Frame.Position == Vector3::Zero);
	CHECK(transform.Frame.QuaternionW == 1.0f);

	const PreviousTransform previous;
	CHECK(previous.Frame.Position == Vector3::Zero);
}

TEST_CASE("a default body is dynamic and unit mass", "[scene][components]") {
	const RigidBody body;
	CHECK(body.Kind == BodyKind::Dynamic);
	CHECK(body.Mass == 1.0f);

	// **There is no `Sleeping` here and there must not be one.** Whether a
	// body is at rest is the solver's, held in `physics::PhysicsWorld` and
	// expressed to the ECS by the row losing its `Motion` — the archetype move
	// `v02v03v04.md`'s allocation table asks for. A flag on this row would be
	// that same state a second time, and readable only by making the visit the
	// move exists to avoid.
	STATIC_REQUIRE(sizeof(RigidBody) == 16);

	// Damping defaults to a vacuum rather than to a guess, so a scene that
	// wants drag has to say so and one that does not is not silently slowed.
	CHECK(body.LinearDamping == 0.0f);
	CHECK(body.AngularDamping == 0.0f);
}

TEST_CASE("a default collider is a solid box on layer one", "[scene][components]") {
	const Collider collider;
	CHECK(collider.Shape == ShapeKind::Box);
	CHECK_FALSE(collider.Trigger);
	CHECK(collider.Layer == LayerMask::Only(0));

	// Everything by default: a collider that tested against nothing would look
	// exactly like a broken broad phase. `LayerMask` itself defaults to empty,
	// so this default is a decision made here rather than one inherited.
	CHECK(collider.Mask == LayerMask::All());
}

TEST_CASE("a default visual is a visible untinted default mesh", "[scene][components]") {
	const Visual visual;
	CHECK(visual.Visible);
	CHECK(visual.Tint.R == 1.0f);
	CHECK(visual.Tint.G == 1.0f);
	CHECK(visual.Tint.B == 1.0f);

	// Invalid rather than a name like "Cube": the consumer owns what its
	// default mesh is, and a name interned here would be a second place that
	// decides.
	CHECK_FALSE(visual.Mesh.IsValid());
	CHECK_FALSE(visual.Material.IsValid());
}

TEST_CASE("a default surface names nothing", "[scene][components]") {
	// Not "default", which would resolve against the SurfaceTable and give a
	// mistyped material name the same behaviour as a correct one.
	const Surface surface;
	CHECK_FALSE(surface.Material.IsValid());
}

TEST_CASE("bounds and half extents agree on halves", "[scene][components]") {
	// A unit cube is a half-extent of 0.5 on each axis, not 1.0. The default is
	// the place that convention is easiest to get backwards, and every world
	// AABB in the engine is derived from it.
	const Bounds bounds;
	CHECK(bounds.HalfExtent.X == 0.5f);
	CHECK(bounds.HalfExtent.Y == 0.5f);
	CHECK(bounds.HalfExtent.Z == 0.5f);
}

TEST_CASE("a quick hash starts at zero and zero is a real value", "[scene][components]") {
	// Nothing may read zero as "not computed yet". Both sides of a comparison
	// come from the same function, and a hash that happens to be zero is a hash
	// like any other.
	const QuickHash hash;
	CHECK(hash.Value == 0u);
}

TEST_CASE("world bounds are a half extent, once per world", "[scene][components]") {
	// The default matches the box the server's placeholder scene has always
	// bounced inside, so migrating that world onto this type changed no
	// number. Half, not full: the containment test compares an absolute
	// coordinate against it directly.
	const WorldBounds bounds;
	CHECK(bounds.HalfExtent == 64.0f);
}
