// The Find predicate, which is the half of the panel that can be silently
// wrong.
//
// **A filter that quietly matches nothing looks exactly like a scene that
// contains nothing**, and neither the panel nor the person reading it can tell
// the two apart. So `MatchesQuery` is a free function over a `Store` and this
// file exercises it directly; the widgets around it need a window and are
// covered by running the editor.
//
// The property this suite is really pinning: **the predicate names no
// property.** Every case below asks about `Transparency`, `Anchored` or
// `Material` without the matcher having heard of any of them, because they
// arrive through `PropertyDescriptor` like everything else. A change that made
// Find special-case a type would still pass the cases that use that type and
// fail the ones that do not.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.find")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using studio::FindQuery;
using studio::MatchesQuery;

namespace studio {
	struct ToolsProbe {
		static void SearchPrepare(Editor &editor) {
			engine::scene::RegisterSceneClasses();
			editor.Universe = std::make_unique<engine::world::Universe>();
			editor.Active = editor.Universe->Create({.Name = Name("search-cache-world")});
			editor.ExplorerWorld = editor.Active;
		}
		static engine::world::Universe &SearchUniverse(Editor &editor) {
			return *editor.Universe;
		}
		static engine::world::WorldId SearchWorld(Editor &editor) {
			return editor.Active;
		}
		static const std::vector<FindResult> &SearchRun(Editor &editor, FindQuery query) {
			editor.Find = std::move(query);
			editor.RunFind();
			return editor.FindResults;
		}
		static void SearchSelectWorld(Editor &editor, engine::world::WorldId world) {
			editor.ExplorerWorld = world;
		}
		static bool SearchTruncated(const Editor &editor) {
			return editor.FindTruncated;
		}
		static bool SearchInitialise(Editor &editor) {
			engine::ui::InterfaceSettings settings;
			settings.DisplayWidth = 1280;
			settings.DisplayHeight = 720;
			editor.ShowExplorer = true;
			return editor.Interface.Initialise(editor.Renderer, nullptr, settings);
		}
		static ImGuiWindow *SearchChild() {
			auto *parent = ImGui::FindWindowByName("Explorer");
			REQUIRE(parent != nullptr);
			for (auto *child : parent->DC.ChildWindows)
				if (child->ChildId == parent->GetID("##explorer-search-results")) return child;
			FAIL("Explorer search child was not submitted");
			return nullptr;
		}
		static void SearchDraw(Editor &editor, float scroll, bool full = false) {
			const auto *previous = full ? SearchChild() : nullptr;
			const ImVec2 childPos = previous ? previous->Pos : ImVec2{};
			const ImVec2 childSize = previous ? previous->Size : ImVec2{};
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({980, 600}, ImGuiCond_Always);
			if (!full) {
				if (auto *parent = ImGui::FindWindowByName("Explorer")) {
					parent->StateStorage.SetBool(parent->GetID("Property search"), true);
					for (auto *child : parent->DC.ChildWindows)
						if (child->ChildId == parent->GetID("##explorer-search-results"))
							ImGui::SetScrollY(child, scroll);
				}
				editor.DrawExplorer();
			} else {
				ImGui::Begin("Explorer");
				ImGui::SetCursorScreenPos(childPos);
				ImGui::SetNextWindowScroll({0, scroll});
				if (ImGui::BeginChild("##explorer-search-results", childSize)) {
					for (size_t index = 0; index < editor.FindResults.size(); ++index) {
						const auto &result = editor.FindResults[index];
						ImGui::PushID(static_cast<int>(index));
						ImGui::Selectable(
							"##result",
							editor.SelectionWorld == result.World && editor.IsSelected(result.Instance)
						);
						ImGui::SameLine();
						ImGui::Text("%s (%s)", result.Name.c_str(), result.Class.c_str());
						ImGui::PopID();
					}
				}
				ImGui::EndChild();
				ImGui::End();
			}
			editor.Interface.End();
		}
		static bool SearchUniform(const Editor &editor) {
			return editor.FindUniformRows;
		}
		static bool SearchSelected(const Editor &editor, Entity entity) {
			return editor.IsSelected(entity);
		}
		static bool SearchCached(const Editor &editor) {
			return editor.FindFingerprintReady;
		}
	};
}

TEST_CASE(
	"Explorer search validates names and keeps computed resource predicates live", "[studio][find][cache]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-search-cache";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity named;
	Entity workspace;
	universe.Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		workspace = engine::scene::WorkspaceOf(store);
		named = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "search-hit-one");
		REQUIRE(store.SetParent(named, workspace));
	});
	FindQuery query;
	query.Class = "Folder";
	query.Property = "Name";
	query.Value = "search-hit";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	CHECK(studio::ToolsProbe::SearchRun(editor, query).front().Instance == named);
	universe.Enter(world, [&](Store &store) { REQUIRE(store.SetInstanceName(named, "miss")); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	universe.Enter(world, [&](Store &store) {
		store.Destroy(named);
		named = store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "search-hit-new");
		REQUIRE(store.SetParent(named, workspace));
	});
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchRun(editor, query).front().Name == "search-hit-new");
	query.Exact = true;
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query.Value = "search-hit-new";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	query.Exact = false;
	query.Property = "na";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.Remove<engine::ecs::InstanceName>(named); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query = {};
	query.Class = "Folder";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.Destroy(named); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query.Property = "missing-property";
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());
	query = {};
	query.Class = "Workspace";
	query.Property = "MaxSurfaces";
	query.Value = "17";
	query.Exact = true;
	universe.Enter(world, [&](Store &store) { store.SetResource(engine::scene::SurfaceLimit{17}); });
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 1);
	CHECK_FALSE(studio::ToolsProbe::SearchCached(editor));
	universe.Enter(world, [&](Store &store) { store.SetResource(engine::scene::SurfaceLimit{23}); });
	CHECK(studio::ToolsProbe::SearchRun(editor, query).empty());

	// The bound depends on EachEntity order, including a non-instance row
	// after the last allowed result. Keep that observable truncation state.
	engine::world::WorldId bounded;
	bounded = universe.Create({.Name = Name("search-bounded-world")});
	studio::ToolsProbe::SearchSelectWorld(editor, bounded);
	Entity raw;
	std::vector<Entity> ordered;
	universe.Enter(bounded, [&](Store &store) {
		for (size_t index = 0; index < 500; ++index)
			ordered.push_back(store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "bounded"));
	});
	query = {};
	query.Class = "Folder";
	const auto boundedResults = studio::ToolsProbe::SearchRun(editor, query);
	REQUIRE(boundedResults.size() == 500);
	CHECK_FALSE(studio::ToolsProbe::SearchTruncated(editor));
	CHECK(boundedResults.front().Instance == ordered.front());
	CHECK(boundedResults.back().Instance == ordered.back());
	universe.Enter(bounded, [&](Store &store) { raw = store.Create(); });
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 500);
	CHECK(studio::ToolsProbe::SearchTruncated(editor));
	universe.Enter(bounded, [&](Store &store) {
		store.Destroy(raw);
		store.Destroy(ordered.front());
		ordered.erase(ordered.begin());
		ordered.push_back(store.CreateInstance(engine::ecs::Classes::Find(Name("Folder")), "bounded"));
	});
	const auto reordered = studio::ToolsProbe::SearchRun(editor, query);
	std::vector<Entity> expected;
	universe.Enter(bounded, [&](Store &store) {
		store.EachEntity([&](Entity entity) {
			std::string matched;
			if (MatchesQuery(store, entity, query, matched)) expected.push_back(entity);
		});
	});
	REQUIRE(reordered.size() == expected.size());
	for (size_t index = 0; index < expected.size(); ++index)
		CHECK(reordered[index].Instance == expected[index]);
}

TEST_CASE(
	"Explorer search clipping preserves full child pixels and result picking", "[studio][find][input]"
) {
	const auto previous = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-search-parity";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previous};
	studio::Editor editor;
	studio::ToolsProbe::SearchPrepare(editor);
	REQUIRE(studio::ToolsProbe::SearchInitialise(editor));
	auto &universe = studio::ToolsProbe::SearchUniverse(editor);
	const auto world = studio::ToolsProbe::SearchWorld(editor);
	Entity first;
	universe.Enter(world, [&](Store &store) {
		engine::scene::InstallServices(store);
		const auto workspace = engine::scene::WorkspaceOf(store);
		for (size_t index = 0; index < 40; ++index) {
			const auto made = store.CreateInstance(
				engine::ecs::Classes::Find(Name("Folder")), "search-hit-" + std::to_string(index)
			);
			REQUIRE(store.SetParent(made, workspace));
			if (index == 0) first = made;
		}
	});
	FindQuery query;
	query.Class = "Folder";
	query.Property = "Name";
	query.Value = "search-hit";
	REQUIRE(studio::ToolsProbe::SearchRun(editor, query).size() == 40);
	const auto parity = [&](float scroll) {
		for (int warm = 0; warm < 4; ++warm)
			studio::ToolsProbe::SearchDraw(editor, scroll);
		const auto *child = studio::ToolsProbe::SearchChild();
		const auto content = child->ContentSize;
		const auto cursorMaximum = child->DC.CursorMaxPos;
		const auto maximum = child->ScrollMax;
		const std::vector<ImDrawVert> vertices(
			child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end()
		);
		const std::vector<ImDrawIdx> indices(
			child->DrawList->IdxBuffer.begin(), child->DrawList->IdxBuffer.end()
		);
		studio::ToolsProbe::SearchDraw(editor, scroll, true);
		child = studio::ToolsProbe::SearchChild();
		CHECK(child->DC.CursorMaxPos.x == cursorMaximum.x);
		CHECK(child->DC.CursorMaxPos.y == cursorMaximum.y);
		CHECK(child->ContentSize.x == content.x);
		CHECK(child->ContentSize.y == content.y);
		CHECK(child->ScrollMax.y == maximum.y);
		CHECK(
			std::equal(
				vertices.begin(),
				vertices.end(),
				child->DrawList->VtxBuffer.begin(),
				child->DrawList->VtxBuffer.end(),
				[](const auto &a, const auto &b) {
					return a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.uv.x == b.uv.x && a.uv.y == b.uv.y &&
						   a.col == b.col;
				}
			)
		);
		CHECK(
			std::equal(
				indices.begin(),
				indices.end(),
				child->DrawList->IdxBuffer.begin(),
				child->DrawList->IdxBuffer.end()
			)
		);
	};
	CHECK(studio::ToolsProbe::SearchUniform(editor));
	parity(0);
	parity(220);
	parity(100000);
	universe.Enter(world, [&](Store &store) {
		REQUIRE(store.SetInstanceName(first, "search-hit-0\nextra"));
	});
	parity(220);
	CHECK_FALSE(studio::ToolsProbe::SearchUniform(editor));
	parity(0);
	const auto *child = studio::ToolsProbe::SearchChild();
	const ImVec2 point(child->InnerRect.Min.x + 30, child->DC.CursorStartPos.y + ImGui::GetFontSize() * .5f);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(point.x, point.y);
	studio::ToolsProbe::SearchDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	studio::ToolsProbe::SearchDraw(editor, 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	studio::ToolsProbe::SearchDraw(editor, 0);
	CHECK(studio::ToolsProbe::SearchSelected(editor, first));
	universe.Enter(world, [&](Store &store) { REQUIRE(store.SetInstanceName(first, "search-hit-0")); });
	parity(0);
	CHECK(studio::ToolsProbe::SearchUniform(editor));
	universe.Enter(world, [&](Store &store) {
		Entity last;
		store.EachEntity([&](Entity entity) {
			if (store.InstanceNameOf(entity) == Name("search-hit-39")) last = entity;
		});
		REQUIRE(last != NULL_ENTITY);
		REQUIRE(store.SetInstanceName(last, "search-hit-" + std::string(300, 'w')));
	});
	// The widest row is below this viewport but must retain the full extent.
	parity(0);
	ImGui::GetStyle().ItemSpacing.x = 3.5f;
	ImGui::GetStyle().FontScaleMain = 1.25f;
	parity(0);
	parity(220);
}

namespace {
	struct Fixture {
		Store World{"find_test"};

		Fixture() {
			engine::scene::EnsureClassTree();
		}

		Entity Part(const char *name) {
			const Entity made = World.CreateInstance(engine::scene::PartClass(), name);
			REQUIRE(made != NULL_ENTITY);
			return made;
		}
	};

	bool Matches(const Store &store, Entity instance, const FindQuery &query) {
		std::string matched;
		return MatchesQuery(store, instance, query, matched);
	}
}

TEST_CASE("an empty query matches every instance", "[studio][find]") {
	// The identity case, and it decides the shape of the control: an empty
	// field does not filter. If an empty query matched nothing, "show me
	// everything" would need a mode of its own.
	Fixture fixture;
	const Entity part = fixture.Part("Anything");

	CHECK(Matches(fixture.World, part, FindQuery{}));
}

TEST_CASE("the class filter is IsA, not an exact name", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery exact;
	exact.Class = "Part";
	CHECK(Matches(fixture.World, part, exact));

	// **The point of using the class tree rather than a string compare.** A
	// `Part` is a `BasePart`, so searching for the base finds the leaf - which
	// is what somebody means by "every part" and what set inclusion already
	// says.
	FindQuery base;
	base.Class = "BasePart";
	CHECK(Matches(fixture.World, part, base));

	FindQuery unrelated;
	unrelated.Class = "Script";
	CHECK_FALSE(Matches(fixture.World, part, unrelated));
}

TEST_CASE("a class nobody registered matches nothing rather than everything", "[studio][find]") {
	// The direction to fail in. A filter that fell back to "match all" when it
	// did not recognise the class would answer a typo with the whole scene.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Class = "NotAClass";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("the name filter is a case-insensitive substring", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("RedDoorFrame");

	FindQuery middle;
	middle.Name = "door";
	CHECK(Matches(fixture.World, part, middle));

	FindQuery missing;
	missing.Name = "window";
	CHECK_FALSE(Matches(fixture.World, part, missing));
}

TEST_CASE("a property is found by name with no value asked for", "[studio][find]") {
	// **`Transparency` is never written down in the matcher.** It arrives
	// through `PropertiesOf`, which is why a property declared by any module
	// tomorrow is searchable with nothing here changing.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "Transparency";

	std::string matched;
	REQUIRE(MatchesQuery(fixture.World, part, query, matched));

	// And the row says what matched, so a result list is readable without
	// clicking every entry.
	CHECK(matched.find("Transparency") != std::string::npos);
}

TEST_CASE("a property nothing declares matches nothing", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "NoSuchProperty";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("a value is matched through its rendered text", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Booleans, numbers and enums all go through `FormatValue`, so one
	// predicate covers every type - and `Anchored` is a *structural* property
	// on top of that, presence of a tag spelled as a bool, which the matcher
	// also never learns.
	const bool anchored = true;
	REQUIRE(fixture.World.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	FindQuery query;
	query.Property = "Anchored";
	query.Value = "true";
	CHECK(Matches(fixture.World, part, query));

	FindQuery other;
	other.Property = "Anchored";
	other.Value = "false";
	CHECK_FALSE(Matches(fixture.World, part, other));
}

TEST_CASE("exact compares through the type rather than the text", "[studio][find]") {
	// **The case the `exact` checkbox exists for.** A substring match on the
	// rendered text says 0.5 is in "0.55", which is right for browsing and
	// wrong for "which parts are exactly half transparent".
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	const float transparency = 0.55f;
	fixture.World.SetProperty(part, Name("Transparency"), &transparency, sizeof(transparency));

	FindQuery loose;
	loose.Property = "Transparency";
	loose.Value = "0.5";
	CHECK(Matches(fixture.World, part, loose));

	FindQuery exact = loose;
	exact.Exact = true;
	CHECK_FALSE(Matches(fixture.World, part, exact));

	FindQuery right;
	right.Property = "Transparency";
	right.Value = "0.55";
	right.Exact = true;
	CHECK(Matches(fixture.World, part, right));
}

TEST_CASE("filters combine with and, not or", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Right class, wrong name: not a match. An `or` would return it and make
	// every additional filter widen the search instead of narrowing it.
	FindQuery query;
	query.Class = "Part";
	query.Name = "NotThisOne";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("an entity that is not an instance never matches", "[studio][find]") {
	// A module's own storage row has no class, and the explorer does not show
	// it. Find must agree, or the result list contains things nothing can
	// select.
	Fixture fixture;
	const Entity bare = fixture.World.Create();
	REQUIRE(bare != NULL_ENTITY);

	CHECK_FALSE(Matches(fixture.World, bare, FindQuery{}));
}
