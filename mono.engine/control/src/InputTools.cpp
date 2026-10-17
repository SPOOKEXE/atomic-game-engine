// Input automation, shaped here and delivered by the host's existing input boundary.

#include <engine/control/Surface.hpp>

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace {
		constexpr size_t MAXIMUM_KEY_BYTES = 128;
		constexpr size_t MAXIMUM_TEXT_BYTES = 16 * 1024;

		bool Number(const json &arguments, const char *name, float &value, std::string &failure) {
			if (!arguments.contains(name) || !arguments[name].is_number()) {
				failure = std::string(name) + " must be a number";
				return false;
			}
			value = arguments[name].get<float>();
			if (!std::isfinite(value)) {
				failure = std::string(name) + " must be finite";
				return false;
			}
			return true;
		}

		bool Text(
			const json &arguments, const char *name, size_t maximum, std::string &value, std::string &failure
		) {
			if (!arguments.contains(name) || !arguments[name].is_string()) {
				failure = std::string(name) + " must be a string";
				return false;
			}
			value = arguments[name].get<std::string>();
			if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos) {
				failure = std::string(name) + " must be a bounded nonempty string without null bytes";
				return false;
			}
			return true;
		}

		bool State(const json &arguments, InputAutomationState &state, std::string &failure) {
			const auto found = arguments.find("down");
			if (found == arguments.end()) {
				state = InputAutomationState::Click;
				return true;
			}
			if (!found->is_boolean()) {
				failure = "down must be a boolean";
				return false;
			}
			state = found->get<bool>() ? InputAutomationState::Down : InputAutomationState::Up;
			return true;
		}

		bool Button(const json &arguments, std::string &button, std::string &failure) {
			const auto found = arguments.find("button");
			if (found == arguments.end()) {
				button = "left";
			} else if (!found->is_string()) {
				failure = "button must be a string";
				return false;
			} else {
				button = found->get<std::string>();
			}
			if (button != "left" && button != "middle" && button != "right") {
				failure = "button must be left, middle or right";
				return false;
			}
			return true;
		}

		bool Modifiers(const json &arguments, std::vector<std::string> &modifiers, std::string &failure) {
			const auto found = arguments.find("modifiers");
			if (found == arguments.end()) return true;
			if (!found->is_array()) {
				failure = "modifiers must be an array";
				return false;
			}
			for (const json &modifier : *found) {
				if (!modifier.is_string()) {
					failure = "each modifier must be a string";
					return false;
				}
				const std::string name = modifier.get<std::string>();
				if ((name != "shift" && name != "control" && name != "alt" && name != "gui") ||
					std::find(modifiers.begin(), modifiers.end(), name) != modifiers.end()) {
					failure = "modifiers must contain unique shift, control, alt, or gui names";
					return false;
				}
				modifiers.push_back(name);
			}
			return true;
		}

		json Schema(json properties, json required = json::array()) {
			return json{
				{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}
			};
		}
	}

	void Surface::AddInputTools(InputAutomationCallback callback) {
		if (!callback) return;
		Add(Tool{
			"emulate_mouse_move",
			"Moves the mouse pointer to client coordinates through this program's input boundary.",
			[] {
				return Schema(
					json{{"x", json{{"type", "number"}}}, {"y", json{{"type", "number"}}}},
					json::array({"x", "y"})
				);
			},
			[callback](const json &arguments, std::string &failure) {
				InputAutomationEvent event;
				event.Kind = InputAutomationKind::MouseMove;
				if (!Number(arguments, "x", event.X, failure) || !Number(arguments, "y", event.Y, failure))
					return json(nullptr);
				return callback(event, failure);
			},
		});
		Add(Tool{
			"emulate_click",
			"Clicks, presses, or releases a mouse button at client coordinates through this program's input "
			"boundary. Omit down for one complete click.",
			[] {
				return Schema(
					json{
						{"x", json{{"type", "number"}}},
						{"y", json{{"type", "number"}}},
						{"button",
						 json{{"type", "string"}, {"enum", json::array({"left", "middle", "right"})}}},
						{"down", json{{"type", "boolean"}}}
					},
					json::array({"x", "y"})
				);
			},
			[callback](const json &arguments, std::string &failure) {
				InputAutomationEvent event;
				event.Kind = InputAutomationKind::MouseButton;
				if (!Number(arguments, "x", event.X, failure) || !Number(arguments, "y", event.Y, failure) ||
					!Button(arguments, event.Button, failure) || !State(arguments, event.State, failure))
					return json(nullptr);
				return callback(event, failure);
			},
		});
		Add(Tool{
			"emulate_mouse_wheel",
			"Turns the mouse wheel by a signed number of vertical notches through this program's input "
			"boundary.",
			[] { return Schema(json{{"notches", json{{"type", "number"}}}}, json::array({"notches"})); },
			[callback](const json &arguments, std::string &failure) {
				InputAutomationEvent event;
				event.Kind = InputAutomationKind::MouseWheel;
				if (!Number(arguments, "notches", event.Wheel, failure)) return json(nullptr);
				return callback(event, failure);
			},
		});
		Add(Tool{
			"emulate_key",
			"Presses, releases, or taps one named keyboard key through this program's input boundary. Omit "
			"down for one complete key press.",
			[] {
				return Schema(
					json{
						{"key", json{{"type", "string"}}},
						{"down", json{{"type", "boolean"}}},
						{"modifiers",
						 json{
							 {"type", "array"},
							 {"items",
							  json{
								  {"type", "string"},
								  {"enum", json::array({"shift", "control", "alt", "gui"})}
							  }},
							 {"uniqueItems", true}
						 }}
					},
					json::array({"key"})
				);
			},
			[callback](const json &arguments, std::string &failure) {
				InputAutomationEvent event;
				event.Kind = InputAutomationKind::Key;
				if (!Text(arguments, "key", MAXIMUM_KEY_BYTES, event.Key, failure) ||
					!State(arguments, event.State, failure) ||
					!Modifiers(arguments, event.Modifiers, failure))
					return json(nullptr);
				return callback(event, failure);
			},
		});
		Add(Tool{
			"emulate_text",
			"Sends UTF-8 text through this program's input boundary.",
			[] {
				return Schema(
					json{{"text", json{{"type", "string"}, {"maxLength", MAXIMUM_TEXT_BYTES}}}},
					json::array({"text"})
				);
			},
			[callback](const json &arguments, std::string &failure) {
				InputAutomationEvent event;
				event.Kind = InputAutomationKind::Text;
				if (!Text(arguments, "text", MAXIMUM_TEXT_BYTES, event.Text, failure)) return json(nullptr);
				return callback(event, failure);
			},
		});
	}
}
