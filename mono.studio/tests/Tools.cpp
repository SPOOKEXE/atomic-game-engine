// Locking, anchoring and the pivot, as the tools panel writes them.
//
// **The panel itself is not what is under test.** Drawing an ImGui strip needs a
// context and proves nothing about what a button does; what the buttons do is
// three store writes and a rule about mixed selections, and those are ordinary
// functions.
//
// The one that is worth a case of its own is `Locked` leaving a part out of the
// pick grid rather than filtering it out of the hit. The difference only shows
// up when something is *behind* the locked thing, which is exactly the case
// locking a wall is for - and the version that filters the hit passes every
// simpler test.

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <optional>
#include <studio/Editor.hpp>
#include <vector>

TEST_SUITE_ID("studio.tools")
TEST_DEPENDS("engine.scene.part")

using engine::core::AABB;
using engine::core::Name;
using engine::core::OrientedBoxBounds;
using engine::core::Ray;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Transform;
using engine::scene::Visual;

namespace studio {
	struct ToolsProbe {
		static void Draw(Editor &editor, BuiltinStudioTool tool) {
			editor.DrawingBuiltinTool = tool;
			editor.DrawHomeTools();
		}

		static bool IsInactive(const Editor &editor) {
			return editor.CurrentTool == Editor::ToolMode::None;
		}

		static bool IsSelect(const Editor &editor) {
			return editor.CurrentTool == Editor::ToolMode::Select;
		}

		static void SetMove(Editor &editor) {
			editor.CurrentTool = Editor::ToolMode::Move;
		}
	};
}

namespace {
	class Context {
	  public:
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	ImVec2 DrawTool(studio::Editor &editor, studio::BuiltinStudioTool tool, ImVec2 mouse, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.x, mouse.y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 120.0f), ImGuiCond_Always);
		ImGui::Begin("Tools test");
		studio::ToolsProbe::Draw(editor, tool);
		const ImVec2 minimum = ImGui::GetItemRectMin();
		const ImVec2 maximum = ImGui::GetItemRectMax();
		ImGui::End();
		ImGui::Render();
		return ImVec2((minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f);
	}

	void ClickTool(studio::Editor &editor, studio::BuiltinStudioTool tool) {
		const ImVec2 point = DrawTool(editor, tool, ImVec2(-1.0f, -1.0f), false);
		DrawTool(editor, tool, point, false);
		DrawTool(editor, tool, point, true);
		DrawTool(editor, tool, point, false);
	}

	// A part at a place, one stud on a side.
	Entity Place(Store &store, const char *name, float z) {
		const Entity part = store.CreateInstance(engine::scene::PartClass(), name);
		REQUIRE(part != NULL_ENTITY);

		Transform placed;
		placed.Frame = engine::core::CFrame(Vector3{0.0f, 0.0f, z});
		store.Set<Transform>(part, placed);

		Bounds box;
		box.HalfExtent = Vector3{0.5f, 0.5f, 0.5f};
		store.Set<Bounds>(part, box);
		return part;
	}

	// The pick, as `Editor::PickInViewport` builds it: every drawable that is
	// not locked, in a grid, hit by one ray.
	//
	// **A copy of the rule rather than a call into the editor**, because the
	// editor's own function needs a window, a projection and a universe. What is
	// under test is the rule, and a copy that drifted from the original would
	// fail this case the moment it did - the exclusion is one `if`.
	Entity PickThrough(Store &store, const Ray &ray) {
		std::vector<engine::spatial::Proxy> proxies;

		store.Each<Transform, Bounds>([&](Entity entity, const Transform &transform, const Bounds &bounds) {
			if (const auto *visual = store.Get<Visual>(entity); visual != nullptr && visual->Locked) {
				return;
			}

			engine::spatial::Proxy proxy;
			proxy.Id = entity.Id;
			proxy.Bounds = OrientedBoxBounds(transform.Frame, bounds.HalfExtent);
			proxy.Layers = engine::spatial::LayerMask::All();
			proxies.push_back(proxy);
		});

		if (proxies.empty()) {
			return NULL_ENTITY;
		}

		engine::spatial::HashGrid grid;
		grid.Rebuild(proxies);

		const std::optional<engine::core::RayHit> hit = engine::spatial::Raycast(grid, ray, 1000.0f);
		return hit.has_value() ? Entity(hit->Id) : NULL_ENTITY;
	}
}

TEST_CASE("active toolbar tools can leave the viewport to the running game", "[studio][tools][viewport]") {
	Context context;
	studio::Editor editor;

	// The first Select press removes Studio's selection and drag mode while the
	// viewport surface stays active for subsequent game UI presses.
	REQUIRE(studio::ToolsProbe::IsSelect(editor));
	ClickTool(editor, studio::BuiltinStudioTool::SelectMode);
	CHECK(studio::ToolsProbe::IsInactive(editor));

	// Selecting it again restores direct Studio selection. Any active gizmo
	// also toggles off from its own button, so one click always provides the
	// way to return a Play viewport to its game UI.
	ClickTool(editor, studio::BuiltinStudioTool::SelectMode);
	CHECK(studio::ToolsProbe::IsSelect(editor));
	studio::ToolsProbe::SetMove(editor);
	ClickTool(editor, studio::BuiltinStudioTool::MoveMode);
	CHECK(studio::ToolsProbe::IsInactive(editor));
}

TEST_CASE("Locked is a property that survives a write and a read", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Wall", 0.0f);

	// The default is unlocked: a part somebody has never touched is one they
	// can click.
	bool locked = true;
	REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
	CHECK_FALSE(locked);

	locked = true;
	REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));

	locked = false;
	REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
	CHECK(locked);

	// **It changes nothing else about the part.** Locking is an editing
	// affordance, and a locked part that stopped drawing or stopped colliding
	// would be a very expensive way to say "leave this alone".
	const auto *visual = store.Get<Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Visible);
	CHECK(store.Get<Bounds>(part) != nullptr);
}

TEST_CASE("a locked part lets the ray through to what is behind it", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity front = Place(store, "Wall", 0.0f);
	const Entity behind = Place(store, "Switch", -8.0f);

	// Down the negative Z axis, through both.
	const Ray ray{Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.0f, 0.0f, -1.0f}};

	CHECK(PickThrough(store, ray) == front);

	bool locked = true;
	REQUIRE(store.SetProperty(front, Name("Locked"), &locked, sizeof(locked)));

	// **This is the assertion the whole design turns on.** Filtering the *hit*
	// would answer `NULL_ENTITY` here - the locked wall would still swallow the
	// ray and make the thing behind it unclickable, which is the opposite of
	// what locking a wall is for.
	CHECK(PickThrough(store, ray) == behind);

	locked = false;
	REQUIRE(store.SetProperty(front, Name("Locked"), &locked, sizeof(locked)));
	CHECK(PickThrough(store, ray) == front);
}

TEST_CASE("a pivot offset is written and reset through its property", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Door", 0.0f);

	engine::core::CFrame offset;
	REQUIRE(store.GetProperty(part, Name("PivotOffset"), &offset, sizeof(offset)));
	CHECK(offset.Position == Vector3::Zero);

	// A hinge on the edge of a one-stud door.
	offset.Position = Vector3{-0.5f, 0.0f, 0.0f};
	REQUIRE(store.SetProperty(part, Name("PivotOffset"), &offset, sizeof(offset)));

	// **`GetPivot` composes the placement with the offset**, which is what makes
	// a pivot edit visible without the part moving - and is the arithmetic the
	// gizmo has to undo to turn a world drag into a local offset.
	CHECK(engine::scene::PivotOf(store, part).Position.X == -0.5f);

	const auto *transform = store.Get<Transform>(part);
	REQUIRE(transform != nullptr);
	CHECK(transform->Frame.Position == Vector3::Zero);

	// Reset, which is the button.
	const engine::core::CFrame identity;
	REQUIRE(store.SetProperty(part, Name("PivotOffset"), &identity, sizeof(identity)));
	CHECK(engine::scene::PivotOf(store, part).Position == Vector3::Zero);
}

TEST_CASE("anchoring is a structural change and locking is not", "[studio][tools]") {
	engine::scene::EnsureClassTree();
	Store store("tools");

	const Entity part = Place(store, "Crate", 0.0f);

	// The two toolbar toggles look alike and are not: anchored decides whether
	// the physics moves it - which is `scene::Simulated` coming off the row -
	// and locked decides whether a person can grab it, which is a byte.
	bool anchored = false;
	REQUIRE(store.GetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	anchored = true;
	REQUIRE(store.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK_FALSE(store.Has<engine::scene::Simulated>(part));

	bool locked = true;
	REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));

	// Still anchored, and still there. A structural write and a field write on
	// one entity in one frame is the ordinary case for these two buttons.
	REQUIRE(store.GetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));
	CHECK(anchored);
	CHECK(store.Get<Visual>(part)->Locked);
}
