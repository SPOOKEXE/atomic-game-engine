#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Terrain.hpp>

#include <algorithm>
#include <stdexcept>

namespace engine::scene {

	namespace {
		// Brings a recipe inside the limits the header states.
		//
		// **Clamped on read rather than refused on write**, which is `SunOf`'s
		// rule about normalising a direction: the values arrive from a save file
		// and a wire as often as from a property setter, and a reader that
		// trusted them would turn somebody else's number into an allocation.
		Terrain Clamped(Terrain terrain) {
			terrain.ChunkExtent = std::clamp(terrain.ChunkExtent, 0.0f, MAX_CHUNK_EXTENT);
			terrain.VerticalExtent = std::max(terrain.VerticalExtent, 0.0f);
			terrain.ViewDistance = std::max(terrain.ViewDistance, 0.0f);
			terrain.ChunkResolution = std::min(terrain.ChunkResolution, MAX_CHUNK_RESOLUTION);
			return terrain;
		}

		ecs::Entity TerrainInstance(const ecs::Store &store) {
			const ecs::Entity workspace = WorkspaceOf(store);
			const ecs::ClassId terrainClass = ecs::Classes::Find(core::Name("Terrain"));
			if (workspace == ecs::NULL_ENTITY || !terrainClass.IsValid()) {
				return ecs::NULL_ENTITY;
			}
			return store.FindFirstChildWhichIsA(workspace, terrainClass);
		}
	}

	Terrain &TerrainOf(ecs::Store &store) {
		ecs::Entity terrain = TerrainInstance(store);
		if (terrain == ecs::NULL_ENTITY && !store.AdoptOnly()) {
			InstallServices(store);
			terrain = TerrainInstance(store);
		}
		if (terrain != ecs::NULL_ENTITY) {
			if (Terrain *recipe = store.GetMutable<Terrain>(terrain)) {
				return *recipe;
			}
		}
		throw std::logic_error("TerrainOf requires the generated Terrain instance");
	}

	Terrain TerrainSettings(const ecs::Store &store) {
		if (const ecs::Entity instance = TerrainInstance(store); instance != ecs::NULL_ENTITY) {
			if (const Terrain *existing = store.Get<Terrain>(instance)) {
				return Clamped(*existing);
			}
		}
		// Old snapshots stored the recipe as a resource. Installation migrates it
		// onto the generated instance and removes this compatibility path.
		if (const Terrain *existing = store.Resource<Terrain>()) {
			return Clamped(*existing);
		}
		return Terrain{};
	}

	bool GeneratesGround(const Terrain &terrain) {
		return terrain.Enabled && terrain.Generator.IsValid() && terrain.ChunkExtent > 0.0f &&
			   terrain.ChunkResolution > 0;
	}
}
