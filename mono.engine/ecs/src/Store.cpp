#include "Instances.hpp"
#include "Promotion.hpp"
#include "Snapshot.hpp"
#include "StoreState.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace engine::ecs {

	namespace {
		// The key a query plan is cached under: the term ids as raw bytes, in
		// the caller's order and with the required and excluded sets after
		// them.
		//
		// **In the caller's order, and that is a correction rather than a
		// choice.** It used to sort them, so that `Each<A, B>` and `Each<B, A>`
		// shared one plan - and `Match::Positions` is built in the *caller's*
		// term order, so the second of those two got the first's column
		// bindings and read an `A` where it had asked for a `B`. Silent, and
		// wrong in the way that is hardest to see: the types line up, the loop
		// runs, and the numbers are somebody else's. Sharing a plan between two
		// orders was never worth anything - a plan is a handful of indices -
		// and it cost correctness.
		//
		// **Built on the caller's stack, not on the heap.** This runs once per
		// query per system per tick - the hottest non-row path in the engine -
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

			explicit PlanKey(const QueryTerms &terms) {
				// **The two filter sets are sorted and the bound terms are
				// not.** Order is meaningful for the bound terms, because it is
				// what the body's parameters are matched against; `With` and
				// `Without` are sets and nothing downstream reads their order,
				// so normalising them keeps two spellings of one filter on one
				// plan.
				//
				// `ComponentId::INVALID` separates the three runs. No
				// registration produces it, so it cannot be mistaken for a term
				// - which is what stops a bound `{A, B}` keying the same bytes
				// as a bound `{A}` that excludes `B`.
				const size_t total = terms.Bound.size() + terms.Required.size() + terms.Excluded.size() + 2;

				uint32_t *keyed = Inline;
				if (total > INLINE_QUERY_TERMS) {
					// Absurd, but it must work rather than corrupt the stack.
					Overflow.resize(total);
					keyed = Overflow.data();
				}

				size_t at = 0;
				for (const ComponentId term : terms.Bound) {
					keyed[at++] = term.Index;
				}

				const auto appendSorted = [&](std::span<const ComponentId> ids) {
					keyed[at++] = ComponentId::INVALID;
					const size_t from = at;
					for (const ComponentId id : ids) {
						keyed[at++] = id.Index;
					}
					std::sort(keyed + from, keyed + at);
				};

				appendSorted(terms.Required);
				appendSorted(terms.Excluded);

				Bytes = std::string_view(reinterpret_cast<const char *>(keyed), at * sizeof(uint32_t));
			}
		};
	}

	Store::Store(std::string_view name)
		: State(new StoreState()), StoreName(name), Owner(CallingThreadToken()) {
		// **The other door into the instance model.** A world can be made and
		// filled from a snapshot without a single `Classes::Register` running
		// first, and the names in that snapshot have to mean what they meant
		// when it was written. See `RegisterInstanceComponents`.
		RegisterInstanceComponents();

		// The clock is the one resource that is always present, so that no
		// system has to check whether the world has a time.
		SetResource(WorldTime{});

		ENGINE_TRACE("store '{}' created", StoreName);
	}

	Store::~Store() {
		delete State;
	}

	void Store::BindToCallingThread() {
		Owner.store(CallingThreadToken(), std::memory_order_relaxed);
	}

	void Store::TooManyFilterTerms(const char *what) const {
		// Abort rather than drop the term. A `Without` that quietly did nothing
		// would be a query silently matching rows it was written to skip, which
		// is worse than not running at all.
		ENGINE_ERROR(
			"store '{}': a query named more than {} `{}` terms. A filter is a handful of components; "
			"past that it has stopped describing a set.",
			StoreName,
			Store::MAX_FILTER_TERMS,
			what
		);
		std::abort();
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

	bool Store::MayMintAuthoritative(const char *what) {
		if (!State->AdoptOnly) {
			return true;
		}

		// Once, not once per attempt. A system that creates in a loop would
		// otherwise write a log line per entity, and the first one already said
		// everything the rest would.
		if (!State->WarnedAboutMinting) {
			State->WarnedAboutMinting = true;
			ENGINE_ERROR(
				"store '{}': refusing {} in a replica. An authoritative index minted here collides "
				"with one the authority minted, and nothing can tell them apart. Use CreatePredicted "
				"for something this world is predicting.",
				StoreName,
				what
			);
		}
		return false;
	}

	Entity Store::Create() {
		RequireOwningThread("Create");

		if (!MayMintAuthoritative("Create")) {
			return NULL_ENTITY;
		}

		const Entity entity = CreateEntity(*State, EntityRange::Authoritative);
		if (entity == NULL_ENTITY) {
			ENGINE_ERROR(
				"store '{}': the authoritative index range is exhausted at {} indices. Nothing wraps "
				"into the predicted range, so this is a refusal rather than a collision.",
				StoreName,
				SparseSet::AUTHORITATIVE_INDICES
			);
		}
		return entity;
	}

	Entity Store::CreatePredicted() {
		RequireOwningThread("CreatePredicted");

		// No adopt-only check, and that is the point of the range: a replica may
		// mint here because the authority never allocates from it.
		const Entity entity = CreateEntity(*State, EntityRange::Predicted);
		if (entity == NULL_ENTITY) {
			ENGINE_ERROR(
				"store '{}': the predicted index range is exhausted at {} indices. Nothing wraps into "
				"the authoritative range, so this is a refusal rather than a collision.",
				StoreName,
				SparseSet::PREDICTED_INDICES
			);
		}
		return entity;
	}

	bool Store::IsPredicted(Entity entity) {
		return entity != NULL_ENTITY && SparseSet::IsPredicted(EntityId::Of(entity).Index);
	}

	bool Store::Promote(Entity predicted, Entity authoritative) {
		RequireOwningThread("Promote");

		return PromoteEntity(*State, StoreName, predicted, authoritative);
	}

	void Store::SetAdoptOnly(bool adoptOnly) {
		RequireOwningThread("SetAdoptOnly");

		State->AdoptOnly = adoptOnly;
	}

	bool Store::AdoptOnly() const {
		return State->AdoptOnly;
	}

	void Store::Protect(Entity instance) {
		RequireOwningThread("Protect");

		if (instance != NULL_ENTITY) {
			State->Protected.insert(instance.Id);
		}
	}

	bool Store::Protected(Entity instance) const {
		return State->Protected.find(instance.Id) != State->Protected.end();
	}

	bool Store::DestroyAuthored(Entity instance) {
		if (Protected(instance)) {
			// **Named rather than silent**, because a `Destroy()` that does
			// nothing and says nothing is a script that carries on believing it
			// worked. The instance's own name is what the author wrote.
			ENGINE_WARN(
				"'{}' is a fixture of this world and may not be destroyed", InstanceNameOf(instance).Text()
			);
			return false;
		}

		DestroyInstance(instance);
		return true;
	}

	bool Store::SetParentAuthored(Entity instance, Entity parent) {
		if (Protected(instance)) {
			ENGINE_WARN(
				"'{}' is a fixture of this world and may not be reparented", InstanceNameOf(instance).Text()
			);
			return false;
		}

		return SetParent(instance, parent);
	}

	Entity Store::MintNamed(std::string_view name, bool predicted) {
		if (name.empty()) {
			return predicted ? CreatePredicted() : Create();
		}

		const std::string key(name);
		const auto found = State->EntitiesByName.find(key);
		if (found != State->EntitiesByName.end() && Alive(found->second)) {
			// A name is a way to point at one thing. Handing back a second
			// entity under the same name would make "the camera" ambiguous the
			// first time a scene was loaded twice.
			return found->second;
		}

		const Entity entity = predicted ? CreatePredicted() : Create();
		if (entity == NULL_ENTITY) {
			// A refused mint must not leave the name pointing at index zero,
			// which is a real entity in every world that has one.
			return NULL_ENTITY;
		}

		State->NamesByIndex.insert_or_assign(EntityId::Of(entity).Index, key);
		State->EntitiesByName.insert_or_assign(key, entity);
		return entity;
	}

	Entity Store::Create(std::string_view name) {
		RequireOwningThread("Create");

		return MintNamed(name, false);
	}

	Entity Store::CreatePredicted(std::string_view name) {
		RequireOwningThread("CreatePredicted");

		return MintNamed(name, true);
	}

	void Store::Destroy(Entity entity) {
		RequireOwningThread("Destroy");

		// **Out of the tree before out of the directory.** See `DetachFromTree`:
		// freeing a row leaves every link that points at it naming something
		// that is gone, and the sibling walk stops at the first of those rather
		// than stepping over it - so destroying the middle child of three used
		// to truncate the list and lose the rest.
		//
		// Before rather than after, because there is no "after": once the row
		// is vacated its own links are gone and there is nothing left to unlink
		// it by.
		DetachFromTree(*State, entity);

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
		// `Adopt` puts it in whichever region the index falls in, so an adopted
		// handle does not have to say which one it came from.
		State->Directory.Adopt(key.Index, key.Generation);
		return true;
	}

	void Store::EachEntity(const std::function<void(Entity)> &body) {
		RequireOwningThread("EachEntity");

		// Deferred, as `Each` is. The directory is being walked and a create
		// inside the body would grow the very thing being iterated.
		const DeferScope defer(*this);

		// The directory rather than the tables, because an entity carrying no
		// components at all is in no table and is still an entity.
		//
		// Both regions, authoritative first. Walking to `Capacity()` alone would
		// miss every predicted entity - silently, since a directory with none
		// behaves identically - which is exactly the shape of bug a second
		// region introduces.
		const size_t capacity = State->Directory.Capacity();
		for (uint32_t index = 0; index < capacity; index++) {
			if (!State->Directory.Live(index)) {
				continue;
			}
			body(EntityId::Pack(index, State->Directory.Generation(index)));
		}

		const size_t predicted = State->Directory.PredictedCapacity();
		for (uint32_t local = 0; local < predicted; local++) {
			const uint32_t index = SparseSet::PREDICTED_BASE + local;
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

	size_t Store::ResidentStorageBytes() const {
		size_t bytes = State->Directory.ResidentBytes();
		for (const Archetype &table : State->Tables) {
			bytes += table.ResidentBytes();
		}
		return bytes;
	}

	// --- components --------------------------------------------------------

	// Qualified, every one of them. The public runtime-keyed methods below
	// share these names deliberately - they are the same operation, one taking
	// a state and one taking a `Store` - and unqualified lookup inside a member
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
		VisitTables(QueryTerms{terms, {}, {}}, body);
	}

	void Store::VisitTables(const QueryTerms &terms, const std::function<void(const TableSlice &)> &body) {
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
		// testing - which is what makes calling this every tick cost nothing
		// after the first frame, and what closes `D00003`.
		if (plan.SeenTables < State->Tables.size()) {
			for (size_t index = plan.SeenTables; index < State->Tables.size(); index++) {
				const ComponentSet &set = State->Tables[index].Set();

				// **All three tests are per table, never per row.** Whether a
				// component is present is a property of the archetype, so an
				// exclusion costs one binary search per table per plan rebuild
				// and nothing at all afterwards - the same price an inclusion
				// pays. That is what makes `Without` worth having rather than a
				// branch in the body.
				if (!set.ContainsAll(terms.Bound) || !set.ContainsAll(terms.Required)) {
					continue;
				}

				bool excluded = false;
				for (const ComponentId term : terms.Excluded) {
					excluded = excluded || set.Contains(term);
				}
				if (excluded) {
					continue;
				}

				QueryPlan::Match match;
				match.Table = static_cast<uint32_t>(index);
				match.Positions.reserve(terms.Bound.size());

				// Resolved in the caller's term order, so the iteration code
				// indexes by parameter position rather than searching per row.
				// The key carries that order for the same reason - see
				// `PlanKey`.
				const std::span<const ComponentId> ids = set.Ids();
				for (const ComponentId term : terms.Bound) {
					const auto at = std::lower_bound(ids.begin(), ids.end(), term);
					match.Positions.push_back(static_cast<size_t>(at - ids.begin()));
				}

				plan.Matches.push_back(std::move(match));
			}
			plan.SeenTables = State->Tables.size();
		}

		// On the stack for the same reason the key is: a heap allocation per
		// query per tick, to hold at most a handful of pointers.
		void *const *inlineColumns[INLINE_QUERY_TERMS];
		std::vector<void *const *> overflowColumns;

		void *const **columns = inlineColumns;
		if (terms.Bound.size() > INLINE_QUERY_TERMS) {
			overflowColumns.resize(terms.Bound.size());
			columns = overflowColumns.data();
		}

		for (const QueryPlan::Match &match : plan.Matches) {
			Archetype &table = State->Tables[match.Table];
			if (table.Rows() == 0) {
				continue;
			}

			// The chunk directory rather than a base address. One slice still
			// covers the whole table - see `TableSlice` on why that is not a
			// detail - and the visitors index it by `Column::ChunkOf(row)`.
			for (size_t term = 0; term < terms.Bound.size(); term++) {
				columns[term] = table.ColumnAt(match.Positions[term]).ChunkData();
			}

			TableSlice slice;
			slice.Rows = table.Rows();
			slice.Entities = table.Entities().data();
			slice.Columns = columns;

			body(slice);
		}
	}

	size_t Store::CountRows(std::span<const ComponentId> terms) {
		return CountRows(QueryTerms{terms, {}, {}});
	}

	size_t Store::CountRows(const QueryTerms &terms) {
		size_t total = 0;
		VisitTables(terms, [&total](const TableSlice &slice) { total += slice.Rows; });
		return total;
	}

	namespace {
		// The caller's terms, canonicalised.
		//
		// **Sorted and deduplicated so that two spellings of one query share a
		// plan.** `VisitTables` keys its cache on the term bytes, so `{A, B}`
		// and `{B, A}` would otherwise build and top up two plans that match
		// exactly the same tables - and a script writing its query arguments in
		// a different order in two files would pay for it twice.
		//
		// A duplicate term is dropped rather than refused: naming a component
		// twice is a redundant filter, not a different one.
		std::vector<ComponentId> Canonical(std::span<const ComponentId> components) {
			std::vector<ComponentId> terms(components.begin(), components.end());
			std::sort(terms.begin(), terms.end());
			terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
			return terms;
		}
	}

	void
	Store::EachMatching(std::span<const ComponentId> components, const std::function<void(Entity)> &body) {
		RequireOwningThread("EachMatching");

		// **An empty query matches nothing.** A query is defined by what it
		// names, and `VisitTables` with no terms would match every table - which
		// is a different question, and `EachEntity` is the one that answers it
		// honestly by walking the directory rather than the tables.
		if (components.empty()) {
			return;
		}

		const std::vector<ComponentId> terms = Canonical(components);

		// Deferred, as `Each` is: the body is a script, and a script destroying
		// what it just found is the ordinary case rather than the exotic one.
		const DeferScope defer(*this);

		VisitTables(terms, [&body](const TableSlice &slice) {
			// `Archetype::Ids` is one contiguous array whatever the columns do,
			// so the entities need no chunk walk - see `ecs/AGENTS.md` on why
			// the columns underneath do.
			for (size_t row = 0; row < slice.Rows; row++) {
				body(slice.Entities[row]);
			}
		});
	}

	void Store::EachMatching(const QueryTerms &terms, const std::function<void(Entity)> &body) {
		RequireOwningThread("EachMatching");
		if (terms.Bound.empty() && terms.Required.empty()) {
			return;
		}

		const DeferScope defer(*this);
		VisitTables(terms, [&body](const TableSlice &slice) {
			for (size_t row = 0; row < slice.Rows; row++) {
				body(slice.Entities[row]);
			}
		});
	}

	void Store::EachMatchingAny(
		std::span<const ComponentId> components, const std::function<void(const Entity *, size_t)> &body
	) {
		RequireOwningThread("EachMatchingAny");

		// An empty query matches nothing, exactly as `EachMatching`'s does.
		if (components.empty()) {
			return;
		}

		// **No query plan, deliberately.** `VisitTables` caches a plan per term
		// list because it also resolves a column position per term, which is the
		// expensive part and the part this does not need: the answer here is the
		// entity ids, which every table keeps in one contiguous array whatever
		// its columns do. What is left is a `Contains` per table per term, which
		// is a binary search over a handful of ids - cheaper than the hash of the
		// term bytes a plan lookup would start with.
		const DeferScope defer(*this);

		for (const Archetype &table : State->Tables) {
			if (table.Rows() == 0) {
				continue;
			}

			const ComponentSet &set = table.Set();
			bool matched = false;
			for (const ComponentId id : components) {
				if (set.Contains(id)) {
					matched = true;
					break;
				}
			}

			if (matched) {
				body(table.Entities().data(), table.Rows());
			}
		}
	}

	size_t Store::CountMatching(std::span<const ComponentId> components) {
		RequireOwningThread("CountMatching");

		if (components.empty()) {
			return 0;
		}
		return CountRows(Canonical(components));
	}

	size_t Store::CountMatching(const QueryTerms &terms) {
		RequireOwningThread("CountMatching");
		if (terms.Bound.empty() && terms.Required.empty()) {
			return 0;
		}
		return CountRows(terms);
	}

	// --- instances ---------------------------------------------------------

	Entity Store::CreateInstance(ClassId id, std::string_view name) {
		RequireOwningThread("CreateInstance");

		// The same check `Create` makes, because this mints from the same
		// authoritative range. It was missing, and `scene::MakePart` grew a copy
		// of it to work around the gap - one minting path honouring the rule and
		// one walking past it is worse than neither, because the one that
		// honours it makes the other look covered.
		if (!MayMintAuthoritative("CreateInstance")) {
			return NULL_ENTITY;
		}

		return engine::ecs::CreateInstance(*State, id, name);
	}

	Entity Store::CreatePredictedInstance(ClassId id, std::string_view name) {
		RequireOwningThread("CreatePredictedInstance");

		return engine::ecs::CreateInstance(*State, id, name, EntityRange::Predicted);
	}

	ClassId Store::ClassOf(Entity instance) const {
		return engine::ecs::ClassOf(*State, instance);
	}

	bool Store::IsA(Entity instance, ClassId id) const {
		return engine::ecs::IsA(*State, instance, id);
	}

	// --- properties by name ------------------------------------------------

	namespace {
		// The descriptor for one name on one instance, or nullptr.
		//
		// Linear over the merged list rather than a map. A class has a handful
		// of properties, the list is contiguous, and a binding that cares about
		// the cost resolves the descriptor once and keeps it - which is what
		// `PropertiesOf` is for.
		const PropertyDescriptor *FindProperty(const Store &store, Entity instance, core::Name name) {
			const ClassId id = store.ClassOf(instance);
			if (!id.IsValid()) {
				return nullptr;
			}

			for (const PropertyDescriptor &property : Classes::Describe(id).Properties) {
				if (property.Name == name) {
					return &property;
				}
			}
			return nullptr;
		}
	}

	std::span<const PropertyDescriptor> Store::PropertiesOf(Entity instance) const {
		const ClassId id = ClassOf(instance);
		return id.IsValid() ? Classes::Describe(id).Properties : std::span<const PropertyDescriptor>{};
	}

	bool Store::GetProperty(Entity instance, core::Name property, void *out, size_t bytes) const {
		const PropertyDescriptor *descriptor = FindProperty(*this, instance, property);
		if (descriptor == nullptr) {
			return false;
		}
		return GetProperty(instance, *descriptor, out, bytes);
	}

	bool
	Store::GetProperty(Entity instance, const PropertyDescriptor &descriptor, void *out, size_t bytes) const {
		if (descriptor.Get == nullptr) {
			return false;
		}
		if (bytes != descriptor.Size || out == nullptr) {
			return false;
		}
		return descriptor.Get(*this, instance, out);
	}

	bool Store::SetProperty(Entity instance, core::Name property, const void *value, size_t bytes) {
		const PropertyDescriptor *descriptor = FindProperty(*this, instance, property);
		if (descriptor == nullptr) {
			return false;
		}
		return SetProperty(instance, *descriptor, value, bytes);
	}

	bool Store::SetPropertyAuthored(Entity instance, core::Name property, const void *value, size_t bytes) {
		const PropertyDescriptor *descriptor = FindProperty(*this, instance, property);
		if (descriptor == nullptr) {
			return false;
		}
		return SetPropertyAuthored(instance, *descriptor, value, bytes);
	}

	bool Store::SetProperty(
		Entity instance, const PropertyDescriptor &descriptor, const void *value, size_t bytes
	) {
		return SetPropertyValue(instance, descriptor, value, bytes, false);
	}

	bool Store::SetPropertyAuthored(
		Entity instance, const PropertyDescriptor &descriptor, const void *value, size_t bytes
	) {
		return SetPropertyValue(instance, descriptor, value, bytes, true);
	}

	bool Store::SetPropertyValue(
		Entity instance, const PropertyDescriptor &descriptor, const void *value, size_t bytes, bool authored
	) {
		RequireOwningThread("SetProperty");

		if (descriptor.Set == nullptr || !descriptor.Writable) {
			return false;
		}

		if (bytes != descriptor.Size || value == nullptr) {
			return false;
		}

		// A replica's rows belong to the authority. v0.3 made minting here
		// impossible; a property write is the same hazard one step along - a
		// client-side script setting a value the next delta overwrites, which
		// presents as "my script works sometimes" rather than as an error.
		//
		// Refused loudly rather than quietly, because a script author cannot
		// see the difference between a write that was rejected and one that
		// was applied and then replaced.
		if (AdoptOnly() && !authored) {
			ENGINE_ERROR(
				"store '{}': refusing to set '{}' in a replica. The authority owns this row, and "
				"a value written here survives until its next delta and no longer.",
				Name(),
				descriptor.Name.Text()
			);
			return false;
		}

		// **The check `PropertyType::Enum` exists for.** The storage is a
		// `core::Name` either way, so without this an unregistered member is
		// written and surfaces later as a part that renders with a default
		// nobody asked for. Refused here rather than in each binding, so both
		// VMs and a future editor get one answer.
		if (descriptor.Type == PropertyType::Enum) {
			const auto member = *static_cast<const core::Name *>(value);
			if (!EnumTable::Has(descriptor.EnumName, member)) {
				ENGINE_ERROR(
					"store '{}': '{}' is not a member of Enum.{}",
					Name(),
					member.IsValid() ? member.Text() : std::string_view("(none)"),
					descriptor.EnumName.Text()
				);
				return false;
			}
		}

		return descriptor.Set(*this, instance, value);
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

	bool Store::HasChildren(Entity instance) const {
		return engine::ecs::HasChildren(*State, instance);
	}

	void Store::EachDescendant(Entity instance, const std::function<void(Entity)> &body) const {
		engine::ecs::EachDescendant(*State, instance, body);
	}

	bool Store::SetInstanceName(Entity instance, std::string_view name) {
		RequireOwningThread("SetInstanceName");

		return engine::ecs::SetInstanceName(*State, instance, name);
	}

	Entity Store::FindFirstChild(Entity instance, std::string_view name, bool recursive) const {
		return engine::ecs::FindFirstChild(*State, instance, name, recursive);
	}

	Entity Store::FindFirstChildOfClass(Entity instance, ClassId id) const {
		return engine::ecs::FindFirstChildOfClass(*State, instance, id);
	}

	Entity Store::FindFirstChildWhichIsA(Entity instance, ClassId id, bool recursive) const {
		return engine::ecs::FindFirstChildWhichIsA(*State, instance, id, recursive);
	}

	Entity Store::FindFirstAncestor(Entity instance, std::string_view name) const {
		return engine::ecs::FindFirstAncestor(*State, instance, name);
	}

	Entity Store::FindFirstAncestorOfClass(Entity instance, ClassId id) const {
		return engine::ecs::FindFirstAncestorOfClass(*State, instance, id);
	}

	Entity Store::FindFirstAncestorWhichIsA(Entity instance, ClassId id) const {
		return engine::ecs::FindFirstAncestorWhichIsA(*State, instance, id);
	}

	void Store::ObserveTree() {
		RequireOwningThread("ObserveTree");

		State->WatchTree = true;
	}

	bool Store::TreeObserved() const {
		return State->WatchTree;
	}

	void Store::TakeTreeChanges(std::vector<TreeChange> &out) {
		RequireOwningThread("TakeTreeChanges");

		out.clear();
		out.swap(State->TreeChanges);
	}

	void Store::OnDescendantRemoving(std::function<void(Entity, Entity)> body) {
		RequireOwningThread("OnDescendantRemoving");

		State->BeforeRemoving = std::move(body);
	}

	void Store::ClearDescendantRemoving() {
		State->BeforeRemoving = {};
	}

	std::string Store::GetFullName(Entity instance) const {
		return engine::ecs::GetFullName(*State, instance);
	}

	void Store::EachRoot(const std::function<void(Entity)> &body) const {
		// Collected and sorted rather than visited in place. The walk is over
		// archetypes, and a row's position in one moves whenever anything
		// changes its component set - so visiting in place would report the
		// world's roots in an order that depends on what happened to the scene
		// rather than on the scene. A recording made in one order and replayed
		// in another diverges the first time a script reads `GetChildren()`.
		std::vector<Entity> roots;
		const_cast<Store *>(this)->Each<const Hierarchy>([&](Entity entity, const Hierarchy &node) {
			if (node.Parent == NULL_ENTITY) {
				roots.push_back(entity);
			}
		});

		std::sort(roots.begin(), roots.end(), [](Entity left, Entity right) { return left.Id < right.Id; });

		for (const Entity root : roots) {
			body(root);
		}
	}

	Entity Store::FindFirstRoot(std::string_view name) const {
		const core::Name wanted(name);

		Entity found = NULL_ENTITY;
		EachRoot([&](Entity root) {
			if (found == NULL_ENTITY && InstanceNameOf(root) == wanted) {
				found = root;
			}
		});
		return found;
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

		// A clone is a mint like any other, and it is the third path into the
		// authoritative range.
		if (!MayMintAuthoritative("CloneInstance")) {
			return NULL_ENTITY;
		}

		return CloneInstanceInRange(source, false);
	}

	Entity Store::ClonePredictedInstance(Entity source) {
		RequireOwningThread("ClonePredictedInstance");

		return CloneInstanceInRange(source, true);
	}

	Entity Store::CloneInstanceInRange(Entity source, bool predicted) {
		std::vector<engine::ecs::ClonedPair> made;
		const Entity copy = engine::ecs::CloneInstance(
			*State, source, made, predicted ? EntityRange::Predicted : EntityRange::Authoritative
		);
		if (copy == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		// **A reference into the subtree is part of what was copied; one out of
		// it is not.** A weld naming the two parts it joins has to name the two
		// *new* parts, or the duplicate is welded to the original and dragging
		// it moves the thing it was copied from. A part naming the terrain it
		// sits on has to keep naming the terrain, because there is only one.
		//
		// Roblox draws the line in exactly that place, and it is the only place
		// that makes both cases work. Here rather than in `Instances.cpp`
		// because a property is reached through generated conversions that take
		// a `Store`, and that layer holds a `StoreState`.
		for (const engine::ecs::ClonedPair &pair : made) {
			for (const PropertyDescriptor &property : PropertiesOf(pair.Copy)) {
				if (property.Type != PropertyType::Reference || !property.Writable ||
					property.Get == nullptr || property.Set == nullptr) {
					continue;
				}

				Entity held;
				if (!property.Get(*this, pair.Copy, &held) || held == NULL_ENTITY) {
					continue;
				}

				for (const engine::ecs::ClonedPair &other : made) {
					if (other.Source == held) {
						property.Set(*this, pair.Copy, &other.Copy);
						break;
					}
				}
			}
		}

		return copy;
	}

	// --- change tracking ---------------------------------------------------

	void Store::ObserveRaw(ComponentId id) {
		if (!WatchComponent(*State, id)) {
			return;
		}

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

	uint64_t Store::ComponentChangeVersionRaw(ComponentId id) const {
		if (!id.IsValid() || id.Index >= State->ComponentChanges.size()) {
			return 0;
		}
		return State->ComponentChanges[id.Index];
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

	void Store::VisitChanged(ComponentId subject, const std::function<void(Entity, void *)> &body) {
		if (!subject.IsValid() || subject.Index >= State->ChangedEntities.size() ||
			State->ComponentChanges[subject.Index] == 0) {
			return;
		}

		// Snapshot the boundary, not the storage. A callback may write another
		// row of the same component and grow this vector. Re-fetching by index
		// survives that growth, while the fixed count leaves the new write for
		// the next phase boundary.
		const size_t count = State->ChangedEntities[subject.Index].size();
		for (size_t index = 0; index < count; index++) {
			if (index >= State->ChangedEntities[subject.Index].size()) {
				break;
			}
			const Entity entity = State->ChangedEntities[subject.Index][index];
			void *value = const_cast<void *>(GetRaw(entity, subject));
			if (value != nullptr) {
				body(entity, value);
			}
		}
	}

	size_t Store::SubjectPosition(const TableSlice &slice, ComponentId subject) const {
		// The subject's bit index in this table, which is also where its column
		// sits: resolved once per table rather than per row.
		const EntityId key = EntityId::Of(slice.Entities[0]);
		const EntityLocation *location = State->Directory.Locate(key.Index);
		const std::span<const ComponentId> ids = State->Tables[location->Archetype].Set().Ids();
		const auto at = std::lower_bound(ids.begin(), ids.end(), subject);
		return static_cast<size_t>(at - ids.begin());
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

	std::span<const ComponentId> Store::ComponentsOf(Entity entity) const {
		const EntityId key = EntityId::Of(entity);
		if (!State->Directory.Alive(key.Index, key.Generation)) {
			return {};
		}

		// An entity with no components is in no table, which is the ordinary
		// state of one that has just been created - a row is what a component
		// buys.
		const EntityLocation *location = State->Directory.Locate(key.Index);
		if (location == nullptr || location->Archetype == EntityLocation::NO_ARCHETYPE) {
			return {};
		}

		// The interned set's own list, which is sorted by id and lives as long
		// as the set does. Copying it here would allocate on a path an editor
		// calls once per visible row per frame.
		return State->Tables[location->Archetype].Set().Ids();
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

	void
	Store::EachRuns(ComponentId component, const std::function<void(const Entity *, void *, size_t)> &body) {
		RequireOwningThread("EachRuns");
		if (!component.IsValid()) {
			return;
		}

		const ComponentId terms[] = {component};
		const DeferScope defer(*this);
		VisitRuns(terms, component, body);
	}

	void Store::VisitRuns(
		std::span<const ComponentId> terms,
		ComponentId subject,
		const std::function<void(const Entity *, void *, size_t)> &body
	) {
		VisitTables(terms, [&](const TableSlice &slice) {
			void *const *valueChunks = slice.Columns[0];
			const size_t stride = Components::Describe(subject).Size;

			// One call per chunk - `VisitBatch`'s shape, for a component named at
			// runtime rather than one of `Each`'s template arguments. Every row in
			// range matters here, unlike `VisitChangedRuns`, so a chunk is one run
			// with no bit-testing inside it.
			size_t row = 0;
			while (row < slice.Rows) {
				const size_t chunk = Column::ChunkOf(row);
				const size_t base = Column::ChunkStart(chunk);
				const size_t end = ChunkEnd(row, slice.Rows);

				// A tag has no column - see `VisitChanged` on why `Columns` is
				// null for one - so there are no bytes to hand back, only entities.
				auto *values = stride == 0 || valueChunks == nullptr
								   ? nullptr
								   : static_cast<std::byte *>(valueChunks[chunk]);

				body(
					slice.Entities + row,
					values == nullptr ? nullptr : values + (row - base) * stride,
					end - row
				);
				row = end;
			}
		});
	}

	void Store::VisitChangedRuns(
		std::span<const ComponentId> terms,
		ComponentId subject,
		const std::function<void(const Entity *, void *, size_t)> &body
	) {
		VisitTables(terms, [&](const TableSlice &slice) {
			void *const *bitChunks = slice.Columns[1];
			void *const *valueChunks = slice.Columns[0];
			const size_t stride = Components::Describe(subject).Size;
			const size_t position = SubjectPosition(slice, subject);

			// Runs of adjacent set bits. A system walks a table in order and
			// writes as it goes, so the changed rows are usually one run or a
			// few - which is what makes a delta a memcpy per run rather than a
			// copy per entity.
			//
			// **A run stops at a chunk boundary even when the bits do not.** The
			// contract this hands the callback is that `data + row * size` is
			// the value for `entities[row]` over the whole run, and past a
			// boundary that address is in the previous chunk's tail - so a
			// delta would send entity A's id with entity B's bytes, which every
			// test here would pass and a client would show as teleporting.
			// `Archetype::Ids` stays one contiguous array, so only the value
			// side needs clipping.
			size_t row = 0;
			while (row < slice.Rows) {
				const size_t chunk = Column::ChunkOf(row);
				const size_t base = Column::ChunkStart(chunk);
				const size_t end = ChunkEnd(row, slice.Rows);
				const auto *bits = static_cast<const DirtyBits *>(bitChunks[chunk]);
				auto *values = static_cast<std::byte *>(valueChunks[chunk]);

				while (row < end) {
					if (!bits[row - base].Test(position)) {
						row++;
						continue;
					}

					const size_t start = row;
					while (row < end && bits[row - base].Test(position)) {
						row++;
					}

					body(slice.Entities + start, values + (start - base) * stride, row - start);
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
			// component, which moves rows between tables - and an iteration
			// that was still walking one of them would be walking a table that
			// no longer holds what it thought.
			State->Firing.clear();
			{
				const DeferScope defer(*this);
				VisitChanged(subject, [this](Entity entity, void *) { State->Firing.push_back(entity); });
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

	void Store::MarkChangedRaw(Entity entity, ComponentId component) {
		RequireOwningThread("MarkChanged");

		// **Through the same door a mutable get uses**, so there is one
		// definition of what marking a row means. `GetComponentMutable` marks
		// and then hands back a pointer; the pointer is what this call does not
		// want, and every other line of it is exactly right.
		(void)GetComponentMutable(*State, entity, component);
	}

	void Store::MarkAllChangedRaw(ComponentId component) {
		RequireOwningThread("MarkAllChanged");

		if (!component.IsValid() || State->Watched.empty()) {
			return;
		}

		const ComponentId bitsId = Components::Of<DirtyBits>();

		for (Archetype &table : State->Tables) {
			Column *bits = table.Find(bitsId);
			if (bits == nullptr || bits->Empty()) {
				continue;
			}

			// The bit index is the component's position in the table's sorted
			// set, which is also where its column sits - the same resolution
			// `MarkWritten` does per write, done once per table here.
			const std::span<const ComponentId> ids = table.Set().Ids();
			const auto at = std::lower_bound(ids.begin(), ids.end(), component);
			if (at == ids.end() || *at != component) {
				continue;
			}

			// Per chunk. A column is only contiguous inside one, so marking
			// `table.Rows()` bits from chunk zero's base would write past its
			// end - and it is a write, so it corrupts whatever the pool handed
			// to somebody else rather than merely reading rubbish.
			const auto position = static_cast<size_t>(at - ids.begin());
			void *const *chunks = bits->ChunkData();
			for (size_t row = 0; row < table.Rows();) {
				const size_t chunk = Column::ChunkOf(row);
				const size_t start = Column::ChunkStart(chunk);
				auto *rows = static_cast<DirtyBits *>(chunks[chunk]);
				const size_t end = ChunkEnd(row, table.Rows());
				for (; row < end; row++) {
					DirtyBits &dirty = rows[row - start];
					if (!dirty.Test(position) && component.Index < State->ChangedEntities.size()) {
						State->ChangedEntities[component.Index].push_back(table.Entities()[row]);
					}
					dirty.Mark(position);
				}
			}

			// The coarse counter moves by the number of rows, so a consumer
			// watching `ChangeVersion` sees a batch write the same way it sees
			// that many individual ones.
			State->Changes += table.Rows();
			if (component.Index < State->ComponentChanges.size() &&
				State->ComponentChanges[component.Index] != 0) {
				State->ComponentChanges[component.Index] += table.Rows();
			}
		}
	}

	void Store::ClearChanges() {
		RequireOwningThread("ClearChanges");

		const ComponentId id = Components::Of<DirtyBits>();
		for (Archetype &table : State->Tables) {
			Column *bits = table.Find(id);
			if (bits == nullptr || bits->Empty()) {
				continue;
			}
			// One memset per chunk, over that chunk's live rows only. The whole
			// row count from chunk zero's base is a heap overrun of everything
			// past the first chunk, and a memset is the version of that which
			// destroys somebody else's data rather than reading it.
			void *const *chunks = bits->ChunkData();
			const size_t rows = bits->Size();
			for (size_t row = 0; row < rows;) {
				const size_t end = ChunkEnd(row, rows);
				std::memset(chunks[Column::ChunkOf(row)], 0, (end - row) * sizeof(DirtyBits));
				row = end;
			}
		}
		for (const ComponentId watched : State->Watched) {
			State->ChangedEntities[watched.Index].clear();
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
		// defer - a nested Each, or a Destroy that cascades. Replaying out of
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
