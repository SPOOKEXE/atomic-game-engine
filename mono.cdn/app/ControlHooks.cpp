// The content origin's MCP composition root.

#include "ControlHooks.hpp"

#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>

#include <cdn/Origin.hpp>
#include <cdn/Service.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace cdn {

	std::vector<engine::control::HookLease>
	ConfigureControlHooks(ControlHookContext context, std::string &failure) {
		struct Entry {
			engine::control::HookDescriptor Descriptor;
			engine::control::HookInstaller Install;
		};
		const auto builtin = [](std::string name) {
			return engine::control::HookDescriptor{
				.Id = "builtin." + std::move(name),
				.Revision = "v1",
				.Purpose = "Built-in feature registration.",
				.Dependencies = {},
				.Limits = {}
			};
		};
		std::vector<Entry> manifest;
		manifest.push_back({builtin("architecture"), [context](engine::control::HookRegistration &) {
								context.Surface.AddArchitectureTools();
							}});
		manifest.push_back({builtin("diagnostics"), [context](engine::control::HookRegistration &) {
								context.Surface.AddDiagnosticTools(true);
							}});
		manifest.push_back({builtin("resources"), [context](engine::control::HookRegistration &) {
								context.Surface.AddStandardResources();
							}});
		manifest.push_back({builtin("prompts"), [context](engine::control::HookRegistration &) {
								context.Surface.AddStandardPrompts();
							}});
		manifest.push_back({builtin("discovery"), [context](engine::control::HookRegistration &) {
								context.Surface.AddDiscoveryTools();
							}});
		manifest.push_back(
			{{.Id = "cdn.product",
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
			 }}
		);
		std::vector<engine::control::HookLease> leases;
		leases.reserve(manifest.size());
		for (Entry &entry : manifest) {
			auto lease = context.Surface.ActivateHook(std::move(entry.Descriptor), entry.Install, failure);
			if (!lease.IsValid()) return {};
			leases.push_back(std::move(lease));
		}
		return leases;
	}
}
