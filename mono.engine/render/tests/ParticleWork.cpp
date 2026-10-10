#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ParticleWork.hpp>
#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.render.particlework")
TEST_DEPENDS("engine.graph.frustum")

using Catch::Approx;
using engine::core::Vector3;
using engine::render::BoundsForParticleDraw;
using engine::render::ParticleCullRecord;
using engine::render::ParticleDrawPlanStamp;
using engine::render::ParticleDrawVisible;
using engine::render::ParticleStepDelta;
using engine::render::ParticleWorkgroups;
using engine::render::ParticleWorkItem;

TEST_CASE("particle work items match the shader's two-word resident layout", "[render][particles]") {
	const ParticleWorkItem item{71, 4098};
	const auto *words = reinterpret_cast<const uint32_t *>(&item);

	STATIC_REQUIRE(sizeof(ParticleWorkItem) == sizeof(uint32_t) * 2);
	CHECK(words[0] == 71);
	CHECK(words[1] == 4098);
}

TEST_CASE("small emitters share full compute groups instead of padding per emitter", "[render][particles]") {
	constexpr uint32_t emitters = 102'400;
	constexpr uint32_t slotsPerEmitter = 6;
	constexpr uint32_t workItems = emitters * slotsPerEmitter;

	CHECK(ParticleWorkgroups(workItems) == 9'600);
	CHECK(ParticleWorkgroups(workItems) < emitters / 10);
	CHECK(ParticleWorkgroups(0) == 0);
	CHECK(ParticleWorkgroups(65) == 2);
}

TEST_CASE("a particle presentation consumes its accumulated simulation time", "[render][particles]") {
	CHECK(ParticleStepDelta(1.0f / 60.0f, 0.0f) == 1.0f / 60.0f);
	CHECK(ParticleStepDelta(1.0f / 60.0f, 0.025f) == 0.025f + 1.0f / 60.0f);
	CHECK(ParticleStepDelta(0.0f, 0.0f) == 0.0f);
}

TEST_CASE("particle draw plan survives device steps until its host bound expires", "[render][particles]") {
	const glm::mat4 camera{1.0f};
	ParticleDrawPlanStamp stamp{
		.ViewProjection = camera,
		.LayoutRevision = 7,
		.ResidentRevision = 11,
		.RebuildAfter = 4.0,
		.CullingSafe = true,
		.Valid = true,
	};

	CHECK(stamp.Reusable(camera, 7, 11, 3.999, true));
	CHECK_FALSE(stamp.Reusable(camera, 7, 11, 4.0, true));
	CHECK_FALSE(stamp.Reusable(glm::mat4{2.0f}, 7, 11, 3.0, true));
	CHECK_FALSE(stamp.Reusable(camera, 8, 11, 3.0, true));
	CHECK_FALSE(stamp.Reusable(camera, 7, 12, 3.0, true));
	CHECK_FALSE(stamp.Reusable(camera, 7, 11, 3.0, false));
}

TEST_CASE("particle draw bounds include spawn motion and billboard reach", "[render][particles][culling]") {
	engine::effects::EmitterBlock block;
	block.Frame.Position = Vector3{10.0f, 20.0f, 30.0f};
	block.Acceleration = Vector3{0.0f, -2.0f, 0.0f};
	block.Curves.Size[0] = 4.0f;
	block.Curves.Squash[0] = 1.0f;

	engine::effects::EmitterSpawnState spawn;
	spawn.Half = Vector3{1.0f, 2.0f, 2.0f};
	spawn.Inherited = Vector3{3.0f, 0.0f, 0.0f};
	spawn.Speed = engine::core::NumberRange{1.0f, 5.0f};
	spawn.Lifetime = engine::core::NumberRange{1.0f, 2.0f};

	const auto bound = BoundsForParticleDraw(block, spawn, 0.5f);
	REQUIRE(bound.Cullable);
	CHECK(bound.Bounds.Centre().X == Approx(block.Frame.Position.X));
	CHECK(bound.Bounds.Centre().Z == Approx(block.Frame.Position.Z));

	// Spawn radius 3, speed reach 16, an 8-by-2 billboard's half diagonal,
	// and the camera-relative offset expand both sides. Downward acceleration
	// expands only the lower side.
	const float symmetric = 3.0f + 16.0f + std::sqrt(17.0f) + 0.5f;
	CHECK(bound.Bounds.Size().X == Approx(symmetric * 2.0f).margin(1e-4f));
	CHECK(bound.Bounds.Maximum.Y == Approx(20.0f + symmetric).margin(1e-4f));
	CHECK(bound.Bounds.Minimum.Y == Approx(20.0f - symmetric - 8.0f).margin(1e-4f));
}

TEST_CASE("particle draw bounds refuse unsafe authored motion", "[render][particles][culling]") {
	engine::effects::EmitterBlock block;
	engine::effects::EmitterSpawnState spawn;
	spawn.Lifetime = engine::core::NumberRange{1.0f};

	block.Drag = -0.1f;
	CHECK_FALSE(BoundsForParticleDraw(block, spawn, 0.0f).Cullable);

	block.Drag = 0.0f;
	block.NoiseStrength = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(BoundsForParticleDraw(block, spawn, 0.0f).Cullable);
}

TEST_CASE("particle group culling rejects only safe off-camera bounds", "[render][particles][culling]") {
	engine::scene::Camera camera;
	camera.NearPlane = 0.1f;
	camera.FarPlane = 100.0f;
	const engine::graph::Frustum frustum = engine::graph::Frustum::FromViewProjection(
		engine::scene::ResolveCamera(engine::core::CFrame{}, camera, 16.0f / 9.0f).ViewProjection
	);
	const engine::core::AABB behind =
		engine::core::AABB::FromCentre(Vector3{0.0f, 0.0f, 10.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const engine::core::AABB ahead =
		engine::core::AABB::FromCentre(Vector3{0.0f, 0.0f, -10.0f}, Vector3{1.0f, 1.0f, 1.0f});

	CHECK_FALSE(ParticleDrawVisible(behind, true, true, frustum));
	CHECK(ParticleDrawVisible(ahead, true, true, frustum));
	CHECK(ParticleDrawVisible(behind, false, true, frustum));
	CHECK(ParticleDrawVisible(behind, true, false, frustum));
}

TEST_CASE(
	"changed emitter bounds become cullable after old particles expire", "[render][particles][culling]"
) {
	ParticleCullRecord record;
	const engine::render::ParticleDrawBounds first{
		engine::core::AABB::FromCentre(Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}),
		true,
	};
	const engine::render::ParticleDrawBounds moved{
		engine::core::AABB::FromCentre(Vector3{100.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}),
		true,
	};

	record.Observe(first, 1, 1, 1, 1.0f, 0.0);
	CHECK(record.Ready(0.0));
	record.Observe(moved, 1, 2, 1, 1.0f, 4.0);
	CHECK_FALSE(record.Ready(4.999));
	CHECK(record.Ready(5.0));

	// Recycling changes the generation consumed by every particle row, so no
	// old tenancy survives and the replacement bound is safe immediately.
	record.Observe(first, 2, 3, 2, 10.0f, 5.0);
	CHECK(record.Ready(5.0));
}

TEST_CASE(
	"particle culling refines a material bound that surrounds the camera", "[render][particles][culling]"
) {
	engine::scene::Camera camera;
	camera.NearPlane = 0.1f;
	camera.FarPlane = 100.0f;
	const engine::graph::Frustum frustum = engine::graph::Frustum::FromViewProjection(
		engine::scene::ResolveCamera(engine::core::CFrame{}, camera, 16.0f / 9.0f).ViewProjection
	);
	const engine::core::AABB behind =
		engine::core::AABB::FromCentre(Vector3{0.0f, 0.0f, 10.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const engine::core::AABB ahead =
		engine::core::AABB::FromCentre(Vector3{0.0f, 0.0f, -10.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const engine::core::AABB material{
		behind.Minimum.Min(ahead.Minimum),
		behind.Maximum.Max(ahead.Maximum),
	};

	// The coarse material run intersects because it crosses the eye. Production
	// must continue to the emitter-sized bound, where the row behind the camera
	// is rejected instead of drawing the whole material run.
	CHECK(ParticleDrawVisible(material, true, true, frustum));
	CHECK_FALSE(ParticleDrawVisible(behind, true, true, frustum));
	CHECK(ParticleDrawVisible(ahead, true, true, frustum));
}

TEST_CASE(
	"particle work residency survives source revisions until its layout changes",
	"[render][particles][layout]"
) {
	using engine::render::ParticleBatch;
	using engine::render::ParticleLayoutMatches;
	using engine::render::ParticleWorkBlockOf;
	engine::effects::EmitterBlock block;
	block.First = 32;
	block.Capacity = 6;
	block.Generation = 1;
	block.Revision = 5;
	block.CurveRevision = 7;
	engine::effects::EmitterSpawnState spawn;
	engine::effects::EmitterRuntime runtime;
	ParticleBatch batch;
	batch.Block = &block;
	batch.Spawn = &spawn;
	batch.Runtime = &runtime;
	batch.Index = 3;
	batch.Texture = engine::core::Name("smoke");
	batch.SourceWorld = engine::core::Name("first-world");
	const std::array prepared{batch};
	const std::array work{ParticleWorkBlockOf(batch)};
	const auto matches = [&] { return ParticleLayoutMatches(std::span(&batch, 1), prepared, work); };
	REQUIRE(matches());

	SECTION("acceleration and curves preserve the work mapping") {
		block.Acceleration = {3, -9, 0};
		block.Revision++;
		block.Curves.Size[0] = 2;
		block.CurveRevision++;
		CHECK(matches());
	}
	SECTION("a copied source packet rebinds without another work upload") {
		auto copiedBlock = block;
		auto copiedSpawn = spawn;
		auto copiedRuntime = runtime;
		batch.Block = &copiedBlock;
		batch.Spawn = &copiedSpawn;
		batch.Runtime = &copiedRuntime;
		CHECK(matches());
	}
	SECTION("texture and generated-image owner change material groups") {
		batch.Texture = engine::core::Name("fire");
		CHECK_FALSE(matches());
		batch = prepared[0];
		batch.SourceWorld = engine::core::Name("second-world");
		CHECK_FALSE(matches());
	}
	SECTION("camera offset and soft masking change material groups") {
		batch.ZOffset = 3;
		CHECK_FALSE(matches());
		batch = prepared[0];
		batch.SoftParticles = !batch.SoftParticles;
		CHECK_FALSE(matches());
	}
	SECTION("block ranges and membership change work destinations") {
		block.First++;
		CHECK_FALSE(matches());
		block.First--;
		block.Capacity++;
		CHECK_FALSE(matches());
		block.Capacity--;
		batch.Index++;
		CHECK_FALSE(matches());
		batch = prepared[0];
		batch.Runtime = nullptr;
		CHECK_FALSE(matches());
		CHECK_FALSE(ParticleLayoutMatches({}, prepared, work));
	}
	SECTION("variable timing changes invalidate the resident timeline") {
		block.VariableFlipbookTiming = true;
		CHECK_FALSE(matches());
		block.VariableFlipbookTiming = false;
		block.Frames++;
		CHECK_FALSE(matches());
		block.Frames--;
		block.InvalidFlipbookTiming = true;
		CHECK_FALSE(matches());
	}
}

TEST_CASE(
	"particle culling bounds refresh only for their emitter's source changes", "[render][particles][culling]"
) {
	ParticleCullRecord record;
	const engine::render::ParticleDrawBounds bounds{{{-2, -3, -4}, {2, 3, 4}}, true};
	record.Observe(bounds, 1, 7, 11, 3, 10);
	CHECK(record.Matches(1, 7, 11));
	CHECK_FALSE(record.Matches(2, 7, 11));
	CHECK_FALSE(record.Matches(1, 8, 11));
	CHECK_FALSE(record.Matches(1, 7, 12));
	CHECK(record.Ready(10));
	record.Observe(bounds, 1, 8, 11, 3, 11);
	CHECK(record.Matches(1, 8, 11));
	CHECK_FALSE(record.Ready(13.999));
	CHECK(record.Ready(14));
}
