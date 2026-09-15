#pragma once

// The editor's ownership layer around the engine data-factory session.
//
// The shared session owns lifecycle revisions, factory ownership and pause
// state. Studio supplies only product work to furnish a candidate world and
// release residency before retirement.

#include <engine/control/Surface.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/world/DataFactory.hpp>

#include <functional>
#include <memory>
#include <string>

namespace studio {

	struct DataFactoryHostCallbacks {
		std::function<bool(
			engine::world::DataFactoryWorldOperation,
			engine::world::Universe &,
			engine::world::WorldId,
			bool,
			std::string &
		)>
			Lifecycle;
		std::function<bool(engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &)>
			Pause;
		std::function<bool(engine::world::Universe &, engine::world::WorldId, std::string &)> Rehydrate;
		std::function<engine::script::DataScriptPackageTransactionDependencies(
			engine::world::Universe &, engine::world::DataFactorySession &
		)>
			PackageDependencies = {};
	};

	// Binds one empty Studio universe to the shared factory lifecycle and MCP
	// feature. The session refuses compatibility worlds itself, so a normal
	// Studio scene can never become factory-owned by accident.
	class DataFactoryHost final {
	  public:
		bool
		Start(engine::world::Universe &universe, DataFactoryHostCallbacks callbacks, std::string &detail);
		void InstallTools(engine::control::Surface &surface, bool rendererReady);

		// Advances the isolated factory universe only while its lifecycle has resumed it.
		bool Tick(float frameSeconds);

		engine::world::DataFactorySession *Session() {
			return Lifecycle.get();
		}

		const engine::world::DataFactorySession *Session() const {
			return Lifecycle.get();
		}

	  private:
		engine::world::Universe *Worlds = nullptr;
		std::unique_ptr<engine::world::DataFactorySession> Lifecycle;
		std::function<engine::script::DataScriptPackageTransactionDependencies(
			engine::world::Universe &, engine::world::DataFactorySession &
		)>
			PackageDependencies;
	};
}
