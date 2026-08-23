#include <engine/ecs/Store.hpp>

#include <studio/Presentation.hpp>

#include <algorithm>

namespace studio {
	float PresentationCeiling(
		const PresentationRates &rates, bool focused, bool worldRunning, bool inputIdle
	) {
		if (rates.Frame <= 0.0f) {
			return 0.0f;
		}

		const float interface_ = inputIdle && !worldRunning ? rates.InterfaceIdle : rates.InterfaceActive;
		const float renderer = focused ? rates.RendererFocused : rates.RendererUnfocused;

		float ceiling = rates.Frame;
		if (interface_ > 0.0f) {
			ceiling = std::min(ceiling, interface_);
		}
		if (renderer > 0.0f) {
			ceiling = std::min(ceiling, renderer);
		}
		return ceiling;
	}

	float PresentationAlpha(bool advancing, engine::world::WorldState state, float accumulator) {
		return advancing && engine::world::Ticks(state) ? accumulator : 1.0f;
	}

	std::string WorldSelectorLabel(std::string_view name, bool active) {
		std::string label(name.empty() ? "?" : name);
		if (active) {
			label += " (ACTIVE)";
		}
		return label;
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
