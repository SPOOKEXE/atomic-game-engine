// The server's MCP composition root.
//
// This file only chooses rows for services this host owns. The control kernel
// owns JSON-RPC dispatch and each feature owns parsing and response shape.

#include <engine/control/DataScriptPackage.hpp>
#include <engine/control/Features.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/PhysicsObservation.hpp>
#include <engine/control/features/ReplicationObservation.hpp>
#include <engine/control/features/Script.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <array>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <stdexcept>
#include <string_view>

namespace server {
	namespace {
		// The replication reader borrows only these server-owned services for one hook lease.
		struct ReplicationObservationControlHookContext {
			engine::control::Surface &Surface;
			engine::world::DataFactorySession *Session = nullptr;
			engine::replication::ReplicationObservations &Records;
			std::string PrimaryWorld;
		};

		struct ServerProductControlHookContext {
			Server &Host;
			engine::control::Surface &Surface;
		};

		struct ServerFactoryControlHookContext {
			Server &Host;
			engine::control::Surface &Surface;
			engine::world::Universe &Universe;
			engine::world::DataFactorySession &Session;
		};
	}

	void Server::ConfigureControlHooks() {
		const std::array standard{
			// The richer server row is owned by server.product below.
			engine::control::features::Universe(Worlds(), true, false),
			engine::control::features::Architecture(),
			engine::control::features::Script(),
			engine::control::features::Diagnostics(),
			engine::control::features::Build(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
			engine::control::features::Discovery(),
		};
		ControlSurface.Enable(standard);

		const ServerProductControlHookContext productContext{.Host = *this, .Surface = ControlSurface};
		std::string failure;
		ProductControlHook = productContext.Surface.ActivateHook(
			{.Id = "server.product",
			 .Revision = "v1",
			 .Purpose = "Reads and administers state owned by this dedicated server.",
			 .Dependencies = {},
			 .Limits = {}},
			[productContext](engine::control::HookRegistration &registration) {
				productContext.Host.RegisterControlTools(registration);
			},
			failure
		);
		if (!ProductControlHook.IsValid()) {
			throw std::runtime_error("could not install server control product hook: " + failure);
		}

		if (ReplicationObservationRecords != nullptr) {
			const ReplicationObservationControlHookContext context{
				.Surface = ControlSurface,
				.Session = DataFactory.get(),
				.Records = *ReplicationObservationRecords,
				.PrimaryWorld = std::string(Worlds().NameOf(PrimaryWorld).Text()),
			};
			ReplicationObservationControlHook = ControlSurface.ActivateHook(
				{.Id = "server.replication-observation",
				 .Revision = "v1",
				 .Purpose = "Completed replication exchange observations for the listening server.",
				 .Dependencies = {"server.product"},
				 .Limits = {{"records", engine::replication::ReplicationObservations::MAXIMUM_RECORDS}}},
				[context](engine::control::HookRegistration &) {
					engine::control::AddReplicationObservationTools(
						context.Surface, context.Session, context.Records, context.PrimaryWorld
					);
				},
				failure
			);
			if (!ReplicationObservationControlHook.IsValid()) {
				ProductControlHook.Close();
				throw std::runtime_error(
					"could not activate server replication observation hook: " + failure
				);
			}
		}

		if (DataFactory == nullptr) return;
		const ServerFactoryControlHookContext factoryContext{
			.Host = *this, .Surface = ControlSurface, .Universe = Worlds(), .Session = *DataFactory
		};

		const auto abortActivation = [this](std::string message) {
			FactoryPackageControlHook.Close();
			FactoryCameraRenderingControlHook.Close();
			FactoryPhysicsControlHook.Close();
			FactorySceneControlHook.Close();
			FactoryLifecycleControlHook.Close();
			ReplicationObservationControlHook.Close();
			ProductControlHook.Close();
			throw std::runtime_error(std::move(message));
		};
		FactoryLifecycleControlHook = factoryContext.Surface.ActivateHook(
			{.Id = "server.data-factory.lifecycle",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server data-factory lifecycle controls.",
			 .Dependencies = {},
			 .Limits = {}},
			[factoryContext](engine::control::HookRegistration &) {
				factoryContext.Surface.AddDataFactoryTools(factoryContext.Session, {.RenderOnly = false});
			},
			failure
		);
		if (!FactoryLifecycleControlHook.IsValid()) {
			abortActivation("could not activate server factory lifecycle hook: " + failure);
		}

		FactorySceneControlHook = factoryContext.Surface.ActivateHook(
			{.Id = "server.data-factory.raw-scene",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory scene and retained-export reads.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[factoryContext](engine::control::HookRegistration &) {
				factoryContext.Surface.AddDataSceneTools(
					factoryContext.Universe, {}, &factoryContext.Session
				);
			},
			failure
		);
		if (!FactorySceneControlHook.IsValid()) {
			abortActivation("could not activate server factory raw-scene hook: " + failure);
		}

		FactoryCameraRenderingControlHook = factoryContext.Surface.ActivateHook(
			{.Id = "server.data-factory.camera-rendering",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory camera calibration reads.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[factoryContext](engine::control::HookRegistration &registration) {
				registration.Add(
					engine::control::features::CameraRenderingDataTool(
						factoryContext.Universe, &factoryContext.Session
					)
				);
			},
			failure
		);
		if (!FactoryCameraRenderingControlHook.IsValid()) {
			abortActivation("could not activate server factory camera-rendering hook: " + failure);
		}

		FactoryPackageControlHook = factoryContext.Surface.ActivateHook(
			{.Id = "server.data-factory.package",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory atomic script package replacement.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[factoryContext](engine::control::HookRegistration &) {
				engine::control::AddDataScriptPackageTool(
					factoryContext.Surface,
					[factoryContext](const engine::script::DataScriptRequest &request) {
						return engine::script::ExecuteDataScriptPackageTransaction(
							{.Universe = factoryContext.Universe,
							 .Session = factoryContext.Session,
							 .RuntimeOf = [factoryContext](
											  engine::world::WorldId world
										  ) { return factoryContext.Host.RuntimeOf(world); },
							 .DiscardRuntime =
								 [factoryContext](engine::world::WorldId world) {
									 std::erase_if(factoryContext.Host.Runtimes, [world](const auto &entry) {
										 return entry.first == world;
									 });
								 },
							 .MakeRuntime =
								 [](engine::ecs::Store &store, const engine::script::RuntimeLimits &limits) {
									 return engine::script::MakeRuntime(
										 store, engine::script::Language::Luau, limits
									 );
								 },
							 .RunPackage = engine::script::RunDataScriptPackage,
							 .InstallSystems = [](
												   engine::ecs::Store &store, engine::ecs::Scheduler &systems
											   ) { RegisterPlaceholderSystems(store, systems); },
							 .Admit =
								 [](std::string_view source, std::string_view entry, std::string &error) {
									 return engine::script::CheckDataScriptPackageSource(
										 engine::script::Language::Luau, source, entry, error
									 );
								 },
							 .Role = engine::script::HostRole::OfServer(),
							 .Present = false},
							request
						);
					}
				);
			},
			failure
		);
		if (!FactoryPackageControlHook.IsValid()) {
			abortActivation("could not activate server factory package hook: " + failure);
		}

		FactoryPhysicsControlHook = factoryContext.Surface.ActivateHook(
			{.Id = "server.data-factory.physics-observation",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server completed fixed-step physics observations.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {{"records", engine::physics::PhysicsObservationLog::CAPACITY}}},
			[factoryContext](engine::control::HookRegistration &) {
				engine::control::AddPhysicsObservationTools(factoryContext.Surface, factoryContext.Session);
			},
			failure
		);
		if (!FactoryPhysicsControlHook.IsValid()) {
			abortActivation("could not activate server factory physics observation hook: " + failure);
		}
	}
}
