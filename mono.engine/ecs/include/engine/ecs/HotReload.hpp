#pragma once

// In-place component data migration for a running world.
//
// Native layouts remain fixed for the loaded binary. A reload migrates values
// to a newer semantic revision, marks the affected column for downstream
// consumers, and refuses stale or repeated revisions.
//
// @tier L3 shared

#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine::ecs {
	class ComponentReloads {
	  public:
		template <class T, class Migration>
		bool Apply(Store &store, uint64_t revision, Migration &&migration) {
			const ComponentId component = Components::Of<T>();
			auto found = std::lower_bound(
				Revisions.begin(),
				Revisions.end(),
				component.Index,
				[](const Revision &entry, uint32_t index) { return entry.Component.Index < index; }
			);
			if (revision == 0 ||
				(found != Revisions.end() && found->Component == component && revision <= found->Value)) {
				return false;
			}

			store.Each<T>(std::forward<Migration>(migration));
			store.MarkAllChanged<T>();
			if (found == Revisions.end() || found->Component != component) {
				Revisions.insert(found, Revision{component, revision});
			} else {
				found->Value = revision;
			}
			return true;
		}

		template <class T> uint64_t Current() const {
			const ComponentId component = Components::Of<T>();
			const auto found = std::lower_bound(
				Revisions.begin(),
				Revisions.end(),
				component.Index,
				[](const Revision &entry, uint32_t index) { return entry.Component.Index < index; }
			);
			return found == Revisions.end() || found->Component != component ? 0 : found->Value;
		}

	  private:
		struct Revision {
			ComponentId Component;
			uint64_t Value = 0;
		};

		std::vector<Revision> Revisions;
	};
}
