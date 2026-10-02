#pragma once

// grug native adapters live outside pinned source catalogue. no documented product row is invented.

#include <engine/imagegraph/Catalogue.hpp>

#include <array>

namespace engine::imagegraph::detail {
	inline const CatalogueEntry *FindNativeCatalogueEntry(std::string_view type) {
		if (type == "pc.global_scope") {
			static const CatalogueEntry globals{
				"pc.global_scope",
				"",
				"Project Globals",
				"native-project",
				"",
				{},
					{},
					0,
					0,
					0,
					{},
				NodeSchema{"pc.global_scope", {}, {}, true}
			};
			return &globals;
		}
		if (type != "image.verlet_simple") return nullptr;
		static constexpr std::array inputs{
			CatalogueInput{"mesh", "Mesh", 0, "Mesh", ValueType::Mesh2D, "", ""},
			CatalogueInput{"substep", "Substep", 1, "Int", ValueType::Integer, "i 8", ""},
			CatalogueInput{"gravity", "Gravity", 2, "Vec2", ValueType::Vector2, "v 0 0.5", ""}
		};
		static constexpr std::array outputs{CatalogueOutput{"mesh", "Mesh", 0, ValueType::Mesh2D}};
		static constexpr std::array ports{
			PortSchema{"mesh", ValueType::Mesh2D, PortDirection::Input},
			PortSchema{"substep", ValueType::Integer, PortDirection::Input},
			PortSchema{"gravity", ValueType::Vector2, PortDirection::Input},
			PortSchema{"mesh", ValueType::Mesh2D, PortDirection::Output}
		};
		static constexpr std::array properties{
			PropertySchema{"substep", ValueType::Integer}, PropertySchema{"gravity", ValueType::Vector2}
		};
		static const CatalogueEntry entry{
			"image.verlet_simple",
			"Node_VerletSim_Simple",
			"Native Verlet",
			"native-simulation",
			"scripts/node_verletSim_simple/node_verletSim_simple.gml",
			inputs,
			outputs,
				0,
				0,
				0,
				{},
			NodeSchema{"image.verlet_simple", ports, properties}
		};
		return &entry;
	}
}
