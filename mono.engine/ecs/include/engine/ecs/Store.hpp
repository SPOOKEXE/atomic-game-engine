#pragma once

// L3 · the storage for one world.
//
// One Store is one world's data — every entity, every component, and every
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
// Thread affinity is checked rather than trusted. A store belongs to the
// thread that bound it, every mutation aborts unless it is on that thread, and
// the check is on in every build — a data race that only shows up under load on
// a player's machine costs far more than a branch.
//
// @tier L3 · shared

#include <engine/core/Log.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Time.hpp>
#include <engine/parallel/Jobs.hpp>

#include <flecs.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace engine::ecs {

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
		void BindToCallingThread();

		// Reports whether the caller is the thread that currently owns the store.
		//
		// @return `true` when the current thread may mutate this Store.
		bool IsOnOwningThread() const {
			return std::this_thread::get_id() == Owner;
		}

		// --- entities ------------------------------------------------------

		// Creates an unnamed entity owned by this store.
		//
		// @return A live entity handle.
		Entity Create();

		// Creates a named entity owned by this store.
		//
		// @param name The backing store name to copy onto the entity.
		// @return A live entity handle.
		Entity Create(std::string_view name);

		// Destroys an entity and all components attached to it.
		//
		// @param entity The entity generation to destroy.
		void Destroy(Entity entity);

		// Reports whether an entity generation is currently alive in this store.
		//
		// @param entity The handle to inspect.
		// @return `false` for NULL_ENTITY, destroyed entities, and stale generations.
		bool Alive(Entity entity) const;

		// There is no Count() of everything, and that is deliberate rather than
		// missing. The backing store's entity space also holds every component
		// registration, tag and builtin module, so a total is a number nobody
		// can act on — and the one that used to be here silently returned zero
		// for the whole of v0.1 because nothing ever called it.
		//
		// Count what you can name: `CountMatching<Ts...>()`.

		// --- components ----------------------------------------------------

		// Adds or replaces one component value on an entity.
		//
		// @param entity The entity that owns the component.
		// @param value  The component value to copy into the store.
		template <class T>
		void Set(Entity entity, const T &value) {
			RequireOwningThread("Set");
			Native().entity(entity.Id).set<T>(value);
		}

		// Reports whether an entity carries a component of the requested type.
		//
		// @param entity The entity to inspect.
		// @return `true` when the component is present.
		template <class T>
		bool Has(Entity entity) const {
			return NativeConst().entity(entity.Id).has<T>();
		}

		// Null when absent. Callers check; there is no Get-or-default, because
		// a default-constructed component silently standing in for a missing
		// one is a bug that reads as working code.
		//
		// @param entity The entity whose component is requested.
		// @return A read-only component pointer, or `nullptr` when absent.
		template <class T>
		const T *Get(Entity entity) const {
			return NativeConst().entity(entity.Id).try_get<T>();
		}

		// Returns mutable access to one component without adding it when absent.
		//
		// @param entity The entity whose component is requested.
		// @return A mutable component pointer, or `nullptr` when absent.
		template <class T>
		T *GetMutable(Entity entity) {
			RequireOwningThread("GetMutable");
			return Native().entity(entity.Id).try_get_mut<T>();
		}

		// Removes one component type without destroying the entity.
		//
		// @param entity The entity from which to remove the component.
		template <class T>
		void Remove(Entity entity) {
			RequireOwningThread("Remove");
			Native().entity(entity.Id).remove<T>();
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
		// Resources live on an entity that is disabled, so no query reaches
		// them. A type used as both a component and a resource therefore does
		// not silently gain a row in `Each<T>`; `tests/Resources.cpp` asserts
		// that rather than trusting it.
		//
		// WorldTime is reserved for the dedicated clock API. Do not set, mutate,
		// or remove it through these generic methods; Time() assumes the resource
		// created by the constructor is still present.

		// Adds or replaces one world-scoped resource.
		//
		// This invalidates every pointer previously returned by Resource or
		// ResourceMutable, regardless of the resource type being set.
		//
		// @param value The resource value to copy into the store.
		template <class T>
		void SetResource(const T &value) {
			RequireOwningThread("SetResource");
			Native().entity(ResourceHolder).set<T>(value);
		}

		// Null when unset, for the same reason Get is: a default-constructed
		// resource standing in for one nobody set is a bug that reads as
		// working code.
		//
		// The returned pointer is invalidated by SetResource or RemoveResource
		// for any resource type, because either operation may move the holder.
		//
		// @return A read-only resource pointer, or `nullptr` when unset.
		template <class T>
		const T *Resource() const {
			return NativeConst().entity(ResourceHolder).try_get<T>();
		}

		// Invalidated by SetResource and RemoveResource of *any* type — adding
		// a resource moves the holder between tables and everything on it with
		// it. Resources are declared when a world is built, so in practice this
		// means: do not hold one across world construction.
		//
		// @return A mutable resource pointer, or `nullptr` when unset.
		template <class T>
		T *ResourceMutable() {
			RequireOwningThread("ResourceMutable");
			return Native().entity(ResourceHolder).try_get_mut<T>();
		}

		// Reports whether the world contains a resource of the requested type.
		//
		// @return `true` when the resource is present.
		template <class T>
		bool HasResource() const {
			return NativeConst().entity(ResourceHolder).has<T>();
		}

		// Removes one world-scoped resource when present.
		//
		// This invalidates every pointer previously returned by Resource or
		// ResourceMutable, regardless of the resource type being removed.
		template <class T>
		void RemoveResource() {
			RequireOwningThread("RemoveResource");
			Native().entity(ResourceHolder).remove<T>();
		}

		// --- time ----------------------------------------------------------
		//
		// The clock is created as a resource and has dedicated read/write methods.
		// Callers must not reach it through the generic resource API above: Time()
		// assumes it remains present, and systems receive a copy to read.

		// By value, not by reference. It is 32 bytes, read once per system, and
		// returning a copy makes two hazards impossible at once — a reference
		// left dangling by a later SetResource, and a system quietly writing to
		// the clock instead of reading it.
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
		// Structural changes during iteration — Create, Destroy, adding a
		// component — are deferred until the loop ends. They are therefore
		// safe, and not visible to the loop that made them.
		//
		// The deferred scope is this wrapper's doing, not the backing store's:
		// iterating from outside a registered system locks the table, and a
		// Destroy inside the loop would abort on that lock. Making it safe here
		// is the whole reason this layer exists rather than the raw API being
		// passed around. Writes *through* a component reference are direct
		// memory writes and are unaffected.
		//
		// @param body Called as `body(Entity, Ts &...)` for every matching entity.
		template <class... Ts, class Body>
		void Each(Body &&body) {
			RequireOwningThread("Each");

			Native().defer_begin();
			Native().each([&](flecs::entity entity, Ts &...components) {
				body(Entity { entity.id() }, components...);
			});
			Native().defer_end();
		}

		// Each, spread across the job system's workers.
		//
		// Parallel *within* a tick, not asynchronous across ticks: this blocks
		// until every entity has been visited, so the tick is still one thing
		// that starts and finishes. That is what keeps a recorded run
		// replayable — a result that lands a tick later on a slower machine
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
		// A scene of one archetype — the common case — parallelises completely;
		// a scene of a thousand tiny archetypes parallelises hardly at all, and
		// that is a storage-layout problem rather than something more threads
		// would fix.
		//
		// **This is slower than Each below a crossover, and the crossover is
		// higher than it looks.** Measured on a 24-core machine over an
		// integration step of three float multiply-adds per row:
		//
		//     entities     Each      EachParallel     
		//        20 000    0.050 ms      0.102 ms   2.0x slower
		//       100 000    0.259 ms      0.195 ms   1.3x faster
		//       500 000    1.204 ms      0.347 ms   3.5x faster
		//     2 000 000    5.925 ms      1.704 ms   3.5x faster
		//
		// So for a cheap body the crossover is somewhere near 60-80k rows, and
		// the ceiling is about 3.5x rather than the core count — past that it is
		// memory bandwidth, not threads. An expensive body crosses over far
		// sooner. Neither number is knowable in advance, which is why the answer
		// is to measure the system you are writing rather than to reach for this
		// by default.
		//
		// @param body  Called concurrently as `body(Entity, Ts &...)` for each match.
		// @param grain The minimum table-row range worth handing to a worker.
		// @tick
		template <class... Ts, class Body>
		void EachParallel(Body &&body, size_t grain = parallel::Jobs::DEFAULT_GRAIN) {
			RequireOwningThread("EachParallel");

			// Not deferred, unlike Each. Deferring exists to make a structural
			// change inside the loop safe, and structural changes are not
			// allowed here at all.
			Native().template query<Ts...>().run([&](flecs::iter &iterator) {
				while (iterator.next()) {
					VisitTableInParallel<Ts...>(
						iterator, body, grain, std::index_sequence_for<Ts...> {});
				}
			});
		}

		// How many entities Each would visit.
		//
		// The query is built once per type list per store and kept, so a system
		// may call this every tick. It used to build one per call, which made a
		// system that published an entity count the most expensive thing in the
		// tick and skewed every measurement taken while it was there — so the
		// count got captured at build time instead, and the world grew a second
		// copy of a fact it already held. Caching the query is what removes the
		// reason to keep that copy.
		//
		// The cached query is a live view, not a snapshot: entities created
		// after the first call are counted by the next one.
		//
		// @return The live number of entities carrying every requested component.
		template <class... Ts>
		size_t CountMatching() {
			static_assert(sizeof...(Ts) > 0, "CountMatching needs at least one component.");
			RequireOwningThread("CountMatching");

			return CachedCount(std::type_index(typeid(std::tuple<Ts...>)), [this] {
				// Untyped, because counting needs the terms and not the column
				// pointers, and one concrete query type keeps the cache a plain
				// map rather than a type-erasure exercise.
				auto builder = World.query_builder<>();
				(builder.with<std::remove_const_t<Ts>>(), ...);
				return builder.build();
			});
		}

		// The escape hatch. Present because wrapping the whole of flecs is not
		// v0.1's job, and named so that a search finds every place that took
		// the shortcut. Every use here is a line the binding generator will
		// have to account for later.
		//
		// @return Mutable access to the backing flecs world.
		// @warning Do not call this from another module; add the required Store
		//          operation instead.
		flecs::world &Native() {
			return World;
		}

	  private:
		// One table's rows, split across workers.
		//
		// The field objects are captured by value on purpose. Each is a
		// {pointer, count, is_shared} triple, so every worker gets its own
		// copy and the parallel body touches no shared iterator state.
		template <class... Ts, class Body, size_t... Indices>
		void VisitTableInParallel(
			flecs::iter &iterator,
			Body &body,
			size_t grain,
			std::index_sequence<Indices...>
		) {
			const auto count = static_cast<size_t>(iterator.count());
			if (count == 0) {
				return;
			}

			auto entities = iterator.entities();
			auto columns = std::make_tuple(iterator.field<Ts>(static_cast<int8_t>(Indices))...);

			parallel::Jobs::For(count, grain, [&](size_t begin, size_t end) {
				for (size_t row = begin; row < end; row++) {
					body(Entity { entities[row] }, std::get<Indices>(columns)[row]...);
				}
			});
		}

		template <class Build>
		size_t CachedCount(std::type_index key, Build &&build) {
			auto found = CountQueries.find(key);
			if (found == CountQueries.end()) {
				found = CountQueries.emplace(key, build()).first;
			}
			return static_cast<size_t>(found->second.count());
		}

		const flecs::world &NativeConst() const {
			return World;
		}

		void RequireOwningThread(const char *what) const;

		flecs::world World;
		std::string StoreName;
		std::thread::id Owner;

		// Every resource hangs off this one entity, which is disabled so that
		// no query reaches it. Created by the constructor, so `Resource<T>()`
		// on a store nobody has set anything on returns null rather than
		// touching a stale id.
		flecs::entity_t ResourceHolder = 0;

		// One persistent query per component list, keyed by the list's type.
		std::unordered_map<std::type_index, flecs::query<>> CountQueries;
	};
}
