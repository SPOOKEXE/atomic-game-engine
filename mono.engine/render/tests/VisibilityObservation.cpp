#include <engine/render/VisibilityObservation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.visibilityobservation")

namespace {
	engine::scene::DrawInstance Instance(uint64_t entity, float transparency = 0.0f) {
		engine::scene::DrawInstance instance;
		instance.Source = entity;
		instance.Transparency = transparency;
		return instance;
	}

	engine::scene::DrawInstance InstanceIn(engine::core::Name world, uint64_t entity) {
		auto instance = Instance(entity);
		instance.SourceWorld = world;
		return instance;
	}
}

TEST_CASE("visibility observations distinguish submission from missing evidence", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(7, 3, engine::core::Name("visibility.world"));

	observations.Observe(Instance(4));
	observations.Observe(Instance(8));
	observations.Submitted(Instance(8));
	observations.Observe(Instance(12, 0.25f));
	observations.Submitted(Instance(12, 0.25f));
	observations.Observe(Instance(16));
	observations.FrustumCulled(Instance(16));

	const auto snapshot = observations.Snapshot();
	REQUIRE(snapshot.Valid);
	CHECK(snapshot.Frame == 7);
	CHECK(snapshot.ViewSlot == 3);
	const auto &rows = snapshot.Observations;
	REQUIRE(rows.size() == 4);
	CHECK(rows[0].State == engine::render::VisibilityState::Unavailable);
	CHECK(rows[0].Cause == engine::render::VisibilityCause::Unavailable);
	CHECK(rows[1].State == engine::render::VisibilityState::SubmittedOpaqueOrMasked);
	CHECK(rows[1].Cause == engine::render::VisibilityCause::OpaqueOrMaskedDraw);
	CHECK(rows[2].State == engine::render::VisibilityState::SubmittedBlended);
	CHECK(rows[2].Cause == engine::render::VisibilityCause::BlendedDraw);
	CHECK(rows[3].State == engine::render::VisibilityState::FrustumCulled);
	CHECK(rows[3].Cause == engine::render::VisibilityCause::Frustum);
}

TEST_CASE("visibility observations retain first source order up to their bound", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("visibility.world"));
	for (uint64_t entity = 1; entity <= engine::render::MAX_VISIBILITY_OBSERVATIONS + 1; entity++) {
		observations.Observe(Instance(entity));
	}

	const auto snapshot = observations.Snapshot();
	const auto &rows = snapshot.Observations;
	REQUIRE(snapshot.Valid);
	REQUIRE(rows.size() == engine::render::MAX_VISIBILITY_OBSERVATIONS);
	CHECK(rows.front().Entity == 1);
	CHECK(rows.back().Entity == engine::render::MAX_VISIBILITY_OBSERVATIONS);
	CHECK(snapshot.Dropped == 1);
}

TEST_CASE("overflow rows are counted once before later visibility updates", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("visibility.world"));
	for (uint64_t entity = 1; entity <= engine::render::MAX_VISIBILITY_OBSERVATIONS + 1; entity++)
		observations.Observe(Instance(entity));
	observations.Observe(Instance(engine::render::MAX_VISIBILITY_OBSERVATIONS + 1));
	observations.FrustumCulled(Instance(engine::render::MAX_VISIBILITY_OBSERVATIONS + 1));
	observations.Submitted(Instance(engine::render::MAX_VISIBILITY_OBSERVATIONS + 1));
	CHECK(observations.Snapshot().Dropped == 1);
}

TEST_CASE("visibility observations keep variants together and worlds apart", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("view.world"));
	const auto first = InstanceIn(engine::core::Name("first.world"), 8);
	auto variant = first;
	variant.Variant = 4;
	const auto second = InstanceIn(engine::core::Name("second.world"), 8);
	observations.Observe(first);
	observations.Observe(variant);
	observations.GpuIndirectCandidate(variant);
	observations.Observe(second);
	observations.FrustumCulled(second);

	const auto snapshot = observations.Snapshot();
	REQUIRE(snapshot.Observations.size() == 2);
	CHECK(snapshot.Observations[0].World == engine::core::Name("first.world"));
	CHECK(snapshot.Observations[0].State == engine::render::VisibilityState::GpuIndirectCandidate);
	CHECK(snapshot.Observations[0].Cause == engine::render::VisibilityCause::GpuIndirectEligibility);
	CHECK(snapshot.Observations[1].World == engine::core::Name("second.world"));
	CHECK(snapshot.Observations[1].State == engine::render::VisibilityState::FrustumCulled);
	CHECK(snapshot.Observations[1].Cause == engine::render::VisibilityCause::Frustum);
}

TEST_CASE("authored opaque submission survives later occlusion eligibility", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("view.world"));
	const auto authoredOpaque = Instance(24);
	observations.Observe(authoredOpaque);
	observations.Submitted(authoredOpaque);
	observations.GpuIndirectCandidate(authoredOpaque);

	const auto snapshot = observations.Snapshot();
	const auto &row = snapshot.Observations.front();
	CHECK(row.State == engine::render::VisibilityState::SubmittedOpaqueOrMasked);
	CHECK(row.Cause == engine::render::VisibilityCause::OpaqueOrMaskedDraw);
}

TEST_CASE("exact frustum culling beats indirect eligibility until a main draw", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("view.world"));
	const auto candidate = Instance(25);
	observations.Observe(candidate);
	observations.GpuIndirectCandidate(candidate);
	observations.FrustumCulled(candidate);
	CHECK(
		observations.Snapshot().Observations.front().State == engine::render::VisibilityState::FrustumCulled
	);
	observations.Submitted(candidate);
	CHECK(
		observations.Snapshot().Observations.front().State ==
		engine::render::VisibilityState::SubmittedOpaqueOrMasked
	);
}

TEST_CASE("visibility observation overflow reports saturation honestly", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("overflow.world"));
	for (uint64_t entity = 1; entity <= engine::render::MAX_VISIBILITY_OBSERVATIONS * 3; entity++)
		observations.Observe(Instance(entity));

	const auto snapshot = observations.Snapshot();
	CHECK(snapshot.Dropped == engine::render::MAX_VISIBILITY_OBSERVATIONS);
	CHECK_FALSE(snapshot.DroppedExact);
}

TEST_CASE("overflow observations count distinct sources across repeated variants", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(1, 0, engine::core::Name("overflow.variants"));
	for (uint64_t entity = 1; entity <= engine::render::MAX_VISIBILITY_OBSERVATIONS; entity++)
		observations.Observe(Instance(entity));
	for (uint64_t entity = engine::render::MAX_VISIBILITY_OBSERVATIONS + 1;
		 entity <= engine::render::MAX_VISIBILITY_OBSERVATIONS + 2000;
		 entity++) {
		observations.Observe(Instance(entity));
		observations.Observe(Instance(entity));
	}
	const auto snapshot = observations.Snapshot();
	CHECK(snapshot.Dropped == 2000);
	CHECK(snapshot.DroppedExact);
}

TEST_CASE("visibility observations preserve completed empty submissions", "[render][visibility]") {
	engine::render::VisibilityObservations observations;
	observations.Begin(12, 5, engine::core::Name("empty.world"));

	const auto snapshot = observations.Snapshot();
	REQUIRE(snapshot.Valid);
	CHECK(snapshot.Frame == 12);
	CHECK(snapshot.ViewSlot == 5);
	CHECK(snapshot.World == engine::core::Name("empty.world"));
	CHECK(snapshot.Observations.empty());
	CHECK(snapshot.Dropped == 0);
}

TEST_CASE(
	"visibility snapshots are invalidated and share scene transparency semantics", "[render][visibility]"
) {
	engine::render::VisibilityObservations observations;
	observations.Begin(9, 2, engine::core::Name("first.world"));
	observations.Observe(Instance(7, 1.0f / 2048.0f));
	observations.Submitted(Instance(7, 1.0f / 2048.0f));
	observations.Submitted(Instance(7, 0.5f));

	const auto snapshot = observations.Snapshot();
	REQUIRE(snapshot.Observations.size() == 1);
	CHECK(snapshot.Observations.front().State == engine::render::VisibilityState::SubmittedBlended);

	observations.Invalidate();
	const auto invalid = observations.Snapshot();
	CHECK_FALSE(invalid.Valid);
	CHECK(invalid.Observations.empty());
}
