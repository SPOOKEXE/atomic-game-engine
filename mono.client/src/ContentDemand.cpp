#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Components.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>

#include <client/ContentDemand.hpp>
#include <unordered_set>

namespace client {

	namespace {
		uint64_t Fold(uint64_t hash, uint64_t value) {
			hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}

		template <class T> void ObserveRevision(engine::ecs::Store &store, uint64_t &revision) {
			store.Observe<T>();
			revision = Fold(revision, store.ComponentChangeVersion<T>());
			// Removal changes the answer too. The change channel records writes and
			// additions; the live count covers a row that disappeared entirely.
			revision = Fold(revision, store.CountMatching<T>());
		}

		// Appends one name if it is a name at all.
		void Want(
			std::vector<engine::core::Name> &out,
			std::unordered_set<uint32_t> &seen,
			const engine::core::Name &name
		) {
			if (name.IsValid() && seen.insert(name.Id()).second) {
				out.push_back(name);
			}
		}
	}

	uint64_t WantedContentRevision(engine::ecs::Store &store) {
		uint64_t revision = 0xcbf29ce484222325ull;
		ObserveRevision<engine::scene::Visual>(store, revision);
		ObserveRevision<engine::scene::MaterialRef>(store, revision);
		ObserveRevision<engine::scene::Sound>(store, revision);
		ObserveRevision<engine::scene::SurfaceAppearance>(store, revision);
		ObserveRevision<engine::gui::Picture>(store, revision);
		ObserveRevision<engine::effects::ParticleEmitter>(store, revision);
		ObserveRevision<engine::effects::Beam>(store, revision);
		ObserveRevision<engine::effects::Trail>(store, revision);
		ObserveRevision<engine::scene::SkyboxTextures>(store, revision);
		ObserveRevision<engine::ecs::Hierarchy>(store, revision);
		return revision;
	}

	void CollectWantedContent(engine::ecs::Store &store, std::vector<engine::core::Name> &out) {
		// Separate typed walks keep every content-bearing component explicit.
		// `WantedContentRevision` prevents this function from running at all in
		// the steady state, so this path is paid when a reference changes rather
		// than once per presentation.
		std::unordered_set<uint32_t> seen;
		seen.reserve(out.size() + 16);
		for (const engine::core::Name &name : out) {
			if (name.IsValid()) {
				seen.insert(name.Id());
			}
		}

		store.Each<const engine::scene::Visual>([&out, &seen](
													engine::ecs::Entity, const engine::scene::Visual &visual
												) { Want(out, seen, visual.Mesh); });

		store.Each<const engine::scene::MaterialRef>(
			[&out, &seen](engine::ecs::Entity, const engine::scene::MaterialRef &material) {
				Want(out, seen, material.Asset);
			}
		);

		store.Each<const engine::scene::Sound>([&out, &seen](
												   engine::ecs::Entity, const engine::scene::Sound &sound
											   ) { Want(out, seen, sound.SoundId); });

		store.Each<const engine::scene::SurfaceAppearance>(
			[&out, &seen](engine::ecs::Entity, const engine::scene::SurfaceAppearance &appearance) {
				// **Every map a part names, not just its colour.** A normal map
				// nothing asked for is a texture that never arrives, and the
				// G-buffer then samples the default for a material that has one
				// - which looks like the map being wrong rather than missing.
				Want(out, seen, appearance.ColourMap);
				Want(out, seen, appearance.NormalMap);
				Want(out, seen, appearance.RoughnessMap);
				Want(out, seen, appearance.OcclusionMap);
				Want(out, seen, appearance.HeightMap);
				Want(out, seen, appearance.MetalnessMap);
				Want(out, seen, appearance.EmissiveMap);
			}
		);

		store.Each<const engine::gui::Picture>([&out, &seen](
												   engine::ecs::Entity, const engine::gui::Picture &picture
											   ) { Want(out, seen, picture.Image); });

		store.Each<const engine::effects::ParticleEmitter>(
			[&out, &seen](engine::ecs::Entity, const engine::effects::ParticleEmitter &emitter) {
				Want(out, seen, emitter.Texture);
			}
		);

		store.Each<const engine::effects::Beam>([&out, &seen](
													engine::ecs::Entity, const engine::effects::Beam &beam
												) { Want(out, seen, beam.Texture); });

		store.Each<const engine::effects::Trail>([&out, &seen](
													 engine::ecs::Entity, const engine::effects::Trail &trail
												 ) { Want(out, seen, trail.Texture); });

		// Skybox faces are demand-loaded like every other texture, but only for
		// the provider hierarchy resolution selected. Asking for every inactive
		// sibling would spend device memory on content that cannot reach a pixel.
		const engine::scene::Environment environment = engine::scene::EnvironmentOf(store);
		if (environment.Skybox == engine::scene::SkyboxSource::Textures && environment.Textures.Enabled) {
			Want(out, seen, environment.Textures.Front);
			Want(out, seen, environment.Textures.Back);
			Want(out, seen, environment.Textures.Left);
			Want(out, seen, environment.Textures.Right);
			Want(out, seen, environment.Textures.Up);
			Want(out, seen, environment.Textures.Down);
		}
	}
}
