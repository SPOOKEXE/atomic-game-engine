#include "Promotion.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>

#include <string>

namespace engine::ecs {

	namespace {
		// One node's tree links, for writing. Every call may move the row it
		// points into, so a caller holding two of these at once is holding one
		// that has already gone stale.
		Hierarchy *MutableNode(StoreState &state, Entity instance) {
			return static_cast<Hierarchy *>(
				GetComponentMutable(state, instance, Components::Of<Hierarchy>())
			);
		}

		// Points every tree link that named `from` at `to`.
		//
		// The tree is the one place `ecs` itself stores entity handles inside
		// components, so it is the one place it can fix up without guessing.
		// Nothing here reaches outside the promoted node's immediate
		// neighbourhood: a parent, two siblings and the direct children are
		// every link that can name it.
		void RelinkTree(StoreState &state, Entity from, Entity to) {
			const Hierarchy *node = MutableNode(state, to);
			if (node == nullptr) {
				// Not an instance, so there is no tree to relink.
				return;
			}

			// Copied, because every lookup below may move this row and leave the
			// pointer above naming somebody else's node.
			const Hierarchy links = *node;

			Hierarchy *parent = MutableNode(state, links.Parent);
			if (parent != nullptr) {
				if (parent->FirstChild == from) {
					parent->FirstChild = to;
				}
				if (parent->LastChild == from) {
					parent->LastChild = to;
				}
			}

			Hierarchy *previous = MutableNode(state, links.PreviousSibling);
			if (previous != nullptr) {
				previous->NextSibling = to;
			}

			Hierarchy *next = MutableNode(state, links.NextSibling);
			if (next != nullptr) {
				next->PreviousSibling = to;
			}

			for (Entity child = links.FirstChild; child != NULL_ENTITY;) {
				Hierarchy *link = MutableNode(state, child);
				if (link == nullptr) {
					break;
				}
				link->Parent = to;
				child = link->NextSibling;
			}
		}
	}

	bool PromoteEntity(StoreState &state, std::string_view world, Entity predicted, Entity authoritative) {
		const EntityId from = EntityId::Of(predicted);
		const EntityId to = EntityId::Of(authoritative);

		if (state.DeferDepth > 0) {
			// Refused rather than deferred. The row keeps its place and its
			// contents, but the id array a running `Each` is holding a pointer
			// into would change underneath it - so the loop would hand a body
			// the new handle for rows it had already visited under the old one.
			ENGINE_ERROR("store '{}': cannot promote from inside an iteration.", world);
			return false;
		}

		if (!SparseSet::IsPredicted(from.Index)) {
			ENGINE_ERROR("store '{}': cannot promote an entity that was not predicted.", world);
			return false;
		}

		if (!state.Directory.Alive(from.Index, from.Generation)) {
			ENGINE_ERROR("store '{}': cannot promote an entity that is not live.", world);
			return false;
		}

		if (authoritative == NULL_ENTITY || SparseSet::IsPredicted(to.Index)) {
			// Promoting into the predicted range would leave the entity exactly
			// as unsafe as it started, having claimed to have fixed it.
			ENGINE_ERROR("store '{}': cannot promote to a handle outside the authoritative range.", world);
			return false;
		}

		if (state.Directory.Live(to.Index)) {
			ENGINE_ERROR("store '{}': cannot promote onto an index that is already live.", world);
			return false;
		}

		// Read before the directory is touched: `Adopt` rebuilds the free lists
		// and the location of the slot being promoted away from is about to be
		// cleared.
		const EntityLocation where = *state.Directory.Locate(from.Index);

		state.Directory.Adopt(to.Index, to.Generation);
		state.Directory.Relocate(to.Index, where);

		if (where.Archetype != EntityLocation::NO_ARCHETYPE) {
			// The row's own copy of the handle. Every query hands this out as
			// the entity it visited, so leaving it would make the promoted row
			// answer to a handle the directory says is dead.
			state.Tables[where.Archetype].Rename(where.Row, authoritative);
		}

		RelinkTree(state, predicted, authoritative);

		if (const auto named = state.NamesByIndex.find(from.Index); named != state.NamesByIndex.end()) {
			const std::string text = named->second;
			state.NamesByIndex.erase(named);
			state.NamesByIndex.insert_or_assign(to.Index, text);
			state.EntitiesByName.insert_or_assign(text, authoritative);
		}

		// Last, and deliberately a `Free` rather than a `DestroyEntity`: the row
		// now belongs to the authoritative handle, so vacating it would delete
		// the state the promotion exists to keep. The generation bump `Free`
		// does is what makes a stale predicted handle read as dead instead of
		// naming whatever the replica predicts next.
		state.Directory.Free(from.Index);
		return true;
	}
}
