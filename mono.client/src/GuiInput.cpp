#include "GuiActions.hpp"

#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/render/SpatialCanvas.hpp>

#include <client/GuiInput.hpp>

namespace client {

	std::vector<engine::gui::GuiEvent> GuiFrameEvents::Ordered() const {
		std::vector<engine::gui::GuiEvent> all;
		all.reserve(Typed.size() + Semantic.size() + Pointer.size());
		all.insert(all.end(), Typed.begin(), Typed.end());
		all.insert(all.end(), Semantic.begin(), Semantic.end());
		all.insert(all.end(), Pointer.begin(), Pointer.end());
		return all;
	}

	void RouteGuiKeyboard(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		engine::gui::Router &router,
		const GuiFrameInput &input,
		GuiFrameEvents &out
	) {
		// Navigation, one key at a time so two presses in one frame move twice.
		// The router's span is reused by its next call, so each is copied out.
		const auto routeActions = [&](const engine::scene::InputState &keyboard,
									  const engine::scene::ControllerState &controllers) {
			const bool textEditing = engine::gui::FocusedTextBox(store) != engine::ecs::NULL_ENTITY;
			for (const engine::gui::SemanticAction action : GuiActions(keyboard, controllers, textEditing)) {
				const auto events = router.UpdateSemantic(store, list, action);
				out.Semantic.insert(out.Semantic.end(), events.begin(), events.end());
			}
		};
		const engine::scene::ControllerState noControllers;
		for (const engine::scene::KeyCode key : input.KeyPresses) {
			engine::scene::InputState oneKey;
			oneKey.Down.Set(key, true);
			routeActions(oneKey, noControllers);
		}
		routeActions(engine::scene::InputState{}, input.Controllers);

		// Typing, in SDL order.
		engine::gui::Typing composing;
		composing.Preedit = input.Composition.Text;
		composing.PreeditStart = input.Composition.Start;
		composing.PreeditLength = input.Composition.Length;
		composing.PreeditRevision = input.Composition.Revision;
		const auto applyTyping = [&](const engine::gui::Typing &edit) {
			if (const engine::gui::TypeResult typed = engine::gui::Type(store, edit); typed.Released) {
				engine::gui::GuiEvent released;
				released.Kind = engine::gui::EventKind::FocusReleased;
				released.Instance = typed.Instance;
				released.Entered = true;
				out.Typed.push_back(released);
			}
		};
		for (const engine::input::PresentationEditEvent &event : input.Edits) {
			engine::gui::Typing edit = composing;
			switch (event.Kind) {
			case engine::input::PresentationEditKind::Text:
				edit.Text = event.Text;
				break;
			case engine::input::PresentationEditKind::Backspace:
				edit.Backspace = true;
				break;
			case engine::input::PresentationEditKind::Submit:
				edit.Submit = true;
				break;
			case engine::input::PresentationEditKind::CaretLeft:
				edit.Caret = -1;
				edit.Extend = event.Extend;
				break;
			case engine::input::PresentationEditKind::CaretRight:
				edit.Caret = 1;
				edit.Extend = event.Extend;
				break;
			}
			applyTyping(edit);
		}
		applyTyping(composing);
	}

	void RouteGuiPointerFrame(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Screen &screen,
		engine::gui::Router &router,
		const GuiFrameInput &input,
		GuiFrameEvents &out,
		const engine::scene::Camera *camera,
		const engine::core::CFrame *cameraFrame,
		float projectionAspect,
		const engine::core::CFrame *geometryFrame
	) {
		// Every press and release edge, then the level. The router is level
		// based, so without the samples a click that went down and up between
		// two frames would never reach it.
		const auto routePointer = [&](engine::gui::Pointer pointer) {
			pointer = engine::render::RouteGuiPointer(
				store, list, screen, router, pointer, camera, cameraFrame, projectionAspect, geometryFrame
			);
			const auto events = router.Update(store, list, pointer);
			out.Pointer.insert(out.Pointer.end(), events.begin(), events.end());
		};
		for (const engine::input::PrimaryPointerSample &sample : input.Samples) {
			engine::gui::Pointer edge = input.Pointer;
			edge.Position = sample.Position;
			edge.Down = sample.Down;
			edge.Cancelled = sample.Cancelled;
			edge.Wheel = 0.0f;
			routePointer(edge);
		}
		routePointer(input.Pointer);
	}

	GuiFrameEvents RouteGuiFrame(
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Screen &screen,
		engine::gui::Router &router,
		const GuiFrameInput &input
	) {
		GuiFrameEvents out;
		RouteGuiKeyboard(store, list, router, input, out);
		RouteGuiPointerFrame(store, list, screen, router, input, out);
		return out;
	}
}
