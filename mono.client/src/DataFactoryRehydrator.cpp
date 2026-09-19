#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <client/DataFactoryRehydrator.hpp>
#include <client/Scene.hpp>
#include <client/WorldSystems.hpp>

namespace client {
	DataFactoryRehydrator::DataFactoryRehydrator(
		engine::world::Universe &worlds,
		DataFactoryRuntimeList &runtimes,
		uint32_t entities,
		uint32_t width,
		uint32_t height,
		std::shared_ptr<engine::script::DataCaptureBridge> capture,
		std::shared_ptr<engine::script::QueuedDataLifecycleBridge> lifecycle
	)
		: Worlds(worlds), Runtimes(runtimes), Entities(entities), Width(width), Height(height),
		  Capture(std::move(capture)), Lifecycle(std::move(lifecycle)) {}

	bool DataFactoryRehydrator::Prepare(
		engine::world::Universe &candidate, engine::world::WorldId world, std::string &detail
	) {
		if (!world.IsValid() || candidate.Count() != 1) {
			detail = "client data-factory mode supports only one local world";
			return false;
		}
		if (Prepared) {
			detail = "client already holds a prepared data-factory runtime";
			return false;
		}
		const engine::core::Name name = candidate.NameOf(world);
		const engine::world::WorldId live = Worlds.Find(name);
		const auto active =
			std::ranges::find_if(Runtimes, [live](const auto &entry) { return entry.first == live; });
		if (active != Runtimes.end() && active->second != nullptr &&
			!active->second->CanDiscardForWorldSwap()) {
			detail = "checkpoint cannot restore non-serializable script VM state";
			return false;
		}

		const engine::world::WorldSettings settings = candidate.SettingsOf(world);
		bool installed = false;
		if (candidate.Enter(
				world,
				[this, &installed, &settings, &detail](
					engine::ecs::Store &store, engine::ecs::Scheduler &systems
				) {
					if (!engine::script::ScriptsIn(store, true, true).empty()) {
						detail = "checkpoint contains scripts whose VM closures are not serializable";
						return;
					}
					InstallPresentation(store, systems, Entities);
					InstallClientWorldSystems(store, systems, settings.PhysicsTickRate);
					if (EnsureLocalPlayer(store) == engine::ecs::NULL_ENTITY) {
						detail = "client could not establish the single-player client";
						return;
					}
					(void)RestoreDefaultCameraMovement(store, systems);
					(void)InstallDefaultCamera(store, systems);
					if (!engine::scene::SetViewportSize(store, Width, Height)) {
						detail = "client could not establish the viewport";
						return;
					}
					engine::script::RuntimeLimits limits;
					limits.Role = engine::script::HostRole::OfBoth();
					limits.DataCapture = Capture;
					limits.DataLifecycle = Lifecycle;
					Runtimes.reserve(Runtimes.size() + 1);
					Prepared.emplace(
						PreparedRuntime{
							.World = engine::core::Name(store.Name()),
							.Runtime =
								engine::script::MakeRuntime(store, engine::script::Language::Luau, limits),
						}
					);
					installed = Prepared->Runtime != nullptr;
					if (!installed) detail = "client could not create the factory script runtime";
				}
			) != engine::world::WorldStatus::Ok ||
			!installed) {
			if (detail.empty()) detail = "client could not rehydrate factory world state";
			return false;
		}
		return true;
	}

	void DataFactoryRehydrator::Commit() noexcept {
		if (!Prepared) return;
		const engine::world::WorldId world = Worlds.Find(Prepared->World);
		if (!world.IsValid()) return;
		std::erase_if(Runtimes, [world](const auto &entry) { return entry.first == world; });
		Runtimes.emplace_back(world, std::move(Prepared->Runtime));
		Prepared.reset();
	}

	void DataFactoryRehydrator::Abort() noexcept {
		Prepared.reset();
	}

	bool DataFactoryRehydrator::PrepareFork(
		std::string_view branchId,
		engine::world::Universe &branch,
		engine::world::WorldId world,
		std::string &detail
	) {
		if (branchId.empty() || !world.IsValid() || branch.Count() != 1) {
			detail = "client branch runtime needs one named isolated world";
			return false;
		}
		if (PreparedFork || ForkRuntimes.contains(std::string(branchId))) {
			detail = "client already owns this branch runtime";
			return false;
		}
		const engine::world::WorldSettings settings = branch.SettingsOf(world);
		bool installed = false;
		if (branch.Enter(
				world,
				[this, &installed, &settings, &detail](
					engine::ecs::Store &store, engine::ecs::Scheduler &systems
				) {
					if (!engine::script::ScriptsIn(store, true, true).empty()) {
						detail = "fork contains scripts whose VM closures are not serializable";
						return;
					}
					// A branch is stepped by DataFactorySession and has no presentation
					// owner. Install simulation only, so it cannot accidentally submit
					// into the parent client's renderer.
					InstallClientWorldSystems(store, systems, settings.PhysicsTickRate);
					engine::script::RuntimeLimits limits;
					limits.Role = engine::script::HostRole::OfBoth();
					limits.DataCapture = Capture;
					limits.DataLifecycle = Lifecycle;
					PreparedFork.emplace(
						PreparedRuntime{
							.World = engine::core::Name(store.Name()),
							.Runtime =
								engine::script::MakeRuntime(store, engine::script::Language::Luau, limits),
						}
					);
					installed = PreparedFork->Runtime != nullptr;
					if (!installed) detail = "client could not create the fork script runtime";
				}
			) != engine::world::WorldStatus::Ok ||
			!installed) {
			if (detail.empty()) detail = "client could not rehydrate fork runtime state";
			PreparedFork.reset();
			return false;
		}
		return true;
	}

	void DataFactoryRehydrator::CommitFork(std::string_view branchId) {
		if (!PreparedFork) return;
		ForkRuntimes.emplace(std::string(branchId), std::move(PreparedFork->Runtime));
		PreparedFork.reset();
	}

	void DataFactoryRehydrator::AbortFork(std::string_view) noexcept {
		PreparedFork.reset();
	}

	void DataFactoryRehydrator::RetireFork(std::string_view branchId) noexcept {
		ForkRuntimes.erase(std::string(branchId));
	}
}
