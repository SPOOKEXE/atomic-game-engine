#pragma once

#include <engine/script/DataScriptPackage.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/DataFactory.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
	// Bytes supplied beside a package manifest. They are copied at the host
	// boundary, then verified against the manifest before a runtime sees them.
	struct DataScriptAssetInput {
		std::string Path;
		std::vector<std::byte> Bytes;
	};

	struct DataScriptRequest {
		std::string InstanceId;
		std::string Manifest;
		std::string Source;
		std::vector<DataScriptAssetInput> Assets;

		// Kept for callers that already send the source address. When present it
		// must agree with the package's source_hash, so it cannot become a second
		// authority for the same bytes.
		std::string SourceHash;
		std::string Name = "mcp";
		uint64_t ExpectedTick = 0;
		uint64_t ExpectedEpoch = 0;
		uint64_t ExpectedVersion = 0;
	};

	struct DataScriptResult {
		world::DataFactoryReply Lifecycle;
		std::optional<DataScriptPackage> Package;
		bool Ran = false;
		bool Atomic = false;
		std::string Error;
	};

	// The package's only host-visible values. A package executor must expose
	// this table under its own binding rules and must not retain its views after
	// `ExecuteDataScript` returns.
	class DataScriptPackageContext final {
	  public:
		DataScriptPackageContext(
			const DataScriptPackage &package, std::span<const DataScriptAssetInput> inputs
		)
			: Package(&package), Inputs(inputs) {}

		bool Can(ScriptCapabilities capability) const;
		const DataScriptScalar *Parameter(std::string_view name) const;
		std::optional<std::span<const std::byte>> Asset(std::string_view path) const;
		uint64_t SeedStream(std::string_view name) const;
		const DataScriptPackage &Manifest() const {
			return *Package;
		}

	  private:
		const DataScriptPackage *Package = nullptr;
		std::span<const DataScriptAssetInput> Inputs;
	};

	struct DataScriptPackageRunResult {
		enum class State : uint8_t { Completed, Deferred, Failed };

		State Terminal = State::Failed;
		std::string Error;
	};

	// Returns a dedicated runtime whose exact grants equal `capabilities`.
	// Reusing a broader runtime is refused by ExecuteDataScript.
	using DataScriptRuntimeResolver =
		std::function<Runtime *(world::WorldId, ScriptCapabilities capabilities)>;

	// Runs package source while making the context available through the VM's
	// binding layer. Completed means no coroutine, promise, or host work remains.
	using DataScriptPackageRunner = std::function<DataScriptPackageRunResult(
		Runtime &, const DataScriptPackageContext &, std::string_view source, std::string_view entry
	)>;

	// Verifies and executes one package against a paused data-factory world.
	//
	// The checkpoint is taken before `Runtime::Run`. A runtime failure restores
	// through DataFactorySession's scratch-universe rehydrate seam. Missing
	// rehydration support therefore refuses before any script runs.
	DataScriptResult ExecuteDataScript(
		world::Universe &universe,
		world::DataFactorySession &session,
		const DataScriptRequest &request,
		const DataScriptRuntimeResolver &runtimeOf,
		const DataScriptPackageRunner &run = {}
	);
}
