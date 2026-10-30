// The pixel canvas of a collector that lives in the world.
//
// **`D00022`, and the same split as the adornments beside it.** A `SurfaceGui`
// sized in pixels-per-stud needs `scene::Bounds`; a `BillboardGui`'s scale is in
// studs against a camera and a viewport. `gui` links neither, so it declares
// `gui::SpatialCanvas` and this module fills it.
//
// The cases below are arranged around the thing that makes this more than a
// multiplication: **absence has to mean something**. A collector nothing can
// measure must end up with no component at all, so `gui::CanvasFor` falls back
// to the authored size - and one that stops being measurable must *lose* the
// component rather than keep a stale one.

#include "Primitives.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.render.spatialcanvas")
TEST_DEPENDS("engine.gui.layout")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::render::ResolveSpatialCanvases;

namespace {
	struct World {
		Store Data;
		engine::gui::Screen Display;
		Entity Workspace;

		explicit World(std::string_view name) : Data(name) {
			engine::gui::RegisterGuiClasses();
			engine::scene::EnsureClassTree();

			Workspace = Data.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("Instance")),
				std::string(engine::gui::WORKSPACE)
			);

			Display.Width = 1600.0f;
			Display.Height = 900.0f;
		}

		Entity Part(const Vector3 &position, const Vector3 &halfExtent) {
			const Entity part = Data.CreateInstance(engine::scene::PartClass(), "Rock");
			Data.SetParent(part, Workspace);
			Data.Set(part, engine::scene::Transform{CFrame(position)});
			Data.Set(part, engine::scene::Bounds{halfExtent});
			return part;
		}

		Entity Collector(const char *klass, Entity parent) {
			const Entity made = Data.CreateInstance(engine::gui::GuiClass(klass), klass);
			Data.SetParent(made, parent);
			Data.Set(made, engine::gui::Layer{});
			return made;
		}

		// A camera at `position` looking wherever - only the distance is read.
		void Camera(const Vector3 &position, float fieldOfViewRadians) {
			const Entity eye = Data.CreateInstance(engine::scene::PartClass(), "Camera");
			Data.SetParent(eye, Workspace);
			Data.Set(eye, engine::scene::Transform{CFrame(position)});
			Data.Set(
				eye,
				engine::scene::Camera{
					.FieldOfViewRadians = fieldOfViewRadians,
					.NearPlane = 0.1f,
					.FarPlane = 500.0f,
					.RenderFeatures = {},
				}
			);

			engine::scene::ActiveCamera active;
			active.Entity = eye;
			active.AspectRatio = Display.Width / Display.Height;
			Data.SetResource(active);
		}

		const engine::gui::SpatialCanvas *Resolved(Entity collector) {
			return Data.Get<engine::gui::SpatialCanvas>(collector);
		}
	};
}

TEST_CASE("a surface gui in pixels-per-stud is sized by its adornee", "[render][canvas]") {
	// The multiplication the entry named. A four-by-three part at fifty pixels
	// per stud is a two-hundred-by-one-fifty canvas, and everything inside it
	// then lays out against a rectangle whose aspect matches the face it is
	// projected onto - which is the whole reason the mode exists.
	World world("render_canvas.perstud");

	const Entity part = world.Part(Vector3{0.0f, 0.0f, 0.0f}, Vector3{2.0f, 1.5f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.Sizing = engine::gui::SurfaceSizingMode::PixelsPerStud;
	state.PixelsPerStud = 50.0f;
	state.On = engine::gui::Face::Front;
	world.Data.Set(surface, state);

	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 1);

	const engine::gui::SpatialCanvas *canvas = world.Resolved(surface);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Size.X == Approx(200.0f));
	CHECK(canvas->Size.Y == Approx(150.0f));
}

TEST_CASE("which face a surface gui is on decides which two studs it spans", "[render][canvas]") {
	// **A part with three different extents, so no two faces agree.** A face is
	// named by the axis it points along and spans the other two, which is easy
	// to write one axis out - and the symptom is a sign that reads sideways
	// rather than anything that looks like an arithmetic mistake.
	World world("render_canvas.faces");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.5f, 0.5f}); // 4 x 3 x 1 studs

	const auto spans = [&](engine::gui::Face face) {
		const Entity surface = world.Collector("SurfaceGui", part);
		engine::gui::Surface state;
		state.Sizing = engine::gui::SurfaceSizingMode::PixelsPerStud;
		state.PixelsPerStud = 1.0f;
		state.On = face;
		world.Data.Set(surface, state);

		ResolveSpatialCanvases(world.Data, world.Display);
		const engine::gui::SpatialCanvas *canvas = world.Resolved(surface);
		REQUIRE(canvas != nullptr);
		return canvas->Size;
	};

	CHECK(spans(engine::gui::Face::Front).X == Approx(4.0f));
	CHECK(spans(engine::gui::Face::Front).Y == Approx(3.0f));

	CHECK(spans(engine::gui::Face::Right).X == Approx(1.0f));
	CHECK(spans(engine::gui::Face::Right).Y == Approx(3.0f));

	CHECK(spans(engine::gui::Face::Top).X == Approx(4.0f));
	CHECK(spans(engine::gui::Face::Top).Y == Approx(1.0f));
}

TEST_CASE("every surface canvas faces outward without mirroring", "[render][canvas]") {
	World world("render_canvas.orientation");
	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.5f, 0.5f});

	for (const engine::gui::Face face : {
			 engine::gui::Face::Front,
			 engine::gui::Face::Back,
			 engine::gui::Face::Left,
			 engine::gui::Face::Right,
			 engine::gui::Face::Top,
			 engine::gui::Face::Bottom,
		 }) {
		const Entity surface = world.Collector("SurfaceGui", part);
		engine::gui::Surface state;
		state.On = face;
		world.Data.Set(surface, state);
		REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) >= 1);

		const engine::gui::SpatialCanvas *canvas = world.Resolved(surface);
		REQUIRE(canvas != nullptr);
		const Vector3 winding = canvas->AxisX.Cross(canvas->AxisY).Unit();
		CHECK(winding.Dot(canvas->Normal) == Approx(-1.0f));
	}
}

TEST_CASE("a fixed-size surface gui keeps its authored canvas and gains a plane", "[render][canvas]") {
	// Fixed size still needs a resolved world plane. Without it the children lay
	// out correctly and the renderer has nowhere to put their pixels.
	World world("render_canvas.fixed");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.5f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.Sizing = engine::gui::SurfaceSizingMode::FixedSize;
	state.CanvasSize = engine::core::Vector2{640.0f, 480.0f};
	world.Data.Set(surface, state);

	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	const engine::gui::SpatialCanvas *spatial = world.Resolved(surface);
	REQUIRE(spatial != nullptr);
	CHECK(spatial->Size.X == Approx(640.0f));
	CHECK(spatial->Size.Y == Approx(480.0f));
	CHECK(spatial->AxisX.Magnitude() == Approx(4.0f));
	CHECK(spatial->AxisY.Magnitude() == Approx(3.0f));

	// And the layout then uses the authored size, which is the fallback the
	// absence exists to select.
	engine::gui::Layout(world.Data, world.Display);
	const engine::gui::Canvas *canvas = world.Data.Get<engine::gui::Canvas>(surface);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Area.Width() == Approx(640.0f));
	CHECK(canvas->Area.Height() == Approx(480.0f));
}

TEST_CASE("a surface gui on something with no bounds resolves nothing", "[render][canvas]") {
	// A `Folder` is a legal parent. Sizing against a part that is not one would
	// mean inventing an extent, and the honest answer is the authored pixels.
	World world("render_canvas.unbounded");

	const Entity folder =
		world.Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Folder")), "Signs");
	world.Data.SetParent(folder, world.Workspace);

	const Entity surface = world.Collector("SurfaceGui", folder);
	engine::gui::Surface state;
	state.Sizing = engine::gui::SurfaceSizingMode::PixelsPerStud;
	world.Data.Set(surface, state);

	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 0);
	CHECK(world.Resolved(surface) == nullptr);
}

TEST_CASE("switching a surface gui to fixed size replaces the measured canvas", "[render][canvas]") {
	World world("render_canvas.stale");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.5f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.Sizing = engine::gui::SurfaceSizingMode::PixelsPerStud;
	state.PixelsPerStud = 50.0f;
	world.Data.Set(surface, state);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	REQUIRE(world.Resolved(surface) != nullptr);

	world.Data.GetMutable<engine::gui::Surface>(surface)->Sizing = engine::gui::SurfaceSizingMode::FixedSize;

	world.Data.GetMutable<engine::gui::Surface>(surface)->CanvasSize = engine::core::Vector2{320.0f, 180.0f};
	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	const engine::gui::SpatialCanvas *fixed = world.Resolved(surface);
	REQUIRE(fixed != nullptr);
	CHECK(fixed->Size.X == Approx(320.0f));
	CHECK(fixed->Size.Y == Approx(180.0f));
}

TEST_CASE("a billboard's scale is studs against the viewport", "[render][canvas]") {
	// **Scale is studs and offset is pixels**, which is the one thing about a
	// `BillboardGui` that catches people: the same `UDim2` means something else
	// here than it does everywhere else in the tree.
	//
	// The numbers are chosen so the conversion is exact rather than approximate.
	// A ninety-degree vertical field of view has `tan(fov/2) = 1`, so at ten
	// studs the viewport spans twenty studs - and a nine-hundred-pixel viewport
	// is therefore forty-five pixels to the stud.
	World world("render_canvas.billboard");

	constexpr float RIGHT_ANGLE = 1.57079632679f;
	world.Camera(Vector3{0.0f, 0.0f, 0.0f}, RIGHT_ANGLE);

	const Entity part = world.Part(Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const Entity billboard = world.Collector("BillboardGui", part);

	engine::gui::Billboard state;
	state.Size = UDim2{2.0f, 20.0f, 1.0f, 0.0f}; // 2 studs + 20 px wide, 1 stud tall
	world.Data.Set(billboard, state);

	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 1);

	const engine::gui::SpatialCanvas *canvas = world.Resolved(billboard);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Size.X == Approx(2.0f * 45.0f + 20.0f));
	CHECK(canvas->Size.Y == Approx(45.0f));
}

TEST_CASE("a billboard shrinks with distance", "[render][canvas]") {
	// The property that makes this worth resolving every frame rather than once.
	// Twice as far is half the size, which is what a player walking away sees
	// and what a canvas computed at load time would not do.
	World world("render_canvas.distance");

	constexpr float RIGHT_ANGLE = 1.57079632679f;
	world.Camera(Vector3::Zero, RIGHT_ANGLE);

	const Entity near = world.Part(Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const Entity far = world.Part(Vector3{0.0f, 0.0f, 20.0f}, Vector3{0.5f, 0.5f, 0.5f});

	engine::gui::Billboard state;
	state.Size = UDim2{1.0f, 0.0f, 1.0f, 0.0f};

	const Entity closer = world.Collector("BillboardGui", near);
	world.Data.Set(closer, state);
	const Entity further = world.Collector("BillboardGui", far);
	world.Data.Set(further, state);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 2);

	const engine::gui::SpatialCanvas *a = world.Resolved(closer);
	const engine::gui::SpatialCanvas *b = world.Resolved(further);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(a->Size.Y == Approx(2.0f * b->Size.Y));
}

TEST_CASE("a billboard in a world with no camera keeps its offset", "[render][canvas]") {
	// A headless world, a world being loaded, a world in a test. There is no
	// distance to measure a stud against, so the authored pixels are the answer
	// - which is exactly the behaviour `gui::CanvasFor` had before this existed.
	World world("render_canvas.nocamera");

	const Entity part = world.Part(Vector3{0.0f, 0.0f, 10.0f}, Vector3{0.5f, 0.5f, 0.5f});
	const Entity billboard = world.Collector("BillboardGui", part);

	engine::gui::Billboard state;
	state.Size = UDim2{4.0f, 200.0f, 4.0f, 50.0f};
	world.Data.Set(billboard, state);

	CHECK(ResolveSpatialCanvases(world.Data, world.Display) == 0);
	CHECK(world.Resolved(billboard) == nullptr);

	engine::gui::Layout(world.Data, world.Display);
	const engine::gui::Canvas *canvas = world.Data.Get<engine::gui::Canvas>(billboard);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Area.Width() == Approx(200.0f));
	CHECK(canvas->Area.Height() == Approx(50.0f));
}

TEST_CASE("the layout lays out against the resolved canvas", "[render][canvas]") {
	// **The end of the seam, and the half neither module can assert alone.**
	// `render` writes a component and `gui` reads it; a case in either module
	// checks one side of a wire. This is the one that fails if the two stop
	// meeting.
	World world("render_canvas.through");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.5f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.Sizing = engine::gui::SurfaceSizingMode::PixelsPerStud;
	state.PixelsPerStud = 50.0f;
	state.CanvasSize = engine::core::Vector2{9999.0f, 9999.0f}; // never used
	world.Data.Set(surface, state);

	const Entity frame = world.Data.CreateInstance(engine::gui::GuiClass("Frame"), "Panel");
	world.Data.SetParent(frame, surface);

	engine::gui::Element element;
	element.Size = UDim2{1.0f, 0.0f, 0.5f, 0.0f};
	world.Data.Set(frame, element);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	engine::gui::Layout(world.Data, world.Display);

	const engine::gui::Resolved *resolved = world.Data.Get<engine::gui::Resolved>(frame);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->Rendered);
	CHECK(resolved->AbsoluteSize.X == Approx(200.0f));
	CHECK(resolved->AbsoluteSize.Y == Approx(75.0f));
}

TEST_CASE("a window pointer projects onto an interactive surface canvas", "[render][canvas][input]") {
	World world("render_canvas.pointer");
	constexpr float RIGHT_ANGLE = 1.57079632679f;
	world.Camera(Vector3::Zero, RIGHT_ANGLE);

	const Entity part = world.Part(Vector3{0.0f, 0.0f, -10.0f}, Vector3{2.0f, 1.0f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);
	engine::gui::Surface state;
	state.On = engine::gui::Face::Back;
	state.CanvasSize = Vector2{400.0f, 200.0f};
	state.Active = true;
	world.Data.Set(surface, state);

	const Entity button = world.Data.CreateInstance(engine::gui::GuiClass("TextButton"), "Press");
	world.Data.SetParent(button, surface);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	engine::gui::DrawList list;
	engine::gui::DrawCommand command;
	command.Kind = engine::gui::DrawKind::Rectangle;
	command.Source = button;
	command.Collector = surface;
	command.Bounds = engine::core::Rect{0.0f, 0.0f, 400.0f, 200.0f};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);

	engine::gui::Router router;
	engine::gui::Pointer windowPointer;
	windowPointer.Position = Vector2{world.Display.Width * 0.5f, world.Display.Height * 0.5f};
	engine::render::SpatialPointer pointer;
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == surface);
	CHECK(pointer.Position.X == Approx(200.0f).margin(0.1f));
	CHECK(pointer.Position.Y == Approx(100.0f).margin(0.1f));

	// A viewport can display a camera and aspect unrelated to the world's active eye.
	engine::scene::Camera viewportLens;
	viewportLens.FieldOfViewRadians = 1.57079632679f;
	const CFrame viewportFrame(Vector3{0.5f, 0.0f, 0.0f});
	const auto viewportPointer = engine::render::RouteGuiPointer(
		world.Data, list, world.Display, router, windowPointer, &viewportLens, &viewportFrame, 1.0f
	);
	CHECK(viewportPointer.Collector == surface);
	CHECK(viewportPointer.Position.X == Approx(250.0f).margin(0.1f));
	windowPointer.Position.X += 20.0f;
	const auto squarePointer = engine::render::RouteGuiPointer(
		world.Data, list, world.Display, router, windowPointer, &viewportLens, &viewportFrame, 1.0f
	);
	const auto widePointer = engine::render::RouteGuiPointer(
		world.Data, list, world.Display, router, windowPointer, &viewportLens, &viewportFrame, 2.0f
	);
	CHECK(squarePointer.Collector == surface);
	CHECK(widePointer.Collector == surface);
	CHECK(widePointer.Position.X > squarePointer.Position.X);
	viewportLens.FieldOfViewRadians = 0.78539816339f;
	const auto narrowLensPointer = engine::render::RouteGuiPointer(
		world.Data, list, world.Display, router, windowPointer, &viewportLens, &viewportFrame, 1.0f
	);
	CHECK(narrowLensPointer.Collector == surface);
	CHECK(narrowLensPointer.Position.X < squarePointer.Position.X);
	windowPointer.Position.X -= 20.0f;
	viewportLens.FarPlane = 5.0f;
	CHECK_FALSE(
		engine::render::ResolveSpatialPointer(
			world.Data, list, world.Display, windowPointer, router, pointer, &viewportLens, &viewportFrame
		)
	);
	viewportLens.FarPlane = 20.0f;
	CHECK(
		engine::render::ResolveSpatialPointer(
			world.Data, list, world.Display, windowPointer, router, pointer, &viewportLens, &viewportFrame
		)
	);

	// The draw pass measures the canvas origin from the camera, not the ray
	// length to the point under the pointer.
	const auto originalCanvas = *world.Data.Get<engine::gui::SpatialCanvas>(surface);
	auto limitedCanvas = originalCanvas;
	limitedCanvas.MaxDistance = 9.6f;
	REQUIRE(limitedCanvas.Origin.Magnitude() > limitedCanvas.MaxDistance);
	world.Data.Set(surface, limitedCanvas);
	CHECK_FALSE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	const CFrame geometryNear(Vector3{0.0f, 0.0f, -2.0f});
	CHECK(
		engine::render::ResolveSpatialPointer(
			world.Data,
			list,
			world.Display,
			windowPointer,
			router,
			pointer,
			nullptr,
			nullptr,
			0.0f,
			&geometryNear
		)
	);
	world.Data.Set(surface, originalCanvas);
	const CFrame geometryBehind(Vector3{0.0f, 0.0f, -20.0f});
	CHECK_FALSE(
		engine::render::ResolveSpatialPointer(
			world.Data,
			list,
			world.Display,
			windowPointer,
			router,
			pointer,
			nullptr,
			nullptr,
			0.0f,
			&geometryBehind
		)
	);

	const Entity fartherSurface = world.Collector("SurfaceGui", part);
	const Entity fartherButton = world.Data.CreateInstance(engine::gui::GuiClass("TextButton"), "Farther");
	REQUIRE(world.Data.SetParent(fartherButton, fartherSurface));
	auto nearTop = originalCanvas;
	nearTop.AlwaysOnTop = true;
	world.Data.Set(surface, nearTop);
	auto farTop = nearTop;
	farTop.Origin.Z -= 1.0f;
	world.Data.Set(fartherSurface, farTop);
	auto fartherCommand = command;
	fartherCommand.Source = fartherButton;
	fartherCommand.Collector = fartherSurface;
	list.Commands.push_back(fartherCommand);
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == fartherSurface);
	auto lateCommand = command;
	lateCommand.Bounds = engine::core::Rect{0.0f, 0.0f, 20.0f, 20.0f};
	list.Commands.push_back(lateCommand);
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == fartherSurface);
	list.Commands.back().Bounds = command.Bounds;
	list.Commands.back().Clip = engine::core::Rect{0.0f, 0.0f, 20.0f, 20.0f};
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == fartherSurface);
	list.Commands.back().Clip = command.Clip;
	list.Commands.back().Transparency = 1.0f;
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == surface);
	list.Commands.pop_back();
	farTop.AlwaysOnTop = false;
	world.Data.Set(fartherSurface, farTop);
	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
	CHECK(pointer.Collector == surface);
	world.Data.DestroyInstance(fartherSurface);
	list.Commands.pop_back();
	world.Data.Set(surface, originalCanvas);

	// **And the same click misses once the surface stops taking input**, which
	// is Roblox's default and the reason `Active` is a property rather than an
	// assumption: a decorative sign must not intercept the shot aimed at what is
	// behind it.
	state.Active = false;
	world.Data.Set(surface, state);
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	CHECK_FALSE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, pointer)
	);
}

TEST_CASE("a spatial scrolling frame accepts its background drag and wheel", "[render][canvas][input]") {
	World world("render_canvas.scrolling");
	world.Camera(Vector3::Zero, 1.57079632679f);
	const Entity part = world.Part(Vector3{0.0f, 0.0f, -10.0f}, Vector3{2.0f, 1.0f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);
	engine::gui::Surface surfaceState;
	surfaceState.On = engine::gui::Face::Back;
	surfaceState.CanvasSize = Vector2{400.0f, 200.0f};
	surfaceState.Active = true;
	world.Data.Set(surface, surfaceState);

	const Entity frame = world.Data.CreateInstance(engine::gui::GuiClass("ScrollingFrame"), "Scroll");
	REQUIRE(world.Data.SetParent(frame, surface));
	engine::gui::Scrolling scrolling;
	scrolling.CanvasSize = UDim2{0.0f, 400.0f, 0.0f, 800.0f};
	world.Data.Set(frame, scrolling);
	engine::gui::ScrollState scrollState;
	scrollState.WindowSize = Vector2{400.0f, 200.0f};
	scrollState.CanvasSize = Vector2{400.0f, 800.0f};
	world.Data.Set(frame, scrollState);
	engine::gui::Resolved resolved;
	resolved.AbsoluteSize = scrollState.WindowSize;
	resolved.Clip = engine::core::Rect{0.0f, 0.0f, 400.0f, 200.0f};
	resolved.Rendered = true;
	world.Data.Set(frame, resolved);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	engine::gui::DrawList list;
	engine::gui::DrawCommand command;
	command.Source = frame;
	command.Collector = surface;
	command.Bounds = resolved.Clip;
	command.Clip = resolved.Clip;
	list.Commands.push_back(command);

	engine::gui::Router router;
	engine::gui::Pointer windowPointer;
	windowPointer.Position = Vector2{world.Display.Width * 0.5f, world.Display.Height * 0.5f};
	engine::render::SpatialPointer spatial;
	SECTION("passive pointer") {
		CHECK_FALSE(
			engine::render::ResolveSpatialPointer(
				world.Data, list, world.Display, windowPointer, router, spatial
			)
		);
		return;
	}
	SECTION("press outside surface") {
		windowPointer.Down = true;
		windowPointer.Position.Y -= 110.0f;
		CHECK_FALSE(
			engine::render::ResolveSpatialPointer(
				world.Data, list, world.Display, windowPointer, router, spatial
			)
		);
		return;
	}
	SECTION("background press") {
		windowPointer.Down = true;
	}
	SECTION("wheel") {
		windowPointer.Wheel = -1.0f;
	}

	REQUIRE(
		engine::render::ResolveSpatialPointer(world.Data, list, world.Display, windowPointer, router, spatial)
	);
	CHECK(spatial.Collector == surface);
	const Vector2 windowStart = windowPointer.Position;
	windowPointer.Position = spatial.Position;
	windowPointer.Collector = spatial.Collector;
	router.Update(world.Data, list, windowPointer);
	if (windowPointer.Down) {
		REQUIRE(router.CanvasHeld() == frame);
		windowPointer.Position = windowStart + Vector2{0.0f, -10.0f};
		REQUIRE(
			engine::render::ResolveSpatialPointer(
				world.Data, list, world.Display, windowPointer, router, spatial
			)
		);
		windowPointer.Position = spatial.Position;
		windowPointer.Collector = spatial.Collector;
		router.Update(world.Data, list, windowPointer);
		CHECK(world.Data.Get<engine::gui::Scrolling>(frame)->CanvasPosition.Y > 0.0f);
		windowPointer.Position = windowStart + Vector2{0.0f, -110.0f};
		REQUIRE(
			engine::render::ResolveSpatialPointer(
				world.Data, list, world.Display, windowPointer, router, spatial
			)
		);
		CHECK(spatial.Position.Y < 0.0f);
		windowPointer.Position = spatial.Position;
		router.Update(world.Data, list, windowPointer);
		CHECK(router.CanvasHeld() == frame);
	} else {
		CHECK(world.Data.Get<engine::gui::Scrolling>(frame)->CanvasPosition.Y > 0.0f);
	}
}

TEST_CASE("an off-axis billboard pointer matches the drawn quad", "[render][canvas][input]") {
	World world("render_canvas.billboard_pointer");
	world.Camera(Vector3::Zero, 1.57079632679f);
	const Entity collector = world.Collector("BillboardGui", world.Workspace);
	const Entity button = world.Data.CreateInstance(engine::gui::GuiClass("TextButton"), "Button");
	REQUIRE(world.Data.SetParent(button, collector));
	engine::gui::SpatialCanvas canvas;
	canvas.Kind = engine::gui::SpatialCanvasKind::Billboard;
	canvas.Interactive = true;
	canvas.Origin = Vector3{6.0f, 1.0f, -10.0f};
	canvas.Size = Vector2{200.0f, 100.0f};
	canvas.BillboardStuds = Vector2{0.5f, 0.25f};
	canvas.BillboardPixels = Vector2{180.0f, 90.0f};
	canvas.WorldSize = Vector2{5.0f, 2.5f};
	world.Data.Set(collector, canvas);

	engine::gui::DrawList list;
	engine::gui::DrawCommand command;
	command.Source = button;
	command.Collector = collector;
	command.Bounds = engine::core::Rect{0.0f, 0.0f, canvas.Size.X, canvas.Size.Y};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);

	engine::scene::Camera lens;
	lens.FieldOfViewRadians = 1.57079632679f;
	const CFrame frame(Vector3{1.0f, 0.0f, 0.0f});
	CFrame geometryFrame = CFrame::Angles(0.0f, 0.2f, 0.0f);
	geometryFrame.Position = Vector3{-1.0f, 0.0f, 0.0f};
	const float aspect = world.Display.Width / world.Display.Height;
	const auto matrix = engine::scene::ResolveCamera(frame, lens, aspect).ViewProjection;
	const Vector3 up = geometryFrame.UpVector();
	const auto quad = engine::render::BillboardQuad(
		canvas.Origin,
		geometryFrame.RightVector(),
		up,
		geometryFrame.Position - canvas.Origin,
		canvas.BillboardStuds,
		canvas.BillboardPixels,
		engine::render::CanvasPixelsPerStud(matrix, canvas.Origin, up, world.Display.Height),
		geometryFrame.LookVector() * -1.0f
	);
	const Vector3 target = quad.Origin + quad.AxisX * 0.8f + quad.AxisY * 0.25f;
	const glm::vec4 clip = matrix * glm::vec4{target.X, target.Y, target.Z, 1.0f};
	engine::gui::Pointer pointer;
	pointer.Position = Vector2{
		(clip.x / clip.w + 1.0f) * world.Display.Width * 0.5f,
		(1.0f - clip.y / clip.w) * world.Display.Height * 0.5f,
	};
	engine::gui::Router router;
	const auto routed = engine::render::RouteGuiPointer(
		world.Data, list, world.Display, router, pointer, &lens, &frame, aspect, &geometryFrame
	);
	REQUIRE(routed.Collector == collector);
	CHECK(routed.Position.X == Approx(canvas.Size.X * 0.8f).margin(0.1f));
	CHECK(routed.Position.Y == Approx(canvas.Size.Y * 0.25f).margin(0.1f));
}

TEST_CASE("a spatial drag detector accepts its inactive frame", "[render][canvas][input]") {
	World world("render_canvas.detector");
	world.Camera(Vector3::Zero, 1.57079632679f);
	const Entity part = world.Part(Vector3{0.0f, 0.0f, -10.0f}, Vector3{2.0f, 1.0f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);
	engine::gui::Surface surfaceState;
	surfaceState.On = engine::gui::Face::Back;
	surfaceState.CanvasSize = Vector2{400.0f, 200.0f};
	surfaceState.Active = true;
	world.Data.Set(surface, surfaceState);
	const Entity frame = world.Data.CreateInstance(engine::gui::GuiClass("Frame"), "DragTarget");
	REQUIRE(world.Data.SetParent(frame, surface));
	const Entity detector = world.Data.CreateInstance(engine::gui::GuiClass("UIDragDetector"), "Drag");
	REQUIRE(world.Data.SetParent(detector, frame));
	world.Data.Set(detector, engine::gui::DragDetector{});
	engine::gui::Resolved resolved;
	resolved.AbsoluteSize = Vector2{400.0f, 200.0f};
	resolved.Clip = engine::core::Rect{0.0f, 0.0f, 400.0f, 200.0f};
	resolved.Rendered = true;
	world.Data.Set(frame, resolved);
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);

	engine::gui::DrawList list;
	engine::gui::DrawCommand command;
	command.Source = frame;
	command.Collector = surface;
	command.Bounds = resolved.Clip;
	command.Clip = resolved.Clip;
	list.Commands.push_back(command);
	engine::gui::Router router;
	engine::gui::Pointer pointer;
	pointer.Down = true;
	pointer.Position = Vector2{world.Display.Width * 0.5f, world.Display.Height * 0.5f};
	engine::render::SpatialPointer spatial;
	REQUIRE(engine::render::ResolveSpatialPointer(world.Data, list, world.Display, pointer, router, spatial));
	pointer.Position = spatial.Position;
	pointer.Collector = spatial.Collector;
	router.Update(world.Data, list, pointer);
	CHECK(router.DragHeld() == detector);
}

TEST_CASE("a surface gui past its maximum distance stops resolving a canvas", "[render][canvas]") {
	// `MaxDistance` measured to the *canvas centre* rather than to the part's
	// origin, which is what a long wall with a sign on one end depends on. The
	// component is still written - the collector is measurable - and it is
	// `Visible` that goes false, which is what `gui::CanvasFor` reads to stop
	// laying anything out under it.
	World world("render_canvas.surfacerange");
	constexpr float RIGHT_ANGLE = 1.57079632679f;
	world.Camera(Vector3::Zero, RIGHT_ANGLE);

	const Entity part = world.Part(Vector3{0.0f, 0.0f, -60.0f}, Vector3{2.0f, 1.0f, 0.5f});
	const Entity surface = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.On = engine::gui::Face::Back;
	state.MaxDistance = 100.0f;
	world.Data.Set(surface, state);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	REQUIRE(world.Resolved(surface) != nullptr);
	CHECK(world.Resolved(surface)->Visible);
	CHECK(world.Resolved(surface)->CurrentDistance == Approx(59.5f).margin(0.01f));

	state.MaxDistance = 50.0f;
	world.Data.Set(surface, state);
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	CHECK_FALSE(world.Resolved(surface)->Visible);

	// And nothing under it is laid out, which is the half a player sees.
	const Entity frame = world.Data.CreateInstance(engine::gui::GuiClass("Frame"), "Panel");
	world.Data.SetParent(frame, surface);
	world.Data.Set(frame, engine::gui::Element{});
	engine::gui::Layout(world.Data, world.Display);

	const engine::gui::Resolved *resolved = world.Data.Get<engine::gui::Resolved>(frame);
	CHECK((resolved == nullptr || !resolved->Rendered));
}

TEST_CASE("a surface gui floats off its face by its own offset", "[render][canvas]") {
	// `ZOffset` adds to the hairline bias rather than replacing it, so stacking
	// two surfaces on one face is one number apart and neither is coplanar with
	// the part.
	World world("render_canvas.zoffset");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 1.0f, 0.5f});
	const Entity plain = world.Collector("SurfaceGui", part);
	const Entity lifted = world.Collector("SurfaceGui", part);

	engine::gui::Surface state;
	state.On = engine::gui::Face::Front;
	world.Data.Set(plain, state);
	state.ZOffset = 0.25f;
	world.Data.Set(lifted, state);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 2);

	// Front looks down -Z, so a positive offset moves the canvas further along it.
	const float apart = world.Resolved(plain)->Origin.Z - world.Resolved(lifted)->Origin.Z;
	CHECK(apart == Approx(0.25f).margin(0.001f));
}

TEST_CASE("a billboard steps its distance and offsets its anchor", "[render][canvas]") {
	// Three properties that all move one number, checked together because that
	// is where they would disagree: `DistanceStep` quantises what the size is
	// computed from, `SizeOffset` moves the anchor by a fraction of that size,
	// and `CurrentDistance` is what a script reads back.
	World world("render_canvas.billboardoffsets");
	constexpr float RIGHT_ANGLE = 1.57079632679f;
	world.Camera(Vector3::Zero, RIGHT_ANGLE);

	const Entity part = world.Part(Vector3{0.0f, 0.0f, -22.0f}, Vector3{1.0f, 1.0f, 1.0f});
	const Entity billboard = world.Collector("BillboardGui", part);

	engine::gui::Billboard state;
	state.Size = UDim2{4.0f, 0.0f, 2.0f, 0.0f};
	state.DistanceStep = 10.0f;
	world.Data.Set(billboard, state);

	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);

	// Twenty-two studs away, stepped to twenty. The pixel size is what that
	// distance produces, so the step is visible in the canvas rather than only
	// in the number.
	CHECK(world.Resolved(billboard)->CurrentDistance == Approx(20.0f));
	const Vector2 stepped = world.Resolved(billboard)->Size;

	state.DistanceStep = 0.0f;
	world.Data.Set(billboard, state);
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);
	CHECK(world.Resolved(billboard)->CurrentDistance == Approx(22.0f));
	CHECK(world.Resolved(billboard)->Size.X < stepped.X);

	// `SizeOffset` moves the anchor towards the bottom left, so the quad moves
	// right and down by half its own size.
	const Vector3 centred = world.Resolved(billboard)->Origin;
	state.SizeOffset = Vector2{0.5f, 0.5f};
	world.Data.Set(billboard, state);
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display) == 1);

	const engine::gui::SpatialCanvas *shifted = world.Resolved(billboard);
	CHECK(shifted->Origin.X == Approx(centred.X - shifted->WorldSize.X * 0.5f).margin(0.001f));
	CHECK(shifted->Origin.Y == Approx(centred.Y + shifted->WorldSize.Y * 0.5f).margin(0.001f));
}

TEST_CASE(
	"an explicit offscreen camera resolves billboards without replacing the player camera", "[render][canvas]"
) {
	World world("render_canvas.explicit");
	const auto part = world.Part({0, 0, -10}, {1, 1, 1});
	const auto collector = world.Collector("BillboardGui", part);
	engine::gui::Billboard billboard;
	billboard.Size = {2, 0, 1, 0};
	world.Data.Set(collector, billboard);
	world.Camera({100, 0, 0}, .5f);
	const auto active = *world.Data.Resource<engine::scene::ActiveCamera>();
	const auto original = *world.Data.Get<engine::scene::Transform>(active.Entity);
	engine::scene::Camera camera;
	camera.FieldOfViewRadians = 1.57079632679f;
	const CFrame frame;
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display, &camera, &frame) == 1);
	const auto *canvas = world.Resolved(collector);
	REQUIRE(canvas != nullptr);
	CHECK(canvas->Size.X == Approx(90));
	CHECK(canvas->Size.Y == Approx(45));
	CHECK(world.Data.Resource<engine::scene::ActiveCamera>()->Entity == active.Entity);
	CHECK(world.Data.Get<engine::scene::Transform>(active.Entity)->Frame.Position == original.Frame.Position);
	world.Data.SetResource(engine::scene::ActiveCamera{});
	REQUIRE(ResolveSpatialCanvases(world.Data, world.Display, &camera, &frame) == 1);
	CHECK(world.Resolved(collector)->Size == canvas->Size);
}
