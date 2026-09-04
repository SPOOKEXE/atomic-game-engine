#pragma once

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/core/Name.hpp>

#include <cstdint>
#include <unordered_map>

namespace studio {
	// The verified assets that already have a runtime representation in Studio.
	// A matching delivery is a retry, while a changed root or kind replaces the
	// resident representation after its new payload has been accepted.
	class ContentResidency {
	  public:
		// Whether Studio already holds the delivered asset representation.
		//
		// The content root covers bytes only. Kind is separate because the manifest
		// deliberately binds it above the asset root, and the same bytes can name a
		// different runtime resource after a republish.
		bool Contains(
			const engine::core::Name &name,
			engine::assets::AssetKind kind,
			const engine::assets::ContentHash &root
		) const {
			const auto found = Roots.find(name.Id());
			return found != Roots.end() && found->second.Kind == kind && found->second.Root == root;
		}

		// Records a delivered runtime asset after its decode and registration succeed.
		void Remember(
			const engine::core::Name &name,
			engine::assets::AssetKind kind,
			const engine::assets::ContentHash &root
		) {
			Roots.insert_or_assign(name.Id(), Entry{kind, root});
		}

	  private:
		struct Entry {
			engine::assets::AssetKind Kind = engine::assets::AssetKind::Unknown;
			engine::assets::ContentHash Root;
		};

		std::unordered_map<uint32_t, Entry> Roots;
	};
}
