// The handshake, and the table read twice.
//
// **Four methods.** MCP is JSON-RPC 2.0 with a fixed opening — `initialize`,
// then `notifications/initialized`, then `tools/list` and `tools/call` for as
// long as the client cares to. Everything program-specific is a row in the
// table; nothing here changes when one is added.
//
// **An unknown tool is an error *result*, not a JSON-RPC error.** MCP draws that
// line deliberately: transport errors are for malformed calls, while a tool that
// refused is something the model is supposed to read and react to. So
// `tools/call` almost always succeeds at the protocol level with `isError` set,
// and the text says what went wrong.

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
		: Name(std::move(name)), Purpose(std::move(purpose)) {}

	void Surface::Add(Tool tool) {
		for (Tool &existing : Tools) {
			if (existing.Name == tool.Name) {
				existing = std::move(tool);
				return;
			}
		}
		Tools.push_back(std::move(tool));
	}

	size_t Surface::Count() const {
		return Tools.size();
	}

	json Surface::ToolList() const {
		json out = json::array();
		for (const Tool &tool : Tools) {
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

	std::string Surface::Answer(const std::string &line) {
		json request;
		try {
			request = json::parse(line);
		} catch (const std::exception &bad) {
			// No id to answer against — the parse is what failed — so this uses
			// the null id JSON-RPC reserves for exactly this case.
			return Error(nullptr, -32700, std::string("parse error: ") + bad.what()).dump();
		}

		if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
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
			return Result(
					   id,
					   json{
						   {"protocolVersion", PROTOCOL_VERSION},
						   {"capabilities", json{{"tools", json{{"listChanged", false}}}}},
						   {"serverInfo",
							json{{"name", Name}, {"title", "atomic — " + Name}, {"version", "0.8"}}},
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
				// A bad index into a world, a name that is not a class — these
				// arrive as exceptions from the json library or from a store, and
				// letting one escape would take the frame loop with it.
				try {
					payload = tool.Call(arguments, failure);
				} catch (const std::exception &thrown) {
					failure = thrown.what();
				}

				if (!failure.empty()) {
					return Result(id, Content(json{{"error", failure}}, true)).dump();
				}
				return Result(id, Content(std::move(payload))).dump();
			}

			return Result(id, Content(json{{"error", "no such tool: " + wanted}}, true)).dump();
		}

		return Error(id, -32601, "no such method: " + method).dump();
	}
}
