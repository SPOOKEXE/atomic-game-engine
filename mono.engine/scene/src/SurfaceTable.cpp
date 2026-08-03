#include <engine/scene/SurfaceTable.hpp>

namespace engine::scene {

	void SurfaceTable::Set(core::Name material, const SurfaceProperties &properties) {
		for (SurfaceRow &row : Rows) {
			if (row.Material == material) {
				row.Properties = properties;
				return;
			}
		}
		Rows.push_back(SurfaceRow{material, properties});
	}

	const SurfaceProperties *SurfaceTable::Find(core::Name material) const {
		// An invalid name matching an invalid row would make "no material set"
		// resolve to whatever was registered first without one, which is the
		// silent default this table exists to refuse.
		if (!material.IsValid()) {
			return nullptr;
		}

		for (const SurfaceRow &row : Rows) {
			if (row.Material == material) {
				return &row.Properties;
			}
		}
		return nullptr;
	}
}
