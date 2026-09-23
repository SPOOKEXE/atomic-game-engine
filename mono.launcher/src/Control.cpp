#include <engine/control/Features.hpp>
#include <engine/core/Log.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <launcher/Launcher.hpp>
#include <launcher/Programs.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace launcher {
	using nlohmann::json;

	void Launcher::InstallControl() {
		const std::array features{
			engine::control::features::Architecture(),
			engine::control::features::Diagnostics(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
			engine::control::features::Discovery(),
		};
		ControlSurface.Enable(features);
		ControlSurface.AddInputTools(
			[this](const engine::control::InputAutomationEvent &event, std::string &failure) {
				if (event.Kind == engine::control::InputAutomationKind::Key &&
					SDL_GetKeyFromName(event.Key.c_str()) == SDLK_UNKNOWN &&
					SDL_GetScancodeFromName(event.Key.c_str()) == SDL_SCANCODE_UNKNOWN) {
					failure = "key is not a recognized SDL key name";
					return json(nullptr);
				}
				int width = Settings.Width;
				int height = Settings.Height;
				if (Window != nullptr) (void)SDL_GetWindowSize(Window, &width, &height);
				if ((event.Kind == engine::control::InputAutomationKind::MouseMove ||
					 event.Kind == engine::control::InputAutomationKind::MouseButton) &&
					(event.X < 0.0f || event.Y < 0.0f || event.X >= width || event.Y >= height)) {
					failure = "pointer coordinates are outside the launcher area";
					return json(nullptr);
				}
				DispatchControlInput(event);
				if (event.State == engine::control::InputAutomationState::Click &&
					(event.Kind == engine::control::InputAutomationKind::MouseButton ||
					 event.Kind == engine::control::InputAutomationKind::Key)) {
					auto release = event;
					release.State = engine::control::InputAutomationState::Up;
					DeferredControlRelease.push_back(std::move(release));
				}
				return json{{"queued", true}};
			}
		);

		ControlSurface.Add(
			engine::control::Tool{
				"launcher_status",
				"List launcher modes, the open form, and the supervised child's current state.",
				[] { return json{{"type", "object"}}; },
				[this](const json &, std::string &) {
					json modes = json::array();
					for (const Mode &mode : Catalogue) {
						modes.push_back({
							{"id", mode.Id},
							{"program", mode.Program},
							{"available", ProgramPresent(ProgramPath(Stage, mode.Program))},
						});
					}
					json commandLine = json::array();
					if (const Mode *mode = FindMode(Catalogue, Open_)) {
						if (const Description *description = Programs.Find(mode->Program)) {
							if (const auto form = Forms.find(mode->Id); form != Forms.end()) {
								commandLine = CommandLine(form->second, *description);
							}
						}
					}
					const char *state = "idle";
					switch (Child.State()) {
					case ChildState::Running:
						state = "running";
						break;
					case ChildState::Ended:
						state = "ended";
						break;
					case ChildState::Failed:
						state = "failed";
						break;
					case ChildState::Idle:
						break;
					}
					return json{
						{"modes", std::move(modes)},
						{"open_mode", Open_},
						{"command_line", std::move(commandLine)},
						{"child_state", state},
						{"child_summary", Child.Summary()},
						{"child_id", Child.Id()},
						{"failure", Failure},
					};
				}
			}
		);
		ControlSurface.Add(
			engine::control::Tool{
				"launcher_open_mode",
				"Open a launcher mode and its form. Use launcher_status to see available mode ids.",
				[] {
					return json{
						{"type", "object"},
						{"properties", {{"mode", {{"type", "string"}}}}},
						{"required", {"mode"}}
					};
				},
				[this](const json &arguments, std::string &failure) {
					if (!arguments.contains("mode") || !arguments["mode"].is_string()) {
						failure = "mode must be a string";
						return json();
					}
					const std::string modeId = arguments["mode"].get<std::string>();
					const Mode *mode = FindMode(Catalogue, modeId);
					if (mode == nullptr || !ProgramPresent(ProgramPath(Stage, mode->Program))) {
						failure = "mode is unknown or its program is not staged";
						return json();
					}
					Open(*mode);
					return json{{"open_mode", Open_}};
				}
			}
		);
		ControlSurface.Add(
			engine::control::Tool{
				"launcher_set_option",
				"Set one declared option on the open mode. Values may contain several entries for repeatable "
				"options.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 {{"name", {{"type", "string"}}},
						  {"enabled", {{"type", "boolean"}}},
						  {"values", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
						{"required", {"name"}}
					};
				},
				[this](const json &arguments, std::string &failure) {
					if (Open_.empty() || !arguments.contains("name") || !arguments["name"].is_string()) {
						failure = "open a mode and supply an option name first";
						return json();
					}
					const Mode *mode = FindMode(Catalogue, Open_);
					const Description *description = mode == nullptr ? nullptr : Programs.Find(mode->Program);
					const std::string name = arguments["name"].get<std::string>();
					const DescribedOption *option =
						description == nullptr ? nullptr : description->Option(name);
					if (option == nullptr) {
						failure = "the open program does not declare that option";
						return json();
					}
					if (arguments.contains("enabled") && !arguments["enabled"].is_boolean()) {
						failure = "enabled must be a boolean";
						return json();
					}
					const bool enabled = arguments.value("enabled", true);
					std::vector<std::string> values;
					if (arguments.contains("values")) {
						if (!arguments["values"].is_array()) {
							failure = "values must be an array of strings";
							return json();
						}
						for (const json &value : arguments["values"]) {
							if (!value.is_string()) {
								failure = "values must be an array of strings";
								return json();
							}
							values.push_back(value.get<std::string>());
						}
					}
					if (enabled && option->TakesValue && values.empty()) {
						failure = "this option requires at least one value";
						return json();
					}
					if (!option->TakesValue && !values.empty()) {
						failure = "this option is a bare flag and takes no values";
						return json();
					}
					Form &form = Forms.at(Open_);
					size_t row = 0;
					while (row < form.Options.size() && form.Options[row].Option != name)
						row++;
					if (row == form.Options.size()) {
						failure = "the option has no form row";
						return json();
					}
					for (size_t extra = form.Options.size(); extra-- > row + 1;) {
						if (form.Options[extra].Option == name) (void)RemoveRow(form, extra);
					}
					if (enabled && option->TakesValue) SetRows(form, row, values);
					form.Options[row].Enabled = enabled;
					if (enabled && option->TakesValue) {
						for (size_t index = row + 1;
							 index < form.Options.size() && form.Options[index].Option == name;
							 index++)
							form.Options[index].Enabled = true;
					}
					return json{{"name", name}, {"enabled", enabled}, {"values", values}};
				}
			}
		);
		ControlSurface.Add(
			engine::control::Tool{
				"launcher_launch",
				"Start the open mode with the current form options.",
				[] { return json{{"type", "object"}}; },
				[this](const json &, std::string &failure) {
					if (Open_.empty()) {
						failure = "open a mode first";
						return json();
					}
					Launch();
					if (!Child.Running()) {
						failure = Failure.empty() ? "the child did not start" : Failure;
						return json();
					}
					return json{{"child_id", Child.Id()}};
				}
			}
		);
		ControlSurface.Add(
			engine::control::Tool{
				"launcher_stop",
				"Request a polite stop of the supervised child.",
				[] { return json{{"type", "object"}}; },
				[this](const json &, std::string &) {
					const bool running = Child.Running();
					if (running) Child.RequestStop();
					return json{{"stop_requested", running}};
				}
			}
		);
		ControlSurface.Add(
			engine::control::Tool{
				"launcher_quit",
				"Close the launcher and stop its supervised child. Also ends headless MCP-only sessions.",
				[] { return json{{"type", "object"}}; },
				[this](const json &, std::string &) {
					Quit = true;
					return json{{"quitting", true}};
				}
			}
		);
	}

	void Launcher::PumpControl() {
		if (!ControlServer.IsRunning()) return;
		std::vector<engine::control::InputAutomationEvent> releases;
		releases.swap(DeferredControlRelease);
		for (const auto &event : releases)
			DispatchControlInput(event);
		ControlServer.Pump([this](const std::string &line) { return ControlSurface.Answer(line); });
	}

	void Launcher::DispatchControlInput(const engine::control::InputAutomationEvent &input) {
		SDL_Event event{};
		event.common.timestamp = SDL_GetTicksNS();
		const uint32_t window = Window == nullptr ? 0 : SDL_GetWindowID(Window);
		switch (input.Kind) {
		case engine::control::InputAutomationKind::MouseMove:
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = window;
			event.motion.x = input.X;
			event.motion.y = input.Y;
			break;
		case engine::control::InputAutomationKind::MouseButton: {
			event.type = input.State == engine::control::InputAutomationState::Up
							 ? SDL_EVENT_MOUSE_BUTTON_UP
							 : SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.windowID = window;
			event.button.button = input.Button == "right"	 ? SDL_BUTTON_RIGHT
								  : input.Button == "middle" ? SDL_BUTTON_MIDDLE
															 : SDL_BUTTON_LEFT;
			event.button.down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.clicks = 1;
			event.button.x = input.X;
			event.button.y = input.Y;
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = window;
			motion.motion.x = input.X;
			motion.motion.y = input.Y;
			Interface.ProcessEvent(motion);
			break;
		}
		case engine::control::InputAutomationKind::MouseWheel:
			event.type = SDL_EVENT_MOUSE_WHEEL;
			event.wheel.windowID = window;
			event.wheel.y = input.Wheel;
			break;
		case engine::control::InputAutomationKind::Key: {
			event.type = input.State == engine::control::InputAutomationState::Up ? SDL_EVENT_KEY_UP
																				  : SDL_EVENT_KEY_DOWN;
			event.key.windowID = window;
			event.key.key = SDL_GetKeyFromName(input.Key.c_str());
			event.key.scancode = SDL_GetScancodeFromName(input.Key.c_str());
			SDL_Keymod modifiers = SDL_KMOD_NONE;
			for (const std::string &name : input.Modifiers) {
				if (name == "shift") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_SHIFT);
				if (name == "control") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_CTRL);
				if (name == "alt") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_ALT);
				if (name == "gui") modifiers = static_cast<SDL_Keymod>(modifiers | SDL_KMOD_GUI);
			}
			if (event.key.scancode == SDL_SCANCODE_UNKNOWN)
				event.key.scancode = SDL_GetScancodeFromKey(event.key.key, &modifiers);
			if (event.key.key == SDLK_UNKNOWN)
				event.key.key = SDL_GetKeyFromScancode(event.key.scancode, modifiers, true);
			event.key.mod = event.type == SDL_EVENT_KEY_UP ? SDL_KMOD_NONE : modifiers;
			event.key.down = event.type == SDL_EVENT_KEY_DOWN;
			break;
		}
		case engine::control::InputAutomationKind::Text:
			event.type = SDL_EVENT_TEXT_INPUT;
			event.text.windowID = window;
			event.text.text = input.Text.c_str();
			break;
		}
		Interface.ProcessEvent(event);
	}
}
