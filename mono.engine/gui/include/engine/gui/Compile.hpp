#pragma once

// The compiled draw list, kept until the tree it came from moves.
//
// **The expensive half is not reading the components.** It is what comes
// after: laying out, sorting siblings by `ZIndex`, intersecting clip
// rectangles, flattening the tree into a list and building a command per
// visible element. A UI is a thing somebody looks at all day, and on almost
// every one of those frames the answer is the same answer as last frame.
//
// So the scan computes a **signature** - a rolling hash of every field the
// compile reads - and when it matches the last one, the compiled list is still
// correct and is kept. This is `scene::QuickHash`'s pattern and
// `studio::HierarchyView`'s, one layer over, and it is the same fallback for
// the same reason: `ecs::Hierarchy`, `gui::Element` and the rest are not
// observed components, so `Store::ChangeVersion` does not move when an element
// is reparented, resized or renamed - and it *does* move when a physics tick
// writes a transform, which would rebuild the UI sixty times a second for
// nothing.
//
// ## What the signature covers, which is the whole correctness argument
//
// | Folded in | Why it has to be |
// |---|---|
// | The store's address | Two worlds built the same way hash identically |
// | The screen size and inset | Every `UDim2` resolves against it |
// | Entity id, per row | A destroyed row and a new one may otherwise match |
// | `Hierarchy` parent, first child, next sibling | The order the flatten descends in |
// | `InstanceName` | `SortOrder::Name` reads it |
// | Every field of every component this module declares | All of it reaches a rectangle or a command |
// | The hovered and pressed instances | `AutoButtonColor` shifts a fill |
//
// A field added to a component has to be added to the fold in `Compile.cpp`,
// and the failure if it is not is a UI one edit stale. `gui/tests/Compile.cpp`
// is what turns that from a rule into a check: it walks every property the
// class tree declares, writes each one, and asserts the signature moved.
//
// **The direction matters and only one way round is safe.** A signature that
// *collides* keeps a list the world has moved on from, which is a UI showing
// what is no longer there; a signature that changes when nothing really did
// costs one rebuild nobody sees. Every choice here leans the second way - an
// archetype that reshuffles its rows without changing a value still
// re-compiles.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// What the compile needs to know that is not in the store.
	//
	// @since v0.8
	struct CompileRequest {
		// The screen a `ScreenGui` collects onto.
		Screen Display;

		// The element the pointer is over, or null.
		//
		// **An input, not a result.** It shifts an `AutoButtonColor` fill, so
		// it changes the compiled list and therefore belongs in the signature.
		// Writing the hover into the component instead would make
		// `BackgroundColor3` read back differently depending on where the mouse
		// is, which is a script bug nobody could see.
		ecs::Entity Hovered;

		// The element the pointer is pressed on, or null.
		ecs::Entity Pressed;
	};

	// A compiled draw list and the signature that says whether it is still
	// good.
	//
	// Long-lived: one per surface being drawn, kept across frames. Holding one
	// per frame would compute a signature, find nothing to compare it against
	// and rebuild every time, which is every cost of this design and none of
	// its benefit.
	//
	// @since v0.8
	class Compiled {
	  public:
		// Brings the list up to date, rebuilding only if it has to.
		//
		// @param store   The world.
		// @param request The screen and the pointer state.
		// @return `true` when the list was rebuilt, `false` when the previous
		//         one was still correct. A caller uploading vertices can skip
		//         the upload on `false`.
		bool Rebuild(ecs::Store &store, const CompileRequest &request);

		// The list, whether or not this frame rebuilt it.
		const DrawList &Commands() const {
			return List;
		}

		// The signature of what the list was built from.
		//
		// Exposed for tests and for a panel reporting why a rebuild happened.
		// Zero before the first `Rebuild`, and zero is not a reserved value -
		// what makes a comparison meaningful is that both sides came out of the
		// same function, exactly as `scene::QuickHash` says of its own.
		uint64_t Signature() const {
			return Stamp;
		}

		// How many times this has rebuilt, and how many times it has been
		// asked. The ratio is the whole point of the class, so it is readable
		// rather than inferred from a profiler.
		size_t Rebuilds() const {
			return Built;
		}

		// How many times the compiled list was asked for.
		//
		// Read beside `Rebuilds`: the two being equal means the cache never hit,
		// which is the shape of a tree whose hash moves every frame.
		//
		// @return The count since construction.
		size_t Requests() const {
			return Asked;
		}

		// Forgets the signature so the next `Rebuild` rebuilds.
		//
		// For a caller whose *backend* state was lost - a device reset, a
		// resized target - where the list is correct and the thing that
		// consumed it is not.
		void Invalidate() {
			Stamp = 0;
			Fresh = false;
		}

	  private:
		DrawList List;
		uint64_t Stamp = 0;
		size_t Built = 0;
		size_t Asked = 0;

		// Whether `Stamp` came from a real scan rather than from the initial
		// zero. Without it, a world whose scan genuinely hashes to zero would
		// be treated as already compiled and would draw nothing, forever.
		bool Fresh = false;
	};

	// Every shader an `ImageLabel` or `ImageButton` in this world names,
	// without duplicates.
	//
	// **`scene::DemandedShaders`'s exact shape, one indirection flatter.**
	// That function walks `MaterialRef` because a part's shader is authored
	// on a child instance; a `Picture` carries its own name directly, so this
	// walks `Picture` rather than anything standing in for it. Both feed the
	// same `render::ShaderLibrary`, which resolves a name against the same
	// `scene::ShaderScript` tree and the same built-ins regardless of which
	// module asked.
	//
	// A `const` walk, so this may be called from a read-only consumer.
	//
	// @param store The world.
	// @param out   Filled with the names, sorted by id. Cleared first.
	// @return How many distinct shaders are named.
	// @since v0.18
	size_t DemandedShaders(ecs::Store &store, std::vector<core::Name> &out);
}
