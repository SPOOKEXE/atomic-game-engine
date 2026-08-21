#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.part")
// MakePart's collider defaults are spatial::LayerMask values.
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Collider;
using engine::scene::LocalTransparencyOf;
using engine::scene::MakePart;
using engine::scene::Motion;
using engine::scene::PartClass;
using engine::scene::PartDesc;
using engine::scene::Portal;
using engine::scene::RegisterSceneClasses;
using engine::scene::RigidBody;
using engine::scene::SetLocalTransparency;
using engine::scene::ShapeKind;
using engine::scene::Simulated;
using engine::scene::Surface;
using engine::scene::Transform;
using engine::scene::Visual;

TEST_CASE("the part class inherits the whole set", "[scene][part]") {
	// Inheritance is set inclusion, so a query for `Transform` matches every
	// part without knowing parts exist. If `Part` stopped descending from
	// `PVInstance`, that query would silently stop matching rather than fail to
	// compile.
	const ClassId part = PartClass();
	REQUIRE(part.IsValid());

	const ClassId basePart = Classes::Find(Name("BasePart"));
	const ClassId pvInstance = Classes::Find(Name("PVInstance"));
	const ClassId instance = Classes::Find(Name("Instance"));

	CHECK(Classes::IsA(part, basePart));
	CHECK(Classes::IsA(part, pvInstance));
	CHECK(Classes::IsA(part, instance));
	CHECK_FALSE(Classes::IsA(instance, part));
}

TEST_CASE("a part carries the five components the plan names", "[scene][part]") {
	Store store("part_test.set");

	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(part != NULL_ENTITY);

	CHECK(store.Has<Transform>(part));
	CHECK(store.Has<Bounds>(part));
	CHECK(store.Has<Visual>(part));
	CHECK(store.Has<Collider>(part));
	CHECK(store.Has<Surface>(part));

	CHECK(store.IsA(part, PartClass()));
}

TEST_CASE("size is halved once, into bounds and collider", "[scene][part]") {
	// `Size` is a full extent because that is what a person types; everything
	// downstream wants the half. Halving in two places is where the two
	// eventually disagree by a factor of two, and a collider twice the size of
	// its bounds is a body that collides with things the broad phase never
	// offered it.
	Store store("part_test.size");

	PartDesc desc;
	desc.Size = Vector3(4.0f, 2.0f, 6.0f);

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	const Bounds *bounds = store.Get<Bounds>(part);
	REQUIRE(bounds != nullptr);
	CHECK(bounds->HalfExtent == Vector3(2.0f, 1.0f, 3.0f));

	const Collider *collider = store.Get<Collider>(part);
	REQUIRE(collider != nullptr);
	CHECK(collider->Extent == bounds->HalfExtent);
}

TEST_CASE("MakePart with Anchored adds no Motion", "[scene][part]") {
	// Named in `v02v03v04.md` §3.7. The decision is presence and not a flag, so
	// an anchored part is in a different archetype and the dynamic queries never
	// visit it - which is the whole reason static geometry costs nothing per
	// tick.
	//
	// Since v0.18 an anchored part stores *neither* component, so this asserts
	// two absences. That is the polarity: static is what a row looks like when
	// nothing has been said about it.
	Store store("part_test.anchored");

	PartDesc anchored;
	anchored.Simulated = false;

	const Entity fixed = MakePart(store, anchored);
	REQUIRE(fixed != NULL_ENTITY);

	CHECK_FALSE(store.Has<Motion>(fixed));
	CHECK_FALSE(store.Has<Simulated>(fixed));

	// **And it has a body regardless**, which is what stops anchoring a part
	// from throwing away the mass and the drag an author typed.
	CHECK(store.Has<RigidBody>(fixed));
}

TEST_CASE("a dynamic part is a body that moves", "[scene][part]") {
	Store store("part_test.dynamic");

	// **Asked for, since v0.18.** A default `PartDesc` is static, which is what
	// makes it agree with `Instance.new("Part")` - the two disagreed while the
	// default was dynamic, and a part built one way fell where the same part
	// built the other way did not.
	PartDesc desc;
	desc.Simulated = true;

	const Entity moving = MakePart(store, desc);
	REQUIRE(moving != NULL_ENTITY);

	CHECK(store.Has<Motion>(moving));
	CHECK(store.Has<Simulated>(moving));
	CHECK(store.Has<RigidBody>(moving));
}

TEST_CASE("a default PartDesc is static", "[scene][part]") {
	// The v0.18 flip, stated on its own so that moving the default back fails
	// here rather than somewhere a stack quietly stops resting.
	Store store("part_test.default");

	const Entity part = MakePart(store, PartDesc{});
	REQUIRE(part != NULL_ENTITY);

	CHECK_FALSE(store.Has<Simulated>(part));
	CHECK_FALSE(store.Has<Motion>(part));

	// And it still weighs what an author typed, which is the v0.15 correction
	// this polarity had to preserve.
	CHECK(store.Has<RigidBody>(part));
}

TEST_CASE("anchored and dynamic parts do not share a query", "[scene][part]") {
	// The archetype claim, checked rather than asserted: the integration query
	// is `<Transform, const Motion>`, and it must visit exactly the parts that
	// can move however many anchored ones sit beside them.
	Store store("part_test.archetypes");

	PartDesc anchored;
	anchored.Simulated = false;

	PartDesc dynamic;
	dynamic.Simulated = true;

	for (int index = 0; index < 5; index++) {
		MakePart(store, anchored);
	}
	MakePart(store, dynamic);
	MakePart(store, dynamic);

	CHECK(store.CountMatching<Transform>() == 7);
	CHECK(store.CountMatching<Transform, Motion>() == 2);
}

TEST_CASE("a description's names and shape reach the components", "[scene][part]") {
	Store store("part_test.described");

	PartDesc desc;
	desc.Frame = CFrame(Vector3(1.0f, 2.0f, 3.0f));
	desc.Shape = ShapeKind::Cylinder;
	desc.Material = Name("part_test.Oak");
	desc.Mesh = Name("part_test.Barrel");

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	CHECK(store.Get<Transform>(part)->Frame.Position == Vector3(1.0f, 2.0f, 3.0f));
	CHECK(store.Get<Collider>(part)->Shape == ShapeKind::Cylinder);
	CHECK(store.Get<Surface>(part)->Material == desc.Material);
	CHECK(store.Get<Visual>(part)->Mesh == desc.Mesh);
}

TEST_CASE("what the description does not name keeps its prototype default", "[scene][part]") {
	// `MakePart` reads before it writes for the two components `PartDesc` only
	// partly describes. Constructing a fresh `Collider` or `Visual` instead
	// would silently overwrite a layer mask or a tint the class had declared,
	// and nothing at the call site would look wrong.
	Store store("part_test.defaults");

	PartDesc desc;
	desc.Shape = ShapeKind::Sphere;

	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	const Collider *collider = store.Get<Collider>(part);
	REQUIRE(collider != nullptr);
	CHECK(collider->Layer == engine::spatial::LayerMask::Only(0));
	CHECK(collider->Mask == engine::spatial::LayerMask::All());
	CHECK_FALSE(collider->Trigger);

	const Visual *visual = store.Get<Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Visible);
	CHECK(visual->Tint.R == 1.0f);

	// **`PartDesc::Material` is what a part *feels* like and lands in `Surface`
	// alone.** What it looks like is not on a part at all any more - a `Material`
	// instance under one names an asset, and `MakePart` creates no children - so
	// a fresh part is drawn with `render::DefaultTexture` until somebody adds one.
	CHECK_FALSE(store.Get<engine::scene::MaterialRef>(part) != nullptr);
}

TEST_CASE("a replica refuses to mint a part", "[scene][part]") {
	// `Store::CreateInstance` used to walk straight past `SetAdoptOnly` - only
	// `Store::Create` checked it - so `MakePart` carried a guard of its own. The
	// guard is gone and the storage covers it now, which is the right place for
	// it; this case stays because the behaviour a caller depends on is unchanged
	// and the *reason* it holds is exactly what moved.
	Store store("part_test.replica");
	store.SetAdoptOnly(true);

	CHECK(MakePart(store, PartDesc{}) == NULL_ENTITY);
}

// --- the property surface ---------------------------------------------------
//
// `v05.md` §5.5's tests. The interesting ones are not "does a setter set" -
// they are the four ways a property write can look like it worked and not have.

namespace {
	// Reads a property into a value of the type it says it is.
	template <class T> T Read(const Store &store, Entity instance, const char *property) {
		T value{};
		REQUIRE(store.GetProperty(instance, Name(property), &value, sizeof(T)));
		return value;
	}

	template <class T> bool Write(Store &store, Entity instance, const char *property, const T &value) {
		return store.SetProperty(instance, Name(property), &value, sizeof(T));
	}
}

TEST_CASE("Portal Enabled is an authored property", "[scene][part]") {
	Store store("portal_property_test");
	RegisterSceneClasses();
	const Entity portal = store.CreateInstance(engine::ecs::Classes::Find(Name("Portal")), "Portal");
	REQUIRE(portal != NULL_ENTITY);

	CHECK(Read<bool>(store, portal, "Enabled"));
	REQUIRE(Write(store, portal, "Enabled", false));
	CHECK_FALSE(Read<bool>(store, portal, "Enabled"));
	CHECK_FALSE(store.Get<Portal>(portal)->Enabled);
}

TEST_CASE("Position writes the translation and keeps the rotation", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// A rotation that is not the identity, so a setter that replaced the whole
	// CFrame would be visible rather than a no-op.
	REQUIRE(Write(store, part, "Orientation", Vector3{0.0f, 90.0f, 0.0f}));
	const Vector3 before = Read<Vector3>(store, part, "Orientation");

	REQUIRE(Write(store, part, "Position", Vector3{3.0f, -4.0f, 5.0f}));

	const Vector3 position = Read<Vector3>(store, part, "Position");
	CHECK(position.X == 3.0f);
	CHECK(position.Y == -4.0f);
	CHECK(position.Z == 5.0f);

	// The whole point: an offset-shaped setter would have written twelve bytes
	// over the front of the CFrame and left the quaternion behind.
	const Vector3 after = Read<Vector3>(store, part, "Orientation");
	CHECK(after.Y == Catch::Approx(before.Y).margin(1.0e-3f));
}

TEST_CASE("Orientation writes the rotation and keeps the translation", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	REQUIRE(Write(store, part, "Position", Vector3{1.0f, 2.0f, 3.0f}));
	REQUIRE(Write(store, part, "Orientation", Vector3{0.0f, 45.0f, 0.0f}));

	const Vector3 position = Read<Vector3>(store, part, "Position");
	CHECK(position.X == 1.0f);
	CHECK(position.Y == 2.0f);
	CHECK(position.Z == 3.0f);

	// Degrees, because Roblox's Orientation is degrees. Radians here would be
	// a factor of 57 that nothing else in the suite would notice.
	CHECK(Read<Vector3>(store, part, "Orientation").Y == Catch::Approx(45.0f).margin(1.0e-2f));
}

TEST_CASE("Size is a full extent over a stored half, and moves the collider too", "[scene][part]") {
	Store store("property_test");
	PartDesc desc;
	desc.Size = Vector3{2.0f, 2.0f, 2.0f};
	const Entity part = MakePart(store, desc);

	CHECK(Read<Vector3>(store, part, "Size").X == 2.0f);
	CHECK(store.Get<Bounds>(part)->HalfExtent.X == 1.0f);

	REQUIRE(Write(store, part, "Size", Vector3{4.0f, 6.0f, 8.0f}));

	CHECK(store.Get<Bounds>(part)->HalfExtent.X == 2.0f);
	CHECK(store.Get<Bounds>(part)->HalfExtent.Y == 3.0f);

	// Drawn at one size and collided at another is the bug this asserts
	// against, and nothing else in the tree would report it.
	CHECK(store.Get<Collider>(part)->Extent.X == 2.0f);
	CHECK(store.Get<Collider>(part)->Extent.Y == 3.0f);
}

TEST_CASE("a property write marks its column changed", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// The one that matters most and is invisible when it breaks: replication
	// builds deltas from the change channel, so a write the channel never sees
	// is a script edit the server never sends.
	store.Observe<Transform>();
	store.ClearChanges();
	CHECK_FALSE(store.Changed<Transform>(part));

	REQUIRE(Write(store, part, "Position", Vector3{1.0f, 0.0f, 0.0f}));
	CHECK(store.Changed<Transform>(part));
}

TEST_CASE("a size that disagrees with the property is refused", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	// A Vector3 handed to a CFrame property. Truncating would leave the
	// rotation from whatever was there before, which reads as a physics bug a
	// long way from the binding that caused it.
	const Vector3 wrong{1.0f, 2.0f, 3.0f};
	CHECK_FALSE(store.SetProperty(part, Name("CFrame"), &wrong, sizeof(wrong)));

	CFrame value;
	CHECK_FALSE(store.GetProperty(part, Name("CFrame"), &value, sizeof(Vector3)));

	// And a name nothing declares. Not `Transparency` any more - that is a real
	// property at v0.6, and a test asserting a gap that has been closed is a
	// test that outlived what it was checking.
	CHECK_FALSE(Write(store, part, "Reflectance", 0.5f));
}

TEST_CASE("Anchored is presence rather than a flag", "[scene][part]") {
	Store store("property_test");

	// **The Roblox property is the negation of the stored tag**, and this is the
	// test that holds `scene::AnchoredProperty` to being the only place that
	// inversion is spelled: every line below reads the property one way and the
	// component the other.
	PartDesc desc;
	desc.Simulated = true;

	const Entity dynamic = MakePart(store, desc);
	CHECK(store.Get<RigidBody>(dynamic) != nullptr);
	CHECK_FALSE(Read<bool>(store, dynamic, "Anchored"));

	// A structural write: the row moves to another archetype rather than a
	// boolean changing inside it.
	REQUIRE(Write(store, dynamic, "Anchored", true));
	CHECK_FALSE(store.Has<Simulated>(dynamic));
	CHECK(store.Get<Motion>(dynamic) == nullptr);
	CHECK(Read<bool>(store, dynamic, "Anchored"));

	REQUIRE(Write(store, dynamic, "Anchored", false));
	CHECK(store.Has<Simulated>(dynamic));
	CHECK(store.Get<Motion>(dynamic) != nullptr);
	CHECK_FALSE(Read<bool>(store, dynamic, "Anchored"));
}

TEST_CASE("CanCollide is the inverse of the trigger flag", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	CHECK(Read<bool>(store, part, "CanCollide"));

	REQUIRE(Write(store, part, "CanCollide", false));
	CHECK(store.Get<Collider>(part)->Trigger);

	// The layer mask is untouched, which is why this maps to Trigger and not
	// to the mask: clearing and restoring a mask loses whatever a game had
	// configured, and the loss only shows up much later.
	CHECK(store.Get<Collider>(part)->Mask == engine::spatial::LayerMask::All());
}

TEST_CASE("a replica refuses a property write", "[scene][part]") {
	Store store("property_test");
	const Entity part = MakePart(store, PartDesc{});

	store.SetAdoptOnly(true);

	// Refused rather than applied-then-overwritten. A script author cannot tell
	// those apart from inside the script.
	CHECK_FALSE(Write(store, part, "Position", Vector3{9.0f, 9.0f, 9.0f}));
	CHECK(store.Get<Transform>(part)->Frame.Position.X == 0.0f);
}

// --- the two opacities ------------------------------------------------------

TEST_CASE("Transparency stores what a script wrote, out of range included", "[scene][part]") {
	Store store("transparency_test");
	const Entity part = MakePart(store, PartDesc{});

	// **Not clamped, and this is an assertion rather than an absence.** It was
	// clamped for one commit. Roblox does not clamp this - `part.Transparency =
	// 2` reads back as 2 - and matching that is not fidelity for its own sake: a
	// script that drives a fade by arithmetic and reads the value back expects
	// what it wrote, and a property that silently rewrites its input is one an
	// author debugs by disbelieving their own assignment.
	//
	// Where the range has to hold is the renderer, which is a different place
	// from where the value is authored. `SurfaceCamera::ImageTransparency` is
	// still clamped, because it is not Roblox's property and has no such
	// expectation to honour.
	REQUIRE(Write(store, part, "Transparency", 2.5f));
	CHECK(Read<float>(store, part, "Transparency") == 2.5f);

	REQUIRE(Write(store, part, "Transparency", -3.0f));
	CHECK(Read<float>(store, part, "Transparency") == -3.0f);

	// The ordinary range round-trips exactly, which is the half that would still
	// hold under a clamp and is worth pinning either way.
	REQUIRE(Write(store, part, "Transparency", 0.0f));
	CHECK(Read<float>(store, part, "Transparency") == 0.0f);

	REQUIRE(Write(store, part, "Transparency", 1.0f));
	CHECK(Read<float>(store, part, "Transparency") == 1.0f);

	REQUIRE(Write(store, part, "Transparency", 0.25f));
	CHECK(Read<float>(store, part, "Transparency") == 0.25f);
}

TEST_CASE("a surface camera has its own opacity, clamped the same way", "[scene][part]") {
	Store store("image_transparency_test");
	RegisterSceneClasses();

	const Entity camera =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);

	// **Two opacities, because a mirror is two things.** The part's
	// `Transparency` is how much of the world shows through the glass; this is
	// how much of the glass shows through the reflection. Until there were two,
	// fading a mirror faded its reflection with it and there was no way to
	// author a transparent pane that still reflects.
	CHECK(Read<float>(store, camera, "ImageTransparency") == 0.0f);

	REQUIRE(Write(store, camera, "ImageTransparency", 0.5f));
	CHECK(Read<float>(store, camera, "ImageTransparency") == 0.5f);

	REQUIRE(Write(store, camera, "ImageTransparency", 4.0f));
	CHECK(Read<float>(store, camera, "ImageTransparency") == 1.0f);

	REQUIRE(Write(store, camera, "ImageTransparency", -1.0f));
	CHECK(Read<float>(store, camera, "ImageTransparency") == 0.0f);
}

TEST_CASE("resizing a surface camera keeps its grade, face and filter", "[scene][part]") {
	Store store("surface_size_test");
	RegisterSceneClasses();

	const Entity camera =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");
	REQUIRE(camera != NULL_ENTITY);

	// Everything on the component that is not the size, authored first.
	REQUIRE(Write(store, camera, "Effect", Name("Thermal")));
	REQUIRE(Write(store, camera, "Face", Name("Left")));
	REQUIRE(Write(store, camera, "ImageTransparency", 0.5f));

	// **The regression this pins:** `SurfaceSize`'s setter built a fresh
	// `SurfaceCamera` and `store.Set` replaced the whole component, so a
	// resize silently reset the grade, the face, the filter and the opacity.
	REQUIRE(Write(store, camera, "SurfaceSize", Vector3{512.0f, 256.0f, 0.0f}));

	const engine::scene::SurfaceCamera *surface = store.Get<engine::scene::SurfaceCamera>(camera);
	REQUIRE(surface != nullptr);
	CHECK(surface->Width == 512);
	CHECK(surface->Height == 256);
	CHECK(Read<Name>(store, camera, "Effect") == Name("Thermal"));
	CHECK(Read<Name>(store, camera, "Face") == Name("Left"));
	CHECK(Read<float>(store, camera, "ImageTransparency") == 0.5f);
}

TEST_CASE("LocalTransparency is read-only and written through its own door", "[scene][part]") {
	Store store("local_transparency_test");
	const Entity part = MakePart(store, PartDesc{});

	CHECK(LocalTransparencyOf(store, part) == 0.0f);
	CHECK(Read<float>(store, part, "LocalTransparency") == 0.0f);

	// The ordinary property door refuses it, exactly as it refuses any other
	// read-only property - `MeshPart::TrianglesCount` gets the same check for
	// the same reason.
	CHECK_FALSE(Write(store, part, "LocalTransparency", 0.7f));
	CHECK(LocalTransparencyOf(store, part) == 0.0f);

	// The dedicated door works on an ordinary store...
	REQUIRE(SetLocalTransparency(store, part, 0.7f));
	CHECK(LocalTransparencyOf(store, part) == 0.7f);
	CHECK(Read<float>(store, part, "LocalTransparency") == 0.7f);

	// ...and, unlike every other write, on a replica too. A viewer fading
	// their own character has to be able to do it from inside a world they do
	// not own - `scene::SetLocalTransparency`'s header carries the argument.
	store.SetAdoptOnly(true);
	REQUIRE(SetLocalTransparency(store, part, 1.0f));
	CHECK(LocalTransparencyOf(store, part) == 1.0f);

	// The ordinary door stays refused even there - this is not a general
	// bypass of `AdoptOnly`, only the one field that was never going to be
	// sent in the first place.
	CHECK_FALSE(Write(store, part, "LocalTransparency", 0.2f));
}

TEST_CASE("Face is an enum, so a misspelling is refused where it was written", "[scene][part]") {
	Store store("face_test");
	RegisterSceneClasses();

	const Entity camera =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");

	// The default reads back as a name rather than as a number, which is what
	// having an `EnumTable` ordinal in the component buys: the storage is one
	// byte and the surface is a name a script can compare.
	CHECK(Read<Name>(store, camera, "Face") == Name("Front"));

	REQUIRE(Write(store, camera, "Face", Name("Top")));
	CHECK(Read<Name>(store, camera, "Face") == Name("Top"));

	// **Refused rather than defaulted.** A face nobody chose is a mirror
	// projecting off the wrong side of a pane, which looks like a broken
	// reflection rather than like a typo - the same argument `Material` makes
	// about `"Plsatic"`.
	CHECK_FALSE(Write(store, camera, "Face", Name("Frnot")));
	CHECK(Read<Name>(store, camera, "Face") == Name("Top"));

	// Roblox's spelling, which is the one a ported script will use.
	CHECK_FALSE(Write(store, camera, "Face", Name("Up")));
	REQUIRE(Write(store, camera, "Face", Name("Bottom")));
	CHECK(Read<Name>(store, camera, "Face") == Name("Bottom"));
}

TEST_CASE("a MeshPart is a BasePart with Roblox's vocabulary", "[scene][part]") {
	Store store("meshpart_test");
	RegisterSceneClasses();

	const ClassId meshPart = engine::ecs::Classes::Find(Name("MeshPart"));
	REQUIRE(meshPart.IsValid());

	// **It adds no component and it is the only class with the vocabulary.**
	// `Visual::Mesh` and `SurfaceAppearance::ColourMap` sit on `BasePart` as
	// *storage*, because the draw-list pass is a batched walk over a fixed
	// signature and an optional column is what that shape cannot express. What
	// v0.10 moved is the naming: `BasePart` is what `Part`, `MeshPart` and a
	// future `UnionOperation` share, and geometry loaded from a file is not
	// shared by any of them.
	CHECK(engine::ecs::Classes::IsA(meshPart, engine::ecs::Classes::Find(Name("BasePart"))));

	const Entity part = store.CreateInstance(meshPart, "Fox");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// Every drawable has both components, which is what lets the draw-list pass
	// read them as columns rather than joining them per row.
	REQUIRE(store.Get<engine::scene::SurfaceAppearance>(part) != nullptr);
	REQUIRE(store.Get<engine::scene::Tags>(part) != nullptr);

	REQUIRE(Write(store, part, "MeshId", Name("props/fox.amesh")));
	CHECK(Read<Name>(store, part, "MeshId") == Name("props/fox.amesh"));

	REQUIRE(Write(store, part, "TextureID", Name("props/fox.atex")));
	CHECK(Read<Name>(store, part, "TextureID") == Name("props/fox.atex"));

	// **One spelling and not two.** `Mesh` and `ColorMap` were aliases of these
	// on `BasePart` and are gone: two names for one field is the duplication
	// `AGENTS.md` calls the most expensive kind, and it had already cost the
	// asset picker a bug - an alias missing from its table gave a plain text
	// field on the name people actually use.
	Name unused;
	CHECK_FALSE(store.GetProperty(part, Name("Mesh"), &unused, sizeof(unused)));
	CHECK_FALSE(store.GetProperty(part, Name("ColorMap"), &unused, sizeof(unused)));
}

TEST_CASE("a plain Part names no mesh and no texture", "[scene][part]") {
	// **The point of the split.** A `Part` is one of six built-in shapes, and a
	// mesh reference on it is a property that does nothing - an author sets it,
	// the part does not change, and nothing says the class was the wrong one.
	// Offering it is worse than not having it.
	//
	// A `Part` is textured by a `Material` instance under it, which is what
	// `scene/Materials.hpp` is for.
	Store store("plain_part_vocabulary");
	RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Plain");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	Name unused;
	CHECK_FALSE(store.GetProperty(part, Name("Mesh"), &unused, sizeof(unused)));
	CHECK_FALSE(store.GetProperty(part, Name("MeshId"), &unused, sizeof(unused)));
	CHECK_FALSE(store.GetProperty(part, Name("ColorMap"), &unused, sizeof(unused)));
	CHECK_FALSE(store.GetProperty(part, Name("TextureID"), &unused, sizeof(unused)));

	// **The storage is still there and is still dense**, which is the half that
	// did not move: the renderer reads these as columns over every drawable.
	CHECK(store.Get<engine::scene::SurfaceAppearance>(part) != nullptr);

	// And the alpha pair stays, because a `Material` on a `Part` samples a
	// texture whose alpha has to be interpreted.
	CHECK(Read<Name>(store, part, "AlphaMode") == Name("Opaque"));
}

TEST_CASE("AlphaMode is an enum, so a misspelling is refused", "[scene][part]") {
	Store store("alpha_mode_test");
	RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "Hair");

	CHECK(Read<Name>(store, part, "AlphaMode") == Name("Opaque"));

	// `Clip` is the mode a character model needs: hair and eyelashes are cut-out
	// planes, and blending them costs a sort a discard does not.
	REQUIRE(Write(store, part, "AlphaMode", Name("Clip")));
	CHECK(Read<Name>(store, part, "AlphaMode") == Name("Clip"));

	CHECK_FALSE(Write(store, part, "AlphaMode", Name("Clpi")));
	CHECK(Read<Name>(store, part, "AlphaMode") == Name("Clip"));

	REQUIRE(Write(store, part, "AlphaCutoff", 0.25f));
	CHECK(Read<float>(store, part, "AlphaCutoff") == 0.25f);

	// Clamped, for `Transparency`'s reason: a cutoff outside zero to one is a
	// surface that is entirely there or entirely gone.
	REQUIRE(Write(store, part, "AlphaCutoff", 4.0f));
	CHECK(Read<float>(store, part, "AlphaCutoff") == 1.0f);
}

// --- pivots -------------------------------------------------------------------

TEST_CASE("a pivot defaults to the placement itself", "[scene][part]") {
	// **Identity means "the centre"**, which is what makes the field safe to put
	// on every placed thing: a part nobody has given a pivot behaves exactly as
	// it did before pivots existed.
	Store store("pivot_default");
	RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Plain");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	const Vector3 placed{3.0f, 4.0f, 5.0f};
	REQUIRE(Write(store, part, "Position", placed));

	CHECK(engine::scene::PivotOf(store, part).Position == placed);
}

TEST_CASE("a pivot offset moves the handle and not the part", "[scene][part]") {
	// A door's hinge: the part stays where it is and what `GetPivot` answers
	// moves to the edge.
	Store store("pivot_offset");
	RegisterSceneClasses();

	const Entity door = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Door");

	const engine::core::CFrame hinge(Vector3(-2.0f, 0.0f, 0.0f));
	REQUIRE(Write(store, door, "PivotOffset", hinge));

	CHECK(engine::scene::PivotOf(store, door).Position.X == Catch::Approx(-2.0f));

	// The placement itself is untouched - a pivot describes a handle, not a
	// move.
	CHECK(Read<Vector3>(store, door, "Position").X == Catch::Approx(0.0f));
}

TEST_CASE("PivotTo puts the handle where it was asked for", "[scene][part]") {
	// **The inverse, and it is the whole of `PivotTo`.** Setting the transform
	// to the target and hoping is what "PivotTo ignores the offset" bugs are:
	// the placement that puts the *pivot* at the target is `target * Offset` -
	// inverted.
	Store store("pivot_to");
	RegisterSceneClasses();

	const Entity door = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Door");
	const engine::core::CFrame hinge(Vector3(-2.0f, 0.0f, 0.0f));
	REQUIRE(Write(store, door, "PivotOffset", hinge));

	const engine::core::CFrame target(Vector3(10.0f, 1.0f, 0.0f));
	REQUIRE(engine::scene::PivotTo(store, door, target));

	// The handle landed exactly where it was sent.
	const engine::core::CFrame pivot = engine::scene::PivotOf(store, door);
	CHECK(pivot.Position.X == Catch::Approx(10.0f));
	CHECK(pivot.Position.Y == Catch::Approx(1.0f));

	// And the part moved to put it there, rather than onto the target itself.
	CHECK(Read<Vector3>(store, door, "Position").X == Catch::Approx(12.0f));
}

TEST_CASE("PivotTo with no offset is a plain move", "[scene][part]") {
	// The common case has to stay the obvious one, or `PivotTo` becomes a thing
	// people avoid.
	Store store("pivot_plain");
	RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Plain");
	const engine::core::CFrame target(Vector3(7.0f, 0.0f, -3.0f));

	REQUIRE(engine::scene::PivotTo(store, part, target));
	CHECK(Read<Vector3>(store, part, "Position").X == Catch::Approx(7.0f));
	CHECK(Read<Vector3>(store, part, "Position").Z == Catch::Approx(-3.0f));
}

TEST_CASE("something with no placement has no pivot to move", "[scene][part]") {
	// A `Folder` is not a `PVInstance`. Answering the identity rather than
	// raising is what lets a script ask any instance without a class check
	// first - the same rule `IsA` follows for a class nobody registered.
	Store store("pivot_absent");
	RegisterSceneClasses();

	const Entity folder = store.CreateInstance(engine::ecs::Classes::Find(Name("Instance")), "Folder");
	REQUIRE(folder != engine::ecs::NULL_ENTITY);

	CHECK(engine::scene::PivotOf(store, folder).Position == Vector3(0.0f, 0.0f, 0.0f));
	CHECK_FALSE(engine::scene::PivotTo(store, folder, engine::core::CFrame(Vector3(5.0f, 0.0f, 0.0f))));
}

// --- what a part is made of ---------------------------------------------------

TEST_CASE("a part weighs its density times its volume, when it has one", "[scene][part]") {
	using engine::scene::Collider;
	using engine::scene::MassOf;
	using engine::scene::PhysicsProperties;
	using engine::scene::RigidBody;
	using engine::scene::ShapeKind;
	using engine::scene::VolumeOf;

	Collider box;
	box.Shape = ShapeKind::Box;
	box.Extent = Vector3{1.0f, 0.5f, 2.0f};

	// Half-extents, as every other consumer of `Collider::Extent` reads them -
	// a two-by-one-by-four metre box.
	CHECK(VolumeOf(box) == Catch::Approx(8.0f));

	RigidBody body;
	body.Mass = 3.0f;

	// **Without the flag the authored mass stands**, which is what makes the
	// component free to be on every part: four floats nothing reads.
	PhysicsProperties properties;
	CHECK(MassOf(box, body, nullptr) == Catch::Approx(3.0f));
	CHECK(MassOf(box, body, &properties) == Catch::Approx(3.0f));

	properties.Custom = true;
	properties.Density = 2.0f;
	CHECK(MassOf(box, body, &properties) == Catch::Approx(16.0f));

	// **Resizing changes what it weighs, which is the whole reason this is
	// density and not mass.** A part scaled up by an author is heavier without
	// anybody editing a second number.
	box.Extent = Vector3{2.0f, 0.5f, 2.0f};
	CHECK(MassOf(box, body, &properties) == Catch::Approx(32.0f));

	// A shape with no volume, or no density, keeps the authored mass rather
	// than becoming weightless - a zero mass is what the solver reads as
	// immovable, which is the opposite of nearly nothing.
	Collider flat = box;
	flat.Extent = Vector3{0.0f, 0.5f, 2.0f};
	CHECK(MassOf(flat, body, &properties) == Catch::Approx(3.0f));

	properties.Density = 0.0f;
	CHECK(MassOf(box, body, &properties) == Catch::Approx(3.0f));
}

TEST_CASE("a sphere and a cylinder have the volumes they look like", "[scene][part]") {
	using engine::scene::Collider;
	using engine::scene::ShapeKind;
	using engine::scene::VolumeOf;

	Collider sphere;
	sphere.Shape = ShapeKind::Sphere;
	sphere.Extent = Vector3{2.0f, 2.0f, 2.0f};
	CHECK(VolumeOf(sphere) == Catch::Approx(33.5103f).epsilon(0.001));

	Collider cylinder;
	cylinder.Shape = ShapeKind::Cylinder;
	cylinder.Extent = Vector3{1.0f, 3.0f, 1.0f};
	CHECK(VolumeOf(cylinder) == Catch::Approx(18.8496f).epsilon(0.001));
}

TEST_CASE("the physical properties are on every part and readable by name", "[scene][part]") {
	using engine::scene::PhysicsProperties;

	Store store("physical_test");
	engine::scene::RegisterSceneClasses();

	const Entity part = store.CreateInstance(engine::scene::PartClass(), "Crate");
	REQUIRE(part != engine::ecs::NULL_ENTITY);

	// On the class, so a properties panel can show the rows on any part rather
	// than only on the ones somebody has already customised.
	REQUIRE(store.Get<PhysicsProperties>(part) != nullptr);

	bool custom = true;
	REQUIRE(store.GetProperty(part, Name("CustomPhysicalProperties"), &custom, sizeof(custom)));
	CHECK_FALSE(custom);

	custom = true;
	REQUIRE(store.SetProperty(part, Name("CustomPhysicalProperties"), &custom, sizeof(custom)));

	const float density = 4.0f;
	const float friction = 0.9f;
	const float elasticity = 0.25f;
	REQUIRE(store.SetProperty(part, Name("Density"), &density, sizeof(density)));
	REQUIRE(store.SetProperty(part, Name("Friction"), &friction, sizeof(friction)));
	REQUIRE(store.SetProperty(part, Name("Elasticity"), &elasticity, sizeof(elasticity)));

	const PhysicsProperties *stored = store.Get<PhysicsProperties>(part);
	REQUIRE(stored != nullptr);
	CHECK(stored->Custom);
	CHECK(stored->Density == Catch::Approx(4.0f));
	CHECK(stored->Friction == Catch::Approx(0.9f));
	CHECK(stored->Elasticity == Catch::Approx(0.25f));

	// **A part made by `Instance.new` is anchored here**, because `Simulated`
	// and `Motion` are not class components - `MakePart` says whether a part has
	// them and a bare `CreateInstance` says nothing at all. So it has to be
	// unanchored before it has a mass, which is also what a script does.
	float mass = 0.0f;
	REQUIRE(store.GetProperty(part, Name("Mass"), &mass, sizeof(mass)));
	CHECK(mass == Catch::Approx(0.0f));

	const bool moving = false;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &moving, sizeof(moving)));

	// **Mass is read-only**, because writing it would mean deciding which of
	// density and size an assignment moves, and both answers are wrong. A
	// default part is one cubic metre, so four is its density.
	REQUIRE(store.GetProperty(part, Name("Mass"), &mass, sizeof(mass)));
	CHECK(mass == Catch::Approx(4.0f));

	const float refused = 99.0f;
	CHECK_FALSE(store.SetProperty(part, Name("Mass"), &refused, sizeof(refused)));

	// An anchored part has no body to weigh, and says zero rather than
	// inventing one.
	const bool anchored = true;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	REQUIRE(store.GetProperty(part, Name("Mass"), &mass, sizeof(mass)));
	CHECK(mass == Catch::Approx(0.0f));
}

TEST_CASE("anchoring a part keeps the numbers an author typed", "[scene][part]") {
	// **The whole point of separating `RigidBody` from the anchored decision.**
	// Until v0.15 the two were one component, so anchoring a part deleted its
	// mass and its drag and unanchoring it brought back the defaults - an author
	// who anchored a crate to move it and let it go again found it weighed one
	// kilogram and slid like glass, with nothing saying why.
	Store store("part_test.keeps");
	engine::scene::EnsureClassTree();

	PartDesc desc;
	desc.Simulated = true;
	const Entity part = MakePart(store, desc);
	REQUIRE(part != NULL_ENTITY);

	{
		RigidBody *body = store.GetMutable<RigidBody>(part);
		REQUIRE(body != nullptr);
		body->Mass = 42.0f;
		body->LinearDamping = 0.25f;
		body->AngularDamping = 0.125f;
	}

	bool anchored = true;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	const RigidBody *whileAnchored = store.Get<RigidBody>(part);
	REQUIRE(whileAnchored != nullptr);
	CHECK(whileAnchored->Mass == 42.0f);
	CHECK(whileAnchored->LinearDamping == 0.25f);
	CHECK(whileAnchored->AngularDamping == 0.125f);

	// And `Mass` still reads zero while it is anchored, which is what the solver
	// takes as immovable - the stored number is not the answer to that question.
	float mass = -1.0f;
	REQUIRE(store.GetProperty(part, Name("Mass"), &mass, sizeof(mass)));
	CHECK(mass == 0.0f);

	anchored = false;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	const RigidBody *letGo = store.Get<RigidBody>(part);
	REQUIRE(letGo != nullptr);
	CHECK(letGo->Mass == 42.0f);
	CHECK(letGo->LinearDamping == 0.25f);
	CHECK(letGo->AngularDamping == 0.125f);
}

TEST_CASE("drag reads on an anchored part rather than raising", "[scene][part]") {
	// `Store::GetProperty` returning false becomes `could not read 'X'` in Luau,
	// so a getter that declines is a script error on a field access that looks
	// like every other one. An anchored part had no `RigidBody` and both of
	// these declined.
	Store store("part_test.drag");
	engine::scene::EnsureClassTree();

	PartDesc desc;
	desc.Simulated = false;
	const Entity part = MakePart(store, desc);

	float damping = -1.0f;
	CHECK(store.GetProperty(part, Name("LinearDamping"), &damping, sizeof(damping)));
	CHECK(damping == 0.0f);
	CHECK(store.GetProperty(part, Name("AngularDamping"), &damping, sizeof(damping)));
	CHECK(damping == 0.0f);
}

TEST_CASE("light classes expose only controls their render path consumes", "[scene][part]") {
	RegisterSceneClasses();
	Store store("part_test.light_surface");

	const auto has = [&](std::string_view className, std::string_view propertyName) {
		const Entity light = store.CreateInstance(Classes::Find(Name(className)), className);
		for (const auto &property : store.PropertiesOf(light)) {
			if (property.Name == Name(propertyName)) {
				return true;
			}
		}
		return false;
	};

	// The renderer has no local-light shadow pass, and a point has no cone or
	// face. A writable row for any of those would claim an effect that cannot
	// occur.
	CHECK_FALSE(has("PointLight", "Shadows"));
	CHECK_FALSE(has("PointLight", "Angle"));
	CHECK_FALSE(has("PointLight", "Face"));

	CHECK(has("SpotLight", "Angle"));
	CHECK(has("SpotLight", "Face"));
	CHECK(has("SurfaceLight", "Angle"));
	CHECK(has("SurfaceLight", "Face"));
	CHECK(has("PointLight", "Brightness"));
}
