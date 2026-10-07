#include "../src/nodes/SourceParticle3DState.hpp"

#include "../src/nodes/Path3D.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_particle_3d_state")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace source_particle3d_state_test {
	struct Run {
		Node Authored{"particles", "pc.3_d_particle", "", {}, {}};
		EvaluationRequest Request;
		NodeContext Context{Authored, *FindCatalogueEntry(Authored.Type), Request};
		SourceParticle3DControls Controls;
		SourceParticle3DState State;
		Run() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			Controls.PoolCapacity = 6;
			Controls.Loop = false;
			Controls.SpawnSpan = {};
		}
		const SourceParticle3DSlot &Slot(size_t i) const {
			return State.Buffers[State.BufferIndex][i];
		}
	};
}
using namespace source_particle3d_state_test;

TEST_CASE(
	"Particle 3D source ping-pong lifespan, acceleration and reuse preserve source records",
	"[imagegraph][particle3d]"
) {
	Run run;
	run.Controls.Lifespan = {2, 2};
	run.Controls.Velocity = {1, 1, 0, 0, 0, 0};
	run.Controls.Acceleration = {1, 1, 0, 0, 0, 0};
	run.Controls.FollowVelocity = true;
	run.Controls.Billboard = true;
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.State.BufferIndex == 1);
	CHECK(run.State.SpawnIndex == 2);
	CHECK(SourceParticle3DDrawCount(run.State) == 1);
	CHECK(run.Slot(0).Particle.LifeTime == 1);
	CHECK(run.Slot(0).Transform.Fields[0] == 1);
	CHECK(run.Slot(1).Particle.MeshIndex == 1);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.State.BufferIndex == 0);
	CHECK(run.Slot(0).Transform.Fields[0] == 3);
	CHECK(run.Slot(0).Particle.Velocity[0] == 2);
	CHECK(run.Slot(0).Transform.Fields[12] == 2);
	CHECK(run.Slot(0).Particle.Active == 1);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 2, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == 7);
	CHECK(run.Slot(0).Particle.LifeTime == 3);
	CHECK(run.Slot(0).Particle.Active == 0);
	run.Controls.SpawnType = SourceParticle3DSpawnType::Trigger;
	run.Controls.Trigger = true;
	run.Controls.Billboard = false;
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 3, run.State));
	CHECK(run.Slot(0).Particle.MeshIndex == 2);
	CHECK(run.Slot(1).Particle.MeshIndex == 3);
	CHECK(run.Slot(0).Particle.LifeTime == 1);
	CHECK(run.Slot(0).Transform.Fields[0] == 1);
	CHECK(run.Slot(0).Particle.RenderFlags == 1);
	CHECK(run.Slot(0).Transform.Fields[12] == 4);
}

TEST_CASE(
	"Particle 3D exact gradients and nested byte multiplication preserve float alpha",
	"[imagegraph][particle3d]"
) {
	Run run;
	const Gradient lifetime{0, {{0, {128, 64, 32, 128}}}}, random{0, {{0, {128, 255, 255, 128}}}};
	const std::array<Colour, 1> palette{{{255, 128, 255, 128}}};
	run.Controls.LifetimeColour = &lifetime;
	run.Controls.RandomColour = &random;
	run.Controls.Palette = palette;
	run.Controls.Alpha = {.5, .5};
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	const auto &colour = run.Slot(0).Particle.Colour;
	CHECK(colour[0] == float(64 / 255.));
	CHECK(colour[1] == float(32 / 255.));
	CHECK(colour[2] == float(32 / 255.));
	CHECK(colour[3] == float(16 / 255.));
}

TEST_CASE(
	"Particle 3D draw-order oracle consumes random blend even with constant gradient",
	"[imagegraph][particle3d]"
) {
	Run run;
	run.Controls.Seed = 12345;
	run.Controls.Velocity = {0, 1, 0, 0, 0, 0};
	const std::array<Colour, 2> palette{{{255, 0, 0, 255}, {0, 0, 255, 255}}};
	run.Controls.Palette = palette;
	run.Controls.PaletteSelection = 2;
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	// Independent official HTML5 seed receipt: first Unit=.7990098701785364, second=.25161494698916326.
	CHECK(run.Slot(0).Transform.Fields[0] == float(.7990098701785364));
	CHECK((run.Slot(0).Particle.Colour == std::array<float, 4>{1, 0, 0, 1}));
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == float(double(float(.7990098701785364)) * 2));
	CHECK((run.Slot(0).Particle.Colour == std::array<float, 4>{1, 0, 0, 1}));
}

TEST_CASE("Particle 3D initial loop prerender resets only spawn index", "[imagegraph][particle3d]") {
	Run run;
	run.Controls.Loop = true;
	run.Controls.TotalFrames = 2;
	run.Controls.SpawnDelay = 1;
	run.Controls.SpawnAmount = {1, 1};
	run.Controls.Lifespan = {10, 10};
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.State.SpawnIndex == 1);
	CHECK(run.State.MaximumBufferIndex == 2);
	CHECK(SourceParticle3DDrawCount(run.State) == 2);
	CHECK(run.Slot(0).Particle.MeshIndex == 0);
	CHECK(run.Slot(1).Particle.MeshIndex == 1);
	CHECK(run.Slot(2).Particle.MeshIndex == 0);
	CHECK(run.Slot(0).Particle.LifeTime == 3);
	CHECK(run.Slot(1).Particle.LifeTime == 2);
	CHECK(run.Slot(2).Particle.LifeTime == 1);
	const auto before = run.State.Buffers;
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 0, run.State));
	CHECK(run.State.Buffers == before);
}

TEST_CASE(
	"Particle 3D ground bounce uses pre-motion position then speed-scaled rebound", "[imagegraph][particle3d]"
) {
	Run run;
	run.Controls.Ground = true;
	run.Controls.SpawnOrigin = {0, 0, .01};
	run.Controls.Velocity = {2, 2, 3, 3, -.1, -.1};
	run.Controls.BounceAmount = .5;
	run.Controls.BounceFriction = .1;
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == float(.2));
	CHECK(run.Slot(0).Transform.Fields[1] == float(.3));
	CHECK(run.Slot(0).Transform.Fields[2] == float(.05));
	CHECK(run.Slot(0).Particle.Velocity[2] == float(.05));
}

TEST_CASE(
	"Particle 3D samples curveMap rather than evaluating the curve continuously", "[imagegraph][particle3d]"
) {
	Run run;
	const Curve curve{{0, 1, 0, 0, 1, 0}, {{0, 0, 0, 0, 0, 1}, {0, 1, 1, 1, 0, 0}}};
	run.Controls.SpeedCurve = &curve;
	run.Controls.Lifespan = {3, 3};
	run.Controls.Velocity = {1, 1, 0, 0, 0, 0};
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == 0);
	// Source caches curveMap at initialization, even when the next frame changes the authored curve.
	const auto cached = run.State.CurveMaps;
	run.Controls.SpeedCurve = nullptr;
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.State.CurveMaps == cached);
	const double index = 32. / 3.;
	const double expected = cached[0][10] + (cached[0][11] - cached[0][10]) * (index - 10);
	CHECK(run.Slot(0).Transform.Fields[0] == float(expected));
}

TEST_CASE("Particle 3D direct and grouped mesh spawn preserve exact positions", "[imagegraph][particle3d]") {
	for (bool mesh : {false, true}) {
		Run run;
		const std::array<Vector3, 1> points{{{2, 3, 4}}};
		const std::array<std::span<const Vector3>, 1> groups{points};
		run.Controls.SpawnOrigin = {99, 99, 99};
		run.Controls.SpawnData = points;
		run.Controls.SpawnMeshVertices = groups;
		run.Controls.SpawnSource =
			mesh ? SourceParticle3DSpawnSource::MeshVertices : SourceParticle3DSpawnSource::DirectData;
		REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
		CHECK(run.Slot(0).Transform.Fields[0] == 2);
		CHECK(run.Slot(0).Transform.Fields[1] == 3);
		CHECK(run.Slot(0).Transform.Fields[2] == 4);
		CHECK((run.Slot(0).StartPosition == std::array<float, 4>{2, 3, 4, 0}));
	}
}

TEST_CASE(
	"Particle 3D failures retain prior state and obey exact ledger and processor budgets",
	"[imagegraph][particle3d]"
) {
	Run run;
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	const auto buffers = run.State.Buffers;
	const uint64_t bytes = run.State.Charge.Bytes();
	EvaluationBudget budget(bytes - 1);
	NodeContext limited{run.Authored, *FindCatalogueEntry(run.Authored.Type), run.Request, budget};
	limited.ByteBudget = Limits::MaximumEvaluationBytes;
	CHECK_FALSE(AdvanceSourceParticle3DState(limited, run.Controls, run.State, 1, run.State));
	CHECK(limited.FailureCode == Status::LimitExceeded);
	CHECK(budget.Used() == 0);
	CHECK(run.State.Buffers == buffers);
	run.Context.ProcessorCount = 100000;
	CHECK_FALSE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.Context.FailureCode == Status::LimitExceeded);
	CHECK(run.State.Buffers == buffers);
	run.Context.ProcessorCount = 1;
	run.Context.FailureCode = Status::Ok;
	CHECK_FALSE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 3, run.State));
	CHECK(run.Context.FailureCode == Status::UnsupportedExecution);
	CHECK(run.State.Buffers == buffers);
	run.Context.FailureCode = Status::Ok;
	run.Controls.SpawnSource = SourceParticle3DSpawnSource::DirectData;
	CHECK_FALSE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Context.FailureCode == Status::UnsupportedExecution);
	CHECK(run.State.Buffers == buffers);
	run.Context.FailureCode = Status::Ok;
	run.Controls.SpawnSource = SourceParticle3DSpawnSource::Shape;
	run.Controls.Lifespan = {0, 0};
	CHECK_FALSE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Context.FailureCode == Status::InvalidValue);
	CHECK(run.State.Buffers == buffers);
	CHECK(run.State.Charge.Bytes() == bytes);
}

TEST_CASE(
	"Particle 3D path spawn ignores origin and follow path uses saved spawn position",
	"[imagegraph][particle3d]"
) {
	Run run;
	PathData3D data;
	data.SourcePolyline = true;
	data.Resolution = 1;
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0, 0, 0, 0}, 1}};
	PathRuntime3D path(data, &run.Context);
	REQUIRE(path.Valid());
	run.Controls.Seed = 12345;
	run.Controls.SpawnSource = SourceParticle3DSpawnSource::Path;
	run.Controls.SpawnPath = &path;
	run.Controls.PathSampleWork = 32;
	run.Controls.SpawnOrigin = {99, 99, 99};
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == float(10 * .7990098701785364));
	CHECK(run.Slot(0).Transform.Fields[1] == 0);
	const float origin = run.Slot(0).StartPosition[0];
	run.Controls.Follow = true;
	run.Controls.FollowPath = &path;
	run.Controls.PathRange = {.5, .5, .5, .5};
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.Slot(0).Transform.Fields[0] == float(5 + double(origin)));
	CHECK(run.Slot(0).Particle.Velocity[0] == float(5 + double(origin) - double(origin)));
}

TEST_CASE(
	"Particle 3D shape spawn families preserve their source distributions", "[imagegraph][particle3d]"
) {
	for (auto shape :
		 {SourceParticle3DSpawnShape::Box,
		  SourceParticle3DSpawnShape::Sphere,
		  SourceParticle3DSpawnShape::Circle}) {
		Run run;
		run.Controls.SpawnShape = shape;
		run.Controls.SpawnSpan = {2, 2, 2};
		run.Controls.SpawnOrigin = {1, 2, 3};
		REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
		const auto &position = run.Slot(0).Transform.Fields;
		if (shape == SourceParticle3DSpawnShape::Circle) {
			CHECK(position[2] == 3);
			CHECK(
				(double(position[0]) - 1) * (double(position[0]) - 1) +
					(double(position[1]) - 2) * (double(position[1]) - 2) ==
				Catch::Approx(4).margin(1e-6)
			);
		} else if (shape == SourceParticle3DSpawnShape::Sphere) {
			CHECK(
				(double(position[0]) - 1) * (double(position[0]) - 1) +
					(double(position[1]) - 2) * (double(position[1]) - 2) +
					(double(position[2]) - 3) * (double(position[2]) - 3) <=
				4.000001
			);
		} else {
			CHECK(position[0] >= -1);
			CHECK(position[0] <= 3);
			CHECK(position[1] >= 0);
			CHECK(position[1] <= 4);
			CHECK(position[2] >= 1);
			CHECK(position[2] <= 5);
		}
	}
}

TEST_CASE("Particle 3D burst and trigger gates follow source frame intervals", "[imagegraph][particle3d]") {
	Run run;
	run.Controls.SpawnType = SourceParticle3DSpawnType::Burst;
	run.Controls.SpawnDelay = 2;
	run.Controls.BurstDuration = 2;
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.State.SpawnIndex == 0);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.State.SpawnIndex == 0);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 2, run.State));
	CHECK(run.State.SpawnIndex == 2);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 3, run.State));
	CHECK(run.State.SpawnIndex == 4);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 4, run.State));
	CHECK(run.State.SpawnIndex == 4);
	run.Controls.SpawnType = SourceParticle3DSpawnType::Trigger;
	run.Controls.Trigger = true;
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 5, run.State));
	CHECK(run.State.SpawnIndex == 6);
	CHECK(run.State.MaximumBufferIndex == 5);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 6, run.State));
	CHECK(run.State.SpawnIndex == 6);
}

TEST_CASE("Particle 3D gravity accumulates before velocity-scaled integration", "[imagegraph][particle3d]") {
	Run run;
	run.Controls.Physics = true;
	run.Controls.Gravity = {1, 1};
	REQUIRE(BeginSourceParticle3DState(run.Context, run.Controls, 0, run.State));
	CHECK(run.Slot(0).Transform.Fields[2] == -1);
	REQUIRE(AdvanceSourceParticle3DState(run.Context, run.Controls, run.State, 1, run.State));
	CHECK(run.Slot(0).Particle.Velocity[2] == -2);
	CHECK(run.Slot(0).Transform.Fields[2] == -3);
}
