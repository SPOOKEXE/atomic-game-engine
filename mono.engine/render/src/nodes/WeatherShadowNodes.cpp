#include "../GpuHeap.hpp"
#include "../ViewRecording.hpp"
#include "../WeatherShadows.hpp"

#include <engine/core/Metrics.hpp>

#include <algorithm>

namespace engine::render {

	void ViewRecording::RegisterWeatherShadowNodes(ContextNodeTable<ViewRecording> &frameNodes) {
		frameNodes.Set(core::Name("weather-shadow-map"), &ViewRecording::RecordNodeWeatherShadowMap);
	}

	bool ViewRecording::RecordNodeWeatherShadowMap(const graph::RunContext &context) {
		if (context.Reads.size() != 2 || context.Writes.size() != 1) return false;
		const auto &environment = CurrentLighting.EnvironmentState;
		const auto &controls = environment.Weather;
		const float effectiveDensity =
			environment.CloudLayer.Density * std::clamp(environment.CloudLayer.Cover, 0.0f, 1.0f);
		auto found = State->WeatherFieldOf(Request.World, Pipeline->Name);
		if (found == nullptr) return true;
		const auto *inputs = found->InputsFor(Command);
		if (!State->Caps.HasCompute || !environment.HasWeatherCompute || !controls.Enabled ||
			!controls.ShadowsEnabled || !(effectiveDensity > 0) || !inputs ||
			inputs->Uniforms.Phase.z == UINT32_MAX || (!inputs->Stateful && found->FieldWords == 0)) {
			found->Shadows = {};
			return true;
		}
		std::array<WeatherResidencyView, weather::MAXIMUM_RESIDENCY_VIEWS> views{};
		size_t viewCount = 0;
		for (const auto &view : State->WeatherViews)
			if (view.World == Request.World && viewCount < views.size()) views[viewCount++] = view.Demand;
		const auto current =
			std::find_if(views.begin(), views.begin() + viewCount, [this](const WeatherResidencyView &view) {
				return view.View.CameraPositionMetres == Request.CameraFrame.Position;
			});
		if (current == views.begin() + viewCount && viewCount < views.size()) {
			WeatherResidencyView view;
			view.View.Key = Request.TargetSlot;
			view.View.CameraPositionMetres = Request.CameraFrame.Position;
			views[viewCount++] = view;
		}
		const WeatherShadowPlan plan = BuildWeatherShadowPlan(
			controls,
			*inputs,
			{State->Sun.x, State->Sun.y, State->Sun.z},
			effectiveDensity,
			scene::MixSignature(environment.Clock.Epoch, environment.WeatherHistoryRevision),
			std::span(views).first(viewCount),
			static_cast<uint32_t>(environment.CloudVolume.Shader)
		);
		if (!plan.Enabled) {
			found->Shadows = {};
			return true;
		}
		const auto *description = Pipeline->Graph.FindResourceDesc(context.Writes.front());
		if (!description || description->Kind != graph::ResourceKind::Storage ||
			description->Format != graph::ResourceFormat::RGBA16F) {
			found->Shadows = {};
			core::Metrics::Count("render.weather.shadow_resource_refusals", 1);
			return true;
		}
		const auto retainCompatible = [&] {
			const auto retained =
				State->FindGraphTarget(*Pipeline, description->Name, graph::NodeScope::World, Request.World);
			if (retained.IsValid() && retained.Texture == found->Shadows.Texture &&
				retained.Width == plan.Resolution && retained.Height == plan.Resolution * plan.Cascades &&
				found->Shadows.Contents.Matches(plan.Signature, Command)) {
				StageHistoryWrites(context, Command);
				return true;
			}
			found->Shadows = {};
			return true;
		};
		const auto retained =
			State->FindGraphTarget(*Pipeline, description->Name, graph::NodeScope::World, Request.World);
		if (retained.IsValid() && found->Shadows.Texture == retained.Texture &&
			retained.Width == plan.Resolution && retained.Height == plan.Resolution * plan.Cascades &&
			found->Shadows.Contents.Matches(plan.Signature, Command)) {
			core::Metrics::Count("render.weather.shadow_cache_hits", 1);
			found->Shadows.Deferred = false;
			StageHistoryWrites(context, Command);
			return true;
		}
		// A frame that made the map stale (a weather tick, the sun moving) is
		// usually the frame that already pays for the field step and the cloud
		// integrate. A ready map of the same size is kept for that frame and
		// rebuilt the next time this world is recorded, so the two costs land on
		// different frames. Every view of the world in that frame sees the same
		// decision, and a map is never put off twice in a row.
		const bool readyAndCompatible = found->Shadows.Contents.Ready && !found->Shadows.Contents.Pending &&
										retained.IsValid() && found->Shadows.Texture == retained.Texture &&
										retained.Width == plan.Resolution &&
										retained.Height == plan.Resolution * plan.Cascades;
		const bool deferringThisFrame =
			found->Shadows.Deferred && found->Shadows.DeferredFrame == State->FrameCounter;
		if (readyAndCompatible && (deferringThisFrame || !found->Shadows.Deferred)) {
			found->Shadows.Deferred = true;
			found->Shadows.DeferredFrame = State->FrameCounter;
			core::Metrics::Count("render.weather.shadow_staggered", 1);
			StageHistoryWrites(context, Command);
			return true;
		}
		found->Shadows.Deferred = false;
		auto *cells = GraphBuffer(context.Reads[0], context, false);
		auto *pages = GraphBuffer(context.Reads[1], context, false);
		if (!cells || !pages || cells != found->Cells || pages != found->Pages) {
			core::Metrics::Count("render.weather.shadow_missing_fields", 1);
			return retainCompatible();
		}
		if (!State->WeatherShadowCompute)
			State->WeatherShadowCompute = State->LoadComputePipeline("weather-shadow.comp", 0, 3, 1, 0, 8, 8);
		if (!State->WeatherShadowCompute) {
			core::Metrics::Count("render.weather.shadow_pipeline_refusals", 1);
			return retainCompatible();
		}
		State->GraphResources.WorldNames[Request.World] = Request.Source->WorldName;
		const auto target = State->EnsureGraphTarget(
			*Pipeline,
			context.Writes.front(),
			Request.World,
			plan.Resolution,
			plan.Resolution * plan.Cascades,
			true
		);
		if (!target.IsValid()) {
			core::Metrics::Count("render.weather.shadow_allocation_refusals", 1);
			return retainCompatible();
		}
		EnterNamedPass(context.Name);
		if (!RecordWeatherShadows(
				Command,
				State->WeatherShadowCompute,
				cells,
				pages,
				target.Texture,
				plan,
				*inputs,
				controls,
				effectiveDensity,
				found->Clock.Buffer
			)) {
			// A refused pass may have cycled the target. Never certify that backing as the old map.
			found->Shadows = {};
			core::Metrics::Count("render.weather.shadow_record_refusals", 1);
			return true;
		}
		found->Shadows.Stage(plan, target.Texture, Command);
		core::Metrics::Count("render.weather.shadow_rebuilds", 1);
		++Result.ComputeDispatches;
		StageHistoryWrites(context, Command);
		return true;
	}
}
