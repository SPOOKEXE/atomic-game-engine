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
		const std::array manifest{
			engine::control::features::Architecture(),
			engine::control::features::Diagnostics(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
			engine::control::features::Discovery(),
		};
		ControlSurface.Enable(manifest);
		struct LauncherControlHookContext {
			Launcher &Host;
			engine::control::Surface &Surface;
		};
		const LauncherControlHookContext context{.Host = *this, .Surface = ControlSurface};
		std::string failure;
		LauncherControlHook = context.Surface.ActivateHook(
			{.Id = "launcher.product",
			 .Revision = "v1",
			 .Purpose = "Launcher input automation and staged-program controls.",
			 .Dependencies = {},
			 .Limits = {{"queued_events", 64}}},
			[this, context](engine::control::HookRegistration &) {
				Launcher &host = context.Host;
				context.Surface.AddInputTools(
					[&host](const engine::control::InputAutomationEvent &event, std::string &failure) {
						if (event.Kind == engine::control::InputAutomationKind::Key &&
							SDL_GetKeyFromName(event.Key.c_str()) == SDLK_UNKNOWN &&
							SDL_GetScancodeFromName(event.Key.c_str()) == SDL_SCANCODE_UNKNOWN) {
							failure = "key is not a recognized SDL key name";
							return json(nullptr);
						}
						int width = host.Settings.Width;
						int height = host.Settings.Height;
						if (host.Window != nullptr) (void)SDL_GetWindowSize(host.Window, &width, &height);
						if ((event.Kind == engine::control::InputAutomationKind::MouseMove ||
							 event.Kind == engine::control::InputAutomationKind::MouseButton) &&
							(event.X < 0.0f || event.Y < 0.0f || event.X >= width || event.Y >= height)) {
							failure = "pointer coordinates are outside the launcher area";
							return json(nullptr);
						}
						host.DispatchControlInput(event);
						if (event.State == engine::control::InputAutomationState::Click &&
							(event.Kind == engine::control::InputAutomationKind::MouseButton ||
							 event.Kind == engine::control::InputAutomationKind::Key)) {
							auto release = event;
							release.State = engine::control::InputAutomationState::Up;
							host.DeferredControlRelease.push_back(std::move(release));
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
						"Set one declared option on the open mode. Values may contain several entries for "
						"repeatable "
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
							if (Open_.empty() || !arguments.contains("name") ||
								!arguments["name"].is_string()) {
								failure = "open a mode and supply an option name first";
								return json();
							}
							const Mode *mode = FindMode(Catalogue, Open_);
							const Description *description =
								mode == nullptr ? nullptr : Programs.Find(mode->Program);
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
						"Close the launcher and stop its supervised child. Also ends headless MCP-only "
						"sessions.",
						[] { return json{{"type", "object"}}; },
						[this](const json &, std::string &) {
							Quit = true;
							return json{{"quitting", true}};
						}
					}
				);
			},
			failure
		);
		if (!LauncherControlHook.IsValid()) {
			throw std::runtime_error("could not activate launcher control hook: " + failure);
		}
	}

} // namespace launcher
