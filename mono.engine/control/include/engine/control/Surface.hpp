#pragma once

// Model Context Protocol, and the table of what a program can be asked to do.
//
// **One protocol, five programs.** The editor, the server, the client, the
// unified harness and the content origin all answer the same handshake and the
// same `tools/list`; what differs is which rows are in the table. A server has
// no selection and a content origin has no worlds, so neither declares tools it
// cannot honour - a client is told exactly what this program can do rather than
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

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

namespace engine::world {
	class Universe;
}

namespace engine::control {

	class Surface;

	// One named group of tools, resources, or prompts a program elects to
	// expose. The installer runs immediately and is not retained.
	//
	// @since v0.20
	struct Feature {
		std::string Name;
		std::function<void(Surface &)> Install;
	};

	// One thing a program can be asked to do.
	//
	// @since v0.8
	struct Tool {
		// What a client calls it. Lower snake case, because that is what every
		// other MCP server uses and a model has seen a great deal of.
		std::string Name;

		// What it does, written for whoever has never seen this engine. This is
		// the only documentation a client gets, so it carries the vocabulary -
		// that a world is a scene, that stopping restores a snapshot.
		std::string Description;

		// JSON Schema for the arguments. An empty object means it takes none.
		std::function<nlohmann::json()> Schema;

		// The work. Sets `failure` and returns null to refuse, which arrives at
		// the client as a tool error rather than a protocol error - the
		// distinction MCP draws so a model can read the reason and try again.
		std::function<nlohmann::json(const nlohmann::json &arguments, std::string &failure)> Call;
	};

	// Something a client may read without calling a tool.
	//
	// **The difference from a tool is who decides to fetch it.** A tool is an
	// action a model chooses; a resource is context a client may attach on its
	// own, and MCP clients do exactly that - they list resources at connect time
	// and offer them. So the layer table, the component catalogue and the
	// module graph belong here rather than only behind a call: they are the
	// things a model should already know before it asks its first question.
	//
	// Read lazily. A resource that is a hundred kilobytes of manifest costs
	// nothing until somebody reads it.
	//
	// @since v0.19
	struct Resource {
		// The address, in this engine's own scheme: `atomic://<what>/<which>`.
		// Stable, because a client remembers one it was told about.
		std::string Uri;

		// A short identifier a person recognises in a list.
		std::string Name;

		// One sentence for whoever has never seen this engine, exactly as a
		// `Tool::Description` is.
		std::string Description;

		// `text/markdown`, `application/json`, `text/plain`.
		std::string MimeType;

		// The contents. Sets `failure` and returns empty to refuse, which
		// arrives as a protocol error - a resource that cannot be read is not a
		// result a model can act on, unlike a tool that declined.
		std::function<std::string(std::string &failure)> Read;
	};

	// One thing a prompt can be told before it renders.
	//
	// **Declared rather than merely accepted**, because a client builds its
	// argument form from this list: an argument a prompt reads and does not
	// declare is one nobody can supply.
	//
	// @since v0.19
	struct PromptArgument {
		// What the caller passes it as.
		std::string Name;

		// One sentence, for a person filling in a form.
		std::string Description;

		// Whether rendering fails without it.
		bool Required = false;
	};

	// One workflow this repository actually has, offered as a prompt.
	//
	// **Prompts are the part of MCP that carries procedure**, and this
	// repository keeps its procedures in files - the completion checklist, the
	// module scaffold, the review pass. A prompt here is how one of those
	// reaches a client that has never opened the repository, and it renders the
	// checked-in text rather than a second copy of it.
	//
	// @since v0.19
	struct Prompt {
		// What a client calls it, and usually what it shows as a command.
		std::string Name;

		// One sentence saying when to reach for it.
		std::string Description;

		// What it may be told, for a client that builds a form from it.
		std::vector<PromptArgument> Arguments;

		// The message. Sets `failure` and returns empty to refuse.
		std::function<std::string(const nlohmann::json &arguments, std::string &failure)> Render;
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

		// Enables a program's explicit feature list, in order.
		//
		// A later feature may replace a row from an earlier one through `Add`,
		// which is how product-specific tools refine shared engine tools without
		// a second registry or a switch in the protocol.
		//
		// @param features Borrowed for this call. Installers are not retained.
		// @since v0.20
		void Enable(std::span<const Feature> features);

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

		// Installs the module graph and the layer table.
		//
		// **The only tools in this module a program with no worlds can still
		// answer honestly.** A content origin holds no scenes and has no
		// `world_list`; the layer stack is the same stack whatever the program
		// is, because it is compiled in from the file `just test-architecture`
		// checks the build against.
		//
		// `layer_table`, `module_get` and `module_may_link`. The last is the
		// question the architecture check answers and a model could not
		// previously ask: whether an edge would be legal, before writing it.
		//
		// @since v0.19
		void AddArchitectureTools();

		// Installs the class table and the type checker.
		//
		// `class_list` and `class_get` read the classes this process actually
		// registered; `script_check` type-checks Luau against the generated
		// declarations without running any of it. **Nothing here evaluates a
		// script**, and `features/Script.hpp` says why in full: a tool runs
		// inside the frame and there is no thread here to interrupt a loop from.
		//
		// `script_check` needs the checkout this program was built from and
		// refuses with a reason when there is not one.
		//
		// @since v0.19
		void AddScriptTools();

		// Installs the log tools, and a sink to feed them.
		//
		// **A ring of the most recent lines, on the process-wide logger.** The
		// editor could already be asked what it had said and no other program
		// could, so a dedicated server misbehaving unattended answered nothing.
		// `log_tail` reads the ring; `log_level` reads and changes the severity
		// floors, per category, while the program runs. `metrics_read` is here
		// too, and is the same question asked of `core::Metrics`: nothing
		// exported those out of the process, so a counter the headless server
		// had kept since v0.9 had never once been read.
		//
		// The sink is installed here rather than by the module, so a program
		// that never opens a control port pays nothing per line. Installed once
		// per process however many surfaces ask, and never removed - the logger
		// outlives every `Surface` and a sink detached mid-line would be a
		// use-after-free in the one component whose job is explaining a crash.
		//
		// @since v0.19
		void AddDiagnosticTools();

		// Installs the test runner.
		//
		// `test_run` starts the suites and returns a handle; `test_result`
		// polls it. **Asynchronous because it has to be** - a full run is
		// minutes, and a tool holds the frame it was called in.
		//
		// It invokes exactly `<build>/tools/testrunner`, with an argument list
		// this module assembles: no shell, no `just`, and no path a client can
		// influence. It runs the suites and does not build them.
		//
		// @since v0.19
		void AddBuildTools();

		// Adds one resource. Later rows win, as `Add` does.
		//
		// @since v0.19
		void AddResource(Resource resource);

		// Adds one prompt. Later rows win, as `Add` does.
		//
		// @since v0.19
		void AddPrompt(Prompt prompt);

		// Installs the resources any program can serve, plus the ones a
		// checkout adds.
		//
		// The layer table and module graph are compiled in, so every program has
		// them. The component catalogue belongs to the optional universe feature.
		// The `AGENTS.md` files and the scripting manifest are files, so
		// they appear only when this executable was staged into a checkout -
		// which is the same principle the tool table follows: a client is told
		// what this program can actually do rather than discovering it by
		// asking for something that fails.
		//
		// @since v0.19
		void AddStandardResources();

		// Installs the prompts for the workflows this repository has.
		//
		// **Rendered from `.claude/commands/*.md` rather than copied**, so a
		// fifth command file is a fifth prompt with no code change and no second
		// copy to keep in step. One prompt is written here and has no file: an
		// architecture-review pass, which drives the module-graph tools above.
		//
		// The file-backed ones appear only inside a checkout.
		//
		// @since v0.19
		void AddStandardPrompts();

		// One JSON-RPC message in, one out. Empty means "no reply", which is
		// what a notification gets.
		std::string Answer(const std::string &line);

		// How many tools are registered, for a log line.
		size_t Count() const;

		// Every registered tool, in the order `tools/list` reports them.
		//
		// **So a program can show its own table.** The editor draws one in an
		// information panel, and building that from a second hand-kept list
		// would be exactly the duplicate this registry exists to prevent - a
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
		// frame, and that assertion is the authority - so a tool switching the
		// graph on had it switched off again before the next frame. A program
		// with such a line ORs this into it; one without can ignore it.
		bool WantsProfiling() const {
			return Profiling;
		}

		// Every registered resource, in the order `resources/list` reports them.
		//
		// Valid until the next `AddResource`.
		//
		// @return The resources.
		// @since v0.19
		std::span<const Resource> Readable() const {
			return Resources;
		}

		// Every registered prompt, in the order `prompts/list` reports them.
		//
		// Valid until the next `AddPrompt`.
		//
		// @return The prompts.
		// @since v0.19
		std::span<const Prompt> Prompted() const {
			return Prompts;
		}

	  private:
		nlohmann::json ToolList() const;
		nlohmann::json ResourceList() const;
		nlohmann::json PromptList() const;

		std::string Name;
		std::string Purpose;
		std::vector<Tool> Tools;
		std::vector<Resource> Resources;
		std::vector<Prompt> Prompts;
		bool Profiling = false;
	};
}
