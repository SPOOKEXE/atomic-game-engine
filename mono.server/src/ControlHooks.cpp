// The server's MCP composition root.
//
// This file only chooses rows for services this host owns. The control kernel
// owns JSON-RPC dispatch and each feature owns parsing and response shape.

#include <engine/control/DataScriptPackage.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/PhysicsObservation.hpp>
#include <engine/control/features/ReplicationObservation.hpp>
#include <engine/control/features/Script.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/script/DataScriptPackageTransaction.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

		struct ControlHookManifestEntry {
			engine::control::HookDescriptor Descriptor;
			engine::control::HookInstaller Install;
			engine::control::HookLease *Lease;
		};

		engine::control::HookDescriptor BuiltinDescriptor(std::string name) {
			return {
				.Id = "builtin." + std::move(name),
				.Revision = "v1",
				.Purpose = "Built-in feature registration.",
				.Dependencies = {},
				.Limits = {}
			};
		}
	}

	void Server::ConfigureControlHooks() {
		BuiltinControlHooks.resize(8);
		std::vector<ControlHookManifestEntry> manifest;
		manifest.reserve(16);
		const auto builtin =
			[this, &manifest](size_t index, std::string name, engine::control::HookInstaller install) {
				manifest.push_back(
					{BuiltinDescriptor(std::move(name)), std::move(install), &BuiltinControlHooks[index]}
				);
			};
		// The richer server engine_info row is owned by server.product below.
		builtin(0, "universe", [this](engine::control::HookRegistration &) {
			ControlSurface.AddUniverseTools(Worlds(), true, false);
		});
		builtin(1, "architecture", [this](engine::control::HookRegistration &) {
			ControlSurface.AddArchitectureTools();
		});
		builtin(2, "script", [this](engine::control::HookRegistration &) {
			ControlSurface.AddScriptTools();
		});
		builtin(3, "diagnostics", [this](engine::control::HookRegistration &) {
			ControlSurface.AddDiagnosticTools(true);
		});
		builtin(4, "build", [this](engine::control::HookRegistration &) { ControlSurface.AddBuildTools(); });
		builtin(5, "resources", [this](engine::control::HookRegistration &) {
			ControlSurface.AddStandardResources();
		});
		builtin(6, "prompts", [this](engine::control::HookRegistration &) {
			ControlSurface.AddStandardPrompts();
		});
		builtin(7, "discovery", [this](engine::control::HookRegistration &) {
			ControlSurface.AddDiscoveryTools();
		});

		const ServerProductControlHookContext productContext{.Host = *this, .Surface = ControlSurface};
		manifest.push_back(
			{{.Id = "server.product",
			  .Revision = "v1",
			  .Purpose = "Reads and administers state owned by this dedicated server.",
			  .Dependencies = {},
			  .Limits = {}},
			 [productContext](engine::control::HookRegistration &registration) {
				 productContext.Host.RegisterControlTools(registration);
			 },
			 &ProductControlHook}
		);

		if (ReplicationObservationRecords != nullptr) {
			const ReplicationObservationControlHookContext context{
				.Surface = ControlSurface,
				.Session = DataFactory.get(),
				.Records = *ReplicationObservationRecords,
				.PrimaryWorld = std::string(Worlds().NameOf(PrimaryWorld).Text()),
			};
			manifest.push_back(
				{{.Id = "server.replication-observation",
				  .Revision = "v1",
				  .Purpose = "Completed replication exchange observations for the listening server.",
				  .Dependencies = {"server.product"},
				  .Limits = {{"records", engine::replication::ReplicationObservations::MAXIMUM_RECORDS}}},
				 [context](engine::control::HookRegistration &) {
					 engine::control::AddReplicationObservationTools(
						 context.Surface, context.Session, context.Records, context.PrimaryWorld
					 );
				 },
				 &ReplicationObservationControlHook}
			);
		}

		if (DataFactory != nullptr) {
			const ServerFactoryControlHookContext factoryContext{
				.Host = *this, .Surface = ControlSurface, .Universe = Worlds(), .Session = *DataFactory
			};

			manifest.push_back(
				{{.Id = "server.data-factory.lifecycle",
				  .Revision = "v1",
				  .Purpose = "Dedicated-server data-factory lifecycle controls.",
				  .Dependencies = {},
				  .Limits = {}},
				 [factoryContext](engine::control::HookRegistration &) {
					 factoryContext.Surface.AddDataFactoryTools(
						 factoryContext.Session, {.RenderOnly = false}
					 );
				 },
				 &FactoryLifecycleControlHook}
			);

			manifest.push_back(
				{{.Id = "server.data-factory.raw-scene",
				  .Revision = "v1",
				  .Purpose = "Dedicated-server factory scene and retained-export reads.",
				  .Dependencies = {"server.data-factory.lifecycle"},
				  .Limits = {}},
				 [factoryContext](engine::control::HookRegistration &) {
					 factoryContext.Surface.AddDataSceneTools(
						 factoryContext.Universe, {}, &factoryContext.Session
					 );
				 },
				 &FactorySceneControlHook}
			);

			manifest.push_back(
				{{.Id = "server.data-factory.camera-rendering",
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
				 &FactoryCameraRenderingControlHook}
			);

			manifest.push_back(
				{{.Id = "server.data-factory.package",
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
										  std::erase_if(
											  factoryContext.Host.Runtimes,
											  [world](const auto &entry) { return entry.first == world; }
										  );
									  },
								  .MakeRuntime =
									  [](engine::ecs::Store &store,
										 const engine::script::RuntimeLimits &limits) {
										  return engine::script::MakeRuntime(
											  store, engine::script::Language::Luau, limits
										  );
									  },
								  .RunPackage = engine::script::RunDataScriptPackage,
								  .InstallSystems =
									  [](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
										  RegisterPlaceholderSystems(store, systems);
									  },
								  .Admit =
									  [](std::string_view source,
										 std::string_view entry,
										 std::string &error) {
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
				 &FactoryPackageControlHook}
			);

			manifest.push_back(
				{{.Id = "server.data-factory.physics-observation",
				  .Revision = "v1",
				  .Purpose = "Dedicated-server completed fixed-step physics observations.",
				  .Dependencies = {"server.data-factory.lifecycle"},
				  .Limits = {{"records", engine::physics::PhysicsObservationLog::CAPACITY}}},
				 [factoryContext](engine::control::HookRegistration &) {
					 engine::control::AddPhysicsObservationTools(
						 factoryContext.Surface, factoryContext.Session
					 );
				 },
				 &FactoryPhysicsControlHook}
			);
		}

		std::string failure;
		for (ControlHookManifestEntry &entry : manifest) {
			*entry.Lease = ControlSurface.ActivateHook(std::move(entry.Descriptor), entry.Install, failure);
			if (entry.Lease->IsValid()) continue;
			FactoryPackageControlHook.Close();
			FactoryCameraRenderingControlHook.Close();
			FactoryPhysicsControlHook.Close();
			FactorySceneControlHook.Close();
			FactoryLifecycleControlHook.Close();
			ReplicationObservationControlHook.Close();
			ProductControlHook.Close();
			for (auto hook = BuiltinControlHooks.rbegin(); hook != BuiltinControlHooks.rend(); ++hook)
				hook->Close();
			throw std::runtime_error("could not activate server control hook: " + failure);
		}
	}
}
