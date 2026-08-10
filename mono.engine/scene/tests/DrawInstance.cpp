#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

TEST_SUITE_ID("engine.scene.drawinstance")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::scene::Bounds;
using engine::scene::DrawInstance;
using engine::scene::MAX_SURFACES;
using engine::scene::OrderScene;
using engine::scene::ScenePlan;
using engine::scene::SurfaceRun;
using engine::scene::Transform;
using engine::scene::Visual;

TEST_CASE("a draw instance survives being copied as bytes", "[scene][drawinstance]") {
	// The property the type exists for: the day a world is a process, this
	// crosses as a block of memory. A field that owned an allocation would
	// compile, pass every test that stayed in one process, and become a
	// use-after-free the day the world moved.
	REQUIRE(std::is_trivially_copyable_v<DrawInstance>);

	DrawInstance instance;
	instance.Frame = CFrame(Vector3(1.0f, 2.0f, 3.0f));
	instance.HalfExtent = Vector3(2.0f, 0.5f, 2.0f);
	instance.Mesh = Name("drawinstance_test.Mesh");

	DrawInstance copy;
	std::memcpy(&copy, &instance, sizeof(DrawInstance));

	CHECK(copy.Frame.Position == instance.Frame.Position);
	CHECK(copy.HalfExtent == instance.HalfExtent);
	CHECK(copy.Mesh == instance.Mesh);
}

TEST_CASE("a default draw instance is a white unit cube at the origin", "[scene][drawinstance]") {
	const DrawInstance instance;
	CHECK(instance.Frame.Position == Vector3::Zero);
	CHECK(instance.HalfExtent.X == 0.5f);
	CHECK(instance.Tint.R == 1.0f);

	// Names rather than handles, and invalid means "the consumer's default".
	// A handle would be a number that only means something inside one process,
	// which is the rule this whole payload is shaped by.
	CHECK_FALSE(instance.Mesh.IsValid());

	// **The texture, where a material name used to sit beside it.** A material
	// is content that resolves to a texture before a draw list is built, so
	// there is nothing left for a draw instance to carry a second name for.
	CHECK_FALSE(instance.Texture.IsValid());
}

TEST_CASE("a draw instance is built from scene components without conversion", "[scene][drawinstance]") {
	// The producer copies rather than converts, which is the reason
	// `HalfExtent` is a half-extent and `Tint` is a `Color3`: a producer that
	// had to convert would be a producer with a device's conventions in it.
	Transform transform;
	transform.Frame = CFrame(Vector3(4.0f, 0.0f, -2.0f));

	Bounds bounds;
	bounds.HalfExtent = Vector3(1.0f, 3.0f, 1.0f);

	Visual visual;
	visual.Mesh = Name("drawinstance_test.Wedge");
	visual.Tint = engine::core::Color3(0.25f, 0.5f, 0.75f);

	// **Every field named, because `-Werror=missing-field-initializers` is on
	// under the `ci` preset.** A partial aggregate initialiser is silently fine
	// under `dev` and fatal there, which is exactly what happened when v0.9
	// widened this type — so the fields are spelled out rather than trailing
	// off and relying on the defaults.
	DrawInstance instance;
	instance.Frame = transform.Frame;
	instance.HalfExtent = bounds.HalfExtent;
	instance.Tint = visual.Tint;
	instance.Mesh = visual.Mesh;

	CHECK(instance.Frame.Position == transform.Frame.Position);
	CHECK(instance.HalfExtent == bounds.HalfExtent);
	CHECK(instance.Tint.G == 0.5f);
	CHECK(instance.Mesh == visual.Mesh);
}

// --- ordering for the transparent pass --------------------------------------

TEST_CASE("an opaque list keeps the order the world produced", "[scene][drawinstance]") {
	// The cheap case, and the common one. A scene with no transparency must
	// come out of this exactly as it went in — that is what makes a recording
	// of one replay, and it means the cost is one pass and no comparisons.
	std::vector<DrawInstance> instances(5);
	for (size_t index = 0; index < instances.size(); index++) {
		instances[index].Frame = CFrame{Vector3{static_cast<float>(index), 0.0f, 0.0f}};
	}

	std::vector<uint32_t> order;
	const size_t opaque = engine::scene::OrderForDrawing(instances, Vector3::Zero, order);

	CHECK(opaque == instances.size());
	CHECK(order == std::vector<uint32_t>{0, 1, 2, 3, 4});
}

TEST_CASE("transparent instances move to the back, farthest first", "[scene][drawinstance]") {
	// A blended fragment mixes with what is already in the target, so a near
	// pane drawn before a far one blends the far one *into* a pixel that should
	// have hidden it — a window that looks right from one side of the room and
	// wrong from the other.
	std::vector<DrawInstance> instances(4);
	instances[0].Frame = CFrame{Vector3{1.0f, 0.0f, 0.0f}};
	instances[0].Transparency = 0.5f;
	instances[1].Frame = CFrame{Vector3{2.0f, 0.0f, 0.0f}};
	instances[2].Frame = CFrame{Vector3{9.0f, 0.0f, 0.0f}};
	instances[2].Transparency = 0.5f;
	instances[3].Frame = CFrame{Vector3{4.0f, 0.0f, 0.0f}};

	std::vector<uint32_t> order;
	const size_t opaque = engine::scene::OrderForDrawing(instances, Vector3::Zero, order);

	CHECK(opaque == 2);

	// The opaque head, in world order.
	CHECK(order[0] == 1);
	CHECK(order[1] == 3);

	// The transparent tail, farthest from the eye first.
	CHECK(order[2] == 2);
	CHECK(order[3] == 0);
}

TEST_CASE("the order depends on where the camera is", "[scene][drawinstance]") {
	// **The first thing the renderer does that depends on which camera is
	// looking.** Two views of one world produce two orders, which is exactly
	// why the ordering is per view rather than baked into the draw list.
	std::vector<DrawInstance> instances(2);
	instances[0].Frame = CFrame{Vector3{0.0f, 0.0f, 0.0f}};
	instances[0].Transparency = 0.5f;
	instances[1].Frame = CFrame{Vector3{10.0f, 0.0f, 0.0f}};
	instances[1].Transparency = 0.5f;

	std::vector<uint32_t> order;

	engine::scene::OrderForDrawing(instances, Vector3{-5.0f, 0.0f, 0.0f}, order);
	CHECK(order == std::vector<uint32_t>{1, 0});

	engine::scene::OrderForDrawing(instances, Vector3{15.0f, 0.0f, 0.0f}, order);
	CHECK(order == std::vector<uint32_t>{0, 1});
}

TEST_CASE("equal distances keep world order, so a recording replays", "[scene][drawinstance]") {
	// An unstable sort would swap them from frame to frame as the comparison
	// fell either way — a determinism failure arriving through a renderer.
	std::vector<DrawInstance> instances(3);
	for (auto &instance : instances) {
		instance.Frame = CFrame{Vector3{3.0f, 0.0f, 0.0f}};
		instance.Transparency = 0.5f;
	}

	std::vector<uint32_t> first;
	std::vector<uint32_t> again;
	engine::scene::OrderForDrawing(instances, Vector3::Zero, first);
	engine::scene::OrderForDrawing(instances, Vector3::Zero, again);

	CHECK(first == std::vector<uint32_t>{0, 1, 2});
	CHECK(first == again);
}

TEST_CASE("a hair of transparency is treated as opaque", "[scene][drawinstance]") {
	// A tween settling on "opaque" lands a few millionths off, and paying a
	// sort, a pipeline switch and the loss of depth writes for that is paying
	// for nothing.
	DrawInstance nearlyOpaque;
	nearlyOpaque.Transparency = 1.0f / 100000.0f;
	CHECK_FALSE(engine::scene::IsTransparent(nearlyOpaque));

	DrawInstance glass;
	glass.Transparency = 0.5f;
	CHECK(engine::scene::IsTransparent(glass));

	DrawInstance solid;
	CHECK_FALSE(engine::scene::IsTransparent(solid));
}

TEST_CASE("an empty list orders to nothing", "[scene][drawinstance]") {
	std::vector<uint32_t> order;
	CHECK(engine::scene::OrderForDrawing({}, Vector3::Zero, order) == 0);
	CHECK(order.empty());
}

namespace {
	// A draw list where each entry is identified by its index, so an assertion
	// about an *order* can name what it expects.
	std::vector<DrawInstance> Casters(const std::vector<bool> &casts) {
		std::vector<DrawInstance> instances(casts.size());
		for (size_t index = 0; index < casts.size(); index++) {
			instances[index].CastShadow = casts[index];
			instances[index].Frame = CFrame(Vector3(static_cast<float>(index), 0.0f, 0.0f));
		}
		return instances;
	}

	std::vector<uint32_t> Identity(size_t count) {
		std::vector<uint32_t> order(count);
		for (size_t index = 0; index < count; index++) {
			order[index] = static_cast<uint32_t>(index);
		}
		return order;
	}
}

TEST_CASE("a scene where everything casts is left alone", "[scene][drawinstance]") {
	// The common case by a long way, and the one where a partition that
	// reordered anything would be a recording that stopped replaying for a
	// feature nobody used.
	const std::vector<DrawInstance> instances = Casters({true, true, true, true});
	std::vector<uint32_t> order = Identity(instances.size());

	CHECK(engine::scene::PartitionCasters(instances, order) == 4);
	CHECK(order == std::vector<uint32_t>{0, 1, 2, 3});
}

TEST_CASE("a scene where nothing casts partitions to nothing", "[scene][drawinstance]") {
	const std::vector<DrawInstance> instances = Casters({false, false, false});
	std::vector<uint32_t> order = Identity(instances.size());

	// Zero, which is what tells the renderer to skip the shadow pass outright
	// rather than clear a depth target nothing writes into.
	CHECK(engine::scene::PartitionCasters(instances, order) == 0);
	CHECK(order == std::vector<uint32_t>{0, 1, 2});
}

TEST_CASE("casters move to the front and keep world order", "[scene][drawinstance]") {
	const std::vector<DrawInstance> instances = Casters({false, true, false, true, true});
	std::vector<uint32_t> order = Identity(instances.size());

	CHECK(engine::scene::PartitionCasters(instances, order) == 3);

	// **Stable on both sides.** The casters keep the order the world produced
	// them in and so do the ones left behind — an unstable partition would
	// shuffle them from frame to frame as the comparison fell either way, which
	// is a determinism failure arriving through a renderer.
	CHECK(order == std::vector<uint32_t>{1, 3, 4, 0, 2});
}

TEST_CASE("an empty range partitions to nothing", "[scene][drawinstance]") {
	// The range the renderer passes for the mirror run in a scene with no
	// mirror in it, which is almost every scene.
	const std::vector<DrawInstance> instances = Casters({true, false});
	std::vector<uint32_t> order;

	CHECK(engine::scene::PartitionCasters(instances, order) == 0);
}

// **The arithmetic the shadow pass actually performs**, exercised through the
// function the renderer calls rather than reproduced here.
//
// That distinction is the whole value of the test. Every field of `ScenePlan`
// becomes a `first_instance` and a count on a draw call, and those are the part
// of a render pass that is easy to get wrong by one and impossible to see in a
// screenshot — a shadow range short by one loses a caster somewhere off screen
// and the frame still looks right. A test that recomputed the offsets would
// only check that the author of the test agreed with themselves.
TEST_CASE("the scene plan divides a view into the runs its passes draw", "[scene][drawinstance]") {
	std::vector<DrawInstance> instances(6);

	// 0: plain opaque caster
	// 1: opaque, does not cast
	// 2: mirror, casts
	// 3: transparent — never reaches the shadow pass whatever it says
	// 4: plain opaque caster
	// 5: mirror, does not cast
	instances[0].CastShadow = true;
	instances[1].CastShadow = false;
	instances[2].CastShadow = true;
	instances[2].Surface = 0;
	instances[3].CastShadow = true;
	instances[3].Transparency = 0.5f;
	instances[4].CastShadow = true;
	instances[5].CastShadow = false;
	instances[5].Surface = 0;

	std::vector<uint32_t> order;
	const engine::scene::ScenePlan plan = engine::scene::OrderScene(instances, Vector3::Zero, order);

	CHECK(plan.Opaque == 5);
	CHECK(plan.Transparent == 1);
	CHECK(plan.Reflected == 3);
	CHECK(plan.Surfaces == 2);
	CHECK(plan.ReflectedCasters == 2);
	CHECK(plan.SurfaceCasters == 1);

	// What the shadow pass's two draws would submit, gathered as the GPU would
	// from `first_instance` and a count.
	std::vector<uint32_t> shadowed;
	for (uint32_t at = 0; at < plan.ReflectedCasters; at++) {
		shadowed.push_back(order[at]);
	}
	for (uint32_t at = 0; at < plan.SurfaceCasters; at++) {
		shadowed.push_back(order[plan.Reflected + at]);
	}
	std::sort(shadowed.begin(), shadowed.end());

	// Instances 0, 2 and 4: every opaque caster, and neither the opaque
	// non-caster, the mirror that does not cast, nor the transparent one that
	// claims to.
	CHECK(shadowed == std::vector<uint32_t>{0, 2, 4});

	// And the surface pass's single range holds no mirror, which is what stops
	// a mirror from filling its own reflection with itself.
	for (uint32_t at = 0; at < plan.Reflected; at++) {
		INFO(at);
		CHECK(instances[order[at]].Surface < 0);
	}
}

// A scene with nothing in it, and a scene with nothing opaque in it. Both reach
// the renderer — a world of glass is a world somebody will build — and both
// would divide by the wrong count if the plan were derived rather than
// returned.
TEST_CASE("a scene plan copes with nothing to draw", "[scene][drawinstance]") {
	std::vector<uint32_t> order;

	const engine::scene::ScenePlan empty = engine::scene::OrderScene({}, Vector3::Zero, order);
	CHECK(empty.Opaque == 0);
	CHECK(empty.Transparent == 0);
	CHECK(empty.Reflected == 0);
	CHECK(empty.Surfaces == 0);
	CHECK(empty.ReflectedCasters == 0);
	CHECK(empty.SurfaceCasters == 0);

	std::vector<DrawInstance> glass(3);
	for (DrawInstance &pane : glass) {
		pane.Transparency = 0.5f;
		pane.CastShadow = true;
	}

	const engine::scene::ScenePlan blended = engine::scene::OrderScene(glass, Vector3::Zero, order);
	CHECK(blended.Opaque == 0);
	CHECK(blended.Transparent == 3);

	// **No casters, though every one of them says it casts.** Glass writing
	// full depth into the shadow map would cast a solid shadow; the pass draws
	// the opaque runs alone, and this is where that is decided.
	CHECK(blended.ReflectedCasters == 0);
	CHECK(blended.SurfaceCasters == 0);
}

// **A texture per mirror means a draw per mirror, and a draw is an offset and a
// count.** Until v0.8 every pane sampled one target, so "the mirrors" was a
// single range and a single sampler binding. `SurfaceRun` is what replaced that,
// and it is only worth anything if each index really is contiguous — a run that
// overlapped another would bind one surface's texture and draw another's panes,
// which is a mirror showing the wrong room and impossible to read off a
// screenshot.
TEST_CASE("mirrors are grouped by the surface they show", "[scene][drawinstance]") {
	std::vector<DrawInstance> instances(7);
	for (DrawInstance &instance : instances) {
		instance.CastShadow = true;
	}

	// Interleaved on purpose, and with a plain instance between them: the
	// grouping has to hold whatever order the world produced, and a list that
	// arrived already sorted would pass without exercising anything.
	instances[0].Surface = 2;
	instances[1].Surface = -1;
	instances[2].Surface = 0;
	instances[3].Surface = 2;
	instances[4].Surface = -1;
	instances[5].Surface = 0;
	instances[6].Surface = 2;

	std::vector<uint32_t> order;
	const ScenePlan plan = OrderScene(instances, Vector3::Zero, order);

	REQUIRE(plan.Opaque == 7);
	REQUIRE(plan.Reflected == 2);
	REQUIRE(plan.Surfaces == 5);

	CHECK(plan.Runs[0].OpaqueCount == 2);
	CHECK(plan.Runs[1].OpaqueCount == 0);
	CHECK(plan.Runs[2].OpaqueCount == 3);

	// Contiguous, in the buffer, and inside the mirror run rather than anywhere
	// in it. Both halves matter: `OpaqueFirst` is handed to a draw call as
	// `first_instance`, so a run that started before `Reflected` would draw the
	// world through a mirror's sampler.
	CHECK(plan.Runs[0].OpaqueFirst == plan.Reflected);
	CHECK(plan.Runs[2].OpaqueFirst == plan.Reflected + plan.Runs[0].OpaqueCount);

	// Every entry in a run really shows that run's surface.
	for (uint8_t surface : {uint8_t{0}, uint8_t{2}}) {
		const SurfaceRun &run = plan.Runs[surface];
		INFO("surface " << static_cast<int>(surface));

		for (uint32_t step = 0; step < run.OpaqueCount; step++) {
			CHECK(instances[order[run.OpaqueFirst + step]].Surface == static_cast<int8_t>(surface));
		}
	}

	// And the non-mirrors are still at the front, untouched by the grouping.
	CHECK(instances[order[0]].Surface == -1);
	CHECK(instances[order[1]].Surface == -1);
}

// **The casters moved, and the plan says where.** They used to be one range
// contiguous from `Reflected` — the shadow pass drew it with a single call — and
// grouping by index took that away, because one run cannot be both grouped by
// surface and split by caster. They are partitioned inside each group now, and a
// shadow pass that still assumed the old single range would silently stop
// drawing some mirrors into the shadow map.
TEST_CASE("shadow casters are partitioned inside each surface run", "[scene][drawinstance]") {
	std::vector<DrawInstance> instances(4);

	instances[0].Surface = 1;
	instances[0].CastShadow = false;
	instances[1].Surface = 0;
	instances[1].CastShadow = true;
	instances[2].Surface = 1;
	instances[2].CastShadow = true;
	instances[3].Surface = 0;
	instances[3].CastShadow = false;

	std::vector<uint32_t> order;
	const ScenePlan plan = OrderScene(instances, Vector3::Zero, order);

	REQUIRE(plan.Surfaces == 4);
	CHECK(plan.SurfaceCasters == 2);

	for (uint8_t surface : {uint8_t{0}, uint8_t{1}}) {
		const SurfaceRun &run = plan.Runs[surface];
		INFO("surface " << static_cast<int>(surface));

		REQUIRE(run.OpaqueCount == 2);
		CHECK(run.OpaqueCasters == 1);

		// The caster first, which is what makes `OpaqueCasters` a count from
		// `OpaqueFirst` rather than a number the pass has to search for.
		CHECK(instances[order[run.OpaqueFirst]].CastShadow);
		CHECK_FALSE(instances[order[run.OpaqueFirst + 1]].CastShadow);
	}
}

// An index past the cap is dropped rather than written past the end of the
// array. A scene may name any number it likes — `BasePart::Surface` takes an
// `int32_t` from a script — and the arrays this indexes are sized by the
// renderer's texture budget, not by what a script can express.
TEST_CASE("a surface index past the cap is dropped", "[scene][drawinstance]") {
	std::vector<DrawInstance> instances(2);
	instances[0].Surface = static_cast<int8_t>(MAX_SURFACES);
	instances[1].Surface = 0;

	std::vector<uint32_t> order;
	const ScenePlan plan = OrderScene(instances, Vector3::Zero, order);

	// Both are still mirrors by the partition — it tests `Surface >= 0` — so the
	// dropped one occupies a slot in the run and simply belongs to no group.
	CHECK(plan.Surfaces == 2);
	CHECK(plan.Runs[0].OpaqueCount == 1);
}

TEST_CASE("a signature is stable for a list that has not changed", "[scene][drawinstance]") {
	// **The property the whole thing rests on.** `render::Renderer` skips a
	// surface pass when this number matches the one its texture was drawn with,
	// so a signature that drifted on its own would not fail — it would silently
	// render every mirror every frame and look exactly like no optimisation at
	// all.
	std::vector<DrawInstance> instances(3);
	instances[0].Frame = CFrame(Vector3(1.0f, 2.0f, 3.0f));
	instances[1].Frame = CFrame(Vector3(-4.0f, 0.5f, 9.0f));
	instances[1].Surface = 2;
	instances[2].Mesh = Name("drawinstance_test.Pane");
	instances[2].Transparency = 0.5f;

	const uint64_t first = engine::scene::SignatureOf(instances);
	CHECK(engine::scene::SignatureOf(instances) == first);

	// A separately built list with the same values hashes the same. Equality is
	// over what a list *says*, not over where it lives.
	std::vector<DrawInstance> twin(instances.begin(), instances.end());
	CHECK(engine::scene::SignatureOf(twin) == first);
}

TEST_CASE("a signature ignores the bytes that carry no meaning", "[scene][drawinstance]") {
	// `Reserved` is explicit padding. It exists so the object representation is
	// deterministic the day a world crosses a process, and it says nothing about
	// what is drawn — so a signature that moved with it would be depending on
	// padding by name.
	DrawInstance instance;
	instance.Frame = CFrame(Vector3(3.0f, 1.0f, -2.0f));
	instance.Mesh = Name("drawinstance_test.Wall");
	instance.Surface = 3;

	const uint64_t before = engine::scene::SignatureOf(std::span(&instance, 1));

	// **One byte, because `Reserved` is one byte.** This wrote two until v0.10,
	// which was out of bounds the whole time and landed in whatever followed the
	// object — harmless by luck until removing `Material` moved the field and the
	// stack canary caught it. The array's own comment says why there is one:
	// `Alpha` took the second.
	instance.Reserved[0] = 0xAB;
	CHECK(engine::scene::SignatureOf(std::span(&instance, 1)) == before);

	// **The type is packed today, and that is why this is an assert rather than
	// a claim in a comment.** `core::Name` is a four-byte id and `Color3` ends
	// four-aligned, so there is no interior hole and a byte-wise hash would in
	// fact agree with the field-wise one on every list anybody can build right
	// now. That is a property of the current field order, not a guarantee: one
	// `double`, one pointer or one reordering opens a hole, and a byte reader
	// would then be folding in whatever the draw list's allocation last held —
	// never matching, so every surface renders every frame and the skip silently
	// stops working. The signature is field-wise and cannot develop that; this
	// assert is here so the next person to widen the type is told which
	// assumption they just changed.
	static_assert(
		sizeof(DrawInstance) == offsetof(DrawInstance, Reserved) + sizeof(DrawInstance::Reserved),
		"DrawInstance has grown padding. scene::SignatureOf is field-wise and is unaffected, "
		"but anything reading this type as bytes is now reading uninitialised memory."
	);
}

TEST_CASE("every field a surface can see moves the signature", "[scene][drawinstance]") {
	// **One case per field, because the failure is per field.** A signature
	// that missed `Tint` would hold a mirror's image through a recolour, and
	// nothing else in the frame would look wrong — the pane would simply keep
	// reflecting the old colour until something unrelated moved.
	const DrawInstance base;
	const uint64_t unchanged = engine::scene::SignatureOf(std::span(&base, 1));

	auto moved = [&](auto edit) {
		DrawInstance instance = base;
		edit(instance);
		return engine::scene::SignatureOf(std::span(&instance, 1));
	};

	CHECK(moved([](DrawInstance &i) { i.Frame = CFrame(Vector3(0.0f, 0.0f, 1.0f)); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.HalfExtent = Vector3(1.0f, 0.5f, 0.5f); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Tint = engine::core::Color3{1.0f, 1.0f, 0.0f}; }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Transparency = 0.5f; }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Mesh = Name("drawinstance_test.Other"); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Texture = Name("drawinstance_test.Steel"); }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.Surface = 0; }) != unchanged);
	CHECK(moved([](DrawInstance &i) { i.CastShadow = false; }) != unchanged);

	// A rotation with the same position, because the quaternion is four floats
	// that a position-only hash would miss entirely — and a mirror on a
	// turntable is exactly the case that would expose it.
	CHECK(moved([](DrawInstance &i) { i.Frame = CFrame::Angles(0.0f, 1.0f, 0.0f); }) != unchanged);
}

TEST_CASE("a signature depends on how many instances there are and their order", "[scene][drawinstance]") {
	// Order counts because the blended tail is drawn in it: two panes swapping
	// places changes which is composited over which, so it is a different image
	// from the same set of instances.
	std::vector<DrawInstance> instances(2);
	instances[0].Frame = CFrame(Vector3(1.0f, 0.0f, 0.0f));
	instances[1].Frame = CFrame(Vector3(2.0f, 0.0f, 0.0f));

	const uint64_t forward = engine::scene::SignatureOf(instances);

	std::swap(instances[0], instances[1]);
	CHECK(engine::scene::SignatureOf(instances) != forward);

	// And a list is not its own prefix. Dropping the last instance has to
	// register, or a mirror would hold its image through something leaving the
	// scene.
	instances.pop_back();
	CHECK(engine::scene::SignatureOf(instances) != forward);

	// An empty list is a real value rather than a sentinel, and pointedly not
	// zero: zero is what an uninitialised slot holds, and a surface that had
	// never rendered would otherwise match an empty scene and skip its first
	// pass.
	CHECK(engine::scene::SignatureOf({}) != 0u);
}

TEST_CASE("mixing folds a value in without discarding what came before", "[scene][drawinstance]") {
	// The renderer adds its own terms — a projection matrix, an opacity — on top
	// of the list's signature, so this has to be a fold rather than a reset.
	const uint64_t base = engine::scene::SignatureOf({});

	CHECK(engine::scene::MixSignature(base, 1u) != base);
	CHECK(engine::scene::MixSignature(base, 1u) != engine::scene::MixSignature(base, 2u));

	// Order within the fold matters, which is what lets the renderer mix one
	// surface's matrix apart from another's rather than into an unordered sum.
	CHECK(
		engine::scene::MixSignature(engine::scene::MixSignature(base, 1u), 2u) !=
		engine::scene::MixSignature(engine::scene::MixSignature(base, 2u), 1u)
	);
}

// --- what is ready to be drawn --------------------------------------------------
//
// **The rule that decides whether a mesh part appears at all**, and it lives
// here rather than in the renderer for `OrderScene`'s reason: a renderer is the
// one module a test cannot exercise, so a rule that decides what reaches a draw
// call is the last place it should live.

TEST_CASE("an instance naming an absent mesh is not drawn", "[scene][drawinstance]") {
	const engine::core::Name loaded("tree.amesh");
	const engine::core::Name missing("rock.amesh");

	std::vector<DrawInstance> instances(3);
	instances[0].Mesh = loaded;

	// **No mesh named at all — an ordinary `Part`.** This one is kept, because
	// the renderer's default cube is what a part *is* rather than a stand-in for
	// something that has not arrived.
	instances[1].Mesh = engine::core::Name{};

	instances[2].Mesh = missing;

	std::vector<DrawInstance> drawable;
	engine::scene::KeepLoaded(
		instances, [&loaded](const engine::core::Name &mesh) { return mesh == loaded; }, drawable
	);

	REQUIRE(drawable.size() == 2);
	CHECK(drawable[0].Mesh == loaded);
	CHECK_FALSE(drawable[1].Mesh.IsValid());

	// **The distinction is the whole point.** A version that dropped both — or
	// kept both — would pass a test that only counted, so this asserts which two
	// survived and that the absent one is not among them.
	for (const DrawInstance &instance : drawable) {
		CHECK(instance.Mesh != missing);
	}
}

TEST_CASE("a mesh arriving makes its parts appear without anything else changing", "[scene][drawinstance]") {
	const engine::core::Name wanted("tree.amesh");

	std::vector<DrawInstance> instances(4);
	for (DrawInstance &instance : instances) {
		instance.Mesh = wanted;
	}

	std::vector<DrawInstance> drawable;

	// Before the content lands, nothing naming it draws — which is what makes a
	// half-loaded scene read as "still loading" rather than as a field of cubes.
	engine::scene::KeepLoaded(instances, [](const engine::core::Name &) { return false; }, drawable);
	CHECK(drawable.empty());

	// And after, every one of them does, in the order the world produced them.
	engine::scene::KeepLoaded(instances, [](const engine::core::Name &) { return true; }, drawable);
	CHECK(drawable.size() == instances.size());
}

TEST_CASE("filtering keeps its buffer across calls", "[scene][drawinstance]") {
	// **Cleared and refilled rather than rebuilt**, because this runs once a
	// frame over every drawable in the world — the rule every buffer in the
	// render path follows.
	std::vector<DrawInstance> instances(64);
	for (DrawInstance &instance : instances) {
		// **Named, because an unnamed one is kept whatever the residency
		// answer** — the fixture has to be able to empty out for the capacity
		// check below to mean anything.
		instance.Mesh = engine::core::Name("tree.amesh");
	}
	std::vector<DrawInstance> drawable;

	engine::scene::KeepLoaded(instances, [](const engine::core::Name &) { return true; }, drawable);
	const size_t capacity = drawable.capacity();
	REQUIRE(capacity >= 64);

	engine::scene::KeepLoaded(instances, [](const engine::core::Name &) { return false; }, drawable);
	CHECK(drawable.empty());
	CHECK(drawable.capacity() == capacity);

	engine::scene::KeepLoaded(instances, [](const engine::core::Name &) { return true; }, drawable);
	CHECK(drawable.size() == 64);
	CHECK(drawable.capacity() == capacity);
}
