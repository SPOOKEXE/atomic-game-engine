#include <engine/control/DataScriptPackage.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/control/features/DataScene.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <studio/DataFactoryHost.hpp>

namespace studio {

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

	void DataFactoryHost::InstallTools(engine::control::Surface &surface, bool rendererReady) {
		if (Lifecycle == nullptr) return;

		// Studio has a renderer, but this host slice has no snapshot-bound
		// presenter yet. Do not list render_only until one can complete it.
		const engine::control::DataFactoryToolSet tools{.RenderOnly = false};
		(void)rendererReady;
		const std::array features{
			engine::control::features::DataFactory(*Lifecycle, tools),
			engine::control::features::DataScene(*Worlds, {}, Lifecycle.get()),
		};
		surface.Enable(features);
		if (PackageDependencies)
			engine::control::AddDataScriptPackageTool(
				surface, [this](const engine::script::DataScriptRequest &request) {
					return engine::script::ExecuteDataScriptPackageTransaction(
						PackageDependencies(*Worlds, *Lifecycle), request
					);
				}
			);
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
				if (instance.empty() || instance.size() > 128 || instance.find('\0') != std::string::npos) {
					failure = "validation_failed: instance_id must contain 1 to 128 bytes";
					return nlohmann::json(nullptr);
				}
				const engine::world::DataFactoryReply reply = session->Inspect(instance);
				if (reply.Status != engine::world::DataFactoryStatus::Ok) {
					failure = std::string(engine::world::Describe(reply.Status)) + ": " + reply.Detail;
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
	}
}
