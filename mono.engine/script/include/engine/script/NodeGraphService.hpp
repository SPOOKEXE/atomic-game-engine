#pragma once

// The `NodeGraph` script library, said without naming a VM.
//
// `NodeGraphService` is a neutral service surface both adapters install from
// the catalogue. Its methods work on integer handles to registries, graphs,
// runners and views held per VM in a `NodeGraphLibrary`. Each adapter also
// runs a short embedded prelude (below) that wraps the handles into the
// `NodeGraph.Registry`, `.Graph`, `.Runner` and `.View` objects scripts use.
//
// Rules the service enforces:
//
// - Script `Evaluate` functions run on the script thread inside `Run`, so
//   script nodes are Sync in every domain. A Pure script node runs read-only,
//   the same guard `World:ForEachParallel` uses.
// - A script registry is its own; a script cannot change the node types the
//   engine registered for its own graphs. An opened engine graph reads them, and
//   only an image graph's document can be edited, through its instance's own
//   `SetGraphDocument` checks.
// - A host value with no script form (an image, a run) reaches a script as an
//   opaque `{ NodeGraphBlob, Type, Bytes }` table that stands for it on a port of
//   this VM. It does not keep the value alive.
// - Hooks are queued during a run and delivered on the script thread after
//   it, in the order they fired. A hook an async node fires between runs is
//   delivered by the next run.
// - Node count, link count, value bytes and run time are bounded per VM and a
//   breach raises a script error.
//
// @tier L9 · shared

#include <engine/ang/Runner.hpp>
#include <engine/angview/View.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/Signals.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace engine::script {

	class ScriptCall;
	struct ServiceSurface;

	// Per-VM bounds. Defaults suit an interactive tool graph.
	struct NodeGraphLimits {
		size_t MaximumObjects = 256;
		size_t MaximumNodes = 1024;
		size_t MaximumLinks = 4096;
		// Bytes of outputs one run may produce.
		size_t MaximumValueBytes = 4 * 1024 * 1024;
		// Wall time one run may take before the call raises.
		double MaximumRunMilliseconds = 250.0;
	};

	// Named graphs a host shows to scripts, read-only. Called on the script thread.
	struct NodeGraphHost {
		std::function<std::vector<std::string>()> Names{};
		// Fills the node types and graph for one name, or says why not.
		std::function<
			bool(std::string_view name, ang::Registry &types, ang::Graph &graph, std::string &error)>
			Open{};
	};

	// One VM's node graph objects. Owned by the adapter's context.
	class NodeGraphLibrary {
	  public:
		// `release` lets go of a retained callback; the library calls it when the
		// object holding one is gone.
		NodeGraphLibrary(
			std::function<void(CallbackRef)> release,
			std::shared_ptr<const NodeGraphHost> host = {},
			NodeGraphLimits limits = {}
		);
		~NodeGraphLibrary();

		NodeGraphLibrary(const NodeGraphLibrary &) = delete;
		NodeGraphLibrary &operator=(const NodeGraphLibrary &) = delete;

		const NodeGraphLimits &Limits() const;

		// Live objects, for tests and leak checks.
		size_t ObjectCount() const;

		struct State;
		State &Data();

	  private:
		std::unique_ptr<State> Owned;
	};

	// The surface the catalogue installs as `NodeGraphService`.
	const ServiceSurface &NodeGraphServiceSurface();

	// Embedded sources each adapter runs once, after the services, with the
	// service as its one argument; each returns the `NodeGraph` global.
	std::string_view NodeGraphLuauPrelude();
	std::string_view NodeGraphJavaScriptPrelude();

	// The ang value a script value becomes on a port of `type`, and back.
	// Tables travel as an opaque "table" blob identified by their contents, except
	// an `{ X, Y }` or `{ W, X, Y, Z }` on a vector2 or vector4 port. A host blob
	// reads as nil here; the service hands it out as a handle instead.
	ang::Value ToGraphValue(const ScriptValue &value, std::string_view type);
	ScriptValue ToScriptValue(const ang::Value &value);
}
