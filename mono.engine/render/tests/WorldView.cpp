#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/LightingEffects.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

TEST_SUITE_ID("engine.render.worldview")
TEST_DEPENDS("engine.render.worldpresentation")
TEST_DEPENDS("engine.render.spatialcanvas")
TEST_DEPENDS("engine.scene.lighting-effects")
TEST_DEPENDS("engine.weather.statefulruntime")

using namespace engine;

namespace {
	double WeatherCheckpointCopyBytes() {
		for (const auto &counter : core::Metrics::Snapshot().Counters)
			if (counter.Name == core::Name("render.weather.checkpoint_collect_bytes")) return counter.Value;
		return 0.0;
	}

	ecs::Entity AddStatefulWeather(ecs::Store &store, const weather::StatefulWeatherCheckpoint &checkpoint) {
		scene::InstallServices(store);
		const auto lighting = scene::ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
		const auto provider =
			store.CreateInstance(ecs::Classes::Find(core::Name("WeatherCompute")), "Weather");
		REQUIRE(store.SetParent(provider, lighting));
		auto controls = *store.Get<scene::WeatherCompute>(provider);
		controls.Mode = scene::WeatherMode::Stateful;
		controls.DefinitionRevision = checkpoint.Revision;
		store.Set(provider, controls);
		scene::WeatherClock clock;
		clock.Epoch = checkpoint.Epoch;
		store.Set(provider, clock);
		store.Set(provider, checkpoint);
		return provider;
	}

	double GpuParticleSampleCopyBytes() {
		for (const auto &counter : core::Metrics::Snapshot().Counters)
			if (counter.Name == core::Name("render.gpu_particle_field.sample_collect_bytes"))
				return counter.Value;
		return 0.0;
	}

	void RegisterViewClasses() {
		scene::RegisterSceneClasses();
		effects::RegisterEffectClasses();
		gui::RegisterGuiClasses();
		render::RegisterPresentationComponents();
		weather::RegisterWeatherRuntimeComponents();
		weather::RegisterStatefulWeatherComponents();
	}
	ecs::Entity PartAt(ecs::Store &store, core::Vector3 position) {
		scene::PartDesc part;
		part.Frame.Position = position;
		return scene::MakePart(store, part);
	}
}

TEST_CASE(
	"lighting effect edits refresh owned presentation data in the same frame",
	"[render][world-view][lighting-effects]"
) {
	RegisterViewClasses();
	ecs::Store store("lighting-effects");
	const auto workspace = scene::InstallServices(store);
	const auto lighting = scene::ServiceOf(store, ecs::Classes::Find(core::Name("Lighting")));
	render::WorldViewFrame frame;
	render::View view;
	view.OverrideLighting = true;
	view.Grid.Enabled = true;
	const auto collect = [&] {
		render::CollectWorldView(store, core::Name("lighting-effects"), frame);
		view.Lighting = frame.Lighting;
		return render::ScenePresentationSignaturesOf(view, {}).Objects;
	};
	const auto exercise =
		[&]<typename Effect>(
			const char *className, const char *property, float editedValue, Effect active, auto readValue
		) {
			CAPTURE(className);
			const auto baseline = collect();
			const auto child = store.CreateInstance(ecs::Classes::Find(core::Name(className)), "Effect");
			store.Set(child, active);
			CHECK(collect() == baseline);
			REQUIRE(store.SetParent(child, lighting));
			auto enabled = collect();
			CHECK(enabled != baseline);
			CHECK(readValue(frame.Lighting) == readValue(scene::LightingOf(store)));
			const auto retained = frame.Lighting;
			REQUIRE(store.SetProperty(child, core::Name(property), &editedValue, sizeof(editedValue)));
			CHECK(readValue(frame.Lighting) == readValue(retained));
			const auto edited = collect();
			CHECK(edited != enabled);
			CHECK(readValue(frame.Lighting) == editedValue);
			enabled = edited;
			const auto editedLighting = frame.Lighting;
			const bool disabled = false;
			REQUIRE(store.SetProperty(child, core::Name("Enabled"), &disabled, sizeof(disabled)));
			CHECK(readValue(frame.Lighting) == readValue(editedLighting));
			CHECK(collect() == baseline);
			const bool enable = true;
			REQUIRE(store.SetProperty(child, core::Name("Enabled"), &enable, sizeof(enable)));
			CHECK(collect() == enabled);
			REQUIRE(store.SetParent(child, workspace));
			CHECK(collect() == baseline);
			REQUIRE(store.SetParent(child, lighting));
			CHECK(collect() == enabled);
			store.DestroyInstance(child);
			CHECK(collect() == baseline);
		};
	exercise("BloomEffect", "Intensity", 0.75f, scene::BloomEffect{.Intensity = 0.5f}, [](const auto &state) {
		return state.BloomIntensity;
	});
	exercise(
		"DepthOfFieldEffect",
		"Intensity",
		0.75f,
		scene::DepthOfFieldEffect{.Intensity = 0.5f},
		[](const auto &state) { return state.DepthOfFieldIntensity; }
	);
	exercise(
		"GodRaysEffect", "Intensity", 0.75f, scene::GodRaysEffect{.Intensity = 0.5f}, [](const auto &state) {
			return state.GodRayIntensity;
		}
	);
	exercise("FogEffect", "End", 50.0f, scene::FogEffect{.Start = 5.0f, .End = 40.0f}, [](const auto &state) {
		return state.FogEnd;
	});
}

TEST_CASE(
	"generic GPU fields collect owned samples and nearest vector fields", "[render][world-view][particles]"
) {
	RegisterViewClasses();
	ecs::Store store("generic-field");
	const auto vector = store.CreateInstance(ecs::Classes::Find(core::Name("VectorField3D")), "Wind");
	scene::VectorField3D wind;
	wind.Vector = {3, 4, 5};
	store.Set(vector, wind);
	const auto field = store.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Particles");
	REQUIRE(store.SetParent(field, vector));
	scene::GpuParticleField authored;
	authored.SpawnSamples.push_back({{1, 2, 3}, 10, {4, 5, 6}, 1});
	store.Set(field, authored);
	store.Set(field, scene::Transform{core::CFrame{core::Vector3{7, 8, 9}}});
	render::WorldViewFrame frame;
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	REQUIRE(frame.GpuParticles);
	CHECK(frame.GpuParticles->Frame.Position == core::Vector3{7, 8, 9});
	CHECK(frame.GpuParticles->ForceField.Source == vector);
	CHECK(frame.GpuParticles->ForceField.Vector == core::Vector3{3, 4, 5});
	CHECK(frame.GpuParticles->Field.SpawnSamples == authored.SpawnSamples);
	CHECK(frame.GpuParticles->Source == field);
	REQUIRE(frame.GpuParticles->DefinitionRevision != 0);
	render::View view;
	view.GpuParticles = frame.GpuParticles;
	const auto signature = [&] { return render::ScenePresentationSignaturesOf(view, {}).Particles; };
	const auto original = signature();
	const double unchangedBytes = GpuParticleSampleCopyBytes();
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() == original);
	CHECK(GpuParticleSampleCopyBytes() == unchangedBytes);
	authored.SpawnSamples[0].Position.X = 99;
	store.Set(field, authored);
	CHECK(frame.GpuParticles->Field.SpawnSamples[0].Position.X == 1);
	const double beforeSampleEdit = GpuParticleSampleCopyBytes();
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	CHECK(
		GpuParticleSampleCopyBytes() ==
		beforeSampleEdit + authored.SpawnSamples.size() * sizeof(scene::GpuParticleSpawnSample)
	);
	CHECK(frame.GpuParticles->Field.SpawnSamples[0].Position.X == 99);
	CHECK(view.GpuParticles->Field.SpawnSamples[0].Position.X == 1);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() != original);
	const auto editedSamples = signature();
	auto style = authored.Styles[0];
	style.Size = 5;
	REQUIRE(scene::SetGpuParticleStyle(store, field, 0, style));
	const double beforeStyleEdit = GpuParticleSampleCopyBytes();
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	CHECK(
		GpuParticleSampleCopyBytes() ==
		beforeStyleEdit + authored.SpawnSamples.size() * sizeof(scene::GpuParticleSpawnSample)
	);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() != editedSamples);
	const auto editedStyle = signature();
	wind.Tangential = 50;
	store.Set(vector, wind);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() != editedStyle);
	const auto editedFlow = signature();
	store.AdvanceTick(1.0f / 60.0f);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() != editedFlow);
	const auto advanced = signature();
	store.SetFrame(1.0f / 60.0f, 1);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	view.GpuParticles = frame.GpuParticles;
	CHECK(signature() == advanced);

	auto currentField = *store.Get<scene::GpuParticleField>(field);
	currentField.Enabled = false;
	store.Set(field, currentField);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	CHECK_FALSE(frame.GpuParticles);
	REQUIRE(view.GpuParticles);
	CHECK(view.GpuParticles->Field.Enabled);
	CHECK(view.GpuParticles->Field.SpawnSamples[0].Position.X == 99);

	currentField.Enabled = true;
	store.Set(field, currentField);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	REQUIRE(frame.GpuParticles);
	CHECK(frame.GpuParticles->Field.SpawnSamples == currentField.SpawnSamples);
	CHECK(frame.GpuParticles->Field.Styles[0].Size == 5);
	CHECK(frame.GpuParticles->ForceField.Tangential == 50);

	currentField.Enabled = false;
	store.Set(field, currentField);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	CHECK_FALSE(frame.GpuParticles);
	const auto replacement =
		store.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Replacement");
	scene::GpuParticleField replacementField;
	replacementField.SpawnSamples.push_back({{8, 9, 10}, 12, {1, 1, 1}, 1});
	store.Set(replacement, replacementField);
	store.Set(replacement, scene::Transform{core::CFrame{core::Vector3{2, 3, 4}}});
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	REQUIRE(frame.GpuParticles);
	CHECK(frame.GpuParticles->Source == replacement);
	CHECK(frame.GpuParticles->Field.SpawnSamples == replacementField.SpawnSamples);
	store.Destroy(replacement);
	render::CollectWorldView(store, core::Name("generic-field"), frame);
	CHECK_FALSE(frame.GpuParticles);
}

TEST_CASE(
	"generic GPU field samples refresh when the source store changes", "[render][world-view][particles]"
) {
	RegisterViewClasses();
	ecs::Store first("generic-field-first");
	const auto firstField =
		first.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Particles");
	scene::GpuParticleField firstDefinition;
	firstDefinition.SpawnSamples.push_back({{1, 2, 3}, 10, {4, 5, 6}, 1});
	first.Set(firstField, firstDefinition);
	first.Set(firstField, scene::Transform{});

	render::WorldViewFrame frame;
	render::CollectWorldView(first, core::Name("same-owner"), frame);
	REQUIRE(frame.GpuParticles);
	const ecs::Entity firstSource = frame.GpuParticles->Source;

	ecs::Store second("generic-field-second");
	const auto secondField =
		second.CreateInstance(ecs::Classes::Find(core::Name("GpuParticleField")), "Particles");
	scene::GpuParticleField secondDefinition;
	secondDefinition.SpawnSamples.push_back({{9, 8, 7}, 11, {6, 5, 4}, 1});
	second.Set(secondField, secondDefinition);
	second.Set(secondField, scene::Transform{});
	CHECK(secondField == firstSource);
	const double beforeWorldSwitch = GpuParticleSampleCopyBytes();
	render::CollectWorldView(second, core::Name("same-owner"), frame);
	REQUIRE(frame.GpuParticles);
	CHECK(frame.GpuParticles->Source == secondField);
	CHECK(frame.GpuParticles->Field.SpawnSamples == secondDefinition.SpawnSamples);
	CHECK(
		GpuParticleSampleCopyBytes() ==
		beforeWorldSwitch + secondDefinition.SpawnSamples.size() * sizeof(scene::GpuParticleSpawnSample)
	);
}

TEST_CASE(
	"current-tick draw collection ignores a stale presentation alpha", "[render][world-view][data-capture]"
) {
	RegisterViewClasses();
	ecs::Store store("current-tick-draw");
	store.SetResource(render::DrawList{});
	const ecs::Entity workspace = scene::InstallServices(store);
	const ecs::Entity part = PartAt(store, {10, 0, 0});
	REQUIRE(store.SetParent(part, workspace));
	REQUIRE(scene::SyncRendered(store) == 1);
	render::CollectInstances(store);
	const auto *previous = store.Get<scene::PreviousTransform>(part);
	REQUIRE(previous != nullptr);
	auto currentTickPrevious = *previous;
	currentTickPrevious.Frame.Position = {0, 0, 0};
	store.Set(part, currentTickPrevious);
	store.SetFrame(0.25f, 0.25f);
	const ecs::WorldTime before = store.Time();
	core::ByteWriter snapshotBefore;
	REQUIRE(store.Save(snapshotBefore));

	render::CollectInstances(store);
	const auto *interpolated = store.Resource<render::DrawList>();
	REQUIRE(interpolated != nullptr);
	REQUIRE(interpolated->Instances.size() == 1);
	CHECK(interpolated->Instances.front().Frame.Position.X == 2.5f);

	render::CollectInstances(store, render::DrawCollectionTime::CurrentTick);
	const auto *current = store.Resource<render::DrawList>();
	REQUIRE(current != nullptr);
	REQUIRE(current->Instances.size() == 1);
	CHECK(current->Instances.front().Frame.Position.X == 10.0f);
	const ecs::WorldTime after = store.Time();
	CHECK(after.Tick == before.Tick);
	CHECK(after.Elapsed == before.Elapsed);
	CHECK(after.Alpha == before.Alpha);
	core::ByteWriter snapshotAfter;
	REQUIRE(store.Save(snapshotAfter));
	CHECK(snapshotAfter.Bytes().size() == snapshotBefore.Bytes().size());
	CHECK(
		std::equal(
			snapshotAfter.Bytes().begin(),
			snapshotAfter.Bytes().end(),
			snapshotBefore.Bytes().begin(),
			snapshotBefore.Bytes().end()
		)
	);
}

TEST_CASE(
	"foreign world interface keeps spatial and player screen ownership separate", "[render][world-view]"
) {
	struct Hook : render::FrameOverlayHook {
		bool Ready = true;
		bool Layers = true;
		bool Overlay = false;
		const render::WorldInterfaceCapture *LastCapture = nullptr;
		size_t LastBatch = 0;
		uint32_t Preparations = 0, WorldDraws = 0, ScreenDraws = 0;
		bool Prepare(void *) override {
			++Preparations;
			return Ready;
		}
		bool AffectsScene() const override {
			return true;
		}
		bool HasWorldOverlay() const override {
			return Overlay;
		}
		bool SupportsWorldLayers() const override {
			return Layers;
		}
		size_t WorldBatchCount() const override {
			return 3;
		}
		uint32_t RecordWorldBatch(const render::WorldInterfaceCapture &capture, size_t batch) override {
			LastCapture = &capture;
			LastBatch = batch;
			return 2;
		}
		uint32_t RecordWorld(
			void *,
			void *,
			const glm::mat4 &,
			const core::CFrame &,
			const core::Color3 &,
			const core::Vector3 &,
			uint32_t,
			uint32_t,
			bool,
			render::WorldColourTarget
		) override {
			++WorldDraws;
			return 7;
		}
		void Record(void *, void *) override {
			++ScreenDraws;
		}
	};
	Hook spatial, screen;
	render::WorldViewInterface composed(&spatial, &screen);
	const auto recordWorld = [](render::WorldViewInterface &hook) {
		return hook.RecordWorld(
			nullptr, nullptr, glm::mat4(1), {}, {}, {}, 10, 10, true, render::WorldColourTarget::Display
		);
	};
	CHECK(composed.AffectsScene());
	const render::WorldInterfaceCapture capture;
	CHECK(composed.WorldBatchCount() == 0);
	CHECK(composed.RecordWorldBatch(capture, 1) == 0);
	REQUIRE(composed.Prepare(nullptr));
	CHECK(composed.SupportsWorldLayers());
	CHECK_FALSE(composed.HasWorldOverlay());
	spatial.Overlay = true;
	CHECK(composed.HasWorldOverlay());
	spatial.Overlay = false;
	CHECK(composed.WorldBatchCount() == 3);
	CHECK(composed.RecordWorldBatch(capture, 1) == 2);
	CHECK(spatial.LastCapture == &capture);
	CHECK(spatial.LastBatch == 1);
	CHECK(screen.LastCapture == nullptr);
	spatial.Layers = false;
	CHECK_FALSE(composed.SupportsWorldLayers());
	spatial.Layers = true;
	CHECK(recordWorld(composed) == 7);
	composed.Record(nullptr, nullptr);
	CHECK(spatial.WorldDraws == 1);
	CHECK(spatial.ScreenDraws == 0);
	CHECK(screen.WorldDraws == 0);
	CHECK(screen.ScreenDraws == 1);
	spatial.Ready = false;
	REQUIRE(composed.Prepare(nullptr));
	CHECK(composed.WorldBatchCount() == 0);
	CHECK(composed.RecordWorldBatch(capture, 2) == 0);
	CHECK(spatial.LastBatch == 1);
	CHECK(recordWorld(composed) == 0);
	composed.Record(nullptr, nullptr);
	CHECK(screen.ScreenDraws == 2);
	screen.Ready = false;
	CHECK_FALSE(composed.Prepare(nullptr));
	CHECK(recordWorld(composed) == 0);
	composed.Record(nullptr, nullptr);
	CHECK(screen.ScreenDraws == 2);
	CHECK(spatial.Preparations == 3);
	CHECK(screen.Preparations == 3);

	spatial.Ready = true;
	render::WorldViewInterface shared(&spatial, &spatial);
	REQUIRE(shared.Prepare(nullptr));
	CHECK(shared.SupportsWorldLayers());
	CHECK(shared.WorldBatchCount() == 3);
	CHECK(shared.RecordWorldBatch(capture, 2) == 2);
	CHECK(spatial.LastBatch == 2);
	CHECK(spatial.Preparations == 4);
	CHECK(recordWorld(shared) == 7);
	shared.Record(nullptr, nullptr);
	CHECK(spatial.ScreenDraws == 1);
	render::WorldViewInterface empty(nullptr, nullptr);
	CHECK_FALSE(empty.Prepare(nullptr));
	CHECK_FALSE(empty.AffectsScene());
	CHECK(empty.SupportsWorldLayers());
	CHECK(empty.WorldBatchCount() == 0);
	CHECK(empty.RecordWorldBatch(capture, 0) == 0);
	CHECK(recordWorld(empty) == 0);
	empty.Record(nullptr, nullptr);
}

TEST_CASE("world view owns a published replica pose and particle inputs", "[render][world-view]") {
	RegisterViewClasses();
	render::WorldViewFrame frame;
	{
		ecs::Store store("observed-source");
		const auto workspace = scene::InstallServices(store);
		const auto part = PartAt(store, {100, 0, 0});
		REQUIRE(store.SetParent(part, workspace));
		render::DrawList draw;
		draw.Instances.emplace_back();
		draw.Instances.back().Source = part.Id;
		draw.Instances.back().Frame.Position = {1, 2, 3};
		draw.JointFrames.emplace_back(core::Vector3(4, 5, 6));
		store.SetResource(std::move(draw));
		store.SetResource(scene::Sun{{0, 0, 1}, {.2f, .3f, .4f}});
		const auto emitter =
			store.CreateInstance(ecs::Classes::Find(core::Name("ParticleEmitter")), "ObservedEmitter");
		REQUIRE(store.SetParent(emitter, part));
		auto emission = *store.Get<effects::ParticleEmitter>(emitter);
		emission.Rate = 0;
		emission.Lifetime = core::NumberRange(10, 10);
		store.Set(emitter, emission);
		effects::InstallParticles(store, 128);
		store.ResourceMutable<effects::ParticleSystem>()->DeviceStepped = true;
		REQUIRE(effects::EmitParticles(store, emitter, 1));
		REQUIRE(effects::RefreshEmitters(store) == 1);
		store.AdvanceTick(.1);
		render::CollectWorldView(store, core::Name("source"), frame);
		REQUIRE(frame.Particles.Batches.size() == 1);
		CHECK(frame.Particles.Batches[0].SourceWorld == core::Name(store.Name()));
		CHECK(frame.Particles.Detached);
		CHECK(frame.Particles.Batches[0].Block == &frame.Particles.Blocks[0]);
		CHECK(frame.Particles.Batches[0].Block != &store.Resource<effects::ParticleSystem>()->Blocks[0]);
		CHECK(frame.Identity == store.Identity());
		CHECK(frame.Tick == store.Time().Tick);
		CHECK(frame.Seconds == store.Time().Elapsed);
		CHECK(frame.ParticleDelta == store.Time().Delta);
		store.AdvanceTick(.1f);
		render::CollectWorldView(store, core::Name("source"), frame);
		CHECK(frame.ParticleDelta == store.Time().Delta);
		render::CollectWorldView(store, core::Name("source"), frame);
		CHECK(frame.ParticleDelta == 0.0f);
	}
	// Store destruction must not invalidate the render packet.
	REQUIRE(frame.Instances.size() == 1);
	CHECK(frame.Instances[0].Frame.Position == core::Vector3(1, 2, 3));
	REQUIRE(frame.Joints.size() == 1);
	CHECK(frame.Joints[0].Position == core::Vector3(4, 5, 6));
	CHECK(frame.Lighting.Ambient == core::Color3(.2f, .3f, .4f));
	CHECK(frame.Particles.Batches[0].Block == frame.Particles.Blocks.data());
	CHECK(frame.Name == core::Name("source"));
	{
		ecs::Store replacement("observed-source");
		scene::InstallServices(replacement);
		replacement.AdvanceTick(.1f);
		replacement.AdvanceTick(.1f);
		REQUIRE(replacement.Time().Tick == frame.Tick);
		render::CollectWorldView(replacement, core::Name("source"), frame);
		CHECK(frame.ParticleDelta == replacement.Time().Delta);
	}

	ecs::Store empty("empty-destination");
	scene::InstallServices(empty);
	render::CollectWorldView(empty, core::Name("destination"), frame);
	CHECK(frame.Name == core::Name("destination"));
	CHECK(frame.Instances.empty());
	CHECK(frame.Joints.empty());
	CHECK(frame.Particles.Batches.empty());
	CHECK(frame.Slots.empty());
	CHECK(frame.Seams.empty());
	CHECK(frame.Portals.empty());
}

TEST_CASE(
	"binding an owned world packet preserves its viewing camera and body selection", "[render][world-view]"
) {
	ecs::Store ownerStore("packet-owner"), replacementStore("packet-owner");
	render::WorldViewFrame frame;
	frame.Name = core::Name("packet-owner");
	frame.Identity = ownerStore.Identity();
	frame.Seconds = 3.5;
	frame.ParticleDelta = 1.0f / 30.0f;
	frame.Instances.emplace_back();
	frame.Instances.front().Frame.Position = {1, 2, -3};
	frame.Joints.emplace_back(core::Vector3{4, 5, 6});
	frame.Lighting.Ambient = {.2f, .3f, .4f};
	render::WorldCameraFrame camera;
	camera.Lights.emplace_back();
	const std::array contentOwners{
		render::WorldContentOwner{core::Name("foreign-room"), core::Name("foreign-assets")}
	};
	render::WorldViewBinding binding{
		.World = 7,
		.Name = frame.Name,
		.Identity = ownerStore.Identity(),
		.ContentOwner = core::Name("packet-assets"),
		.ForeignContentOwners = contentOwners,
		.Pipeline = core::Name("native-packet")
	};
	render::SceneTarget target{19, 11};
	const std::array<uint32_t, 1> hiddenRows{0};
	render::View view;
	view.CameraFrame = core::CFrame(core::Vector3{8, 9, 10}) * core::CFrame::Angles(.1f, .2f, .3f);
	view.Camera.FieldOfViewRadians = .9f;
	view.Projection = glm::mat4(2);
	view.Target = &target;
	view.Slot = 3;
	view.EyeRig = 83;
	view.EyePlayer = 17;
	view.EyeHiddenRows = hiddenRows;
	view.EyeImage = 41;
	view.EyeTransparentImages = {42, 43};
	view.EyeSpatialOverlayImage = 44;
	view.EyeImageKey = core::Name("old-eye");
	view.LensPrograms = 45;
	view.LensContentOwner = core::Name("old-lenses");
	view.LensTimeSeconds = 99;
	view.World = 13;
	view.WorldName = core::Name("old-world");
	view.Pipeline = core::Name("old-pipeline");
	view.Damage.Scene = false;
	const auto before = view;
	for (int refusal = 0; refusal < 4; ++refusal) {
		CAPTURE(refusal);
		auto invalid = binding;
		if (refusal == 0) invalid.Name = {};
		if (refusal == 1) invalid.Name = core::Name("another-owner");
		if (refusal == 2) invalid.Identity = 0;
		if (refusal == 3) invalid.Identity = replacementStore.Identity();
		CHECK_FALSE(render::BindWorldView(frame, camera, invalid, view));
		CHECK(view.World == before.World);
		CHECK(view.WorldName == before.WorldName);
		CHECK(view.Pipeline == before.Pipeline);
		CHECK(view.Instances.data() == before.Instances.data());
		CHECK(view.EyeImage == before.EyeImage);
		CHECK(view.LensPrograms == before.LensPrograms);
		CHECK(view.LensTimeSeconds == before.LensTimeSeconds);
	}
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	CHECK(view.World == binding.World);
	CHECK(view.WorldName == frame.Name);
	CHECK(view.ContentOwner == binding.ContentOwner);
	CHECK(view.ContentOwnerOf(core::Name("foreign-room")) == core::Name("foreign-assets"));
	CHECK(view.Pipeline == binding.Pipeline);
	CHECK(view.Instances.data() == frame.Instances.data());
	CHECK(view.JointFrames.data() == frame.Joints.data());
	CHECK(view.Lights.data() == camera.Lights.data());
	CHECK(view.OverrideLighting);
	CHECK(view.Lighting.Ambient == frame.Lighting.Ambient);
	CHECK(view.ParticleDelta == frame.ParticleDelta);
	CHECK(view.CameraFrame.Position == before.CameraFrame.Position);
	CHECK(view.CameraFrame.Rotation() == before.CameraFrame.Rotation());
	CHECK(view.Camera.FieldOfViewRadians == before.Camera.FieldOfViewRadians);
	CHECK(view.Projection == before.Projection);
	CHECK(view.Target == before.Target);
	CHECK(view.Slot == before.Slot);
	CHECK(view.EyeRig == before.EyeRig);
	CHECK(view.EyePlayer == before.EyePlayer);
	CHECK(view.EyeHiddenRows.data() == hiddenRows.data());
	CHECK_FALSE(view.Damage.Scene);
	CHECK(view.EyeImage == 0);
	CHECK(view.EyeTransparentImages == std::array<uint64_t, 2>{});
	CHECK(view.EyeSpatialOverlayImage == 0);
	CHECK_FALSE(view.EyeImageKey.IsValid());
	CHECK(view.LensPrograms == 0);
	CHECK_FALSE(view.LensContentOwner);
	REQUIRE(view.LensTimeSeconds);
	CHECK(*view.LensTimeSeconds == 3.5f);
}

TEST_CASE(
	"world camera layers follow explicit eyes without replacing the active camera", "[render][world-view]"
) {
	RegisterViewClasses();
	ecs::Store store("camera-layers");
	const auto workspace = scene::InstallServices(store);
	const auto active = store.CreateInstance(scene::CameraClass(), "UnrelatedEye");
	const core::CFrame activeFrame(core::Vector3(100, 100, 100));
	store.Set(active, scene::Transform{activeFrame});
	store.SetResource(scene::ActiveCamera{active});
	const auto part = PartAt(store, {0, 0, -10});
	REQUIRE(store.SetParent(part, workspace));
	const auto billboard = store.CreateInstance(gui::GuiClass("BillboardGui"), "WorldLabel");
	REQUIRE(store.SetParent(billboard, part));
	gui::Billboard label;
	label.Size = {2, 0, 2, 0};
	store.Set(billboard, label);
	const auto screen = store.CreateInstance(gui::GuiClass("ScreenGui"), "PlayerScreen");
	for (const auto parent : {billboard, screen}) {
		const auto rectangle = store.CreateInstance(gui::GuiClass("Frame"), "Panel");
		REQUIRE(store.SetParent(rectangle, parent));
		gui::Element element;
		element.Size = {1, 0, 1, 0};
		store.Set(rectangle, element);
		store.Set(rectangle, gui::Background{});
	}
	std::vector<ecs::Entity> pointLights, spotLights;
	pointLights.reserve(256);
	spotLights.reserve(256);
	for (size_t index = 0; index < 256; ++index) {
		const auto anchor = PartAt(store, {float(index * 20), 0, -10});
		const auto point = store.CreateInstance(ecs::Classes::Find(core::Name("PointLight")), "PointLight");
		REQUIRE(store.SetParent(point, anchor));
		pointLights.push_back(point);
		const auto spot = store.CreateInstance(ecs::Classes::Find(core::Name("SpotLight")), "SpotLight");
		REQUIRE(store.SetParent(spot, anchor));
		spotLights.push_back(spot);
	}
	for (uint32_t index = 0; index < 256; index++) {
		const auto volume = store.Create();
		store.Set(volume, scene::Volume{.Seed = index});
		store.Set(volume, scene::Transform{.Frame = core::CFrame{{0.0f, 0.0f, -10.0f - float(index)}}});
	}
	std::array<ecs::Entity, 2> ends;
	for (size_t index = 0; index < ends.size(); ++index) {
		ends[index] = store.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "BeamEnd");
		REQUIRE(store.SetParent(ends[index], part));
		scene::Attachment attachment;
		attachment.Frame.Position = {index == 0 ? -2.0f : 2.0f, 0, 0};
		store.Set(ends[index], attachment);
	}
	scene::ResolveAttachments(store);
	const auto beamEntity = store.CreateInstance(ecs::Classes::Find(core::Name("Beam")), "Beam");
	effects::Beam beam;
	beam.Attachment0 = ends[0];
	beam.Attachment1 = ends[1];
	beam.FaceCamera = true;
	beam.Width0 = beam.Width1 = 1;
	store.Set(beamEntity, beam);
	render::View eye;
	render::WorldCameraFrame first, locked, unlocked;
	render::CollectWorldCamera(store, eye, {200, 200}, first);
	REQUIRE(first.Lights.size() == render::MAX_SCENE_LIGHTS);
	REQUIRE(first.VolumeCount == scene::MAX_SCENE_VOLUMES);
	CHECK(first.Lights[0].Position.X == 0);
	REQUIRE_FALSE(first.Ribbons.Vertices.empty());
	REQUIRE_FALSE(first.SpatialCommands.Commands.empty());
	CHECK(
		std::all_of(
			first.SpatialCommands.Commands.begin(),
			first.SpatialCommands.Commands.end(),
			[&](const auto &command) { return command.Collector == billboard; }
		)
	);
	const float nearWidth = store.Get<gui::SpatialCanvas>(billboard)->Size.X;
	const auto firstVertex = first.Ribbons.Vertices[0].Position;
	const float farLight = 240.0f;
	eye.VisibilityFrame = eye.CameraFrame;
	eye.CameraFrame.Position = {farLight, 10, 10};
	render::CollectWorldCamera(store, eye, {200, 200}, locked);
	CHECK(eye.CameraFrame.Position == core::Vector3{farLight, 10, 10});
	REQUIRE(locked.Lights.size() == render::MAX_SCENE_LIGHTS);
	REQUIRE(locked.VolumeCount == scene::MAX_SCENE_VOLUMES);
	CHECK(locked.Lights[0].Position.X == 0);
	CHECK(store.Get<gui::SpatialCanvas>(billboard)->Size.X == nearWidth);
	REQUIRE_FALSE(locked.Ribbons.Vertices.empty());
	CHECK(locked.Ribbons.Vertices[0].Position == firstVertex);

	eye.VisibilityFrame.reset();
	render::CollectWorldCamera(store, eye, {200, 200}, unlocked);
	REQUIRE(unlocked.Lights.size() == render::MAX_SCENE_LIGHTS);
	REQUIRE(unlocked.VolumeCount == scene::MAX_SCENE_VOLUMES);
	CHECK(unlocked.Lights[0].Position.X == farLight);
	CHECK(first.Lights[0].Position.X == 0);
	CHECK(store.Get<gui::SpatialCanvas>(billboard)->Size.X < nearWidth);
	REQUIRE(first.SpatialCollectors.size() == 1);
	CHECK(first.SpatialCollectors[0].Collector == billboard);
	CHECK(first.SpatialCollectors[0].Canvas.Size.X == nearWidth);
	REQUIRE(unlocked.SpatialCollectors.size() == 1);
	CHECK(unlocked.SpatialCollectors[0].Canvas.Size.X < nearWidth);
	REQUIRE_FALSE(unlocked.Ribbons.Vertices.empty());
	CHECK(unlocked.Ribbons.Vertices[0].Position != firstVertex);
	CHECK(first.Ribbons.Vertices[0].Position == firstVertex);
	CHECK(store.Resource<scene::ActiveCamera>()->Entity == active);
	CHECK(store.Get<scene::Transform>(active)->Frame.Position == activeFrame.Position);
	CHECK(store.Get<scene::Transform>(active)->Frame.Rotation() == activeFrame.Rotation());

	const char *configuredFrames = std::getenv("MONO_LIGHT_SELECTION_STRESS_FRAMES");
	if (configuredFrames == nullptr) return;
	const unsigned long requested = std::strtoul(configuredFrames, nullptr, 10);
	REQUIRE(requested > 0);
	const uint32_t frames = static_cast<uint32_t>(std::min(requested, 10'000ul));
	std::array<scene::VolumeState, scene::MAX_SCENE_VOLUMES> volumes;
	std::vector<render::SceneLight> selectedLights;
	const auto setEnabled = [&](const std::vector<ecs::Entity> &lights, bool enabled) {
		for (const ecs::Entity entity : lights) {
			auto light = *store.Get<scene::Light>(entity);
			light.Enabled = enabled;
			store.Set(entity, light);
		}
	};
	const auto measureLights = [&](const char *label) {
		const auto started = std::chrono::steady_clock::now();
		for (uint32_t frame = 0; frame < frames; frame++) {
			const core::Vector3 selectionEye = frame & 1u ? core::Vector3{farLight, 10, 10} : core::Vector3{};
			render::CollectLights(store, selectionEye, {}, selectedLights);
		}
		const double milliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::cout << label << " frames=" << frames << ", cpu ms/frame=" << milliseconds / double(frames)
				  << '\n';
	};
	const auto measureVolumes = [&] {
		const auto started = std::chrono::steady_clock::now();
		for (uint32_t frame = 0; frame < frames; frame++) {
			const core::Vector3 selectionEye = frame & 1u ? core::Vector3{farLight, 10, 10} : core::Vector3{};
			scene::ResolveVolumes(store, selectionEye, {}, volumes);
		}
		const double milliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::cout << "fog volume selection frames=" << frames
				  << ", cpu ms/frame=" << milliseconds / double(frames) << '\n';
	};
	setEnabled(pointLights, true);
	setEnabled(spotLights, false);
	measureLights("point light selection");
	setEnabled(pointLights, false);
	setEnabled(spotLights, true);
	measureLights("spot light selection");
	setEnabled(pointLights, true);
	measureLights("mixed local light selection");
	measureVolumes();
}

TEST_CASE("frozen camera GUI collection preserves snapshot bytes", "[render][world-view]") {
	RegisterViewClasses();
	ecs::Store store("frozen-camera-gui");
	const auto workspace = scene::InstallServices(store);
	const auto part = PartAt(store, {0, 0, -10});
	REQUIRE(store.SetParent(part, workspace));
	const auto billboard = store.CreateInstance(gui::GuiClass("BillboardGui"), "WorldLabel");
	REQUIRE(store.SetParent(billboard, part));
	gui::Billboard label;
	label.Size = {2, 0, 2, 0};
	store.Set(billboard, label);
	const auto panel = store.CreateInstance(gui::GuiClass("Frame"), "Panel");
	REQUIRE(store.SetParent(panel, billboard));
	gui::Element element;
	element.Size = {1, 0, 1, 0};
	store.Set(panel, element);
	store.Set(panel, gui::Background{});
	core::ByteWriter before;
	REQUIRE(store.Save(before));
	render::View eye;
	render::WorldCameraFrame captured;
	render::CollectWorldCamera(store, eye, {200, 200}, captured, true);
	core::ByteWriter after;
	REQUIRE(store.Save(after));
	CHECK(before.Bytes().size() == after.Bytes().size());
	CHECK(std::equal(before.Bytes().begin(), before.Bytes().end(), after.Bytes().begin()));
	REQUIRE_FALSE(captured.SpatialCommands.Commands.empty());
	CHECK(captured.SpatialCollectors.size() == 1);
}

TEST_CASE(
	"light collection keeps an offscreen light that reaches visible receivers", "[render][world-view]"
) {
	RegisterViewClasses();
	ecs::Store store("light-influence-order");
	const auto workspace = scene::InstallServices(store);

	// These sixteen lights fill the device budget, but none reaches the visible
	// receiver below.
	for (size_t index = 0; index < render::MAX_SCENE_LIGHTS; index++) {
		const auto anchor = PartAt(store, {float(index), 0.0f, 0.0f});
		REQUIRE(store.SetParent(anchor, workspace));
		const auto bulb = store.CreateInstance(ecs::Classes::Find(core::Name("PointLight")), "distractor");
		REQUIRE(store.SetParent(bulb, anchor));
		auto distractor = *store.Get<scene::Light>(bulb);
		distractor.Range = 0.01f;
		store.Set(bulb, distractor);
	}

	// This is the shape of a light copied through a portal: its source sits well
	// outside the view while its radius still reaches a floor tile in the view.
	const auto carriedAnchor = PartAt(store, {50.0f, 0.0f, -100.0f});
	REQUIRE(store.SetParent(carriedAnchor, workspace));
	const auto carried = store.CreateInstance(ecs::Classes::Find(core::Name("PointLight")), "carried");
	REQUIRE(store.SetParent(carried, carriedAnchor));
	auto light = *store.Get<scene::Light>(carried);
	light.Range = 50.0f;
	store.Set(carried, light);

	std::vector<render::SceneLight> lights;
	// The giant floor intersects every light sphere. Centre distance must break
	// that zero-distance tie deterministically in favour of the carried light.
	const std::array receivers{
		core::AABB::FromCentre({0.0f, 0.0f, -100.0f}, {1.0f, 1.0f, 1.0f}),
		core::AABB::FromCentre({0.0f, 0.0f, -100.0f}, {100.0f, 1.0f, 100.0f}),
	};
	REQUIRE(render::CollectLights(store, {}, receivers, lights) == render::MAX_SCENE_LIGHTS);
	CHECK(lights.front().Position.X == 50.0f);
	CHECK(lights.front().Position.Z == -100.0f);
	CHECK(std::any_of(lights.begin(), lights.end(), [](const render::SceneLight &candidate) {
		return candidate.Position.X == 50.0f && candidate.Position.Z == -100.0f && candidate.Range == 50.0f;
	}));
}

TEST_CASE(
	"stateful weather packets retain owned checkpoints until their publication changes",
	"[render][world-view][weather]"
) {
	RegisterViewClasses();
	ecs::Store store("checkpoint-packet");
	const weather::StatefulWeatherCheckpoint original{10, 1, 1, {std::byte{1}, std::byte{2}, std::byte{3}}};
	const auto provider = AddStatefulWeather(store, original);
	render::WorldViewFrame frame;
	const core::Name owner("checkpoint-packet");
	const auto beforeCopy = WeatherCheckpointCopyBytes();
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpoint == original.Bytes);
	CHECK(WeatherCheckpointCopyBytes() == beforeCopy + original.Bytes.size());
	CHECK(frame.WeatherCheckpointTick == original.Tick);
	CHECK(frame.WeatherCheckpointEpoch == original.Epoch);
	CHECK(frame.WeatherCheckpointRevision == original.Revision);
	CHECK(frame.WeatherCheckpointSource == provider);
	const auto version = frame.WeatherCheckpointVersion;
	const auto *retained = frame.WeatherCheckpoint.data();
	const auto copied = WeatherCheckpointCopyBytes();
	store.AdvanceTick(1.0f / 60.0f);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpoint.data() == retained);
	CHECK(frame.WeatherCheckpointVersion == version);
	CHECK(WeatherCheckpointCopyBytes() == copied);

	render::WorldCameraFrame camera;
	render::WorldViewBinding binding{};
	binding.World = 3;
	binding.Name = owner;
	binding.Identity = store.Identity();
	render::View view;
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	CHECK(view.WeatherCheckpoint.data() == frame.WeatherCheckpoint.data());
	CHECK(view.WeatherCheckpointVersion == version);
	CHECK(view.WeatherCheckpointTick == original.Tick);
	CHECK(view.WeatherCheckpointEpoch == original.Epoch);
	CHECK(view.WeatherCheckpointRevision == original.Revision);
	const auto beforeSignature = render::ScenePresentationSignaturesOf(view, {}).Environment;

	store.ClearChanges();
	const auto other = AddStatefulWeather(store, original);
	auto changed = original;
	changed.Bytes[0] = std::byte{9};
	store.Set(other, changed);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpointVersion == version);
	CHECK(WeatherCheckpointCopyBytes() == copied);
	CHECK(frame.WeatherCheckpoint == original.Bytes);

	store.Set(provider, changed);
	CHECK(frame.WeatherCheckpoint == original.Bytes);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpoint == changed.Bytes);
	CHECK(frame.WeatherCheckpointVersion != version);
	CHECK(WeatherCheckpointCopyBytes() == copied + changed.Bytes.size());
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	CHECK(render::ScenePresentationSignaturesOf(view, {}).Environment != beforeSignature);

	store.ClearChanges();
	changed.Bytes[1] = std::byte{8};
	store.Set(provider, changed);
	store.ClearChanges();
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpoint == changed.Bytes);
	const auto sameBytesVersion = frame.WeatherCheckpointVersion;
	store.Set(provider, changed);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherCheckpointVersion == sameBytesVersion);
	CHECK(frame.WeatherCheckpoint.data() == retained);
}

TEST_CASE(
	"stateful weather checkpoint packets clear across source mode epoch and world replacement",
	"[render][world-view][weather]"
) {
	RegisterViewClasses();
	ecs::Store store("checkpoint-retirement");
	weather::StatefulWeatherCheckpoint checkpoint{10, 1, 1, {std::byte{1}, std::byte{2}}};
	const auto provider = AddStatefulWeather(store, checkpoint);
	render::WorldViewFrame frame;
	const core::Name owner("checkpoint-retirement");
	const auto collect = [&] { render::CollectWorldView(store, owner, frame); };
	collect();
	REQUIRE_FALSE(frame.WeatherCheckpoint.empty());
	const auto firstVersion = frame.WeatherCheckpointVersion;
	store.GetMutable<scene::WeatherClock>(provider)->Epoch = 2;
	collect();
	CHECK(frame.WeatherCheckpoint.empty());
	CHECK(frame.WeatherCheckpointVersion != firstVersion);
	CHECK(frame.WeatherCheckpointEpoch == 0);
	checkpoint.Epoch = 2;
	store.Set(provider, checkpoint);
	collect();
	CHECK(frame.WeatherCheckpoint == checkpoint.Bytes);
	CHECK(frame.WeatherCheckpointEpoch == 2);
	store.GetMutable<scene::WeatherCompute>(provider)->Mode = scene::WeatherMode::Procedural;
	collect();
	CHECK(frame.WeatherCheckpoint.empty());
	store.GetMutable<scene::WeatherCompute>(provider)->Mode = scene::WeatherMode::Stateful;
	collect();
	CHECK(frame.WeatherCheckpoint == checkpoint.Bytes);
	store.GetMutable<scene::WeatherCompute>(provider)->Enabled = false;
	collect();
	CHECK(frame.WeatherCheckpoint.empty());
	store.GetMutable<scene::WeatherCompute>(provider)->Enabled = true;
	collect();
	store.Remove<weather::StatefulWeatherCheckpoint>(provider);
	collect();
	CHECK(frame.WeatherCheckpoint.empty());
	store.Set(provider, checkpoint);
	collect();
	const auto other = AddStatefulWeather(store, checkpoint);
	store.DestroyInstance(provider);
	collect();
	CHECK(frame.WeatherCheckpointSource == other);
	CHECK(frame.WeatherCheckpoint == checkpoint.Bytes);
	const auto previousWorldVersion = frame.WeatherCheckpointVersion;
	ecs::Store replacement("checkpoint-retirement");
	AddStatefulWeather(replacement, checkpoint);
	render::CollectWorldView(replacement, owner, frame);
	CHECK(frame.Identity == replacement.Identity());
	CHECK(frame.WeatherCheckpointVersion != previousWorldVersion);
	ecs::Store empty("checkpoint-retirement");
	render::CollectWorldView(empty, owner, frame);
	CHECK(frame.WeatherCheckpoint.empty());
	CHECK(frame.WeatherCheckpointSource == ecs::NULL_ENTITY);
}

TEST_CASE(
	"camera weather bindings preserve authored particles and copied response lighting",
	"[render][world-view][weather]"
) {
	RegisterViewClasses();
	ecs::Store store("camera-weather-bind");
	render::WorldViewFrame frame;
	frame.Name = core::Name("camera-weather-bind");
	frame.Identity = store.Identity();
	frame.GpuParticles.emplace();
	frame.GpuParticles->Field.Seed = 11;
	render::WorldCameraFrame camera;
	camera.WeatherEffects.Precipitation.emplace();
	camera.WeatherEffects.Precipitation->Field.Seed = 55;
	camera.WeatherEffects.HasLighting = true;
	camera.WeatherEffects.Lighting = frame.Lighting;
	camera.WeatherEffects.Lighting.Ambient.R = 0.123f;
	render::WorldViewBinding binding{};
	binding.World = 1;
	binding.Name = frame.Name;
	binding.Identity = store.Identity();
	render::View view;
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	REQUIRE(view.GpuParticles);
	REQUIRE(view.WeatherParticles.size() == 1);
	CHECK(view.WeatherParticles.data() == &*camera.WeatherEffects.Precipitation);
	CHECK(view.GpuParticles->Field.Seed == 11);
	CHECK(view.WeatherParticles.front().Field.Seed == 55);
	CHECK(view.Lighting.Ambient.R == 0.123f);
	camera.WeatherEffects.Precipitation.reset();
	camera.WeatherEffects.HasLighting = false;
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	CHECK(view.WeatherParticles.empty());
	CHECK(view.Lighting.Ambient == frame.Lighting.Ambient);
}

TEST_CASE(
	"trusted weather stream names retain accepted paths while provider edits wait",
	"[render][world-view][weather]"
) {
	RegisterViewClasses();
	ecs::Store store("trusted-weather-name");
	weather::StatefulWeatherCheckpoint checkpoint{10, 1, 1, {std::byte{1}}};
	const auto provider = AddStatefulWeather(store, checkpoint);
	render::WorldViewFrame frame;
	const core::Name owner("trusted-weather-name");
	render::CollectWorldView(store, owner, frame);
	const auto currentPath = weather::WeatherLayerName(store, provider);
	REQUIRE_FALSE(currentPath.empty());
	CHECK(frame.WeatherSimulationLayer.Text() == currentPath);
	const auto initialName = frame.WeatherSimulationLayer;
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherSimulationLayer == initialName);
	scene::WeatherAcceptedDefinition accepted{*store.Get<scene::WeatherCompute>(provider), currentPath};
	store.Set(provider, accepted);
	const core::Name renamed("WeatherAfterBacklog");
	REQUIRE(store.SetProperty(provider, core::Name("Name"), &renamed, sizeof(renamed)));
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherSimulationLayer.Text() == currentPath);
	store.Remove<scene::WeatherAcceptedDefinition>(provider);
	const auto renamedPath = weather::WeatherLayerName(store, provider);
	REQUIRE(renamedPath != currentPath);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherSimulationLayer.Text() == renamedPath);

	accepted.Layer = "Canonical.Accepted.Provider#default";
	store.Set(provider, accepted);
	render::CollectWorldView(store, owner, frame);
	CHECK(frame.WeatherSimulationLayer.Text() == accepted.Layer);
	render::WorldCameraFrame camera;
	render::WorldViewBinding binding{};
	binding.World = 3;
	binding.Name = owner;
	binding.Identity = store.Identity();
	render::View view;
	REQUIRE(render::BindWorldView(frame, camera, binding, view));
	CHECK(view.WeatherSimulationLayer == frame.WeatherSimulationLayer);
	store.GetMutable<scene::WeatherCompute>(provider)->Enabled = false;
	accepted.Controls.Enabled = false;
	store.Set(provider, accepted);
	render::CollectWorldView(store, owner, frame);
	CHECK_FALSE(frame.WeatherSimulationLayer.IsValid());
}
