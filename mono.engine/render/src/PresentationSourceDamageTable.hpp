#pragma once

// Private descriptor list shared by the source collector and its contract test.

#include "PresentationSource.hpp"
#include "PresentationSourceDamage.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Visibility.hpp>

#include <tuple>

namespace engine::render {
	namespace presentation_source_damage_detail {
		using ecs::Entity;
		using ecs::Store;

		enum class DamageClass {
			Pose,
			Full,
		};

		enum class ChangePolicy {
			ChangedRows,
			RevisionOnly,
		};

		using RevisionSlot = DrawList::SourceRevision DrawList::SourceRevisions::*;

		// Each entry binds a component to its retained epoch and the cheapest
		// damage signal that is sufficient for that source.
		template <class Component, RevisionSlot Revision, DamageClass Class, ChangePolicy Policy>
		struct Source {
			using ComponentType = Component;
			static constexpr DamageClass Damage = Class;
			static constexpr ChangePolicy Change = Policy;
			static constexpr RevisionSlot RevisionMember = Revision;
			template <class Relevant>
			void Collect(
				Store &store, DrawList &drawList, Relevant &&relevant, PresentationSourceDamage &damage
			) const {
				store.Observe<Component>();
				DrawList::SourceRevision &seen = drawList.Revisions.*Revision;
				const uint64_t writes = store.ComponentChangeVersion<Component>();
				const uint64_t membership = store.ComponentMembershipVersion<Component>();

				if (!drawList.SourcesReady) {
					seen = DrawList::SourceRevision{writes, membership};
					return;
				}

				if (membership != seen.Membership) {
					seen = DrawList::SourceRevision{writes, membership};
					damage.Full = true;
					return;
				}
				bool changed = false;

				if constexpr (Policy == ChangePolicy::ChangedRows) {
					if (writes == seen.Writes) {
						return;
					}
					seen.Writes = writes;

					bool visited = false;
					store.EachChanged<Component>([&](Entity entity, Component &) {
						visited = true;
						changed |= relevant(entity);
					});
					// Epochs survive an unpresented tick, row bits do not. An empty
					// walk therefore means the cached rows may be stale.
					changed |= !visited;
				} else {
					changed = writes != seen.Writes;
					seen.Writes = writes;
				}

				if constexpr (Class == DamageClass::Pose) {
					damage.Pose |= changed;
				} else {
					damage.Full |= changed;
				}
			}
		};

		// Component type, epoch slot, and damage class live together here. Adding
		// a source cannot silently omit its invalidation check from the collector.
		constexpr auto SOURCE_TABLE = std::tuple{
			Source<
				scene::Transform,
				&DrawList::SourceRevisions::Transform,
				DamageClass::Pose,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::PreviousTransform,
				&DrawList::SourceRevisions::PreviousTransform,
				DamageClass::Pose,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::Bounds,
				&DrawList::SourceRevisions::Bounds,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::Visual,
				&DrawList::SourceRevisions::Visual,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::SurfaceAppearance,
				&DrawList::SourceRevisions::SurfaceAppearance,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::Tags,
				&DrawList::SourceRevisions::Tags,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::LocalTransparency,
				&DrawList::SourceRevisions::LocalTransparency,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::CharacterLimb,
				&DrawList::SourceRevisions::CharacterLimb,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::Skeleton,
				&DrawList::SourceRevisions::Skeleton,
				DamageClass::Pose,
				ChangePolicy::RevisionOnly>{},
			Source<
				scene::Bone,
				&DrawList::SourceRevisions::Bone,
				DamageClass::Pose,
				ChangePolicy::RevisionOnly>{},
			Source<
				scene::Rendered,
				&DrawList::SourceRevisions::Rendered,
				DamageClass::Full,
				ChangePolicy::RevisionOnly>{},
			Source<
				scene::LODAuto,
				&DrawList::SourceRevisions::LODAuto,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::LODCustom,
				&DrawList::SourceRevisions::LODCustom,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::LODSettings,
				&DrawList::SourceRevisions::LODSettings,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
			Source<
				scene::RenderEffects,
				&DrawList::SourceRevisions::RenderEffects,
				DamageClass::Full,
				ChangePolicy::ChangedRows>{},
		};
	}

}
