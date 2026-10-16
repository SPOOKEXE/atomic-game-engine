#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_SUITE_ID("engine.scene.update-cadence")

using namespace engine::scene;

namespace {
	UpdateCadence TownPolicy() {
		UpdateCadence policy;
		policy.Identity = engine::core::Name("town.citizen.blacksmith");
		policy.Domains[static_cast<size_t>(UpdateDomain::Perception)] = {8, 12};
		policy.Domains[static_cast<size_t>(UpdateDomain::Behaviour)] = {4, 8};
		policy.Domains[static_cast<size_t>(UpdateDomain::VisualPose)] = {4, 8};
		policy.DemotionDelayTicks = 3;
		return policy;
	}

	bool Complete(DomainCadenceState &state, const CadenceDecision &decision) {
		return CommitUpdate(
			state, decision, decision.Tick, decision.SourceRevision, decision.PolicyRevision, true
		);
	}
}

TEST_CASE("cadence uses stable named slots and independent fixed tick domains", "[scene][cadence]") {
	const auto policy = TownPolicy();
	REQUIRE(ValidUpdateCadence(policy));
	CHECK(CadencePhase("hello", 1000) == 491);
	CHECK(CadencePhase("hello", 0) == 0);
	CHECK(UpdateDomainName(UpdateDomain::VisualPose) == "visual-pose");
	CHECK(UpdateDomainName(UpdateDomain::Count).empty());
	UpdateCadenceState state;
	for (size_t index = 0; index < UPDATE_DOMAIN_COUNT; ++index) {
		const auto domain = static_cast<UpdateDomain>(index);
		auto &committed = state.Domains[index];
		const auto first = CadenceDue(policy, committed, domain, 0, 1);
		REQUIRE(first.Due);
		CHECK(first.Reason == CadenceReason::FirstUpdate);
		REQUIRE(Complete(committed, first));
		CHECK_FALSE(CadenceDue(policy, committed, domain, 0, 1).Due);
		for (uint64_t tick = 1; tick < 80; ++tick) {
			const auto due = CadenceDue(policy, committed, domain, tick, 1);
			const auto interval = policy.Domains[index];
			CHECK(
				due.Due == (tick % interval.IntervalTicks ==
							CadencePhase(policy.Identity.Text(), interval.IntervalTicks))
			);
			CHECK(due.AgeTicks <= interval.MaximumStalenessTicks);
			if (due.Due) REQUIRE(Complete(committed, due));
		}
	}
}

TEST_CASE(
	"cadence rejects incomplete stale and duplicate results without moving its clock", "[scene][cadence]"
) {
	const auto policy = TownPolicy();
	DomainCadenceState state;
	const auto due = CadenceDue(policy, state, UpdateDomain::Perception, 7, 5);
	REQUIRE(due.Due);
	CHECK_FALSE(CommitUpdate(state, due, 7, 5, policy.Revision, false));
	CHECK_FALSE(CommitUpdate(state, due, 8, 5, policy.Revision, true));
	CHECK_FALSE(CommitUpdate(state, due, 7, 6, policy.Revision, true));
	CHECK_FALSE(CommitUpdate(state, due, 7, 5, policy.Revision + 1, true));
	CHECK(state.Generation == 0);
	CHECK(state.LastCommittedTick == 0);
	REQUIRE(Complete(state, due));
	CHECK_FALSE(Complete(state, due));
	CHECK(state.Generation == 1);
	CHECK(state.LastCommittedTick == 7);
	const auto stale = CadenceDue(policy, state, UpdateDomain::Perception, 25, 5);
	CHECK(stale.Due);
	CHECK(stale.Reason == CadenceReason::MaximumStaleness);
	CHECK(stale.AgeTicks == 18);
	REQUIRE(Complete(state, stale));
	CHECK(state.Generation == 2);
	CHECK(state.LastCommittedTick == 25);
	const auto changed = CadenceDue(policy, state, UpdateDomain::Perception, 25, 6);
	CHECK(changed.Due);
	CHECK(changed.Reason == CadenceReason::RevisionChanged);
	REQUIRE(Complete(state, changed));
	const auto reset = CadenceDue(policy, state, UpdateDomain::Perception, 1, 6);
	CHECK(reset.Due);
	CHECK(reset.Reason == CadenceReason::ClockReset);
	state.Generation = std::numeric_limits<uint64_t>::max();
	const auto exhausted = CadenceDue(policy, state, UpdateDomain::Perception, 30, 7);
	CHECK_FALSE(Complete(state, exhausted));
}

TEST_CASE(
	"combat promotion refreshes immediately and holds full fidelity before demoting", "[scene][cadence]"
) {
	auto policy = TownPolicy();
	DomainCadenceState state;
	REQUIRE(Complete(state, CadenceDue(policy, state, UpdateDomain::Behaviour, 10, 1)));
	const auto combat = static_cast<uint32_t>(UpdateCriticality::Combat);
	const auto promoted = CadenceDue(policy, state, UpdateDomain::Behaviour, 10, 1, combat);
	CHECK(promoted.Due);
	CHECK(promoted.Reason == CadenceReason::Critical);
	REQUIRE(Complete(state, promoted));
	CHECK_FALSE(CadenceDue(policy, state, UpdateDomain::Behaviour, 10, 1, combat).Due);
	const auto newlyOwned = CadenceDue(
		policy,
		state,
		UpdateDomain::Behaviour,
		10,
		1,
		combat | static_cast<uint32_t>(UpdateCriticality::PlayerOwned)
	);
	CHECK(newlyOwned.Due);
	CHECK(newlyOwned.Reason == CadenceReason::Critical);
	REQUIRE(Complete(state, newlyOwned));
	for (uint64_t tick = 11; tick <= 13; ++tick) {
		const auto held = CadenceDue(policy, state, UpdateDomain::Behaviour, tick, 1);
		CHECK(held.Due);
		CHECK(held.Reason == CadenceReason::DemotionHold);
		REQUIRE(Complete(state, held));
	}
	for (uint64_t tick = 14; tick < 20; ++tick) {
		const auto ordinary = CadenceDue(policy, state, UpdateDomain::Behaviour, tick, 1);
		CHECK(ordinary.Due == (tick % 4 == CadencePhase(policy.Identity.Text(), 4)));
		if (ordinary.Due) REQUIRE(Complete(state, ordinary));
	}
	policy.CriticalReasons = static_cast<uint32_t>(UpdateCriticality::Projectile);
	const auto projectile = CadenceDue(policy, state, UpdateDomain::Behaviour, 20, 1);
	CHECK(projectile.Due);
	CHECK(projectile.CriticalReasons == policy.CriticalReasons);
	REQUIRE(Complete(state, projectile));
	const auto maximum = CadenceDue(policy, state, UpdateDomain::Behaviour, UINT64_MAX, 1, combat);
	CHECK(maximum.HoldUntilTick == UINT64_MAX);
}

TEST_CASE("invalid intervals fail visibly while unnamed subjects keep full fidelity", "[scene][cadence]") {
	const auto valid = TownPolicy();
	for (int invalid = 0; invalid < 7; ++invalid) {
		auto policy = valid;
		if (invalid == 0) policy.Revision = 0;
		if (invalid == 1) policy.Domains[0].IntervalTicks = 0;
		if (invalid == 2) policy.Domains[0].MaximumStalenessTicks = 1;
		if (invalid == 3) policy.Domains[0].MaximumStalenessTicks = MAXIMUM_CADENCE_TICKS + 1;
		if (invalid == 4) policy.DemotionDelayTicks = MAXIMUM_CADENCE_TICKS + 1;
		if (invalid == 5) policy.CriticalReasons = 16;
		if (invalid == 6)
			policy.Identity = engine::core::Name(std::string(MAXIMUM_CADENCE_IDENTITY_BYTES + 1, 'x'));
		CHECK_FALSE(ValidUpdateCadence(policy));
		const auto refused = CadenceDue(policy, {}, UpdateDomain::Perception, 1, 1);
		CHECK_FALSE(refused.Due);
		CHECK(refused.Reason == CadenceReason::InvalidPolicy);
	}
	CHECK_FALSE(CadenceDue(valid, {}, UpdateDomain::Count, 1, 1).Due);
	CHECK_FALSE(CadenceDue(valid, {}, UpdateDomain::Perception, 1, 0).Due);
	CHECK_FALSE(CadenceDue(valid, {}, UpdateDomain::Perception, 1, 1, 16).Due);
	auto unnamed = valid;
	unnamed.Identity = {};
	DomainCadenceState state;
	REQUIRE(Complete(state, CadenceDue(unnamed, state, UpdateDomain::Perception, 0, 1)));
	for (uint64_t tick = 1; tick < 10; ++tick)
		REQUIRE(Complete(state, CadenceDue(unnamed, state, UpdateDomain::Perception, tick, 1)));
}

TEST_CASE("saved cadence resumes exact decisions while derived pose history resets", "[scene][cadence]") {
	RegisterSceneComponents();
	engine::ecs::Store source("cadence.source");
	const auto subject = source.Create();
	auto policy = TownPolicy();
	policy.Owner = subject;
	UpdateCadenceState state;
	REQUIRE(Complete(state.Domains[0], CadenceDue(policy, state.Domains[0], UpdateDomain::Perception, 5, 9)));
	source.Set(subject, policy);
	source.Set(subject, state);
	PoseCadenceHistory history;
	history.Poses[subject] = state.Domains[0];
	source.SetResource(history);
	engine::core::ByteWriter bytes;
	REQUIRE(source.Save(bytes));
	engine::core::ByteReader reader(bytes.Bytes());
	engine::ecs::Store restored("cadence.restored");
	REQUIRE(restored.Load(reader));
	const auto *back = restored.Get<UpdateCadence>(subject);
	const auto *saved = restored.Get<UpdateCadenceState>(subject);
	REQUIRE(back != nullptr);
	REQUIRE(saved != nullptr);
	CHECK(back->Identity.Text() == policy.Identity.Text());
	CHECK(back->Owner == subject);
	CHECK(saved->Domains[0].Generation == state.Domains[0].Generation);
	REQUIRE(restored.Resource<PoseCadenceHistory>() != nullptr);
	CHECK(restored.Resource<PoseCadenceHistory>()->Poses.empty());
	for (uint64_t tick = 6; tick < 30; ++tick) {
		const auto original = CadenceDue(policy, state.Domains[0], UpdateDomain::Perception, tick, 9);
		const auto replay = CadenceDue(*back, saved->Domains[0], UpdateDomain::Perception, tick, 9);
		CHECK(original.Due == replay.Due);
		CHECK(original.Reason == replay.Reason);
		CHECK(original.AgeTicks == replay.AgeTicks);
	}
	const auto &descriptor = engine::ecs::Components::Describe(engine::ecs::Components::Of<UpdateCadence>());
	engine::core::ByteWriter malformed;
	policy.Domains[0].IntervalTicks = 0;
	descriptor.Write(malformed, &policy, 1);
	engine::core::ByteReader invalid(malformed.Bytes());
	auto unchanged = TownPolicy();
	descriptor.Read(invalid, &unchanged, 1);
	CHECK(invalid.Failed());
	CHECK(unchanged.Domains[0].IntervalTicks == 8);
}
