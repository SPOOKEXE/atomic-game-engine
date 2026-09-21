#pragma once

// Host-owned atomic scratch-world execution for a verified data-script package.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/world/DataFactory.hpp>

#include <functional>
#include <memory>

namespace engine::script {

	// A data-script execution runtime lookup.
	using DataScriptRuntimeLookup = std::function<Runtime *(world::WorldId world)>;
	// A data-script execution runtime discard.
	using DataScriptRuntimeDiscard = std::function<void(world::WorldId world)>;
	// A data-script package runtime factory.
	using DataScriptPackageRuntimeFactory =
		std::function<std::unique_ptr<Runtime>(ecs::Store &, const RuntimeLimits &)>;
	// A data-script package systems.
	using DataScriptPackageSystems = std::function<void(ecs::Store &, ecs::Scheduler &)>;
	// A data-script package admission.
	using DataScriptPackageAdmission = std::function<bool(std::string_view, std::string_view, std::string &)>;
	// A data-script package preparation.
	using DataScriptPackagePreparation =
		std::function<bool(world::Universe &, world::WorldId, std::string &)>;
	// A data-script package post swap.
	using DataScriptPackagePostSwap = std::function<void(world::WorldId)>;
	// A data-script package preflight.
	using DataScriptPackagePreflight = std::function<bool(world::WorldId, std::string &)>;

	// The program supplies its runtime ownership and presentation wiring. The
	// transaction copies only serialized world bytes between the live and scratch
	// universes, so failed package work cannot expose a partial world.
	struct DataScriptPackageTransactionDependencies {
		// Live universe whose target world may be replaced after a successful run.
		world::Universe &Universe;
		// Host-owned paused data-factory session for snapshot and commit operations.
		world::DataFactorySession &Session;
		// Looks up a live runtime by world identity.
		DataScriptRuntimeLookup RuntimeOf = {};
		// Destroys a runtime after its scratch world is discarded or replaced.
		DataScriptRuntimeDiscard DiscardRuntime = {};
		// Creates the isolated runtime attached to the scratch store.
		DataScriptPackageRuntimeFactory MakeRuntime = {};
		// Executes verified source through the isolated runtime's binding layer.
		DataScriptPackageRunner RunPackage = {};
		// Installs the systems required by the scratch world before execution.
		DataScriptPackageSystems InstallSystems = {};
		// Restores copied world state and host wiring into the scratch universe.
		DataScriptPackagePreparation PrepareWorld = {};
		// Confirms the live world can be swapped before package work begins.
		DataScriptPackagePreflight Preflight = {};
		// Reconnects host presentation after the scratch world replaces the live world.
		DataScriptPackagePostSwap AfterSwap = {};
		// Applies host policy to the requested instance and package manifest.
		DataScriptPackageAdmission Admit = {};
		// Host role used when admitting and running the package.
		HostRole Role = HostRole::OfServer();
		// Whether the transaction must install presentation-facing systems.
		bool Present = false;
	};

	// Runs a package against a scratch copy and replaces the live universe only
	// after terminal package work, presentation wiring, and lifecycle commit all
	// succeed. The result is a value reply suitable for the control-surface row.
	DataScriptResult ExecuteDataScriptPackageTransaction(
		const DataScriptPackageTransactionDependencies &dependencies, const DataScriptRequest &request
	);
}
