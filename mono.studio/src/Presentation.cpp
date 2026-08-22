#include <engine/ecs/Store.hpp>

#include <studio/Presentation.hpp>

namespace studio {

	float PresentationAlpha(bool advancing, engine::world::WorldState state, float accumulator) {
		return advancing && engine::world::Ticks(state) ? accumulator : 1.0f;
	}

	void AppendReplicaVisualInstances(
		engine::core::Name replicaWorld,
		std::span<const engine::scene::DrawInstance> replica,
		std::vector<engine::scene::DrawInstance> &authority
	) {
		authority.reserve(authority.size() + 64);

		for (const engine::scene::DrawInstance &instance : replica) {
			if (!engine::ecs::Store::IsPredicted(engine::ecs::Entity(instance.Source))) {
				continue;
			}

			authority.push_back(instance);
			if (!authority.back().SourceWorld.IsValid()) {
				authority.back().SourceWorld = replicaWorld;
			}
		}
	}
}
