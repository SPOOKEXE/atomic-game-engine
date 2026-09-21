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

	engine::control::HookLease ConfigureControlHooks(
		engine::control::Surface &surface,
		Origin &origin,
		Service &service,
		engine::control::Server &server,
		std::string &failure
	) {
		const std::array features{
			engine::control::features::Architecture(),
			engine::control::features::Diagnostics(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
			engine::control::features::Discovery(),
		};
		surface.Enable(features);

		return surface.ActivateHook(
			{.Id = "cdn.product",
			 .Revision = "v1",
			 .Purpose = "Reads state owned by this content-origin process.",
			 .Dependencies = {},
			 .Limits = {}},
			[&origin, &service, &server](engine::control::HookRegistration &registration) {
				registration.Add(
					engine::control::Tool{
						"engine_info",
						"This content origin's own state: where it is listening, the manifest root it "
						"serves, cache use, request counters, and its loopback control endpoint.",
						[] { return nlohmann::json{{"type", "object"}}; },
						[&origin, &service, &server](const nlohmann::json &, std::string &) {
							const ServiceCounters &counts = service.Counters();
							const std::shared_ptr<const Publication> publication = origin.Current();
							return nlohmann::json{
								{"endpoint", service.Local().Text()},
								{"manifest",
								 publication == nullptr ? std::string()
														: publication->Contents().Root().ToHex()},
								{"cache",
								 nlohmann::json{
									 {"bytes", origin.Cache().Bytes()},
									 {"entries", origin.Cache().Count()},
									 {"capacity", origin.Cache().Capacity()}
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
								 nlohmann::json{{"port", server.Port()}, {"served", server.Served()}}},
							};
						},
					}
				);
			},
			failure
		);
	}
}
