#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Visibility.hpp>
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
using engine::scene::MaterialRef;
using engine::scene::Motion;
using engine::scene::NormalId;
using engine::scene::Portal;
using engine::scene::PreviousTransform;
using engine::scene::QuickHash;
using engine::scene::Rendered;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Surface;
using engine::scene::SurfaceCamera;
using engine::scene::SurfaceEffect;
using engine::scene::SurfaceLens;
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
	CHECK(std::is_trivially_copyable_v<Portal>);
	CHECK(std::is_trivially_copyable_v<SurfaceLens>);
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
	// **`ImageTransparency` widened this one and the two bytes it needed came
	// out of the named padding**, which is the same trade `Visual` records
	// above: a float needs four-byte alignment, so it could not sit in the three
	// bytes after `Surface` — the struct grew — while `Face` is a byte-wide enum
	// and took one of them for nothing. Two are left.
	//
	// **`TagFilter` widened it again, by four**, and it could not have been
	// paid for out of those two: a `uint32_t` needs four-byte alignment and the
	// remaining padding is a tail after a pair of bytes. That is a real cost on
	// a component every mirror in a world carries, and it is worth it because
	// the alternative — a name resolved per instance per pass — is a lookup in
	// the draw loop rather than four bytes in a row that is already sixteen.
	//
	// **`Effect` widened nothing**, and that is the point of a named reserve: a
	// byte-wide addition to a component every mirror carries came out of the two
	// spare bytes rather than out of a fourth word. One is left, and the day it
	// runs out this case fails rather than a hole appearing in a snapshot.
	CHECK(
		sizeof(SurfaceCamera) == 2 * sizeof(uint16_t) + sizeof(float) + sizeof(uint32_t) + sizeof(int8_t) +
									 sizeof(NormalId) + sizeof(SurfaceEffect) + 1
	);
	CHECK(offsetof(SurfaceCamera, Reserved) + sizeof(SurfaceCamera::Reserved) == sizeof(SurfaceCamera));

	// **A portal is a handle, a world and a reserve**, and the second of those
	// is what makes the third necessary. Which part the hole leads to decides
	// where the camera stands; which *world* decides what it draws, and the two
	// are separate because only the first is arithmetic a store can do for
	// itself. An `Entity` is eight bytes and a `Name` is four, so four are left
	// over and are named rather than left to the compiler.
	CHECK(offsetof(Portal, Reserved) + sizeof(Portal::Reserved) == sizeof(Portal));

	// **Ten floats and a `CFrame`, and not one byte more.** A fitted frustum is
	// four extents, two distances and a plane, and the placement the pane was
	// mapped by rides with it — a portal's frustum is fitted to the *mapped*
	// source pane, so the transform that mapped it is part of the answer rather
	// than something to look up again. A `CFrame` is a `Vector3` and a
	// quaternion, both four-byte aligned, so it opens no hole beside the floats.
	// A member of another width added here would, and a hole in a component is
	// uninitialised bytes in a snapshot.
	CHECK(sizeof(SurfaceLens) == 10 * sizeof(float) + sizeof(engine::core::CFrame));
	CHECK(offsetof(SurfaceLens, Mapping) + sizeof(engine::core::CFrame) == sizeof(SurfaceLens));

	// A face is one byte, so a component can hold one without the row noticing.
	CHECK(sizeof(NormalId) == sizeof(uint8_t));

	// And so is an effect, which is what let it come out of the reserve.
	CHECK(sizeof(SurfaceEffect) == sizeof(uint8_t));

	CHECK(sizeof(RigidBody) == 3 * sizeof(float) + sizeof(BodyKind) + 3);
	CHECK(
		sizeof(Collider) ==
		sizeof(Vector3) + 2 * sizeof(LayerMask) + sizeof(ShapeKind) + sizeof(bool) + sizeof(uint16_t)
	);
	// **Three fields added since v0.6, and only one of them widened the
	// struct.** `Transparency` is a float and needed four-byte alignment, so it
	// could not live in the three named bytes after `Visible` and the row got
	// wider. `Surface` is an `int8_t` and `CastShadow` is a `bool`, so each took
	// one of those bytes and cost nothing — which is what named padding is
	// *for*, and what this check is here to keep honest. One byte is left.
	//
	// **Two `Name`s, and both of the changes that got it there are v0.10's.**
	// `Material` came off — a material is content named by a `Material`
	// instance, not a word on every drawable, `scene/Materials.hpp` — and
	// `Fitted` went on, which records the mesh `Bounds` was last shaped to fit.
	//
	// **The second one is paid for on every part in the world**, including every
	// plain `Part` that will never name a mesh, and that is the honest cost of
	// keeping `client::CollectInstances` a batched walk over fixed columns: the
	// same trade `SurfaceAppearance` makes one component over. Four bytes an
	// entity buys a fit rule with nothing to keep in step — see `Visual::Fitted`
	// for why a bool would have been cheaper and wrong.
	//
	// **Four bytes of room again, as of v0.12.** `Surface`, `CastShadow` and
	// `Locked` used the original three; the hole was then empty and the next
	// `bool` would have widened the row anyway, so it was widened once on
	// purpose. This line is what makes the *next* growth visible in a diff
	// rather than discovered in a profile.
	CHECK(
		sizeof(Visual) == sizeof(Color3) + 2 * sizeof(Name) + sizeof(float) + sizeof(bool) + sizeof(int8_t) +
							  sizeof(bool) + sizeof(bool) + sizeof(Visual::Reserved)
	);

	CHECK(sizeof(Rendered) == sizeof(uint8_t) + 3);

	// The named padding is the last thing in each, so a member appended after
	// it would reopen the hole silently.
	//
	// Written as `sizeof(T::Reserved)` rather than as a literal, because the
	// literal is the thing that goes stale: `Visual::Reserved` shrank from two
	// bytes to one when `CastShadow` moved into the hole, and a hand-written 2
	// here would have failed for a struct that was in fact still exactly
	// packed.
	CHECK(offsetof(RigidBody, Reserved) + sizeof(RigidBody::Reserved) == sizeof(RigidBody));
	CHECK(offsetof(Collider, Reserved) + sizeof(Collider::Reserved) == sizeof(Collider));
	CHECK(offsetof(Visual, Reserved) + sizeof(Visual::Reserved) == sizeof(Visual));
	CHECK(offsetof(Rendered, Reserved) + sizeof(Rendered::Reserved) == sizeof(Rendered));
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

	// **A `Visual` no longer carries a material at all**, which is the change
	// v0.10 made and this is where it is pinned. It used to default to
	// `Plastic` so a properties panel had something to show and a script had
	// something to compare against — both real problems with a seventeen-name
	// enum nothing sampled. A material is content now: a `Material` instance
	// under the part names one, and a part with none draws
	// `render::DefaultTexture`. See `scene/Materials.hpp`.
	//
	// **Nothing asserts the absence, and there is no way to.** A `requires`
	// expression naming a member of a concrete type is a hard error rather than
	// a substitution failure, so the check that "`Visual` has no `Material`" is
	// the compiler refusing every call site — which it does, loudly, and which is
	// what the rest of this change is. The test below is the positive half.
}

TEST_CASE("a material reference starts at none", "[scene][components]") {
	// **`None` and not `Plastic`, which is the whole shape of the change.** The
	// enum defaulted to a value the renderer could not act on; this defaults to
	// "nothing chosen yet", which is a state the renderer draws — the engine's
	// own white plastic — and a state an author can read as unfinished.
	const MaterialRef material;
	CHECK_FALSE(material.Asset.IsValid());
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
