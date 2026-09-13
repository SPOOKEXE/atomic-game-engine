#pragma once

// Client-owned atomic scratch-world execution for a verified data-script package.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/world/DataFactory.hpp>

#include <functional>
#include <memory>

namespace client {

	using DataScriptRuntimeLookup = std::function<engine::script::Runtime *(engine::world::WorldId world)>;
	using DataScriptRuntimeDiscard = std::function<void(engine::world::WorldId world)>;
	using DataScriptPackageRuntimeFactory = std::function<std::unique_ptr<engine::script::Runtime>(
		engine::ecs::Store &store, const engine::script::RuntimeLimits &limits
	)>;
	using DataScriptPackageSystems =
		std::function<void(engine::ecs::Store &store, engine::ecs::Scheduler &systems)>;

	// The program supplies its runtime ownership and presentation wiring. The
	// transaction copies only serialized world bytes between the live and scratch
	// universes, so failed package work cannot expose a partial world.
	struct DataScriptPackageTransactionDependencies {
		engine::world::Universe &Universe;
		engine::world::DataFactorySession &Session;
		DataScriptRuntimeLookup RuntimeOf;
		DataScriptRuntimeDiscard DiscardRuntime;
		DataScriptPackageRuntimeFactory MakeRuntime;
		engine::script::DataScriptPackageRunner RunPackage;
		DataScriptPackageSystems InstallSystems;
	};

	// Runs a package against a scratch copy and replaces the live universe only
	// after terminal package work, presentation wiring, and lifecycle commit all
	// succeed. The result is a value reply suitable for the control-surface row.
	engine::script::DataScriptResult ExecuteDataScriptPackageTransaction(
		const DataScriptPackageTransactionDependencies &dependencies,
		const engine::script::DataScriptRequest &request
	);
}
