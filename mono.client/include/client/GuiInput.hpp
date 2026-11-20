#pragma once

// The one place a host hands a frame of input to a GUI canvas.
//
// Both the shipped client and each Studio viewport keep an `input::Translator`
// that turns SDL events into ordered key presses, text edits and pointer
// samples. A host maps those into the canvas it compiled, then calls
// `RouteGuiFrame`, which applies them to the canvas's router in one fixed
// order. See docs/INPUT.md for the whole path from SDL to scripts.

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/input/Translate.hpp>
#include <engine/scene/Input.hpp>

#include <span>
#include <vector>

namespace engine::scene {
	struct Camera;
}

namespace client {

	// One frame of input for one canvas, already in that canvas's coordinates.
	struct GuiFrameInput {
		// The pointer as it stands at the end of the frame. `Inside` is the
		// host's answer to "is the mouse over this canvas and not over chrome".
		engine::gui::Pointer Pointer;

		// Ordered press, release and held-motion samples since the last frame,
		// so a click that went down and up between two frames still clicks.
		std::span<const engine::input::PrimaryPointerSample> Samples;

		// Ordered key presses for navigation and activation.
		std::span<const engine::scene::KeyCode> KeyPresses;

		// Controller presses for navigation and activation.
		engine::scene::ControllerState Controllers;

		// Ordered committed text and editing keys for a focused `TextBox`.
		std::span<const engine::input::PresentationEditEvent> Edits;

		// The input method's uncommitted text, shown in the focused box.
		engine::input::TextComposition Composition;
	};

	// The events one frame produced, kept apart in the order a runtime should
	// hear them: typing happened first, then navigation, then the pointer.
	struct GuiFrameEvents {
		std::vector<engine::gui::GuiEvent> Typed;
		std::vector<engine::gui::GuiEvent> Semantic;
		std::vector<engine::gui::GuiEvent> Pointer;

		bool Empty() const {
			return Typed.empty() && Semantic.empty() && Pointer.empty();
		}

		// All three, in delivery order.
		std::vector<engine::gui::GuiEvent> Ordered() const;
	};

	// Routes navigation keys and typing for one frame.
	//
	// Key and pad navigation first, then typing into the focused box in SDL
	// order. Fills `Semantic` and `Typed` of `out`.
	void RouteGuiKeyboard(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		engine::gui::Router &router,
		const GuiFrameInput &input,
		GuiFrameEvents &out
	);

	// Routes every pointer sample, then the pointer level. Fills `Pointer` of
	// `out`. The camera arguments are the ones `render::RouteGuiPointer` needs
	// to hit spatial canvases; a host with only screen GUI leaves them null.
	void RouteGuiPointerFrame(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Screen &screen,
		engine::gui::Router &router,
		const GuiFrameInput &input,
		GuiFrameEvents &out,
		const engine::scene::Camera *camera = nullptr,
		const engine::core::CFrame *cameraFrame = nullptr,
		float projectionAspect = 0.0f,
		const engine::core::CFrame *geometryFrame = nullptr
	);

	// Routes one whole frame against the list just compiled: the keyboard,
	// then the pointer. Typing goes before the pointer because the characters
	// were aimed at whichever box held the keyboard when they arrived, and a
	// press this frame may move the focus to another one.
	//
	// @param store  The world the list was compiled from.
	// @param list   The list compiled this frame.
	// @param screen The screen the list was compiled for.
	// @param router This canvas's router; its hover and press carry over.
	// @param input  This frame's input, in canvas coordinates.
	// @return The events, grouped for delivery.
	GuiFrameEvents RouteGuiFrame(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Screen &screen,
		engine::gui::Router &router,
		const GuiFrameInput &input
	);
}
