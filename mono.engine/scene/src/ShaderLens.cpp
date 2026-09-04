#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/ShaderLens.hpp>

#include <algorithm>

namespace engine::scene {

	namespace {
		bool Earlier(const ShaderLensState &left, const ShaderLensState &right) {
			if (left.Priority != right.Priority) {
				return left.Priority < right.Priority;
			}
			if (left.Shader != right.Shader) {
				return left.Shader.Id() < right.Shader.Id();
			}
			return left.EntityId < right.EntityId;
		}
	}

	size_t ResolveShaderLenses(const ecs::Store &store, std::span<ShaderLensState> out) {
		size_t count = 0;
		auto &mutableStore = const_cast<ecs::Store &>(store);
		mutableStore.Each<const ShaderLens, const Transform>(
			[&](ecs::Entity entity, const ShaderLens &lens, const Transform &transform) {
				if (!lens.Enabled || !lens.Shader.IsValid() || !(lens.Radius > 0.0f) || out.empty()) {
					return;
				}

				ShaderLensState state{
					.Frame = transform.Frame,
					.Shader = lens.Shader,
					.EntityId = entity.Id,
					.Radius = lens.Radius,
					.InnerRadius = std::clamp(lens.InnerRadius, 0.0f, lens.Radius),
					.Falloff = std::clamp(lens.Falloff, 0.0f, 1.0f),
					.Strength = std::max(lens.Strength, 0.0f),
					.Spin = lens.Spin,
					.Priority = lens.Priority,
					.Shape = lens.Shape,
				};

				if (count == out.size()) {
					// Ordered low to high so rendering composes lower priorities first.
					// The lowest value is therefore the one a newly better candidate
					// replaces when the presentation budget is full.
					if (!Earlier(out.front(), state)) {
						return;
					}
					for (size_t index = 1; index < count; index++) {
						out[index - 1] = out[index];
					}
					count--;
				}

				size_t insert = 0;
				while (insert < count && Earlier(out[insert], state)) {
					insert++;
				}
				for (size_t index = count; index > insert; index--) {
					out[index] = out[index - 1];
				}
				out[insert] = state;
				count++;
			}
		);
		return count;
	}

	size_t DemandedLensShaders(const ecs::Store &store, std::vector<core::Name> &out) {
		out.clear();
		auto &mutableStore = const_cast<ecs::Store &>(store);
		mutableStore.Each<const ShaderLens>([&](ecs::Entity, const ShaderLens &lens) {
			if (lens.Enabled && lens.Shader.IsValid()) {
				out.push_back(lens.Shader);
			}
		});
		std::sort(out.begin(), out.end(), [](const core::Name &left, const core::Name &right) {
			return left.Id() < right.Id();
		});
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out.size();
	}
}
