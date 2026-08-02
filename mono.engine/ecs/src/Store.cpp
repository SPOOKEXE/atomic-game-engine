#include "Instances.hpp"
#include "Snapshot.hpp"
#include "StoreState.hpp"

#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace engine::ecs {

	namespace {
		// The key a query plan is cached under: the sorted term ids as raw
		// bytes, so two systems asking for the same components in different
		// orders share one plan.
		//
		// **Built on the caller's stack, not on the heap.** This runs once per
		// query per system per tick — the hottest non-row path in the engine —
		// and the previous shape allocated a vector *and* a string on every
		// call, to look up a cache entry that was almost always already there.
		// A stack buffer plus a transparent hash makes the hit path allocate
		// nothing; only an insert has to own its bytes.
		//
		// Nesting-safe by construction: the storage is a local, so a system
		// iterating inside an iteration gets its own.
		struct PlanKey {
			uint32_t Inline[INLINE_QUERY_TERMS];
			std::vector<uint32_t> Overflow;
			std::string_view Bytes;

			explicit PlanKey(std::span<const ComponentId> terms) {
				uint32_t *sorted = Inline;
				if (terms.size() > INLINE_QUERY_TERMS) {
					// Absurd, but it must work rather than corrupt the stack.
					Overflow.resize(terms.size());
					sorted = Overflow.data();
				}

				for (size_t index = 0; index < terms.size(); index++) {
					sorted[index] = terms[index].Index;
				}
				std::sort(sorted, sorted + terms.size());

				Bytes =
					std::string_view(reinterpret_cast<const char *>(sorted), terms.size() * sizeof(uint32_t));
			}
		};
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

		return CreateEntity(*State);
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

		DestroyEntity(*State, entity);
	}

	bool Store::Alive(Entity entity) const {
		return IsEntityAlive(*State, entity);
	}

	bool Store::CreateAt(Entity entity) {
		RequireOwningThread("CreateAt");

		const EntityId key = EntityId::Of(entity);
		if (State->Directory.Alive(key.Index, key.Generation)) {
			return false;
		}

		// The sender's index *and* generation. Matching on the index alone
		// would let a recycled slot arrive wearing the old entity's identity.
		State->Directory.Restore(key.Index, key.Generation, true);
		State->Directory.FinishRestore(
			std::max(State->Directory.Capacity(), static_cast<size_t>(key.Index) + 1)
		);
		return true;
	}

	void Store::EachEntity(const std::function<void(Entity)> &body) {
		RequireOwningThread("EachEntity");

		// Deferred, as `Each` is. The directory is being walked and a create
		// inside the body would grow the very thing being iterated.
		const DeferScope defer(*this);

		// The directory rather than the tables, because an entity carrying no
		// components at all is in no table and is still an entity.
		const size_t capacity = State->Directory.Capacity();
		for (uint32_t index = 0; index < capacity; index++) {
			if (!State->Directory.Live(index)) {
				continue;
			}
			body(EntityId::Pack(index, State->Directory.Generation(index)));
		}
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

	// Qualified, every one of them. The public runtime-keyed methods below
	// share these names deliberately — they are the same operation, one taking
	// a state and one taking a `Store` — and unqualified lookup inside a member
	// finds the member first and recurses.
	void Store::SetRaw(Entity entity, ComponentId id, const void *value) {
		engine::ecs::SetComponent(*State, entity, id, value);
	}

	bool Store::HasRaw(Entity entity, ComponentId id) const {
		return engine::ecs::HasComponent(*State, entity, id);
	}

	const void *Store::GetRaw(Entity entity, ComponentId id) const {
		return engine::ecs::GetComponent(*State, entity, id);
	}

	void *Store::GetRawMutable(Entity entity, ComponentId id) {
		return engine::ecs::GetComponentMutable(*State, entity, id);
	}

	void Store::RemoveRaw(Entity entity, ComponentId id) {
		engine::ecs::RemoveComponent(*State, entity, id);
	}

	// --- resources ---------------------------------------------------------

	void Store::SetResourceRaw(ComponentId id, const void *value) {
		SetResourceValue(*State, id, value);
	}

	const void *Store::GetResourceRaw(ComponentId id) const {
		return GetResourceValue(*State, id);
	}

	void *Store::GetResourceRawMutable(ComponentId id) {
		return const_cast<void *>(GetResourceRaw(id));
	}

	void Store::RemoveResourceRaw(ComponentId id) {
		State->Resources.Erase(id.Index);
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
		const PlanKey key(terms);

		// Found without allocating; inserted only the first time, when the key
		// finally has to own its bytes.
		auto found = State->Plans.find(key.Bytes);
		if (found == State->Plans.end()) {
			found = State->Plans.emplace(std::string(key.Bytes), QueryPlan{}).first;
		}
		QueryPlan &plan = found->second;

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

		// On the stack for the same reason the key is: a heap allocation per
		// query per tick, to hold at most a handful of pointers.
		void *inlineColumns[INLINE_QUERY_TERMS];
		std::vector<void *> overflowColumns;

		void **columns = inlineColumns;
		if (terms.size() > INLINE_QUERY_TERMS) {
			overflowColumns.resize(terms.size());
			columns = overflowColumns.data();
		}

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
			slice.Columns = columns;

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

		return engine::ecs::CreateInstance(*State, id, name);
	}

	ClassId Store::ClassOf(Entity instance) const {
		return engine::ecs::ClassOf(*State, instance);
	}

	bool Store::IsA(Entity instance, ClassId id) const {
		return engine::ecs::IsA(*State, instance, id);
	}

	core::Name Store::InstanceNameOf(Entity instance) const {
		return engine::ecs::InstanceNameOf(*State, instance);
	}

	Entity Store::ParentOf(Entity instance) const {
		return engine::ecs::ParentOf(*State, instance);
	}

	void Store::EachChild(Entity instance, const std::function<void(Entity)> &body) const {
		engine::ecs::EachChild(*State, instance, body);
	}

	Entity Store::FindFirstChild(Entity instance, std::string_view name) const {
		return engine::ecs::FindFirstChild(*State, instance, name);
	}

	bool Store::IsDescendantOf(Entity instance, Entity ancestor) const {
		return engine::ecs::IsDescendantOf(*State, instance, ancestor);
	}

	bool Store::SetParent(Entity instance, Entity parent) {
		RequireOwningThread("SetParent");

		return engine::ecs::SetParent(*State, instance, parent);
	}

	void Store::DestroyInstance(Entity instance) {
		RequireOwningThread("DestroyInstance");

		engine::ecs::DestroyInstance(*State, instance);
	}

	Entity Store::CloneInstance(Entity source) {
		RequireOwningThread("CloneInstance");

		return engine::ecs::CloneInstance(*State, source);
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

	void Store::SetComponent(Entity entity, ComponentId component, const void *value) {
		SetRaw(entity, component, value);
	}

	bool Store::HasComponent(Entity entity, ComponentId component) const {
		return HasRaw(entity, component);
	}

	const void *Store::GetComponent(Entity entity, ComponentId component) const {
		return GetRaw(entity, component);
	}

	void Store::RemoveComponent(Entity entity, ComponentId component) {
		RemoveRaw(entity, component);
	}

	void Store::EachChangedRuns(
		ComponentId component, const std::function<void(const Entity *, void *, size_t)> &body
	) {
		RequireOwningThread("EachChangedRuns");
		if (!component.IsValid()) {
			return;
		}

		const ComponentId terms[] = {component, Components::Of<DirtyBits>()};
		const DeferScope defer(*this);
		VisitChangedRuns(terms, component, body);
	}

	void Store::VisitChangedRuns(
		std::span<const ComponentId> terms,
		ComponentId subject,
		const std::function<void(const Entity *, void *, size_t)> &body
	) {
		VisitTables(terms, [&](const TableSlice &slice) {
			auto *bits = static_cast<DirtyBits *>(slice.Columns[1]);
			auto *values = static_cast<std::byte *>(slice.Columns[0]);
			const size_t stride = Components::Describe(subject).Size;

			// The subject's bit index in this table, resolved once per table.
			size_t position = 0;
			{
				const EntityId key = EntityId::Of(slice.Entities[0]);
				const EntityLocation *location = State->Directory.Locate(key.Index);
				const std::span<const ComponentId> ids = State->Tables[location->Archetype].Set().Ids();
				const auto at = std::lower_bound(ids.begin(), ids.end(), subject);
				position = static_cast<size_t>(at - ids.begin());
			}

			// Runs of adjacent set bits. A system walks a table in order and
			// writes as it goes, so the changed rows are usually one run or a
			// few — which is what makes a delta a memcpy per run rather than a
			// copy per entity.
			size_t row = 0;
			while (row < slice.Rows) {
				if (!bits[row].Test(position)) {
					row++;
					continue;
				}

				const size_t start = row;
				while (row < slice.Rows && bits[row].Test(position)) {
					row++;
				}

				body(slice.Entities + start, values + start * stride, row - start);
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

	bool Store::Save(core::ByteWriter &writer) const {
		return SaveSnapshot(*State, StoreName, writer);
	}

	bool Store::Load(core::ByteReader &reader) {
		RequireOwningThread("Load");

		return LoadSnapshot(*State, StoreName, reader);
	}

	bool Store::Apply(core::ByteReader &reader, ApplyMode mode) {
		RequireOwningThread("Apply");

		return ApplySnapshot(*State, reader, mode);
	}

	void Store::Clear() {
		RequireOwningThread("Clear");

		ClearWorld(*State);
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
