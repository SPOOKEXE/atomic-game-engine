#include <engine/assets/ContentHash.hpp>
#include <engine/script/DataScriptExecutor.hpp>

#include <span>

namespace engine::script {
	namespace {
		bool ValidPackageInputPath(std::string_view path) {
			if (path.empty() || path.size() > 256 || path.front() == '/' ||
				path.find('\\') != std::string_view::npos)
				return false;
			size_t segment = 0;
			while (segment < path.size()) {
				const size_t end = path.find('/', segment);
				const std::string_view part = path.substr(
					segment, end == std::string_view::npos ? path.size() - segment : end - segment
				);
				if (part.empty() || part == "." || part == "..") return false;
				if (end == std::string_view::npos) return true;
				segment = end + 1;
			}
			return false;
		}

		const DataScriptAssetInput *
		InputFor(std::span<const DataScriptAssetInput> inputs, std::string_view path) {
			const DataScriptAssetInput *found = nullptr;
			for (const DataScriptAssetInput &input : inputs) {
				if (!ValidPackageInputPath(input.Path)) return nullptr;
				if (input.Path == path) {
					if (found != nullptr) return nullptr;
					found = &input;
				}
			}
			return found;
		}
	}

	bool DataScriptPackageContext::Can(ScriptCapabilities capability) const {
		return Package != nullptr && HasCapabilities(Package->Capabilities, capability);
	}

	const DataScriptScalar *DataScriptPackageContext::Parameter(std::string_view name) const {
		if (Package == nullptr) return nullptr;
		for (const DataScriptParameter &parameter : Package->Parameters)
			if (parameter.Name == name) return &parameter.Value;
		return nullptr;
	}

	std::optional<std::span<const std::byte>> DataScriptPackageContext::Asset(std::string_view path) const {
		if (Package == nullptr) return std::nullopt;
		for (const DataScriptPackageAsset &asset : Package->Assets) {
			if (asset.Path != path) continue;
			const DataScriptAssetInput *input = InputFor(Inputs, path);
			if (input == nullptr || input->Bytes.size() > Package->Budget.AssetBytes ||
				assets::Hasher::Of(input->Bytes) != asset.Hash)
				return std::nullopt;
			return std::span<const std::byte>(input->Bytes);
		}
		return std::nullopt;
	}

	uint64_t DataScriptPackageContext::SeedStream(std::string_view name) const {
		return Package == nullptr ? 0 : DataScriptSeedStream(Package->Seed, name);
	}

	DataScriptResult ExecuteDataScript(
		world::Universe &universe,
		world::DataFactorySession &session,
		const DataScriptRequest &request,
		const DataScriptRuntimeResolver &runtimeOf,
		const DataScriptPackageRunner &run
	) {
		DataScriptResult result;
		const DataScriptPackageParseResult parsed = ParseDataScriptPackage(request.Manifest);
		if (!parsed) {
			result.Error = parsed.Error;
			return result;
		}
		const DataScriptPackage &package = *parsed.Package;
		result.Package = package;
		if (request.InstanceId.empty() || request.Source.size() > package.Budget.SourceBytes) {
			result.Error = "source or instance_id exceeds the package budget";
			return result;
		}
		const auto actual =
			assets::Hasher::Of(std::as_bytes(std::span(request.Source.data(), request.Source.size())));
		if (actual != package.SourceHash) {
			result.Error = "package source_hash does not match blake3-256 source";
			return result;
		}
		if (!request.SourceHash.empty()) {
			const auto supplied = assets::ContentHash::FromHex(request.SourceHash);
			if (!supplied || *supplied != package.SourceHash) {
				result.Error = "source_hash does not match the package source_hash";
				return result;
			}
		}

		uint64_t assetBytes = 0;
		for (const DataScriptPackageAsset &asset : package.Assets) {
			const DataScriptAssetInput *input = InputFor(request.Assets, asset.Path);
			if (input == nullptr || input->Bytes.size() > package.Budget.AssetBytes - assetBytes) {
				result.Error = "package asset is missing, duplicated, or exceeds the budget";
				return result;
			}
			assetBytes += input->Bytes.size();
			if (assets::Hasher::Of(input->Bytes) != asset.Hash) {
				result.Error = "package asset hash does not match blake3-256 bytes";
				return result;
			}
		}
		if (request.Assets.size() != package.Assets.size()) {
			result.Error = "package has undeclared asset bytes";
			return result;
		}

		result.Lifecycle = session.Inspect(request.InstanceId);
		if (result.Lifecycle.Status != world::DataFactoryStatus::Ok) {
			result.Error = result.Lifecycle.Detail;
			return result;
		}
		if (result.Lifecycle.Clock.Tick != request.ExpectedTick ||
			result.Lifecycle.WorldEpoch != request.ExpectedEpoch ||
			result.Lifecycle.WorldVersion != request.ExpectedVersion) {
			result.Error = "expected lifecycle revision does not match";
			return result;
		}
		if (!session.AllSystemsPaused(request.InstanceId)) {
			result.Error = "data script execution requires an all_systems pause";
			return result;
		}
		const world::WorldId world = universe.Find(core::Name(request.InstanceId));
		Runtime *runtime = world.IsValid() ? runtimeOf(world, package.Capabilities) : nullptr;
		if (runtime == nullptr) {
			result.Error = "no runtime is installed for the data-script world";
			return result;
		}
		if (runtime->Access() != package.Capabilities) {
			result.Error = "runtime is not package-scoped to the declared capabilities";
			return result;
		}

		std::string checkpoint;
		result.Lifecycle = session.Checkpoint(request.InstanceId, checkpoint);
		if (result.Lifecycle.Status != world::DataFactoryStatus::Ok) {
			result.Error =
				result.Lifecycle.Detail.empty() ? "atomic rollback is unavailable" : result.Lifecycle.Detail;
			return result;
		}
		result.Atomic = true;
		DataScriptPackageContext context(package, request.Assets);
		DataScriptPackageRunResult executed;
		try {
			executed = run ? run(*runtime, context, request.Source, package.Entry)
						   : runtime->RunDataScriptPackage(context, request.Source, package.Entry);
		} catch (const std::exception &exception) {
			executed.Error = "package executor threw: " + std::string(exception.what());
		} catch (...) {
			executed.Error = "package executor threw an unknown exception";
		}
		if (executed.Terminal == DataScriptPackageRunResult::State::Completed) {
			const world::DataFactoryReply committed = session.CommitExternalMutation(
				request.InstanceId, request.ExpectedTick, request.ExpectedVersion
			);
			if (committed.Status != world::DataFactoryStatus::Ok) {
				result.Lifecycle = session.Restore(request.InstanceId, checkpoint);
				if (result.Lifecycle.Status != world::DataFactoryStatus::Ok) {
					result.Atomic = false;
					result.Error = committed.Detail + "; rollback failed: " + result.Lifecycle.Detail;
				} else {
					result.Error = committed.Detail;
				}
				return result;
			}
			result.Lifecycle = committed;
			result.Ran = true;
			return result;
		}

		const std::string runtimeError =
			executed.Error.empty() ? executed.Terminal == DataScriptPackageRunResult::State::Deferred
										 ? "package execution deferred work past the atomic boundary"
									 : runtime->LastError().empty() ? "runtime rejected the package source"
																	: runtime->LastError()
								   : executed.Error;
		result.Lifecycle = session.Restore(request.InstanceId, checkpoint);
		if (result.Lifecycle.Status == world::DataFactoryStatus::Ok) {
			result.Error = runtimeError;
		} else {
			result.Atomic = false;
			result.Error = runtimeError + "; rollback failed: " + result.Lifecycle.Detail;
		}
		return result;
	}
}
