#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/scene/Attachments.hpp>

#include <algorithm>
#include <studio/Presentation.hpp>

namespace studio {
	namespace {
		constexpr uint64_t STUDIO_PARTICLE_ACTIVATION_REVISION = 1;

		uint64_t FoldParticleSelection(uint64_t hash, uint64_t value) {
			hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}

		bool ParticleEmitterHasStudioParent(const engine::ecs::Store &store, engine::ecs::Entity emitter) {
			const engine::ecs::Entity parent = store.ParentOf(emitter);
			if (parent == engine::ecs::NULL_ENTITY) {
				return false;
			}

			static const engine::ecs::ClassId pvInstance =
				engine::ecs::Classes::Find(engine::core::Name("PVInstance"));
			if (pvInstance.IsValid() && store.IsA(parent, pvInstance)) {
				return true;
			}

			static const engine::ecs::ClassId attachment = engine::scene::AttachmentClass();
			return attachment.IsValid() && store.IsA(parent, attachment) && pvInstance.IsValid() &&
				   store.FindFirstAncestorWhichIsA(parent, pvInstance) != engine::ecs::NULL_ENTITY;
		}
	}

	bool ParticleEmitterVisibleInStudio(
		const engine::ecs::Store &store,
		engine::ecs::Entity emitter,
		const engine::effects::ParticleEmitter &settings
	) {
		if (!settings.Enabled) {
			return false;
		}

		return ParticleEmitterHasStudioParent(store, emitter);
	}

	engine::render::ParticleBatchSelection StudioParticleSelection(engine::ecs::Store &store) {
		store.Observe<engine::ecs::Hierarchy>();
		static const engine::core::Name selectionName("studio.preview-particles");
		uint64_t revision = 0xcbf29ce484222325ull;
		revision = FoldParticleSelection(revision, store.ComponentChangeVersion<engine::ecs::Hierarchy>());
		revision = FoldParticleSelection(revision, store.CountMatching<engine::ecs::Hierarchy>());
		return {selectionName, ParticleEmitterVisibleInStudio, revision};
	}

	size_t CollectStudioParticleBatches(
		engine::ecs::Store &store, engine::render::ParticleFrame &frame, bool renderingEnabled
	) {
		if (!renderingEnabled) {
			frame.Clear();
			return 0;
		}
		return engine::render::CollectParticleBatches(store, frame, StudioParticleSelection(store));
	}

	bool AdvanceStudioParticlePreview(
		engine::ecs::Store &store, float delta, bool worldRunning, bool renderingEnabled
	) {
		if (worldRunning || !renderingEnabled) {
			return false;
		}
		(void)engine::effects::RefreshEmitters(
			store, ParticleEmitterHasStudioParent, STUDIO_PARTICLE_ACTIVATION_REVISION
		);
		(void)engine::effects::StepParticles(store, std::max(delta, 0.0f));
		return true;
	}

	bool StatusBarSnapshot::Refresh(
		double now,
		size_t viewport,
		uint32_t framesPerSecond,
		uint32_t drawCalls,
		uint64_t triangles,
		uint32_t culled
	) {
		if (Valid && viewport == Viewport && now < NextSample) {
			return false;
		}

		NextSample = now + 0.25;
		Viewport = viewport;
		FramesPerSecond = framesPerSecond;
		DrawCalls = drawCalls;
		Triangles = triangles;
		Culled = culled;
		Valid = true;
		return true;
	}

	float
	PresentationCeiling(const PresentationRates &rates, bool focused, bool worldRunning, bool inputIdle) {
		if (rates.Uncapped) {
			return 0.0f;
		}

		const auto ceiling = [](float rate) { return rate >= 361.0f ? 0.0f : std::max(rate, 0.0f); };
		const float interface_ =
			ceiling(inputIdle && !worldRunning ? rates.InterfaceIdle : rates.InterfaceActive);
		const float renderer = ceiling(focused ? rates.RendererFocused : rates.RendererUnfocused);

		if (interface_ <= 0.0f) {
			return std::max(renderer, 0.0f);
		}
		if (renderer <= 0.0f) {
			return interface_;
		}
		return std::min(interface_, renderer);
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
