// The server's MCP composition root.
//
// This file only chooses rows for services this host owns. The control kernel
// owns JSON-RPC dispatch and each feature owns parsing and response shape.

#include <engine/control/DataScriptPackage.hpp>
#include <engine/control/Features.hpp>
#include <engine/control/features/DataScene.hpp>
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

		std::string failure;
		ProductControlHook = ControlSurface.ActivateHook(
			{.Id = "server.product",
			 .Revision = "v1",
			 .Purpose = "Reads and administers state owned by this dedicated server.",
			 .Dependencies = {},
			 .Limits = {}},
			[this](engine::control::HookRegistration &registration) { RegisterControlTools(registration); },
			failure
		);
		if (!ProductControlHook.IsValid()) {
			throw std::runtime_error("could not install server control product hook: " + failure);
		}

		if (DataFactory == nullptr) return;

		const auto abortActivation = [this](std::string message) {
			FactoryPackageControlHook.Close();
			FactoryCameraRenderingControlHook.Close();
			FactorySceneControlHook.Close();
			FactoryLifecycleControlHook.Close();
			throw std::runtime_error(std::move(message));
		};
		FactoryLifecycleControlHook = ControlSurface.ActivateHook(
			{.Id = "server.data-factory.lifecycle",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server data-factory lifecycle controls.",
			 .Dependencies = {},
			 .Limits = {}},
			[this](engine::control::HookRegistration &) {
				ControlSurface.AddDataFactoryTools(*DataFactory, {.RenderOnly = false});
			},
			failure
		);
		if (!FactoryLifecycleControlHook.IsValid()) {
			abortActivation("could not activate server factory lifecycle hook: " + failure);
		}

		FactorySceneControlHook = ControlSurface.ActivateHook(
			{.Id = "server.data-factory.raw-scene",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory scene and retained-export reads.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[this](engine::control::HookRegistration &) {
				ControlSurface.AddDataSceneTools(Worlds(), {}, DataFactory.get());
			},
			failure
		);
		if (!FactorySceneControlHook.IsValid()) {
			abortActivation("could not activate server factory raw-scene hook: " + failure);
		}

		FactoryCameraRenderingControlHook = ControlSurface.ActivateHook(
			{.Id = "server.data-factory.camera-rendering",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory camera calibration reads.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[this](engine::control::HookRegistration &registration) {
				registration.Add(
					engine::control::features::CameraRenderingDataTool(Worlds(), DataFactory.get())
				);
			},
			failure
		);
		if (!FactoryCameraRenderingControlHook.IsValid()) {
			abortActivation("could not activate server factory camera-rendering hook: " + failure);
		}

		FactoryPackageControlHook = ControlSurface.ActivateHook(
			{.Id = "server.data-factory.package",
			 .Revision = "v1",
			 .Purpose = "Dedicated-server factory atomic script package replacement.",
			 .Dependencies = {"server.data-factory.lifecycle"},
			 .Limits = {}},
			[this](engine::control::HookRegistration &) {
				engine::control::AddDataScriptPackageTool(
					ControlSurface, [this](const engine::script::DataScriptRequest &request) {
						return engine::script::ExecuteDataScriptPackageTransaction(
							{.Universe = Worlds(),
							 .Session = *DataFactory,
							 .RuntimeOf = [this](engine::world::WorldId world) { return RuntimeOf(world); },
							 .DiscardRuntime =
								 [this](engine::world::WorldId world) {
									 std::erase_if(Runtimes, [world](const auto &entry) {
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
	}
}
