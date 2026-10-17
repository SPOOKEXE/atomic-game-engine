// Studio-owned composition for optional control rows.

#include "ControlAutomation.hpp"

#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/Script.hpp>
#include <engine/control/features/Universe.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <studio/DataFactoryHost.hpp>
#include <studio/Editor.hpp>

namespace studio {
	using nlohmann::json;

	namespace {
		// Studio control rows borrow these editor-owned services for their lease lifetime.
		struct StudioControlHookContext {
			Editor &Host;
			engine::control::Surface &Surface;
			engine::world::Universe &Universe;
		};

		uint8_t MouseButton(std::string_view button) {
			if (button == "left") return SDL_BUTTON_LEFT;
			if (button == "middle") return SDL_BUTTON_MIDDLE;
			if (button == "right") return SDL_BUTTON_RIGHT;
			return 0;
		}

		SDL_Keymod KeyboardModifiers(uint8_t modifiers) {
			SDL_Keymod translated = SDL_KMOD_NONE;
			if ((modifiers & automation::KeyboardModifierShift) != 0) translated |= SDL_KMOD_SHIFT;
			if ((modifiers & automation::KeyboardModifierControl) != 0) translated |= SDL_KMOD_CTRL;
			if ((modifiers & automation::KeyboardModifierAlt) != 0) translated |= SDL_KMOD_ALT;
			if ((modifiers & automation::KeyboardModifierGui) != 0) translated |= SDL_KMOD_GUI;
			return translated;
		}
	}

	void Editor::ActivateControlHooks() {
		const StudioControlHookContext context{
			.Host = *this, .Surface = ControlSurface, .Universe = *Universe
		};
		std::string failure;
		StudioControlHook = context.Surface.ActivateHook(
			{
				.Id = "studio.product",
				.Revision = "v1",
				.Purpose = "Editor-only selection, automation, and play controls.",
				.Dependencies = {},
				.Limits = {},
			},
			[context](engine::control::HookRegistration &registration) {
				context.Host.RegisterControlTools(registration);
			},
			failure
		);
		if (!failure.empty()) throw std::runtime_error("could not activate Studio control hook: " + failure);

		StudioSceneRenderingHook = context.Surface.ActivateHook(
			{
				.Id = "studio.scene-rendering",
				.Revision = "v1",
				.Purpose = "Reads camera calibration for Studio's rendered scene.",
				.Dependencies = {},
				.Limits = {},
			},
			[context](engine::control::HookRegistration &registration) {
				registration.Add(engine::control::features::CameraRenderingDataTool(context.Universe));
			},
			failure
		);
		if (!failure.empty()) {
			StudioControlHook.Close();
			throw std::runtime_error("could not activate Studio scene-rendering control hook: " + failure);
		}
	}

	void Editor::EnableControlFeatures() {
		struct PermanentHook {
			const char *Id;
			std::function<void()> Install;
		};
		struct StudioControlManifest {
			std::array<PermanentHook, 6> Factory;
			std::array<PermanentHook, 8> Interactive;
		};
		const StudioControlManifest manifest{
			.Factory =
				{
					PermanentHook{"builtin.architecture", [this] { ControlSurface.AddArchitectureTools(); }},
					PermanentHook{"builtin.script", [this] { ControlSurface.AddScriptTools(); }},
					PermanentHook{"builtin.diagnostics", [this] { ControlSurface.AddDiagnosticTools(); }},
					PermanentHook{"builtin.resources", [this] { ControlSurface.AddStandardResources(); }},
					PermanentHook{"builtin.prompts", [this] { ControlSurface.AddStandardPrompts(); }},
					PermanentHook{"builtin.discovery", [this] { ControlSurface.AddDiscoveryTools(); }},
				},
			.Interactive = {
				// Studio owns its richer engine_info row through its product hook.
				PermanentHook{
					"builtin.universe", [this] { ControlSurface.AddUniverseTools(*Universe, true, false); }
				},
				PermanentHook{"builtin.architecture", [this] { ControlSurface.AddArchitectureTools(); }},
				PermanentHook{"builtin.script", [this] { ControlSurface.AddScriptTools(); }},
				PermanentHook{"builtin.diagnostics", [this] { ControlSurface.AddDiagnosticTools(false); }},
				PermanentHook{"builtin.build", [this] { ControlSurface.AddBuildTools(); }},
				PermanentHook{"builtin.resources", [this] { ControlSurface.AddStandardResources(); }},
				PermanentHook{"builtin.prompts", [this] { ControlSurface.AddStandardPrompts(); }},
				PermanentHook{"builtin.discovery", [this] { ControlSurface.AddDiscoveryTools(); }},
			},
		};
		const auto install = [this](const auto &hooks) {
			PermanentControlHooks.reserve(hooks.size());
			for (const PermanentHook &hook : hooks) {
				std::string failure;
				auto lease = ControlSurface.ActivateHook(
					{.Id = hook.Id,
					 .Revision = "v1",
					 .Purpose = "Built-in feature registration.",
					 .Dependencies = {},
					 .Limits = {}},
					[&hook](engine::control::HookRegistration &) { hook.Install(); },
					failure
				);
				if (!lease.IsValid()) throw std::runtime_error(failure);
				PermanentControlHooks.push_back(std::move(lease));
			}
		};
		if (FactoryHost != nullptr) {
			// A factory session owns world state and lifecycle. Ordinary Studio
			// universe tools would mutate the same world outside that session.
			install(manifest.Factory);
			FactoryHost->InstallTools({.Surface = ControlSurface, .RendererReady = true});
			return;
		}

		install(manifest.Interactive);
		ActivateControlHooks();
		struct StudioInputControlHookContext {
			Editor &Host;
			engine::control::Surface &Surface;
		};
		const StudioInputControlHookContext inputContext{.Host = *this, .Surface = ControlSurface};
		std::string inputFailure;
		StudioInputControlHook = inputContext.Surface.ActivateHook(
			{.Id = "studio.input",
			 .Revision = "v1",
			 .Purpose = "Queues Studio input through the active window boundary.",
			 .Dependencies = {"studio.product"},
			 .Limits = {{"queued_events", 64}}},
			[inputContext](engine::control::HookRegistration &) {
				inputContext.Surface.AddInputTools(
					[inputContext](const engine::control::InputAutomationEvent &event, std::string &failure) {
						Editor &host = inputContext.Host;
						const uint32_t window = host.Window == nullptr ? 0 : SDL_GetWindowID(host.Window);
						const auto push = [&](SDL_Event &input, const char *what) {
							if (!SDL_PushEvent(&input)) {
								failure = std::string("could not queue ") + what + ": " + SDL_GetError();
								return false;
							}
							host.Interface.QueueAutomationEvent(input);
							return true;
						};
						const auto motion = [&](float x, float y) {
							SDL_Event input{};
							input.type = SDL_EVENT_MOUSE_MOTION;
							input.motion.timestamp = SDL_GetTicksNS();
							input.motion.windowID = window;
							input.motion.x = x;
							input.motion.y = y;
							return push(input, "mouse motion");
						};
						switch (event.Kind) {
						case engine::control::InputAutomationKind::MouseMove:
							return motion(event.X, event.Y)
									   ? json{{"queued", true}, {"x", event.X}, {"y", event.Y}}
									   : json(nullptr);
						case engine::control::InputAutomationKind::MouseButton: {
							if (event.State == engine::control::InputAutomationState::Click &&
								host.PendingControlClick.has_value()) {
								failure = "an emulated click is already in progress";
								return json(nullptr);
							}
							const uint8_t button = MouseButton(event.Button);
							if (!motion(event.X, event.Y)) return json(nullptr);
							SDL_Event input{};
							input.type = event.State == engine::control::InputAutomationState::Up
											 ? SDL_EVENT_MOUSE_BUTTON_UP
											 : SDL_EVENT_MOUSE_BUTTON_DOWN;
							input.button.timestamp = SDL_GetTicksNS();
							input.button.windowID = window;
							input.button.button = button;
							input.button.down = event.State != engine::control::InputAutomationState::Up;
							input.button.clicks = 1;
							input.button.x = event.X;
							input.button.y = event.Y;
							if (!push(input, "mouse button")) return json(nullptr);
							if (event.State == engine::control::InputAutomationState::Click) {
								host.PendingControlClick =
									Editor::ControlClick{event.X, event.Y, button, false};
							}
							return json{
								{"queued", true}, {"x", event.X}, {"y", event.Y}, {"button", event.Button}
							};
						}
						case engine::control::InputAutomationKind::MouseWheel: {
							SDL_Event input{};
							input.type = SDL_EVENT_MOUSE_WHEEL;
							input.wheel.timestamp = SDL_GetTicksNS();
							input.wheel.windowID = window;
							input.wheel.y = event.Wheel;
							return push(input, "mouse wheel")
									   ? json{{"queued", true}, {"notches", event.Wheel}}
									   : json(nullptr);
						}
						case engine::control::InputAutomationKind::Key: {
							if (event.State == engine::control::InputAutomationState::Click &&
								host.PendingControlKey.has_value()) {
								failure = "an emulated key is already in progress";
								return json(nullptr);
							}
							uint8_t modifierBits = automation::KeyboardModifierNone;
							if (!automation::ParseKeyboardModifiers(event.Modifiers, modifierBits, failure))
								return json(nullptr);
							SDL_Keymod modifiers = KeyboardModifiers(modifierBits);
							SDL_Keycode key = SDL_GetKeyFromName(event.Key.c_str());
							SDL_Scancode scancode = SDL_GetScancodeFromName(event.Key.c_str());
							if (key == SDLK_UNKNOWN && scancode == SDL_SCANCODE_UNKNOWN) {
								failure = "key is not a recognized SDL key name";
								return json(nullptr);
							}
							if (scancode == SDL_SCANCODE_UNKNOWN)
								scancode = SDL_GetScancodeFromKey(key, &modifiers);
							if (key == SDLK_UNKNOWN) key = SDL_GetKeyFromScancode(scancode, modifiers, true);
							SDL_Event input{};
							input.type = event.State == engine::control::InputAutomationState::Up
											 ? SDL_EVENT_KEY_UP
											 : SDL_EVENT_KEY_DOWN;
							input.key.timestamp = SDL_GetTicksNS();
							input.key.windowID = window;
							input.key.scancode = scancode;
							input.key.key = key;
							input.key.mod = modifiers;
							input.key.down = event.State != engine::control::InputAutomationState::Up;
							if (!push(input, "key")) return json(nullptr);
							if (event.State == engine::control::InputAutomationState::Click) {
								host.PendingControlKey = Editor::ControlKey{
									static_cast<uint32_t>(scancode),
									static_cast<uint32_t>(key),
									static_cast<uint16_t>(modifiers),
									false
								};
							}
							return json{{"queued", true}, {"key", event.Key}, {"modifiers", event.Modifiers}};
						}
						case engine::control::InputAutomationKind::Text: {
							if (host.PendingControlText.has_value()) {
								failure = "emulated text is already in progress";
								return json(nullptr);
							}
							host.PendingControlText = Editor::ControlText{event.Text, false};
							SDL_Event input{};
							input.type = SDL_EVENT_TEXT_INPUT;
							input.text.timestamp = SDL_GetTicksNS();
							input.text.windowID = window;
							input.text.text = host.PendingControlText->Text.c_str();
							if (!push(input, "text input")) {
								host.PendingControlText.reset();
								return json(nullptr);
							}
							return json{{"queued", true}, {"bytes", event.Text.size()}};
						}
						}
						failure = "input event is unknown";
						return json(nullptr);
					}
				);
			},
			inputFailure
		);
		if (!StudioInputControlHook.IsValid()) {
			StudioSceneRenderingHook.Close();
			StudioControlHook.Close();
			throw std::runtime_error("could not activate Studio input control hook: " + inputFailure);
		}
	}
}
