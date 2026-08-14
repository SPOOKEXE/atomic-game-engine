#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Teams.hpp>

#include <cmath>

namespace engine::scene {

	bool SameTeamColour(const core::Color3 &left, const core::Color3 &right) {
		const auto within = [](float first, float second) {
			return std::fabs(first - second) <= TEAM_COLOUR_TOLERANCE;
		};
		return within(left.R, right.R) && within(left.G, right.G) && within(left.B, right.B);
	}

	ecs::ClassId TeamClass() {
		// **Through `ServiceClass`, because `Teams` is where a `Team` lives.**
		// The whole service tree is one registration and this is one of its
		// doors — the shape `PlayerClass` already has beside it.
		static const ecs::ClassId team = (ServiceClass(), ecs::Classes::Find(core::Name("Team")));
		return team;
	}

	ecs::ClassId SpawnLocationClass() {
		static const ecs::ClassId spawn =
			(EnsureClassTree(), ecs::Classes::Find(core::Name("SpawnLocation")));
		return spawn;
	}

	ecs::Entity TeamsOf(const ecs::Store &store) {
		return ServiceOf(store, ecs::Classes::Find(core::Name("Teams")));
	}

	ecs::Entity AddTeam(ecs::Store &store, std::string_view name, const core::Color3 &colour) {
		const ecs::Entity teams = TeamsOf(store);
		if (teams == ecs::NULL_ENTITY) {
			// Refused rather than furnished on the way past, which is
			// `AddPlayer`'s rule: a caller here has skipped `InstallServices`,
			// and minting the service quietly would hide that until something
			// else asked for it.
			return ecs::NULL_ENTITY;
		}

		const ecs::Entity team = store.CreateInstance(TeamClass(), name);
		if (team == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		store.SetParent(team, teams);
		store.Set(team, Team{colour});
		return team;
	}

	bool SetPlayerTeam(ecs::Store &store, ecs::Entity player, ecs::Entity team) {
		if (!store.Alive(player) || !store.IsA(player, PlayerClass())) {
			return false;
		}

		if (team != ecs::NULL_ENTITY && (!store.Alive(team) || !store.IsA(team, TeamClass()))) {
			return false;
		}

		store.Set(player, PlayerTeam{team});
		return true;
	}

	ecs::Entity TeamOf(const ecs::Store &store, ecs::Entity player) {
		const PlayerTeam *held = store.Get<PlayerTeam>(player);
		if (held == nullptr || !store.Alive(held->Instance)) {
			return ecs::NULL_ENTITY;
		}
		return held->Instance;
	}
}
