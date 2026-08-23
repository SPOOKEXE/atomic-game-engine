#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>

#include <cstdint>
#include <vector>

namespace engine::scene {

	namespace {
		using ecs::Entity;
		using ecs::Hierarchy;
		using ecs::NULL_ENTITY;
		using ecs::Store;

		// The walk's stack, kept between calls so a steady scene allocates
		// nothing.
		//
		// **Scratch and only scratch**: it is cleared on entry and carries no
		// meaning across calls, so reusing it cannot make one tick depend on the
		// last. `thread_local` rather than a member because a world is ticked by
		// whichever worker claimed it - two worlds syncing at once must not share
		// one buffer, and which worker runs which world is not this function's to
		// know.
		std::vector<Entity> &Stack() {
			static thread_local std::vector<Entity> stack;
			return stack;
		}

		// --- the fold --------------------------------------------------------
		//
		// `gui/Compile.cpp`'s and `studio/Hierarchy.cpp`'s, kept identical on
		// purpose: the same constant, the same finaliser, the same order
		// dependence. Three signatures in one repository that mix differently
		// are three things a reviewer has to hold separately, and none of them
		// is more correct than the others.

		constexpr uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;

		// splitmix64's finaliser: the cheapest mix that avalanches every input
		// bit across all sixty-four output ones.
		//
		// **Not `std::hash`.** The standard says nothing about what it
		// produces, so two builds of this program may disagree. Nothing here
		// crosses a process - the comparison is this frame against the last,
		// in one store - but a hash whose value is a property of the compiler
		// is one nobody can write a test for, and the test is what keeps the
		// field list honest.
		constexpr uint64_t Scramble(uint64_t value) {
			value ^= value >> 30;
			value *= 0xBF58476D1CE4E5B9ull;
			value ^= value >> 27;
			value *= 0x94D049BB133111EBull;
			value ^= value >> 31;
			return value;
		}

		// Folds one term in, order included.
		//
		// **Order-dependent deliberately.** Two archetypes that swap their rows
		// hold the same instances and would walk to the same answer, so an
		// order-independent fold would be the more accurate one - and would
		// also collide far more readily, because commutative folds do. A
		// reshuffle costs one walk nobody sees; a collision is a hidden part
		// still drawing.
		constexpr uint64_t Fold(uint64_t running, uint64_t term) {
			return (running ^ Scramble(term)) * GOLDEN;
		}

		constexpr uint64_t Fold(uint64_t running, Entity value) {
			return Fold(running, value.Id);
		}

		// Everything the walk's answer depends on, in two linear column scans.
		//
		// **No store address folded in, unlike `gui` and `studio`.** Both of
		// those keep their stamp in an object a caller can point at either
		// world, so they have to tell the worlds apart. This one lives on the
		// world it describes, so there is no second world to confuse it with -
		// and an address is precisely the term that differs between two runs of
		// one scene, which is what `just determinism` compares. Folding one in
		// would buy nothing and make the *decision* run-dependent.
		//
		// A collision keeps a stale answer and a spurious change costs one
		// walk, so everything here leans towards folding more rather than less:
		// the `Visual` pass covers rows that are not in `Workspace` at all, and
		// the `Hierarchy` pass covers every instance in the world rather than
		// only the drawable ones. Restricting either would be an ancestry test
		// per row, which is the cost this exists to avoid.
		//
		// @param store     The world.
		// @param workspace What `WorkspaceOf` resolved to, folded because the
		//        walk starts there and a world that gains or loses a
		//        `Workspace` changes every answer at once.
		// @return The fold.
		uint64_t Signature(Store &store, Entity workspace) {
			uint64_t stamp = Fold(uint64_t{0}, workspace);

			// Component epochs are monotonic and are not cleared with per-tick
			// dirty bits. Counts cover removals, which have no surviving row on
			// which to leave a write bit. This turns the steady path from two full
			// column scans into four integer reads without maintaining a second
			// copy of the tree.
			store.Observe<Hierarchy>();
			store.Observe<Visual>();
			stamp = Fold(stamp, store.ComponentChangeVersion<Hierarchy>());
			stamp = Fold(stamp, store.CountMatching<Hierarchy>());
			stamp = Fold(stamp, store.ComponentChangeVersion<Visual>());
			stamp = Fold(stamp, store.CountMatching<Visual>());

			return stamp;
		}
	}

	size_t SyncRendered(Store &store) {
		ENGINE_PROFILE_CAT("sync rendered", engine::core::ProfileCategory::ECS);

		Entity workspace = NULL_ENTITY;
		uint64_t stamp = 0;
		{
			ENGINE_PROFILE_CAT("sync rendered.revision", engine::core::ProfileCategory::ECS);
			workspace = WorkspaceOf(store);
			stamp = Signature(store, workspace);
		}

		// --- the early-out: has anything the answer depends on moved? --------
		//
		// On almost every frame this is all that happens. Two linear passes
		// over packed columns, no random lookups and no `std::function`, and
		// what they produce is a number to compare against the last one.
		//
		// **A memo, not a hook.** `scene/AGENTS.md` argues that this tag is
		// derived by a sweep rather than maintained at every reparent, because
		// ancestry is not local and no set of hooks is ever complete. Nothing
		// here maintains anything: the walk below runs in full the moment the
		// tree or a `Visual` moves, and what is skipped is a walk that would
		// have written back exactly the rows already present. The argument
		// survives, which is the thing a reviewer should check first.
		//
		// The resource is created here rather than by `InstallServices`,
		// because this function is the only thing that reads or writes it and
		// a fixture nobody could explain is worse than a lazy one. It is
		// registered explicitly in `RegisterSceneComponents` - an unregistered
		// resource type is minted under the compiler's spelling and
		// `Store::Save` then refuses the world.
		if (!store.HasResource<RenderedSignature>()) {
			store.SetResource(RenderedSignature{});
		}

		RenderedSignature &memo = *store.ResourceMutable<RenderedSignature>();

		if (memo.Fresh != 0 && memo.Stamp == stamp) {
			// **Counted rather than remembered.** The sweep leaves every
			// surviving row with `Mark` at zero, so the number of rows
			// carrying the tag *is* the answer this returns, and
			// `CountMatching` keeps its query per store rather than building
			// one per call. Caching the count in the resource instead would be
			// a second copy of a derived fact, which is the thing `ecs`'s own
			// invariants open by refusing.
			ENGINE_PROFILE_CAT("sync rendered.count", engine::core::ProfileCategory::ECS);
			return store.CountMatching<Rendered>();
		}

		memo.Stamp = stamp;
		memo.Fresh = 1;

		// --- the walk: mark every visible descendant of Workspace ------------
		//
		// Depth first over an explicit stack. Recursion would put the depth of
		// the scene onto the C stack, and a scene's depth is the author's to
		// choose - the same reason `InstanceGetDescendants` spells its walk out
		// in the bindings.
		//
		// A world with no Workspace marks nothing and the sweep below then
		// clears everything, which is the empty screen `Visibility.hpp` argues
		// for rather than a scene that draws its own storage.
		if (workspace != NULL_ENTITY) {
			ENGINE_PROFILE_CAT("sync rendered.walk", engine::core::ProfileCategory::ECS);
			std::vector<Entity> &pending = Stack();
			pending.clear();
			pending.push_back(workspace);

			while (!pending.empty()) {
				const Entity current = pending.back();
				pending.pop_back();

				// Children collected before the row is touched. `Set` below is
				// structural and may move `current`'s row; entity handles
				// survive that and component pointers do not, so nothing read
				// here may outlive it.
				store.EachChild(current, [&pending](Entity child) { pending.push_back(child); });

				// **The Workspace itself is not drawn.** It is a service - a
				// container with a place in the tree and nothing to render -
				// and it carries no `Visual` anyway. Skipped explicitly because
				// "is a descendant of" is the rule, and a thing is not a
				// descendant of itself.
				if (current == workspace) {
					continue;
				}

				// Not every descendant is drawable, and that is ordinary rather
				// than exceptional: a `Folder`, a `Script`, a `Camera` and a
				// model's own node all sit in `Workspace` and none of them has a
				// `Visual`. They are walked *through* - their children may well
				// be parts - and not marked.
				const Visual *visual = store.Get<Visual>(current);
				if (visual == nullptr || !visual->Visible) {
					continue;
				}

				// **`GetUnobserved` and not `GetMutable`, and the difference is
				// not a micro-optimisation.** A mutable pointer handed out by
				// `GetMutable` counts as a write, and a write marks a dirty bit
				// in any table carrying a `DirtyBits` column - which in the
				// studio is every table holding a `Transform`, because that one
				// is watched. So this line used to do `state.Changes++` once per
				// rendered entity per `PreRender` frame, setting a bit nothing
				// reads and permanently falsifying the invariant
				// `physics/SyncBroadphase.cpp` states and depends on: *"an
				// unchanged counter means nothing authored has happened."* Its
				// outer gate never held and two dirty-bit scans ran every tick
				// for nothing. `gui/Compile.hpp` rests on the same counter.
				//
				// Safe here **only** because of the rule in `Visibility.hpp`:
				// nothing outside this function may add, remove or write a
				// `Rendered`, so there is no observer whose change could be
				// lost by not reporting this one. If that rule is ever relaxed,
				// this line goes back to `GetMutable` in the same commit.
				if (Rendered *mark = store.GetUnobserved<Rendered>(current); mark != nullptr) {
					mark->Mark = 1;
					continue;
				}

				// --- it was not drawn last frame ------------------------------
				//
				// **A thing that has just appeared did not come from anywhere,
				// and the draw list has to be told.** `client::BuildDrawList`
				// interpolates `PreviousTransform` towards `Transform`, and a
				// row that has only just arrived carries whatever previous frame
				// it was *created* with - the identity, for anything
				// `Instance.new` made and a script then placed. Drawing that
				// interpolation is a part flying in from the origin for as long
				// as it takes the next tick's `capture-previous` to run: at 300
				// frames against a 60 Hz tick, five frames of it. That is the
				// flash `examples/Slide.luau` shows on every block it spawns.
				//
				// **Seeded here rather than in `PlaceInstance`, which
				// deliberately does not touch it.** An authored write to a part
				// already on screen *is* motion, and clearing the previous frame
				// there turns every scripted animation in the engine into
				// stepped motion at the tick rate - `Part.cpp` carries that
				// refutation and it still stands. The difference is *appearing*
				// versus *moving*, and this branch is the one place that knows
				// which of the two happened.
				//
				// It covers the same fault arriving by three other doors: an
				// entity replicated into a world, one whose `Visible` was turned
				// back on after being moved while hidden, and one reparented
				// into `Workspace` from somewhere outside it. None of them
				// travelled from where they were last drawn, because they were
				// not drawn.
				//
				// **Before the `Set`, because that is structural.** A row may
				// move when a component is added, and a pointer read before it
				// does not survive - the same rule the child walk above obeys.
				//
				// `GetUnobserved` for the reason the `Rendered` write above
				// gives: this is presentation state derived by this pass, and
				// marking a dirty bit for it would falsify "an unchanged counter
				// means nothing authored has happened" for every table holding a
				// `Transform`.
				if (const Transform *placed = store.Get<Transform>(current); placed != nullptr) {
					if (PreviousTransform *previous = store.GetUnobserved<PreviousTransform>(current);
						previous != nullptr) {
						previous->Frame = placed->Frame;
					}
				}

				store.Set(current, Rendered{1, {}});
			}
		}

		// --- the sweep: drop the tag from everything the walk did not mark ----
		//
		// This is what makes the pass correct without hooking every reparent:
		// an entity that left `Workspace`, was hidden, or had its ancestor
		// moved is simply one the walk did not reach, and it loses the tag here
		// whatever the reason was.
		//
		// `Remove` inside `Each` is deferred by the store until the loop ends,
		// so the rows are not moved underneath the iteration that asked for it.
		size_t rendered = 0;
		{
			ENGINE_PROFILE_CAT("sync rendered.sweep", engine::core::ProfileCategory::ECS);
			store.Each<Rendered>([&store, &rendered](Entity entity, Rendered &tag) {
				if (tag.Mark == 0) {
					store.Remove<Rendered>(entity);
					return;
				}

				// Cleared for the next pass, which is what keeps the field zero
				// everywhere a snapshot or a comparison can see it.
				tag.Mark = 0;
				rendered++;
			});
		}

		// **A gauge and not a counter**: this is a level, and how many entities
		// a world is drawing is the first number to ask for when the answer to
		// "why is nothing on screen" is "everything left the walk".
		core::Metrics::SetGauge("scene.rendered", static_cast<double>(rendered));
		ENGINE_TRACE("{} entity/entities are rendered", rendered);
		return rendered;
	}
}
