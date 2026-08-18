#pragma once

// L3 · the storage for one world.
//
// One Store is one world's data - every entity, every component, and every
// piece of world-scoped state. There is no global store and no way to reach
// another one from here: `world` at L4 hands out identifiers, never stores, and
// this is the type it is refusing to hand out.
//
// **Everything the tick reads and writes lives in here.** Per-entity data is a
// component; one-of-a-kind data is a resource. A module that keeps a private
// member for something a system touches has put half the world outside the
// world, and the half outside is the half that is not covered by the affinity
// check, not visible to the profiler, and not there when a world is serialised
// or replayed.
//
// The storage underneath is the engine's own - `ComponentSet` names an
// archetype, `Column` holds one component's values contiguously, and
// `SparseSet` maps an entity to its row. That is what makes a property
// reachable by name at runtime, a column serialisable without knowing its type,
// and a world restorable into another process.
//
// Thread affinity is checked rather than trusted. A store belongs to the
// thread that bound it, every mutation aborts unless it is on that thread, and
// the check is on in every build - a data race that only shows up under load on
// a player's machine costs far more than a branch.
//
// @tier L3 · shared

#include <engine/core/Log.hpp>
#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Column.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Time.hpp>
#include <engine/parallel/Jobs.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace engine::ecs {

	struct StoreState;

	// Declared in `Classes.hpp`, which this header deliberately does not
	// include: a property is a description and the storage only ever hands
	// spans of them out. Include `Classes.hpp` to read one.
	struct PropertyDescriptor;

	// What a query matches on, in three parts.
	//
	// **Every one of them is answered per archetype, never per row.** Whether a
	// table holds a component is a property of the table, so a query with two
	// exclusions costs two binary searches the first time it meets a table and
	// nothing at all on every tick afterwards - the same price an inclusion
	// pays. That is what makes `Without` worth having: the alternative is a
	// branch per row per tick over rows the loop should never have visited.
	//
	// @since v0.15
	struct QueryTerms {
		// The components the body is handed a reference to, in the order it
		// names them. Order is meaningful here and nowhere else in this struct.
		std::span<const ComponentId> Bound;

		// Components an entity must carry, without the body being handed them.
		//
		// **The half that is easy to overlook and is used more often.** A tag
		// has no data, so naming it in `Bound` forces the body to take a
		// parameter it cannot use; naming it here says the same thing and
		// leaves the signature alone.
		std::span<const ComponentId> Required;

		// Components an entity must not carry.
		std::span<const ComponentId> Excluded;
	};

	// Owns every entity, component, resource, and clock value for one world.
	//
	// A Store starts bound to its construction thread. Checked storage mutations
	// abort in every build when called from any other thread;
	// BindToCallingThread is the explicit handoff used before another thread
	// begins ticking the world.
	class Store {
	  public:
		// Constructs an empty world store bound to the calling thread.
		//
		// @param name The diagnostic name to copy and retain.
		explicit Store(std::string_view name);

		// Destroys the world and all storage it owns.
		~Store();

		// Store ownership and world storage cannot be copied.
		Store(const Store &) = delete;

		// Store ownership and world storage cannot be copy-assigned.
		Store &operator=(const Store &) = delete;

		// Store ownership and world storage cannot be moved.
		Store(Store &&) = delete;

		// Store ownership and world storage cannot be move-assigned.
		Store &operator=(Store &&) = delete;

		// Returns this store's diagnostic name.
		//
		// @return A view valid for the lifetime of this Store.
		std::string_view Name() const {
			return StoreName;
		}

		// --- affinity ------------------------------------------------------

		// Hands the store to the calling thread. Called by whatever ticks it.
		// A store constructed on the loader thread and ticked on a worker is
		// the ordinary case, not an error. The previous owner must stop accessing
		// the store before this handoff.
		//
		// Rebinding every tick is expected rather than exceptional: a world is
		// picked up by whichever job worker claims it, so the owner is a
		// different thread most ticks. That is why the owner is atomic - the
		// handoff races the affinity check of any thread still holding a stale
		// belief about who owns this store, and a plain read of a plain
		// `std::thread::id` there is a data race by the letter of the standard
		// even where it happens to be benign.
		//
		// @threadsafe
		void BindToCallingThread();

		// Reports whether the caller is the thread that currently owns the store.
		//
		// @return `true` when the current thread may mutate this Store.
		// @threadsafe
		bool IsOnOwningThread() const {
			return std::this_thread::get_id() == Owner.load(std::memory_order_relaxed);
		}

		// --- entities ------------------------------------------------------

		// Creates an unnamed entity owned by this store.
		//
		// A new entity carries no components and occupies no table until it is
		// given one. That is not a special case to work around: an entity is a
		// directory slot, and a row is what a component buys.
		//
		// **Authoritative**: the index comes from the low half of the index
		// space, which is the half a replica never mints from. Refused, and
		// `NULL_ENTITY`, in an adopt-only store - see `SetAdoptOnly` - and also
		// when the authoritative range has issued all 2³¹ of its indices, which
		// is a refusal rather than a wrap into the predicted range.
		//
		// @return A live entity handle, or `NULL_ENTITY`.
		Entity Create();

		// Creates an entity a replica predicted, from the reserved high range.
		//
		// **The safe way for a replica to own something the server has not
		// confirmed.** An index minted here is 2³¹ or higher and the authority
		// never allocates one, so a predicted projectile cannot collide with an
		// entity the server made - which is what `SetAdoptOnly` had to forbid
		// minting outright to avoid.
		//
		// Legal in an adopt-only store, and that is the whole difference between
		// this and `Create`: a replica still may not mint an *authoritative*
		// entity, because that index is the authority's to hand out.
		//
		// A predicted entity is local. It is never sent, and the authority never
		// mentions one, so nothing in a snapshot from a server can name it. When
		// the server does answer with the real entity, `Promote` is the explicit
		// step that makes the local row that entity.
		//
		// @return A live entity handle, or `NULL_ENTITY` when the predicted
		//         range has issued all of its indices.
		Entity CreatePredicted();

		// Creates a named entity a replica predicted.
		//
		// The named counterpart of `CreatePredicted`, with `Create`'s name
		// semantics: an empty name creates an unnamed entity, and a name already
		// in use hands back the entity holding it. Present so that the two mint
		// paths have the same shape - a predicted entity that could not be named
		// would make `Promote`'s name fix-up a branch nothing could reach, and a
		// branch nothing reaches is one that is wrong by the time it is needed.
		//
		// @param name The name to copy and associate with the entity.
		// @return A live entity handle, or `NULL_ENTITY`.
		Entity CreatePredicted(std::string_view name);

		// Reports whether a handle names a locally predicted entity.
		//
		// A property of the handle rather than of any store, so it answers for a
		// handle read out of a component without a directory lookup. `Store`
		// rather than `Entity` because the index layout is deliberately not part
		// of the handle's public surface.
		//
		// @param entity The handle to classify.
		// @return `true` when its index falls in the predicted range.
		static bool IsPredicted(Entity entity);

		// Rewrites a predicted entity's identity to an authoritative one.
		//
		// **The explicit step that a predicted entity becoming a server one
		// is.** The row does not move: every component value, the archetype and
		// the row index stay exactly as they were, and only the handle naming
		// them changes. Rebuilding the entity instead would throw away what the
		// client predicted, which is the state the prediction existed to have.
		//
		// **This is the primitive, and the policy is deliberately not here.**
		// *When* to promote, which authoritative handle to promote to, and what
		// to do with a prediction the server never confirms belong to whatever
		// predicts - and nothing does yet, because that wants a projectile,
		// which wants v0.4's physics and `Part`. Guessing the rule before its
		// consumer exists is what `ROADMAP.md` says not to do here, so this
		// operation exists and no caller decides for a future one. See
		// `ecs/AGENTS.md`.
		//
		// **An `ecs::Entity` stored inside another component is not rewritten.**
		// The directory, the row's own id, this store's name maps and the
		// instance hierarchy around it all follow the new handle; a handle in
		// some other component does not, because nothing in `TypeDescriptor`
		// says which of a component's bytes are entity handles. Those keep the
		// predicted value and read as **dead** - the predicted index's
		// generation is bumped as it is freed - rather than as a different
		// entity. A caller holding such a field rewrites it itself, since it is
		// the layer that knows the field is a handle.
		//
		// Refused from inside `Each`, because the id array an iteration is
		// holding a pointer into is exactly what this rewrites.
		//
		// @param predicted     A live handle from `CreatePredicted`.
		// @param authoritative The handle it should answer to, in the
		//                      authoritative range and not already live here.
		// @return `false` when either handle is in the wrong range, the
		//         predicted one is not live, the authoritative one already is,
		//         or the call came from inside an iteration.
		bool Promote(Entity predicted, Entity authoritative);

		// Refuses to mint *authoritative* entities in this store.
		//
		// **For a replica.** An entity is an index plus a generation, and the
		// authoritative half of the index space belongs to whoever owns the
		// simulation - so an authoritative entity a replica minted for itself
		// would collide *exactly* with one the authority minted, and `Apply`
		// would be right to treat them as the same entity because nothing tells
		// them apart. The failure is not a crash: it is two different things
		// quietly becoming one, discovered later and somewhere else.
		//
		// Set, `Create` and `CreateInstance` return `NULL_ENTITY` and say so
		// once. Two things are deliberately unaffected:
		//
		//   - **`CreatePredicted`**, because the predicted range is exactly the
		//     range a replica may mint from and the authority never allocates
		//     one. That is the difference this flag now draws, and it is drawn
		//     at the call site rather than by a mode somewhere else.
		//   - **`CreateAt`**, because adopting a handle somebody else issued is
		//     the whole point - this refuses *minting*, not receiving. Which is
		//     the same shape `world::Postbox` enforces for a replica's bus
		//     writes: an operation a client wants performed goes up as an input
		//     and comes back as state.
		//
		// @param adoptOnly Whether to refuse minting authoritative entities.
		void SetAdoptOnly(bool adoptOnly);

		// Whether this store refuses to mint authoritative entities.
		//
		// @return `true` when only adopted and predicted entities may exist
		//         here.
		bool AdoptOnly() const;

		// Creates a named entity owned by this store.
		//
		// Names are for the things a person or a save file has to be able to
		// point at, not for every entity. An empty name creates an unnamed
		// entity; a name already in use returns the entity holding it rather
		// than creating a second.
		//
		// @param name The name to copy and associate with the entity.
		// @return A live entity handle.
		Entity Create(std::string_view name);

		// Destroys an entity and all components attached to it.
		//
		// Deferred when called from inside Each, and applied when that loop
		// ends.
		//
		// @param entity The entity generation to destroy.
		void Destroy(Entity entity);

		// Reports whether an entity generation is currently alive in this store.
		//
		// @param entity The handle to inspect.
		// @return `false` for NULL_ENTITY, destroyed entities, and stale generations.
		bool Alive(Entity entity) const;

		// The name an entity was created with.
		//
		// @param entity The entity to ask about.
		// @return The name, or an empty view when the entity is unnamed or dead.
		std::string_view NameOf(Entity entity) const;

		// The entity holding a name.
		//
		// @param name The name to look up.
		// @return The entity, or NULL_ENTITY when nothing holds that name.
		Entity Find(std::string_view name) const;

		// There is no Count() of everything, and that is deliberate rather than
		// missing. Count what you can name: `CountMatching<Ts...>()`.

		// Creates an entity at an exact index and generation.
		//
		// **For a replica applying authoritative state, and nothing else.**
		// Ordinary creation allocates the next free index; this takes the one
		// it is given, because a replicated handle has to mean the same thing
		// on both machines - an entity stored inside a component is only still
		// the same entity if the directory agrees.
		//
		// `Save`/`Load` and `Apply` already do this internally for a whole
		// world; a delta needs it for one row, and duplicating the mechanism
		// outside the class would mean two places that know how the directory
		// is laid out.
		//
		// **Receiving, not minting**, so `SetAdoptOnly` does not apply. A world
		// that mints authoritative entities of its own *and* adopts them from
		// somebody else will still eventually be told to adopt one it already
		// has - the index ranges separate an authority from a replica, not two
		// authorities from each other. A replica mints from the predicted range
		// and adopts everything else, and that pair does not collide.
		//
		// @param entity The exact handle to bring into being.
		// @return `false` when that handle is already live here.
		bool CreateAt(Entity entity);

		// Visits every live entity, in index order.
		//
		// The primitive an interest filter and a debug view both want: "every
		// entity", including ones carrying no components at all, which a query
		// cannot express because a query is defined by the components it names.
		//
		// Not a `Count()` in disguise. It is a walk, and a caller that wants a
		// number should still count what it can name - this exists for the
		// callers that have to look at each one.
		//
		// Index order rather than creation order, and deterministic: two runs
		// of the same world visit the same entities in the same sequence, which
		// is what lets anything built on it be compared between runs.
		//
		// **Structural changes are deferred**, as they are inside `Each`: the
		// directory is being walked, and creating an entity mid-walk would grow
		// the very thing being iterated.
		//
		// @param body Called as `body(Entity)` for each live entity.
		void EachEntity(const std::function<void(Entity)> &body);

		// --- components ----------------------------------------------------

		// Adds or replaces one component value on an entity.
		//
		// Adding a component the entity does not have moves its row to another
		// table, which invalidates every pointer previously returned by Get or
		// GetMutable for that entity. Replacing a component it already has does
		// not move anything.
		//
		// Deferred when called from inside Each.
		//
		// @param entity The entity that owns the component.
		// @param value  The component value to copy into the store.
		template <class T> void Set(Entity entity, const T &value) {
			RequireOwningThread("Set");
			SetRaw(entity, Components::Of<T>(), &value);
		}

		// Reports whether an entity carries a component of the requested type.
		//
		// @param entity The entity to inspect.
		// @return `true` when the component is present.
		template <class T> bool Has(Entity entity) const {
			return HasRaw(entity, Components::Of<T>());
		}

		// Null when absent. Callers check; there is no Get-or-default, because
		// a default-constructed component silently standing in for a missing
		// one is a bug that reads as working code.
		//
		// Invalidated by anything that moves the entity's row: adding or
		// removing a component on it, or destroying another entity in the same
		// table.
		//
		// @param entity The entity whose component is requested.
		// @return A read-only component pointer, or `nullptr` when absent.
		template <class T> const T *Get(Entity entity) const {
			return static_cast<const T *>(GetRaw(entity, Components::Of<T>()));
		}

		// Returns mutable access to one component without adding it when absent.
		//
		// @param entity The entity whose component is requested.
		// @return A mutable component pointer, or `nullptr` when absent.
		template <class T> T *GetMutable(Entity entity) {
			RequireOwningThread("GetMutable");
			return static_cast<T *>(GetRawMutable(entity, Components::Of<T>()));
		}

		// Mutable access that deliberately does **not** count as a write.
		//
		// `GetMutable` cannot know whether the caller used the pointer it
		// handed back, so it reports the write unconditionally - a change
		// reported that did not happen costs a consumer one wasted rebuild,
		// where a change missed costs it correctness. That is the right
		// default. This is the one documented exception to it, and it exists
		// because the default has a cost nothing was paying for:
		// `ChangeVersion` moves for **any** component that happens to sit in a
		// table carrying `DirtyBits`, not only for an observed one. So writing
		// a derived per-frame tag through `GetMutable` advances the world's
		// change counter every frame, and every gate built on "an unchanged
		// counter means nothing authored has happened" - `physics`'s static
		// broadphase and `gui`'s compile are the two - is falsified for good.
		//
		// **Legitimate only for a component nothing may observe**, which means
		// all three of these hold and a reviewer should check all three:
		//
		//  1. no caller does or may call `Observe<T>()` for it;
		//  2. no caller does or may call `Changed<T>()` or `EachChanged<T>()`
		//     for it;
		//  3. the type's own header says who is allowed to write it, so a
		//     future consumer cannot appear without reading that first.
		//
		// `scene::Rendered` is the one caller and satisfies all three:
		// `scene/Visibility.hpp` states that nothing outside `SyncRendered`
		// may add, remove or write one, so there is no second party whose
		// change could be lost here.
		//
		// If the component might ever be observed, use `GetMutable`. A missed
		// change is not recoverable by the consumer that missed it.
		//
		// @param entity The entity whose component is requested.
		// @return A mutable component pointer, or `nullptr` when absent.
		template <class T> T *GetUnobserved(Entity entity) {
			RequireOwningThread("GetUnobserved");
			return const_cast<T *>(static_cast<const T *>(GetRaw(entity, Components::Of<T>())));
		}

		// Removes one component type without destroying the entity.
		//
		// Deferred when called from inside Each.
		//
		// @param entity The entity from which to remove the component.
		template <class T> void Remove(Entity entity) {
			RequireOwningThread("Remove");
			RemoveRaw(entity, Components::Of<T>());
		}

		// --- resources -----------------------------------------------------
		//
		// World-scoped state: one of it, for the whole world. The clock, the
		// camera, the list a render pass reads.
		//
		// This exists so that no module has to keep a private member for
		// something another module reads. Two copies of one fact drift apart
		// the first time one of them is written inside a branch, and the bug
		// that follows reproduces about once a week.
		//
		// **A resource is not a component, and the distinction is the design
		// rather than a naming convention:**
		//
		//   > Componentise what you iterate. One-of-a-kind state is a resource.
		//
		// A camera stored as a component on one entity costs an archetype, a
		// query and a loop that runs once. It also stops being findable: "which
		// entity has the camera" is a question the world cannot answer without
		// a search, where `Resource<ActiveCamera>()` is a lookup.
		//
		// Resources live outside the table space entirely, so no query can
		// reach one. A type used as a component *and* as a resource therefore
		// cannot silently gain a row in `Each<T>` - not because something
		// remembered to hide it, but because there is nowhere for it to appear.
		//
		// WorldTime is reserved for the dedicated clock API. Do not set, mutate,
		// or remove it through these generic methods; Time() assumes the resource
		// created by the constructor is still present.

		// Adds or replaces one world-scoped resource.
		//
		// @param value The resource value to copy into the store.
		template <class T> void SetResource(const T &value) {
			RequireOwningThread("SetResource");
			SetResourceRaw(Components::Of<T>(), &value);
		}

		// Null when unset, for the same reason Get is: a default-constructed
		// resource standing in for one nobody set is a bug that reads as
		// working code.
		//
		// Unlike the previous storage, this pointer is **not** invalidated by
		// setting an unrelated resource. Each resource owns its own storage,
		// so only removing or re-setting *this* one moves it.
		//
		// @return A read-only resource pointer, or `nullptr` when unset.
		template <class T> const T *Resource() const {
			return static_cast<const T *>(GetResourceRaw(Components::Of<T>()));
		}

		// Mutable access to one world-scoped resource.
		//
		// @return A mutable resource pointer, or `nullptr` when unset.
		template <class T> T *ResourceMutable() {
			RequireOwningThread("ResourceMutable");
			return static_cast<T *>(GetResourceRawMutable(Components::Of<T>()));
		}

		// Reports whether the world contains a resource of the requested type.
		//
		// @return `true` when the resource is present.
		template <class T> bool HasResource() const {
			return GetResourceRaw(Components::Of<T>()) != nullptr;
		}

		// Removes one world-scoped resource when present.
		template <class T> void RemoveResource() {
			RequireOwningThread("RemoveResource");
			RemoveResourceRaw(Components::Of<T>());
		}

		// --- time ----------------------------------------------------------
		//
		// The clock is created as a resource and has dedicated read/write methods.
		// Callers must not reach it through the generic resource API above: Time()
		// assumes it remains present, and systems receive a copy to read.

		// By value, not by reference. It is 32 bytes, read once per system, and
		// returning a copy makes two hazards impossible at once - a reference
		// left dangling by a later resource change, and a system quietly
		// writing to the clock instead of reading it.
		//
		// @return A copy of the world's current clock state.
		WorldTime Time() const;

		// One simulation step. Advances Elapsed and Tick and records the fixed
		// delta the step used.
		//
		// @param delta Simulated seconds advanced by this tick.
		void AdvanceTick(float delta);

		// The presentation side of the clock, set once per frame before the
		// render phases. Never touched by a headless world.
		//
		// @param frameDelta Wall seconds elapsed since the previous frame.
		// @param alpha      Interpolation position between the last and next tick.
		void SetFrame(float frameDelta, float alpha);

		// --- iteration -----------------------------------------------------

		// Visits every entity carrying all of Ts. The callback takes the entity
		// and one reference per component, with the requested const qualification
		// and in the order named.
		//
		// Structural changes during iteration - Create, Destroy, adding a
		// component - are deferred until the loop ends. They are therefore
		// safe, and not visible to the loop that made them.
		//
		// Writes *through* a component reference are direct memory writes and
		// are unaffected.
		//
		// @param body Called as `body(Entity, Ts &...)` for every matching entity.
		template <class... Ts, class Body> void Each(Body &&body) {
			RequireOwningThread("Each");

			const ComponentId terms[] = {Components::Of<std::remove_const_t<Ts>>()...};
			const DeferScope defer(*this);

			VisitTables(terms, [&](const TableSlice &slice) {
				VisitRows<Ts...>(slice, body, std::index_sequence_for<Ts...>{});
			});
		}

		// Each, one call per contiguous run of rows instead of one per entity.
		//
		// The body is handed a row count and one array pointer per component,
		// which is what the storage already holds - a table is columns of
		// contiguous rows, and Each spends its time turning that back into one
		// call per row. A system that writes a packed output array wants the
		// columns, not the rows: the loop is then something the compiler can
		// unroll and vectorise, and the per-entity call disappears.
		//
		// Use it when the body is uniform across rows and the output is an
		// array. Use Each when the body branches per entity, needs the Entity,
		// or is doing something structural - this one hands out raw pointers
		// into live tables and gives up all three:
		//
		//   - **No structural changes.** Not deferred, for the same reason
		//     EachParallel is not: deferring exists to make a Create or Destroy
		//     inside the loop safe, and one here would move the very arrays the
		//     body is holding.
		//   - **No Entity.** A body that needs entities wants Each.
		//
		// Batches arrive in table order, and a table is not a unit anybody
		// declared: adding a component to one entity moves it to another table
		// and changes how the rows divide. So the *number* of batches and their
		// sizes are not stable across frames, and only the concatenation of them
		// is meaningful. Nothing may depend on where one batch ends.
		//
		// @param body Called as `body(size_t rows, Ts *...columns)` once per
		//             table run, with `rows` always non-zero.
		// @tick
		template <class... Ts, class Body> void EachBatch(Body &&body) {
			RequireOwningThread("EachBatch");

			const ComponentId terms[] = {Components::Of<std::remove_const_t<Ts>>()...};

			VisitTables(terms, [&](const TableSlice &slice) {
				VisitBatch<Ts...>(slice, body, std::index_sequence_for<Ts...>{});
			});
		}

		// EachBatch, spread across the job system's workers.
		//
		// The body is handed the index its first row occupies *in the iteration
		// as a whole*, so a caller filling a packed output array writes to
		// `output[first + row]` and nothing else. That is what makes this safe
		// to parallelise where EachBatch's running cursor is not: the slices are
		// disjoint by construction rather than by an atomic, and two runs of the
		// same world produce the same array in the same order.
		//
		// Rows are partitioned within a table and tables are walked in order, so
		// `first` is deterministic even though the work is not ordered.
		//
		// Every restriction EachBatch and EachParallel carry applies here, and
		// one is worth repeating because the pointers make it easy to break:
		// **no writes outside the rows the body was given.**
		//
		// @param body  Called concurrently as `body(size_t first, size_t rows,
		//              Ts *...columns)`, with `rows` always non-zero.
		// @param grain The minimum run of rows worth handing to a worker.
		// @return Rows visited in total, which is what the caller should size
		//         its output down to afterwards.
		// @tick
		template <class... Ts, class Body>
		size_t EachBatchParallel(Body &&body, size_t grain = parallel::Jobs::DEFAULT_GRAIN) {
			RequireOwningThread("EachBatchParallel");

			const ComponentId terms[] = {Components::Of<std::remove_const_t<Ts>>()...};
			size_t visited = 0;

			VisitTables(terms, [&](const TableSlice &slice) {
				visited +=
					VisitBatchParallel<Ts...>(slice, body, grain, visited, std::index_sequence_for<Ts...>{});
			});

			return visited;
		}

		// Each, spread across the job system's workers.
		//
		// Parallel *within* a tick, not asynchronous across ticks: this blocks
		// until every entity has been visited, so the tick is still one thing
		// that starts and finishes. That is what keeps a recorded run
		// replayable - a result that lands a tick later on a slower machine
		// would not.
		//
		// The body runs on many threads at once. It may read and write the
		// components it is handed, and touch nothing else:
		//
		//   - **No structural changes.** A checked mutation from a worker aborts
		//     on thread affinity. Small jobs may run inline on the owning thread,
		//     where that check cannot enforce this rule, so the body must not call
		//     Create, Destroy, Set or Remove in either execution mode.
		//   - **No writes outside the row.** Two workers hold neighbouring
		//     rows of the same array; reaching sideways is a race the affinity
		//     check cannot see.
		//
		// `grain` is the smallest run of rows worth handing to another thread.
		// Below it the whole table runs inline, because waking a worker costs
		// more than a short loop.
		//
		// Rows are partitioned within a table, and tables are walked in order.
		// A scene of one archetype - the common case - parallelises completely;
		// a scene of a thousand tiny archetypes parallelises hardly at all, and
		// that is a storage-layout problem rather than something more threads
		// would fix.
		//
		// **This is slower than Each below a crossover, and the crossover is far
		// higher than it looks.** Re-measured by `engine.ecs.bench.iteration` in
		// the `bench` preset at `-O3`, on a 24-thread machine, over three float
		// adds per row - the cheapest body there is:
		//
		//     entities       Each   EachParallel
		//        8 192    1.45 us      25.9 us     18x slower
		//       32 768    5.54 us      31.5 us      5.7x slower
		//      131 072    23.3 us      36.1 us      1.5x slower
		//      262 144    49.1 us      48.6 us     the crossover
		//      500 000    96.1 us      72.1 us      1.3x faster
		//
		// **Below 262,144 rows this call is a loss, and the default grain lets
		// it be made from 32,768.** Two things moved it there and only one of
		// them is this module's: the serial loop halved when the build went to
		// `-O3`, and it had already fallen by half again with the chunked
		// storage. The pool's handover did not move - about 31 us, measured
		// empty by `engine.parallel.bench.dispatch` - so the row count that
		// repays it went up by the same factor the loop came down.
		//
		// The ceiling past the crossover is about 1.3x rather than the core
		// count, and that is memory bandwidth rather than threads: at 500k rows
		// both paths are streaming twelve megabytes out of DRAM.
		//
		// **A body more expensive than three adds crosses far sooner and should
		// pass a grain.** `physics::IntegrateMotion` carries a `CFrame` per row
		// and crosses near 8,000; it passes 1024 and says why at the constant.
		//
		// @param body  Called concurrently as `body(Entity, Ts &...)` for each match.
		// @param grain The minimum table-row range worth handing to a worker.
		// @tick
		template <class... Ts, class Body>
		void EachParallel(Body &&body, size_t grain = parallel::Jobs::DEFAULT_GRAIN) {
			RequireOwningThread("EachParallel");

			const ComponentId terms[] = {Components::Of<std::remove_const_t<Ts>>()...};

			// Not deferred, unlike Each. Deferring exists to make a structural
			// change inside the loop safe, and structural changes are not
			// allowed here at all.
			VisitTables(terms, [&](const TableSlice &slice) {
				VisitRowsParallel<Ts...>(slice, body, grain, std::index_sequence_for<Ts...>{});
			});
		}

		// How many entities Each would visit.
		//
		// The plan is built once per term list and kept, so a system may call
		// this every tick. The cached plan is a live view rather than a
		// snapshot: entities and tables created afterwards are counted by the
		// next call.
		//
		// @return The live number of entities carrying every requested component.
		template <class... Ts> size_t CountMatching() {
			static_assert(sizeof...(Ts) > 0, "CountMatching needs at least one component.");
			RequireOwningThread("CountMatching");

			const ComponentId terms[] = {Components::Of<std::remove_const_t<Ts>>()...};
			return CountRows(terms);
		}

		// The most `With` and `Without` terms one query may name.
		//
		// A filter is a handful of components. Eight is far past anything real,
		// and a query that wants a ninth is a query that has stopped describing
		// a set - so this refuses rather than growing, which is the same call
		// `Components::Adopt` makes about a type registered twice.
		static constexpr size_t MAX_FILTER_TERMS = 8;

		// A query being built: the components to hand the body, and the ones to
		// match on without handing over.
		//
		// **One shape for every iteration this class offers**, which is the
		// point of it. `Each`, `EachBatch`, `EachParallel` and `CountMatching`
		// each took a term list and nothing else, so a query that wanted to say
		// "and not a wall" had to say it as a branch inside the body - over rows
		// the loop should never have visited, once per row per tick, forever.
		// The filter is answered per archetype instead.
		//
		// Built by `Store::Query` and finished by one of the terminal calls
		// below. Nothing is evaluated until then, so the chain costs nothing on
		// its own:
		//
		//     store.Query<Transform, const Motion>()
		//          .With<Collider>()
		//          .Without<Anchored>()
		//          .Each([](Entity, Transform &frame, const Motion &motion) { ... });
		//
		// A selection holds a reference to its store and spans into its own
		// arrays, so it is a scope-local value: build it, finish it, drop it.
		//
		// @since v0.15
		template <class... Ts> class Selection {
		  public:
			// Requires `Us...` without handing them to the body.
			//
			// **The half worth reaching for first.** A tag has no data, so
			// naming it in `Query<...>` forces the body to take a parameter it
			// cannot read; this says the same thing and leaves the signature
			// alone. Repeatable, and the sets are unioned.
			//
			// @return This selection, for chaining.
			template <class... Us> Selection &With() {
				Append(RequiredTerms, RequiredCount, "With", {Components::Of<std::remove_const_t<Us>>()...});
				return *this;
			}

			// Refuses any entity carrying one of `Us...`.
			//
			// Answered per archetype, so an exclusion costs one binary search
			// per table the first time the plan meets it and nothing
			// afterwards. Repeatable, and the sets are unioned.
			//
			// @return This selection, for chaining.
			template <class... Us> Selection &Without() {
				Append(
					ExcludedTerms, ExcludedCount, "Without", {Components::Of<std::remove_const_t<Us>>()...}
				);
				return *this;
			}

			// `Store::Each`, over what this selection matches.
			//
			// @param body Called as `body(Entity, Ts &...)` for every match.
			template <class Body> void Each(Body &&body) {
				Owner->RequireOwningThread("Query::Each");

				const DeferScope defer(*Owner);
				Owner->VisitTables(Terms(), [&](const TableSlice &slice) {
					Owner->VisitRows<Ts...>(slice, body, std::index_sequence_for<Ts...>{});
				});
			}

			// `Store::EachBatch`, over what this selection matches.
			//
			// @param body Called as `body(size_t rows, Ts *...columns)` per run.
			template <class Body> void EachBatch(Body &&body) {
				Owner->RequireOwningThread("Query::EachBatch");

				Owner->VisitTables(Terms(), [&](const TableSlice &slice) {
					Owner->VisitBatch<Ts...>(slice, body, std::index_sequence_for<Ts...>{});
				});
			}

			// `Store::EachParallel`, over what this selection matches.
			//
			// Every restriction that call carries applies here unchanged - most
			// of all that the body makes no structural change and writes no row
			// but its own.
			//
			// @param body  Called concurrently as `body(Entity, Ts &...)`.
			// @param grain The minimum run of rows worth handing to a worker.
			template <class Body>
			void EachParallel(Body &&body, size_t grain = parallel::Jobs::DEFAULT_GRAIN) {
				Owner->RequireOwningThread("Query::EachParallel");

				Owner->VisitTables(Terms(), [&](const TableSlice &slice) {
					Owner->VisitRowsParallel<Ts...>(slice, body, grain, std::index_sequence_for<Ts...>{});
				});
			}

			// `Store::EachBatchParallel`, over what this selection matches.
			//
			// @param body  Called concurrently as `body(size_t first, size_t
			//              rows, Ts *...columns)`.
			// @param grain The minimum run of rows worth handing to a worker.
			// @return Rows visited in total.
			template <class Body>
			size_t EachBatchParallel(Body &&body, size_t grain = parallel::Jobs::DEFAULT_GRAIN) {
				Owner->RequireOwningThread("Query::EachBatchParallel");

				size_t visited = 0;
				Owner->VisitTables(Terms(), [&](const TableSlice &slice) {
					visited += Owner->VisitBatchParallel<Ts...>(
						slice, body, grain, visited, std::index_sequence_for<Ts...>{}
					);
				});
				return visited;
			}

			// How many entities `Each` would visit.
			//
			// @return The live number of entities this selection matches.
			size_t Count() {
				Owner->RequireOwningThread("Query::Count");
				return Owner->CountRows(Terms());
			}

		  private:
			friend class Store;

			explicit Selection(Store &owner)
				: Owner(&owner), BoundTerms{Components::Of<std::remove_const_t<Ts>>()...} {}

			void Append(
				ComponentId (&into)[MAX_FILTER_TERMS],
				size_t &count,
				const char *what,
				std::initializer_list<ComponentId> ids
			) {
				for (const ComponentId id : ids) {
					if (count == MAX_FILTER_TERMS) {
						Owner->TooManyFilterTerms(what);
					}
					into[count++] = id;
				}
			}

			QueryTerms Terms() const {
				return QueryTerms{
					std::span<const ComponentId>(BoundTerms, sizeof...(Ts)),
					std::span<const ComponentId>(RequiredTerms, RequiredCount),
					std::span<const ComponentId>(ExcludedTerms, ExcludedCount),
				};
			}

			Store *Owner = nullptr;

			// One extra, so a selection binding nothing still declares an array
			// rather than a zero-length one.
			ComponentId BoundTerms[sizeof...(Ts) + 1];
			ComponentId RequiredTerms[MAX_FILTER_TERMS];
			ComponentId ExcludedTerms[MAX_FILTER_TERMS];
			size_t RequiredCount = 0;
			size_t ExcludedCount = 0;
		};

		// Begins a query binding `Ts...` to the body's parameters.
		//
		// `Each<Ts...>(body)` is this with no filters and is still the right
		// call when there are none. Reach for this one to add a `With` or a
		// `Without`; see `Selection`.
		//
		// @return A selection to add filters to and then finish.
		// @since v0.15
		template <class... Ts> Selection<Ts...> Query() {
			return Selection<Ts...>(*this);
		}

		// Visits every entity carrying all of `components`, named at run time.
		//
		// **The runtime counterpart of `Each<Ts...>`, and it hands over the
		// entity alone.** `Each` gives a reference per component because the
		// caller named the types and the compiler knows what they are; a caller
		// that resolved *names* has no type to hand a reference to, so it reads
		// what it wants through `GetComponent` and the descriptor behind it.
		// That is the same split the component accessors above already draw.
		//
		// This is what a script's query is: a list of component names, resolved
		// once, and the entities carrying all of them. An empty list matches
		// nothing rather than everything - a query is defined by what it names,
		// and `EachEntity` is the walk that answers "everything".
		//
		// Tables are walked in id order and rows in row order, so two runs of
		// one world visit the same entities in the same sequence.
		//
		// **Structural changes are deferred**, exactly as they are inside
		// `Each`: the body is free to create and destroy, and the loop will not
		// see it.
		//
		// @param components The components an entity must carry, in any order.
		// @param body       Called as `body(Entity)` for each match.
		// @since v0.12
		void EachMatching(std::span<const ComponentId> components, const std::function<void(Entity)> &body);

		// How many entities `EachMatching` would visit.
		//
		// The plan is cached per term list exactly as `CountMatching<Ts...>`'s
		// is, so a caller may ask every tick.
		//
		// @param components The components an entity must carry.
		// @return The live number of entities carrying every one of them.
		// @since v0.12
		size_t CountMatching(std::span<const ComponentId> components);

		// --- instances -----------------------------------------------------
		//
		// The Roblox-shaped half. An instance is an entity in the archetype its
		// class names, carrying `InstanceClass`, `Hierarchy` and `InstanceName`
		// alongside whatever else the class holds.
		//
		// The tree is organisational, not spatial: parenting moves nothing and
		// propagates nothing, because a part's transform is world-space. That
		// is Roblox's model and it is why the hierarchy costs four handles per
		// node instead of a scene graph.

		// Creates an instance of a class, starting from its prototype row.
		//
		// A column copy per component rather than a constructor call, which is
		// also what makes `Clone` need no separate machinery.
		//
		// Mints an **authoritative** entity, so an adopt-only store refuses it
		// exactly as it refuses `Create`. That check used to be missing here,
		// and `scene::MakePart` carried a copy of it because this path walked
		// straight past the flag - one minting path honouring the rule and one
		// not is worse than neither, because the one that does makes the other
		// look covered.
		//
		// @param id   The class to instantiate.
		// @param name The instance's name, which need not be unique.
		// @return The new instance, NULL_ENTITY for an invalid class, or
		//         NULL_ENTITY in an adopt-only store.
		Entity CreateInstance(ClassId id, std::string_view name = {});

		// Creates a locally predicted instance from a class prototype.
		//
		// The predicted range is the only range a replica owns. This is the
		// instance-shaped counterpart of `CreatePredicted`, for an editor or a
		// prediction system making a row that must not collide with authority.
		//
		// @param id   The class to instantiate.
		// @param name The instance's name, which need not be unique.
		// @return The new predicted instance, or NULL_ENTITY for an invalid class.
		Entity CreatePredictedInstance(ClassId id, std::string_view name = {});

		// Copies one instance, its components and its whole subtree.
		//
		// Mints an authoritative entity, so an adopt-only store refuses it for
		// the same reason it refuses `Create`.
		//
		// The copy is parented nowhere, exactly as `:Clone()` leaves it - a
		// clone that appeared in the world at the moment it was made would run
		// its scripts before the caller had finished configuring it.
		//
		// References *inside* the subtree are rewritten to point at the copies.
		// A reference pointing out of it is left alone, because the thing it
		// names was not copied.
		//
		// @param source The instance to copy.
		// @return The copy, or NULL_ENTITY when the source is not an instance.
		Entity CloneInstance(Entity source);

		// Copies an instance and its subtree into the predicted entity range.
		//
		// @param source The instance to copy.
		// @return The predicted copy, parented nowhere, or NULL_ENTITY when the
		//         source is not an archivable instance.
		Entity ClonePredictedInstance(Entity source);

		// Destroys an instance and everything under it.
		//
		// Roblox keeps a destroyed instance readable while a script holds a
		// reference; here the row is freed and the handle becomes a tombstone.
		// Zombie rows are a cost every iterating system pays forever, and a
		// generation check already makes the stale handle safe.
		//
		// Deferred when called from inside Each.
		//
		// **This is the engine moving its own furniture and is never refused.**
		// `studio::PlayLink` destroying a player, `Debris` draining its queue,
		// `RojoSync` rebuilding a subtree and `Editor::EnsureViewerCamera`
		// destroying a camera are all this call, and a guard here would refuse
		// every one of them. What an *author* asks for goes through
		// `DestroyAuthored`.
		//
		// @param instance The root of the subtree to destroy.
		void DestroyInstance(Entity instance);

		// Destroys an instance on behalf of whoever is authoring the world.
		//
		// **The same call with the protection applied**, and the split is the
		// whole design. "An author" is a question only the caller can answer:
		// a script's `Destroy()`, the editor's Delete key and the properties
		// panel are authored, and every move the engine makes on its own behalf
		// is not. A flag on the store would have to be set and unset around each
		// of those, which is a second thing to forget.
		//
		// @param instance The root of the subtree to destroy.
		// @return `false` when `Protected` refuses it, having destroyed nothing.
		bool DestroyAuthored(Entity instance);

		// The class an entity was created as.
		//
		// @param instance The entity to ask about.
		// @return The class, or an invalid id when it is not an instance.
		ClassId ClassOf(Entity instance) const;

		// Reports whether an instance is of a class or one derived from it.
		//
		// @param instance The entity to test.
		// @param id       The class to test against.
		// @return `true` when the instance's class is `id` or descends from it.
		bool IsA(Entity instance, ClassId id) const;

		// --- properties by name ---------------------------------------------
		//
		// The runtime path a binding uses: a name learned at run time, a value
		// whose type came with it, and no template in sight.
		//
		// **This is the first write into the storage that does not go through a
		// typed `Set<T>`**, so every safety property the typed path gets from
		// the compiler - right component, right size, right world - this one has
		// to get from a check. The change mark is the exception and comes for
		// free: a conversion reaches its component through `GetMutable`, which
		// already counts as a write.
		//
		// Bytes and a size rather than a template, because the caller is a
		// marshalling layer holding a value it typed at run time. The size is
		// checked against the descriptor, so a `Vector3` handed to a `CFrame`
		// property fails instead of writing twelve bytes into twenty-eight and
		// leaving the rest of the rotation behind.

		// Reads one property of an instance by name.
		//
		// @param instance The instance to read.
		// @param property The property name, as a script spells it.
		// @param out      Where to write the value.
		// @param bytes    The size of `out`, which must match the descriptor.
		// @return `false` when there is no such property, the size disagrees, or
		//         the instance does not carry what the conversion reads.
		bool GetProperty(Entity instance, core::Name property, void *out, size_t bytes) const;

		// Writes one property of an instance by name.
		//
		// A structural property - `Anchored` - moves the row to another
		// archetype, so inside iteration it is deferred by the same queue every
		// other structural change uses.
		//
		// @param instance The instance to write.
		// @param property The property name, as a script spells it.
		// @param value    The value to write.
		// @param bytes    The size of `value`, which must match the descriptor.
		// @return `false` when there is no such property, the size disagrees,
		//         the property is read-only, this store is adopt-only, or the
		//         instance does not carry what the conversion writes.
		bool SetProperty(Entity instance, core::Name property, const void *value, size_t bytes);

		// The named form of `SetPropertyAuthored`.
		//
		// @param instance The instance to write.
		// @param property The property name.
		// @param value    The value to write.
		// @param bytes    The size of `value`.
		// @return `false` on the ordinary lookup or validation failures.
		bool SetPropertyAuthored(Entity instance, core::Name property, const void *value, size_t bytes);

		// The properties an instance's class exposes, merged with its bases'.
		//
		// @param instance The instance to describe.
		// @return The descriptors, or an empty span when it is not an instance.
		std::span<const PropertyDescriptor> PropertiesOf(Entity instance) const;

		// --- the same two, for a caller that already holds the descriptor ----
		//
		// **The by-name pair above look up the descriptor, and a binding has
		// already done that.** A script write goes `__newindex` → find the
		// property by name → marshal the value → `SetProperty(name)` → find the
		// property by name *again*. Each of those finds is a `Classes::Describe`
		// - which takes the class table's lock - plus a linear scan of the
		// class's merged property list.
		//
		// So the by-name pair are for a caller holding a string, and these are
		// for one holding a descriptor. Roughly half the class-table traffic in
		// a scripted frame was the second lookup of a descriptor the caller was
		// standing on.
		//
		// **The descriptor must be one of this instance's.** It is not
		// re-validated against the class, because the only way to get one is
		// `PropertiesOf` or the by-name lookup, and a caller that mixed two
		// instances' descriptors would be reaching past the only API that
		// produces them. What *is* still checked is everything a wrong value
		// could do: the size, the enum membership, and whether this store may be
		// written at all.

		// Reads one property of an instance through a descriptor already found.
		//
		// @param instance   The instance to read.
		// @param descriptor One of this instance's class's property descriptors.
		// @param out        Where to write the value.
		// @param bytes      The size of `out`, which must match the descriptor.
		// @return `false` when the size disagrees or the instance does not carry
		//         what the conversion reads.
		// @since v0.8
		bool
		GetProperty(Entity instance, const PropertyDescriptor &descriptor, void *out, size_t bytes) const;

		// Writes one property of an instance through a descriptor already found.
		//
		// @param instance   The instance to write.
		// @param descriptor One of this instance's class's property descriptors.
		// @param value      The value to write.
		// @param bytes      The size of `value`, which must match the descriptor.
		// @return `false` when the size disagrees, the property is read-only,
		//         this store is adopt-only, the value is not a member of the
		//         named enum, or the instance does not carry what the conversion
		//         writes.
		// @since v0.8
		bool
		SetProperty(Entity instance, const PropertyDescriptor &descriptor, const void *value, size_t bytes);

		// Writes a property on behalf of an editor authoring this exact store.
		//
		// Unlike the runtime path, this may write an authority-owned row in an
		// adopt-only store. It is deliberately explicit: a client script still
		// receives the ordinary refusal, while an editor inspecting a client view
		// can make a local change that the next authoritative delta may replace.
		//
		// @param instance   The instance to write.
		// @param descriptor One of this instance's property descriptors.
		// @param value      The value to write.
		// @param bytes      The size of `value`.
		// @return `false` on the ordinary validation failures.
		bool SetPropertyAuthored(
			Entity instance, const PropertyDescriptor &descriptor, const void *value, size_t bytes
		);

		// Moves an instance under a new parent, or to no parent.
		//
		// Appends to the end of the sibling list, so `EachChild` yields
		// insertion order - which replication and replay both depend on.
		//
		// **Assigning the parent it already has is a no-op**, exactly as it is
		// in Roblox. Without that it was an unlink and an append, which moved
		// the instance to the back of its own siblings and changed what
		// `GetChildren()` returns for a write that changed nothing.
		//
		// Refuses to make an instance its own ancestor, because a cycle in the
		// tree is a hang in every walk of it rather than a wrong answer.
		//
		// @param instance The instance to move.
		// @param parent   The new parent, or NULL_ENTITY to detach.
		// @return `false` when the move was refused.
		bool SetParent(Entity instance, Entity parent);

		// Reparents an instance on behalf of whoever is authoring the world.
		//
		// `DestroyAuthored`'s twin, for `DestroyAuthored`'s reason.
		//
		// @param instance The instance to move.
		// @param parent   The new parent, or NULL_ENTITY to detach.
		// @return `false` when the move was refused.
		bool SetParentAuthored(Entity instance, Entity parent);

		// Refuses an author's `Destroy` and `.Parent` on this instance.
		//
		// **A set of entities rather than a flag on a component, because the
		// fact lives above `ecs` and this is L2.** `scene::ServiceComponent::
		// Fixture` is what decides - a world with no `Workspace` is not a world
		// an author meant to build, and deleting one turns every
		// `game:GetService` in the place into a runtime error a long way from
		// the delete that caused it - and `scene` is L7. A mirrored flag on an
		// `ecs` row would be the second copy of one fact rule 2 exists to
		// refuse, so the store holds identity and nothing else.
		//
		// **Filled by `scene::InstallServices` and by nothing else**, which is
		// what keeps it from being configuration somebody forgets. That function
		// is the one door a service arrives through, it is idempotent, and it
		// runs on a world loaded from a file as well as on a fresh one - so a
		// world that has services has this, by construction rather than by
		// somebody remembering. `SetAdoptOnly` is the shape *not* to copy: it is
		// set by exactly one caller and nothing checks that it was.
		//
		// @param instance The instance an author may not remove.
		void Protect(Entity instance);

		// Whether an author's `Destroy` or `.Parent` is refused on this
		// instance.
		//
		// @param instance The instance to ask about.
		// @return `true` when it is protected.
		bool Protected(Entity instance) const;

		// The parent of an instance.
		//
		// @param instance The instance to ask about.
		// @return The parent, or NULL_ENTITY for a root or a non-instance.
		Entity ParentOf(Entity instance) const;

		// Visits every child in insertion order.
		//
		// Safe against the body reparenting or destroying the child it was
		// handed, because the next sibling is read before the body runs.
		//
		// @param instance The parent whose children to visit.
		// @param body     Called as `body(Entity)` for each child.
		void EachChild(Entity instance, const std::function<void(Entity)> &body) const;

		// Visits everything under an instance, nearest first.
		//
		// **Depth first, in the order a recursive walk written by hand would
		// produce** - a child, then everything under that child, then the next
		// child. That is Roblox's `GetDescendants` order, and scripts index
		// into the result, so it is a contract rather than a detail.
		//
		// The instance itself is not visited. Iterative rather than recursive:
		// a scene's depth is the author's to choose.
		//
		// @param instance The root of the walk, not itself visited.
		// @param body     Called as `body(Entity)` for each descendant.
		void EachDescendant(Entity instance, const std::function<void(Entity)> &body) const;

		// Renames an instance.
		//
		// **Not `Create(name)`**, which is the store's own lookup of singular
		// things. An instance name is not unique: siblings may share one,
		// exactly as they may in Roblox.
		//
		// @param instance The instance to rename.
		// @param name     The new name. An empty one leaves it unnamed.
		// @return `false` when the entity is not an instance.
		bool SetInstanceName(Entity instance, std::string_view name);

		// The first child with a name, searching in insertion order.
		//
		// A walk rather than an index, because most instances have few children
		// and an index per node would allocate for every one of them.
		//
		// @param instance  The parent to search.
		// @param name      The name to find. An empty name matches nothing.
		// @param recursive Whether to search the whole subtree. The children
		//                  are still answered first, so the nearest match wins
		//                  rather than whichever one a depth-first pass reached
		//                  soonest.
		// @return The instance, or NULL_ENTITY when none matches.
		Entity FindFirstChild(Entity instance, std::string_view name, bool recursive = false) const;

		// The first child created as exactly a class.
		//
		// **Exactly, which is what separates it from `FindFirstChildWhichIsA`.**
		// A `Part` is a `BasePart`, so asking for a `BasePart` this way finds
		// nothing; that is Roblox's split and the reason both exist.
		//
		// @param instance The parent to search.
		// @param id       The class to match.
		// @return The child, or NULL_ENTITY when none matches.
		Entity FindFirstChildOfClass(Entity instance, ClassId id) const;

		// The first child of a class or one derived from it.
		//
		// @param instance  The parent to search.
		// @param id        The class to match against.
		// @param recursive Whether to search the whole subtree.
		// @return The instance, or NULL_ENTITY when none matches.
		Entity FindFirstChildWhichIsA(Entity instance, ClassId id, bool recursive = false) const;

		// The nearest ancestor with a name.
		//
		// @param instance The instance to search above. Not itself considered.
		// @param name     The name to find. An empty name matches nothing.
		// @return The ancestor, or NULL_ENTITY when none matches.
		Entity FindFirstAncestor(Entity instance, std::string_view name) const;

		// The nearest ancestor created as exactly a class.
		//
		// @param instance The instance to search above. Not itself considered.
		// @param id       The class to match.
		// @return The ancestor, or NULL_ENTITY when none matches.
		Entity FindFirstAncestorOfClass(Entity instance, ClassId id) const;

		// The nearest ancestor of a class or one derived from it.
		//
		// @param instance The instance to search above. Not itself considered.
		// @param id       The class to match against.
		// @return The ancestor, or NULL_ENTITY when none matches.
		Entity FindFirstAncestorWhichIsA(Entity instance, ClassId id) const;

		// Starts recording reparents, so that a listener can be told about them.
		//
		// **Opt in, exactly as observing a component is.** A world nobody is
		// watching stores nothing and pays one branch per `SetParent`; the
		// alternative is a list that grows for the life of every world with no
		// script draining it. Idempotent, and there is no way back off - the
		// same shape `Observe` has, for the same reason: a second listener
		// connecting must not be able to switch the first one off.
		void ObserveTree();

		// Whether reparents are being recorded.
		//
		// @return `true` once `ObserveTree` has been called.
		bool TreeObserved() const;

		// Hands over everything recorded since the last call, and forgets it.
		//
		// **Taken rather than read, because a half-drained queue is the bug
		// this shape removes.** A caller that walked the list and then cleared
		// it would lose anything a listener caused while it was walking; a swap
		// leaves the store's list empty before the first listener runs, so a
		// reparent made from inside a handler belongs to the next delivery.
		//
		// @param out Cleared, then swapped with what the store has recorded.
		void TakeTreeChanges(std::vector<TreeChange> &out);

		// Installs the one listener told about a removal *before* it happens.
		//
		// **The only synchronous signal in the engine, and the only one that
		// has to be.** Everything else is recorded and delivered at the next
		// barrier, because a handler re-entering from inside a write would see
		// a half-written row - `script/Changes.hpp` sets out that rule. This
		// one is called at the *top* of the operation, before a single link
		// moves, so the tree it is handed is entirely consistent. That is also
		// the only position from which its contract can hold at all: a handler
		// told "this is leaving" is told while it is still there, which a queue
		// drained a tick later cannot offer.
		//
		// Called as `body(ancestor, descendant)` once per losing ancestor per
		// leaving instance, which is Roblox's fan-out for `DescendantRemoving`.
		//
		// **One listener, replaced rather than added to.** A world runs one
		// script runtime, and a second subscriber would need a disconnect
		// mechanism for a surface with one caller.
		//
		// The listener must be cleared before it outlives whatever it captured.
		// See `ClearDescendantRemoving`.
		//
		// @param body What to call. An empty function is the same as clearing.
		void OnDescendantRemoving(std::function<void(Entity, Entity)> body);

		// Takes the removal listener back.
		//
		// **Called before the store outlives the VM**, exactly as
		// `script::ChangeQueue::Detach` is: a store is often declared before
		// the runtime that binds it and destroyed after, so a listener holding
		// a `lua_State *` outlives the state unless something takes it back.
		void ClearDescendantRemoving();

		// The dotted path from the root of the tree down to an instance.
		//
		// **From the world's root, and there is no `game.` in front of it.** A
		// world's roots *are* the services - `scene::InstallServices` puts
		// `Workspace` and the rest in as ordinary instances - so a part inside
		// a model comes out as `Workspace.Model.Part`, which is what Roblox
		// prints for the same thing.
		//
		// @param instance The instance to describe.
		// @return The path, or an empty string when it is not an instance.
		std::string GetFullName(Entity instance) const;

		// Whether an instance has any children at all.
		//
		// **O(1), which `EachChild` is not.** A tree view asks this per row to
		// decide whether to draw an expander, and answering it with `EachChild`
		// walks the entire sibling list to set a bool - free on a leaf, and two
		// hundred steps on the root of a two-hundred-part scene, every frame.
		// `Hierarchy::FirstChild` is the answer already.
		//
		// @param instance The instance to ask about.
		// @return `true` when it has at least one child.
		bool HasChildren(Entity instance) const;

		// Visits every instance with no parent.
		//
		// **The world's own children.** `SetParent(instance, NULL_ENTITY)` means
		// "a root of this world", and `script`'s `workspace` is the world - so
		// `workspace:GetChildren()` is this. `EachChild` cannot answer it,
		// because the null entity carries no `Hierarchy` and therefore heads no
		// sibling list.
		//
		// **Ordered by entity id, which is creation order and not insertion
		// order**, and the difference is worth stating rather than hiding. A
		// child list is threaded in insertion order, so reparenting moves an
		// instance to the end of its new siblings; a root that was detached and
		// reattached keeps its original place here. Deterministic either way,
		// which is what a recording needs - an archetype walk would not have
		// been, because a row moves when its archetype does.
		//
		// The cost is a scan of every instance in the world. That is why this is
		// a call and not a cached list: a root list maintained on every reparent
		// would be a second copy of a fact `Hierarchy::Parent` already holds,
		// and rule 2 exists because two copies drift.
		//
		// @param body Called as `body(Entity)` for each root.
		void EachRoot(const std::function<void(Entity)> &body) const;

		// The first root with a name, in the order `EachRoot` visits.
		//
		// @param name The name to find.
		// @return The root, or NULL_ENTITY when none matches.
		Entity FindFirstRoot(std::string_view name) const;

		// Reports whether one instance is inside another's subtree.
		//
		// @param instance The instance to test.
		// @param ancestor The subtree root to test against.
		// @return `true` when instance is ancestor or sits beneath it.
		bool IsDescendantOf(Entity instance, Entity ancestor) const;

		// The name an instance carries.
		//
		// @param instance The instance to ask about.
		// @return The name, or an invalid Name when unnamed.
		core::Name InstanceNameOf(Entity instance) const;

		// --- components, named at runtime ------------------------------------
		//
		// The runtime counterpart of `Set<T>` and friends, for a layer that
		// resolves component *names* rather than naming types. Replication
		// reads a name off a wire and a game file will name components in text;
		// neither can be a template parameter.
		//
		// The value is raw, so a caller writes and reads it through the
		// component's own `TypeDescriptor` - which is what makes a type with a
		// custom serialiser behave correctly instead of crossing as its object
		// representation.

		// Adds or replaces a component named at runtime.
		//
		// @param entity    The entity to write to.
		// @param component The component's id, from `Components::Find`.
		// @param value     A pointer to a value of that type, or null for a tag.
		void SetComponent(Entity entity, ComponentId component, const void *value);

		// Whether an entity carries a component named at runtime.
		//
		// @param entity    The entity to test.
		// @param component The component's id.
		// @return `true` when it is present.
		bool HasComponent(Entity entity, ComponentId component) const;

		// Reads a component named at runtime.
		//
		// @param entity    The entity to read.
		// @param component The component's id.
		// @return A pointer to the value, or null when absent.
		const void *GetComponent(Entity entity, ComponentId component) const;

		// Removes a component named at runtime.
		//
		// @param entity    The entity to write to.
		// @param component The component's id.
		void RemoveComponent(Entity entity, ComponentId component);

		// Everything an entity carries, in component-id order.
		//
		// **The question a query cannot ask.** `EachMatching` answers "who has
		// these", and an editor row, a debug view and a script inspecting an
		// entity it did not build all need the other direction - and the only
		// way to get it before this was to test every registered id in turn.
		//
		// The span is the entity's archetype's own id list, so it is valid until
		// the entity's row moves: adding or removing a component on it, or
		// destroying it. That is the same lifetime `Get` hands out and for the
		// same reason.
		//
		// @param entity The entity to inspect.
		// @return Its components, or an empty span for a dead entity or one
		//         carrying nothing.
		// @since v0.12
		std::span<const ComponentId> ComponentsOf(Entity entity) const;

		// --- change tracking -----------------------------------------------

		// Starts recording which entities write to `T`.
		//
		// Off by default and per type, because tracking costs a bit per row and
		// most components have nobody asking. Declare what is observed when the
		// world is built: observing later has to move every entity already
		// carrying the component into an archetype that has somewhere to put
		// the bits, which is correct but is a structural change nobody asked
		// for at a moment nobody expected.
		//
		// Idempotent.
		template <class T> void Observe() {
			RequireOwningThread("Observe");
			ObserveRaw(Components::Of<T>());
		}

		// Reports whether `T` is being tracked.
		//
		// @return `true` when writes to `T` set a bit.
		template <class T> bool Observed() const {
			return ObservedRaw(Components::Of<T>());
		}

		// Reports whether one entity's `T` was written since the last
		// ClearChanges.
		//
		// Always false for a component nobody observes, which is the honest
		// answer rather than an optimistic one: nothing recorded it, so nothing
		// can say.
		//
		// @param entity The entity to ask about.
		// @return `true` when the component changed.
		template <class T> bool Changed(Entity entity) const {
			return ChangedRaw(entity, Components::Of<T>());
		}

		// Starts recording writes to a component named at runtime.
		//
		// The runtime counterpart of `Observe<T>`, for the same reason the
		// component accessors above have one: a layer that resolved a
		// *property* name cannot name the type it projects onto as a template
		// parameter, and a script naming a property is naming the component
		// underneath it.
		//
		// **The polling half, beside `OnChangedComponent`'s signal half.** A
		// script connecting to `.Changed` goes through the signal; a script
		// asking "did this change since I last looked" goes through this and
		// `ChangedComponent`, which costs no connection and no fan-out.
		//
		// Idempotent, and carries `Observe<T>`'s own warning about observing
		// late.
		//
		// @param component The component's id.
		void ObserveComponent(ComponentId component) {
			RequireOwningThread("ObserveComponent");
			ObserveRaw(component);
		}

		// Reports whether a component named at runtime was written since the
		// last ClearChanges.
		//
		// Always false for a component nobody observes, exactly as `Changed<T>`
		// is.
		//
		// @param entity    The entity to ask about.
		// @param component The component's id.
		// @return `true` when the component changed.
		bool ChangedComponent(Entity entity, ComponentId component) const {
			return ChangedRaw(entity, component);
		}

		// Records that every entity carrying `T` has just been written.
		//
		// **For a system that swept the world through `Each` and wrote through
		// the reference it was handed.** Dirty bits are set by `Set` and by
		// nothing else, because a mutable reference handed out by an iteration
		// is a pointer the store never sees written through - so an integrator
		// that does `position.Value = position.Value + ...` marks nothing, and a
		// replication delta built from the bits carries none of it. That is the
		// fast path the whole storage layout exists to make fast, so "write
		// through `Set` instead" is the wrong answer.
		//
		// The claim it makes is the honest one for that case and not for
		// others: *everything holding this component changed.* A system that
		// wrote some rows and not others and calls this has over-reported, and
		// over-reporting costs bandwidth rather than correctness - a delta
		// carrying an unchanged value writes the same value at the other end.
		// Under-reporting is the failure that cannot be recovered from, which is
		// why this is the primitive rather than a per-row one nobody would call
		// in a hot loop.
		//
		// Does nothing for a component nobody observes, and nothing to a table
		// with no bits.
		// Records that one row's component changed, without writing it.
		//
		// **For the writer that cannot report its own writes**, and there is
		// exactly one shape of those: a parallel pass. `Store::EachParallel`
		// hands out raw references from worker threads, so it cannot mark
		// anything - marking is a write to a shared bitset and that is the one
		// thing a parallel body may not do. `physics::IntegrateMotion` is the
		// pass in question and its comment has said since v0.4 that a consumer
		// needing a delta out of an integrated world "marks it in its own
		// publish step". This is what that sentence needed and did not have.
		//
		// **Not `MarkAllChanged`, and the difference is the whole point.** That
		// one claims every row carrying the component moved, including anchored
		// geometry - which defeats `physics::SyncBroadphase`'s outer gate and
		// rebuilds the static index every tick, for ever. This claims one row.
		//
		// `GetMutable` would also mark it, and calling that for the side effect
		// is what this replaces: a discarded pointer reads as a mistake and gets
		// tidied away by somebody who cannot see what it was for.
		//
		// A no-op for a dead entity, for a component the row does not carry, and
		// for a component nobody is observing.
		//
		// @param entity The row.
		// @since v0.14
		template <class T> void MarkChanged(Entity entity) {
			RequireOwningThread("MarkChanged");
			MarkChangedRaw(entity, Components::Of<T>());
		}

		// `MarkChanged<T>`, for a component named at run time.
		//
		// @param entity    The row.
		// @param component The component.
		void MarkChangedRaw(Entity entity, ComponentId component);

		// Records that every entity carrying `T` has just been written.
		//
		// **The whole column, and it is the expensive one.** A replica joining
		// wants every row of a component regardless of what moved, which is what
		// this is for; anything that knows which rows it touched wants
		// `MarkChanged` instead, because marking the column makes the next delta
		// carry all of it.
		template <class T> void MarkAllChanged() {
			RequireOwningThread("MarkAllChanged");
			MarkAllChangedRaw(Components::Of<T>());
		}

		// Records that every entity carrying a component named at runtime has
		// just been written.
		//
		// @param component The component's id, from `Components::Find`.
		void MarkAllChangedRaw(ComponentId component);

		// Visits every entity whose `T` was written since the last
		// ClearChanges.
		//
		// This is the shape a replication delta and a render invalidation both
		// want: the changed rows, not every row plus a test.
		//
		// @param body Called as `body(Entity, T &)` for each changed entity.
		template <class T, class Body> void EachChanged(Body &&body) {
			RequireOwningThread("EachChanged");

			const ComponentId id = Components::Of<T>();
			const ComponentId terms[] = {id, Components::Of<DirtyBits>()};
			const DeferScope defer(*this);

			VisitChanged(terms, id, [&](Entity entity, void *value) {
				body(entity, *static_cast<T *>(value));
			});
		}

		// Visits every *run* of adjacent changed rows, as arrays.
		//
		// The shape a replication delta wants. `EachChanged` hands over one row
		// at a time, which is right for a signal and wrong for a delta: rows
		// that changed together are usually adjacent - a system walks a table in
		// order and writes as it goes - so a delta over runs is a memcpy per
		// run instead of a copy per entity.
		//
		// A run never crosses a table, so the entities and values in one call
		// are contiguous in storage and may be copied as blocks.
		//
		// @param body Called as `body(const Entity *, T *, size_t rows)` for
		//             each run, with `rows` always non-zero.
		template <class T, class Body> void EachChangedBatch(Body &&body) {
			RequireOwningThread("EachChangedBatch");

			const ComponentId id = Components::Of<T>();
			const ComponentId terms[] = {id, Components::Of<DirtyBits>()};
			const DeferScope defer(*this);

			VisitChangedRuns(terms, id, [&body](const Entity *entities, void *values, size_t rows) {
				body(entities, static_cast<T *>(values), rows);
			});
		}

		// `EachChangedBatch`, for a component named at runtime.
		//
		// The shape replication needs. It resolves component *names* at startup
		// - an id means something else in the process it is talking to - so it
		// cannot name a type at compile time, and templating it on one would
		// mean the set of replicated components had to be a template parameter
		// list rather than a table read from a game file.
		//
		// The values are raw. A caller writes them through the component's own
		// `TypeDescriptor`, which is what makes a type with a custom serialiser
		// cross correctly rather than as its object representation.
		//
		// @param component The component to walk.
		// @param body      Called as `body(const Entity *, void *, size_t rows)`
		//                  for each run, with `rows` always non-zero.
		void EachChangedRuns(
			ComponentId component, const std::function<void(const Entity *, void *, size_t)> &body
		);

		// --- change signals --------------------------------------------------

		// A registered change signal, so it can be taken back.
		//
		// A number rather than a callable, because two `std::function`s are not
		// comparable and a disconnect has to name exactly one connection.
		//
		// @since v0.2
		struct Connection {
			// Zero for a connection that was never made.
			uint64_t Id = 0;

			// Whether this names a connection.
			//
			// @return `true` when it came from `OnChanged`.
			bool Valid() const {
				return Id != 0;
			}
		};

		// Calls `body` at the next phase boundary for every entity whose `T`
		// was written.
		//
		// **Not at the moment of assignment.** A callback that ran inside a
		// batch would let a script mutate the world in the middle of a loop
		// over it, and a tick would stop being one thing that starts and
		// finishes. It also means a property written three times in one tick
		// signals once, with the value it ended up at, rather than three times
		// with two values nobody will ever see.
		//
		// Observes `T` as a side effect, because a signal on a type nothing
		// records would never fire and the silence would look like a bug in
		// the listener. That carries `Observe`'s own warning: do it when the
		// world is built rather than later.
		//
		// @param body Called as `body(Store &, Entity, const T &)`.
		// @return The connection, for `Disconnect`.
		template <class T, class Body> Connection OnChanged(Body &&body) {
			RequireOwningThread("OnChanged");
			Observe<T>();

			return Listen(
				Components::Of<T>(),
				[body = std::forward<Body>(body)](Store &store, Entity entity, const void *value) {
					body(store, entity, *static_cast<const T *>(value));
				}
			);
		}

		// Calls `body` at the next phase boundary for every entity whose
		// component - named at runtime - was written.
		//
		// The runtime counterpart of `OnChanged<T>`, and `script`'s `.Changed`
		// is the caller. A property is a projection of one or more components,
		// so a listener that wants "did this instance's `Position` move" has to
		// subscribe to whatever `PropertyDescriptor::Reads` names - which is a
		// `ComponentId` read out of a descriptor, not a type it can write down.
		//
		// The value arrives as raw bytes for the same reason: the subscriber
		// resolved a name and has no type to cast to. A caller that does know
		// the type should use `OnChanged<T>`.
		//
		// Observes the component as a side effect, exactly as `OnChanged<T>`
		// does, and carries the same warning about observing late.
		//
		// @param component The component's id.
		// @param body      Called as `body(Store &, Entity, const void *)`.
		// @return The connection, for `Disconnect`.
		Connection
		OnChangedComponent(ComponentId component, std::function<void(Store &, Entity, const void *)> body) {
			RequireOwningThread("OnChangedComponent");
			ObserveRaw(component);
			return Listen(component, std::move(body));
		}

		// Takes back a change signal.
		//
		// @param connection The connection to drop.
		// @return `false` when it was already dropped or never made.
		bool Disconnect(Connection connection);

		// How many change signals are registered.
		//
		// @return The listener count.
		size_t Listeners() const;

		// Fires every change signal for what has been written since the last
		// clear.
		//
		// Called by `World::Tick` after the simulation phases, which is the
		// phase boundary the signals are named for. A `Store` driven by hand
		// calls it wherever its own boundary is.
		//
		// **Re-entrant calls do nothing.** A listener that writes has made a
		// change belonging to the *next* boundary; firing it inside this one
		// would be the mid-batch dispatch the whole design avoids, and a
		// listener that writes what it listens to would never stop.
		//
		// @return The number of listener calls made.
		size_t FlushSignals();

		// Clears every recorded change.
		//
		// Called at a phase boundary rather than after each read, because a
		// property written three times in one tick should signal once and every
		// consumer should see the same set. A memset per observed table.
		void ClearChanges();

		// How many writes to observed components have been recorded.
		//
		// The coarse signal, and the one a batch write still moves: a consumer
		// that only needs "did anything change at all" can compare this instead
		// of walking rows.
		//
		// @return A counter that only increases.
		uint64_t ChangeVersion() const;

		// --- snapshots -----------------------------------------------------

		// Writes the whole world: entities, tables, resources, names, clock.
		//
		// **Component types are recorded by name**, never by id, so a snapshot
		// written by one process restores into another that assigned different
		// ids - which is the entire point, since the consumers are a restart
		// after a crash, a world moving between host processes, and a recording
		// replayed by a later build.
		//
		// The entity directory is reproduced exactly, index and generation
		// alike. A component may hold an `Entity` - a parent, a target, an
		// owner - and those handles are only still valid if the directory comes
		// back unchanged rather than being re-allocated in order.
		//
		// @param writer The writer to append the snapshot to.
		// @return `false` when the world holds a component with no
		//         serialisation, which is refused rather than written as bytes
		//         that cannot be read back.
		bool Save(core::ByteWriter &writer) const;

		// Replaces this world's entire contents with a snapshot.
		//
		// On any failure the store is left **empty** rather than half-restored,
		// because a world that is partly one snapshot and partly another is
		// worse than no world at all - it looks like it works.
		//
		// @param reader The reader to consume.
		// @return `false` on a corrupt, truncated, or wrong-version snapshot,
		//         or when it names a component this build does not have.
		bool Load(core::ByteReader &reader);

		// Applies a snapshot to a world that is already running.
		//
		// **The capability the replication seam exists to reserve.** `Load`
		// replaces a world; this one merges into it. A client holding a replica
		// receives authoritative state every tick and has to reconcile against
		// what it already has - same entity, new values, no destroy-and-recreate
		// - because destroying and recreating would reset everything the client
		// predicted and make every correction a visible pop.
		//
		// Cheap to allow for now and expensive to retrofit, which is why it is
		// here before the layer that needs it. `v02v03.md` §2.12.
		//
		// Entity handles are matched by index *and* generation, so an entity the
		// sender destroyed and recreated is a different entity here too rather
		// than the old one wearing new values.
		//
		// **A locally predicted entity survives `Authoritative` mode.** "The
		// sender did not mention it" is the definition of a prediction - the
		// authority allocates nothing from the predicted range and so cannot
		// mention one - and destroying them here would delete every prediction
		// on the first correction. Retiring a prediction is `Promote`, or a
		// destroy the predicting layer makes on purpose.
		//
		// On failure the live world is **left as it was** - not cleared, and not
		// half-merged. A replica that lost its world to a corrupt packet would
		// be worse off than one that ignored it.
		//
		// @param reader The snapshot to apply.
		// @param mode   What to do with entities the snapshot does not mention.
		// @return `false` when the snapshot could not be read.
		bool Apply(core::ByteReader &reader, ApplyMode mode);

		// Empties the world: every entity, table, resource and name.
		//
		// The clock is reset and re-created, because a world with no clock is
		// one where every system has to check.
		void Clear();

		// The snapshot format this build writes and accepts.
		//
		// A reader refuses anything else outright. A format that tries to be
		// tolerant of versions it has never seen is one that restores a world
		// nobody can reason about.
		//
		// **2 - the entity directory is two runs rather than one.** The
		// directory is written as a run of `Capacity()` entries, and the index
		// space now has two regions with 2³¹ indices between them, so a version
		// 1 reader handed a version 2 stream would read the predicted run's
		// generations as authoritative slots and silently produce a world with
		// entities nothing named. Bumped rather than sniffed, because the two
		// layouts are indistinguishable from the bytes alone.
		//
		// **3 - `ecs.InstanceClass` is written as the class's name.** It was the
		// registration index, which is `AGENTS.md` rule 4 in a file: nothing
		// restores the class table the way `Load` restores the directory, so a
		// world reloaded by a build that registered its classes in another order
		// came back with every instance the wrong class. The bump is here rather
		// than left to the reader because a length prefix read as a `ClassId`
		// does not fail on that field - it consumes the bytes of the values
		// behind it, and the load dies somewhere unrelated with nothing naming
		// the cause.
		//
		// A component's serialiser is not the envelope this version usually
		// describes, and that is not a reason to leave it. `ecs.InstanceName`
		// went the same way at v0.8 without one, and every world saved before it
		// loaded with its names garbled rather than being refused.
		//
		// **4 - a script's source path is written as its text.**
		// `script.LuaSourceContainer` and its JavaScript twin hold a
		// `core::Name` and had no writer of their own, so raw serialisation put
		// an *interning index* in the file - the exact hazard
		// `DescribeType`'s warning names. A world written by one process and
		// read by another resolved every script's path to whatever that process
		// happened to have interned in that slot, which is a game file that
		// loads cleanly and runs the wrong programs. Same shape as 3, one type
		// along, and refused for the same reason: a length prefix read as a
		// four-byte id consumes the values behind it.
		static constexpr uint32_t SNAPSHOT_VERSION = 4;

		// The number of tables this world holds.
		//
		// A diagnostic: a world with thousands of tables is a world whose
		// components are fragmenting its storage, and that shows up as
		// iteration that will not parallelise.
		//
		// @return The archetype count.
		size_t TableCount() const;

		// Bytes this world's row storage is holding, live rows or not.
		//
		// **The number the chunked-storage item is about.** A column never gives
		// capacity back, so a world that peaked at ten thousand entities and
		// settled at a hundred still holds the peak - invisible with one world
		// and the entire footprint with a thousand of them in one host. A
		// diagnostic: nothing acts on it at runtime, and it is here so that a fee
		// is pinned by a test rather than described in a comment, exactly as
		// `SparseSet::ResidentSlots` is.
		//
		// Covers the columns, the per-table entity id arrays and the entity
		// directory's pages. Excludes resources, names and query plans, which do
		// not grow with the population.
		//
		// @return The resident bytes.
		size_t ResidentStorageBytes() const;

	  private:
		// One table's rows, resolved for one term list.
		//
		// **One slice per table, never one per chunk**, and that is load-bearing
		// rather than incidental: `VisitBatchParallel` decides whether to wake
		// the pool from `slice.Rows`, and `Jobs::For` refuses anything below
		// `MINIMUM_GRAINS` grains. A slice that was one chunk would put every
		// dispatch under the floor, and a 500k parallel iteration would quietly
		// become a serial one - a measured 3.5x, lost with nothing failing. The
		// chunk division happens *inside* the visitors and inside the worker
		// body, where it cannot reach the dispatch decision.
		struct TableSlice {
			size_t Rows = 0;
			const Entity *Entities = nullptr;

			// One chunk directory per term, in the order the caller named them:
			// `Columns[term][Column::ChunkOf(row)]` is the base of the chunk that
			// row falls in. Null for a component with no data, which is why the
			// row accessors below resolve an empty type without indexing it.
			void *const *const *Columns = nullptr;
		};

		// Collects structural changes for the length of a scope.
		struct DeferScope {
			Store &Owner;

			explicit DeferScope(Store &owner) : Owner(owner) {
				Owner.BeginDefer();
			}
			~DeferScope() {
				Owner.EndDefer();
			}
		};

		// One row of a run, given the run's base pointer.
		//
		// A component with no data has no bytes to point at, so every instance
		// of it is the same instance - which is exactly true, since an empty
		// type has no state to tell two of them apart. Handing back a shared
		// object keeps a tag usable as a query term without giving a column of
		// nothing a pointer nobody could dereference.
		template <class T> static T &RowAt(T *base, size_t offset) {
			if constexpr (std::is_empty_v<std::remove_const_t<T>>) {
				return *base;
			} else {
				return base[offset];
			}
		}

		// The rows of one chunk, one call to `body` each.
		//
		// **The bases are parameters, not looked up in the loop.** Resolving
		// `chunks[chunk]` per row per term costs two extra loads on the hottest
		// path in the engine, and the compiler cannot hoist them because the
		// body may alias the directory - measured at **+92% on `Each` over 10k
		// rows** before the lookup was pulled out here.
		template <class... Ts, class Body>
		static void RunRows(const Entity *entities, size_t rows, Body &body, Ts *...bases) {
			for (size_t row = 0; row < rows; row++) {
				body(entities[row], RowAt<Ts>(bases, row)...);
			}
		}

		// The first row of a run, as a pointer the batch paths hand out.
		//
		// The shared instance again for an empty type, and **not** shifted by
		// the offset: a body handed a tag pointer has one object and no array,
		// and `&shared + offset` is arithmetic past the end of an object even
		// when nobody dereferences it.
		template <class T> static T *RunBase(void *const *chunks, size_t chunk, size_t offset) {
			if constexpr (std::is_empty_v<std::remove_const_t<T>>) {
				static std::remove_const_t<T> shared;
				return &shared;
			} else {
				return static_cast<T *>(chunks[chunk]) + offset;
			}
		}

		// The end of the chunk `row` falls in, clipped to `rows`.
		//
		// Every visitor below walks by chunk rather than by row so that the
		// chunk base is hoisted out of the inner loop: resolving the directory
		// per row per term would be paid on the hottest path in the engine to
		// recompute an address that only changes at a boundary.
		static size_t ChunkEnd(size_t row, size_t rows) {
			const size_t boundary = Column::ChunkLimit(row);
			return boundary < rows ? boundary : rows;
		}

		template <class... Ts, class Body, size_t... Indices>
		void VisitRows(const TableSlice &slice, Body &body, std::index_sequence<Indices...>) {
			size_t row = 0;
			while (row < slice.Rows) {
				const size_t chunk = Column::ChunkOf(row);
				const size_t offset = row - Column::ChunkStart(chunk);
				const size_t end = ChunkEnd(row, slice.Rows);
				RunRows<Ts...>(
					slice.Entities + row,
					end - row,
					body,
					RunBase<Ts>(slice.Columns[Indices], chunk, offset)...
				);
				row = end;
			}
		}

		template <class... Ts, class Body, size_t... Indices>
		void VisitBatch(const TableSlice &slice, Body &body, std::index_sequence<Indices...>) {
			// One call per chunk. `EachBatch` promises nothing about where a
			// batch ends, and the constraint the other way - that rows inside
			// one batch are adjacent - is exactly what a chunk boundary is.
			size_t row = 0;
			while (row < slice.Rows) {
				const size_t chunk = Column::ChunkOf(row);
				const size_t offset = row - Column::ChunkStart(chunk);
				const size_t end = ChunkEnd(row, slice.Rows);
				body(end - row, RunBase<Ts>(slice.Columns[Indices], chunk, offset)...);
				row = end;
			}
		}

		template <class... Ts, class Body, size_t... Indices>
		size_t VisitBatchParallel(
			const TableSlice &slice, Body &body, size_t grain, size_t base, std::index_sequence<Indices...>
		) {
			if (slice.Rows == 0) {
				return 0;
			}

			// The whole table in one dispatch, so the pool sees the row count it
			// has to weigh the handover against. The chunk split is below,
			// inside the worker.
			parallel::Jobs::For(slice.Rows, grain, [&](size_t begin, size_t end) {
				size_t row = begin;
				while (row < end) {
					const size_t chunk = Column::ChunkOf(row);
					const size_t offset = row - Column::ChunkStart(chunk);
					const size_t stop = ChunkEnd(row, end);
					body(base + row, stop - row, RunBase<Ts>(slice.Columns[Indices], chunk, offset)...);
					row = stop;
				}
			});

			return slice.Rows;
		}

		template <class... Ts, class Body, size_t... Indices>
		void VisitRowsParallel(
			const TableSlice &slice, Body &body, size_t grain, std::index_sequence<Indices...>
		) {
			if (slice.Rows == 0) {
				return;
			}

			parallel::Jobs::For(slice.Rows, grain, [&](size_t begin, size_t end) {
				size_t row = begin;
				while (row < end) {
					const size_t chunk = Column::ChunkOf(row);
					const size_t offset = row - Column::ChunkStart(chunk);
					const size_t stop = ChunkEnd(row, end);
					RunRows<Ts...>(
						slice.Entities + row,
						stop - row,
						body,
						RunBase<Ts>(slice.Columns[Indices], chunk, offset)...
					);
					row = stop;
				}
			});
		}

		// --- the non-template half -----------------------------------------
		//
		// Every template above narrows to one of these. Keeping the storage out
		// of the header is what lets the layout change without recompiling
		// every system, and what keeps flecs-shaped debt from reappearing.

		void SetRaw(Entity entity, ComponentId id, const void *value);
		bool HasRaw(Entity entity, ComponentId id) const;
		const void *GetRaw(Entity entity, ComponentId id) const;
		void *GetRawMutable(Entity entity, ComponentId id);
		void RemoveRaw(Entity entity, ComponentId id);

		void SetResourceRaw(ComponentId id, const void *value);
		const void *GetResourceRaw(ComponentId id) const;
		void *GetResourceRawMutable(ComponentId id);
		void RemoveResourceRaw(ComponentId id);

		void
		VisitTables(std::span<const ComponentId> terms, const std::function<void(const TableSlice &)> &body);
		void VisitTables(const QueryTerms &terms, const std::function<void(const TableSlice &)> &body);
		size_t CountRows(std::span<const ComponentId> terms);
		size_t CountRows(const QueryTerms &terms);

		// Reports a query naming more filter terms than `MAX_FILTER_TERMS`, and
		// does not return.
		[[noreturn]] void TooManyFilterTerms(const char *what) const;

		void ObserveRaw(ComponentId id);
		bool ObservedRaw(ComponentId id) const;
		Connection Listen(ComponentId id, std::function<void(Store &, Entity, const void *)> body);
		bool ChangedRaw(Entity entity, ComponentId id) const;
		void VisitChanged(
			std::span<const ComponentId> terms,
			ComponentId subject,
			const std::function<void(Entity, void *)> &body
		);

		void VisitChangedRuns(
			std::span<const ComponentId> terms,
			ComponentId subject,
			const std::function<void(const Entity *, void *, size_t)> &body
		);

		// Where `subject` sits in a slice's table, which is also its dirty bit.
		size_t SubjectPosition(const TableSlice &slice, ComponentId subject) const;

		void BeginDefer();
		void EndDefer();

		// Whether this world may mint from the authoritative range, complaining
		// once when it may not.
		//
		// One place rather than three: `Create`, `CreateInstance` and
		// `CloneInstance` all mint the same kind of index, and the version of
		// this that lived inside `Create` alone is why `CreateInstance` walked
		// past the rule for a whole version.
		bool MayMintAuthoritative(const char *what);

		Entity CloneInstanceInRange(Entity source, bool predicted);

		bool SetPropertyValue(
			Entity instance,
			const PropertyDescriptor &descriptor,
			const void *value,
			size_t bytes,
			bool authored
		);

		// The body of both named-create paths.
		//
		// A `bool` rather than the directory's own range type, so this header
		// stays clear of the storage layout it deliberately does not expose.
		Entity MintNamed(std::string_view name, bool predicted);

		void RequireOwningThread(const char *what) const;

		std::unique_ptr<StoreState> State;
		std::string StoreName;
		std::atomic<std::thread::id> Owner;
	};
}
