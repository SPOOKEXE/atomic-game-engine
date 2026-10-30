// Device-free pointer routing over retained screen and spatial GUI fixtures.

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/SpatialCanvas.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Bench.hpp>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

TEST_SUITE_ID("client.bench.gui-pointer")

namespace {
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::gui::Pointer;

	void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}

	struct Fixture {
		Store World{"bench.gui-pointer"};
		engine::gui::Router Router;
		engine::gui::Screen Display{.Width = 1600.0f, .Height = 900.0f};
		engine::gui::DrawList List;
		Entity FirstSurface;
		Entity FirstButton;
		Entity Screen;
		Entity Scroller;
		bool ReportedMemory = false;
		bool ReverseWheel = false;

		Fixture(size_t decorations, size_t surfaces, size_t inert, bool modal, bool scroller = false) {
			using namespace engine;
			gui::RegisterGuiClasses();
			scene::EnsureClassTree();
			const Entity camera = World.CreateInstance(scene::PartClass(), "Camera");
			World.Set(camera, scene::Transform{});
			World.Set(camera, scene::Camera{});
			World.SetResource(scene::ActiveCamera{camera});
			Screen = World.CreateInstance(gui::GuiClass("ScreenGui"), "Screen");
			Entity modalDialog;
			if (modal) {
				modalDialog = World.CreateInstance(gui::GuiClass("Frame"), "Dialog");
				World.SetParent(modalDialog, Screen);
				const Entity scope = World.CreateInstance(gui::GuiClass("UIModalScope"), "Modal");
				World.SetParent(scope, modalDialog);
			}

			for (size_t index = 0; index < surfaces; ++index) {
				const Entity surface = World.CreateInstance(gui::GuiClass("SurfaceGui"), "Surface");
				gui::SpatialCanvas spatial;
				spatial.Origin = {-2.0f, 1.0f, -10.0f - static_cast<float>(index)};
				spatial.AxisX = {4.0f, 0.0f, 0.0f};
				spatial.AxisY = {0.0f, -2.0f, 0.0f};
				spatial.Normal = {0.0f, 0.0f, 1.0f};
				spatial.Size = {400.0f, 200.0f};
				spatial.Interactive = true;
				World.Set(surface, spatial);
				const Entity button = World.CreateInstance(gui::GuiClass("TextButton"), "Button");
				World.SetParent(button, surface);
				gui::DrawCommand command;
				command.Source = button;
				command.Collector = surface;
				command.Spatial = true;
				command.Bounds = core::Rect{0.0f, 0.0f, 400.0f, 200.0f};
				command.Clip = command.Bounds;
				List.Commands.push_back(command);
				if (index == 0) {
					FirstSurface = surface;
					FirstButton = button;
				}
			}

			for (size_t index = 0; index < decorations; ++index) {
				const Entity frame = World.CreateInstance(gui::GuiClass("Frame"), "Decoration");
				World.SetParent(frame, modal && index == 0 ? modalDialog : Screen);
				gui::DrawCommand command;
				command.Source = frame;
				command.Collector = Screen;
				command.Bounds = core::Rect{0.0f, 0.0f, 100.0f, 100.0f};
				command.Clip = command.Bounds;
				List.Commands.push_back(command);
			}
			if (modal) {
				Require(decorations > 0, "GUI pointer modal requires a scoped command");
				const Entity outside = World.CreateInstance(gui::GuiClass("TextButton"), "Outside");
				World.SetParent(outside, Screen);
				gui::DrawList probe;
				gui::DrawCommand target;
				target.Source = outside;
				target.Collector = Screen;
				target.Bounds = core::Rect{700.0f, 350.0f, 900.0f, 550.0f};
				target.Clip = target.Bounds;
				probe.Commands.push_back(target);
				probe.Commands.push_back(List.Commands[surfaces]);
				Pointer pointer;
				pointer.Position = {800.0f, 450.0f};
				pointer.ScreenOnly = true;
				gui::Router probeRouter;
				Require(
					!probeRouter.WantsPointer(World, probe, pointer),
					"GUI modal failed to block outside target"
				);
				probe.Commands.pop_back();
				Require(
					probeRouter.WantsPointer(World, probe, pointer), "GUI modal control target unavailable"
				);
			}
			if (scroller) {
				Scroller = World.CreateInstance(gui::GuiClass("ScrollingFrame"), "List");
				World.SetParent(Scroller, Screen);
				gui::Scrolling scrolling;
				scrolling.CanvasPosition = {0.0f, 100.0f};
				World.Set(Scroller, scrolling);
				gui::ScrollState state;
				state.CanvasSize = {200.0f, 400.0f};
				state.WindowSize = {200.0f, 200.0f};
				World.Set(Scroller, state);
				gui::Resolved resolved;
				resolved.AbsolutePosition = {700.0f, 350.0f};
				resolved.AbsoluteSize = {200.0f, 200.0f};
				resolved.Clip = core::Rect{700.0f, 350.0f, 900.0f, 550.0f};
				resolved.Rendered = true;
				World.Set(Scroller, resolved);
				gui::DrawCommand command;
				command.Source = Scroller;
				command.Collector = Screen;
				command.Bounds = resolved.Clip;
				command.Clip = resolved.Clip;
				List.Commands.push_back(command);
			}
			if (inert > 0) {
				const Entity folder = World.CreateInstance(ecs::Classes::Find(Name("Folder")), "World");
				Require(folder != ecs::NULL_ENTITY, "GUI pointer world folder unavailable");
				for (size_t index = 0; index < inert; ++index) {
					const Entity part = World.CreateInstance(scene::PartClass(), "Part");
					World.SetParent(part, folder);
				}
			}

			// Check projected ownership and event order once, before timed samples.
			Pointer pointer;
			pointer.Position = {800.0f, 450.0f};
			const Pointer routed = Route(pointer);
			Check(routed, false);
			if (FirstSurface != ecs::NULL_ENTITY) {
				const auto hover = Router.Update(World, List, routed);
				Require(
					hover.size() == 1 && hover[0].Kind == gui::EventKind::MouseEnter &&
						hover[0].Instance == FirstButton,
					"GUI pointer hover oracle changed"
				);
				Pointer down = routed;
				down.Down = true;
				const auto pressed = Router.Update(World, List, down);
				Require(
					pressed.size() == 1 && pressed[0].Kind == gui::EventKind::InputBegan &&
						pressed[0].Instance == FirstButton && Router.Pressed() == FirstButton,
					"GUI pointer press oracle changed"
				);
				const auto released = Router.Update(World, List, routed);
				Require(
					released.size() == 2 && released[0].Kind == gui::EventKind::InputEnded &&
						released[1].Kind == gui::EventKind::Activated &&
						released[0].Instance == FirstButton && released[1].Instance == FirstButton,
					"GUI pointer release oracle changed"
				);
			}
			if (scroller) Router.Forget();
		}

		Pointer Route(Pointer pointer) {
			return engine::render::RouteGuiPointer(World, List, Display, Router, pointer);
		}

		void Check(const Pointer &routed, bool screenWheel) const {
			if (FirstSurface == engine::ecs::NULL_ENTITY || screenWheel) {
				Require(
					routed.ScreenOnly && routed.Collector == engine::ecs::NULL_ENTITY &&
						std::abs(routed.Position.X - 800.0f) < 0.1f &&
						std::abs(routed.Position.Y - 450.0f) < 0.1f,
					"GUI pointer screen oracle changed"
				);
				return;
			}
			Require(
				!routed.ScreenOnly && routed.Collector == FirstSurface &&
					std::abs(routed.Position.X - 200.0f) < 0.1f &&
					std::abs(routed.Position.Y - 100.0f) < 0.1f,
				"GUI pointer spatial oracle changed"
			);
		}

		void MeasureMemory(std::string_view name, Pointer pointer) {
			if (ReportedMemory) return;
			ReportedMemory = true;
			const char *setting = std::getenv("MONO_GUI_POINTER_MEMORY");
			if (setting == nullptr || std::string_view(setting) != "1") return;
			Require(engine::core::HeapProfile::IsCompiledIn(), "GUI pointer heap hooks are unavailable");
			const auto before = engine::core::HeapProfile::Totals();
			const Pointer routed = Route(pointer);
			const auto after = engine::core::HeapProfile::Totals();
			Check(routed, Scroller != engine::ecs::NULL_ENTITY && pointer.Wheel != 0.0f);
			Require(after.DroppedScopes == before.DroppedScopes, "GUI pointer heap scopes dropped");
			std::cout << "gui-pointer-heap row=" << name << " scope=route-only gpu=not-used timing_valid=0"
					  << " allocated_bytes=" << after.TotalBytes - before.TotalBytes
					  << " allocated_blocks=" << after.TotalBlocks - before.TotalBlocks
					  << " live_bytes_delta=" << after.LiveBytes - before.LiveBytes
					  << " live_blocks_delta=" << after.LiveBlocks - before.LiveBlocks
					  << " process_lifetime_peak_bytes=" << after.PeakBytes << '\n';
		}
	};

	void Route(Fixture &fixture, std::string_view name, bool wheel = false) {
		Pointer pointer;
		pointer.Position = {800.0f, 450.0f};
		pointer.Wheel = wheel ? -1.0f : 0.0f;
		fixture.MeasureMemory(name, pointer);
		const Pointer routed = fixture.Route(pointer);
		fixture.Check(routed, fixture.Scroller != engine::ecs::NULL_ENTITY && wheel);
		engine::testing::Consume(routed.Position.X);
		engine::testing::Consume(routed.Collector.Id);
	}

	void PressRelease(Fixture &fixture) {
		using engine::gui::EventKind;
		Pointer pointer;
		pointer.Position = {800.0f, 450.0f};
		pointer.Down = true;
		const Pointer down = fixture.Route(pointer);
		fixture.Check(down, false);
		const auto pressed = fixture.Router.Update(fixture.World, fixture.List, down);
		Require(
			pressed.size() == 1 && pressed[0].Kind == EventKind::InputBegan &&
				pressed[0].Instance == fixture.FirstButton && fixture.Router.Pressed() == fixture.FirstButton,
			"GUI pointer benchmark press changed"
		);
		pointer.Down = false;
		const Pointer up = fixture.Route(pointer);
		fixture.Check(up, false);
		const auto released = fixture.Router.Update(fixture.World, fixture.List, up);
		Require(
			released.size() == 2 && released[0].Kind == EventKind::InputEnded &&
				released[1].Kind == EventKind::Activated && released[0].Instance == fixture.FirstButton &&
				released[1].Instance == fixture.FirstButton &&
				fixture.Router.Pressed() == engine::ecs::NULL_ENTITY,
			"GUI pointer benchmark release changed"
		);
		engine::testing::Consume(released.size());
	}
}

BENCH("GUI pointer empty", 1) {
	static Fixture fixture(0, 0, 0, false);
	Route(fixture, "empty");
}
BENCH("GUI pointer screen decorations", 1) {
	static Fixture fixture(256, 0, 0, false);
	Route(fixture, "screen-decorations");
}
BENCH("GUI pointer one spatial canvas", 1) {
	static Fixture fixture(256, 1, 0, false);
	Route(fixture, "spatial-one");
}
BENCH("GUI pointer eight spatial canvases", 1) {
	static Fixture fixture(256, 8, 0, false);
	Route(fixture, "spatial-eight");
}
BENCH("GUI pointer eight spatial canvases with modal", 1) {
	static Fixture fixture(256, 8, 0, true);
	Route(fixture, "spatial-modal");
}
BENCH("GUI wheel miss empty world", 1) {
	static Fixture fixture(0, 0, 0, false);
	Route(fixture, "wheel-empty", true);
}
BENCH("GUI wheel miss 4096 world parts", 1) {
	static Fixture fixture(0, 0, 4096, false);
	Route(fixture, "wheel-world", true);
}
BENCH("GUI wheel screen scroller and spatial canvas", 1) {
	static Fixture fixture(0, 1, 0, false, true);
	Pointer pointer;
	pointer.Position = {800.0f, 450.0f};
	pointer.Wheel = fixture.ReverseWheel ? 1.0f : -1.0f;
	fixture.ReverseWheel = !fixture.ReverseWheel;
	fixture.MeasureMemory("wheel-screen", pointer);
	const Pointer routed = fixture.Route(pointer);
	fixture.Check(routed, true);
	const float before = fixture.World.Get<engine::gui::Scrolling>(fixture.Scroller)->CanvasPosition.Y;
	const auto events = fixture.Router.Update(fixture.World, fixture.List, routed);
	const float after = fixture.World.Get<engine::gui::Scrolling>(fixture.Scroller)->CanvasPosition.Y;
	Require(
		events.empty() && std::abs((after - before) + pointer.Wheel * 60.0f) < 0.01f,
		"GUI pointer wheel oracle changed"
	);
	engine::testing::Consume(after);
}
BENCH("GUI pointer spatial press and release", 1) {
	static Fixture fixture(64, 1, 0, false);
	static bool reportedMemory = false;
	if (!reportedMemory) {
		reportedMemory = true;
		const char *setting = std::getenv("MONO_GUI_POINTER_MEMORY");
		if (setting != nullptr && std::string_view(setting) == "1") {
			Require(engine::core::HeapProfile::IsCompiledIn(), "GUI pointer heap hooks are unavailable");
			const auto before = engine::core::HeapProfile::Totals();
			PressRelease(fixture);
			const auto after = engine::core::HeapProfile::Totals();
			Require(after.DroppedScopes == before.DroppedScopes, "GUI pointer heap scopes dropped");
			std::cout << "gui-pointer-heap row=spatial-press-release scope=route-and-update gpu=not-used "
						 "timing_valid=0"
					  << " allocated_bytes=" << after.TotalBytes - before.TotalBytes
					  << " allocated_blocks=" << after.TotalBlocks - before.TotalBlocks
					  << " live_bytes_delta=" << after.LiveBytes - before.LiveBytes
					  << " live_blocks_delta=" << after.LiveBlocks - before.LiveBlocks
					  << " process_lifetime_peak_bytes=" << after.PeakBytes << '\n';
		}
	}
	PressRelease(fixture);
}
