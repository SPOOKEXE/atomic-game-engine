// The content origin's MCP composition root.

#include "ControlHooks.hpp"

#include <engine/control/Features.hpp>
#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>

#include <array>
#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace cdn {

	engine::control::HookLease ConfigureControlHooks(ControlHookContext context, std::string &failure) {
		const std::array features{
			engine::control::features::Architecture(),
			engine::control::features::Diagnostics(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
			engine::control::features::Discovery(),
		};
		context.Surface.Enable(features);

		return context.Surface.ActivateHook(
			{.Id = "cdn.product",
			 .Revision = "v1",
			 .Purpose = "Reads state owned by this content-origin process.",
			 .Dependencies = {},
			 .Limits = {}},
			[context](engine::control::HookRegistration &registration) {
				registration.Add(
					engine::control::Tool{
						"engine_info",
						"This content origin's own state: where it is listening, the manifest root it "
						"serves, cache use, request counters, and its loopback control endpoint.",
						[] { return nlohmann::json{{"type", "object"}}; },
						[context](const nlohmann::json &, std::string &) {
							const ServiceCounters &counts = context.ContentService.Counters();
							const std::shared_ptr<const Publication> publication =
								context.ContentOrigin.Current();
							return nlohmann::json{
								{"endpoint", context.ContentService.Local().Text()},
								{"manifest",
								 publication == nullptr ? std::string()
														: publication->Contents().Root().ToHex()},
								{"cache",
								 nlohmann::json{
									 {"bytes", context.ContentOrigin.Cache().Bytes()},
									 {"entries", context.ContentOrigin.Cache().Count()},
									 {"capacity", context.ContentOrigin.Cache().Capacity()}
								 }},
								{"requests",
								 nlohmann::json{
									 {"bundles", counts.Bundles},
									 {"refused", counts.Refused},
									 {"missing", counts.Missing},
									 {"sentBytes", counts.SentBytes},
									 {"receivedBytes", counts.ReceivedBytes}
								 }},
								{"control",
								 nlohmann::json{
									 {"port", context.ControlServer.Port()},
									 {"served", context.ControlServer.Served()}
								 }},
							};
						},
					}
				);
			},
			failure
		);
	}
}
