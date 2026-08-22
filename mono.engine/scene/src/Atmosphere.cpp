#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <algorithm>

namespace engine::scene {

	namespace {
		// The `Lighting` service, or a null entity.
		//
		// By class and never by name, which is this module's fixture rule: a
		// script renaming `Lighting` must not make the sky disappear.
		ecs::Entity LightingService(const ecs::Store &store) {
			return ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
		}

		// The first descendant of `Lighting` carrying `T`, or a null entity.
		//
		// **A descendant walk rather than a child lookup**, so that a `Clouds`
		// under an `Atmosphere` and a `Clouds` directly under `Lighting` both
		// resolve. Roblox hangs its cloud layer off `Terrain` for reasons that are
		// an accident of where its volumetric renderer read the data from, and an
		// author following either arrangement should get a sky.
		//
		// **The first in tree order and never an arbitrary one**, which is
		// `FindSpawn`'s rule: a world with two atmospheres has to pick the same
		// one on every host, and archetype order is not the same on two machines.
		template <class T> ecs::Entity FirstUnderLighting(const ecs::Store &store) {
			const ecs::Entity lighting = LightingService(store);
			if (lighting == ecs::NULL_ENTITY) {
				return ecs::NULL_ENTITY;
			}

			ecs::Entity found = ecs::NULL_ENTITY;
			store.EachDescendant(lighting, [&](ecs::Entity descendant) {
				if (found == ecs::NULL_ENTITY && store.Get<T>(descendant) != nullptr) {
					found = descendant;
				}
			});
			return found;
		}

		// Brings authored values into the range the header states.
		//
		// **Clamped on read rather than refused on write**, which is `LightingOf`'s
		// rule for `FogStart` and `FogEnd`: these arrive from a save file and a
		// wire as often as from a property setter, and a density above one is a
		// shader reading past the end of a lookup rather than a slightly thick
		// day.
		Atmosphere Clamped(Atmosphere air) {
			air.Density = std::clamp(air.Density, 0.0f, 1.0f);
			air.Offset = std::clamp(air.Offset, -1.0f, 1.0f);
			air.Glare = std::clamp(air.Glare, 0.0f, 10.0f);
			air.Haze = std::clamp(air.Haze, 0.0f, 10.0f);
			return air;
		}

		Clouds Clamped(Clouds sky) {
			sky.Cover = std::clamp(sky.Cover, 0.0f, 1.0f);
			sky.Density = std::clamp(sky.Density, 0.0f, 1.0f);
			sky.WindSpeed = std::max(sky.WindSpeed, 0.0f);
			return sky;
		}
	}

	Atmosphere AtmosphereOf(const ecs::Store &store) {
		const ecs::Entity instance = FirstUnderLighting<Atmosphere>(store);
		if (instance == ecs::NULL_ENTITY) {
			// Clear air, which is what a world authored before this had. Not the
			// struct's own defaults: those describe a hazy day, and a place that
			// never asked for haze must not acquire it on load.
			Atmosphere clear;
			clear.Density = 0.0f;
			return clear;
		}
		return Clamped(*store.Get<Atmosphere>(instance));
	}

	Clouds CloudsOf(const ecs::Store &store) {
		const ecs::Entity instance = FirstUnderLighting<Clouds>(store);
		if (instance == ecs::NULL_ENTITY) {
			Clouds none;
			none.Enabled = false;
			return none;
		}
		return Clamped(*store.Get<Clouds>(instance));
	}

	ecs::ClassId AtmosphereClass() {
		// Through the one tree registration, for `AttachmentClass`'s reason.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Atmosphere"));
	}
}
