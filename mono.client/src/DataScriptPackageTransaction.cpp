#include <engine/assets/ContentHash.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/script/DataScriptPackage.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <client/DataScriptPackageTransaction.hpp>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace client {
	using engine::script::DataScriptPackage;
	using engine::script::DataScriptPackageContext;
	using engine::script::DataScriptPackageRunResult;
	using engine::script::DataScriptRequest;
	using engine::script::DataScriptResult;
	using engine::world::DataFactoryStatus;

	namespace {
		bool ValidateAssets(
			const DataScriptPackage &package, const DataScriptRequest &request, std::string &error
		) {
			size_t assetBytes = 0;
			for (const auto &asset : package.Assets) {
				const auto found = std::ranges::find(
					request.Assets, asset.Path, &engine::script::DataScriptAssetInput::Path
				);
				if (found == request.Assets.end() ||
					found->Bytes.size() > package.Budget.AssetBytes - assetBytes ||
					engine::assets::Hasher::Of(found->Bytes) != asset.Hash) {
					error = "package asset is missing, exceeds its budget, or has a hash mismatch";
					return false;
				}
				assetBytes += found->Bytes.size();
			}
			if (request.Assets.size() != package.Assets.size()) {
				error = "package has undeclared asset bytes";
				return false;
			}
			return true;
		}

		void Fail(DataScriptResult &result, std::string error) {
			result.Error = std::move(error);
		}
	}

	DataScriptResult ExecuteDataScriptPackageTransaction(
		const DataScriptPackageTransactionDependencies &dependencies, const DataScriptRequest &request
	) {
		DataScriptResult result;
		const engine::world::WorldId live =
			dependencies.Universe.Find(engine::core::Name(request.InstanceId));
		result.Lifecycle = dependencies.Session.Inspect(request.InstanceId);
		if (result.Lifecycle.InstanceId.empty()) result.Lifecycle.InstanceId = request.InstanceId;
		if (!live.IsValid() || result.Lifecycle.Status != DataFactoryStatus::Ok) {
			Fail(result, "unknown instance_id");
			return result;
		}

		engine::script::Runtime *const active =
			dependencies.RuntimeOf ? dependencies.RuntimeOf(live) : nullptr;
		if (active != nullptr && !active->CanDiscardForWorldSwap()) {
			Fail(result, "active_script_runtime_unsupported");
			return result;
		}
		if (!dependencies.Session.AllSystemsPaused(request.InstanceId) ||
			result.Lifecycle.Clock.Tick != request.ExpectedTick ||
			result.Lifecycle.WorldEpoch != request.ExpectedEpoch ||
			result.Lifecycle.WorldVersion != request.ExpectedVersion) {
			Fail(result, "expected lifecycle revision does not match an all_systems pause");
			return result;
		}

		const auto parsed = engine::script::ParseDataScriptPackage(request.Manifest);
		if (!parsed || request.Source.size() > parsed.Package->Budget.SourceBytes ||
			engine::assets::Hasher::Of(
				std::as_bytes(std::span(request.Source.data(), request.Source.size()))
			) != parsed.Package->SourceHash) {
			Fail(result, parsed ? "package source_hash does not match source" : parsed.Error);
			return result;
		}
		if (!request.SourceHash.empty()) {
			const auto supplied = engine::assets::ContentHash::FromHex(request.SourceHash);
			if (!supplied || *supplied != parsed.Package->SourceHash) {
				Fail(result, "source_hash does not match the package source_hash");
				return result;
			}
		}
		if (!ValidateAssets(*parsed.Package, request, result.Error)) return result;
		if (!engine::script::CheckDataScriptPackageSource(
				engine::script::Language::Luau, request.Source, parsed.Package->Entry, result.Error
			))
			return result;
		if (!dependencies.MakeRuntime || !dependencies.InstallSystems) {
			Fail(result, "data-script package transaction is missing client dependencies");
			return result;
		}

		engine::core::ByteWriter bytes(0, dependencies.Session.CheckpointByteLimit());
		try {
			if (!dependencies.Universe.Save(bytes)) {
				Fail(result, "live world is not serializable");
				return result;
			}
		} catch (const std::length_error &) {
			Fail(result, "live world exceeds the configured checkpoint byte limit");
			return result;
		}

		engine::world::Universe scratch;
		engine::core::ByteReader reader(bytes.Bytes());
		if (!scratch.Load(reader) || reader.Remaining() != 0) {
			Fail(result, "scratch world could not be loaded");
			return result;
		}
		const engine::world::WorldId scratchWorld = scratch.Find(engine::core::Name(request.InstanceId));
		if (!scratchWorld.IsValid()) {
			Fail(result, "scratch world does not contain instance_id");
			return result;
		}

		std::unique_ptr<engine::script::Runtime> packageRuntime;
		try {
			scratch.Enter(scratchWorld, [&](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				engine::script::RuntimeLimits limits;
				limits.Role = engine::script::HostRole::OfBoth();
				limits.Capabilities = parsed.Package->Capabilities;
				limits.PackageOnly = true;
				packageRuntime = dependencies.MakeRuntime(store, limits);
				if (!packageRuntime) {
					Fail(result, "could not create package runtime");
					return;
				}
				const DataScriptPackageContext context(*parsed.Package, request.Assets);
				const DataScriptPackageRunResult ran =
					dependencies.RunPackage
						? dependencies.RunPackage(
							  *packageRuntime, context, request.Source, parsed.Package->Entry
						  )
						: packageRuntime->RunDataScriptPackage(
							  context, request.Source, parsed.Package->Entry
						  );
				if (ran.Terminal != DataScriptPackageRunResult::State::Completed) {
					Fail(result, ran.Error.empty() ? "runtime rejected the package source" : ran.Error);
					return;
				}
				dependencies.InstallSystems(store, systems);
			});
		} catch (const std::exception &exception) {
			Fail(result, "package execution failed: " + std::string(exception.what()));
		} catch (...) {
			Fail(result, "package execution failed");
		}
		// The scratch VM may keep bindings into its store. Tear it down before
		// the candidate takes ownership of the live universe's slot.
		packageRuntime.reset();
		if (!result.Error.empty()) return result;

		result.Lifecycle = dependencies.Session.CommitExternalMutation(
			request.InstanceId, request.ExpectedTick, request.ExpectedVersion
		);
		if (result.Lifecycle.Status != DataFactoryStatus::Ok) {
			Fail(result, result.Lifecycle.Detail);
			return result;
		}
		if (dependencies.DiscardRuntime) dependencies.DiscardRuntime(live);
		dependencies.Universe.ReplaceWith(scratch);
		result.Package = *parsed.Package;
		result.Atomic = true;
		result.Ran = true;
		return result;
	}
}
