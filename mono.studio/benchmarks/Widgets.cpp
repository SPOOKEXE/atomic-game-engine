// What the editor's panels cost to build, per frame, on the CPU.
//
// **This is the half of an editor's frame nobody was measuring.** The frame
// graph showed a wide blank between the simulation and `Renderer::Render` and
// the panel's own `unmarked` figure was already counting it; profiling it named
// `build interface` as 0.14 ms of a 0.29 ms frame, with the explorer alone at
// 0.397 ms on its worst frame. An editor is a program somebody has open all day,
// so a panel that costs more than the world it is showing is worth a number
// rather than an opinion.
//
// ## What this measures, and what it deliberately does not
//
// **Its own imgui context, with no backends and no device.** `ui::Interface`
// refuses to start without an initialised renderer - reasonably, since it owns
// the SDL and GPU backends - and a benchmark that needed a GPU would measure the
// driver on whichever machine ran it, and would not run at all on a build box.
// So this creates a bare context, sets a display size and a default font, and
// submits widgets into it. That is precisely the cost `build interface` is: the
// layout, the id stack, the draw-list building. What happens after
// `ImGui::Render` is the renderer's and is measured, where it can be, by
// `bench_render`.
//
// **The panels' own functions are `Editor` members and are not called here.**
// An `Editor` needs a window, a device and a universe. What is submitted below
// is the same *shape* - the same widgets in the same order per row, taken from
// `Explorer.cpp` and `Properties.cpp` - so a change to how many widgets a row
// costs shows up here. A change to which rows are submitted at all does not, and
// that is the one thing to keep in mind when reading these numbers.
//
// **The store walk is the real thing.** `EachRoot`, `EachChild`,
// `InstanceNameOf` and `ClassOf` are engine calls with no imgui in them, so
// those cases run against a real `Store` built the way a scene is, and measure
// exactly what the explorer does.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/world/Universe.hpp>

#include <imgui.h>
#include <string>
#include <studio/Widgets.hpp>
#include <vector>

TEST_SUITE_ID("studio.bench.widgets")

using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::testing::Consume;

namespace widget_bench {

	// A bare imgui context, built once and reused.
	//
	// **No backends, so `NewFrame` has to be given what a platform backend would
	// have supplied** - a display size and a built font atlas - which is the same
	// arrangement `ui::Interface` uses for a headless run and for the same
	// reason: a zero-sized display clips every panel to nothing, and `NewFrame`
	// asserts on an atlas nothing built.
	struct Context {
		Context() {
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1600.0f, 900.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;

			// No ini file. A benchmark that read one would measure a different
			// layout on every machine, and would write one into whatever
			// directory it was run from.
			io.IniFilename = nullptr;

			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext();
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;
	};

	Context &Live() {
		static Context context;
		return context;
	}

	// One frame's worth of submission, wrapped so every case is measured the
	// same way. The `Render` is included because building the draw lists is part
	// of what a frame pays.
	template <class Body> void Frame(Body &&body) {
		ImGui::NewFrame();

		// A window that is always open and always the same size, so the cases
		// differ by what is inside them and by nothing else.
		ImGui::SetNextWindowSize(ImVec2(420.0f, 800.0f));
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		if (ImGui::Begin("bench")) {
			body();
		}
		ImGui::End();

		ImGui::Render();
	}

	// A world shaped like the example scenes: one root with `count` children,
	// which is what `SkyGrid.luau` and `Rings.luau` both build.
	//
	// **Flat rather than deep, because that is the shape that hurts.** A tree
	// with a hundred levels is a hundred rows; a root with two hundred children
	// is two hundred rows, all of them submitted, and it is the case an author
	// actually has open.
	struct Scene {
		Store World{"widget_bench"};
		Entity Root;
		std::vector<Entity> Children;

		explicit Scene(size_t count) {
			engine::scene::RegisterSceneClasses();

			const auto partClass = Classes::Find(engine::core::Name("Part"));
			Root = World.CreateInstance(partClass, "Workspace");

			Children.reserve(count);
			for (size_t index = 0; index < count; index++) {
				const Entity child = World.CreateInstance(partClass, "Block" + std::to_string(index));
				World.SetParent(child, Root);
				Children.push_back(child);
			}
		}
	};

	Scene &SceneOf(size_t count) {
		static Scene scene(count);
		return scene;
	}
}

using widget_bench::Frame;
using widget_bench::Live;
using widget_bench::SceneOf;

// --- the explorer -----------------------------------------------------------

BENCH("explorer walk · 200 instances", 2000) {
	// **The store half, exactly as the explorer does it.** No imgui at all: this
	// is `EachRoot`, then per row the name, the class and the has-children
	// probe. If this is a large fraction of the panel's cost then the fix is in
	// the engine calls rather than in how many widgets a row submits.
	Live();
	widget_bench::Scene &scene = SceneOf(200);

	for (int pass = 0; pass < 2000; pass++) {
		size_t rows = 0;

		scene.World.EachRoot([&](Entity root) {
			Consume(scene.World.InstanceNameOf(root));
			Consume(scene.World.ClassOf(root));
			rows++;
		});

		for (const Entity child : scene.Children) {
			Consume(scene.World.Alive(child));
			Consume(scene.World.InstanceNameOf(child));
			Consume(scene.World.ClassOf(child));
			rows++;
		}

		Consume(rows);
	}
}

BENCH("explorer has-children probe · 200 instances", 2000) {
	// **The one call the explorer makes per row that is not O(1).** A row asks
	// "does this have children" so it can decide whether to draw an arrow, and
	// the only way to ask used to be `EachChild`, which walks the whole sibling
	// list. On a leaf that is free; on the root of a two-hundred-part scene it
	// is two hundred steps to answer a question the first child settles.
	Live();
	widget_bench::Scene &scene = SceneOf(200);

	for (int pass = 0; pass < 2000; pass++) {
		bool any = false;
		scene.World.EachChild(scene.Root, [&](Entity) { any = true; });
		Consume(any);
	}
}

BENCH("explorer tree · 200 rows", 500) {
	// The widgets one row of the explorer submits, in the order `DrawTreeNode`
	// submits them: the id, the node, the four "was the last item" questions,
	// the two drag-and-drop scopes, and the dimmed class name beside it.
	Live();

	for (int pass = 0; pass < 500; pass++) {
		Frame([] {
			if (ImGui::TreeNodeEx("Workspace", ImGuiTreeNodeFlags_DefaultOpen)) {
				for (int row = 0; row < 200; row++) {
					ImGui::PushID(row);

					constexpr ImGuiTreeNodeFlags FLAGS =
						ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
						ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;

					ImGui::TreeNodeEx("##node", FLAGS, "Block%d", row);

					Consume(ImGui::IsItemToggledOpen());
					Consume(ImGui::IsItemHovered());
					Consume(ImGui::IsItemClicked(ImGuiMouseButton_Left));
					Consume(ImGui::IsItemClicked(ImGuiMouseButton_Right));

					if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
						ImGui::EndDragDropSource();
					}
					if (ImGui::BeginDragDropTarget()) {
						ImGui::EndDragDropTarget();
					}

					ImGui::SameLine();
					ImGui::TextUnformatted("Part");

					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		});
	}
}

BENCH("explorer filter · 200 instances", 2000) {
	// What a typed filter costs over the same scene. `FuzzyMatch` is a
	// subsequence scan per name, run on every root every frame the box is not
	// empty.
	Live();
	widget_bench::Scene &scene = SceneOf(200);

	for (int pass = 0; pass < 2000; pass++) {
		int score = 0;
		size_t hits = 0;
		for (const Entity child : scene.Children) {
			const engine::core::Name name = scene.World.InstanceNameOf(child);
			if (name.IsValid() && studio::FuzzyMatch("bl", name.Text(), score)) {
				hits++;
			}
		}
		Consume(hits);
	}
}

// --- the other panels -------------------------------------------------------

BENCH("properties grid · 24 rows", 2000) {
	// The properties panel's shape: a two-column table, a label and an editor
	// per row. It reads every property of the selection every frame, because
	// imgui is immediate mode and there is no other way to draw a value.
	Live();

	for (int pass = 0; pass < 2000; pass++) {
		Frame([] {
			if (ImGui::BeginTable("##props", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
				for (int row = 0; row < 24; row++) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted("PropertyName");

					ImGui::TableSetColumnIndex(1);
					ImGui::PushID(row);
					ImGui::SetNextItemWidth(-1.0f);

					float value = static_cast<float>(row);
					ImGui::DragFloat("##value", &value, 0.01f);

					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		});
	}
}

BENCH("scrolling table · 200 rows", 500) {
	// The Worlds panel, the frame graph's span list and the output panel are all
	// this: a scrolling table with a frozen header. **imgui clips a table's rows
	// for you** - rows outside the scroll region are skipped - which is exactly
	// the property the explorer's tree does not have, and the reason this case
	// sits beside the tree one.
	Live();

	for (int pass = 0; pass < 500; pass++) {
		Frame([] {
			constexpr ImGuiTableFlags FLAGS =
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;

			if (ImGui::BeginTable("##rows", 4, FLAGS, ImVec2(0.0f, 400.0f))) {
				ImGui::TableSetupColumn("span");
				ImGui::TableSetupColumn("busy");
				ImGui::TableSetupColumn("idle");
				ImGui::TableSetupColumn("share");
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();

				for (int row = 0; row < 200; row++) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted("some.span.name");
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.2f", 0.01);
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted("-");
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.1f%%", 2.5);
				}
				ImGui::EndTable();
			}
		});
	}
}

BENCH("empty frame", 5000) {
	// The floor. Every case above pays this, so a case that is not far above it
	// is a case measuring `NewFrame` and `Render` rather than its own widgets.
	Live();

	for (int pass = 0; pass < 5000; pass++) {
		Frame([] {});
	}
}

BENCH("explorer tree · 200 rows, 20 on screen", 500) {
	// **The case the panel is actually in.** An explorer is a few hundred rows
	// in a pane that shows twenty, so most of what a naive tree submits is work
	// for rows nobody can see - and unlike a table, a tree cannot be clipped by
	// row index because the visible set depends on what is expanded.
	//
	// This is the same tree as above inside a scroll region, with the per-row
	// extras skipped when `IsItemVisible` says the row is outside it - which is
	// what `DrawTreeNode` does. The difference between this and the case above
	// is what that guard is worth.
	Live();

	for (int pass = 0; pass < 500; pass++) {
		Frame([] {
			if (ImGui::BeginChild("##scroll", ImVec2(0.0f, 400.0f), ImGuiChildFlags_Borders)) {
				if (ImGui::TreeNodeEx("Workspace", ImGuiTreeNodeFlags_DefaultOpen)) {
					for (int row = 0; row < 200; row++) {
						ImGui::PushID(row);

						constexpr ImGuiTreeNodeFlags FLAGS =
							ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
							ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;

						ImGui::TreeNodeEx("##node", FLAGS, "Block%d", row);

						if (!ImGui::IsItemVisible()) {
							ImGui::PopID();
							continue;
						}

						Consume(ImGui::IsItemToggledOpen());
						Consume(ImGui::IsItemHovered());
						Consume(ImGui::IsItemClicked(ImGuiMouseButton_Left));
						Consume(ImGui::IsItemClicked(ImGuiMouseButton_Right));

						if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
							ImGui::EndDragDropSource();
						}
						if (ImGui::BeginDragDropTarget()) {
							ImGui::EndDragDropTarget();
						}

						ImGui::SameLine();
						ImGui::TextUnformatted("Part");

						ImGui::PopID();
					}
					ImGui::TreePop();
				}
			}
			ImGui::EndChild();
		});
	}
}

BENCH("worlds panel · 4 rows", 2000) {
	// **The shape that turned out to be the editor's most expensive panel per
	// row.** The frame graph put `worlds` at 0.025 ms against the explorer's
	// 0.009 for a quarter of the rows - so the cost is per row rather than per
	// scene, and this is the row: a span-all-columns selectable, a context-menu
	// probe, the name, the state, the count, and a button that opens the same
	// menu the probe would have.
	//
	// **Two popup calls per row is the part worth watching.**
	// `BeginPopupContextItem` and `BeginPopup` both hash an id and test the
	// popup stack, and both are made whether or not any menu is open - which,
	// for a panel nobody is right-clicking, is every frame of every run.
	Live();

	for (int pass = 0; pass < 2000; pass++) {
		Frame([] {
			constexpr ImGuiTableFlags FLAGS =
				ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;

			if (ImGui::BeginTable("##worlds", 4, FLAGS)) {
				ImGui::TableSetupColumn("scene");
				ImGui::TableSetupColumn("state");
				ImGui::TableSetupColumn("objects");
				ImGui::TableSetupColumn("");
				ImGui::TableHeadersRow();

				for (int row = 0; row < 4; row++) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(row);

					Consume(
						ImGui::Selectable(
							"##row",
							row == 0,
							ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
						)
					);

					const bool rowMenu = ImGui::BeginPopupContextItem("##world-menu");

					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextUnformatted("SkyGrid");

					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted("active");

					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%zu", size_t{196});

					ImGui::TableSetColumnIndex(3);
					if (ImGui::SmallButton("...")) {
						ImGui::OpenPopup("##world-menu");
					}

					if (rowMenu || ImGui::BeginPopup("##world-menu")) {
						ImGui::EndPopup();
					}

					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		});
	}
}

BENCH("worlds panel · engine calls for 4 rows", 5000) {
	// **The other half of the Worlds panel, and the half the numbers pointed
	// at.** The widget shape above is 3.9 us; the panel measured 24 us in a real
	// frame. The difference is not imgui, it is what the row asks the universe
	// for - a name, a state, whether it is remote, and the list of worlds
	// itself.
	//
	// `Universe::Worlds()` is the one to watch: it builds and returns a
	// `std::vector` every call, and the panels call it several times a frame
	// between them.
	Live();

	engine::world::Universe universe;
	for (int index = 0; index < 4; index++) {
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name("World" + std::to_string(index));
		universe.Create(settings);
	}

	for (int pass = 0; pass < 5000; pass++) {
		for (const engine::world::WorldId world : universe.Worlds()) {
			Consume(universe.NameOf(world));
			Consume(universe.StateOf(world));
			Consume(universe.IsRemote(world));
		}
	}
}
