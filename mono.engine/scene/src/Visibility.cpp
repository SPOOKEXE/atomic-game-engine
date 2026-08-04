#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Visibility.hpp>

#include <vector>

namespace engine::scene {

	namespace {
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::Store;

		// The walk's stack, kept between calls so a steady scene allocates
		// nothing.
		//
		// **Scratch and only scratch**: it is cleared on entry and carries no
		// meaning across calls, so reusing it cannot make one tick depend on the
		// last. `thread_local` rather than a member because a world is ticked by
		// whichever worker claimed it — two worlds syncing at once must not share
		// one buffer, and which worker runs which world is not this function's to
		// know.
		std::vector<Entity> &Stack() {
			static thread_local std::vector<Entity> stack;
			return stack;
		}
	}

	size_t SyncRendered(Store &store) {
		ENGINE_PROFILE_CAT("sync rendered", engine::core::ProfileCategory::ECS);

		const Entity workspace = WorkspaceOf(store);

		// --- the walk: mark every visible descendant of Workspace ------------
		//
		// Depth first over an explicit stack. Recursion would put the depth of
		// the scene onto the C stack, and a scene's depth is the author's to
		// choose — the same reason `InstanceGetDescendants` spells its walk out
		// in the bindings.
		//
		// A world with no Workspace marks nothing and the sweep below then
		// clears everything, which is the empty screen `Visibility.hpp` argues
		// for rather than a scene that draws its own storage.
		if (workspace != NULL_ENTITY) {
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

				// **The Workspace itself is not drawn.** It is a service — a
				// container with a place in the tree and nothing to render —
				// and it carries no `Visual` anyway. Skipped explicitly because
				// "is a descendant of" is the rule, and a thing is not a
				// descendant of itself.
				if (current == workspace) {
					continue;
				}

				// Not every descendant is drawable, and that is ordinary rather
				// than exceptional: a `Folder`, a `Script`, a `Camera` and a
				// model's own node all sit in `Workspace` and none of them has a
				// `Visual`. They are walked *through* — their children may well
				// be parts — and not marked.
				const Visual *visual = store.Get<Visual>(current);
				if (visual == nullptr || !visual->Visible) {
					continue;
				}

				if (Rendered *mark = store.GetMutable<Rendered>(current); mark != nullptr) {
					mark->Mark = 1;
				} else {
					store.Set(current, Rendered{1, {}});
				}
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

		return rendered;
	}
}
