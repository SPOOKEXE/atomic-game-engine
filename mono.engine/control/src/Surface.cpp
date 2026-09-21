// The handshake, and three tables each read twice.
//
// **MCP is JSON-RPC 2.0 with a fixed opening** - `initialize`, then
// `notifications/initialized`, then whatever the client cares to ask. What it
// may ask for is `tools/list` and `tools/call`, `resources/list` and
// `resources/read`, `prompts/list` and `prompts/get`. Everything
// program-specific is a row in one of the three tables; nothing here changes
// when one is added.
//
// **Three kinds because a client uses them at three moments.** A tool is an
// action a model chooses. A resource is context a client may attach on its own,
// which is why the layer table and the component catalogue are one - a model
// should have them before its first question rather than after its third call.
// A prompt is a procedure this repository already has written down, offered
// where somebody can invoke it.
//
// **An unknown tool is an error *result*, not a JSON-RPC error.** MCP draws that
// line deliberately: transport errors are for malformed calls, while a tool that
// refused is something the model is supposed to read and react to. So
// `tools/call` almost always succeeds at the protocol level with `isError` set,
// and the text says what went wrong.

#include <engine/control/DataFactoryOperationLedger.hpp>
#include <engine/control/Surface.hpp>

#include <nlohmann/json.hpp>
#include <utility>

namespace engine::control {

	using nlohmann::json;

	namespace {
		// A date rather than a number, which is the specification's own scheme.
		constexpr const char *PROTOCOL_VERSION = "2025-06-18";

		json Error(const json &id, int code, const std::string &message) {
			return json{
				{"jsonrpc", "2.0"}, {"id", id}, {"error", json{{"code", code}, {"message", message}}}
			};
		}

		json Result(const json &id, json payload) {
			return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(payload)}};
		}

		// **The payload goes out as text rather than as structured content**,
		// and that is worth stating because the JSON is right there. Structured
		// output is a newer part of the specification and clients disagree about
		// it; a pretty-printed object inside a text block is understood by all of
		// them, and is what a person reading a transcript can follow.
		json Content(json payload, bool failed = false) {
			return json{
				{"content", json::array({json{{"type", "text"}, {"text", payload.dump(2)}}})},
				{"isError", failed},
			};
		}
	}

	Surface::Surface(std::string name, std::string purpose)
		: Name(std::move(name)), Purpose(std::move(purpose)), HookRegistry_(*this),
		  FactoryOperations(std::make_shared<DataFactoryOperationLedger>()) {}

	void Surface::Add(Tool tool) {
		if (CurrentRegistration != nullptr) {
			CurrentRegistration->Add(std::move(tool));
			return;
		}
		for (Tool &existing : Tools) {
			if (existing.Name == tool.Name) {
				if (HookRegistry_.OwnsTool(tool.Name)) {
					throw std::runtime_error("an active hook owns tool: " + tool.Name);
				}
				existing = std::move(tool);
				return;
			}
		}
		Tools.push_back(std::move(tool));
	}

	void Surface::Enable(std::span<const Feature> features) {
		for (const Feature &feature : features) {
			if (!feature.Install) continue;
			std::string failure;
			HookLease lease = HookRegistry_.ActivateBuiltin(
				{.Id = "builtin." + feature.Name,
				 .Revision = "v1",
				 .Purpose = "Built-in feature registration.",
				 .Dependencies = {},
				 .Limits = {}},
				[this, &feature](HookRegistration &registration) {
					struct ResetRegistration {
						Surface &Owner;
						HookRegistration *Previous;
						~ResetRegistration() {
							Owner.CurrentRegistration = Previous;
						}
					} reset{*this, CurrentRegistration};
					CurrentRegistration = &registration;
					feature.Install(*this);
				},
				failure
			);
			if (!failure.empty()) throw std::runtime_error(failure);
			BuiltinHooks.push_back(std::move(lease));
		}
	}

	HookLease
	Surface::ActivateHook(HookDescriptor descriptor, const HookInstaller &installer, std::string &failure) {
		return HookRegistry_.Activate(
			std::move(descriptor),
			[this, &installer](HookRegistration &registration) {
				struct ResetRegistration {
					Surface &Owner;
					HookRegistration *Previous;
					~ResetRegistration() {
						Owner.CurrentRegistration = Previous;
					}
				} reset{*this, CurrentRegistration};
				CurrentRegistration = &registration;
				if (installer) installer(registration);
			},
			failure
		);
	}

	void Surface::SetDataCaptureAvailabilityProvider(std::function<DataCaptureAvailability()> provider) {
		CaptureAvailabilityProvider = std::move(provider);
	}

	DataCaptureAvailability Surface::CaptureAvailability() const {
		if (!CaptureAvailabilityProvider) {
			return {
				.Available = false,
				.Channels = {},
				.Detail = "capture channels are not implemented by this host"
			};
		}
		return CaptureAvailabilityProvider();
	}

	void Surface::SetRenderGraphProvider(RenderGraphProvider provider) {
		RenderGraphProviderCallback = std::move(provider);
	}

	const RenderGraphProvider &Surface::RenderGraph() const {
		return RenderGraphProviderCallback;
	}

	void Surface::AddResource(Resource resource) {
		if (CurrentRegistration != nullptr) {
			CurrentRegistration->Add(std::move(resource));
			return;
		}
		for (Resource &existing : Resources) {
			if (existing.Uri == resource.Uri) {
				if (HookRegistry_.OwnsResource(resource.Uri)) {
					throw std::runtime_error("an active hook owns resource: " + resource.Uri);
				}
				existing = std::move(resource);
				return;
			}
		}
		Resources.push_back(std::move(resource));
	}

	void Surface::AddPrompt(Prompt prompt) {
		if (CurrentRegistration != nullptr) {
			CurrentRegistration->Add(std::move(prompt));
			return;
		}
		for (Prompt &existing : Prompts) {
			if (existing.Name == prompt.Name) {
				if (HookRegistry_.OwnsPrompt(prompt.Name)) {
					throw std::runtime_error("an active hook owns prompt: " + prompt.Name);
				}
				existing = std::move(prompt);
				return;
			}
		}
		Prompts.push_back(std::move(prompt));
	}

	size_t Surface::Count() const {
		const_cast<HookRegistry &>(HookRegistry_).Reap();
		return Tools.size();
	}

	void Surface::InstallHookTool(Tool tool, std::string_view ownerId, uint64_t ownerGeneration, bool) {
		const std::string name = tool.Name;
		const std::string id(ownerId);
		const auto call = std::move(tool.Call);
		tool.Call =
			[this, name, id, ownerGeneration, call](const nlohmann::json &arguments, std::string &failure) {
				if (!HookRegistry_.Holds(name, id, ownerGeneration)) {
					failure = "hook is draining: " + name;
					return nlohmann::json(nullptr);
				}
				struct Guard {
					HookRegistry &Registry;
					std::string Id;
					uint64_t Generation;
					~Guard() {
						Registry.Release(Id, Generation);
					}
				} guard{HookRegistry_, id, ownerGeneration};
				return call(arguments, failure);
			};
		for (Tool &existing : Tools) {
			if (existing.Name == tool.Name) {
				existing = std::move(tool);
				return;
			}
		}
		Tools.push_back(std::move(tool));
	}

	void Surface::InstallHookResource(
		Resource resource, std::string_view ownerId, uint64_t ownerGeneration, bool
	) {
		const std::string uri = resource.Uri;
		const auto read = std::move(resource.Read);
		const std::string id(ownerId);
		resource.Read = [this, uri, id, ownerGeneration, read](std::string &failure) {
			if (!HookRegistry_.HoldsResource(uri, id, ownerGeneration)) {
				failure = "hook is draining: " + uri;
				return std::string{};
			}
			struct Guard {
				HookRegistry &Registry;
				std::string Id;
				uint64_t Generation;
				~Guard() {
					Registry.Release(Id, Generation);
				}
			} guard{HookRegistry_, id, ownerGeneration};
			return read(failure);
		};
		for (Resource &existing : Resources) {
			if (existing.Uri == resource.Uri) {
				existing = std::move(resource);
				return;
			}
		}
		Resources.push_back(std::move(resource));
	}

	void Surface::InstallHookPrompt(Prompt prompt, std::string_view ownerId, uint64_t ownerGeneration, bool) {
		const std::string name = prompt.Name;
		const auto render = std::move(prompt.Render);
		const std::string id(ownerId);
		prompt.Render =
			[this, name, id, ownerGeneration, render](const nlohmann::json &arguments, std::string &failure) {
				if (!HookRegistry_.HoldsPrompt(name, id, ownerGeneration)) {
					failure = "hook is draining: " + name;
					return std::string{};
				}
				struct Guard {
					HookRegistry &Registry;
					std::string Id;
					uint64_t Generation;
					~Guard() {
						Registry.Release(Id, Generation);
					}
				} guard{HookRegistry_, id, ownerGeneration};
				return render(arguments, failure);
			};
		for (Prompt &existing : Prompts) {
			if (existing.Name == prompt.Name) {
				existing = std::move(prompt);
				return;
			}
		}
		Prompts.push_back(std::move(prompt));
	}

	void Surface::RemoveHookTool(std::string_view name, std::string_view, uint64_t) {
		std::erase_if(Tools, [name](const Tool &tool) { return tool.Name == name; });
	}

	void Surface::RemoveHookResource(std::string_view uri) {
		std::erase_if(Resources, [uri](const Resource &resource) { return resource.Uri == uri; });
	}

	void Surface::RemoveHookPrompt(std::string_view name) {
		std::erase_if(Prompts, [name](const Prompt &prompt) { return prompt.Name == name; });
	}

	json Surface::ToolList() const {
		json out = json::array();
		for (const Tool &tool : Tools) {
			if (!HookRegistry_.VisibleTool(tool.Name)) continue;
			out.push_back(
				json{
					{"name", tool.Name},
					{"description", tool.Description},
					{"inputSchema", tool.Schema ? tool.Schema() : json{{"type", "object"}}},
				}
			);
		}
		return out;
	}

	json Surface::ResourceList() const {
		json out = json::array();
		for (const Resource &resource : Resources) {
			if (!HookRegistry_.VisibleResource(resource.Uri)) continue;
			out.push_back(
				json{
					{"uri", resource.Uri},
					{"name", resource.Name},
					{"description", resource.Description},
					{"mimeType", resource.MimeType},
				}
			);
		}
		return out;
	}

	json Surface::PromptList() const {
		json out = json::array();
		for (const Prompt &prompt : Prompts) {
			if (!HookRegistry_.VisiblePrompt(prompt.Name)) continue;
			json arguments = json::array();
			for (const PromptArgument &argument : prompt.Arguments) {
				arguments.push_back(
					json{
						{"name", argument.Name},
						{"description", argument.Description},
						{"required", argument.Required},
					}
				);
			}

			out.push_back(
				json{
					{"name", prompt.Name},
					{"description", prompt.Description},
					{"arguments", std::move(arguments)},
				}
			);
		}
		return out;
	}

	std::string Surface::Answer(const std::string &line) {
		HookRegistry_.Reap();
		json request;
		try {
			request = json::parse(line);
		} catch (const std::exception &bad) {
			// No id to answer against - the parse is what failed - so this uses
			// the null id JSON-RPC reserves for exactly this case.
			return Error(nullptr, -32700, std::string("parse error: ") + bad.what()).dump();
		}

		// **The object test comes first and the id is read only afterwards.**
		// `value` on an array throws, so reading the id out of whatever arrived
		// let a well-formed JSON array escape this function as an exception -
		// through `Server::Pump` and into the frame loop, over a message a
		// client could send by accident.
		if (!request.is_object()) {
			return Error(nullptr, -32600, "not a JSON-RPC request").dump();
		}
		if (!request.contains("method") || !request["method"].is_string()) {
			return Error(request.value("id", json(nullptr)), -32600, "not a JSON-RPC request").dump();
		}

		const std::string method = request["method"].get<std::string>();
		const json id = request.value("id", json(nullptr));

		// **A notification is answered with nothing at all.** JSON-RPC says a
		// request without an id gets no response, and MCP's handshake sends one
		// every time; a server that replied would put a message on the wire the
		// client is not reading and desynchronise the stream.
		const bool notification = !request.contains("id") || request["id"].is_null();

		if (method == "initialize") {
			// **A capability appears only when there is something behind it**,
			// which is the tool table's own rule applied to the handshake. A
			// server that advertised `resources` and then listed none would have
			// a client offering an attachment menu that is always empty; a
			// dedicated server outside a checkout has no `AGENTS.md` to serve
			// and says so here rather than at the first read.
			json capabilities{{"tools", json{{"listChanged", false}}}};
			if (!Resources.empty()) {
				capabilities["resources"] = json{{"listChanged", false}, {"subscribe", false}};
			}
			if (!Prompts.empty()) {
				capabilities["prompts"] = json{{"listChanged", false}};
			}

			return Result(
					   id,
					   json{
						   {"protocolVersion", PROTOCOL_VERSION},
						   {"capabilities", std::move(capabilities)},
						   {"serverInfo",
							json{{"name", Name}, {"title", "atomic - " + Name}, {"version", "0.19"}}},
						   {"instructions", Purpose},
					   }
			)
				.dump();
		}

		if (notification) {
			return {};
		}

		if (method == "ping") {
			return Result(id, json::object()).dump();
		}

		if (method == "tools/list") {
			return Result(id, json{{"tools", ToolList()}}).dump();
		}

		if (method == "tools/call") {
			const json params = request.value("params", json::object());
			if (!params.contains("name") || !params["name"].is_string()) {
				return Error(id, -32602, "tools/call needs a tool name").dump();
			}

			const std::string wanted = params["name"].get<std::string>();
			const json arguments = params.value("arguments", json::object());

			for (const Tool &tool : Tools) {
				if (tool.Name != wanted) {
					continue;
				}

				std::string failure;
				json payload;

				// **A tool that throws is a failed tool, not a failed program.**
				// A bad index into a world, a name that is not a class - these
				// arrive as exceptions from the json library or from a store, and
				// letting one escape would take the frame loop with it.
				try {
					payload = tool.Call(arguments, failure);
				} catch (const std::exception &thrown) {
					failure = thrown.what();
				}

				if (!failure.empty()) {
					// Refusals may carry recovery data, such as the current world
					// versions for a stale write. Keep that object while marking the
					// transport result as an error so a client can act on it.
					if (!payload.is_object()) {
						payload = json{{"error", failure}};
					} else if (!payload.contains("error")) {
						payload["error"] = failure;
					}
					const std::string reply = Result(id, Content(std::move(payload), true)).dump();
					HookRegistry_.Reap();
					return reply;
				}
				const std::string reply = Result(id, Content(std::move(payload))).dump();
				HookRegistry_.Reap();
				return reply;
			}

			return Result(id, Content(json{{"error", "no such tool: " + wanted}}, true)).dump();
		}

		if (method == "resources/list") {
			return Result(id, json{{"resources", ResourceList()}}).dump();
		}

		// Declared and empty, because a client that asks is entitled to an
		// answer rather than "no such method". Nothing here serves a templated
		// resource: every address this module offers is a fixed one.
		if (method == "resources/templates/list") {
			return Result(id, json{{"resourceTemplates", json::array()}}).dump();
		}

		if (method == "resources/read") {
			const json params = request.value("params", json::object());
			if (!params.contains("uri") || !params["uri"].is_string()) {
				return Error(id, -32602, "resources/read needs a uri").dump();
			}

			const std::string wanted = params["uri"].get<std::string>();
			for (const Resource &resource : Resources) {
				if (resource.Uri != wanted) {
					continue;
				}

				// **A resource that cannot be read is a protocol error, and a
				// tool that refused is not.** The difference is what the caller
				// can do about it: a declined tool call is an answer a model
				// reads and reacts to, while a resource is context a client
				// attached on its own and there is nothing to react to. MCP
				// spells the second `-32002`.
				std::string failure;
				std::string contents;
				try {
					contents = resource.Read(failure);
				} catch (const std::exception &thrown) {
					failure = thrown.what();
				}

				if (!failure.empty()) {
					const std::string reply = Error(id, -32002, failure).dump();
					HookRegistry_.Reap();
					return reply;
				}

				const std::string reply = Result(
											  id,
											  json{
												  {"contents",
												   json::array({json{
													   {"uri", resource.Uri},
													   {"name", resource.Name},
													   {"mimeType", resource.MimeType},
													   {"text", std::move(contents)},
												   }})},
											  }
				)
											  .dump();
				HookRegistry_.Reap();
				return reply;
			}

			return Error(id, -32002, "no such resource: " + wanted).dump();
		}

		if (method == "prompts/list") {
			return Result(id, json{{"prompts", PromptList()}}).dump();
		}

		if (method == "prompts/get") {
			const json params = request.value("params", json::object());
			if (!params.contains("name") || !params["name"].is_string()) {
				return Error(id, -32602, "prompts/get needs a prompt name").dump();
			}

			const std::string wanted = params["name"].get<std::string>();
			const json arguments = params.value("arguments", json::object());

			for (const Prompt &prompt : Prompts) {
				if (prompt.Name != wanted) {
					continue;
				}

				std::string failure;
				std::string text;
				try {
					text = prompt.Render(arguments, failure);
				} catch (const std::exception &thrown) {
					failure = thrown.what();
				}

				if (!failure.empty()) {
					const std::string reply = Error(id, -32602, failure).dump();
					HookRegistry_.Reap();
					return reply;
				}

				const std::string reply =
					Result(
						id,
						json{
							{"description", prompt.Description},
							{"messages",
							 json::array({json{
								 {"role", "user"},
								 {"content", json{{"type", "text"}, {"text", std::move(text)}}},
							 }})},
						}
					)
						.dump();
				HookRegistry_.Reap();
				return reply;
			}

			return Error(id, -32602, "no such prompt: " + wanted).dump();
		}

		return Error(id, -32601, "no such method: " + method).dump();
	}
}
