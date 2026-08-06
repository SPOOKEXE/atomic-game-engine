// What a user interface costs per frame, at the tree sizes an interface has.
//
// **Layout runs every frame over every element, and compilation runs every
// frame over everything laid out.** Neither is optional and neither is
// incremental in the general case, so an interface that is merely *large* is a
// per-frame cost the game pays whether or not anything on it changed. That is
// the number this suite exists to produce, and the reason `Compiled` has a
// signature at all.
//
// **Depth and breadth are separated on purpose, because they fail differently.**
// A wide tree is a long linear walk and its cost is the element count. A deep
// tree is a recursive resolve where every level multiplies its parent's
// rectangle, and its cost can be the element count *times the depth* if
// anything is recomputed on the way down instead of being passed along. The two
// ladders below carry the same number of elements in each shape, so a
// difference between them is that multiplication and nothing else.
//
// **The `Compiled` rows are where the design either pays or does not.** A
// signature is computed over the whole tree every frame in order to skip a
// rebuild — so the unchanged path costs one signature pass, and it is only
// worth having if that pass is far cheaper than the rebuild it avoids. Both are
// measured. If they are close, the class is doing twice the work on every frame
// that *does* change and saving nothing on the frames that do not.
//
// --- what these rows have already been used for -------------------------------
//
// The first run of this suite reported **275 ns per element**, which is around a
// hundred times what iterating an entity costs in `engine.ecs.bench.iteration`
// and made a ten-thousand-element interface 2.9 ms of every frame while sitting
// perfectly still. Three things in `Layout.cpp` accounted for a third of it, and
// none of them were visible without a per-element figure to divide by:
//
//   - `ModifiersOf` tested seven component types against every child, when a
//     modifier is a `UIComponent` and one `Get<Element>` rules out all seven.
//     A container is usually a frame full of frames, so that was seven misses
//     per child per frame. → 275 to 243.
//   - It then ran **twice per element** — once to measure the node and once to
//     place it — over the same child list. The result is carried on `Item` now.
//     → 243 to 200.
//   - `ChildItems` heap-allocated a fresh list per container per frame, and
//     `InstanceNameOf` took the process-wide name registry's lock for every
//     child whether or not the container sorted by name. → 200 to 173.
//
// **What is left is structural rather than wasteful.** `EachChild` walks an
// intrusive `FirstChild`/`NextSibling` list through a `std::function`, so each
// child costs a type-erased call and a pointer chase into another table — and
// two such walks per element remain, because measuring a node and placing it
// both need its child list and the two happen at different times. Merging them
// needs either contiguous child storage in `ecs` or a scratch arena holding one
// child list per element rather than per depth. Both are larger decisions than
// a benchmark should make on its own; this note is here so the next person
// starts from the analysis rather than from the number.

#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.gui.bench.interface")

using engine::ecs::Entity;
using engine::ecs::Store;
using engine::gui::Compiled;
using engine::gui::CompileRequest;
using engine::gui::GuiClass;
using engine::gui::Layout;
using engine::gui::RegisterGuiClasses;
using engine::gui::Screen;
using engine::testing::Consume;

namespace interface_bench {

	// A 1080p canvas, which is what an interface is authored against.
	Screen Display() {
		Screen screen;
		screen.Width = 1920.0f;
		screen.Height = 1080.0f;
		return screen;
	}

	// A world holding one interface tree, built once per shape.
	//
	// **Built lazily rather than at static-initialisation time**, for the reason
	// `ecs`'s own suite gives: a store binds its owning thread on construction,
	// and the thread that constructs a namespace static is not necessarily the
	// one that runs the body.
	struct Interface {
		std::unique_ptr<Store> Data;
		Entity Root;
	};

	// Registers the class tree exactly once for the process.
	//
	// Registration interns names, and doing it per world would make the first
	// tree in a run pay for something none of the others do — which would show
	// up as the 1000-element row being dearer than the 4000-element one.
	void EnsureRegistered() {
		static const bool once = [] {
			RegisterGuiClasses();
			return true;
		}();
		Consume(once);
	}

	// A tree of `count` frames, `depth` levels deep, each level fanning out
	// evenly to reach the count.
	//
	// Every element gets a real `Element` with a scale-and-offset size, because
	// a `UDim2` that is pure offset skips the parent-relative arithmetic that is
	// the expensive half of resolving one.
	Interface &TreeOf(size_t count, size_t depth) {
		static std::vector<std::pair<std::pair<size_t, size_t>, Interface>> built;
		for (auto &[key, made] : built) {
			if (key.first == count && key.second == depth) {
				return made;
			}
		}

		EnsureRegistered();

		Interface made;
		made.Data = std::make_unique<Store>("gui_bench_" + std::to_string(count) + "_" + std::to_string(depth));
		Store &store = *made.Data;

		made.Root = store.CreateInstance(GuiClass("ScreenGui"), "ScreenGui");

		// **Exactly `depth` levels, each holding `count / depth` elements.**
		// Written as a fixed per-level quota rather than a fan-out ratio because
		// a ratio overshoots: it fills the tree in the first two or three levels
		// and the remaining levels come out empty, so a row labelled "16 deep"
		// is really four deep and the ladder measures nothing. Each level's
		// elements are spread round-robin across the previous level's, so the
		// tree is genuinely both wide and deep rather than a chain.
		std::vector<Entity> previous{made.Root};
		std::vector<Entity> current;
		size_t placed = 0;

		const size_t perLevel = depth == 0 ? count : (count / depth == 0 ? 1 : count / depth);

		for (size_t level = 0; level < depth && placed < count; level++) {
			// The last level takes the remainder, so the totals come out exact
			// rather than short by the division's truncation.
			const size_t here = (level + 1 == depth) ? (count - placed) : perLevel;

			current.clear();
			current.reserve(here);
			for (size_t child = 0; child < here && placed < count; child++) {
				const Entity parent = previous[child % previous.size()];
				{
					const Entity element = store.CreateInstance(GuiClass("Frame"), "Frame");
					store.SetParent(element, parent);

					// `GetMutable` rather than `Get`, which hands back a
					// `const T *` — the store's read and write paths are
					// separate so that a change is always an explicit one.
					if (auto *shape = store.GetMutable<engine::gui::Element>(element)) {
						// Scale plus offset, so resolving it needs the parent's
						// rectangle. A pure-offset size would let a resolver
						// shortcut and would measure the shortcut.
						shape->Size = engine::core::UDim2(
							engine::core::UDim(0.4f, 8.0f), engine::core::UDim(0.4f, 8.0f)
						);
						shape->Position = engine::core::UDim2(
							engine::core::UDim(0.1f, 2.0f), engine::core::UDim(0.1f, 2.0f)
						);
					}

					current.push_back(element);
					placed++;
				}
			}

			if (current.empty()) {
				break;
			}
			previous = current;
		}

		built.emplace_back(std::make_pair(count, depth), std::move(made));
		return built.back().second;
	}

	// A `Compiled` per tree shape, kept across frames the way a surface keeps
	// one. Holding one per frame would compute a signature, find nothing to
	// compare it against and rebuild every time — every cost of the design and
	// none of its benefit.
	Compiled &CompiledFor(size_t count, size_t depth) {
		static std::vector<std::pair<std::pair<size_t, size_t>, std::unique_ptr<Compiled>>> built;
		for (auto &[key, compiled] : built) {
			if (key.first == count && key.second == depth) {
				return *compiled;
			}
		}
		built.emplace_back(std::make_pair(count, depth), std::make_unique<Compiled>());
		return *built.back().second;
	}
}

using namespace interface_bench;

// --- layout, by breadth ---------------------------------------------------------
//
// One iteration is one element, so the ladder divides into a per-element cost
// and says directly whether layout is linear in the tree.

BENCH("Layout · 100 elements, 3 deep", 100) {
	Interface &tree = TreeOf(100, 3);
	Consume(Layout(*tree.Data, Display()));
}

BENCH("Layout · 1k elements, 3 deep", 1000) {
	Interface &tree = TreeOf(1000, 3);
	Consume(Layout(*tree.Data, Display()));
}

BENCH("Layout · 10k elements, 3 deep", 10'000) {
	// **A ten-thousand-element interface is an inventory grid, not an absurdity**
	// — a hundred slots with a hundred badges each is exactly this. If the
	// per-element cost is flat from a hundred up to here, layout is linear and a
	// large interface is merely proportionally expensive; if it climbs, there is
	// a per-element search and the grid is quadratic.
	Interface &tree = TreeOf(10'000, 3);
	Consume(Layout(*tree.Data, Display()));
}

// --- layout, by depth -------------------------------------------------------------
//
// **The same element count in a deeper tree.** Every level resolves against its
// parent's rectangle, so a deep tree does the same number of resolves as a wide
// one — unless something is recomputed on the way down, in which case this
// ladder climbs and the wide one does not.

BENCH("Layout · 1k elements, 2 deep", 1000) {
	Interface &tree = TreeOf(1000, 2);
	Consume(Layout(*tree.Data, Display()));
}

BENCH("Layout · 1k elements, 8 deep", 1000) {
	Interface &tree = TreeOf(1000, 8);
	Consume(Layout(*tree.Data, Display()));
}

BENCH("Layout · 1k elements, 16 deep", 1000) {
	// Sixteen levels is a panel inside a scroller inside a tab inside a window,
	// nested the way a real editor nests things. Read against the 2-deep row at
	// the same count: any gap is the cost of depth itself.
	Interface &tree = TreeOf(1000, 16);
	Consume(Layout(*tree.Data, Display()));
}

// --- compilation ------------------------------------------------------------------

BENCH("Compiled::Rebuild · 1k elements, unchanged", 1000) {
	// **The frame that should be nearly free**, and the one an interface spends
	// almost all of its frames in: nothing moved, so the signature matches and
	// no list is built. What it costs is one signature pass over the whole tree
	// — which is not nothing, and is the price of the optimisation.
	Interface &tree = TreeOf(1000, 3);
	Compiled &compiled = CompiledFor(1000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

BENCH("Compiled::Rebuild · 1k elements, forced rebuild", 1000) {
	// **What the signature is buying**, measured against the row above.
	// `Invalidate` forgets the signature so every call rebuilds, which is the
	// cost of a frame where something genuinely changed. If the two rows are
	// close, the signature pass costs about what it saves and the class is not
	// earning its complexity.
	Interface &tree = TreeOf(1000, 3);
	Compiled &compiled = CompiledFor(1000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	compiled.Invalidate();
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

BENCH("Compiled::Rebuild · 10k elements, unchanged", 10'000) {
	Interface &tree = TreeOf(10'000, 3);
	Compiled &compiled = CompiledFor(10'000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

BENCH("Compiled::Rebuild · 10k elements, forced rebuild", 10'000) {
	Interface &tree = TreeOf(10'000, 3);
	Compiled &compiled = CompiledFor(10'000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	compiled.Invalidate();
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

// --- a frame -----------------------------------------------------------------------

BENCH("frame · 1k elements laid out and compiled, nothing changed", 1000) {
	// **The steady state**, which is what an interface does on almost every
	// frame: lay out, check the signature, upload nothing. One iteration is one
	// element, so this figure times the element count is the per-frame share a
	// game pays for having an interface open at all.
	//
	// Compare against a 16.7 ms frame. An interface that eats a visible slice of
	// it while sitting perfectly still is one whose layout should be
	// incremental, and the two rows above say whether the cost is in the layout
	// or in the signature.
	Interface &tree = TreeOf(1000, 3);
	Compiled &compiled = CompiledFor(1000, 3);

	CompileRequest request;
	request.Display = Display();

	Consume(Layout(*tree.Data, request.Display));
	Consume(compiled.Rebuild(*tree.Data, request));
}

BENCH("frame · 1k elements with the pointer moving over them", 1000) {
	// **Hover is an input to compilation, not a result** — it shifts an
	// `AutoButtonColor` fill, so it changes the compiled list and belongs in the
	// signature. Which means a moving mouse invalidates the list every frame,
	// and this row is what an interface costs while the player is simply moving
	// the cursor across it. That is not a rare case; it is most of the time
	// anybody is looking at a menu.
	Interface &tree = TreeOf(1000, 3);
	Compiled &compiled = CompiledFor(1000, 3);
	static size_t frame = 0;

	CompileRequest request;
	request.Display = Display();

	// A different element hovered each frame, so the signature genuinely differs
	// and the rebuild genuinely happens.
	const std::vector<Entity> &elements = [&tree]() -> const std::vector<Entity> & {
		static std::vector<Entity> found;
		if (found.empty()) {
			tree.Data->Each<engine::gui::Element>([](Entity entity, engine::gui::Element &) {
				found.push_back(entity);
			});
		}
		return found;
	}();

	if (!elements.empty()) {
		request.Hovered = elements[frame % elements.size()];
	}
	frame++;

	Consume(Layout(*tree.Data, request.Display));
	Consume(compiled.Rebuild(*tree.Data, request));
}
