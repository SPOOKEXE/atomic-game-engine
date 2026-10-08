#include "../src/nodes/SourceParticle2DState.hpp"

#include "../src/nodes/SourceParticle2DReplay.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_particle_2d_state")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
namespace source_particle2d_state_test {
	struct Run {
		Node Authored{"particles", "pc.particle", "", {}, {}};
		EvaluationRequest Request;
		NodeContext Context{Authored, *FindCatalogueEntry(Authored.Type), Request};
		SourceParticle2DControls Controls;
		SourceParticle2DState State;
		std::array<Vector2, 1> Positions{{{4, 4}}};
		Gradient White{0, {{0, {255, 255, 255, 255}}}};
		Run() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			Controls.PoolCapacity = 4;
			Controls.TotalFrames = 8;
			Controls.Loop = false;
			Controls.SpawnSource = 4;
			Controls.SpawnData = Positions;
			Controls.Distribution = 0;
			Controls.SpawnType = 1;
			Controls.SpawnDelay = 0;
			Controls.SpawnAmount = {1, 1};
			Controls.Lifespan = {2, 2};
			Controls.Direction = {{0, 0, 0, 0, 0, 0}, 6};
			Controls.Speed = {1, 1};
			Controls.SpawnColour = Controls.LifetimeColour = &White;
		}
		bool Begin() {
			return BeginSourceParticle2DState(Context, Controls, State);
		}
		bool Step(int64_t frame) {
			return AdvanceSourceParticle2DState(Context, Controls, State, frame);
		}
	};
}
using namespace source_particle2d_state_test;
TEST_CASE(
	"Particle 2D newborn movement and postdecrement lifespan retain the real source pool",
	"[imagegraph][particle2d]"
) {
	Run run;
	REQUIRE(run.Begin());
	CHECK(run.State.Frame == -1);
	REQUIRE(run.Step(0));
	CHECK(run.State.SpawnTotal == 1);
	CHECK(run.State.Slots[0].Life == 1);
	CHECK(run.State.Slots[0].Data.State.Position[0] == 5);
	CHECK_FALSE(run.State.Slots[0].Data.State.SpriteSlot);
	REQUIRE(run.Step(1));
	CHECK(run.State.Slots[0].Life == 0);
	CHECK(run.State.Slots[0].Data.State.Position[0] == 6);
	REQUIRE(run.Step(2));
	CHECK(run.State.Slots[0].Life == -1);
	CHECK(run.State.Slots[0].Data.State.Active);
	REQUIRE(run.Step(3));
	CHECK_FALSE(run.State.Slots[0].Data.State.Active);
	CHECK(run.State.Slots[0].Data.State.XHistory[0] == 5);
	CHECK(run.State.Slots[0].Data.State.XHistory[2] == 7);
}
TEST_CASE(
	"Particle 2D receipt resumes independent velocity and rejects replay frame gaps",
	"[imagegraph][particle2d]"
) {
	Run run;
	run.Controls.Physics = true;
	run.Controls.Acceleration = {1, 1};
	REQUIRE(run.Begin());
	REQUIRE(run.Step(0));
	StructValue receipt;
	AllocationReservation charge;
	REQUIRE(EncodeSourceParticle2DReceipt(run.Context, run.State, receipt, charge));
	Run resumed;
	REQUIRE(DecodeSourceParticle2DReceipt(resumed.Context, receipt, resumed.State));
	REQUIRE(resumed.Step(1));
	CHECK(resumed.State.Slots[0].Data.State.Position[0] == 7);
	CHECK(resumed.State.Slots[0].Velocity[0] == 3);
	CHECK_FALSE(resumed.Step(3));
	CHECK(resumed.Context.FailureCode == Status::UnsupportedExecution);
}
TEST_CASE(
	"Particle 2D trigger circular reuse and empty-array sprites preserve source spawn distinction",
	"[imagegraph][particle2d]"
) {
	Run run;
	run.Controls.SpawnType = 2;
	run.Controls.Trigger = true;
	REQUIRE(run.Begin());
	for (int frame = 0; frame < 6; ++frame)
		REQUIRE(run.Step(frame));
	CHECK(run.State.SpawnTotal == 6);
	CHECK(run.State.Runner == 2);
	CHECK(run.State.SpawnIndex == 2);
	CHECK(run.State.Slots[1].Life == 1);
	Run empty;
	empty.Controls.SpriteArray = empty.Controls.SpriteEmptyArray = true;
	REQUIRE(empty.Begin());
	REQUIRE(empty.Step(0));
	CHECK(empty.State.SpawnTotal == 0);
}
TEST_CASE(
	"Particle 2D ground bounce precedes acceleration and retained line histories outlive active particles",
	"[imagegraph][particle2d]"
) {
	Run run;
	run.Controls.Direction = {{0, -90, -90, 0, 0, 0}, 6};
	run.Controls.Ground = true;
	run.Controls.GroundOffset = {0, 0};
	run.Controls.BounceAmount = .5;
	run.Controls.RenderType = 1;
	REQUIRE(run.Begin());
	REQUIRE(run.Step(0));
	CHECK(run.State.Slots[0].Data.State.Position[1] == 4);
	CHECK(run.State.Slots[0].Velocity[1] == Catch::Approx(-.5));
	REQUIRE(run.Step(1));
	CHECK(run.State.Slots[0].Data.State.Position[1] == Catch::Approx(3.5));
	CHECK(run.State.Slots[0].BlendHistory[0] == 0xffffffff);
	CHECK(run.State.Slots[0].ScaleXHistory[0] == 1);
	REQUIRE(run.Step(2));
	REQUIRE(run.Step(3));
	CHECK_FALSE(run.State.Slots[0].Data.State.Active);
	CHECK(run.State.Slots[0].TrailLife == 4);
}
TEST_CASE(
	"Particle 2D curve maps use timeline precision and source fast lookup", "[imagegraph][particle2d]"
) {
	Run run;
	Curve curve;
	curve.Header = {0, 1, 0, 0, 1, 0};
	curve.Anchors = {{0, 0, 0, 0, 1. / 3, 1. / 3}, {-1. / 3, -1. / 3, 1, 1, 0, 0}};
	run.Controls.SpeedCurve = &curve;
	REQUIRE(run.Begin());
	REQUIRE(run.State.Curves[0].size() == 9);
	CHECK(SourceParticle2DCurveAt(run.State, 0, .2) == Catch::Approx(.125));
	CHECK(SourceParticle2DCurveAt(run.State, 0, .99) == Catch::Approx(.875));
}
