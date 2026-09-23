#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Storm.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/scene/Volume.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

TEST_SUITE_ID("engine.render.worldview")
TEST_DEPENDS("engine.render.worldpresentation")
TEST_DEPENDS("engine.render.spatialcanvas")

using namespace engine;

namespace {
	void RegisterViewClasses() {
		scene::RegisterSceneClasses();
		effects::RegisterEffectClasses();
		gui::RegisterGuiClasses();
		render::RegisterPresentationComponents();
	}
	ecs::Entity PartAt(ecs::Store &store, core::Vector3 position) {
		scene::PartDesc part;
		part.Frame.Position = position;
		return scene::MakePart(store, part);
	}
}

TEST_CASE("retained cloud density follows a frozen storm preset change", "[render][world-view][cloud]") {
	ecs::Store store("frozen-cloud");
	physics::Storm storm;
	physics::SetStorm(store, storm);
	render::WorldViewFrame frame;
	render::CollectWorldView(store, core::Name("frozen-cloud"), frame);
	REQUIRE(frame.CloudDensity.has_value());
	const float originalWidth = frame.CloudDensity->Config.RootSize.X;
	const auto *originalNodes = frame.CloudDensity->Nodes.data();
	render::CollectWorldView(store, core::Name("frozen-cloud"), frame);
	CHECK(frame.CloudDensity->Nodes.data() == originalNodes);

	storm.State.Parameters.InfluenceRadius *= 1.5f;
	physics::SetStorm(store, storm);
	render::CollectWorldView(store, core::Name("frozen-cloud"), frame);
	REQUIRE(frame.CloudDensity.has_value());
	CHECK(frame.CloudDensity->Config.RootSize.X == originalWidth * 1.5f);
	storm.State.Parameters.Energy += 0.01f;
	physics::SetStorm(store, storm);
	render::CollectWorldView(store, core::Name("frozen-cloud"), frame);
	REQUIRE(frame.CloudParameters.has_value());
	CHECK(frame.CloudParameters->Energy == storm.State.Parameters.Energy);
}

TEST_CASE("storm cloud density follows the selected GPU particle field", "[render][world-view][cloud]") {
	RegisterViewClasses();
	ecs::Store store("selected-cloud-field");
	physics::SetStorm(store, {});
	const ecs::Entity selected = PartAt(store, {});
	const ecs::Entity ignored = PartAt(store, {1.0f, 0.0f, 0.0f});
	store.Set(selected, scene::GpuParticleField{.CloudDensity = false});
	store.Set(ignored, scene::GpuParticleField{.CloudDensity = true});

	render::WorldViewFrame frame;
	render::CollectWorldView(store, core::Name("selected-cloud-field"), frame);
	REQUIRE(frame.GpuParticles.has_value());
	CHECK_FALSE(frame.GpuParticles->Field.CloudDensity);
	CHECK_FALSE(frame.CloudDensity.has_value());

	store.Set(selected, scene::GpuParticleField{.CloudDensity = true});
	store.Set(ignored, scene::GpuParticleField{.CloudDensity = false});
	render::CollectWorldView(store, core::Name("selected-cloud-field"), frame);
	CHECK(frame.GpuParticles->Field.CloudDensity);
	CHECK(frame.CloudDensity.has_value());
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
		REQUIRE(effects::RefreshEmitters(store) == 1);
		REQUIRE(effects::EmitParticles(store, emitter, 1));
		store.AdvanceTick(.1);
		render::CollectWorldView(store, core::Name("source"), frame);
		REQUIRE(frame.Particles.Batches.size() == 1);
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
	render::WorldCameraFrame first, second;
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
	eye.CameraFrame.Position = {farLight, 10, 10};
	render::CollectWorldCamera(store, eye, {200, 200}, second);
	REQUIRE(second.Lights.size() == render::MAX_SCENE_LIGHTS);
	REQUIRE(second.VolumeCount == scene::MAX_SCENE_VOLUMES);
	CHECK(second.Lights[0].Position.X == farLight);
	CHECK(first.Lights[0].Position.X == 0);
	CHECK(store.Get<gui::SpatialCanvas>(billboard)->Size.X < nearWidth);
	REQUIRE(first.SpatialCollectors.size() == 1);
	CHECK(first.SpatialCollectors[0].Collector == billboard);
	CHECK(first.SpatialCollectors[0].Canvas.Size.X == nearWidth);
	REQUIRE(second.SpatialCollectors.size() == 1);
	CHECK(second.SpatialCollectors[0].Canvas.Size.X < nearWidth);
	REQUIRE_FALSE(second.Ribbons.Vertices.empty());
	CHECK(second.Ribbons.Vertices[0].Position != firstVertex);
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
