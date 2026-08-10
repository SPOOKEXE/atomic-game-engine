#pragma once

// Model Context Protocol, and the table of what a program can be asked to do.
//
// **One protocol, five programs.** The editor, the server, the client, the
// unified harness and the content origin all answer the same handshake and the
// same `tools/list`; what differs is which rows are in the table. A server has
// no selection and a content origin has no worlds, so neither declares tools it
// cannot honour — a client is told exactly what this program can do rather than
// discovering it by calling something that fails.
//
// **A registry rather than a switch**, which is the whole reason this is a
// module and not a file in `mono.studio`. Adding a tool is one `Add` call beside
// the thing it exposes; nothing in the protocol changes, and no second list has
// to be kept in step with the first. `tools/list` and `tools/call` read the same
// table, so a tool that is callable is described and a tool that is described is
// callable.
//
// **Everything runs on whichever thread calls `Answer`.** That is the program's
// main thread, driven from `Server::Pump` in its frame or tick loop, because
// `Universe::Enter` aborts on a foreign thread rather than racing. A tool is
// therefore allowed to touch a world, and is expected to be quick about it.
//
// @tier shared

#include <engine/world/Universe.hpp>

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

namespace engine::control {

	// One thing a program can be asked to do.
	//
	// @since v0.8
	struct Tool {
		// What a client calls it. Lower snake case, because that is what every
		// other MCP server uses and a model has seen a great deal of.
		std::string Name;

		// What it does, written for whoever has never seen this engine. This is
		// the only documentation a client gets, so it carries the vocabulary —
		// that a world is a scene, that stopping restores a snapshot.
		std::string Description;

		// JSON Schema for the arguments. An empty object means it takes none.
		std::function<nlohmann::json()> Schema;

		// The work. Sets `failure` and returns null to refuse, which arrives at
		// the client as a tool error rather than a protocol error — the
		// distinction MCP draws so a model can read the reason and try again.
		std::function<nlohmann::json(const nlohmann::json &arguments, std::string &failure)> Call;
	};

	// The protocol, the table, and what a program says about itself.
	//
	// @since v0.8
	class Surface final {
	  public:
		// @param name    What this program calls itself to a client.
		// @param purpose One sentence a model reads before its first call.
		Surface(std::string name, std::string purpose);

		// Adds one tool. Later rows win, so a program may replace an inherited
		// one with a better-informed version of itself.
		void Add(Tool tool);

		// Installs the tools any program with worlds can answer.
		//
		// **The class tree and the storage under it, which are two views of one
		// world.** `engine_info`, `world_list`, `world_tree`, `instance_get` and
		// `instance_set` are the first; `component_list`, `entity_query`,
		// `component_get` and `component_set` are the second, added at v0.12
		// beside the script surface they mirror. `profile_frame` is neither.
		//
		// A client that could only see classes could not see anything a game
		// declared for itself, which is most of what a game is.
		//
		// **Takes the universe by reference and keeps it**, so the caller must
		// outlive the surface. Every program here owns its universe for its
		// whole life, which is why this is a reference and not a handle.
		//
		// @param universe The worlds to expose.
		// @param writable Whether `instance_set` is offered at all. A replica
		//                 refuses writes at the store, and a tool that always
		//                 fails is worse than one that was never listed.
		void AddUniverseTools(world::Universe &universe, bool writable = true);

		// One JSON-RPC message in, one out. Empty means "no reply", which is
		// what a notification gets.
		std::string Answer(const std::string &line);

		// How many tools are registered, for a log line.
		size_t Count() const;

		// Every registered tool, in the order `tools/list` reports them.
		//
		// **So a program can show its own table.** The editor draws one in an
		// information panel, and building that from a second hand-kept list
		// would be exactly the duplicate this registry exists to prevent — a
		// panel that says a tool exists when it does not is worse than no panel.
		//
		// Valid until the next `Add`.
		//
		// @return The tools.
		// @since v0.12
		std::span<const Tool> Registered() const {
			return Tools;
		}

		// Whether a client has asked for the frame graph.
		//
		// **Read by programs that assert the profiler's state every frame.** The
		// editor decides collection from whether its panel is open, once per
		// frame, and that assertion is the authority — so a tool switching the
		// graph on had it switched off again before the next frame. A program
		// with such a line ORs this into it; one without can ignore it.
		bool WantsProfiling() const {
			return Profiling;
		}

	  private:
		nlohmann::json ToolList() const;

		std::string Name;
		std::string Purpose;
		std::vector<Tool> Tools;
		bool Profiling = false;
	};
}
