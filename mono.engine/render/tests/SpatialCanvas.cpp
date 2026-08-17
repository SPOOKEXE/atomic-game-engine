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

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
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
			Data.Set(eye, engine::scene::Camera{fieldOfViewRadians, 0.1f, 500.0f});

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

	engine::render::SpatialPointer pointer;
	REQUIRE(
		engine::render::ResolveSpatialPointer(
			world.Data,
			list,
			world.Display,
			Vector2{world.Display.Width * 0.5f, world.Display.Height * 0.5f},
			pointer
		)
	);
	CHECK(pointer.Collector == surface);
	CHECK(pointer.Position.X == Approx(200.0f).margin(0.1f));
	CHECK(pointer.Position.Y == Approx(100.0f).margin(0.1f));
}
