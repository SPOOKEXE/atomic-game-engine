#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
	CHECK_FALSE(instance.Material.IsValid());
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
	visual.Material = Name("drawinstance_test.Oak");
	visual.Tint = engine::core::Color3(0.25f, 0.5f, 0.75f);

	const DrawInstance instance{
		transform.Frame, bounds.HalfExtent, visual.Tint, visual.Mesh, visual.Material
	};

	CHECK(instance.Frame.Position == transform.Frame.Position);
	CHECK(instance.HalfExtent == bounds.HalfExtent);
	CHECK(instance.Tint.G == 0.5f);
	CHECK(instance.Mesh == visual.Mesh);
	CHECK(instance.Material == visual.Material);
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
