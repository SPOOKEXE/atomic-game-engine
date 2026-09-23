#include <engine/control/DataScriptPackage.hpp>
#include <engine/control/features/PhysicsObservation.hpp>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <studio/DataFactoryHost.hpp>

namespace studio {

	DataFactoryHost::~DataFactoryHost() {
		CloseControlHooks();
	}

	void DataFactoryHost::CloseControlHooks() {
		FactorySelectionHook.Close();
		FactoryPackageHook.Close();
		FactoryPhysicsHook.Close();
		FactorySceneHook.Close();
		FactoryLifecycleHook.Close();
	}

	bool DataFactoryHost::Start(
		engine::world::Universe &universe, DataFactoryHostCallbacks callbacks, std::string &detail
	) {
		if (Lifecycle != nullptr) {
			detail = "Studio data-factory host is already started";
			return false;
		}
		if (universe.Count() != 0) {
			detail = "Studio data-factory mode requires an empty universe";
			return false;
		}
		if (!callbacks.Lifecycle || !callbacks.Pause || !callbacks.Rehydrate) {
			detail = "Studio data-factory host is missing a lifecycle participant";
			return false;
		}

		Worlds = &universe;
		Lifecycle = std::make_unique<engine::world::DataFactorySession>(universe);
		Lifecycle->SetWorldLifecycle(std::move(callbacks.Lifecycle));
		Lifecycle->SetPauseParticipant(std::move(callbacks.Pause));
		Lifecycle->SetRehydrate(std::move(callbacks.Rehydrate));
		PackageDependencies = std::move(callbacks.PackageDependencies);
		return true;
	}

	bool DataFactoryHost::Tick(float frameSeconds) {
		if (Worlds == nullptr || Lifecycle == nullptr) return false;
		for (const engine::world::WorldId world : Worlds->Worlds()) {
			if (Worlds->StateOf(world) != engine::world::WorldState::Active) continue;
			Worlds->Tick(frameSeconds);
			return true;
		}
		return false;
	}

	void DataFactoryHost::InstallTools(DataFactoryControlHookContext context) {
		auto &[surface, rendererReady] = context;
		if (Lifecycle == nullptr) return;
		CloseControlHooks();

		// Studio has a renderer, but this host slice has no snapshot-bound
		// presenter yet. Do not list render_only until one can complete it.
		const engine::control::DataFactoryToolSet tools{.RenderOnly = false};
		(void)rendererReady;
		std::string failure;
		const auto abortActivation = [this](std::string message) {
			FactorySelectionHook.Close();
			FactoryPackageHook.Close();
			FactoryPhysicsHook.Close();
			FactorySceneHook.Close();
			FactoryLifecycleHook.Close();
			throw std::runtime_error(std::move(message));
		};
		FactoryLifecycleHook = surface.ActivateHook(
			{
				.Id = "studio.data-factory.lifecycle",
				.Revision = "v1",
				.Purpose = "Studio data-factory lifecycle controls.",
				.Dependencies = {},
				.Limits = {},
			},
			[this, &surface, tools](engine::control::HookRegistration &) {
				surface.AddDataFactoryTools(*Lifecycle, tools);
			},
			failure
		);
		if (!failure.empty()) abortActivation("could not activate Studio factory lifecycle hook: " + failure);

		FactorySceneHook = surface.ActivateHook(
			{
				.Id = "studio.data-factory.raw-scene",
				.Revision = "v1",
				.Purpose = "Studio factory scene, raw-scene, and retained-export reads.",
				.Dependencies = {"studio.data-factory.lifecycle"},
				.Limits = {},
			},
			[this, &surface](engine::control::HookRegistration &) {
				surface.AddDataSceneTools(*Worlds, {}, Lifecycle.get());
			},
			failure
		);
		if (!failure.empty()) abortActivation("could not activate Studio raw-scene hook: " + failure);

		FactoryPhysicsHook = surface.ActivateHook(
			{
				.Id = "studio.data-factory.physics-observation",
				.Revision = "v1",
				.Purpose = "Studio factory fixed-step physics observation reads.",
				.Dependencies = {"studio.data-factory.lifecycle"},
				.Limits = {},
			},
			[this, &surface](engine::control::HookRegistration &) {
				engine::control::AddPhysicsObservationTools(surface, *Lifecycle);
			},
			failure
		);
		if (!failure.empty())
			abortActivation("could not activate Studio physics observation hook: " + failure);

		if (PackageDependencies) {
			FactoryPackageHook = surface.ActivateHook(
				{
					.Id = "studio.data-factory.package",
					.Revision = "v1",
					.Purpose = "Studio factory atomic script package replacement.",
					.Dependencies = {"studio.data-factory.lifecycle"},
					.Limits = {},
				},
				[this, &surface](engine::control::HookRegistration &) {
					engine::control::AddDataScriptPackageTool(
						surface, [this](const engine::script::DataScriptRequest &request) {
							return engine::script::ExecuteDataScriptPackageTransaction(
								PackageDependencies(*Worlds, *Lifecycle), request
							);
						}
					);
				},
				failure
			);
			if (!failure.empty()) abortActivation("could not activate Studio package hook: " + failure);
		}

		FactorySelectionHook = surface.ActivateHook(
			{
				.Id = "studio.data-factory.selection",
				.Revision = "v1",
				.Purpose = "Studio factory world selection read.",
				.Dependencies = {"studio.data-factory.lifecycle"},
				.Limits = {},
			},
			[this, &surface](engine::control::HookRegistration &) {
				surface.Add({
					"world_select",
					"Selects the one factory-owned Studio world and returns its pinned lifecycle revision.",
					[] {
						return nlohmann::json{
							{"type", "object"},
							{"additionalProperties", false},
							{"properties",
							 {{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}},
							{"required", {"instance_id"}},
						};
					},
					[session = Lifecycle.get()](const nlohmann::json &values, std::string &failure) {
						if (!values.is_object() || values.size() != 1 || !values.contains("instance_id") ||
							!values.at("instance_id").is_string()) {
							failure = "validation_failed: instance_id must be the only string argument";
							return nlohmann::json(nullptr);
						}
						const std::string instance = values.at("instance_id").get<std::string>();
						if (instance.empty() || instance.size() > 128 ||
							instance.find('\0') != std::string::npos) {
							failure = "validation_failed: instance_id must contain 1 to 128 bytes";
							return nlohmann::json(nullptr);
						}
						const engine::world::DataFactoryReply reply = session->Inspect(instance);
						if (reply.Status != engine::world::DataFactoryStatus::Ok) {
							failure =
								std::string(engine::world::Describe(reply.Status)) + ": " + reply.Detail;
							return nlohmann::json(nullptr);
						}
						return nlohmann::json{
							{"status", "ok"},
							{"instance_id", reply.InstanceId},
							{"world_epoch", reply.WorldEpoch},
							{"world_version", reply.WorldVersion},
							{"tick", reply.Clock.Tick},
							{"time_ns", reply.Clock.TimeNanoseconds},
						};
					},
				});
			},
			failure
		);
		if (!failure.empty()) abortActivation("could not activate Studio factory selection hook: " + failure);
	}
}
