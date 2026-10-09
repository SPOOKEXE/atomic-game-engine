#include "Snapshot.hpp"

#include "Instances.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/Time.hpp>

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::ecs {

	namespace {
		// Recognises a snapshot before anything else is read.
		//
		// Eight bytes rather than a version alone, so a reader handed a stream
		// that is not a snapshot at all fails on the first field instead of
		// interpreting arbitrary bytes as a table count.
		constexpr uint64_t SNAPSHOT_MAGIC = 0x504E'534F'4E4F'4D55ull;

		struct LocalBranch {
			Entity Root;
			Entity Parent;
		};

		const Hierarchy *HierarchyOf(const StoreState &state, Entity entity, ComponentId hierarchy) {
			return static_cast<const Hierarchy *>(GetComponent(state, entity, hierarchy));
		}

		Hierarchy *MutableHierarchy(StoreState &state, Entity entity, ComponentId hierarchy) {
			return static_cast<Hierarchy *>(GetComponentMutable(state, entity, hierarchy));
		}

		// Resolve desired ancestors and earlier siblings before moving a node.
		// Validate cycles before any live rows or local branches are touched.
		bool OverlayHierarchyOrder(
			const StoreState &incoming,
			std::span<const Entity> entities,
			ComponentId hierarchy,
			std::vector<Entity> &ordered
		) {
			if (!hierarchy.IsValid()) return true;
			enum class Visit : uint8_t { Visiting, Visited };
			struct Pending {
				Entity Instance;
				bool Expanded;
			};
			std::unordered_map<Entity, Visit> visits;
			std::vector<Pending> pending;
			for (const Entity root : entities) {
				pending.push_back({root, false});
				while (!pending.empty()) {
					const Pending step = pending.back();
					pending.pop_back();
					const Hierarchy *node = HierarchyOf(incoming, step.Instance, hierarchy);
					if (node == nullptr) continue;
					if (step.Expanded) {
						visits.at(step.Instance) = Visit::Visited;
						ordered.push_back(step.Instance);
						continue;
					}
					if (const auto found = visits.find(step.Instance); found != visits.end()) {
						if (found->second == Visit::Visiting) return false;
						continue;
					}
					visits.emplace(step.Instance, Visit::Visiting);
					pending.push_back({step.Instance, true});
					const Hierarchy *previous = HierarchyOf(incoming, node->PreviousSibling, hierarchy);
					if (previous != nullptr && previous->Parent == node->Parent)
						pending.push_back({node->PreviousSibling, false});
					if (HierarchyOf(incoming, node->Parent, hierarchy) != nullptr)
						pending.push_back({node->Parent, false});
				}
			}
			return true;
		}

		// Keep the receiver's local branches separate from the incoming sibling
		// chains. Their rows and internal links remain in the receiving store.
		std::vector<LocalBranch>
		LocalBranches(const StoreState &state, const StoreState &incoming, ApplyMode mode) {
			const ComponentId hierarchy = Components::Assigned<Hierarchy>();
			if (!hierarchy.IsValid()) return {};
			const auto mentioned = [&](Entity entity) {
				const EntityId key = EntityId::Of(entity);
				return incoming.Directory.Alive(key.Index, key.Generation);
			};
			std::vector<LocalBranch> candidates;
			std::vector<Entity> parents;
			std::unordered_set<Entity> roots;
			std::unordered_set<Entity> seenParents;
			for (const Archetype &table : state.Tables) {
				for (const Entity entity : table.Entities()) {
					if (!SparseSet::IsPredicted(EntityId::Of(entity).Index) || mentioned(entity)) continue;
					const Hierarchy *node = HierarchyOf(state, entity, hierarchy);
					if (node == nullptr || node->Parent == NULL_ENTITY) continue;
					const Entity parent = node->Parent;
					const bool parentMentioned = mentioned(parent);
					if (!parentMentioned &&
						(mode == ApplyMode::Overlay || SparseSet::IsPredicted(EntityId::Of(parent).Index)))
						continue;
					candidates.push_back({entity, parent});
					roots.insert(entity);
					if (seenParents.insert(parent).second) parents.push_back(parent);
				}
			}
			std::vector<LocalBranch> branches;
			branches.reserve(candidates.size());
			for (const Entity parent : parents) {
				EachChild(state, parent, [&](Entity child) {
					if (roots.erase(child) != 0) branches.push_back({child, parent});
				});
			}
			// Also recover a live local branch whose parent already lost its link
			// in an earlier correction. Its retained parent still identifies it.
			for (const LocalBranch branch : candidates) {
				if (roots.erase(branch.Root) != 0) branches.push_back(branch);
			}
			return branches;
		}

		// This is a merge detail, not a reparent: removal callbacks must never
		// observe a temporary detach of an otherwise unchanged local instance.
		void IsolateLocalBranch(StoreState &state, LocalBranch branch, ComponentId hierarchy) {
			const Hierarchy links = *HierarchyOf(state, branch.Root, hierarchy);
			if (Hierarchy *previous = MutableHierarchy(state, links.PreviousSibling, hierarchy);
				previous != nullptr && previous->NextSibling == branch.Root)
				previous->NextSibling = links.NextSibling;
			if (Hierarchy *next = MutableHierarchy(state, links.NextSibling, hierarchy);
				next != nullptr && next->PreviousSibling == branch.Root)
				next->PreviousSibling = links.PreviousSibling;
			if (Hierarchy *parent = MutableHierarchy(state, branch.Parent, hierarchy); parent != nullptr) {
				if (parent->FirstChild == branch.Root) parent->FirstChild = links.NextSibling;
				if (parent->LastChild == branch.Root) parent->LastChild = links.PreviousSibling;
			}
			Hierarchy *root = MutableHierarchy(state, branch.Root, hierarchy);
			root->Parent = NULL_ENTITY;
			root->PreviousSibling = NULL_ENTITY;
			root->NextSibling = NULL_ENTITY;
		}

		void RestoreLocalBranch(StoreState &state, LocalBranch branch, ComponentId hierarchy) {
			if (HierarchyOf(state, branch.Root, hierarchy) == nullptr) return;
			const Hierarchy *parent = HierarchyOf(state, branch.Parent, hierarchy);
			if (parent == nullptr) {
				// A destroyed authority parent leaves the intact local subtree as
				// a root. A reused index at another generation is a different parent.
				if (state.WatchTree) state.TreeChanges.push_back({branch.Root, branch.Parent, NULL_ENTITY});
				return;
			}
			Entity last = parent->LastChild;
			if (last != NULL_ENTITY && HierarchyOf(state, last, hierarchy) == nullptr) {
				last = NULL_ENTITY;
				EachChild(state, branch.Parent, [&](Entity child) { last = child; });
			}
			if (last != NULL_ENTITY) MutableHierarchy(state, last, hierarchy)->NextSibling = branch.Root;
			Hierarchy *host = MutableHierarchy(state, branch.Parent, hierarchy);
			if (last == NULL_ENTITY) host->FirstChild = branch.Root;
			host->LastChild = branch.Root;
			Hierarchy *root = MutableHierarchy(state, branch.Root, hierarchy);
			root->Parent = branch.Parent;
			root->PreviousSibling = last;
		}

		// Attributes live in a shared resource, but receiver-created rows still
		// belong to their local instances when authority rows are replaced.
		void PreserveLocalAttributes(const StoreState &state, StoreState &incoming) {
			const ComponentId attributes = Components::Assigned<AttributeTable>();
			if (!attributes.IsValid()) return;
			const Column *held = state.Resources.Find(attributes.Index);
			Column *imported = incoming.Resources.Find(attributes.Index);
			if (held == nullptr || imported == nullptr) return;
			const auto &local = *static_cast<const AttributeTable *>(held->At(0));
			auto &merged = *static_cast<AttributeTable *>(imported->At(0));
			const auto receiverOwns = [&](uint32_t index) {
				return SparseSet::IsPredicted(index) && state.Directory.Live(index) &&
					   !incoming.Directory.Live(index);
			};
			for (const auto &[index, values] : local.Entities) {
				if (receiverOwns(index)) merged.Entities.insert_or_assign(index, values);
			}
			for (const auto &[index, revisions] : local.Revisions) {
				if (receiverOwns(index)) merged.Revisions.insert_or_assign(index, revisions);
			}
			merged.NextRevision = std::max(merged.NextRevision, local.NextRevision);
		}
	}

	bool SaveSnapshot(const StoreState &state, std::string_view name, core::ByteWriter &writer) {
		// The component names this snapshot mentions, in the order it mentions
		// them. Tables refer to components by their ordinal here rather than by
		// name repeated per table, and the reader resolves ordinals once.
		std::vector<ComponentId> mentioned;
		const auto ordinalOf = [&mentioned](ComponentId id) {
			const auto at = std::find(mentioned.begin(), mentioned.end(), id);
			if (at != mentioned.end()) {
				return static_cast<uint32_t>(at - mentioned.begin());
			}
			mentioned.push_back(id);
			return static_cast<uint32_t>(mentioned.size() - 1);
		};

		// Built into a scratch buffer first, because the component table has to
		// be written before the things that refer to it and is only complete
		// once they have been walked.
		// The component table needs a second pass, but it must share the outer
		// checkpoint budget so a hostile world cannot allocate an unbounded body
		// before the outer writer gets a chance to refuse it.
		core::ByteWriter body(0, writer.Remaining());

		body.WriteUInt32(static_cast<uint32_t>(state.Tables.size()));
		for (const Archetype &table : state.Tables) {
			body.WriteUInt32(static_cast<uint32_t>(table.Set().Size()));
			for (const ComponentId id : table.Set().Ids()) {
				const TypeDescriptor &descriptor = Components::Describe(id);
				if (descriptor.Size > 0 && !descriptor.Serialisable) {
					ENGINE_ERROR(
						"store '{}': cannot snapshot, component '{}' has no serialisation.",
						name,
						descriptor.Name.Text()
					);
					return false;
				}
				body.WriteUInt32(ordinalOf(id));
			}

			body.WriteUInt64(table.Rows());
			if (!table.Write(body)) {
				return false;
			}
		}

		// Sorted by the component's *name text* before writing.
		//
		// An unordered map iterates in whatever order its buckets happen to be
		// in, so two saves of the same world produced different bytes - and a
		// re-save after a load produced different bytes again, because the load
		// inserted in a different order. A snapshot that is not byte-stable
		// cannot be compared, which is what a recording and a CI determinism
		// job both do. Sorted by text rather than by id, because ids are
		// assigned in interning order and that differs after a restore.
		std::vector<uint32_t> resourceKeys;
		resourceKeys.reserve(state.Resources.Size());
		for (const auto &entry : state.Resources.Entries()) {
			resourceKeys.push_back(entry.Index);
		}
		std::sort(resourceKeys.begin(), resourceKeys.end(), [](uint32_t left, uint32_t right) {
			return Components::Describe(ComponentId{left}).Name.Text() <
				   Components::Describe(ComponentId{right}).Name.Text();
		});

		body.WriteUInt32(static_cast<uint32_t>(resourceKeys.size()));
		for (const uint32_t index : resourceKeys) {
			const Column &column = *state.Resources.Find(index);
			const ComponentId id{index};
			const TypeDescriptor &descriptor = Components::Describe(id);
			if (descriptor.Size > 0 && !descriptor.Serialisable) {
				ENGINE_ERROR(
					"store '{}': cannot snapshot, resource '{}' has no serialisation.",
					name,
					descriptor.Name.Text()
				);
				return false;
			}
			body.WriteUInt32(ordinalOf(id));
			if (!column.Write(body)) {
				return false;
			}
		}

		// Sorted by entity index, which is stable across a restore because the
		// directory is reproduced exactly rather than re-allocated.
		std::vector<uint32_t> namedIndices;
		namedIndices.reserve(state.NamesByIndex.size());
		for (const auto &entry : state.NamesByIndex) {
			namedIndices.push_back(entry.first);
		}
		std::sort(namedIndices.begin(), namedIndices.end());

		body.WriteUInt32(static_cast<uint32_t>(namedIndices.size()));
		for (const uint32_t index : namedIndices) {
			body.WriteUInt32(index);
			body.WriteString(state.NamesByIndex.at(index));
		}

		body.WriteUInt32(static_cast<uint32_t>(state.Watched.size()));
		for (const ComponentId id : state.Watched) {
			body.WriteUInt32(ordinalOf(id));
		}

		// --- the header, now that the component table is complete ---

		writer.WriteUInt64(SNAPSHOT_MAGIC);
		writer.WriteUInt32(Store::SNAPSHOT_VERSION);
		writer.WriteString(name);

		writer.WriteUInt32(static_cast<uint32_t>(mentioned.size()));
		for (const ComponentId id : mentioned) {
			writer.WriteName(Components::Describe(id).Name);
		}

		// The directory, exactly as it stands. Generations included, because a
		// handle stored inside a component is only valid if its generation
		// comes back too.
		//
		// **Two runs, one per index region.** The regions are 2³¹ indices apart
		// and each is dense from its own base, so writing one run over the whole
		// space would be two billion entries of nothing. Written as a count and
		// a run each rather than as index/value pairs, because a run costs five
		// bytes an entry against nine and the authoritative region is dense by
		// construction. This split is what `SNAPSHOT_VERSION` 2 is.
		const size_t issued = state.Directory.Capacity();
		writer.WriteUInt64(issued);
		for (size_t index = 0; index < issued; index++) {
			writer.WriteUInt32(state.Directory.Generation(static_cast<uint32_t>(index)));
			writer.WriteBool(state.Directory.Live(static_cast<uint32_t>(index)));
		}

		const size_t predicted = state.Directory.PredictedCapacity();
		writer.WriteUInt64(predicted);
		for (size_t local = 0; local < predicted; local++) {
			const auto index = static_cast<uint32_t>(SparseSet::PREDICTED_BASE + local);
			writer.WriteUInt32(state.Directory.Generation(index));
			writer.WriteBool(state.Directory.Live(index));
		}

		state.Directory.WriteAllocationState(writer);
		writer.WriteRaw(body.Bytes().data(), body.Size());
		return true;
	}

	bool LoadSnapshot(StoreState &state, std::string &name, core::ByteReader &reader) {
		ClearWorld(state);

		if (reader.ReadUInt64() != SNAPSHOT_MAGIC) {
			ENGINE_ERROR("store '{}': not a snapshot.", name);
			ClearWorld(state);
			return false;
		}

		const uint32_t version = reader.ReadUInt32();
		if (version != Store::SNAPSHOT_VERSION) {
			ENGINE_ERROR(
				"store '{}': snapshot version {}, this build reads {}.",
				name,
				version,
				Store::SNAPSHOT_VERSION
			);
			ClearWorld(state);
			return false;
		}

		name = std::string(reader.ReadString());

		// Names to this process's ids. A component the snapshot names and this
		// build does not have is a refusal rather than a gap: the rows carrying
		// it would be silently narrower than they were written.
		const uint32_t componentCount = reader.ReadUInt32();
		std::vector<ComponentId> resolved;
		resolved.reserve(componentCount);
		for (uint32_t index = 0; index < componentCount && !reader.Failed(); index++) {
			const core::Name componentName = reader.ReadName();
			const ComponentId id = Components::Find(componentName);
			if (!id.IsValid()) {
				ENGINE_ERROR(
					"store '{}': snapshot names component '{}', which this build does not have.",
					name,
					componentName.Text()
				);
				ClearWorld(state);
				return false;
			}
			resolved.push_back(id);
		}

		const auto lookup = [&resolved](uint32_t ordinal) {
			return ordinal < resolved.size() ? resolved[ordinal] : ComponentId{};
		};

		// Two runs, one per index region - see `SaveSnapshot`. Each count is
		// checked against what its region can actually hold before anything is
		// allocated: a corrupt or hostile stream claiming four billion entries
		// would otherwise have the loop below walk to it one failed read at a
		// time, and `FinishRestore` sweep the same range again afterwards.
		const uint64_t issued = reader.ReadUInt64();
		if (issued > SparseSet::AUTHORITATIVE_INDICES) {
			ENGINE_ERROR("store '{}': snapshot claims {} authoritative entities.", name, issued);
			ClearWorld(state);
			return false;
		}
		for (uint64_t index = 0; index < issued && !reader.Failed(); index++) {
			const uint32_t generation = reader.ReadUInt32();
			const bool live = reader.ReadBool();
			state.Directory.Restore(static_cast<uint32_t>(index), generation, live);
		}

		const uint64_t predicted = reader.ReadUInt64();
		if (predicted > SparseSet::PREDICTED_INDICES) {
			ENGINE_ERROR("store '{}': snapshot claims {} predicted entities.", name, predicted);
			ClearWorld(state);
			return false;
		}
		for (uint64_t local = 0; local < predicted && !reader.Failed(); local++) {
			const uint32_t generation = reader.ReadUInt32();
			const bool live = reader.ReadBool();
			state.Directory.Restore(
				static_cast<uint32_t>(SparseSet::PREDICTED_BASE + local), generation, live
			);
		}

		if (reader.Failed()) {
			ClearWorld(state);
			return false;
		}
		state.Directory.FinishRestore(static_cast<size_t>(issued), static_cast<size_t>(predicted));
		if (!state.Directory.ReadAllocationState(reader)) {
			ENGINE_ERROR("store '{}': snapshot has invalid allocation state.", name);
			ClearWorld(state);
			return false;
		}

		const uint32_t tableCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < tableCount && !reader.Failed(); index++) {
			const uint32_t members = reader.ReadUInt32();
			std::vector<ComponentId> ids;
			ids.reserve(members);
			for (uint32_t member = 0; member < members && !reader.Failed(); member++) {
				ids.push_back(lookup(reader.ReadUInt32()));
			}

			const uint64_t rows = reader.ReadUInt64();
			if (reader.Failed()) {
				break;
			}

			// Interned directly rather than through TableFor, which would add a
			// DirtyBits column the snapshot has already accounted for.
			const ComponentSet &set = ComponentSet::Intern(ids);
			const auto table = static_cast<uint32_t>(state.Tables.size());
			state.Tables.emplace_back(set);
			state.TableBySet.emplace(set.Id(), table);

			// **`ids` rather than nothing, and it is the whole cross-process
			// fix.** The columns were written in the *writer's* id order and
			// this table's are in this process's, which differ whenever the two
			// programs registered their components in a different sequence.
			if (!state.Tables.back().Read(reader, static_cast<size_t>(rows), ids)) {
				ClearWorld(state);
				return false;
			}

			// Every row's entity now knows where it lives again.
			const Archetype &restored = state.Tables.back();
			for (size_t row = 0; row < restored.Rows(); row++) {
				const EntityId key = EntityId::Of(restored.EntityAt(row));
				state.Directory.Relocate(key.Index, EntityLocation{table, static_cast<uint32_t>(row)});
			}
		}

		const uint32_t resourceCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < resourceCount && !reader.Failed(); index++) {
			const ComponentId id = lookup(reader.ReadUInt32());
			if (!id.IsValid()) {
				ClearWorld(state);
				return false;
			}

			Column column(id);
			if (!column.Read(reader, 1)) {
				ClearWorld(state);
				return false;
			}
			state.Resources.Assign(id, std::move(column));
		}

		const uint32_t nameCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < nameCount && !reader.Failed(); index++) {
			const uint32_t owner = reader.ReadUInt32();
			const std::string owned(reader.ReadString());
			if (reader.Failed()) {
				break;
			}
			state.NamesByIndex.insert_or_assign(owner, owned);
			state.EntitiesByName.insert_or_assign(
				owned, EntityId::Pack(owner, state.Directory.Generation(owner))
			);
		}

		const uint32_t watchedCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < watchedCount && !reader.Failed(); index++) {
			WatchComponent(state, lookup(reader.ReadUInt32()));
		}

		if (reader.Failed()) {
			ClearWorld(state);
			return false;
		}

		// The clock is a resource and came back with the rest, but a snapshot
		// written before one existed would leave the world without one.
		if (GetResourceValue(state, Components::Of<WorldTime>()) == nullptr) {
			const WorldTime clock{};
			SetResourceValue(state, Components::Of<WorldTime>(), &clock);
		}

		return true;
	}

	bool ApplySnapshot(
		StoreState &state,
		core::ByteReader &reader,
		ApplyMode mode,
		ApplyClock clock,
		const std::function<bool(ComponentId)> &resourceAllow
	) {
		// Read into a scratch world first, so a corrupt snapshot cannot leave
		// the live one half-merged. The live world is only touched once the
		// whole thing has parsed.
		//
		// A second world rather than an in-place parse: correctness first, and
		// the in-place version is an optimisation with a measurement attached
		// rather than a starting point. `ecs/docs/TODO.md` carries it.
		StoreState scratch;
		std::string scratchName = "apply.scratch";
		if (!LoadSnapshot(scratch, scratchName, reader)) {
			return false;
		}

		const ComponentId hierarchy = Components::Assigned<Hierarchy>();

		// --- what the snapshot knows about ---
		std::vector<Entity> incoming;
		for (const Archetype &table : scratch.Tables) {
			for (const Entity entity : table.Entities()) {
				incoming.push_back(entity);
			}
		}
		std::vector<Entity> hierarchyOrder;
		if (mode == ApplyMode::Overlay &&
			!OverlayHierarchyOrder(scratch, incoming, hierarchy, hierarchyOrder))
			return false;
		const auto localBranches = LocalBranches(state, scratch, mode);
		for (const LocalBranch branch : localBranches)
			IsolateLocalBranch(state, branch, hierarchy);

		// --- entities here that the snapshot does not mention ---
		if (mode == ApplyMode::Authoritative) {
			std::vector<Entity> stale;
			for (const Archetype &table : state.Tables) {
				for (const Entity entity : table.Entities()) {
					const EntityId key = EntityId::Of(entity);
					if (SparseSet::IsPredicted(key.Index)) {
						// **A predicted entity is never stale.** "The sender did
						// not mention it" is the whole definition of a
						// prediction - the authority allocates nothing from this
						// range, so its snapshot cannot mention one, and
						// destroying it here would delete every prediction on
						// the first correction. Whether a prediction has outlived
						// its usefulness is the predicting layer's call, made
						// through `Promote` or a destroy; it is not something a
						// snapshot's silence decides.
						continue;
					}
					if (!scratch.Directory.Alive(key.Index, key.Generation)) {
						stale.push_back(entity);
					}
				}
			}
			for (const Entity entity : stale) {
				// Out of the tree before out of the directory, exactly as
				// `Store::Destroy` does it. See `DetachFromTree`: freeing a row
				// leaves every link that points at it naming something gone,
				// and the sibling walk stops at the first of those rather than
				// stepping over it.
				//
				// Masked here rather than harmless - the sweep takes every
				// unmentioned entity and the pass below rewrites `Hierarchy` on
				// the ones that survive - but a second reader should not have
				// to reconstruct that argument to know this line is safe, and
				// it stops being true the moment the sweep narrows.
				DetachFromTree(state, entity);
				DestroyEntity(state, entity);
			}
		}

		// --- bring every incoming entity into line ---
		const ComponentId dirtyBits = Components::Of<DirtyBits>();
		for (const Entity entity : incoming) {
			const EntityId key = EntityId::Of(entity);

			if (!state.Directory.Alive(key.Index, key.Generation)) {
				// Not here, or here at a different generation - which means the
				// sender destroyed and recreated it, so this is a different
				// entity and the old one goes.
				if (state.Directory.Live(key.Index)) {
					const Entity replaced = EntityId::Pack(key.Index, state.Directory.Generation(key.Index));

					// **This is the one that was reachable.** It sits outside
					// the authoritative-only sweep above, so it runs in Overlay
					// mode - where nothing rewrites the `Hierarchy` of entities
					// the snapshot does not mention. A surviving parent kept
					// naming the freed row, `EachChild` ended its walk there,
					// and every sibling behind it was alive, in the save file,
					// and reachable from nothing.
					DetachFromTree(state, replaced);
					DestroyEntity(state, replaced);
				}

				// Restored at the sender's index *and* generation, so a handle
				// held anywhere - including inside another component - still
				// names the same entity on both sides.
				state.Directory.Adopt(key.Index, key.Generation);
			}

			// The components the sender says it has, and only those: a component
			// the sender dropped has to be dropped here too, or a replica
			// accumulates state the authority no longer believes in.
			const EntityLocation from = *scratch.Directory.Locate(key.Index);
			const ComponentSet &wanted = from.Archetype == EntityLocation::NO_ARCHETYPE
											 ? ComponentSet::Empty()
											 : scratch.Tables[from.Archetype].Set();

			const EntityLocation here = *state.Directory.Locate(key.Index);
			if (here.Archetype != EntityLocation::NO_ARCHETYPE) {
				const ComponentSet &held = state.Tables[here.Archetype].Set();
				for (const ComponentId id : held.Ids()) {
					// DirtyBits belongs to the receiving store's observation policy.
					// A sender that does not observe this entity's components omits it,
					// but removing it here would immediately be undone by Tracked().
					if (id != dirtyBits && !wanted.Contains(id)) {
						if (id == hierarchy) DetachFromTree(state, entity);
						RemoveComponent(state, entity, id);
					}
				}
			}

			for (const ComponentId id : wanted.Ids()) {
				if (mode == ApplyMode::Overlay && id == hierarchy) {
					// A partial tree must not replace indexes of unmentioned children.
					if (HierarchyOf(state, entity, hierarchy) == nullptr) {
						const Hierarchy blank{};
						SetComponent(state, entity, id, &blank);
					}
					continue;
				}
				const Column *column = scratch.Tables[from.Archetype].Find(id);
				SetComponent(state, entity, id, column->At(from.Row));
			}
		}
		// Desired parents move before their children, and earlier incoming
		// siblings move first. Unchanged parents retain their live order.
		for (const Entity entity : hierarchyOrder) {
			const Entity parent = HierarchyOf(scratch, entity, hierarchy)->Parent;
			if (parent != NULL_ENTITY && HierarchyOf(state, parent, hierarchy) == nullptr) {
				// A join preface may arrive before its external parent. The full
				// world snapshot will install that relationship when it arrives.
				const EntityId key = EntityId::Of(parent);
				if (!scratch.Directory.Alive(key.Index, key.Generation)) continue;
			}
			if (!SetParent(state, entity, parent)) {
				for (const LocalBranch branch : localBranches)
					RestoreLocalBranch(state, branch, hierarchy);
				return false;
			}
		}

		for (const LocalBranch branch : localBranches)
			RestoreLocalBranch(state, branch, hierarchy);

		// --- resources and the clock ---
		PreserveLocalAttributes(state, scratch);
		const ComponentId time = Components::Of<WorldTime>();
		for (const auto &entry : scratch.Resources.Entries()) {
			if (clock == ApplyClock::PreserveLocal && entry.Index == time.Index) continue;
			const ComponentId id{entry.Index};
			if (resourceAllow && !resourceAllow(id)) continue;
			SetResourceValue(state, id, entry.Storage.At(0));
		}

		return true;
	}
}
