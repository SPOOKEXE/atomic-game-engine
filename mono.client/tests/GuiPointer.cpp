#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.gui_pointer")

namespace {
	using engine::core::Name;
	using engine::core::Vector2;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	struct PointerWorld {
		Store World{"client.gui-pointer"};
		engine::gui::Router Router;
		engine::gui::Screen Display{.Width = 1600.0f, .Height = 900.0f};
		engine::gui::DrawList List;
		Entity Screen;
		Entity Frame;
		Entity Surface;

		PointerWorld() {
			engine::gui::RegisterGuiClasses();
			engine::scene::EnsureClassTree();
			const Entity camera = World.CreateInstance(engine::scene::PartClass(), "Camera");
			World.Set(camera, engine::scene::Transform{});
			World.Set(camera, engine::scene::Camera{});
			World.SetResource(engine::scene::ActiveCamera{camera});

			Screen = World.CreateInstance(engine::gui::GuiClass("ScreenGui"), "Screen");
			Frame = World.CreateInstance(engine::gui::GuiClass("ScrollingFrame"), "List");
			World.SetParent(Frame, Screen);
			World.Set(Frame, engine::gui::Scrolling{});
			engine::gui::ScrollState state;
			state.CanvasSize = {200.0f, 400.0f};
			state.WindowSize = {200.0f, 200.0f};
			World.Set(Frame, state);
			engine::gui::Resolved resolved;
			resolved.AbsolutePosition = {700.0f, 350.0f};
			resolved.AbsoluteSize = {200.0f, 200.0f};
			resolved.Clip = engine::core::Rect{700.0f, 350.0f, 900.0f, 550.0f};
			resolved.Rendered = true;
			World.Set(Frame, resolved);

			Surface = World.CreateInstance(engine::gui::GuiClass("SurfaceGui"), "Surface");
			engine::gui::SpatialCanvas spatial;
			spatial.Origin = {-2.0f, 1.0f, -10.0f};
			spatial.AxisX = {4.0f, 0.0f, 0.0f};
			spatial.AxisY = {0.0f, -2.0f, 0.0f};
			spatial.Normal = {0.0f, 0.0f, 1.0f};
			spatial.Size = {400.0f, 200.0f};
			spatial.Interactive = true;
			World.Set(Surface, spatial);
			const Entity button = World.CreateInstance(engine::gui::GuiClass("TextButton"), "Button");
			World.SetParent(button, Surface);

			engine::gui::DrawCommand command;
			command.Source = button;
			command.Collector = Surface;
			command.Spatial = true;
			command.Bounds = engine::core::Rect{0.0f, 0.0f, 400.0f, 200.0f};
			command.Clip = command.Bounds;
			List.Commands.push_back(command);
			command.Source = Frame;
			command.Collector = Screen;
			command.Spatial = false;
			command.Bounds = engine::core::Rect{700.0f, 350.0f, 900.0f, 550.0f};
			command.Clip = command.Bounds;
			List.Commands.push_back(command);
		}
	};
}

TEST_CASE("screen scrolling takes pointer before a spatial button", "[client][gui][input]") {
	PointerWorld world;
	engine::gui::Pointer pointer;
	pointer.Position = {800.0f, 450.0f};
	pointer.Down = true;
	pointer.ScreenOnly = true;
	const auto routed =
		engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(routed.ScreenOnly);
	CHECK(routed.Collector == engine::ecs::NULL_ENTITY);
	(void)world.Router.Update(world.World, world.List, routed);
	REQUIRE(world.Router.CanvasHeld() == world.Frame);
	CHECK(world.Router.Pressed() == engine::ecs::NULL_ENTITY);
	pointer.Position = {800.0f, 330.0f};
	const auto moved =
		engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(moved.ScreenOnly);
	(void)world.Router.Update(world.World, world.List, moved);
	CHECK(world.World.Get<engine::gui::Scrolling>(world.Frame)->CanvasPosition.Y == Catch::Approx(120.0f));
}

TEST_CASE("screen wheel reaches a scrolling frame before a spatial button", "[client][gui][input]") {
	PointerWorld world;
	engine::gui::Pointer pointer;
	pointer.Position = {800.0f, 450.0f};
	pointer.Wheel = -1.0f;
	pointer.ScreenOnly = true;
	const auto routed =
		engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(routed.ScreenOnly);
	(void)world.Router.Update(world.World, world.List, routed);
	CHECK(world.World.Get<engine::gui::Scrolling>(world.Frame)->CanvasPosition.Y > 0.0f);
}

TEST_CASE("decorative and non-scrollable screen GUI leave spatial input available", "[client][gui][input]") {
	PointerWorld world;
	SECTION("decorative frame") {
		world.World.Remove<engine::gui::Scrolling>(world.Frame);
	}
	SECTION("content fits") {
		auto state = *world.World.Get<engine::gui::ScrollState>(world.Frame);
		state.CanvasSize = state.WindowSize;
		world.World.Set(world.Frame, state);
	}
	engine::gui::Pointer pointer;
	pointer.Position = {800.0f, 450.0f};
	pointer.Down = true;
	pointer.ScreenOnly = true;
	const auto routed =
		engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK_FALSE(routed.ScreenOnly);
	CHECK(routed.Collector == world.Surface);
	CHECK(routed.Position.X == Catch::Approx(200.0f).margin(0.1f));
	CHECK(routed.Position.Y == Catch::Approx(100.0f).margin(0.1f));
}

TEST_CASE("screen controls do not steal a captured spatial press", "[client][gui][input]") {
	PointerWorld world;
	auto scrolling = *world.World.Get<engine::gui::Scrolling>(world.Frame);
	scrolling.Enabled = false;
	world.World.Set(world.Frame, scrolling);
	engine::gui::Pointer pointer;
	pointer.Position = {800.0f, 450.0f};
	pointer.Down = true;
	pointer.ScreenOnly = true;
	auto routed =
		engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	REQUIRE(routed.Collector == world.Surface);
	(void)world.Router.Update(world.World, world.List, routed);
	REQUIRE(world.Router.Pressed() != engine::ecs::NULL_ENTITY);
	scrolling.Enabled = true;
	world.World.Set(world.Frame, scrolling);
	routed = engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(routed.Collector == world.Surface);
	CHECK_FALSE(routed.ScreenOnly);

	// Move beyond the surface onto a screen scroller while the spatial press is held.
	auto resolved = *world.World.Get<engine::gui::Resolved>(world.Frame);
	resolved.AbsolutePosition = {1300.0f, 350.0f};
	resolved.Clip = engine::core::Rect{1300.0f, 350.0f, 1500.0f, 550.0f};
	world.World.Set(world.Frame, resolved);
	world.List.Commands.back().Bounds = resolved.Clip;
	world.List.Commands.back().Clip = resolved.Clip;
	pointer.Position = {1400.0f, 450.0f};
	routed = engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(routed.Collector == world.Surface);
	CHECK_FALSE(routed.ScreenOnly);
	CHECK(routed.Position.X > 400.0f);
	CHECK_FALSE(routed.Cancelled);

	// A lost surface cannot receive window-space coordinates as canvas pixels.
	world.World.Remove<engine::gui::SpatialCanvas>(world.Surface);
	routed = engine::render::RouteGuiPointer(world.World, world.List, world.Display, world.Router, pointer);
	CHECK(routed.Cancelled);
	CHECK_FALSE(routed.Inside);
}
