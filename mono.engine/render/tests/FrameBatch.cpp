// FrameBatch owns the partial-submit path, so these cases use a real headless
// device instead of replacing Renderer with a fake that cannot hold a command.

#include "FrameBatch.hpp"

#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"

#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>

TEST_SUITE_ID("engine.render.framebatch")

namespace {
	using namespace engine;

	render::View ViewFor(render::SceneTarget &target, uint64_t world, size_t slot) {
		render::View view;
		view.Target = &target;
		view.World = world;
		view.WorldName = core::Name("frame-batch-world-" + std::to_string(world));
		view.SnapshotId = "frame-batch-snapshot-" + std::to_string(world);
		view.Slot = slot;
		view.Camera.NearPlane = 0.1f;
		view.Camera.FarPlane = 100.0f;
		return view;
	}

#if ENGINE_ASSERTS_ENABLED
	bool SameLighting(const scene::WorldLighting &left, const scene::WorldLighting &right) {
		return left.Direction.X == right.Direction.X && left.Direction.Y == right.Direction.Y &&
			   left.Direction.Z == right.Direction.Z && left.Ambient.R == right.Ambient.R &&
			   left.Ambient.G == right.Ambient.G && left.Ambient.B == right.Ambient.B &&
			   left.Direct.R == right.Direct.R && left.Direct.G == right.Direct.G &&
			   left.Direct.B == right.Direct.B;
	}

	struct ResetFrameBatchFailure {
		~ResetFrameBatchFailure() {
			render::test_support::SetFrameBatchFailureBeforeGroupForTests(std::nullopt);
		}
	};
#endif
}

TEST_CASE("FrameBatch rejects an invalid earlier offscreen view before acquisition", "[render][gpu][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::OverlayImage overlay;
	render::SceneTarget valid{19, 13};
	render::SceneTarget invalid{};
	std::array views{ViewFor(invalid, 2, 0), ViewFor(valid, 1, 1)};

	const render::FrameBatchResult skipped =
		render::FrameBatch(fixture.Render).Run(views, overlay, nullptr, false, nullptr);
	CHECK(skipped.Outcome == render::FrameBatchOutcome::SkippedBeforeAcquisition);
	CHECK_FALSE(skipped.Frame.Submitted);
	CHECK_FALSE(fixture.Render.Visibility().Valid);

	const render::FrameBatchResult recovered =
		render::FrameBatch(fixture.Render).Run(std::span(views).last(1), overlay, nullptr, false, nullptr);
	CHECK(recovered.Outcome == render::FrameBatchOutcome::Submitted);
	CHECK(recovered.Frame.Submitted);
}

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"FrameBatch discards a failed group after acquisition and restores batch-owned state", "[render][gpu][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	render::OverlayImage overlay;
	render::SceneTarget firstTarget{23, 17};
	render::SceneTarget secondTarget{23, 17};
	std::array views{ViewFor(firstTarget, 11, 0), ViewFor(secondTarget, 12, 1)};

	scene::WorldLighting baseline;
	baseline.Direction = {0.0f, -1.0f, 0.0f};
	baseline.Ambient = {0.1f, 0.2f, 0.3f};
	baseline.Direct = {0.4f, 0.5f, 0.6f};
	fixture.Render.SetLighting(baseline);
	views[0].OverrideLighting = true;
	views[0].Lighting = baseline;
	views[0].Lighting.Ambient = {0.9f, 0.8f, 0.7f};

	const auto pipeline = fixture.Render.ResolvePipelineIdentity({});
	REQUIRE(pipeline);
	const render::ViewMutationIdentity identity{
		.WorldName = std::string(views[0].WorldName.Text()),
		.SnapshotId = views[0].SnapshotId,
		.Pipeline = pipeline->Name,
		.PipelineRevision = pipeline->Revision,
		.ViewSlot = views[0].Slot,
	};
	const render::HookHandle cameraHook = fixture.Render.Hooks().FindHook(core::Name("view.camera"));
	REQUIRE(cameraHook.IsValid());
	const render::ArmViewMutationResult mutation = fixture.Render.Hooks().ArmViewMutation(
		cameraHook,
		{.Identity = identity,
		 .CameraFrame = core::CFrame(core::Vector3{3.0f, 2.0f, 1.0f}),
		 .Camera = std::nullopt,
		 .Projection = std::nullopt}
	);
	REQUIRE(mutation.Status == render::HookBindStatus::Ok);

	ResetFrameBatchFailure reset;
	render::test_support::SetFrameBatchFailureBeforeGroupForTests(1);
	const render::FrameBatchResult failed =
		render::FrameBatch(fixture.Render).Run(views, overlay, nullptr, false, nullptr);
	CHECK(failed.Outcome == render::FrameBatchOutcome::SubmittedAfterViewFailure);
	CHECK(failed.Frame.Submitted);
	CHECK_FALSE(fixture.Render.Visibility().Valid);
	CHECK(
		fixture.Render.Hooks().PollViewMutation(mutation.Mutation).Status ==
		render::ViewMutationStatus::AppliedAwaitingRestore
	);
	CHECK(SameLighting(fixture.Render.CurrentLighting(), baseline));

	render::test_support::SetFrameBatchFailureBeforeGroupForTests(std::nullopt);
	const render::FrameBatchResult recovered =
		render::FrameBatch(fixture.Render).Run(std::span(views).first(1), overlay, nullptr, false, nullptr);
	CHECK(recovered.Outcome == render::FrameBatchOutcome::Submitted);
	CHECK(recovered.Frame.Submitted);
	CHECK(fixture.Render.Visibility().Valid);
	CHECK(
		fixture.Render.Hooks().PollViewMutation(mutation.Mutation).Status ==
		render::ViewMutationStatus::Applied
	);
	CHECK(SameLighting(fixture.Render.CurrentLighting(), baseline));
}
#endif
