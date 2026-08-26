#include <engine/ecs/Store.hpp>
#include <engine/scene/Terrain.hpp>

#include <algorithm>

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
	}

	Terrain &TerrainOf(ecs::Store &store) {
		if (!store.HasResource<Terrain>()) {
			store.SetResource(Terrain{});
		}
		return *store.ResourceMutable<Terrain>();
	}

	Terrain TerrainSettings(const ecs::Store &store) {
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
