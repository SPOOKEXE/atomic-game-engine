#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/gui/Components.hpp>
#include <engine/scene/Components.hpp>

#include <client/ContentDemand.hpp>

namespace client {

	namespace {
		// Appends one name if it is a name at all.
		void Want(std::vector<engine::core::Name> &out, const engine::core::Name &name) {
			if (name.IsValid()) {
				out.push_back(name);
			}
		}
	}

	void CollectWantedContent(engine::ecs::Store &store, std::vector<engine::core::Name> &out) {
		// **`Each` over seven component types rather than one batched walk.**
		// This runs on the content pump and not in the frame — it answers "is
		// there anything new to ask for", which changes when a scene is authored
		// or streamed rather than every tick — so the constant factor is not
		// where the cost is. `client::CollectInstances` is the loop that needed
		// the batched form and it is a different loop.
		store.Each<engine::scene::Visual>([&out](engine::ecs::Entity, engine::scene::Visual &visual) {
			Want(out, visual.Mesh);
		});

		store.Each<engine::scene::MaterialRef>(
			[&out](engine::ecs::Entity, engine::scene::MaterialRef &material) { Want(out, material.Asset); }
		);

		store.Each<engine::scene::Sound>([&out](engine::ecs::Entity, engine::scene::Sound &sound) {
			Want(out, sound.SoundId);
		});

		store.Each<engine::scene::SurfaceAppearance>(
			[&out](engine::ecs::Entity, engine::scene::SurfaceAppearance &appearance) {
				// **Every map a part names, not just its colour.** A normal map
				// nothing asked for is a texture that never arrives, and the
				// G-buffer then samples the default for a material that has one
				// — which looks like the map being wrong rather than missing.
				Want(out, appearance.ColourMap);
				Want(out, appearance.NormalMap);
				Want(out, appearance.RoughnessMap);
				Want(out, appearance.OcclusionMap);
				Want(out, appearance.HeightMap);
				Want(out, appearance.EmissiveMap);
			}
		);

		store.Each<engine::gui::Picture>([&out](engine::ecs::Entity, engine::gui::Picture &picture) {
			Want(out, picture.Image);
		});

		store.Each<engine::effects::ParticleEmitter>(
			[&out](engine::ecs::Entity, engine::effects::ParticleEmitter &emitter) {
				Want(out, emitter.Texture);
			}
		);

		store.Each<engine::effects::Beam>([&out](engine::ecs::Entity, engine::effects::Beam &beam) {
			Want(out, beam.Texture);
		});

		store.Each<engine::effects::Trail>([&out](engine::ecs::Entity, engine::effects::Trail &trail) {
			Want(out, trail.Texture);
		});
	}
}
