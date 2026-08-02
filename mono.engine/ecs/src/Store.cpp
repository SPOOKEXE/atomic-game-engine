#include "StoreState.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace engine::ecs {

	namespace {
		// The key a query plan is cached under.
		//
		// The sorted term ids as raw bytes. A string rather than a vector so
		// the map needs no custom hash, and sorted so that two systems asking
		// for the same components in different orders share one plan.
		std::string PlanKey(std::span<const ComponentId> terms) {
			std::vector<uint32_t> sorted;
			sorted.reserve(terms.size());
			for (const ComponentId term : terms) {
				sorted.push_back(term.Index);
			}
			std::sort(sorted.begin(), sorted.end());

			return std::string(
				reinterpret_cast<const char *>(sorted.data()), sorted.size() * sizeof(uint32_t)
			);
		}

		// Whether a component's writes are recorded in this world.
		bool Watching(const StoreState &state, ComponentId id) {
			return std::find(state.Watched.begin(), state.Watched.end(), id) != state.Watched.end();
		}

		// The set a table actually holds, which gains a DirtyBits column when
		// any member of it is observed.
		//
		// Done here rather than at every call site so that there is one answer
		// to "does this table track changes" and no way to create a table that
		// should have tracked and does not.
		const ComponentSet &Tracked(const StoreState &state, const ComponentSet &set) {
			if (state.Watched.empty() || set.IsEmpty()) {
				return set;
			}

			for (const ComponentId id : set.Ids()) {
				if (Watching(state, id)) {
					return set.With(Components::Of<DirtyBits>());
				}
			}
			return set;
		}

		// The table for a set, creating it when this world has not needed one.
		uint32_t TableFor(StoreState &state, const ComponentSet &wanted) {
			const ComponentSet &set = Tracked(state, wanted);
			const auto found = state.TableBySet.find(set.Id());
			if (found != state.TableBySet.end()) {
				return found->second;
			}

			const auto index = static_cast<uint32_t>(state.Tables.size());
			state.Tables.emplace_back(set);
			state.TableBySet.emplace(set.Id(), index);
			return index;
		}

		// Moves an entity's row into another table and fixes up whoever the
		// removal displaced.
		void Relocate(StoreState &state, uint32_t index, EntityLocation from, uint32_t toTable) {
			Archetype &destination = state.Tables[toTable];
			const Entity entity = EntityId::Pack(index, state.Directory.Generation(index));

			uint32_t row = 0;
			if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
				row = static_cast<uint32_t>(destination.Append(entity));
			} else {
				Archetype &source = state.Tables[from.Archetype];
				row = static_cast<uint32_t>(destination.AdoptRow(source, from.Row, entity));

				const Entity moved = source.RemoveSwapBack(from.Row);
				if (moved != NULL_ENTITY) {
					// The swap-back put somebody else where this row was, and
					// their directory entry still says otherwise.
					state.Directory.Relocate(
						EntityId::Of(moved).Index, EntityLocation{from.Archetype, from.Row}
					);
				}
			}

			state.Directory.Relocate(index, EntityLocation{toTable, row});
		}

		// Records that one component of one row was written.
		//
		// The bit index is the component's position in the table's sorted set,
		// which is the same position its column sits at — so this is an index
		// the caller already has rather than a second lookup.
		void MarkWritten(StoreState &state, uint32_t table, uint32_t row, ComponentId id) {
			if (state.Watched.empty()) {
				return;
			}

			Archetype &archetype = state.Tables[table];
			Column *bits = archetype.Find(Components::Of<DirtyBits>());
			if (bits == nullptr) {
				return;
			}

			const std::span<const ComponentId> ids = archetype.Set().Ids();
			const auto at = std::lower_bound(ids.begin(), ids.end(), id);
			if (at == ids.end() || *at != id) {
				return;
			}

			static_cast<DirtyBits *>(bits->At(row))->Mark(static_cast<size_t>(at - ids.begin()));
			state.Changes++;
		}

		// Drops an entity's row entirely.
		void Vacate(StoreState &state, EntityLocation from) {
			if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
				return;
			}

			Archetype &table = state.Tables[from.Archetype];
			const Entity moved = table.RemoveSwapBack(from.Row);
			if (moved != NULL_ENTITY) {
				state.Directory.Relocate(EntityId::Of(moved).Index, EntityLocation{from.Archetype, from.Row});
			}
		}
	}

	Store::Store(std::string_view name)
		: State(std::make_unique<StoreState>()), StoreName(name), Owner(std::this_thread::get_id()) {
		// The clock is the one resource that is always present, so that no
		// system has to check whether the world has a time.
		SetResource(WorldTime{});

		ENGINE_TRACE("store '{}' created", StoreName);
	}

	Store::~Store() = default;

	void Store::BindToCallingThread() {
		Owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
	}

	void Store::RequireOwningThread(const char *what) const {
		if (IsOnOwningThread()) {
			return;
		}

		// Abort rather than throw. A store touched from the wrong thread has
		// already raced by the time this runs; unwinding would leave the
		// corruption in place and hand it to whoever catches. The stack at the
		// moment of the violation is the only useful artifact.
		ENGINE_ERROR(
			"store '{}': {} called from a thread that does not own it. "
			"A world's storage is touched only by the thread that ticks it.",
			StoreName,
			what
		);
		std::abort();
	}

	// --- entities ----------------------------------------------------------

	Entity Store::Create() {
		RequireOwningThread("Create");

		const uint32_t index = State->Directory.Allocate();
		return EntityId::Pack(index, State->Directory.Generation(index));
	}

	Entity Store::Create(std::string_view name) {
		RequireOwningThread("Create");

		if (name.empty()) {
			return Create();
		}

		const std::string key(name);
		const auto found = State->EntitiesByName.find(key);
		if (found != State->EntitiesByName.end() && Alive(found->second)) {
			// A name is a way to point at one thing. Handing back a second
			// entity under the same name would make "the camera" ambiguous the
			// first time a scene was loaded twice.
			return found->second;
		}

		const Entity entity = Create();
		State->NamesByIndex.insert_or_assign(EntityId::Of(entity).Index, key);
		State->EntitiesByName.insert_or_assign(key, entity);
		return entity;
	}

	void Store::Destroy(Entity entity) {
		RequireOwningThread("Destroy");

		const EntityId id = EntityId::Of(entity);
		if (!State->Directory.Alive(id.Index, id.Generation)) {
			return;
		}

		if (State->DeferDepth > 0) {
			State->Commands.push_back(Command{Command::Kind::Destroy, entity, ComponentId{}, nullptr});
			return;
		}

		Vacate(*State, *State->Directory.Locate(id.Index));

		const auto named = State->NamesByIndex.find(id.Index);
		if (named != State->NamesByIndex.end()) {
			State->EntitiesByName.erase(named->second);
			State->NamesByIndex.erase(named);
		}

		State->Directory.Free(id.Index);
	}

	bool Store::Alive(Entity entity) const {
		if (entity.Id == 0) {
			return false;
		}
		const EntityId id = EntityId::Of(entity);
		return State->Directory.Alive(id.Index, id.Generation);
	}

	std::string_view Store::NameOf(Entity entity) const {
		if (!Alive(entity)) {
			return {};
		}
		const auto found = State->NamesByIndex.find(EntityId::Of(entity).Index);
		return found == State->NamesByIndex.end() ? std::string_view{} : std::string_view(found->second);
	}

	Entity Store::Find(std::string_view name) const {
		const auto found = State->EntitiesByName.find(std::string(name));
		if (found == State->EntitiesByName.end() || !Alive(found->second)) {
			return NULL_ENTITY;
		}
		return found->second;
	}

	size_t Store::TableCount() const {
		return State->Tables.size();
	}

	// --- components --------------------------------------------------------

	void Store::SetRaw(Entity entity, ComponentId id, const void *value) {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return;
		}

		if (State->DeferDepth > 0) {
			const TypeDescriptor &descriptor = Components::Describe(id);

			// The value is copied because the caller's may be a temporary that
			// is gone by the time the loop ends. Allocated at the component's
			// own alignment rather than the allocator's default, since a
			// component may be over-aligned.
			void *payload = nullptr;
			if (descriptor.Size > 0) {
				payload = ::operator new(descriptor.Size, std::align_val_t(descriptor.Alignment));
				descriptor.CopyConstruct(payload, value, 1);
			}

			State->Commands.push_back(Command{Command::Kind::Set, entity, id, payload});
			return;
		}

		const EntityLocation location = *State->Directory.Locate(key.Index);

		if (location.Archetype != EntityLocation::NO_ARCHETYPE) {
			if (Column *column = State->Tables[location.Archetype].Find(id); column != nullptr) {
				// Already present, so this is an overwrite and nothing moves.
				column->Assign(location.Row, value);
				MarkWritten(*State, location.Archetype, location.Row, id);
				return;
			}
		}

		const ComponentSet &current = location.Archetype == EntityLocation::NO_ARCHETYPE
										  ? ComponentSet::Empty()
										  : State->Tables[location.Archetype].Set();

		const uint32_t destination = TableFor(*State, current.With(id));
		Relocate(*State, key.Index, location, destination);

		const EntityLocation moved = *State->Directory.Locate(key.Index);
		State->Tables[moved.Archetype].Find(id)->Assign(moved.Row, value);

		// A component that has just appeared has changed as surely as one that
		// was overwritten, and a consumer rebuilding from a delta needs it.
		MarkWritten(*State, moved.Archetype, moved.Row, id);
	}

	bool Store::HasRaw(Entity entity, ComponentId id) const {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return false;
		}

		const EntityLocation *location = State->Directory.Locate(key.Index);
		if (location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return false;
		}
		return State->Tables[location->Archetype].Set().Contains(id);
	}

	const void *Store::GetRaw(Entity entity, ComponentId id) const {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return nullptr;
		}

		const EntityLocation *location = State->Directory.Locate(key.Index);
		if (location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return nullptr;
		}

		const Column *column = State->Tables[location->Archetype].Find(id);
		if (column == nullptr) {
			return nullptr;
		}

		// A component with no data has no bytes to point at, but present and
		// absent still have to be distinguishable — so a tag reports the column
		// itself. Dereferencing it is meaningless for an empty type either way;
		// what matters is that null means absent.
		return column->Describe().Size == 0 ? static_cast<const void *>(column) : column->At(location->Row);
	}

	void *Store::GetRawMutable(Entity entity, ComponentId id) {
		void *value = const_cast<void *>(GetRaw(entity, id));

		// Handing out a mutable pointer counts as a write. Whether the caller
		// used it is not knowable from here, and a change reported that did not
		// happen costs a consumer one wasted rebuild — where a change missed
		// costs it correctness.
		if (value != nullptr && !State->Watched.empty()) {
			const EntityId key = EntityId::Of(entity);
			const EntityLocation *location = State->Directory.Locate(key.Index);
			if (location != nullptr && location->Archetype != EntityLocation::NO_ARCHETYPE) {
				MarkWritten(*State, location->Archetype, location->Row, id);
			}
		}

		return value;
	}

	void Store::RemoveRaw(Entity entity, ComponentId id) {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return;
		}

		if (State->DeferDepth > 0) {
			State->Commands.push_back(Command{Command::Kind::Remove, entity, id, nullptr});
			return;
		}

		const EntityLocation location = *State->Directory.Locate(key.Index);
		if (location.Archetype == EntityLocation::NO_ARCHETYPE) {
			return;
		}

		const ComponentSet &current = State->Tables[location.Archetype].Set();
		if (!current.Contains(id)) {
			return;
		}

		const ComponentSet &reduced = current.Without(id);
		if (reduced.IsEmpty()) {
			// Back to no components at all, which is a directory slot and no
			// row rather than a row in a table of nothing.
			Vacate(*State, location);
			State->Directory.Relocate(key.Index, EntityLocation{});
			return;
		}

		Relocate(*State, key.Index, location, TableFor(*State, reduced));
	}

	// --- resources ---------------------------------------------------------

	void Store::SetResourceRaw(ComponentId id, const void *value) {
		if (!id.IsValid()) {
			return;
		}

		auto found = State->Resources.find(id.Index);
		if (found == State->Resources.end()) {
			found = State->Resources.emplace(id.Index, Column(id)).first;
		}

		Column &column = found->second;
		if (column.Empty()) {
			column.PushCopy(value);
		} else {
			column.Assign(0, value);
		}
	}

	const void *Store::GetResourceRaw(ComponentId id) const {
		const auto found = State->Resources.find(id.Index);
		if (found == State->Resources.end() || found->second.Empty()) {
			return nullptr;
		}

		const Column &column = found->second;
		return column.Describe().Size == 0 ? static_cast<const void *>(&column) : column.At(0);
	}

	void *Store::GetResourceRawMutable(ComponentId id) {
		return const_cast<void *>(GetResourceRaw(id));
	}

	void Store::RemoveResourceRaw(ComponentId id) {
		State->Resources.erase(id.Index);
	}

	// --- time --------------------------------------------------------------

	WorldTime Store::Time() const {
		// Always present: the constructor sets it and nothing removes it.
		return *Resource<WorldTime>();
	}

	void Store::AdvanceTick(float delta) {
		RequireOwningThread("AdvanceTick");

		auto *time = ResourceMutable<WorldTime>();
		time->Delta = delta;
		time->Elapsed += static_cast<double>(delta);
		time->Tick++;
	}

	void Store::SetFrame(float frameDelta, float alpha) {
		RequireOwningThread("SetFrame");

		auto *time = ResourceMutable<WorldTime>();
		time->FrameDelta = frameDelta;
		time->Alpha = alpha;
	}

	// --- iteration ---------------------------------------------------------

	void Store::VisitTables(
		std::span<const ComponentId> terms, const std::function<void(const TableSlice &)> &body
	) {
		QueryPlan &plan = State->Plans[PlanKey(terms)];

		// Topped up rather than rebuilt. Tables are only ever added, so
		// everything matched before still matches and only the new ones need
		// testing — which is what makes calling this every tick cost nothing
		// after the first frame, and what closes `D00003`.
		if (plan.SeenTables < State->Tables.size()) {
			for (size_t index = plan.SeenTables; index < State->Tables.size(); index++) {
				const ComponentSet &set = State->Tables[index].Set();
				if (!set.ContainsAll(terms)) {
					continue;
				}

				QueryPlan::Match match;
				match.Table = static_cast<uint32_t>(index);
				match.Positions.reserve(terms.size());

				// Resolved in the caller's term order, so the iteration code
				// indexes by parameter position rather than searching per row.
				const std::span<const ComponentId> ids = set.Ids();
				for (const ComponentId term : terms) {
					const auto at = std::lower_bound(ids.begin(), ids.end(), term);
					match.Positions.push_back(static_cast<size_t>(at - ids.begin()));
				}

				plan.Matches.push_back(std::move(match));
			}
			plan.SeenTables = State->Tables.size();
		}

		std::vector<void *> columns(terms.size(), nullptr);

		for (const QueryPlan::Match &match : plan.Matches) {
			Archetype &table = State->Tables[match.Table];
			if (table.Rows() == 0) {
				continue;
			}

			for (size_t term = 0; term < terms.size(); term++) {
				columns[term] = table.ColumnAt(match.Positions[term]).Data();
			}

			TableSlice slice;
			slice.Rows = table.Rows();
			slice.Entities = table.Entities().data();
			slice.Columns = columns.data();

			body(slice);
		}
	}

	size_t Store::CountRows(std::span<const ComponentId> terms) {
		size_t total = 0;
		VisitTables(terms, [&total](const TableSlice &slice) { total += slice.Rows; });
		return total;
	}

	// --- instances ---------------------------------------------------------

	Entity Store::CreateInstance(ClassId id, std::string_view name) {
		RequireOwningThread("CreateInstance");

		const ClassInfo &info = Classes::Describe(id);
		if (info.Set == nullptr) {
			return NULL_ENTITY;
		}

		const Entity entity = Create();
		const EntityId key = EntityId::Of(entity);

		// Straight into the class's archetype rather than one component at a
		// time. Adding them one by one would walk the entity through every
		// intermediate table on the way, creating each of them.
		const uint32_t table = TableFor(*State, *info.Set);
		Relocate(*State, key.Index, EntityLocation{}, table);

		const EntityLocation location = *State->Directory.Locate(key.Index);
		Archetype &archetype = State->Tables[location.Archetype];

		// The prototype row, copied column by column. This is what makes a
		// default a value rather than a constructor, and it is the same
		// operation Clone performs from a different source.
		for (const ComponentId component : info.Set->Ids()) {
			if (const void *value = Classes::DefaultOf(id, component); value != nullptr) {
				archetype.Find(component)->Assign(location.Row, value);
			}
		}

		const InstanceClass declared{id};
		archetype.Find(Components::Of<InstanceClass>())->Assign(location.Row, &declared);

		if (!name.empty()) {
			const InstanceName label{core::Name(name)};
			archetype.Find(Components::Of<InstanceName>())->Assign(location.Row, &label);
		}

		return entity;
	}

	ClassId Store::ClassOf(Entity instance) const {
		const InstanceClass *declared = Get<InstanceClass>(instance);
		return declared == nullptr ? ClassId{} : declared->Class;
	}

	bool Store::IsA(Entity instance, ClassId id) const {
		return Classes::IsA(ClassOf(instance), id);
	}

	core::Name Store::InstanceNameOf(Entity instance) const {
		const InstanceName *label = Get<InstanceName>(instance);
		return label == nullptr ? core::Name{} : label->Value;
	}

	Entity Store::ParentOf(Entity instance) const {
		const Hierarchy *node = Get<Hierarchy>(instance);
		return node == nullptr ? NULL_ENTITY : node->Parent;
	}

	void Store::EachChild(Entity instance, const std::function<void(Entity)> &body) const {
		const Hierarchy *node = Get<Hierarchy>(instance);
		if (node == nullptr) {
			return;
		}

		Entity child = node->FirstChild;
		while (child != NULL_ENTITY) {
			// Read before the body runs, so a body that reparents or destroys
			// the child it was handed does not lose its place in the list.
			const Hierarchy *link = Get<Hierarchy>(child);
			const Entity next = link == nullptr ? NULL_ENTITY : link->NextSibling;

			body(child);
			child = next;
		}
	}

	Entity Store::FindFirstChild(Entity instance, std::string_view name) const {
		const core::Name wanted(name);

		const Hierarchy *node = Get<Hierarchy>(instance);
		if (node == nullptr) {
			return NULL_ENTITY;
		}

		for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
			if (InstanceNameOf(child) == wanted) {
				return child;
			}
			const Hierarchy *link = Get<Hierarchy>(child);
			child = link == nullptr ? NULL_ENTITY : link->NextSibling;
		}
		return NULL_ENTITY;
	}

	bool Store::IsDescendantOf(Entity instance, Entity ancestor) const {
		for (Entity walk = instance; walk != NULL_ENTITY; walk = ParentOf(walk)) {
			if (walk == ancestor) {
				return true;
			}
		}
		return false;
	}

	bool Store::SetParent(Entity instance, Entity parent) {
		RequireOwningThread("SetParent");

		Hierarchy *node = GetMutable<Hierarchy>(instance);
		if (node == nullptr) {
			return false;
		}
		if (parent != NULL_ENTITY && GetMutable<Hierarchy>(parent) == nullptr) {
			return false;
		}

		// A cycle is not a wrong answer, it is a hang in every walk of the
		// tree — including the one that would destroy it.
		if (parent != NULL_ENTITY && IsDescendantOf(parent, instance)) {
			return false;
		}

		// --- unlink from the old parent ---
		if (node->Parent != NULL_ENTITY) {
			Hierarchy *previous =
				node->PreviousSibling == NULL_ENTITY ? nullptr : GetMutable<Hierarchy>(node->PreviousSibling);
			Hierarchy *next =
				node->NextSibling == NULL_ENTITY ? nullptr : GetMutable<Hierarchy>(node->NextSibling);

			if (previous != nullptr) {
				previous->NextSibling = node->NextSibling;
			}
			if (next != nullptr) {
				next->PreviousSibling = node->PreviousSibling;
			}

			if (Hierarchy *old = GetMutable<Hierarchy>(node->Parent); old != nullptr) {
				if (old->FirstChild == instance) {
					old->FirstChild = node->NextSibling;
				}
				if (old->LastChild == instance) {
					old->LastChild = node->PreviousSibling;
				}
			}

			// Re-read: every GetMutable above may have moved this row.
			node = GetMutable<Hierarchy>(instance);
		}

		node->Parent = NULL_ENTITY;
		node->NextSibling = NULL_ENTITY;
		node->PreviousSibling = NULL_ENTITY;

		if (parent == NULL_ENTITY) {
			return true;
		}

		// --- link at the end of the new parent, so order is insertion order ---
		Hierarchy *host = GetMutable<Hierarchy>(parent);
		const Entity last = host->LastChild;

		if (last == NULL_ENTITY) {
			host->FirstChild = instance;
			host->LastChild = instance;
		} else {
			GetMutable<Hierarchy>(last)->NextSibling = instance;
			GetMutable<Hierarchy>(parent)->LastChild = instance;
		}

		node = GetMutable<Hierarchy>(instance);
		node->Parent = parent;
		node->PreviousSibling = last;
		return true;
	}

	void Store::DestroyInstance(Entity instance) {
		RequireOwningThread("DestroyInstance");

		if (!Alive(instance)) {
			return;
		}

		// Children collected before anything is destroyed, because destroying
		// one rewrites the sibling links the walk is standing on.
		std::vector<Entity> children;
		EachChild(instance, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			DestroyInstance(child);
		}

		SetParent(instance, NULL_ENTITY);
		Destroy(instance);
	}

	Entity Store::CloneOne(Entity source) {
		const EntityId key = EntityId::Of(source);
		const EntityLocation from = *State->Directory.Locate(key.Index);
		if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
			return NULL_ENTITY;
		}

		const ComponentSet &set = State->Tables[from.Archetype].Set();

		const Entity copy = Create();
		const EntityId copyKey = EntityId::Of(copy);
		const uint32_t table = TableFor(*State, set);
		Relocate(*State, copyKey.Index, EntityLocation{}, table);

		// The source row may have moved when the copy was placed, so it is
		// re-read rather than remembered.
		const EntityLocation now = *State->Directory.Locate(key.Index);
		const EntityLocation into = *State->Directory.Locate(copyKey.Index);

		Archetype &destination = State->Tables[into.Archetype];
		Archetype &origin = State->Tables[now.Archetype];

		for (const ComponentId component : set.Ids()) {
			destination.Find(component)->Assign(into.Row, origin.Find(component)->At(now.Row));
		}

		// A copy belongs to no tree until somebody parents it.
		Hierarchy detached;
		destination.Find(Components::Of<Hierarchy>())->Assign(into.Row, &detached);

		return copy;
	}

	Entity Store::CloneInstance(Entity source) {
		RequireOwningThread("CloneInstance");

		if (!Alive(source) || Get<Hierarchy>(source) == nullptr) {
			return NULL_ENTITY;
		}

		const Entity copy = CloneOne(source);
		if (copy == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		std::vector<Entity> children;
		EachChild(source, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			const Entity copied = CloneInstance(child);
			if (copied != NULL_ENTITY) {
				SetParent(copied, copy);
			}
		}

		return copy;
	}

	// --- change tracking ---------------------------------------------------

	void Store::ObserveRaw(ComponentId id) {
		if (!id.IsValid() || ObservedRaw(id)) {
			return;
		}

		State->Watched.push_back(id);

		// Every table already holding this component needs somewhere to put the
		// bits, and the entities in it have to move there. Declaring what is
		// observed when a world is built avoids all of this; doing it late is
		// supported because a debugging session should not have to restart, not
		// because it is free.
		std::vector<Entity> moving;
		for (const Archetype &table : State->Tables) {
			if (!table.Set().Contains(id) || table.Set().Contains(Components::Of<DirtyBits>())) {
				continue;
			}
			for (const Entity entity : table.Entities()) {
				moving.push_back(entity);
			}
		}

		for (const Entity entity : moving) {
			const EntityId key = EntityId::Of(entity);
			const EntityLocation location = *State->Directory.Locate(key.Index);
			const ComponentSet &set = State->Tables[location.Archetype].Set();
			Relocate(*State, key.Index, location, TableFor(*State, set));
		}
	}

	bool Store::ObservedRaw(ComponentId id) const {
		return std::find(State->Watched.begin(), State->Watched.end(), id) != State->Watched.end();
	}

	bool Store::ChangedRaw(Entity entity, ComponentId id) const {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation) || !id.IsValid()) {
			return false;
		}

		const EntityLocation *location = State->Directory.Locate(key.Index);
		if (location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return false;
		}

		const Archetype &table = State->Tables[location->Archetype];
		const Column *bits = table.Find(Components::Of<DirtyBits>());
		if (bits == nullptr) {
			return false;
		}

		const std::span<const ComponentId> ids = table.Set().Ids();
		const auto at = std::lower_bound(ids.begin(), ids.end(), id);
		if (at == ids.end() || *at != id) {
			return false;
		}

		return static_cast<const DirtyBits *>(bits->At(location->Row))
			->Test(static_cast<size_t>(at - ids.begin()));
	}

	void Store::VisitChanged(
		std::span<const ComponentId> terms,
		ComponentId subject,
		const std::function<void(Entity, void *)> &body
	) {
		VisitTables(terms, [&](const TableSlice &slice) {
			// The bits are the second term, and the subject the first, because
			// EachChanged names them in that order.
			auto *bits = static_cast<DirtyBits *>(slice.Columns[1]);
			auto *values = static_cast<std::byte *>(slice.Columns[0]);
			const size_t stride = Components::Describe(subject).Size;

			// The position the subject occupies in this table, which is what
			// its bit index is. Resolved once per table rather than per row.
			size_t position = 0;
			{
				const EntityId key = EntityId::Of(slice.Entities[0]);
				const EntityLocation *location = State->Directory.Locate(key.Index);
				const std::span<const ComponentId> ids = State->Tables[location->Archetype].Set().Ids();
				const auto at = std::lower_bound(ids.begin(), ids.end(), subject);
				position = static_cast<size_t>(at - ids.begin());
			}

			for (size_t row = 0; row < slice.Rows; row++) {
				if (bits[row].Test(position)) {
					body(slice.Entities[row], values + row * stride);
				}
			}
		});
	}

	Store::Connection Store::Listen(ComponentId id, std::function<void(Store &, Entity, const void *)> body) {
		StoreState::Listener listener;
		listener.Subject = id;
		listener.Id = State->NextConnection++;
		listener.Body = std::move(body);

		State->Listeners.push_back(std::move(listener));
		return Connection{State->Listeners.back().Id};
	}

	bool Store::Disconnect(Connection connection) {
		RequireOwningThread("Disconnect");

		if (!connection.Valid()) {
			return false;
		}

		const auto found = std::find_if(
			State->Listeners.begin(), State->Listeners.end(), [connection](const auto &listener) {
				return listener.Id == connection.Id;
			}
		);

		if (found == State->Listeners.end()) {
			return false;
		}

		State->Listeners.erase(found);
		return true;
	}

	size_t Store::Listeners() const {
		return State->Listeners.size();
	}

	size_t Store::FlushSignals() {
		RequireOwningThread("FlushSignals");

		if (State->Listeners.empty() || State->Flushing) {
			return 0;
		}

		State->Flushing = true;
		size_t fired = 0;

		// By subject, so the changed set for one component is walked once
		// however many listeners want it. A listener list is short and a table
		// walk is not, which is the way round that decides the loop order.
		std::vector<ComponentId> subjects;
		for (const StoreState::Listener &listener : State->Listeners) {
			if (std::find(subjects.begin(), subjects.end(), listener.Subject) == subjects.end()) {
				subjects.push_back(listener.Subject);
			}
		}

		for (const ComponentId subject : subjects) {
			// Collected before anything fires. A listener may add or remove a
			// component, which moves rows between tables — and an iteration
			// that was still walking one of them would be walking a table that
			// no longer holds what it thought.
			State->Firing.clear();
			{
				const ComponentId terms[] = {subject, Components::Of<DirtyBits>()};
				const DeferScope defer(*this);
				VisitChanged(terms, subject, [this](Entity entity, void *) {
					State->Firing.push_back(entity);
				});
			}

			for (const Entity entity : State->Firing) {
				for (const StoreState::Listener &listener : State->Listeners) {
					if (listener.Subject != subject) {
						continue;
					}

					// Re-checked per call rather than once: an earlier listener
					// may have destroyed the entity or taken the component
					// away, and handing the next one a pointer into a row that
					// has moved is the bug this whole shape exists to avoid.
					const void *value = GetRaw(entity, subject);
					if (value == nullptr) {
						break;
					}

					listener.Body(*this, entity, value);
					fired++;
				}
			}
		}

		State->Flushing = false;
		return fired;
	}

	void Store::ClearChanges() {
		RequireOwningThread("ClearChanges");

		const ComponentId id = Components::Of<DirtyBits>();
		for (Archetype &table : State->Tables) {
			Column *bits = table.Find(id);
			if (bits == nullptr || bits->Empty()) {
				continue;
			}
			std::memset(bits->Data(), 0, bits->Size() * sizeof(DirtyBits));
		}
	}

	uint64_t Store::ChangeVersion() const {
		return State->Changes;
	}

	// --- snapshots ---------------------------------------------------------

	namespace {
		// Recognises a snapshot before anything else is read.
		//
		// Eight bytes rather than a version alone, so a reader handed a stream
		// that is not a snapshot at all fails on the first field instead of
		// interpreting arbitrary bytes as a table count.
		constexpr uint64_t SNAPSHOT_MAGIC = 0x504E'534F'4E4F'4D55ull;
	}

	bool Store::Save(core::ByteWriter &writer) const {
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
		core::ByteWriter body;

		body.WriteUInt32(static_cast<uint32_t>(State->Tables.size()));
		for (const Archetype &table : State->Tables) {
			body.WriteUInt32(static_cast<uint32_t>(table.Set().Size()));
			for (const ComponentId id : table.Set().Ids()) {
				const TypeDescriptor &descriptor = Components::Describe(id);
				if (descriptor.Size > 0 && !descriptor.Serialisable) {
					ENGINE_ERROR(
						"store '{}': cannot snapshot, component '{}' has no serialisation.",
						StoreName,
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
		// in, so two saves of the same world produced different bytes — and a
		// re-save after a load produced different bytes again, because the load
		// inserted in a different order. A snapshot that is not byte-stable
		// cannot be compared, which is what a recording and a CI determinism
		// job both do. Sorted by text rather than by id, because ids are
		// assigned in interning order and that differs after a restore.
		std::vector<uint32_t> resourceKeys;
		resourceKeys.reserve(State->Resources.size());
		for (const auto &entry : State->Resources) {
			resourceKeys.push_back(entry.first);
		}
		std::sort(resourceKeys.begin(), resourceKeys.end(), [](uint32_t left, uint32_t right) {
			return Components::Describe(ComponentId{left}).Name.Text() <
				   Components::Describe(ComponentId{right}).Name.Text();
		});

		body.WriteUInt32(static_cast<uint32_t>(resourceKeys.size()));
		for (const uint32_t index : resourceKeys) {
			const Column &column = State->Resources.at(index);
			const ComponentId id{index};
			const TypeDescriptor &descriptor = Components::Describe(id);
			if (descriptor.Size > 0 && !descriptor.Serialisable) {
				ENGINE_ERROR(
					"store '{}': cannot snapshot, resource '{}' has no serialisation.",
					StoreName,
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
		namedIndices.reserve(State->NamesByIndex.size());
		for (const auto &entry : State->NamesByIndex) {
			namedIndices.push_back(entry.first);
		}
		std::sort(namedIndices.begin(), namedIndices.end());

		body.WriteUInt32(static_cast<uint32_t>(namedIndices.size()));
		for (const uint32_t index : namedIndices) {
			body.WriteUInt32(index);
			body.WriteString(State->NamesByIndex.at(index));
		}

		body.WriteUInt32(static_cast<uint32_t>(State->Watched.size()));
		for (const ComponentId id : State->Watched) {
			body.WriteUInt32(ordinalOf(id));
		}

		// --- the header, now that the component table is complete ---

		writer.WriteUInt64(SNAPSHOT_MAGIC);
		writer.WriteUInt32(SNAPSHOT_VERSION);
		writer.WriteString(StoreName);

		writer.WriteUInt32(static_cast<uint32_t>(mentioned.size()));
		for (const ComponentId id : mentioned) {
			writer.WriteName(Components::Describe(id).Name);
		}

		// The directory, exactly as it stands. Generations included, because a
		// handle stored inside a component is only valid if its generation
		// comes back too.
		const size_t issued = State->Directory.Capacity();
		writer.WriteUInt64(issued);
		for (size_t index = 0; index < issued; index++) {
			writer.WriteUInt32(State->Directory.Generation(static_cast<uint32_t>(index)));
			writer.WriteBool(State->Directory.Live(static_cast<uint32_t>(index)));
		}

		writer.WriteRaw(body.Bytes().data(), body.Size());
		return true;
	}

	bool Store::Load(core::ByteReader &reader) {
		RequireOwningThread("Load");
		Clear();

		if (reader.ReadUInt64() != SNAPSHOT_MAGIC) {
			ENGINE_ERROR("store '{}': not a snapshot.", StoreName);
			Clear();
			return false;
		}

		const uint32_t version = reader.ReadUInt32();
		if (version != SNAPSHOT_VERSION) {
			ENGINE_ERROR(
				"store '{}': snapshot version {}, this build reads {}.", StoreName, version, SNAPSHOT_VERSION
			);
			Clear();
			return false;
		}

		StoreName = std::string(reader.ReadString());

		// Names to this process's ids. A component the snapshot names and this
		// build does not have is a refusal rather than a gap: the rows carrying
		// it would be silently narrower than they were written.
		const uint32_t componentCount = reader.ReadUInt32();
		std::vector<ComponentId> resolved;
		resolved.reserve(componentCount);
		for (uint32_t index = 0; index < componentCount && !reader.Failed(); index++) {
			const core::Name name = reader.ReadName();
			const ComponentId id = Components::Find(name);
			if (!id.IsValid()) {
				ENGINE_ERROR(
					"store '{}': snapshot names component '{}', which this build does not have.",
					StoreName,
					name.Text()
				);
				Clear();
				return false;
			}
			resolved.push_back(id);
		}

		const auto lookup = [&resolved](uint32_t ordinal) {
			return ordinal < resolved.size() ? resolved[ordinal] : ComponentId{};
		};

		const uint64_t issued = reader.ReadUInt64();
		for (uint64_t index = 0; index < issued && !reader.Failed(); index++) {
			const uint32_t generation = reader.ReadUInt32();
			const bool live = reader.ReadBool();
			State->Directory.Restore(static_cast<uint32_t>(index), generation, live);
		}
		if (reader.Failed()) {
			Clear();
			return false;
		}
		State->Directory.FinishRestore(static_cast<size_t>(issued));

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
			const auto table = static_cast<uint32_t>(State->Tables.size());
			State->Tables.emplace_back(set);
			State->TableBySet.emplace(set.Id(), table);

			if (!State->Tables.back().Read(reader, static_cast<size_t>(rows))) {
				Clear();
				return false;
			}

			// Every row's entity now knows where it lives again.
			const Archetype &restored = State->Tables.back();
			for (size_t row = 0; row < restored.Rows(); row++) {
				const EntityId key = EntityId::Of(restored.EntityAt(row));
				State->Directory.Relocate(key.Index, EntityLocation{table, static_cast<uint32_t>(row)});
			}
		}

		const uint32_t resourceCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < resourceCount && !reader.Failed(); index++) {
			const ComponentId id = lookup(reader.ReadUInt32());
			if (!id.IsValid()) {
				Clear();
				return false;
			}

			Column column(id);
			if (!column.Read(reader, 1)) {
				Clear();
				return false;
			}
			State->Resources.insert_or_assign(id.Index, std::move(column));
		}

		const uint32_t nameCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < nameCount && !reader.Failed(); index++) {
			const uint32_t owner = reader.ReadUInt32();
			const std::string name(reader.ReadString());
			if (reader.Failed()) {
				break;
			}
			State->NamesByIndex.insert_or_assign(owner, name);
			State->EntitiesByName.insert_or_assign(
				name, EntityId::Pack(owner, State->Directory.Generation(owner))
			);
		}

		const uint32_t watchedCount = reader.ReadUInt32();
		for (uint32_t index = 0; index < watchedCount && !reader.Failed(); index++) {
			const ComponentId id = lookup(reader.ReadUInt32());
			if (id.IsValid()) {
				State->Watched.push_back(id);
			}
		}

		if (reader.Failed()) {
			Clear();
			return false;
		}

		// The clock is a resource and came back with the rest, but a snapshot
		// written before one existed would leave the world without one.
		if (!HasResource<WorldTime>()) {
			SetResource(WorldTime{});
		}

		return true;
	}

	bool Store::Apply(core::ByteReader &reader, ApplyMode mode) {
		RequireOwningThread("Apply");

		// Read into a scratch world first, so a corrupt snapshot cannot leave
		// the live one half-merged. The live world is only touched once the
		// whole thing has parsed.
		//
		// A second store rather than an in-place parse: correctness first, and
		// the in-place version is an optimisation with a measurement attached
		// rather than a starting point. `ecs/docs/TODO.md` carries it.
		Store scratch("apply.scratch");
		if (!scratch.Load(reader)) {
			return false;
		}

		// --- what the snapshot knows about ---
		std::vector<Entity> incoming;
		for (const Archetype &table : scratch.State->Tables) {
			for (const Entity entity : table.Entities()) {
				incoming.push_back(entity);
			}
		}

		// --- entities here that the snapshot does not mention ---
		if (mode == ApplyMode::Authoritative) {
			std::vector<Entity> stale;
			for (const Archetype &table : State->Tables) {
				for (const Entity entity : table.Entities()) {
					const EntityId key = EntityId::Of(entity);
					if (!scratch.State->Directory.Alive(key.Index, key.Generation)) {
						stale.push_back(entity);
					}
				}
			}
			for (const Entity entity : stale) {
				Destroy(entity);
			}
		}

		// --- bring every incoming entity into line ---
		for (const Entity entity : incoming) {
			const EntityId key = EntityId::Of(entity);

			if (!State->Directory.Alive(key.Index, key.Generation)) {
				// Not here, or here at a different generation — which means the
				// sender destroyed and recreated it, so this is a different
				// entity and the old one goes.
				if (State->Directory.Live(key.Index)) {
					Destroy(EntityId::Pack(key.Index, State->Directory.Generation(key.Index)));
				}

				// Restored at the sender's index *and* generation, so a handle
				// held anywhere — including inside another component — still
				// names the same entity on both sides.
				State->Directory.Restore(key.Index, key.Generation, true);
				State->Directory.FinishRestore(
					std::max(State->Directory.Capacity(), static_cast<size_t>(key.Index) + 1)
				);
			}

			// The components the sender says it has, and only those: a
			// component the sender dropped has to be dropped here too, or a
			// replica accumulates state the authority no longer believes in.
			const EntityLocation from = *scratch.State->Directory.Locate(key.Index);
			const ComponentSet &wanted = from.Archetype == EntityLocation::NO_ARCHETYPE
											 ? ComponentSet::Empty()
											 : scratch.State->Tables[from.Archetype].Set();

			const EntityLocation here = *State->Directory.Locate(key.Index);
			if (here.Archetype != EntityLocation::NO_ARCHETYPE) {
				const ComponentSet &held = State->Tables[here.Archetype].Set();
				for (const ComponentId id : held.Ids()) {
					if (!wanted.Contains(id)) {
						RemoveRaw(entity, id);
					}
				}
			}

			for (const ComponentId id : wanted.Ids()) {
				const Column *column = scratch.State->Tables[from.Archetype].Find(id);
				SetRaw(entity, id, column->At(from.Row));
			}
		}

		// --- resources and the clock ---
		for (const auto &[index, column] : scratch.State->Resources) {
			SetResourceRaw(ComponentId{index}, column.At(0));
		}

		return true;
	}

	void Store::Clear() {
		State->Tables.clear();
		State->TableBySet.clear();
		State->Directory.Clear();
		State->NamesByIndex.clear();
		State->EntitiesByName.clear();
		State->Resources.clear();
		State->Plans.clear();
		State->Commands.clear();
		State->Watched.clear();
		State->DeferDepth = 0;

		SetResource(WorldTime{});
	}

	// --- deferral ----------------------------------------------------------

	void Store::BeginDefer() {
		State->DeferDepth++;
	}

	void Store::EndDefer() {
		State->DeferDepth--;
		if (State->DeferDepth > 0 || State->Commands.empty()) {
			return;
		}

		// Taken by move before replaying, because applying a command may itself
		// defer — a nested Each, or a Destroy that cascades. Replaying out of
		// the live vector would walk a container being appended to.
		std::vector<Command> commands;
		commands.swap(State->Commands);

		for (const Command &command : commands) {
			switch (command.What) {
			case Command::Kind::Destroy:
				Destroy(command.Target);
				break;
			case Command::Kind::Set:
				SetRaw(command.Target, command.Component, command.Payload);
				break;
			case Command::Kind::Remove:
				RemoveRaw(command.Target, command.Component);
				break;
			}

			if (command.Payload != nullptr) {
				const TypeDescriptor &descriptor = Components::Describe(command.Component);
				descriptor.Destruct(command.Payload, 1);
				::operator delete(command.Payload, std::align_val_t(descriptor.Alignment));
			}
		}
	}
}
