#pragma once

// Client-owned reconstruction of state that a universe checkpoint omits.

#include <engine/core/Name.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/DataFactory.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::script {
	class DataCaptureBridge;
	class QueuedDataLifecycleBridge;
}

namespace client {
	// Type used for Data Factory Runtime List.
	using DataFactoryRuntimeList =
		std::vector<std::pair<engine::world::WorldId, std::shared_ptr<engine::script::Runtime>>>;

	// Rebuilds one compatible factory world's scheduler and empty host runtime.
	// Script closures are VM state rather than world state and therefore refuse
	// rehydration before a candidate can replace the live world.
	class DataFactoryRehydrator final {
	  public:
		// Builds the client-side transaction that stages replacement factory worlds.
		DataFactoryRehydrator(
			engine::world::Universe &worlds,
			DataFactoryRuntimeList &runtimes,
			uint32_t entities,
			uint32_t width,
			uint32_t height,
			std::shared_ptr<engine::script::DataCaptureBridge> capture,
			std::shared_ptr<engine::script::QueuedDataLifecycleBridge> lifecycle
		);

		// Stages candidate world resources before the factory atomically swaps them in.
		bool Prepare(engine::world::Universe &candidate, engine::world::WorldId world, std::string &detail);
		// Publishes the prepared replacement world and releases staging state.
		void Commit() noexcept;
		// Discards staged replacement state after a failed rehydration.
		void Abort() noexcept;

		// A fork owns an isolated Universe, so its VM ownership cannot share the
		// live-world replacement transaction. Branches with scripts are refused:
		// closures are not present in a checkpoint and recreating them would invent
		// execution state. The compatible no-script path still owns an empty VM by
		// branch id, which keeps later branch-only host state out of the parent.
		bool PrepareFork(
			std::string_view branchId,
			engine::world::Universe &branch,
			engine::world::WorldId world,
			std::string &detail
		);
		// Publishes the prepared fork identified by branchId.
		void CommitFork(std::string_view branchId);
		// Discards a fork that did not reach a publishable state.
		void AbortFork(std::string_view branchId) noexcept;
		// Removes a published fork after its factory lease ends.
		void RetireFork(std::string_view branchId) noexcept;

	  private:
		struct PreparedRuntime {
			engine::core::Name World;
			std::shared_ptr<engine::script::Runtime> Runtime;
		};

		engine::world::Universe &Worlds;
		DataFactoryRuntimeList &Runtimes;
		uint32_t Entities = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::shared_ptr<engine::script::DataCaptureBridge> Capture;
		std::shared_ptr<engine::script::QueuedDataLifecycleBridge> Lifecycle;
		std::optional<PreparedRuntime> Prepared;
		std::optional<PreparedRuntime> PreparedFork;
		std::unordered_map<std::string, std::shared_ptr<engine::script::Runtime>> ForkRuntimes;
	};
}
