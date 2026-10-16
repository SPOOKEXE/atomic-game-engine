#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/script/ServiceSurface.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/script/UpdateCadenceService.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_SUITE_ID("engine.script.updatecadenceservice")
TEST_DEPENDS("engine.scene.update-cadence")

using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::UpdateDomain;
using engine::script::BeginNpcUpdate;
using engine::script::CommitNpcBehaviour;
using engine::script::CommitNpcPerception;
using engine::script::ConfigureNpcCadence;
using engine::script::NpcBehaviour;
using engine::script::NpcCombatReady;
using engine::script::NpcPerception;
using engine::script::SetNpcCombat;

namespace {
	Entity Actor(Store &store, const char *name) {
		return store.CreateInstance(engine::scene::HumanoidClass(), name);
	}

	bool Configure(Store &store, Entity actor, std::string_view identity) {
		return ConfigureNpcCadence(store, actor, identity, {4, 7}, {7, 11}, 3);
	}
}

TEST_CASE("NPC cadence commits complete perception and movement at the fixed tick", "[script][cadence]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::script::RegisterScriptComponents();
	Store store("town_a");
	const Entity actor = Actor(store, "Guard");
	REQUIRE(Configure(store, actor, "town.guard.a"));
	const auto *policy = store.Get<engine::scene::UpdateCadence>(actor);
	REQUIRE(policy != nullptr);
	CHECK(policy->Domains[static_cast<size_t>(UpdateDomain::Perception)].IntervalTicks == 4);
	CHECK(policy->Domains[static_cast<size_t>(UpdateDomain::Behaviour)].IntervalTicks == 7);
	CHECK(policy->Domains[static_cast<size_t>(UpdateDomain::VisualPose)].IntervalTicks == 1);

	const auto perception = BeginNpcUpdate(store, 17, actor, UpdateDomain::Perception, 1);
	REQUIRE(perception.Due);
	REQUIRE(perception.Token != 0);
	CHECK_FALSE(CommitNpcPerception(store, 17, actor, perception.Token, 1, true, {1, 2, 3}, false));
	CHECK_FALSE(CommitNpcPerception(store, 18, actor, perception.Token, 1, true, {1, 2, 3}, true));
	CHECK_FALSE(CommitNpcPerception(store, 17, actor, perception.Token, 2, true, {1, 2, 3}, true));
	CHECK_FALSE(store.Get<NpcPerception>(actor)->Visible);
	REQUIRE(CommitNpcPerception(store, 17, actor, perception.Token, 1, true, {1, 2, 3}, true));
	CHECK(store.Get<NpcPerception>(actor)->Visible);
	CHECK(store.Get<NpcPerception>(actor)->TargetPosition == Vector3{1, 2, 3});
	CHECK_FALSE(CommitNpcPerception(store, 17, actor, perception.Token, 1, false, {}, true));
	CHECK(store.Get<NpcPerception>(actor)->Visible);

	const auto behaviour = BeginNpcUpdate(store, 17, actor, UpdateDomain::Behaviour, 1);
	REQUIRE(behaviour.Due);
	CHECK_FALSE(CommitNpcBehaviour(store, 17, actor, behaviour.Token, 1, {2, 0, 0}, true));
	CHECK(store.Get<NpcBehaviour>(actor)->MoveDirection == Vector3{});
	REQUIRE(CommitNpcBehaviour(store, 17, actor, behaviour.Token, 1, {0, 0, 1}, true));
	CHECK(store.Get<NpcBehaviour>(actor)->MoveDirection == Vector3{0, 0, 1});
	CHECK(store.Get<engine::scene::Humanoid>(actor)->MoveDirection == Vector3{0, 0, 1});
	CHECK_FALSE(NpcCombatReady(store, actor, 1));
}

TEST_CASE("promotion requires same-tick refreshed results before combat", "[script][cadence]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::script::RegisterScriptComponents();
	Store store("town_b");
	const Entity actor = Actor(store, "Guard");
	REQUIRE(Configure(store, actor, "town.guard.b"));
	const auto stale = BeginNpcUpdate(store, 31, actor, UpdateDomain::Perception, 2);
	REQUIRE(stale.Due);
	REQUIRE(SetNpcCombat(store, actor, true));
	CHECK_FALSE(CommitNpcPerception(store, 31, actor, stale.Token, 2, true, {}, true));
	CHECK_FALSE(NpcCombatReady(store, actor, 2));

	const auto perception = BeginNpcUpdate(store, 31, actor, UpdateDomain::Perception, 2);
	REQUIRE(perception.Due);
	REQUIRE(CommitNpcPerception(store, 31, actor, perception.Token, 2, true, {2, 0, 0}, true));
	CHECK_FALSE(NpcCombatReady(store, actor, 2));
	const auto behaviour = BeginNpcUpdate(store, 31, actor, UpdateDomain::Behaviour, 2);
	REQUIRE(behaviour.Due);
	REQUIRE(CommitNpcBehaviour(store, 31, actor, behaviour.Token, 2, {1, 0, 0}, true));
	CHECK(NpcCombatReady(store, actor, 2));

	store.AdvanceTick(store.Time().Delta);
	CHECK_FALSE(NpcCombatReady(store, actor, 2));
	const auto repeat = BeginNpcUpdate(store, 31, actor, UpdateDomain::Perception, 2);
	CHECK(repeat.Due);
	CHECK(repeat.Reason == engine::scene::CadenceReason::Critical);
	REQUIRE(SetNpcCombat(store, actor, false));
	CHECK_FALSE(CommitNpcPerception(store, 31, actor, repeat.Token, 2, true, {}, true));
	CHECK_FALSE(NpcCombatReady(store, actor, 2));
}

TEST_CASE(
	"pending admission is tied to Store incarnation and replaced on a second Begin", "[script][cadence]"
) {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::script::RegisterScriptComponents();
	Store first("town_same_name");
	Store second("town_same_name");
	const Entity left = Actor(first, "Guard");
	const Entity right = Actor(second, "Guard");
	REQUIRE(Configure(first, left, "town.guard.c"));
	REQUIRE(Configure(second, right, "town.guard.c"));
	const auto old = BeginNpcUpdate(first, 41, left, UpdateDomain::Perception, 1);
	const auto fresh = BeginNpcUpdate(second, 41, right, UpdateDomain::Perception, 1);
	REQUIRE(old.Due);
	REQUIRE(fresh.Due);
	CHECK(old.Token != fresh.Token);
	CHECK_FALSE(CommitNpcPerception(second, 41, right, old.Token, 1, true, {}, true));
	CHECK_FALSE(CommitNpcPerception(first, 42, left, old.Token, 1, true, {}, true));
	REQUIRE(CommitNpcPerception(second, 41, right, fresh.Token, 1, true, {}, true));
	const auto replacement = BeginNpcUpdate(first, 41, left, UpdateDomain::Perception, 1);
	REQUIRE(replacement.Due);
	CHECK_FALSE(CommitNpcPerception(first, 41, left, old.Token, 1, true, {}, true));
	CHECK(replacement.Token != old.Token);
	first.AdvanceTick(first.Time().Delta);
	CHECK_FALSE(CommitNpcPerception(first, 41, left, replacement.Token, 1, true, {}, true));
	CHECK(engine::script::UpdateCadenceServiceSurface().Methods.size() == 8);
}

TEST_CASE(
	"player ownership forces fresh work and pending tokens do not survive a save", "[script][cadence]"
) {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::script::RegisterScriptComponents();
	Store store("town_owned");
	const Entity actor = Actor(store, "Guard");
	REQUIRE(Configure(store, actor, "town.guard.owned"));
	const auto first = BeginNpcUpdate(store, 91, actor, UpdateDomain::Perception, 1);
	REQUIRE(first.Due);
	REQUIRE(CommitNpcPerception(store, 91, actor, first.Token, 1, true, {}, true));
	const Entity player = store.CreateInstance(engine::scene::PlayerClass(), "Owner");
	store.Set(actor, engine::scene::NetworkOwner{player});
	store.AdvanceTick(store.Time().Delta);
	const auto promoted = BeginNpcUpdate(store, 91, actor, UpdateDomain::Perception, 1);
	REQUIRE(promoted.Due);
	CHECK(promoted.Reason == engine::scene::CadenceReason::Critical);

	engine::core::ByteWriter bytes;
	REQUIRE(store.Save(bytes));
	engine::core::ByteReader reader(bytes.Bytes());
	Store restored("town_owned_restored");
	REQUIRE(restored.Load(reader));
	const auto *pending = restored.Get<engine::script::NpcCadencePending>(actor);
	REQUIRE(pending != nullptr);
	CHECK(pending->Domains[static_cast<size_t>(UpdateDomain::Perception)].Token == 0);
	CHECK_FALSE(CommitNpcPerception(restored, 91, actor, promoted.Token, 1, true, {}, true));
	store.SetAdoptOnly(true);
	CHECK_FALSE(BeginNpcUpdate(store, 91, actor, UpdateDomain::Behaviour, 1).Due);
	CHECK_FALSE(SetNpcCombat(store, actor, true));
}
