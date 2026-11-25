// Pointer gestures on a NodeCanvas, driven through the Router like a real frame.

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.gui.nodecanvasgesture")
TEST_DEPENDS("engine.gui.nodecanvas")
TEST_DEPENDS("engine.angview.view")

using Catch::Approx;
using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using namespace engine::gui;

namespace {
	// A 600x400 canvas at the origin with two nodes:
	//   source at (50, 50), 100x60, output "value" centred at (145, 75)
	//   sink at (300, 50), 100x60, input "value" centred at (305, 75)
	struct Scene {
		Store Data{"gui.node_canvas_gesture"};
		CompileRequest Request;
		Compiled List;
		Router Route;
		Entity Canvas;
		Entity Source;
		Entity Sink;
		Entity Output;
		Entity Input;

		explicit Scene(const char *inputType = "number", float zoom = 1.0f) {
			RegisterGuiClasses();
			InstallGuiServices(Data);
			const Entity container =
				Data.CreateInstance(engine::ecs::Classes::Find(Name("Instance")), "StarterGui");
			Request.Display.Width = 800.0f;
			Request.Display.Height = 600.0f;
			const Entity screen = Make("ScreenGui", container);
			Canvas = Make("NodeCanvas", screen);
			Box(Canvas, 0.0f, 0.0f, 600.0f, 400.0f);
			NodeCanvas view;
			view.Zoom = zoom;
			Data.Set(Canvas, view);
			Source = Node("source", 50.0f);
			Sink = Node("sink", 300.0f);
			Output = Port(Source, NodePortDirection::Output, "number", 90.0f);
			Input = Port(Sink, NodePortDirection::Input, inputType, 0.0f);
		}

		Entity Make(std::string_view klass, Entity parent) {
			const Entity made = Data.CreateInstance(GuiClass(klass), klass);
			Data.SetParent(made, parent);
			return made;
		}

		void Box(Entity instance, float x, float y, float width, float height, bool active = false) {
			Element element;
			element.Position = UDim2{0.0f, x, 0.0f, y};
			element.Size = UDim2{0.0f, width, 0.0f, height};
			element.Active = active;
			Data.Set(instance, element);
		}

		Entity Node(const char *id, float x) {
			const Entity node = Make("NodeCanvasNode", Canvas);
			Box(node, x, 50.0f, 100.0f, 60.0f);
			NodeCanvasNode state;
			state.Id = Name(id);
			Data.Set(node, state);
			return node;
		}

		Entity Port(Entity node, NodePortDirection direction, const char *type, float x) {
			const Entity port = Make("NodeCanvasPort", node);
			Box(port, x, 20.0f, 10.0f, 10.0f, true);
			Data.Set(port, NodeCanvasPort{Name("value"), Name(type), direction});
			return port;
		}

		std::vector<GuiEvent> Send(float x, float y, bool down) {
			Request.Hovered = Route.Hovered();
			Request.Pressed = Route.Pressed();
			Request.Wire = Route.NodeWire();
			List.Rebuild(Data, Request);
			Pointer pointer;
			pointer.Position = Vector2{x, y};
			pointer.Down = down;
			const auto events = Route.Update(Data, List.Commands(), pointer);
			return {events.begin(), events.end()};
		}

		// Press, travel in two steps, release; returns every event seen.
		std::vector<GuiEvent> Drag(Vector2 from, Vector2 to) {
			std::vector<GuiEvent> all;
			for (const auto &step :
				 {Send(from.X, from.Y, false),
				  Send(from.X, from.Y, true),
				  Send((from.X + to.X) * 0.5f, (from.Y + to.Y) * 0.5f, true),
				  Send(to.X, to.Y, true),
				  Send(to.X, to.Y, false)}) {
				all.insert(all.end(), step.begin(), step.end());
			}
			return all;
		}

		size_t Links() {
			size_t count = 0;
			Data.EachChild(Canvas, [&](Entity child) {
				count += Data.Get<NodeCanvasLink>(child) != nullptr ? 1 : 0;
			});
			return count;
		}

		static bool Has(const std::vector<GuiEvent> &events, EventKind kind, Entity instance) {
			for (const GuiEvent &event : events) {
				if (event.Kind == kind && event.Instance == instance) {
					return true;
				}
			}
			return false;
		}
	};
}

TEST_CASE("dragging a node moves its element by the pointer travel over zoom", "[gui][nodecanvas][gesture]") {
	Scene scene("number", 2.0f);
	// At zoom 2 the source node draws at (100, 100)-(300, 220).
	scene.Drag({160.0f, 180.0f}, {200.0f, 200.0f});
	const Element *element = scene.Data.Get<Element>(scene.Source);
	CHECK(element->Position.X.Offset == Approx(70.0f));
	CHECK(element->Position.Y.Offset == Approx(60.0f));
	CHECK(scene.Data.Get<Element>(scene.Sink)->Position.X.Offset == Approx(300.0f));
	CHECK(scene.Route.GraphHeld() == NULL_ENTITY);
}

TEST_CASE("dragging empty canvas pans it", "[gui][nodecanvas][gesture]") {
	Scene scene("number", 2.0f);
	scene.Drag({500.0f, 350.0f}, {440.0f, 330.0f});
	const NodeCanvas *canvas = scene.Data.Get<NodeCanvas>(scene.Canvas);
	CHECK(canvas->Pan.X == Approx(30.0f));
	CHECK(canvas->Pan.Y == Approx(10.0f));
	CHECK(canvas->Zoom == Approx(2.0f));
}

TEST_CASE("pulling a wire from an output onto an input links them", "[gui][nodecanvas][gesture]") {
	Scene scene;
	scene.Send(145.0f, 75.0f, false);
	scene.Send(145.0f, 75.0f, true);
	scene.Send(220.0f, 80.0f, true);
	CHECK(scene.Route.GraphHeld() == scene.Canvas);
	NodeCanvasWire wire = scene.Route.NodeWire();
	CHECK(wire.Canvas == scene.Canvas);
	CHECK(wire.From.X == Approx(145.0f));
	CHECK(wire.To.X == Approx(220.0f));
	CHECK(!wire.OverPort);
	const auto over = scene.Send(305.0f, 75.0f, true);
	wire = scene.Route.NodeWire();
	CHECK(wire.OverPort);
	CHECK(wire.Accepts);
	const auto released = scene.Send(305.0f, 75.0f, false);
	CHECK(scene.Links() == 1);
	CHECK(scene.Route.NodeWire().Canvas == NULL_ENTITY);
	// The press became a gesture, so the port it began on is not activated.
	CHECK(!Scene::Has(released, EventKind::Activated, scene.Output));
	CHECK(!Scene::Has(over, EventKind::Activated, scene.Output));
}

TEST_CASE("the pending wire is drawn while it is pulled", "[gui][nodecanvas][gesture]") {
	Scene scene;
	scene.Send(145.0f, 75.0f, false);
	scene.Send(145.0f, 75.0f, true);
	scene.Send(220.0f, 75.0f, true);
	scene.Request.Wire = scene.Route.NodeWire();
	scene.List.Rebuild(scene.Data, scene.Request);
	bool drawn = false;
	for (const DrawCommand &command : scene.List.Commands().Commands) {
		drawn = drawn || (command.Source == scene.Canvas && command.Kind == DrawKind::Rectangle &&
						  command.Bounds.Max.X - command.Bounds.Min.X == Approx(75.0f));
	}
	CHECK(drawn);
	scene.Send(220.0f, 75.0f, false);
}

TEST_CASE("a wire dropped on an incompatible input links nothing", "[gui][nodecanvas][gesture]") {
	Scene scene("string");
	scene.Send(145.0f, 75.0f, false);
	scene.Send(145.0f, 75.0f, true);
	scene.Send(300.0f, 76.0f, true);
	scene.Send(305.0f, 75.0f, true);
	CHECK(scene.Route.NodeWire().OverPort);
	CHECK(!scene.Route.NodeWire().Accepts);
	scene.Send(305.0f, 75.0f, false);
	CHECK(scene.Links() == 0);
}

TEST_CASE("a press released in place is still a click", "[gui][nodecanvas][gesture]") {
	Scene scene;
	scene.Send(145.0f, 75.0f, false);
	const auto pressed = scene.Send(145.0f, 75.0f, true);
	CHECK(Scene::Has(pressed, EventKind::InputBegan, scene.Output));
	// A wobble under the claim distance stays a click.
	scene.Send(146.0f, 76.0f, true);
	const auto released = scene.Send(146.0f, 76.0f, false);
	CHECK(Scene::Has(released, EventKind::Activated, scene.Output));
	CHECK(scene.Links() == 0);
	CHECK(scene.Data.Get<Element>(scene.Source)->Position.X.Offset == Approx(50.0f));
}

TEST_CASE("forgetting the router drops a gesture in flight", "[gui][nodecanvas][gesture]") {
	Scene scene;
	scene.Send(145.0f, 75.0f, false);
	scene.Send(145.0f, 75.0f, true);
	scene.Send(220.0f, 75.0f, true);
	REQUIRE(scene.Route.GraphHeld() == scene.Canvas);
	scene.Route.Forget();
	CHECK(scene.Route.GraphHeld() == NULL_ENTITY);
	CHECK(scene.Route.NodeWire().Canvas == NULL_ENTITY);
	scene.Send(305.0f, 75.0f, false);
	CHECK(scene.Links() == 0);
}
