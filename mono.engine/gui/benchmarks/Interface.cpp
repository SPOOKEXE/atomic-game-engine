// What a user interface costs per frame, at the tree sizes an interface has.
//
// **The retained client lets `Compiled::Rebuild` own the layout gate.** An
// unchanged source scans its signature and returns before layout, compilation,
// and presentation work. The explicit layout rows below remain useful
// diagnostics for accepted changes, where a large interface is still a
// proportional cost.
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
// source scan decides whether to skip the canonical pipeline, so the unchanged
// path costs one signature pass. It is only worth having if that pass is far
// cheaper than the accepted layout and rebuild it avoids. Both are measured.
//
// --- what these rows have already been used for -------------------------------
//
// Before the retained layout gate, the first full-path run reported **275 ns per
// element**, around a hundred times what iterating an entity costs in
// `engine.ecs.bench.iteration`. Three things in `Layout.cpp` accounted for a
// third of it, and none were visible without a per-element figure to divide by:
//
//   - `ModifiersOf` tested seven component types against every child, when a
//     modifier is a `UIComponent` and one `Get<Element>` rules out all seven.
//     A container is usually a frame full of frames, so that was seven misses
//     per child per frame. → 275 to 243.
//   - It then ran **twice per element** - once to measure the node and once to
//     place it - over the same child list. The result is carried on `Item` now.
//     → 243 to 200.
//   - `ChildItems` heap-allocated a fresh list per container per frame, and
//     `InstanceNameOf` took the process-wide name registry's lock for every
//     child whether or not the container sorted by name. → 200 to 173.
//
// That left one structural cost: `EachChild` walks an intrusive
// `FirstChild`/`NextSibling` list through a `std::function`, so each child is a
// type-erased call and a pointer chase into another table - and *two* such walks
// per element remained, because measuring a node and placing it both need its
// child list and the two happen at different times.
//
//   - Merged, by having the one walk produce both answers at once and parking
//     the child run in a shared arena that `Place` reads back. The arena is a
//     stack: a node marks it, measures its children - which appends each
//     child's own children on top - places them, and releases to the mark
//     through a destructor rather than a statement. → 173 to **152**.
//
// **So the pass now walks each child list exactly once, and 152 ns is what a
// walk costs rather than what waste costs.** Going further means changing where
// children live: contiguous per-parent storage in `ecs` would turn the walk
// into a linear scan of a span and would benefit every consumer of `EachChild`,
// not just this one. That is a decision about the entity store rather than
// about layout, which is why it is written down here and not attempted.
//
// A 10 000-element forced layout and compile went from 2.9 ms to 1.5 ms over
// these four changes, about 179 ns per element. The retained scenario rows below
// separately measure the source-scan hit that avoids this work.

#include <engine/core/Name.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/gui/VirtualCollection.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
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
	using namespace engine::gui;

	// Optimized benchmark builds remove assertions. Fixture construction is
	// part of the measured contract, so a failed setup must stop the run instead
	// of silently timing an empty draw list.
	void Require(bool condition) {
		if (!condition) {
			std::abort();
		}
	}

	template <typename T> T &Require(T *value) {
		Require(value != nullptr);
		return *value;
	}

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
		Entity Container;
		Entity Root;
	};

	// Registers the class tree exactly once for the process.
	//
	// Registration interns names, and doing it per world would make the first
	// tree in a run pay for something none of the others do - which would show
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
		made.Data =
			std::make_unique<Store>("gui_bench_" + std::to_string(count) + "_" + std::to_string(depth));
		Store &store = *made.Data;

		made.Container =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui");
		made.Root = store.CreateInstance(GuiClass("ScreenGui"), "ScreenGui");
		store.SetParent(made.Root, made.Container);

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
					// `const T *` - the store's read and write paths are
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

		// A collector without a GUI service ancestor is deliberately ignored by
		// layout. Verify the fixture is a drawable tree before timing it.
		Require(Layout(store, Display()) == count);

		built.emplace_back(std::make_pair(count, depth), std::move(made));
		return built.back().second;
	}

	// A `Compiled` per tree shape, kept across frames the way a surface keeps
	// one. Holding one per frame would compute a signature, find nothing to
	// compare it against and rebuild every time - every cost of the design and
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

	// One ordinary screen tree with a named root. Scenario fixtures keep their
	// stores alive across benchmark samples, matching the retained world a client
	// uses rather than charging setup work to every frame.
	struct Scenario {
		std::unique_ptr<Store> Data;
		Entity Container;
		Entity Root;
		Compiled List;
	};

	Scenario MakeScenario(std::string_view name, bool services = false) {
		EnsureRegistered();
		Scenario made;
		made.Data = std::make_unique<Store>(std::string(name));
		if (services) {
			InstallGuiServices(*made.Data);
		}
		made.Container = made.Data->CreateInstance(
			engine::ecs::Classes::Find(engine::core::Name("Instance")), "StarterGui"
		);
		made.Root = made.Data->CreateInstance(GuiClass("ScreenGui"), "ScreenGui");
		made.Data->SetParent(made.Root, made.Container);
		return made;
	}

	void SetRect(Store &store, Entity entity, float x, float y, float width, float height) {
		Element element;
		element.Position = engine::core::UDim2{0.0f, x, 0.0f, y};
		element.Size = engine::core::UDim2{0.0f, width, 0.0f, height};
		store.Set(entity, element);
	}

	struct HudScenario {
		Scenario World;
		Entity Accent;
	};

	HudScenario &Hud() {
		static std::unique_ptr<HudScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<HudScenario>();
		made->World = MakeScenario("gui_bench_hud");
		Store &store = *made->World.Data;
		for (size_t index = 0; index < 24; index++) {
			const Entity label = store.CreateInstance(GuiClass("TextLabel"), "HudLabel");
			store.SetParent(label, made->World.Root);
			SetRect(
				store,
				label,
				24.0f + static_cast<float>(index % 6) * 208.0f,
				24.0f + static_cast<float>(index / 6) * 42.0f,
				180.0f,
				32.0f
			);
			if (Label *text = store.GetMutable<Label>(label)) {
				text->Text = "HUD " + std::to_string(index);
			}
			if (index == 0) {
				made->Accent = label;
			}
		}
		Require(Layout(store, Display()) == 24);
		CompileRequest request;
		request.Display = Display();
		Require(made->World.List.Rebuild(store, request));
		return *made;
	}

	struct TextEditScenario {
		Scenario World;
		Entity Fixed;
		Entity Automatic;
	};

	TextEditScenario &TextEdits() {
		static std::unique_ptr<TextEditScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<TextEditScenario>();
		made->World = MakeScenario("gui_bench_text_edits", true);
		Store &store = *made->World.Data;
		const Entity automaticParent = store.CreateInstance(GuiClass("Frame"), "AutomaticPanel");
		store.SetParent(automaticParent, made->World.Root);
		SetRect(store, automaticParent, 40.0f, 40.0f, 0.0f, 0.0f);
		store.GetMutable<Element>(automaticParent)->Automatic = engine::gui::AutomaticSize::XY;

		made->Automatic = store.CreateInstance(GuiClass("TextBox"), "AutomaticText");
		store.SetParent(made->Automatic, automaticParent);
		SetRect(store, made->Automatic, 0.0f, 0.0f, 0.0f, 0.0f);
		store.GetMutable<Element>(made->Automatic)->Automatic = engine::gui::AutomaticSize::XY;
		store.GetMutable<Label>(made->Automatic)->Text = "automatic";

		made->Fixed = store.CreateInstance(GuiClass("TextBox"), "FixedText");
		store.SetParent(made->Fixed, made->World.Root);
		SetRect(store, made->Fixed, 40.0f, 180.0f, 360.0f, 36.0f);
		store.GetMutable<Label>(made->Fixed)->Text = "fixed";

		Require(Layout(store, Display()) == 3);
		CompileRequest request;
		request.Display = Display();
		Require(made->World.List.Rebuild(store, request));
		return *made;
	}

	struct VirtualScenario {
		Scenario World;
		Entity List;
	};

	VirtualScenario &VirtualRows() {
		static std::unique_ptr<VirtualScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<VirtualScenario>();
		made->World = MakeScenario("gui_bench_virtual_rows");
		Store &store = *made->World.Data;
		made->List = store.CreateInstance(GuiClass("ScrollingFrame"), "Inventory");
		store.SetParent(made->List, made->World.Root);
		SetRect(store, made->List, 32.0f, 32.0f, 640.0f, 480.0f);
		const Entity collection = store.CreateInstance(GuiClass("UIVirtualCollection"), "Rows");
		store.SetParent(collection, made->List);
		const Entity row = store.CreateInstance(GuiClass("TextLabel"), "RowTemplate");
		store.SetParent(row, collection);
		SetRect(store, row, 0.0f, 0.0f, 640.0f, 24.0f);
		store.GetMutable<Label>(row)->Text = "virtual row";

		VirtualCollection source;
		source.ItemCount = 1'000'000;
		source.FixedExtent = 24.0f;
		source.ExtentPolicy = VirtualExtentPolicy::Measured;
		source.Overscan = 4;
		for (uint32_t index = 0; index < 64; index++) {
			VirtualRecord record;
			record.Key = "row-" + std::to_string(index);
			record.MeasuredExtent = index % 3 == 0 ? 36.0f : 20.0f;
			source.Page.Records.push_back(std::move(record));
		}
		store.Set(collection, std::move(source));
		Require(Layout(store, Display()) == 2);
		CompileRequest request;
		request.Display = Display();
		Require(made->World.List.Rebuild(store, request));
		return *made;
	}

	// A non-virtual comparison point: an inventory whose thousand rows really
	// are instances. This keeps the virtual-source number honest by exposing
	// what materialising an ordinary data table costs through the same pipeline.
	Scenario &RealRows() {
		static std::unique_ptr<Scenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<Scenario>(MakeScenario("gui_bench_real_rows"));
		Store &store = *made->Data;
		const Entity list = store.CreateInstance(GuiClass("ScrollingFrame"), "Inventory");
		store.SetParent(list, made->Root);
		SetRect(store, list, 32.0f, 32.0f, 640.0f, 480.0f);
		for (size_t index = 0; index < 1000; index++) {
			const Entity row = store.CreateInstance(GuiClass("TextLabel"), "Row");
			store.SetParent(row, list);
			SetRect(store, row, 0.0f, static_cast<float>(index) * 24.0f, 640.0f, 24.0f);
			store.GetMutable<Label>(row)->Text = "Row " + std::to_string(index);
		}
		Require(Layout(store, Display()) == 1001);
		CompileRequest request;
		request.Display = Display();
		Require(made->List.Rebuild(store, request));
		return *made;
	}

	struct LocaleScenario {
		Scenario World;
		LocalizationCatalogue Catalogue;
	};

	LocaleScenario &Locales() {
		static std::unique_ptr<LocaleScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<LocaleScenario>();
		made->World = MakeScenario("gui_bench_locales");
		Store &store = *made->World.Data;
		const Entity label = store.CreateInstance(GuiClass("TextLabel"), "LocalizedStatus");
		store.SetParent(label, made->World.Root);
		SetRect(store, label, 64.0f, 64.0f, 480.0f, 48.0f);
		store.GetMutable<Label>(label)->Text = "Ready";
		store.Set(label, LabelPresentation{engine::core::Name("status")});
		Require(made->Catalogue.Set({"en", engine::core::Name("status"), "Ready: {count, number}"}));
		Require(made->Catalogue.Set({"fr", engine::core::Name("status"), "Pret: {count, number}"}));
		Require(made->Catalogue.Set({"ja", engine::core::Name("status"), "準備完了: {count, number}"}));
		Require(made->Catalogue.Set({"ar", engine::core::Name("status"), "جاهز: {count, number}"}));
		LabelLocalizationArguments arguments;
		arguments.Count = 1;
		arguments.Values[0].Name = engine::core::Name("count");
		arguments.Values[0].Type = LocalizedArgumentType::Number;
		arguments.Values[0].Number = 42.0;
		store.Set(label, arguments);
		Require(Layout(store, Display()) == 1);
		return *made;
	}

	struct NavigationScenario {
		Scenario World;
	};

	NavigationScenario &Navigation() {
		static std::unique_ptr<NavigationScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<NavigationScenario>();
		made->World = MakeScenario("gui_bench_navigation", true);
		Store &store = *made->World.Data;
		for (size_t index = 0; index < 144; index++) {
			const Entity button = store.CreateInstance(GuiClass("TextButton"), "Control");
			store.SetParent(button, made->World.Root);
			SetRect(
				store,
				button,
				16.0f + static_cast<float>(index % 12) * 104.0f,
				16.0f + static_cast<float>(index / 12) * 42.0f,
				96.0f,
				34.0f
			);
			store.GetMutable<Element>(button)->Selectable = true;
		}
		Require(Layout(store, Display()) == 144);
		CompileRequest request;
		request.Display = Display();
		Require(made->World.List.Rebuild(store, request));
		return *made;
	}

	struct SpatialScenario {
		Scenario World;
		std::vector<Entity> Collectors;
	};

	SpatialScenario &SpatialBoundary() {
		static std::unique_ptr<SpatialScenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<SpatialScenario>();
		made->World = MakeScenario("gui_bench_spatial_boundary");
		Store &store = *made->World.Data;
		const Entity workspace = store.CreateInstance(
			engine::ecs::Classes::Find(engine::core::Name("Instance")), std::string(WORKSPACE)
		);
		for (size_t index = 0; index < 128; index++) {
			const Entity collector = store.CreateInstance(GuiClass("SurfaceGui"), "SpatialHud");
			store.SetParent(collector, workspace);
			SpatialCanvas canvas;
			canvas.Size = {320.0f, 180.0f};
			canvas.Visible = index % 2 == 0;
			store.Set(collector, canvas);
			const Entity child = store.CreateInstance(GuiClass("Frame"), "Panel");
			store.SetParent(child, collector);
			SetRect(store, child, 0.0f, 0.0f, 320.0f, 180.0f);
			made->Collectors.push_back(collector);
		}
		CompileRequest request;
		request.Display = Display();
		Require(made->World.List.Rebuild(store, request));
		return *made;
	}

	Scenario &ViewportFrames() {
		static std::unique_ptr<Scenario> made;
		if (made != nullptr) {
			return *made;
		}

		made = std::make_unique<Scenario>(MakeScenario("gui_bench_viewport_frames"));
		Store &store = *made->Data;
		for (size_t index = 0; index < 8; index++) {
			const Entity frame = store.CreateInstance(GuiClass("ViewportFrame"), "Viewport");
			store.SetParent(frame, made->Root);
			SetRect(
				store,
				frame,
				static_cast<float>(index % 4) * 160.0f,
				static_cast<float>(index / 4) * 120.0f,
				144.0f,
				104.0f
			);
			Viewport viewport;
			viewport.CurrentCamera = Entity{static_cast<uint32_t>(index + 1)};
			viewport.UpdateMode = index == 7 ? ViewportUpdateMode::EveryFrame : ViewportUpdateMode::OnChange;
			store.Set(frame, viewport);
		}
		CompileRequest request;
		request.Display = Display();
		Require(made->List.Rebuild(store, request));
		return *made;
	}
}

using namespace interface_bench;

// --- layout, by breadth ---------------------------------------------------------
//
// One iteration is one element, so the ladder divides into a per-element cost
// and says directly whether layout is linear in the tree.

BENCH_PER_ITEM("Layout · 100 elements, 3 deep", 100) {
	Interface &tree = TreeOf(100, 3);
	Consume(Layout(*tree.Data, Display()));
}

BENCH_PER_ITEM("Layout · 1k elements, 3 deep", 1000) {
	Interface &tree = TreeOf(1000, 3);
	Consume(Layout(*tree.Data, Display()));
}

BENCH_PER_ITEM("Layout · 10k elements, 3 deep", 10'000) {
	// **A ten-thousand-element interface is an inventory grid, not an absurdity**
	// - a hundred slots with a hundred badges each is exactly this. If the
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
// one - unless something is recomputed on the way down, in which case this
// ladder climbs and the wide one does not.

BENCH_PER_ITEM("Layout · 1k elements, 2 deep", 1000) {
	Interface &tree = TreeOf(1000, 2);
	Consume(Layout(*tree.Data, Display()));
}

BENCH_PER_ITEM("Layout · 1k elements, 8 deep", 1000) {
	Interface &tree = TreeOf(1000, 8);
	Consume(Layout(*tree.Data, Display()));
}

BENCH_PER_ITEM("Layout · 1k elements, 16 deep", 1000) {
	// Sixteen levels is a panel inside a scroller inside a tab inside a window,
	// nested the way a real editor nests things. Read against the 2-deep row at
	// the same count: any gap is the cost of depth itself.
	Interface &tree = TreeOf(1000, 16);
	Consume(Layout(*tree.Data, Display()));
}

// --- compilation ------------------------------------------------------------------

BENCH_PER_ITEM("Compiled::Rebuild · 1k elements, unchanged", 1000) {
	// **The frame that should be nearly free**, and the one an interface spends
	// almost all of its frames in: nothing moved, so the signature matches and
	// no list is built. What it costs is one signature pass over the whole tree
	// - which is not nothing, and is the price of the optimisation.
	Interface &tree = TreeOf(1000, 3);
	Compiled &compiled = CompiledFor(1000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

BENCH_PER_ITEM("Compiled::Rebuild · 1k elements, forced rebuild", 1000) {
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

BENCH_PER_ITEM("Compiled::Rebuild · 10k elements, unchanged", 10'000) {
	Interface &tree = TreeOf(10'000, 3);
	Compiled &compiled = CompiledFor(10'000, 3);

	CompileRequest request;
	request.Display = Display();

	Layout(*tree.Data, request.Display);
	Consume(compiled.Rebuild(*tree.Data, request));
	Consume(compiled.Commands().Commands.size());
}

BENCH_PER_ITEM("Compiled::Rebuild · 10k elements, forced rebuild", 10'000) {
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

BENCH_PER_ITEM("frame · 1k elements, forced layout then compile", 1000) {
	// This remains an explicit full-pipeline diagnostic. The retained scenario rows use
	// `Rebuild` directly, which is the client path and skips this work on a hit.
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

BENCH_PER_ITEM("frame · 1k elements with the pointer moving over them", 1000) {
	// **Hover is an input to compilation, not a result** - it shifts an
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

// --- retained scenarios ----------------------------------------------------------
//
// These are bounded player-facing workloads. They retain their world and draw
// list between samples, so a row measures the work a client performs
// after loading rather than entity construction or font/catalogue setup.

BENCH("UI · unchanged 24-label HUD", 1) {
	HudScenario &hud = Hud();
	CompileRequest request;
	request.Display = Display();
	Consume(hud.World.List.Rebuild(*hud.World.Data, request));
	Consume(hud.World.List.Commands().Commands.size());
}

BENCH("UI · 24-label HUD colour change", 1) {
	HudScenario &hud = Hud();
	static bool bright = false;
	Background *background = hud.World.Data->GetMutable<Background>(hud.Accent);
	Require(background).Color =
		bright ? engine::core::Color3{0.15f, 0.6f, 1.0f} : engine::core::Color3{1.0f, 0.3f, 0.1f};
	bright = !bright;

	CompileRequest request;
	request.Display = Display();
	Consume(hud.World.List.Rebuild(*hud.World.Data, request));
	Consume(hud.World.List.Commands().Commands.size());
}

BENCH("UI · text edit outside automatic-size chain, 3 elements", 1) {
	TextEditScenario &text = TextEdits();
	if (FocusedTextBox(*text.World.Data) != text.Fixed) {
		Require(Focus(*text.World.Data, text.Fixed));
	}
	static bool insert = true;
	Typing typing;
	if (insert) {
		typing.Text = "x";
	} else {
		typing.Backspace = true;
	}
	const TypeResult typed = Type(*text.World.Data, typing);
	insert = !insert;
	Require(typed.Changed);
	Consume(typed.Instance);

	CompileRequest request;
	request.Display = Display();
	Consume(text.World.List.Rebuild(*text.World.Data, request));
	Consume(text.World.List.Commands().Commands.size());
}

BENCH("UI · text edit inside automatic-size chain, 3 elements", 1) {
	TextEditScenario &text = TextEdits();
	if (FocusedTextBox(*text.World.Data) != text.Automatic) {
		Require(Focus(*text.World.Data, text.Automatic));
	}
	static bool insert = true;
	Typing typing;
	if (insert) {
		typing.Text = "x";
	} else {
		typing.Backspace = true;
	}
	const TypeResult typed = Type(*text.World.Data, typing);
	insert = !insert;
	Require(typed.Changed);
	Consume(typed.Instance);

	CompileRequest request;
	request.Display = Display();
	Consume(text.World.List.Rebuild(*text.World.Data, request));
	Consume(text.World.List.Commands().Commands.size());
}

BENCH("UI · 1,000,000-item virtual source, 64 published rows", 1) {
	VirtualScenario &virtuals = VirtualRows();
	CompileRequest request;
	request.Display = Display();
	Consume(virtuals.World.List.Rebuild(*virtuals.World.Data, request));
	Consume(virtuals.World.List.Commands().Commands.size());
}

BENCH("UI · 1,000 real rows", 1) {
	Scenario &rows = RealRows();
	CompileRequest request;
	request.Display = Display();
	Consume(rows.List.Rebuild(*rows.Data, request));
	Consume(rows.List.Commands().Commands.size());
}

BENCH("UI · 1,000,000-item virtual rapid mixed-height scroll, 64 rows", 1) {
	VirtualScenario &virtuals = VirtualRows();
	Scrolling *scrolling = virtuals.World.Data->GetMutable<Scrolling>(virtuals.List);
	// The source is a million entries long but only the published page is ever
	// materialized. Moving within it exercises culling and retained compilation.
	Require(scrolling).CanvasPosition.Y = std::fmod(scrolling->CanvasPosition.Y + 137.0f, 960.0f);

	CompileRequest request;
	request.Display = Display();
	Consume(virtuals.World.List.Rebuild(*virtuals.World.Data, request));
	Consume(virtuals.World.List.Commands().Commands.size());
}

BENCH("UI · 1 localized label across 4 locales including RTL", 1) {
	LocaleScenario &locales = Locales();
	static size_t locale = 0;
	static constexpr std::array<std::string_view, 4> NAMES{"en", "fr", "ja", "ar"};
	static constexpr std::array<float, 4> TEXT_SCALES{1.0f, 1.0f, 1.1f, 1.0f};

	CompileRequest request;
	request.Display = Display();
	request.Display.TextScale = TEXT_SCALES[locale];
	request.Catalogue = &locales.Catalogue;
	request.Locale = std::string(NAMES[locale]);
	locale = (locale + 1) % NAMES.size();

	Consume(locales.World.List.Rebuild(*locales.World.Data, request));
	Consume(locales.World.List.Commands().Commands.size());
}

BENCH("UI · 24-label display, DPI, safe-area, and accessibility profile changes", 1) {
	HudScenario &hud = Hud();
	static size_t profile = 0;
	static constexpr std::array<Screen, 3> PROFILES{
		Screen{
			.Width = 1920.0f,
			.Height = 1080.0f,
			.DevicePixelRatio = 1.0f,
			.SafeArea = {},
			.Occluded = {},
		},
		Screen{
			.Width = 1170.0f,
			.Height = 2532.0f,
			.FramebufferScale = 3.0f,
			.DevicePixelRatio = 3.0f,
			.Orientation = DisplayOrientation::Portrait,
			.SafeArea = DisplayInsets{0.0f, 47.0f, 0.0f, 34.0f},
			.Occluded = {},
			.TextScale = 1.15f,
		},
		Screen{
			.Width = 1280.0f,
			.Height = 720.0f,
			.DevicePixelRatio = 1.0f,
			.SafeArea = DisplayInsets{24.0f, 0.0f, 24.0f, 0.0f},
			.Occluded = {},
			.TextScale = 1.3f,
			.InterfaceScale = 0.9f,
		},
	};

	CompileRequest request;
	request.Display = PROFILES[profile];
	profile = (profile + 1) % PROFILES.size();
	Consume(hud.World.List.Rebuild(*hud.World.Data, request));
	Consume(hud.World.List.Commands().Commands.size());
}

BENCH("UI · dense keyboard and gamepad navigation, 144 controls", 1) {
	NavigationScenario &navigation = Navigation();
	static size_t direction = 0;
	static constexpr std::array<SelectionMove, 4> MOVES{
		SelectionMove::Right, SelectionMove::Down, SelectionMove::Left, SelectionMove::Up
	};
	const bool moved = SelectNext(*navigation.World.Data, navigation.World.List.Commands(), MOVES[direction]);
	direction = (direction + 1) % MOVES.size();
	Consume(moved);
	Consume(GuiServiceOf(*navigation.World.Data));
}

BENCH("UI · 128 spatial collectors at the visibility boundary", 1) {
	SpatialScenario &spatial = SpatialBoundary();
	static size_t collector = 0;
	SpatialCanvas *canvas = spatial.World.Data->GetMutable<SpatialCanvas>(spatial.Collectors[collector]);
	Require(canvas).Visible = !canvas->Visible;
	collector = (collector + 1) % spatial.Collectors.size();

	CompileRequest request;
	request.Display = Display();
	Consume(spatial.World.List.Rebuild(*spatial.World.Data, request));
	Consume(spatial.World.List.Commands().Commands.size());
}

BENCH("UI · compiler cache with 7 unchanged and 1 EveryFrame ViewportFrame", 1) {
	Scenario &viewports = ViewportFrames();
	CompileRequest request;
	request.Display = Display();
	// The shared compiler retains these commands. Target scheduling and scene
	// rendering are a renderer-only measurement because gui has no device.
	Consume(viewports.List.Rebuild(*viewports.Data, request));
	Consume(viewports.List.Commands().Commands.size());
}
