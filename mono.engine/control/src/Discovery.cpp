// The data-factory discovery operation. It reads only the control registry:
// proposed operations and capture channels are never inferred from engine
// internals, because a caller can use only what this host can actually call.

#include <engine/control/Surface.hpp>
#include <engine/core/Version.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace engine::control {

	using nlohmann::json;

	namespace {
		constexpr std::string_view CONTRACT_VERSION = "1";
		constexpr std::string_view SCHEMA_VERSION = "1";
		constexpr size_t MAXIMUM_REQUESTED_CHANNELS = 64;
		constexpr size_t MAXIMUM_CHANNEL_NAME_BYTES = 128;

		bool IsChannelIdentifier(std::string_view channel) {
			return !channel.empty() && std::all_of(channel.begin(), channel.end(), [](unsigned char byte) {
				return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
			});
		}

		json Unsupported(std::string_view name, std::string_view reason) {
			return json{{"name", name}, {"supported", false}, {"reason", reason}};
		}

		json DescribeOperations(const Surface &surface) {
			json operations = json::array();
			for (const Tool &tool : surface.Registered()) {
				operations.push_back(
					json{
						{"name", tool.Name},
						{"input_schema", tool.Schema ? tool.Schema() : json{{"type", "object"}}}
					}
				);
			}
			return operations;
		}

		json DescribeUnavailableOperations(const Surface &surface) {
			json operations = json::array();
			for (std::string_view name : std::array{
					 "reset_world",
					 "pause",
					 "resume",
					 "step",
					 "snapshot",
					 "capture",
					 "capture_render_screenshot",
					 "checkpoint",
					 "restore",
					 "apply_intervention",
					 "step_and_capture",
				 }) {
				const bool registered = std::any_of(
					surface.Registered().begin(), surface.Registered().end(), [name](const Tool &tool) {
						return tool.Name == name;
					}
				);
				if (registered) continue;
				operations.push_back(
					Unsupported(name, "this host does not implement the data-factory operation")
				);
			}
			return operations;
		}
	}

	void Surface::AddDiscoveryTools() {
		Add(Tool{
			"negotiate",
			"Pure data-factory capability discovery. Reports this host's callable MCP operations and "
			"their schemas, and names unsupported proposed capture capabilities instead of guessing "
			"from renderer internals. It does not create, load, reset, or mutate a world.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"contract_version", json{{"type", "string"}}},
						 {"schema_version", json{{"type", "string"}}},
						 {"requested_channels",
						  json{
							  {"type", "array"},
							  {"items",
							   json{
								   {"type", "string"},
								   {"pattern", "^[a-z0-9_]+$"},
								   {"maxLength", MAXIMUM_CHANNEL_NAME_BYTES}
							   }},
							  {"maxItems", MAXIMUM_REQUESTED_CHANNELS}
						  }},
					 }},
				};
			},
			[this](const json &arguments, std::string &failure) -> json {
				if (!arguments.is_object()) {
					failure = "negotiate arguments must be an object";
					return nullptr;
				}
				const auto requireVersion = [&](const char *field, std::string_view supported) {
					if (!arguments.contains(field)) {
						return true;
					}
					if (!arguments[field].is_string() || arguments[field].get<std::string>() != supported) {
						failure = "capability_unsupported: " + std::string(field) + " is not supported";
						return false;
					}
					return true;
				};
				if (!requireVersion("contract_version", CONTRACT_VERSION) ||
					!requireVersion("schema_version", SCHEMA_VERSION)) {
					return nullptr;
				}

				const DataCaptureAvailability capture = CaptureAvailability();
				const auto supports = [&capture](std::string_view channel) {
					return capture.Available &&
						   std::find(capture.Channels.begin(), capture.Channels.end(), channel) !=
							   capture.Channels.end();
				};
				const std::string unavailable = capture.Detail.empty()
													? "capture channels are not implemented by this host"
													: capture.Detail;

				json channels = json::array();
				if (arguments.contains("requested_channels")) {
					const json &requested = arguments["requested_channels"];
					if (!requested.is_array()) {
						failure = "requested_channels must be an array";
						return nullptr;
					}
					if (requested.size() > MAXIMUM_REQUESTED_CHANNELS) {
						failure = "requested_channels exceeds the 64-channel discovery limit";
						return nullptr;
					}
					for (const json &value : requested) {
						if (!value.is_string()) {
							failure =
								"requested_channels must contain lowercase ASCII identifiers up to 128 bytes";
							return nullptr;
						}
						const std::string &channel = value.get_ref<const std::string &>();
						if (channel.size() > MAXIMUM_CHANNEL_NAME_BYTES || !IsChannelIdentifier(channel)) {
							failure =
								"requested_channels must contain lowercase ASCII identifiers up to 128 bytes";
							return nullptr;
						}
						channels.push_back(
							supports(channel) ? json{{"name", channel}, {"supported", true}}
											  : Unsupported(channel, unavailable)
						);
					}
				}

				json limits = json::array();
				limits.push_back(
					json{
						{"name", "requested_channels"},
						{"supported", true},
						{"maximum", MAXIMUM_REQUESTED_CHANNELS}
					}
				);
				limits.push_back(
					capture.Available
						? json{{"name", "channels"}, {"supported", true}, {"maximum", 3}}
						: Unsupported(
							  "channels",
							  capture.Detail == "capture channels are not implemented by this host"
								  ? "this host does not implement the data-factory operation"
								  : unavailable
						  )
				);
				for (std::string_view name : std::array{
						 "script_bytes",
						 "entities",
						 "pixels",
						 "image_bytes",
						 "checkpoint_bytes",
						 "readbacks_in_flight",
						 "render_recursion",
						 "operation_time",
						 "resource_ttl",
					 }) {
					limits.push_back(
						Unsupported(name, "this host does not implement the data-factory operation")
					);
				}

				return json{
					{"contract_version", CONTRACT_VERSION},
					{"schema_version", SCHEMA_VERSION},
					{"engine_version", std::string(core::Version())},
					{"operations", DescribeOperations(*this)},
					{"unsupported_operations", DescribeUnavailableOperations(*this)},
					{"requested_channels", std::move(channels)},
					{"limits", std::move(limits)},
					{"headless_cpu", Unsupported("headless_cpu", "not declared by this control host")},
					{"offscreen_gpu",
					 capture.Available ? json{{"name", "offscreen_gpu"}, {"supported", true}}
									   : Unsupported("offscreen_gpu", unavailable)},
				};
			},
		});
	}
}
