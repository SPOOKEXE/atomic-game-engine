#pragma once

// Host-owned atomic scratch-world execution for a verified data-script package.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/world/DataFactory.hpp>

#include <functional>
#include <memory>

namespace engine::script {

	using DataScriptRuntimeLookup = std::function<Runtime *(world::WorldId world)>;
	using DataScriptRuntimeDiscard = std::function<void(world::WorldId world)>;
	using DataScriptPackageRuntimeFactory =
		std::function<std::unique_ptr<Runtime>(ecs::Store &, const RuntimeLimits &)>;
	using DataScriptPackageSystems = std::function<void(ecs::Store &, ecs::Scheduler &)>;
	using DataScriptPackageAdmission = std::function<bool(std::string_view, std::string_view, std::string &)>;
	using DataScriptPackagePreparation =
		std::function<bool(world::Universe &, world::WorldId, std::string &)>;
	using DataScriptPackagePostSwap = std::function<void(world::WorldId)>;
	using DataScriptPackagePreflight = std::function<bool(world::WorldId, std::string &)>;

	// The program supplies its runtime ownership and presentation wiring. The
	// transaction copies only serialized world bytes between the live and scratch
	// universes, so failed package work cannot expose a partial world.
	struct DataScriptPackageTransactionDependencies {
		world::Universe &Universe;
		world::DataFactorySession &Session;
		DataScriptRuntimeLookup RuntimeOf = {};
		DataScriptRuntimeDiscard DiscardRuntime = {};
		DataScriptPackageRuntimeFactory MakeRuntime = {};
		DataScriptPackageRunner RunPackage = {};
		DataScriptPackageSystems InstallSystems = {};
		DataScriptPackagePreparation PrepareWorld = {};
		DataScriptPackagePreflight Preflight = {};
		DataScriptPackagePostSwap AfterSwap = {};
		DataScriptPackageAdmission Admit = {};
		HostRole Role = HostRole::OfServer();
		bool Present = false;
	};

	// Runs a package against a scratch copy and replaces the live universe only
	// after terminal package work, presentation wiring, and lifecycle commit all
	// succeed. The result is a value reply suitable for the control-surface row.
	DataScriptResult ExecuteDataScriptPackageTransaction(
		const DataScriptPackageTransactionDependencies &dependencies, const DataScriptRequest &request
	);
}
