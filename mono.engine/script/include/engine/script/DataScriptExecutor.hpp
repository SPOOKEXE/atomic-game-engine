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
		// Manifest-relative asset path whose digest is declared by the package.
		std::string Path;
		// Caller-owned asset bytes verified against the path's declared BLAKE3 digest.
		std::vector<std::byte> Bytes;
	};

	// A data-script execution request.
	struct DataScriptRequest {
		// Data-factory instance identifier.
		std::string InstanceId;
		// Untrusted version-one package manifest to parse before execution.
		std::string Manifest;
		// Source bytes whose digest must match the parsed package manifest.
		std::string Source;
		// Digest-addressed package assets.
		std::vector<DataScriptAssetInput> Assets;

		// Kept for callers that already send the source address. When present it
		// must agree with the package's source_hash, so it cannot become a second
		// authority for the same bytes.
		std::string SourceHash;
		// Script-visible name.
		std::string Name = "mcp";
		// Live-world tick required before the package may begin.
		uint64_t ExpectedTick = 0;
		// World incarnation required before the package may begin.
		uint64_t ExpectedEpoch = 0;
		// World state version required before the package may begin.
		uint64_t ExpectedVersion = 0;
	};

	// A data-script execution result.
	struct DataScriptResult {
		// Host lifecycle reply describing admission, execution, or rollback.
		world::DataFactoryReply Lifecycle;
		// Verified package manifest.
		std::optional<DataScriptPackage> Package;
		// True after the runner was invoked with a verified package.
		bool Ran = false;
		// True when the scratch-world result was committed to the live world.
		bool Atomic = false;
		// Failure diagnostic.
		std::string Error;
	};

	// The package's only host-visible values. A package executor must expose
	// this table under its own binding rules and must not retain its views after
	// `ExecuteDataScript` returns.
	class DataScriptPackageContext final {
	  public:
		// Borrows a verified package and its host-supplied assets for one run.
		DataScriptPackageContext(
			const DataScriptPackage &package, std::span<const DataScriptAssetInput> inputs
		)
			: Package(&package), Inputs(inputs) {}

		// Checks whether this capability is granted.
		bool Can(ScriptCapabilities capability) const;
		// Finds a declared scalar parameter by its script-visible name.
		const DataScriptScalar *Parameter(std::string_view name) const;
		// Returns verified asset bytes for a declared manifest-relative path.
		std::optional<std::span<const std::byte>> Asset(std::string_view path) const;
		// Derives the package's deterministic uint64 stream for a stable name.
		uint64_t SeedStream(std::string_view name) const;
		// Returns the verified package manifest borrowed by this execution context.
		const DataScriptPackage &Manifest() const {
			return *Package;
		}

	  private:
		const DataScriptPackage *Package = nullptr;
		std::span<const DataScriptAssetInput> Inputs;
	};

	// A data-script package run result.
	struct DataScriptPackageRunResult {
		// Runner completion state used to decide commit or rollback.
		enum class State : uint8_t { Completed, Deferred, Failed };

		// Terminal state returned by the VM runner.
		State Terminal = State::Failed;
		// Failure diagnostic.
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
