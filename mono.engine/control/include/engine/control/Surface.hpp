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

#include <engine/control/HookRegistry.hpp>
#include <engine/script/GltfSceneExport.hpp>

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <vector>

namespace engine::world {
	class Universe;
	class DataFactorySession;
}

namespace engine::script {
	class DataCaptureBridge;
}

namespace engine::control {

	class Surface;
	class DataFactoryOperationLedger;

	// One named group of tools, resources, or prompts a program elects to
	// expose. The installer runs immediately and is not retained.
	//
	// @since v0.20
	struct Feature {
		// Stable feature identity and its immediate installer.
		//@{
		std::string Name;
		std::function<void(Surface &)> Install;
		//@}
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

	// The host reports capture readiness through this small value rather than
	// discovery reaching into a renderer from the MCP thread.
	struct DataCaptureAvailability {
		// Whether the host has a capture bridge ready to accept tickets.
		bool Available = false;
		// Render channel names the current host can capture.
		std::vector<std::string> Channels;
		// Host supplied reason when capture is unavailable or constrained.
		std::string Detail;
	};

	// The lifecycle rows a host can support. A headless host omits renderer
	// dependent rows so discovery lists only callable operations.
	struct DataFactoryToolSet {
		// True when the host may expose only tools that require a renderer.
		bool RenderOnly = true;
	};

	// Host callback that serializes the current frame graph or explains why it cannot.
	using RenderGraphProvider = std::function<nlohmann::json(const nlohmann::json &, std::string &)>;

	// One input action a host accepts from the control surface.
	//
	// The control module names no windowing API. A graphical host routes this to
	// its existing event pump, while a host without an input path leaves these
	// tools unregistered.
	enum class InputAutomationKind : uint8_t {
		MouseMove,
		MouseButton,
		MouseWheel,
		Key,
		Text,
	};

	// Whether an emulated button or key is a complete click, press, or release.
	enum class InputAutomationState : uint8_t {
		Click,
		Down,
		Up,
	};

	struct InputAutomationEvent {
		InputAutomationKind Kind = InputAutomationKind::MouseMove;
		InputAutomationState State = InputAutomationState::Click;
		float X = 0.0f;
		float Y = 0.0f;
		float Wheel = 0.0f;
		std::string Button;
		std::string Key;
		std::string Text;
		std::vector<std::string> Modifiers;
	};

	// Delivers a validated automation event to the host's input boundary.
	using InputAutomationCallback =
		std::function<nlohmann::json(const InputAutomationEvent &, std::string &)>;

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
		Surface(const Surface &) = delete;
		Surface &operator=(const Surface &) = delete;
		Surface(Surface &&) = delete;
		Surface &operator=(Surface &&) = delete;

		// Adds one row during built-in feature or active-hook installation. A legacy
		// direct call is recorded as a surface-lifetime built-in activation.
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

		// Activates an optional provider through owned transactional registration.
		// During installation, Add, AddResource, and AddPrompt stage rows in the
		// same transaction, so existing row builders can become a host hook without
		// a shadow registration path.
		HookLease
		ActivateHook(HookDescriptor descriptor, const HookInstaller &installer, std::string &failure);
		// The owner table for optional host providers.
		HookRegistry &Hooks() {
			return HookRegistry_;
		}
		// Advances optional hooks on the surface thread.
		void PumpHooks() {
			HookRegistry_.Pump();
		}
		// Returns the installed optional-hook registry.
		const HookRegistry &Hooks() const {
			return HookRegistry_;
		}

		// Supplies the host-owned capture readiness snapshot used by `negotiate`.
		// No provider means this surface has no capture host.
		void SetDataCaptureAvailabilityProvider(std::function<DataCaptureAvailability()> provider);
		// Returns the latest readiness snapshot from the host-owned capture provider.
		DataCaptureAvailability CaptureAvailability() const;
		// Sets the host callback used to answer render graph requests.
		void SetRenderGraphProvider(RenderGraphProvider provider);
		// Borrows the current render graph callback; it is empty until a host registers one.
		const RenderGraphProvider &RenderGraph() const;

		// The one replay and audit ledger shared by all installed data-factory
		// mutation tools. It is surface-local because MCP clients do not share
		// authority across host processes.
		std::shared_ptr<DataFactoryOperationLedger> DataFactoryOperations() const {
			return FactoryOperations;
		}

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
		// @param includeEngineInfo Whether to publish the generic engine_info row.
		//                 Products with a richer replacement own that row themselves.
		void AddUniverseTools(world::Universe &universe, bool writable = true, bool includeEngineInfo = true);

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
		void AddDiagnosticTools(bool includeLogTail = true);

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

		// Installs the five input automation tools when the host has an input
		// boundary that can consume them. The callback owns event delivery, so
		// control remains usable in programs built without SDL.
		void AddInputTools(InputAutomationCallback callback);

		// Installs pure capability and schema discovery for an external data
		// factory. The result reports this surface's registered tools, while
		// proposed operations and limits with no implementation stay explicitly
		// unsupported rather than becoming promises by name alone.
		//
		// @since v0.24
		void AddDiscoveryTools();

		// Installs lifecycle tools backed by one host-owned data-factory session.
		void AddDataFactoryTools(world::DataFactorySession &session, DataFactoryToolSet tools = {});
		// Installs ticket submission, polling, byte reads, and release tools for the supplied bridge.
		void AddDataCaptureTools(
			world::DataFactorySession &session, std::shared_ptr<script::DataCaptureBridge> bridge
		);

		// Installs read-only data-scene observations for worlds owned by `universe`.
		void AddDataSceneTools(
			world::Universe &universe,
			std::shared_ptr<script::DataCaptureBridge> bridge = {},
			world::DataFactorySession *session = nullptr,
			script::GltfMeshSource meshSource = {},
			script::GltfTextureSource textureSource = {}
		);
		// Copies one camera and selected stable object poses after the lifecycle
		// session has proven a retained all-systems-paused snapshot is still live.
		void AddTemporalSampleTools(world::Universe &universe, world::DataFactorySession &session);
		// Installs rig export tools, optionally fencing reads to a data-factory session revision.
		void AddRigExportTools(world::Universe &universe, world::DataFactorySession *session = nullptr);

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
			const_cast<HookRegistry &>(HookRegistry_).Reap();
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
			const_cast<HookRegistry &>(HookRegistry_).Reap();
			return Resources;
		}

		// Every registered prompt, in the order `prompts/list` reports them.
		//
		// Valid until the next `AddPrompt`.
		//
		// @return The prompts.
		// @since v0.19
		std::span<const Prompt> Prompted() const {
			const_cast<HookRegistry &>(HookRegistry_).Reap();
			return Prompts;
		}

	  private:
		friend class HookRegistry;
		void InstallHookTool(Tool tool, std::string_view id, uint64_t generation, bool replaceBuiltin);
		void
		InstallHookResource(Resource resource, std::string_view id, uint64_t generation, bool replaceBuiltin);
		void InstallHookPrompt(Prompt prompt, std::string_view id, uint64_t generation, bool replaceBuiltin);
		void RemoveHookTool(std::string_view name, std::string_view id, uint64_t generation);
		void RemoveHookResource(std::string_view uri);
		void RemoveHookPrompt(std::string_view name);
		nlohmann::json ToolList() const;
		nlohmann::json ResourceList() const;
		nlohmann::json PromptList() const;

		std::string Name;
		std::string Purpose;
		HookRegistry HookRegistry_;
		HookRegistration *CurrentRegistration = nullptr;
		std::vector<Tool> Tools;
		uint64_t NextBuiltinRegistration = 0;
		std::function<DataCaptureAvailability()> CaptureAvailabilityProvider;
		RenderGraphProvider RenderGraphProviderCallback;
		std::shared_ptr<DataFactoryOperationLedger> FactoryOperations;
		std::vector<Resource> Resources;
		std::vector<Prompt> Prompts;
		bool Profiling = false;
		std::vector<HookLease> BuiltinHooks;
	};
}
