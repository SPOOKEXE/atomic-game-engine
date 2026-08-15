#pragma once

// The Explorer's tree, compiled from the store and kept until the store moves.
//
// **One archetype scan, not a tree walk.** The tree is `ecs::Hierarchy`, a
// component on every instance, so "every root in this world" and "every
// instance whose name matches" are a linear pass over packed columns. Asking
// the tree the same questions means chasing a handle per node through a
// directory lookup, in whatever order the tree happens to be shaped. That is
// what the hierarchy being an ECS component buys, and the Explorer is the panel
// that gets to spend it.
//
// ## Compiled, and re-compiled only when the world says so
//
// The expensive half is not the scan. It is what comes after: sorting, working
// out which rows a filter leaves standing, and flattening the tree into the
// list of rows the panel actually draws. An editor is a program somebody has
// open all day, and on almost every one of those frames the answer is the same
// answer as last frame.
//
// So the scan computes a **signature** - a rolling hash of every field the
// flatten reads - and when it matches the last one, the compiled rows are still
// correct and are kept. This is `scene::QuickHash`'s pattern one layer up, and
// it is the same fallback for the same reason: `Hierarchy` and `InstanceName`
// are not observed components, so `Store::ChangeVersion` does not move when
// something is reparented or renamed.
//
// **The signature covers exactly what the flatten reads**, and that is the
// whole correctness argument rather than a hopeful one:
//
// | Field | Why it is in the signature |
// |---|---|
// | `Entity` | A destroyed row and a new one may otherwise look identical |
// | `Hierarchy::Parent` | Which rows are roots |
// | `Hierarchy::FirstChild`, `NextSibling` | The order the flatten descends in |
// | `InstanceName` | Drawn on the row, and what the filter matches |
// | `InstanceClass` | Drawn on the row |
// | The filter text, open set and reveal set | Inputs, so they change the rows |
//
// `LastChild` and `PreviousSibling` are absent because nothing here reads them.
// A field added to the flatten has to be added here, and the failure if it is
// not is a row that is one edit stale - so the rule is written down rather than
// left to be remembered.
//
// The direction matters and only one way round is safe. A signature that
// **collides** would keep rows the world has moved on from, which is the stale
// panel `mono.studio/AGENTS.md` forbids; a signature that changes when nothing
// really did costs one rebuild nobody sees. Every choice here leans the second
// way - an archetype that reshuffles its rows without changing a value still
// re-compiles.
//
// ## Why a flat list rather than a recursion
//
// The panel used to recurse, submitting an imgui tree node per row and relying
// on imgui to own what was open. That cannot be clipped: `TreePush` and
// `TreePop` have to run in order, so a thousand-row scene submitted a thousand
// nodes to draw the thirty on screen. A flat list in display order can be
// walked by `ImGuiListClipper`, and it makes two other things fall out for
// free - a shift-click range is index arithmetic, and scrolling to an instance
// is a row number.
//
// The cost is that expansion is ours now rather than imgui's. That is still not
// world state: it is a set of entity ids in the editor, exactly as the
// selection is.

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Instance.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace studio {

	// One drawn row of the tree, in display order.
	//
	// Everything the panel needs to draw a row without going back to the store,
	// which is what makes the compiled list worth keeping at all.
	struct HierarchyRow {
		// The instance this row draws.
		engine::ecs::Entity Instance;

		// Its parent, or NULL_ENTITY for a root.
		engine::ecs::Entity Parent;

		// What it is called, or an invalid name when unnamed.
		engine::core::Name Name;

		// What class it was created as.
		engine::ecs::ClassId Class;

		// The two strings the row draws, resolved once when the row was built.
		//
		// **`core::Name::Text()` takes a process-wide mutex**, and its own
		// header says in as many words that it is the serialisation path rather
		// than a hot one. Drawing a name and a class name through it is two
		// lock acquisitions per row per frame, on a panel that redraws every
		// frame because imgui is immediate mode.
		//
		// Holding the pointer is safe for exactly the reason `Label` documents:
		// the interned storage is a deque that never moves and from which
		// nothing is ever removed, so the view outlives the process. And it is
		// safe to *cache* because the signature folds in the name and the class
		// - a rename changes the stamp, which rebuilds the row, which resolves
		// the text again.
		//@{
		const char *Text = "";
		const char *ClassText = "";
		//@}

		// How far to indent. A root is 0.
		uint16_t Depth = 0;

		// Whether an expander should be drawn.
		//
		// **"Has a child this view would show", not "has a child".** While a
		// filter is narrowing the tree, an arrow that opened onto nothing would
		// be an arrow the author has to try before learning it is empty.
		bool HasChildren = false;

		// Whether this row's children are in the list below it.
		bool Open = false;

		// Whether this row's own name matched the filter.
		//
		// False for an ancestor that is only present because a match sits
		// beneath it, which is what lets the panel dim the difference.
		bool Matched = false;
	};

	// What the panel is asking the tree to look like.
	struct HierarchyRequest {
		// The filter box's contents. Empty shows everything.
		std::string_view Filter;

		// Instances the author has opened, by handle. Order is not read.
		std::span<const engine::ecs::Entity> Open;

		// Instances whose ancestors must be opened whatever else is asked -
		// the selection, when something outside this panel chose it.
		std::span<const engine::ecs::Entity> Reveal;
	};

	// The Explorer's view of one world's tree.
	//
	// One per drawn world, held across frames. Holds handles and names, never a
	// pointer into the store.
	class HierarchyView {
	  public:
		// What `RowOf` answers for an instance the view is not showing.
		static constexpr size_t NO_ROW = static_cast<size_t>(-1);

		// Reads the world, and rebuilds the rows if anything they depend on
		// moved.
		//
		// @param store   The world to read. Must already be entered.
		// @param request What the panel wants shown.
		// @return `true` when the rows were rebuilt, and `false` when the
		//         signature matched and the previous rows were kept.
		bool Rebuild(engine::ecs::Store &store, const HierarchyRequest &request);

		// The rows to draw, in display order.
		//
		// @return The rows, valid until the next `Rebuild` that returns `true`.
		std::span<const HierarchyRow> Rows() const;

		// Where an instance sits in `Rows`.
		//
		// @param instance The instance to find.
		// @return Its row index, or `NO_ROW` when it is not shown.
		size_t RowOf(engine::ecs::Entity instance) const;

		// Whether an instance is anywhere in this world.
		//
		// True even for a row a filter is hiding, because "is this handle still
		// live" and "is it on screen" are different questions and answering
		// them with one value is how a selection quietly disappears.
		//
		// @param instance The instance to test.
		// @return `true` when the last scan saw it.
		bool Holds(engine::ecs::Entity instance) const;

		// Whether one instance sits inside another's subtree.
		//
		// **Answered from the compiled nodes, not from the store**, so it
		// covers rows a filter is hiding as well as rows on screen - and so the
		// panel logic that needs it can be tested without a world open.
		//
		// Reflexive, matching `Store::IsDescendantOf`: an instance is inside
		// its own subtree.
		//
		// @param instance The instance to test.
		// @param ancestor The subtree root to test against.
		// @return `true` when `instance` is `ancestor` or sits beneath it.
		bool IsUnder(engine::ecs::Entity instance, engine::ecs::Entity ancestor) const;

		// Whether a filter is narrowing the view.
		//
		// @return `true` when the last request carried a non-empty filter.
		bool Filtering() const;

		// How many instances the world holds.
		//
		// A count of a scan that has already happened, rather than a second
		// pass to answer it.
		//
		// @return The instance count as of the last scan.
		size_t Count() const;

		// How many instances the filter matched by name.
		//
		// @return The match count, or 0 when nothing is being filtered.
		size_t MatchCount() const;

		// Forgets the compiled rows, so the next `Rebuild` re-compiles.
		//
		// **For the one thing the signature cannot see.** Which world a view is
		// looking at is folded in as the store's address, so pointing it at a
		// different live world already re-compiles. What that cannot catch is a
		// world destroyed and another allocated at the same address holding the
		// same content - rare enough to be an escape hatch rather than a
		// branch, and real enough to have one.
		void Forget();

	  private:
		// What the scan records per instance.
		//
		// **Keyed by `Entity::Id` and sorted, rather than indexed by an entity
		// index.** `ecs::Entity` deliberately does not expose its index - the
		// layout is the store's, and reading it from outside is how code starts
		// depending on it - so the dense side table the store uses internally
		// is not available here. A sorted vector searched with a binary chop is
		// fourteen integer compares on a ten-thousand-instance world over one
		// contiguous allocation, which is nearer that table than a hash map is.
		struct Node {
			// The complete handle, and the sort key.
			uint64_t Id = 0;

			// The three links the flatten reads. `LastChild` and
			// `PreviousSibling` are not copied because nothing here walks
			// backwards.
			engine::ecs::Entity Parent;
			engine::ecs::Entity FirstChild;
			engine::ecs::Entity NextSibling;

			engine::core::Name Name;
			engine::ecs::ClassId Class;

			// Where this instance ended up in `RowList`, or `NO_ROW`.
			size_t Row = NO_ROW;

			// `MATCH`, `KEEP` and `OPEN`.
			uint8_t Flags = 0;
		};

		// The row's own name matched the filter.
		static constexpr uint8_t MATCH = 1u << 0;

		// The row survives the filter: it matched, or something under it did.
		static constexpr uint8_t KEEP = 1u << 1;

		// The row must be opened whatever the author last clicked. Always set
		// contiguously from a node up to its root, which is what lets both
		// propagation walks stop at the first ancestor already carrying it.
		static constexpr uint8_t OPEN = 1u << 2;

		// One entry of the flatten's own stack.
		struct Pending {
			engine::ecs::Entity Instance;
			uint16_t Depth = 0;
		};

		// The node for a handle, or null when this world does not hold it.
		//
		// @param instance The handle to look up.
		// @return The node, valid until the next rebuild.
		Node *Find(engine::ecs::Entity instance);

		// The const half of `Find`.
		//
		// @param instance The handle to look up.
		// @return The node, or null.
		const Node *Find(engine::ecs::Entity instance) const;

		// Reads the world a second time and builds the rows from it.
		//
		// Reached only when the signature moved, which is what makes a second
		// pass over the columns worth having: the frame that pays for it is a
		// frame where something was created, renamed, reparented or destroyed,
		// and the frame that does not is every other one.
		//
		// @param store     The world to read.
		// @param request   What the panel wants shown.
		// @param instances How many rows the scan counted, to size the store.
		void Compile(engine::ecs::Store &store, const HierarchyRequest &request, size_t instances);

		// Sets `flags` on every ancestor of `from`, stopping at the first that
		// already carries `OPEN`.
		//
		// Bounded by the node count, so a tree that somehow held a cycle draws
		// a wrong picture rather than hanging. `Store::SetParent` refuses to
		// make one; this is the second lock on that door, and it costs a
		// comparison.
		//
		// @param from  Whose parents to mark. Not itself marked.
		// @param flags What to set on each.
		void MarkAncestors(engine::ecs::Entity from, uint8_t flags);

		// Appends a subtree to `RowList`, in display order.
		//
		// @param root  The subtree root to emit.
		// @param depth How far to indent it.
		void Flatten(engine::ecs::Entity root, uint16_t depth);

		std::vector<Node> Nodes;
		std::vector<HierarchyRow> RowList;
		std::vector<engine::ecs::Entity> RootList;

		// The open set, sorted, so the flatten can chop rather than scan. Kept
		// here so the sort is one allocation that survives frames rather than
		// one per rebuild.
		std::vector<uint64_t> OpenSorted;

		// The flatten's stack and its child scratch, members for the reason
		// `Editor::ChildScratch` is one: a vector built inside the walk is an
		// allocation per node, and this walk visits every drawn row.
		std::vector<Pending> Stack;
		std::vector<engine::ecs::Entity> Fringe;

		// The last signature, and whether there is one at all. Zero is a real
		// hash and not "unset", exactly as `scene::QuickHash` says of its own.
		uint64_t Stamp = 0;
		bool Stamped = false;

		bool Narrowed = false;
		size_t Matches = 0;
	};

	// The rows a shift-click covers, from the anchor to the row just clicked.
	//
	// **Over the drawn order, not over the tree.** A range in a tree view is
	// what the eye sees between two rows, which is the flattened order with the
	// closed subtrees left out - so it is row indices and not an ancestor walk.
	//
	// Either end being off screen - deleted, collapsed away, filtered out -
	// yields nothing, and the caller falls back to a plain click. That is what
	// every list does, and what an author reads the gesture as when the row
	// they remember shift-clicking from is no longer there.
	//
	// @param view   The compiled tree the rows were drawn from.
	// @param anchor Where the range starts.
	// @param to     Where it ends.
	// @return The rows inclusive of both ends, or an empty span.
	std::span<const HierarchyRow>
	RowsBetween(const HierarchyView &view, engine::ecs::Entity anchor, engine::ecs::Entity to);

	// The members of a set that are not inside another member.
	//
	// **What a multi-selection drag actually moves.** Dragging a model and one
	// of its own parts together means "move the model": moving both would take
	// the part out of the model on the way, which is the one outcome nobody
	// dragging them together wants. Roblox drops the nested ones for the same
	// reason.
	//
	// Order is preserved, so the caller's first choice stays first.
	//
	// @param view   The compiled tree, for the ancestor test.
	// @param moving The instances asked for.
	// @param out    Cleared, then filled with the ones to act on.
	void TopMost(
		const HierarchyView &view,
		std::span<const engine::ecs::Entity> moving,
		std::vector<engine::ecs::Entity> &out
	);

}
