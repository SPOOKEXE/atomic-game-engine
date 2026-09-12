// The client's half of a world, and what a snapshot restore does to it.
//
// **Written because the studio's Stop broke the viewport and nothing said so.**
// Press Play, press Stop, and the explorer still showed every instance while the
// screen went black - which is the worst shape a bug can have, because the thing
// that is wrong and the thing that looks wrong are in different modules. A
// headless test over the same sequence is where that gets cornered.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/CameraPortalView.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <client/ActiveScenes.hpp>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cmath>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.presentation")

using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

TEST_CASE("active scenes copy valid cameras after one presentation batch", "[client][active-scenes]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiComponents();
	Universe worlds({.Mode = world::ExecutionMode::WorldParallel});
	const auto zulu = worlds.Create({.Name = Name("Zulu")});
	const auto alpha = worlds.Create({.Name = Name("Alpha")});
	const auto retired = worlds.Create({.Name = Name("Retired")});
	const auto install = [&](WorldId world, float position) {
		worlds.Enter(world, [&](Store &store) {
			const auto eye = store.CreateInstance(scene::CameraClass(), "Eye");
			store.Set(eye, scene::Transform{core::CFrame(Vector3{position, 0, 0})});
			store.Set(eye, scene::Camera{});
			const auto anchor = store.CreateInstance(ecs::Classes::Find(Name("Part")), "LightAnchor");
			store.Set(anchor, scene::Transform{core::CFrame(Vector3{position, 1, 0})});
			const auto light = store.CreateInstance(ecs::Classes::Find(Name("PointLight")), "Light");
			REQUIRE(store.SetParent(light, anchor));
			store.SetResource(scene::ActiveCamera{eye});
			store.SetResource(render::DrawList{});
		});
	};
	install(zulu, 3);
	install(alpha, 1);
	worlds.Enter(retired, [](Store &store) { store.SetResource(render::DrawList{}); });

	client::ActiveSceneCollector collector;
	const std::array demands{
		client::ActiveSceneDemand{world::Presentation{zulu, .016f, .25f}, Name("zulu-pipeline")},
		client::ActiveSceneDemand{world::Presentation{retired, .016f, .25f}, Name("retired-pipeline")},
		client::ActiveSceneDemand{world::Presentation{alpha, .016f, .25f}, Name("alpha-pipeline")},
		client::ActiveSceneDemand{world::Presentation{zulu, .016f, .25f}, Name("zulu-pipeline")},
	};
	REQUIRE(collector.Collect(worlds, demands, {640, 480}) == 2);
	REQUIRE(collector.Scenes().size() == 2);
	CHECK(collector.Scenes()[0].Name == Name("Alpha"));
	CHECK(collector.Scenes()[0].View.Pipeline == Name("alpha-pipeline"));
	CHECK(collector.Scenes()[0].View.CameraFrame.Position.X == 1);
	CHECK(collector.Scenes()[1].Name == Name("Zulu"));
	CHECK(collector.Scenes()[1].View.Pipeline == Name("zulu-pipeline"));
	CHECK(collector.Scenes()[1].View.CameraFrame.Position.X == 3);
	CHECK(collector.Views().size() == 2);
	CHECK(collector.Views()[0].Instances.data() == collector.Scenes()[0].Frame->Instances.data());
	REQUIRE(collector.Views()[0].Lights.size() == 1);
	CHECK(collector.Views()[0].Lights.data() == collector.Scenes()[0].CameraLayers->Lights.data());

	size_t submissions = 0;
	std::vector<world::WorldId> submittedWorlds;
	std::vector<Name> submittedPipelines;
	const render::FrameResult submitted = collector.SubmitBatch(
		zulu, collector.Scenes()[1].View, 640, 480, false, {}, [&](std::span<const render::View> batch) {
			++submissions;
			for (const render::View &view : batch) {
				submittedWorlds.push_back(view.World == alpha.Index ? alpha : zulu);
				submittedPipelines.push_back(view.Pipeline);
			}
			render::FrameResult result;
			result.Presented = true;
			return result;
		}
	);
	CHECK(submissions == 1);
	CHECK(submitted.Presented);
	CHECK(submittedWorlds == std::vector<world::WorldId>{alpha, zulu});
	CHECK(submittedPipelines == std::vector<Name>{Name("alpha-pipeline"), Name("zulu-pipeline")});

	std::vector<const render::SceneTarget *> firstTargets;
	collector.SubmitBatch(
		zulu, collector.Scenes()[1].View, 640, 480, true, {}, [&](std::span<const render::View> batch) {
			REQUIRE(batch.size() == 2);
			for (const render::View &view : batch) {
				REQUIRE(view.Target != nullptr);
				CHECK(view.Target->Width == 640);
				CHECK(view.Target->Height == 480);
				firstTargets.push_back(view.Target);
			}
			CHECK(batch.back().Target != collector.Scenes()[1].View.Target);
			return render::FrameResult{};
		}
	);
	REQUIRE(firstTargets.size() == 2);
	CHECK(firstTargets[0] != firstTargets[1]);

	collector.SubmitBatch(
		zulu, collector.Scenes()[1].View, 0, 0, false, {}, [&](std::span<const render::View> batch) {
			REQUIRE(batch.size() == 2);
			REQUIRE(batch.front().Target != nullptr);
			CHECK(batch.front().Target->Width == 1);
			CHECK(batch.front().Target->Height == 1);
			CHECK(batch.back().Target == nullptr);
			return render::FrameResult{};
		}
	);

	REQUIRE(worlds.Destroy(alpha) == world::WorldStatus::Ok);
	const std::array remaining{
		client::ActiveSceneDemand{world::Presentation{alpha, .016f, .5f}, Name("alpha-pipeline")},
		client::ActiveSceneDemand{world::Presentation{zulu, .016f, .5f}, Name("zulu-pipeline")},
	};
	REQUIRE(collector.Collect(worlds, remaining, {640, 480}) == 1);
	REQUIRE(collector.Scenes().size() == 1);
	CHECK(collector.Scenes().front().Name == Name("Zulu"));
	collector.SubmitBatch(
		zulu, collector.Scenes().front().View, 320, 180, true, {}, [&](std::span<const render::View> batch) {
			REQUIRE(batch.size() == 1);
			REQUIRE(batch.front().Target != nullptr);
			CHECK(batch.front().Target->Width == 320);
			CHECK(batch.front().Target->Height == 180);
			return render::FrameResult{};
		}
	);
}

TEST_CASE("a trailing eye draws its original world after body admission", "[client][camera-portal-world]") {
	using namespace engine;
	scene::RegisterSceneClasses();
	replication::RegisterReplicationComponents();
	Universe worlds;
	const auto near = worlds.Create({.Name = Name("Near")});
	const auto far = worlds.Create({.Name = Name("Far")});
	const auto replica = worlds.Create({.Name = Name("Far (client)")});
	scene::CameraPortalView history;
	core::CFrame original(Vector3{1, 2, 3});
	REQUIRE(scene::StepCameraPortalView(history, "Near", original, {}) == scene::CameraPortalStep::Settled);
	scene::SeamTransform through{
		core::CFrame(Vector3{10, 20, 30}) * core::CFrame::Angles(.3f, .7f, .2f), Vector3{4, 5, 6}, .5f
	};
	REQUIRE(scene::RebaseCameraPortalView(history, through));
	worlds.Enter(replica, [&](Store &store) {
		store.SetResource(world::Replica{true, Name("Far"), Name("viewer")});
		const auto camera = store.CreateInstance(scene::CameraClass(), "Eye");
		store.Set(camera, history);
		store.SetResource(scene::ActiveCamera{camera});
	});
	auto eye = through.Place(original);
	scene::Camera lens;
	const float nearPlane = lens.NearPlane;
	CHECK(client::ResolveCameraPortalWorld(worlds, replica, far, eye, lens) == near);
	CHECK((eye.Position - original.Position).Magnitude() < .0001f);
	CHECK(lens.NearPlane == Catch::Approx(nearPlane * 2));
}

TEST_CASE(
	"camera routing crosses and returns through copied destination topology", "[client][remote-eye-route]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	replication::RegisterReplicationComponents();
	Universe viewer, producer;
	const auto near = viewer.Create({.Name = Name("Near")});
	const bool persistentOwner = GENERATE(false, true);
	const auto owner = viewer.Create({.Name = Name("ViewOwner")});
	const auto remoteFar = viewer.CreateRemote({.Name = Name("Far")}, Name("producer"));
	const auto far = producer.Create({.Name = Name("Far")});
	producer.CreateRemote({.Name = Name("Near")}, Name("viewer"));
	producer.CreateRemote({.Name = Name("ViewOwner")}, Name("viewer"));
	REQUIRE(viewer.ConfigurePresentation(123));
	REQUIRE(producer.ConfigurePresentation(456));
	const auto install = [](Universe &worlds, WorldId world, bool reverse) {
		worlds.Enter(world, [&](Store &store) {
			const auto pane = store.CreateInstance(ecs::Classes::Find(Name("Part")), "Door");
			const auto standIn = store.CreateInstance(ecs::Classes::Find(Name("Part")), "StandIn");
			const core::CFrame front;
			const auto back =
				core::CFrame(Vector3{0, 0, -.2f}) * core::CFrame::Angles(0, 3.14159265358979323846f, 0);
			store.Set(pane, scene::Transform{reverse ? back : front});
			store.Set(standIn, scene::Transform{reverse ? front : back});
			store.Set(pane, scene::Bounds{{2, 3, .1f}});
			store.Set(standIn, scene::Bounds{{2, 3, .1f}});
			const auto portal = store.CreateInstance(ecs::Classes::Find(Name("Portal")), "Portal");
			REQUIRE(store.SetParent(portal, pane));
			store.Set(
				portal,
				scene::Portal{.Destination = standIn, .DestinationWorld = Name(reverse ? "Near" : "Far")}
			);
		});
	};
	install(viewer, near, false);
	install(producer, far, true);
	Entity camera, humanoid;
	viewer.Enter(near, [&](Store &store) {
		camera = store.CreateInstance(scene::CameraClass(), "Eye");
		humanoid = store.CreateInstance(ecs::Classes::Find(Name("Humanoid")), "Subject");
		store.Set(camera, scene::CameraSubject{humanoid, false});
		store.SetResource(scene::ActiveCamera{camera});
	});
	render::Renderer sourceRenderer, destinationRenderer;
	render::PortalImageHost images(viewer, sourceRenderer), destination(producer, destinationRenderer);
	REQUIRE(destination.Serve(far).Generation != 0);
	REQUIRE(
		viewer.ApplyPresentationDirectory(Name("producer"), producer.LocalPresentationDirectory()) ==
		world::PresentationStatus::Ok
	);
	constexpr render::PortalImageHost::Time now{};
	core::CFrame eye(Vector3{0, 0, 1});
	scene::Camera lens;
	const auto topologyOwner = persistentOwner ? owner : WorldId{};
	REQUIRE(
		client::ResolveCameraPortalWorld(viewer, near, near, eye, lens, &images, now, topologyOwner) == near
	);
	eye.Position.Z = -1;
	CHECK_FALSE(
		client::ResolveCameraPortalWorld(viewer, near, near, eye, lens, &images, now, topologyOwner).IsValid()
	);
	CHECK(eye.Position.Z == -1);
	viewer.Enter(near, [&](Store &store) {
		const auto *history = store.Get<scene::CameraPortalView>(camera);
		REQUIRE(history != nullptr);
		CHECK(history->World == "Near");
		CHECK(history->Previous.Position.Z == 1);
	});
	REQUIRE(
		producer.ApplyPresentationDirectory(Name("viewer"), viewer.LocalPresentationDirectory()) ==
		world::PresentationStatus::Ok
	);
	const auto requests = viewer.TakePresentationOutbound();
	REQUIRE(requests.size() == 1);
	REQUIRE(
		producer.IngestPresentation(Name("viewer"), requests[0].Message) == world::PresentationStatus::Ok
	);
	destination.Pump(0, 1, now);
	const auto replies = producer.TakePresentationOutbound();
	REQUIRE(replies.size() == 1);
	REQUIRE(viewer.IngestPresentation(Name("producer"), replies[0].Message) == world::PresentationStatus::Ok);
	const bool hostAlreadyPumped = GENERATE(false, true);
	CAPTURE(hostAlreadyPumped);
	if (hostAlreadyPumped) images.Pump(0, 1, now);
	REQUIRE(
		client::ResolveCameraPortalWorld(viewer, near, near, eye, lens, &images, now, topologyOwner) ==
		remoteFar
	);
	CHECK((eye.Position - Vector3{0, 0, -1}).Magnitude() < .0001f);
	eye = core::CFrame(Vector3{0, 0, 1});
	bool available = true;
	auto next = now;
	SECTION("return crossing") {}
	SECTION("withdrawn endpoint") {
		viewer.RetirePresentationHost(Name("producer"));
		available = false;
	}
	SECTION("expired topology") {
		next += std::chrono::seconds(1);
		available = false;
	}
	const auto result =
		client::ResolveCameraPortalWorld(viewer, near, near, eye, lens, &images, next, topologyOwner);
	CHECK(result == (available ? near : WorldId{}));
	CHECK((eye.Position - Vector3{0, 0, 1}).Magnitude() < .0001f);
	viewer.Enter(near, [&](Store &store) {
		CHECK(store.Get<scene::CameraPortalView>(camera)->World == (available ? "Near" : "Far"));
		CHECK(store.Get<scene::CameraSubject>(camera)->Target == humanoid);
	});
	images.RemoveWorld(near);
	(void)viewer.Destroy(near);
	CHECK((images.Topology(remoteFar, next) != nullptr) == (available && persistentOwner));
}

TEST_CASE(
	"portal image route keeps its authenticated producer when a replica arrives",
	"[client][portal-image-route]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	replication::RegisterReplicationComponents();
	Universe viewer, producer;
	REQUIRE(viewer.ConfigurePresentation(123));
	REQUIRE(producer.ConfigurePresentation(456));
	const auto near = viewer.Create({.Name = Name("Near (client)")});
	const auto far = producer.Create({.Name = Name("Far")});
	const auto remote = viewer.CreateRemote({.Name = Name("Far")}, Name("producer"));
	const auto joining = viewer.Create({.Name = Name("Far (client)")});
	viewer.Enter(near, [](Store &store) {
		const auto workspace = scene::InstallServices(store);
		store.SetResource(world::Replica{true, Name("Near"), Name("viewer")});
		scene::PartDesc part;
		part.Frame.Position = {0, 0, -4};
		part.Size = {4, 4, .1f};
		const auto entrance = scene::MakePart(store, part);
		REQUIRE(store.SetParent(entrance, workspace));
		part.Frame.Position.X = 20;
		const auto exit = scene::MakePart(store, part);
		REQUIRE(store.SetParent(exit, workspace));
		const auto camera = store.CreateInstance(ecs::Classes::Find(Name("Portal")), "Door");
		REQUIRE(store.SetParent(camera, entrance));
		store.Set(camera, scene::Portal{.Destination = exit, .DestinationWorld = Name("Far")});
		std::vector<scene::SurfaceSlot> slots;
		scene::GatherSurfaceSlots(store, slots);
		REQUIRE(slots.size() == 1);
		store.GetMutable<scene::SurfaceCamera>(camera)->Surface = slots[0].Index;
	});
	viewer.Enter(joining, [](Store &store) {
		store.SetResource(world::Replica{true, Name("Far"), Name("viewer")});
		store.SetResource(replication::SnapshotBuffer{});
		physics::PreparePhysicsWorld(store);
	});
	render::Renderer renderer, destinationRenderer;
	render::PortalImageHost images(viewer, renderer), destination(producer, destinationRenderer);
	const auto endpoint = destination.Serve(far);
	REQUIRE(endpoint.Generation != 0);
	REQUIRE(
		viewer.ApplyPresentationDirectory(Name("producer"), producer.LocalPresentationDirectory()) ==
		world::PresentationStatus::Ok
	);
	render::View view;
	view.World = near.Index;
	view.WorldName = viewer.NameOf(near);
	std::vector<render::PortalView> portals;
	std::vector<render::SurfaceView> surfaces;
	constexpr render::PortalImageHost::Time now{};
	CHECK_FALSE(
		client::UpdatePortalImages(
			viewer, images, near, view, {.Width = 32, .Height = 32}, portals, surfaces, 0, now, {}, joining
		)
	);
	REQUIRE(portals.size() == 1);
	REQUIRE_FALSE(viewer.TakePresentationOutbound().empty());
	REQUIRE(viewer.LookupPresentation(remote, render::PORTAL_REQUEST_CHANNEL) == endpoint);
	viewer.Enter(joining, [](Store &store) { client::RecordReplicatedTick(store, 7); });
	portals.clear();
	CHECK_FALSE(
		client::UpdatePortalImages(
			viewer,
			images,
			near,
			view,
			{.Width = 32, .Height = 32},
			portals,
			surfaces,
			0,
			now + std::chrono::milliseconds(1)
		)
	);
	CHECK(viewer.LookupPresentation(joining, render::PORTAL_REQUEST_CHANNEL).Generation == 0);
	SECTION("an explicitly admitted replica serves locally while the remote endpoint remains") {
		(void)client::UpdatePortalImages(
			viewer,
			images,
			near,
			view,
			{.Width = 32, .Height = 32},
			portals,
			surfaces,
			0,
			now + std::chrono::milliseconds(2),
			{},
			near
		);
		CHECK(viewer.LookupPresentation(joining, render::PORTAL_REQUEST_CHANNEL).Generation == 0);
		(void)client::UpdatePortalImages(
			viewer,
			images,
			near,
			view,
			{.Width = 32, .Height = 32},
			portals,
			surfaces,
			0,
			now + std::chrono::milliseconds(3),
			{},
			joining
		);
		CHECK(viewer.LookupPresentation(joining, render::PORTAL_REQUEST_CHANNEL).Generation != 0);
		CHECK(viewer.LookupPresentation(remote, render::PORTAL_REQUEST_CHANNEL) == endpoint);
	}
	SECTION("withdrawal still allows the existing local fallback") {
		destination.RemoveWorld(far);
		REQUIRE(
			viewer.ApplyPresentationDirectory(Name("producer"), producer.LocalPresentationDirectory()) ==
			world::PresentationStatus::Ok
		);
		portals.clear();
		(void)client::UpdatePortalImages(
			viewer,
			images,
			near,
			view,
			{.Width = 32, .Height = 32},
			portals,
			surfaces,
			0,
			now + std::chrono::milliseconds(2)
		);
		CHECK(viewer.LookupPresentation(joining, render::PORTAL_REQUEST_CHANNEL).Generation != 0);
	}
}

TEST_CASE("portal routes wait for a destination replica snapshot", "[client][portal-arrival-route]") {
	engine::replication::RegisterReplicationComponents();
	Universe worlds;
	const auto source = worlds.Create({.Name = Name("Near (client)")});
	const auto authority = worlds.Create({.Name = Name("Far")});
	const auto joining = worlds.Create({.Name = Name("Far (client)")});
	worlds.Enter(source, [](Store &store) {
		store.SetResource(engine::world::Replica{true, Name("Near"), Name("viewer")});
	});
	worlds.Enter(joining, [](Store &store) {
		store.SetResource(engine::world::Replica{true, Name("Far"), Name("viewer")});
		store.SetResource(engine::replication::SnapshotBuffer{});
		engine::physics::PreparePhysicsWorld(store);
	});
	std::vector<client::WorldIdentity> surveyed;
	client::SurveyWorlds(worlds, surveyed);
	CHECK(client::ResolveDestinationWorld(surveyed, source, Name("Far")) == authority);
	worlds.Enter(joining, [](Store &store) { client::RecordReplicatedTick(store, 7); });
	client::SurveyWorlds(worlds, surveyed);
	CHECK(client::ResolveDestinationWorld(surveyed, source, Name("Far")) == joining);
	worlds.Enter(joining, [](Store &store) { store.SetResource(engine::replication::SnapshotBuffer{}); });
	worlds.Destroy(authority);
	client::SurveyWorlds(worlds, surveyed);
	CHECK_FALSE(client::ResolveDestinationWorld(surveyed, source, Name("Far")).IsValid());
}

namespace {

	// Where a script's content lives now.
	//
	// **`part.Parent = workspace` used to make a root and now makes a child of
	// the `Workspace` service**, so a lookup by root finds nothing. See
	// `script/LuauBindings.hpp`'s `OpenWorkspace` for why the two notions of "the
	// workspace" were collapsed, and `scene/Visibility.hpp` for what the tree
	// now decides.
	//
	// Falls back to a root, because some of these scripts deliberately leave an
	// instance unparented - an orphan is still reachable from C++ through
	// `EachRoot`, and only a *script* is unable to list one. A test about
	// signals or tasks should not have to care which of the two its fixture is.
	Entity InScene(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			if (const Entity child = store.FindFirstChild(workspace, name);
				child != engine::ecs::NULL_ENTITY) {
				return child;
			}
		}
		return store.FindFirstRoot(name);
	}
	WorldId AddWorld(Universe &universe, std::string_view name) {
		engine::scene::RegisterSceneClasses();

		WorldSettings settings;
		settings.Name = Name(name);

		const WorldId id = universe.Create(settings);
		universe.Enter(id, [](Store &store, Scheduler &systems) {
			client::InstallPresentation(store, systems, 16);
		});
		return id;
	}

	// A part, **in the scene** - which since v0.7 means under `Workspace`
	// rather than merely alive in the world.
	//
	// The parenting is the fixture's job and not a detail of it: a draw list is
	// the `Workspace` subtree now, so a part created and left unparented is one
	// the world is entitled to publish nothing for. See
	// `scene/Visibility.hpp`; `AddOrphan` below is the other half of the same
	// statement.
	void AddPart(Universe &universe, WorldId world, std::string_view name) {
		universe.Enter(world, [name](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);

			const Vector3 size{8.0f, 2.0f, 4.0f};
			store.SetProperty(part, Name("Size"), &size, sizeof(size));

			store.SetParent(part, engine::scene::InstallServices(store));
		});
	}

	// A part that is complete and belongs to nothing.
	void AddOrphan(Universe &universe, WorldId world, std::string_view name) {
		universe.Enter(world, [name](Store &store) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);

			const Vector3 size{8.0f, 2.0f, 4.0f};
			store.SetProperty(part, Name("Size"), &size, sizeof(size));
		});
	}

	// How many instances the world published for its renderer, after one
	// presentation phase.
	size_t Drawn(Universe &universe, WorldId world) {
		universe.Present(world, 1.0f / 60.0f, 0.0f);

		size_t count = 0;
		universe.Enter(world, [&count](Store &store) {
			if (const auto *list = store.Resource<engine::render::DrawList>()) {
				count = list->Instances.size();
			}
		});
		return count;
	}
}

TEST_CASE("particle light properties reach the render batch", "[client][presentation][particles]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "particle-light-properties");

	universe.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);
		engine::scene::PartDesc parentDescription;
		parentDescription.Simulated = false;
		const Entity parent = engine::scene::MakePart(store, parentDescription);
		store.SetParent(parent, workspace);

		const Entity emitter =
			store.CreateInstance(engine::ecs::Classes::Find(Name("ParticleEmitter")), "LitEmitter");
		store.SetParent(emitter, parent);
		auto *settings = store.GetMutable<engine::effects::ParticleEmitter>(emitter);
		REQUIRE(settings != nullptr);
		settings->Rate = 60.0f;
		settings->Lifetime = engine::core::NumberRange{1.0f, 1.0f};
		settings->LightEmission = 0.35f;
		settings->LightInfluence = 0.8f;

		engine::scene::ResolveAttachments(store);
		REQUIRE(engine::effects::RefreshEmitters(store) == 1);
		store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;
		const engine::effects::ParticleStatistics first = engine::effects::StepParticles(store, 1.0f / 30.0f);
		const engine::effects::ParticleStatistics second =
			engine::effects::StepParticles(store, 1.0f / 30.0f);
		CHECK(first.Live == 0);
		CHECK(second.Live == 0);

		engine::render::ParticleFrame frame;
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 1);
		REQUIRE(frame.Batches.size() == 1);
		const auto *system = store.Resource<engine::effects::ParticleSystem>();
		CHECK(first.Emitted == 0);
		CHECK(second.Emitted == 0);
		CHECK(frame.SourceRevision == system->PresentationRevision);

		(void)engine::effects::StepParticles(store, 1.0f / 30.0f);
		system = store.Resource<engine::effects::ParticleSystem>();
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 1);
		CHECK(frame.Batches[0].LightEmission == Catch::Approx(0.35f));
		CHECK(frame.Batches[0].LightInfluence == Catch::Approx(0.8f));

		// The block is what the batch carries now, and the renderer steps and
		// draws from it - see `render::ParticleBatch`.
		REQUIRE(frame.Batches[0].Block != nullptr);
		REQUIRE(frame.Batches[0].Spawn != nullptr);
		REQUIRE(frame.Batches[0].Runtime != nullptr);
		CHECK(frame.Batches[0].Block->Capacity > 0);
		CHECK(frame.Batches[0].Runtime->ContinuousRate == 60.0f);
		CHECK(frame.Pool > 0);
	});
}

// **The studio's copy, and it is the studio that needs it.** `Renderer::Render`
// runs after `Universe::Enter` has returned, and by then the world may be
// stepping again - so a batch pointing into `ParticleSystem::Blocks` is a
// pointer into an array whose owner is running. `Detach` copies the blocks and
// repoints; the copy is a few hundred bytes an emitter, against the twenty-eight
// a particle used to cost when a batch was a span of them.
TEST_CASE("a detached particle frame stops pointing into the world", "[client][presentation][particles]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "particle-detach");

	universe.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::InstallServices(store);
		engine::scene::PartDesc parentDescription;
		parentDescription.Simulated = false;
		const Entity parent = engine::scene::MakePart(store, parentDescription);
		store.SetParent(parent, workspace);

		for (int index = 0; index < 3; index++) {
			const Entity emitter = store.CreateInstance(engine::ecs::Classes::Find(Name("ParticleEmitter")));
			store.SetParent(emitter, parent);
			auto *settings = store.GetMutable<engine::effects::ParticleEmitter>(emitter);
			REQUIRE(settings != nullptr);
			settings->Rate = 60.0f;
			settings->Lifetime = engine::core::NumberRange{1.0f, 1.0f};
		}

		engine::scene::ResolveAttachments(store);
		REQUIRE(engine::effects::RefreshEmitters(store) == 3);
		store.ResourceMutable<engine::effects::ParticleSystem>()->DeviceStepped = true;
		CHECK(engine::effects::StepParticles(store, 1.0f / 30.0f).Emitted == 0);

		engine::render::ParticleFrame frame;
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 3);

		const auto *system = store.Resource<engine::effects::ParticleSystem>();
		const engine::effects::EmitterBlock *pool = system->Blocks.data();
		for (const engine::render::ParticleBatch &batch : frame.Batches) {
			REQUIRE(batch.Block >= pool);
			REQUIRE(batch.Block < pool + system->Blocks.size());
		}

		frame.Detach();
		const uint64_t collectedRevision = frame.Revision;
		const uint64_t collectedResident = frame.ResidentRevision;
		const engine::effects::EmitterBlock *const detachedPool = frame.Blocks.data();

		// A render-rate collection of the same simulation revision is constant
		// time and Detach is safe to repeat. Studio does both between ticks.
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 3);
		frame.Detach();
		CHECK(frame.Revision == collectedRevision);
		CHECK(frame.Blocks.data() == detachedPool);

		// **Every batch, and each still describing its own block.** Repointing
		// them all at the first copy would be a frame of three emitters drawing
		// one emitter's particles three times, which is the kind of wrong that
		// looks like a working scene.
		REQUIRE(frame.Blocks.size() == frame.Batches.size());
		for (size_t at = 0; at < frame.Batches.size(); at++) {
			CHECK(frame.Batches[at].Block == frame.Blocks.data() + at);
			CHECK(frame.Batches[at].Block->First == system->Blocks[at].First);
			CHECK(frame.Batches[at].Block->Capacity == system->Blocks[at].Capacity);
			CHECK(frame.Batches[at].Block->Generation == system->Blocks[at].Generation);
		}

		// Simulation advances the source revision without changing layout or any
		// device-table input. The detached storage and its safe pointers stay put.
		(void)engine::effects::StepParticles(store, 1.0f / 30.0f);
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 3);
		CHECK(frame.Revision == collectedRevision + 1);
		CHECK(frame.ResidentRevision == collectedResident);
		CHECK(frame.Detached);
		CHECK(frame.Blocks.data() == detachedPool);
		for (size_t at = 0; at < frame.Batches.size(); at++) {
			CHECK(frame.Batches[at].Block == frame.Blocks.data() + at);
		}

		// Draw-state changes are layout changes even without another simulation
		// step. They rebuild the borrowed batch metadata once, then can be detached
		// again for the renderer outside the world boundary.
		const uint64_t collectedLayout = frame.LayoutRevision;
		auto *changedEmitter = store.GetMutable<engine::effects::ParticleEmitter>(system->Blocks[0].Owner);
		REQUIRE(changedEmitter != nullptr);
		changedEmitter->Additive = !changedEmitter->Additive;
		REQUIRE(engine::effects::RefreshEmitters(store) == 3);
		REQUIRE(engine::render::CollectParticleBatches(store, frame) == 3);
		CHECK(frame.LayoutRevision == collectedLayout + 1);
		CHECK(frame.ResidentRevision == collectedResident + 1);
		CHECK_FALSE(frame.Detached);
		CHECK(frame.Batches[0].Additive == changedEmitter->Additive);
	});
}

TEST_CASE("a world with presentation installed publishes what it holds", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.basic");
	AddPart(universe, world, "One");
	AddPart(universe, world, "Two");

	CHECK(Drawn(universe, world) == 2);
}

// **The rule the renderer had never been told**, and the reason
// `scene/Visibility.hpp` exists: a draw list is the `Workspace` subtree, not
// every entity that happens to carry the right components.
//
// Before v0.7 both of these drew. A part in `ReplicatedStorage`, a template
// under `StarterGui` and an orphan a script had made and not yet parented were
// all complete parts by a component test, so all of them were on screen.
TEST_CASE("only the Workspace subtree is published", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.gate");

	AddPart(universe, world, "InScene");
	AddOrphan(universe, world, "Orphan");

	CHECK(Drawn(universe, world) == 1);

	// Parenting it in is what makes it appear, and nothing else has to happen:
	// no component is added, no flag is set by the caller, and the part was
	// complete the whole time.
	universe.Enter(world, [](Store &store) {
		const Entity orphan = store.FindFirstRoot("Orphan");
		REQUIRE(orphan != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(orphan, engine::scene::WorkspaceOf(store)));
	});

	CHECK(Drawn(universe, world) == 2);

	// And taking it out again removes it, which is the half a set of hooks on
	// the *parenting* side would most easily get wrong - the gate is derived
	// from the tree every pass rather than maintained by whoever moved
	// something.
	universe.Enter(world, [](Store &store) {
		const Entity orphan = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Orphan");
		REQUIRE(orphan != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(orphan, engine::ecs::NULL_ENTITY));
	});

	CHECK(Drawn(universe, world) == 1);
}

// A whole model moving is the case a per-instance hook cannot answer: nothing
// reparented the parts, and every one of them changed scene.
TEST_CASE("a subtree follows its ancestor in and out of the scene", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.subtree");

	universe.Enter(world, [](Store &store) {
		const Entity model = store.CreateInstance(engine::scene::PartClass(), "Model");
		for (int index = 0; index < 3; index++) {
			const Entity child = store.CreateInstance(engine::scene::PartClass(), "Limb");
			REQUIRE(store.SetParent(child, model));
		}
		REQUIRE(store.SetParent(model, engine::scene::InstallServices(store)));
	});

	// The model and its three limbs.
	CHECK(Drawn(universe, world) == 4);

	universe.Enter(world, [](Store &store) {
		const Entity model = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Model");
		REQUIRE(store.SetParent(model, engine::ecs::NULL_ENTITY));
	});

	CHECK(Drawn(universe, world) == 0);
}

// `Visible` was declared, bound, saved and reloaded from v0.4, and no draw path
// read it. It is a term of the gate now rather than a branch in a loop - see
// `scene/Visibility.hpp` on why a tag and not a boolean test.
TEST_CASE("an invisible part in the Workspace is not published", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.visible");
	AddPart(universe, world, "Seen");
	AddPart(universe, world, "Hidden");

	CHECK(Drawn(universe, world) == 2);

	universe.Enter(world, [](Store &store) {
		const Entity hidden = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Hidden");
		const bool visible = false;
		REQUIRE(store.SetProperty(hidden, Name("Visible"), &visible, sizeof(visible)));
	});

	CHECK(Drawn(universe, world) == 1);

	// **Still collides, still exists, still has its transform.** `Visible` and
	// `Transparency` are different questions and neither is "delete it" - a
	// draw path that treated one as the other would give invisible parts no
	// physics.
	universe.Enter(world, [](Store &store) {
		const Entity hidden = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Hidden");
		CHECK(hidden != engine::ecs::NULL_ENTITY);
		CHECK(store.Get<engine::scene::Collider>(hidden) != nullptr);
	});
}

TEST_CASE("a universe survives a snapshot with a draw list in it", "[client][presentation]") {
	// **`DrawList` had no registration at all before v0.7**, so
	// `Store::SetResource` minted one under the compiler's spelling of the type
	// and `Store::Save` refused it for having no serialisation. Nothing noticed
	// until the studio tried to snapshot a world in order to restore it later.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.snapshot");
	AddPart(universe, world, "One");

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));
	CHECK(writer.Bytes().size() > 0);
}

TEST_CASE("what a restore puts back is still drawable", "[client][presentation]") {
	// The studio's Stop, in a test: snapshot, change the world, restore, and
	// ask whether the renderer would see anything. The instances coming back is
	// half the answer and the half a tree view can show; the draw list being
	// refilled is the other half and the half a screenshot showed was missing.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.restore");
	AddPart(universe, world, "Original");

	REQUIRE(Drawn(universe, world) == 1);

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));

	std::vector<std::byte> snapshot(writer.Bytes().begin(), writer.Bytes().end());

	// What "running the game" did to the scene.
	AddPart(universe, world, "MadeWhileRunning");
	REQUIRE(Drawn(universe, world) == 2);

	engine::core::ByteReader reader(snapshot);
	REQUIRE(universe.Load(reader));

	// **The schedulers went with the worlds.** `Universe::Load` clears its
	// registry and adopts fresh worlds, so a restored world has an empty
	// scheduler and publishes nothing until presentation is installed again.
	// That is the studio's job and this is the line that says so.
	const WorldId restored = universe.Find(Name("presentation.restore"));
	REQUIRE(restored.IsValid());

	universe.Enter(restored, [](Store &store, Scheduler &systems) {
		client::InstallPresentation(store, systems, 16);
	});

	CHECK(Drawn(universe, restored) == 1);
}

TEST_CASE("a part's transparency survives a snapshot", "[client][presentation]") {
	// Found while chasing the black viewport above: `scene::Visual` has a
	// custom serialiser, and a field a custom serialiser forgets is a field
	// that silently resets on every load. This is the cheapest possible test
	// for that whole class of bug, and it applies to whatever is added to
	// `Visual` next.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.visual");

	universe.Enter(world, [](Store &store) {
		const Entity part = store.CreateInstance(engine::scene::PartClass(), "Glass");

		const float transparency = 0.75f;
		REQUIRE(store.SetProperty(part, Name("Transparency"), &transparency, sizeof(transparency)));

		const engine::core::Color3 tint{0.9f, 0.35f, 0.15f};
		REQUIRE(store.SetProperty(part, Name("Color"), &tint, sizeof(tint)));

		// **`CastShadow` is the next field this was written for.** It arrived at
		// v0.7 into the same hand-written serialiser, and it is off here rather
		// than on so that a reader that silently dropped it would come back
		// `true` and fail - a check against the default is no check at all.
		const bool casts = false;
		REQUIRE(store.SetProperty(part, Name("CastShadow"), &casts, sizeof(casts)));

		// Same for `Visible`, which had no coverage here either.
		const bool visible = false;
		REQUIRE(store.SetProperty(part, Name("Visible"), &visible, sizeof(visible)));

		// **And `Locked`, which arrived at v0.12 into the same serialiser.**
		// Set to `true` here for `CastShadow`'s reason inverted: the default is
		// `false`, so a reader that dropped the field would come back unlocked
		// and this would fail. A part somebody locked and then saved coming
		// back grabbable is the one thing locking it was for.
		const bool locked = true;
		REQUIRE(store.SetProperty(part, Name("Locked"), &locked, sizeof(locked)));
	});

	engine::core::ByteWriter writer;
	REQUIRE(universe.Save(writer));
	std::vector<std::byte> snapshot(writer.Bytes().begin(), writer.Bytes().end());

	engine::core::ByteReader reader(snapshot);
	REQUIRE(universe.Load(reader));

	universe.Enter(universe.Find(Name("presentation.visual")), [](Store &store) {
		const Entity part = InScene(store, "Glass");
		REQUIRE(part != engine::ecs::NULL_ENTITY);

		float transparency = 0.0f;
		REQUIRE(store.GetProperty(part, Name("Transparency"), &transparency, sizeof(transparency)));
		CHECK(transparency == 0.75f);

		engine::core::Color3 tint;
		REQUIRE(store.GetProperty(part, Name("Color"), &tint, sizeof(tint)));
		CHECK(tint.R == 0.9f);

		bool casts = true;
		REQUIRE(store.GetProperty(part, Name("CastShadow"), &casts, sizeof(casts)));
		CHECK_FALSE(casts);

		bool visible = true;
		REQUIRE(store.GetProperty(part, Name("Visible"), &visible, sizeof(visible)));
		CHECK_FALSE(visible);

		bool locked = false;
		REQUIRE(store.GetProperty(part, Name("Locked"), &locked, sizeof(locked)));
		CHECK(locked);
	});
}

// `CastShadow` reaching the renderer at all, which is a different question from
// it surviving a file: the draw list is a flat copy of the `Visual` and a field
// left out of *that* is one the shadow pass can never see. `Transparency` and
// `Surface` were both missing from the replica's copy of this list for exactly
// that reason.
TEST_CASE("what a part looks like reaches the draw list whole", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.drawfields");
	AddPart(universe, world, "Pane");

	universe.Enter(world, [](Store &store) {
		const Entity pane = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Pane");

		const float transparency = 0.5f;
		REQUIRE(store.SetProperty(pane, Name("Transparency"), &transparency, sizeof(transparency)));

		const bool casts = false;
		REQUIRE(store.SetProperty(pane, Name("CastShadow"), &casts, sizeof(casts)));
	});

	REQUIRE(Drawn(universe, world) == 1);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<engine::render::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);

		CHECK(list->Instances[0].Transparency == 0.5f);
		CHECK_FALSE(list->Instances[0].CastShadow);
	});
}

// **A mirror, in a world built the way every host except `--scene` builds
// one.** This is the regression that made the studio's mirror world a plain
// white rectangle: `aim-surface-cameras` was registered only by
// `BuildScriptedWorld`, so the studio, `--game` and an imported world all
// presented a mirror that was never aimed.
//
// The visible half of that is not the camera. Aiming is also what writes
// `Visual::Surface` onto the pane - step 4 of `scene/SurfaceCameras.hpp` - so
// without the system the pane keeps the default of -1, samples no texture, and
// draws as its own flat tint. A white pane looks like a broken surface pass,
// which is why it went to the renderer twice before it came here.
//
// Asserted through `Universe::Present` rather than by calling
// `AimSurfaceCameras` directly, because what was wrong was the registration and
// nothing else: `scene/tests/SurfaceCameras.cpp` already proves the arithmetic,
// and it passed the whole time the mirror was white.
TEST_CASE("a world that only presents still aims its mirrors", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.mirror");
	AddPart(universe, world, "Pane");

	universe.Enter(world, [](Store &store) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		const Entity pane = store.FindFirstChild(workspace, "Pane");

		// The viewer, which is what there is a reflection *of*. Without one a
		// mirror has nothing to compute rather than a default.
		const Entity eye = store.CreateInstance(engine::scene::CameraClass(), "Eye");
		store.SetParent(eye, workspace);
		store.Set(eye, engine::scene::Transform{engine::core::CFrame(Vector3{0.0f, 0.0f, 20.0f})});
		store.SetResource(engine::scene::ActiveCamera{eye, 16.0f / 9.0f});

		// Parented to the pane and given a face, which is the whole of the
		// setup this feature exists to make sufficient.
		const Entity reflection =
			store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");
		store.Set(reflection, engine::scene::SurfaceCamera{});
		REQUIRE(store.SetParent(reflection, pane));

		REQUIRE(store.Get<engine::scene::Visual>(pane)->Surface == -1);
	});

	// The authored pane is published; cameras carry no drawable geometry.
	CHECK(Drawn(universe, world) == 1);

	universe.Enter(world, [](Store &store) {
		const Entity pane = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Pane");

		// The assertion the white rectangle was: a pane told which texture it
		// shows, by nothing more than a camera being parented to it.
		CHECK(store.Get<engine::scene::Visual>(pane)->Surface == 0);

		const auto *list = store.Resource<engine::render::DrawList>();
		REQUIRE(list != nullptr);

		// And it reaches the draw list, which is the half `Visual` alone does
		// not prove - the renderer reads the copy, not the component.
		const auto pane_drawn = std::find_if(
			list->Instances.begin(), list->Instances.end(), [](const engine::scene::DrawInstance &instance) {
				return instance.Surface == 0;
			}
		);
		CHECK(pane_drawn != list->Instances.end());
	});
}

// --- authoring a transform without a tick ------------------------------------

TEST_CASE("a part moved without a tick is drawn where it was moved to", "[client][presentation]") {
	// **The editor's whole case, and it was drawing every part in the wrong
	// place.** `World::Present` runs `PreRender` alone; `capture-previous` is a
	// `PreSimulation` system, so a *suspended* world - which is what the studio
	// shows while you author - never updates `PreviousTransform`. The draw list
	// interpolates from that stale value toward the current one, and a suspended
	// world's alpha does not advance either, so a part dragged in the properties
	// panel stayed exactly where it started while its selection outline - which
	// reads `Transform` directly - moved away from it.
	//
	// Two parts of the frame disagreeing about where something is reads as a
	// renderer fault, which is the most expensive kind of wrong place to look.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.authored");
	AddPart(universe, world, "Dragged");

	// A first frame, so the entity has been presented at least once.
	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const Entity part = InScene(store, "Dragged");
		REQUIRE(part != engine::ecs::NULL_ENTITY);

		const Vector3 moved{12.0f, 3.0f, -5.0f};
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// **Alpha one, which is what a world that is not simulating has to be
	// presented at.** There is nothing to interpolate *towards* when no tick is
	// coming; the current transform is the whole truth. `Editor::Present` passes
	// this for a suspended world.
	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<engine::render::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Frame.Position.X == Catch::Approx(12.0f));
		CHECK(list->Instances[0].Frame.Position.Y == Catch::Approx(3.0f));
		CHECK(list->Instances[0].Frame.Position.Z == Catch::Approx(-5.0f));
	});
}

TEST_CASE("a scripted move still interpolates across a tick", "[client][presentation]") {
	// **The case that refuted the obvious fix.** Making an authored write clear
	// `PreviousTransform` - "a teleport, not a simulation step" - reads well and
	// breaks every scripted animation in the engine: `examples/Rings.luau` sets
	// `CFrame` once a tick and relies on the draw list interpolating between
	// ticks, which is what buys smooth motion at 300 frames a second over a
	// 60 Hz simulation. Clearing it turns all of that into stepped motion at the
	// tick rate.
	//
	// So a property write moves `Transform` and nothing else, and the editor's
	// problem - a suspended world whose previous frame is never captured - is
	// fixed by presenting such a world at alpha one instead. `PlaceInstance`
	// carries both halves.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.interpolated");
	AddPart(universe, world, "Animated");

	universe.Present(world, 1.0f / 60.0f, 1.0f);

	universe.Enter(world, [](Store &store) {
		const Entity part = InScene(store, "Animated");
		const Vector3 moved{40.0f, 0.0f, 0.0f};
		REQUIRE(store.SetProperty(part, Name("Position"), &moved, sizeof(moved)));
	});

	// Halfway between the frame it was at and the frame it was put at.
	universe.Present(world, 1.0f / 60.0f, 0.5f);

	universe.Enter(world, [](Store &store) {
		const auto *list = store.Resource<engine::render::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Frame.Position.X == Catch::Approx(20.0f));
	});
}

// --- derived state a world that never ticks still needs -----------------------
//
// **Three passes had the same bug and one of them shipped as a visible fault.**
// A studio in Edit mode never ticks: `Editor::Simulate` returns before
// `Universe::Tick`, and `World::Present` runs `PreRender` alone. So a pass
// registered in `PreSimulation` does not run at all while somebody is
// authoring - and everything downstream of it reads whatever the component was
// last left holding, which for something the editor just made is the type's
// default.
//
// `PreviousTransform` was the one that got noticed, because "the part draws at
// the origin" is impossible to miss. These two are the same shape and were
// quieter: a material that does nothing until you press Play, and a lamp that
// lights the origin instead of the part it hangs off.
//
// The tests present without ever ticking, which is exactly what the studio does.

TEST_CASE("a material assigned without a tick reaches the draw list", "[client][presentation]") {
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.material");
	AddPart(universe, world, "Crate");

	const Name asset("materials/oak.amat");
	const Name colour("materials/oak_Color.atex");
	const Name metalness("materials/oak_Metalness.atex");

	universe.Enter(world, [&asset, &colour, &metalness](Store &store) {
		REQUIRE(
			engine::scene::RecordMaterial(
				store, asset, engine::scene::MaterialMaps{.Colour = colour, .Metalness = metalness}
			)
		);

		const Entity crate = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Crate");
		const Entity material = store.CreateInstance(engine::scene::MaterialClass(), "Oak");
		REQUIRE(store.SetParent(material, crate));

		auto *ref = store.GetMutable<engine::scene::MaterialRef>(material);
		REQUIRE(ref != nullptr);
		ref->Asset = asset;
	});

	REQUIRE(Drawn(universe, world) == 1);

	universe.Enter(world, [&colour, &metalness](Store &store) {
		// The component the resolve pass writes...
		const Entity crate = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Crate");
		CHECK(store.Get<engine::scene::SurfaceAppearance>(crate)->ColourMap == colour);
		CHECK(store.Get<engine::scene::SurfaceAppearance>(crate)->MetalnessMap == metalness);

		// ...and the copy of it the renderer actually samples, which is the half
		// that decides whether anything looks different on screen.
		const auto *list = store.Resource<engine::render::DrawList>();
		REQUIRE(list != nullptr);
		REQUIRE(list->Instances.size() == 1);
		CHECK(list->Instances[0].Texture == colour);
		CHECK(list->Instances[0].MetalnessMap == metalness);
	});
}

TEST_CASE("a light on an attachment is placed without a tick", "[client][presentation]") {
	// `Attachment::WorldFrame` is a cache with one writer, and `CollectLights`
	// reads it rather than walking the hierarchy per lamp. Unresolved, it is the
	// identity - so every lamp in an edited world lit the origin, whatever it
	// was actually parented to.
	Universe universe;
	const WorldId world = AddWorld(universe, "presentation.lamp");
	AddPart(universe, world, "Post");

	const Vector3 stood{10.0f, 0.0f, -4.0f};
	const Vector3 raised{0.0f, 6.0f, 0.0f};

	universe.Enter(world, [&stood, &raised](Store &store) {
		const Entity post = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Post");
		REQUIRE(store.SetProperty(post, Name("Position"), &stood, sizeof(stood)));

		const Entity point = store.CreateInstance(engine::ecs::Classes::Find(Name("Attachment")), "Top");
		REQUIRE(store.SetParent(point, post));
		store.GetMutable<engine::scene::Attachment>(point)->Frame = engine::core::CFrame(raised);

		const Entity bulb = store.CreateInstance(engine::ecs::Classes::Find(Name("PointLight")), "Bulb");
		REQUIRE(store.SetParent(bulb, point));
	});

	universe.Present(world, 1.0f / 60.0f, 1.0f);

	std::vector<engine::render::SceneLight> lights;
	universe.Enter(world, [&lights](Store &store) {
		CHECK(engine::render::CollectLights(store, Vector3{}, lights) == 1);
	});

	REQUIRE(lights.size() == 1);
	CHECK(lights[0].Position.X == Catch::Approx(stood.X + raised.X));
	CHECK(lights[0].Position.Y == Catch::Approx(stood.Y + raised.Y));
	CHECK(lights[0].Position.Z == Catch::Approx(stood.Z + raised.Z));
}

namespace {
	// A pair of panes facing each other across a hundred units, each a portal
	// into the other. The arrangement every portal example builds.
	//
	// @param store  The world.
	// @param apart  How far the second pane is down +X from the first.
	// @param second      Whether to make the far pane a portal back, which is what
	//                    gives the first one a partner.
	// @param assignSlots Whether the fixture stands in for the local aim pass.
	void MakePortalPair(Store &store, float apart, bool second, bool assignSlots = true) {
		const Entity services = engine::scene::InstallServices(store);

		const auto pane = [&](std::string_view name, const Vector3 &at) {
			const Entity part = store.CreateInstance(engine::scene::PartClass(), name);
			store.SetParent(part, services);
			store.Set(part, engine::scene::Transform{engine::core::CFrame{at}});
			store.Set(part, engine::scene::Bounds{Vector3{2.0f, 3.0f, 0.25f}});
			return part;
		};

		const Entity near = pane("Near", Vector3::Zero);
		const Entity far = pane("Far", Vector3{apart, 0.0f, 0.0f});

		const auto hole = [&](std::string_view name, Entity on, Entity to, int8_t slot) {
			const Entity camera =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), name);
			engine::scene::SurfaceCamera target;
			if (assignSlots) {
				target.Surface = slot;
			}
			store.Set(camera, target);

			engine::scene::Portal portal;
			portal.Destination = to;
			store.Set(camera, portal);

			store.SetParent(camera, on);
		};

		hole("NearHole", near, far, 0);
		if (second) {
			hole("FarHole", far, near, 1);
		}
	}
}

TEST_CASE("new portals claim no render slot before the local aim pass", "[client][presentation]") {
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) { MakePortalPair(store, 100.0f, true, false); });

	// A server snapshot can deliver several authored portals before this
	// client's viewer exists. Their slots are derived from that local viewer,
	// so none may masquerade as slot zero while the aim pass has no answer.
	std::vector<engine::render::PortalView> portals;
	universe.Enter(here, [&portals](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 0);
	});
	CHECK(portals.empty());
}

TEST_CASE("a same-world hole leaves the surface path for the recursive one", "[client][presentation]") {
	// **The pivot, stated as a test.** A `SurfaceCamera` is placed from the eye,
	// so when one surface pass draws another pane it projects that pane's image
	// with a matrix taken from the eye rather than from the camera the pass is
	// rendering from - the wrong viewpoint, not a stale one. A same-world portal
	// is therefore drawn by `render::PortalView` and must *not* also arrive as a
	// `SurfaceView`, or the pane is drawn twice and the second answer is wrong.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) { MakePortalPair(store, 100.0f, true); });

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	std::vector<engine::render::SurfaceView> views;

	universe.Enter(here, [&portals, &views](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 2);
		(void)client::CollectSurfaceViews(store, views, portals);
	});

	REQUIRE(portals.size() == 2);
	CHECK(views.empty());

	// Each hole names the other, so the level one opens can skip the pane it is
	// standing at - CodeParade's `skipPortal`.
	CHECK(portals[0].Partner == portals[1].Index);
	CHECK(portals[1].Partner == portals[0].Index);

	// **The rectangle is the seam's, so it is the pane's *face* and not its
	// box.** A quarter of a unit off the part's centre is the half-extent along
	// the face normal, which is what `FaceOf` measures and what the sub-camera's
	// clip plane is put on.
	const engine::render::PortalView &first = portals[0];
	const engine::render::PortalView &second = portals[1];
	CHECK(first.Centre.X == Catch::Approx(0.0f).margin(0.01f));
	CHECK(std::abs(first.Centre.Z) == Catch::Approx(0.25f));
	CHECK(std::abs(first.Normal.Z) == Catch::Approx(1.0f));

	// **The defining property of the map**: the source pane's centre lands on
	// the destination's. Everything the pass does - where the sub-camera stands,
	// where its near plane is skewed to - follows from it.
	const Vector3 landed = first.Warp.Point(first.Centre);
	CHECK(landed.X == Catch::Approx(second.Centre.X).margin(0.01f));
	CHECK(landed.Y == Catch::Approx(second.Centre.Y).margin(0.01f));
	CHECK(landed.Z == Catch::Approx(second.Centre.Z).margin(0.01f));

	// Two panes of one size, so the hole tells no lie about it.
	CHECK(first.Warp.Scale == Catch::Approx(1.0f));

	// **A lap through and back returns you, from either side**, which is what
	// makes it a hole rather than a one-way door. One map per pane is what buys
	// that: a pane's map and its partner's are exact inverses, so the lap closes
	// no matter which face was entered. The pair of side-picked maps this used to
	// carry both landed on the same side of the far pane and were therefore not
	// inverses - a lap that started from behind came back displaced and turned by
	// whatever angle the pair turns through.
	const Vector3 eye{0.0f, 1.0f, 5.0f};
	const Vector3 there = first.Warp.Point(eye);

	// A hundred units along X, because that is where the far room is. **Measured
	// on the axis the pair is laid out on rather than as a distance**, so the
	// case says "the other room" and not "and this far into it" - how far into it
	// is the round trip's business, checked below.
	CHECK(std::abs(there.X - eye.X) == Catch::Approx(100.0f).margin(0.5f));

	const Vector3 home = second.Warp.Point(there);
	CHECK(home.X == Catch::Approx(eye.X).margin(0.01f));
	CHECK(home.Y == Catch::Approx(eye.Y).margin(0.01f));
	CHECK(home.Z == Catch::Approx(eye.Z).margin(0.01f));

	// And the same lap from the pane's other face, which is the case the old
	// arrangement got wrong.
	const Vector3 behind{0.0f, 1.0f, -5.0f};
	const Vector3 across = first.Warp.Point(behind);
	const Vector3 back = second.Warp.Point(across);
	CHECK(back.X == Catch::Approx(behind.X).margin(0.01f));
	CHECK(back.Y == Catch::Approx(behind.Y).margin(0.01f));
	CHECK(back.Z == Catch::Approx(behind.Z).margin(0.01f));
}

TEST_CASE("a hole with no partner still recurses, and a lone pane has none", "[client][presentation]") {
	// A one-way hole is a real arrangement - a pane leading into a room with no
	// pane back - and it must still be drawn recursively. What it has no answer
	// for is which slot the level below should skip, and -1 is that answer
	// rather than a slot number that happens to be zero.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) { MakePortalPair(store, 60.0f, false); });

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	universe.Enter(here, [&portals](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 1);
	});

	REQUIRE(portals.size() == 1);
	CHECK(portals[0].Partner == -1);
}

TEST_CASE("a disabled portal mouth leaves no capture or mirror behind", "[client][presentation]") {
	Universe universe;
	const WorldId here = AddWorld(universe, "here");

	universe.Enter(here, [](Store &store) {
		MakePortalPair(store, 60.0f, true);

		bool disabled = false;
		store.Each<engine::scene::Portal>([&](Entity, engine::scene::Portal &portal) {
			if (!disabled) {
				portal.Enabled = false;
				disabled = true;
			}
		});
		REQUIRE(disabled);
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	std::vector<engine::render::SurfaceView> mirrors;
	universe.Enter(here, [&portals, &mirrors](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 1);
		CHECK(client::CollectSurfaceViews(store, mirrors, portals) == 0);
	});

	REQUIRE(portals.size() == 1);
	CHECK(portals[0].Partner == -1);
	CHECK(mirrors.empty());
}

TEST_CASE("a cross-world pane keeps its surface camera", "[client][presentation]") {
	// The authored surface camera remains available to the cross-world image adapter.
	Universe universe;
	const WorldId here = AddWorld(universe, "here");
	(void)AddWorld(universe, "there");

	universe.Enter(here, [](Store &store) {
		const Entity services = engine::scene::InstallServices(store);

		const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
		store.SetParent(pane, services);

		const Entity stand = store.CreateInstance(engine::scene::PartClass(), "StandIn");
		store.SetParent(stand, services);

		const Entity camera = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
		engine::scene::SurfaceCamera surface;
		surface.Surface = 0;
		store.Set(camera, surface);

		engine::scene::Portal portal;
		portal.Destination = stand;
		portal.DestinationWorld = Name("there");
		store.Set(camera, portal);

		store.SetParent(camera, pane);
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);

	std::vector<engine::render::PortalView> portals;
	std::vector<engine::render::SurfaceView> views;
	universe.Enter(here, [&portals, &views](Store &store) {
		CHECK(client::CollectPortalViews(store, portals) == 0);
		(void)client::CollectSurfaceViews(store, views, portals);
	});

	CHECK(portals.empty());
	CHECK_FALSE(views.empty());
}

TEST_CASE("cross-world foreground clones preserve both directions", "[client][presentation]") {
	Universe universe;

	const WorldId here = AddWorld(universe, "two.mouths.here");
	const WorldId there = AddWorld(universe, "two.mouths.there");

	// One pane at the origin with its `Front` face at z = -0.2, a stand-in the
	// same size so the hole does not change scale, and a body standing in the
	// pane. The stand-ins are a hundred units apart in opposite directions, so
	// a clone's position says on its own which mouth produced it.
	const auto build = [&universe](WorldId world, std::string_view other, int8_t slot, float standAtX) {
		universe.Enter(world, [other, slot, standAtX](Store &store) {
			const Entity services = engine::scene::InstallServices(store);

			const Vector3 paneSize{10.0f, 8.0f, 0.4f};

			const Entity pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
			store.SetProperty(pane, Name("Size"), &paneSize, sizeof(paneSize));
			store.SetParent(pane, services);

			const Entity stand = store.CreateInstance(engine::scene::PartClass(), "StandIn");
			const Vector3 standAt{standAtX, 0.0f, 0.0f};
			store.SetProperty(stand, Name("Size"), &paneSize, sizeof(paneSize));
			store.SetProperty(stand, Name("Position"), &standAt, sizeof(standAt));
			store.SetParent(stand, services);

			const Entity camera =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
			engine::scene::SurfaceCamera target;
			target.Surface = slot;
			store.Set(camera, target);

			engine::scene::Portal portal;
			portal.Destination = stand;
			portal.DestinationWorld = Name(other);
			store.Set(camera, portal);
			store.SetParent(camera, pane);

			// **Standing in the pane, which is not a crossing.** The body has
			// not moved at all - `PreviousTransform` is where it is - so the
			// interpolation cannot move the clone whatever the frame's alpha.
			const Entity body = store.CreateInstance(engine::scene::PartClass(), "Body");
			const Vector3 bodySize{1.0f, 2.0f, 1.0f};
			const Vector3 bodyAt{0.0f, 0.0f, -0.1f};
			store.SetProperty(body, Name("Size"), &bodySize, sizeof(bodySize));
			store.SetProperty(body, Name("Position"), &bodyAt, sizeof(bodyAt));
			store.SetParent(body, services);
			store.Set(body, engine::scene::Motion{});
			store.Set(body, engine::scene::PreviousTransform{engine::core::CFrame(bodyAt)});
		});
	};

	build(here, "two.mouths.there", 2, 100.0f);
	build(there, "two.mouths.here", 5, -100.0f);

	universe.Tick(1.0f / 60.0f);
	universe.Present(here, 1.0f / 60.0f, 0.0f);
	universe.Present(there, 1.0f / 60.0f, 0.0f);

	// What each world published for itself, which is what the two lists below
	// are measured against.
	const auto publishedBy = [&universe](WorldId world) {
		size_t count = 0;
		universe.Enter(world, [&count](Store &store) {
			if (const auto *list = store.Resource<engine::render::DrawList>()) {
				count = list->Instances.size();
			}
		});
		return count;
	};

	const size_t ownHere = publishedBy(here);
	const size_t ownThere = publishedBy(there);
	REQUIRE(ownHere > 0);
	REQUIRE(ownThere > 0);

	// The far half of a clone, found by the one thing that identifies it: it is
	// the only row a hundred units out along X.
	const auto cloneAt = [](const std::vector<engine::scene::DrawInstance> &rows, float x) {
		size_t found = 0;
		for (const engine::scene::DrawInstance &row : rows) {
			if (std::abs(row.Frame.Position.X - x) > 0.001f) {
				continue;
			}
			found++;

			// **The same depth into the far pane as into the near one**, which
			// is what makes the two halves meet at the plane rather than
			// overlap or leave a gap - mirrored across it, because the map
			// carries a pane's front hemisphere to the far pane's back one.
			CHECK(row.Frame.Position.Z == Catch::Approx(-0.3f).margin(0.001f));

			// **Never a surface itself**, or the copy would claim the slot its
			// original writes and the two would fight over one texture.
			CHECK(row.Surface == -1);
		}
		return found;
	};

	SECTION("with authored surface slots") {}
	SECTION("before viewport surface slots are assigned") {
		for (const auto world : {here, there}) {
			universe.Enter(world, [](Store &store) {
				for (auto &row : store.ResourceMutable<engine::render::DrawList>()->Instances) {
					row.Surface = -1;
				}
			});
		}
	}
	for (const auto world : {here, there}) {
		std::vector<engine::scene::DrawInstance> drawn;
		universe.Enter(world, [&](Store &store) {
			drawn = store.Resource<engine::render::DrawList>()->Instances;
		});
		const size_t own = drawn.size();
		CHECK(client::AppendForeignPortalClones(universe, world, drawn) == 1);
		CHECK(drawn.size() == own + 1);
		CHECK(cloneAt(drawn, world == here ? -100.0f : 100.0f) == 1);
		CHECK(drawn.back().SourceWorld == universe.NameOf(world == here ? there : here));
	}
}

TEST_CASE("foreground portal bodies come from the viewer's replica", "[client][presentation]") {
	engine::replication::RegisterReplicationComponents();
	Universe universe;

	// Two rooms and one viewer's copy of each, named the way a play link names
	// them.
	const WorldId here = AddWorld(universe, "xworld.near");
	const WorldId there = AddWorld(universe, "xworld.far");
	const WorldId hereSeen = AddWorld(universe, "xworld.near (client 1)");
	const WorldId thereSeen = AddWorld(universe, "xworld.far (client 1)");

	const auto mirrors = [&universe](WorldId replica, std::string_view of) {
		universe.Enter(replica, [of](Store &store) {
			store.SetResource(engine::world::Replica{true, Name(of), Name("client 1")});
		});
	};
	mirrors(hereSeen, "xworld.near");
	mirrors(thereSeen, "xworld.far");

	// One authored scene, built into all four. That is what a replica holds: a
	// copy of what the author wrote, naming the worlds the author named.
	//
	// `tintPercent` is the one thing that differs, so a row in the picture says
	// which of the four rooms produced it.
	const auto furnish = [&universe](WorldId world, std::string_view other, float tintPercent) {
		universe.Enter(world, [other, tintPercent](Store &store) {
			const Entity workspace = engine::scene::InstallServices(store);

			engine::scene::PartDesc slab;
			slab.Size = Vector3{10.0f, 8.0f, 0.4f};
			slab.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, 0.0f});
			slab.Simulated = false;
			const Entity block = engine::scene::MakePart(store, slab);
			store.SetInstanceName(block, "PortalBlock");
			store.SetParent(block, workspace);

			engine::scene::PartDesc stand;
			stand.Size = slab.Size;
			stand.Frame = engine::core::CFrame(Vector3{0.0f, 4.0f, -0.6f});
			stand.Simulated = false;
			const Entity beyond = engine::scene::MakePart(store, stand);
			store.SetParent(beyond, workspace);
			if (auto *look = store.GetMutable<engine::scene::Visual>(beyond)) {
				look->Transparency = 1.0f;
			}

			// A body straddling the pane contributes its near-side half.
			engine::scene::PartDesc body;
			body.Size = Vector3{2.0f, 5.0f, 2.0f};
			body.Frame = engine::core::CFrame(Vector3{0, 2.5f, -.1f});
			body.Simulated = false;
			const Entity marker = engine::scene::MakePart(store, body);
			store.SetInstanceName(marker, "Occupant");
			auto visual = *store.Get<engine::scene::Visual>(marker);
			visual.Tint = {tintPercent / 100.0f, 0, 0};
			store.Set(marker, visual);
			store.SetParent(marker, workspace);

			const Entity eye = store.CreateInstance(engine::ecs::Classes::Find(Name("Camera")), "Eye");
			store.Set(eye, engine::scene::Transform{engine::core::CFrame(Vector3{0.0f, 5.0f, 16.0f})});
			store.SetResource(engine::scene::ActiveCamera{eye, 16.0f / 9.0f});

			const Entity hole =
				store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Hole");
			store.Set(hole, engine::scene::SurfaceCamera{});

			engine::scene::Portal portal;
			portal.Destination = beyond;
			portal.DestinationWorld = Name(other);
			store.Set(hole, portal);
			store.SetParent(hole, block);
		});
	};

	// The authored names on both sides, which is the whole fixture: no world
	// names a replica, because no author can.
	furnish(here, "xworld.far", 10.0f);
	furnish(there, "xworld.near", 20.0f);
	furnish(hereSeen, "xworld.far", 30.0f);
	furnish(thereSeen, "xworld.near", 40.0f);
	for (const WorldId replica : {hereSeen, thereSeen}) {
		universe.Enter(replica, [](Store &store) {
			engine::replication::SnapshotBuffer received;
			received.RecordTick(1);
			store.SetResource(std::move(received));
		});
	}

	for (const WorldId world : {here, there, hereSeen, thereSeen}) {
		universe.Enter(world, [](Store &store) { (void)engine::scene::AimSurfaceCameras(store); });
		universe.Present(world, 1.0f / 60.0f, 0.0f);
	}

	std::vector<engine::scene::DrawInstance> drawn;
	universe.Enter(hereSeen, [&](Store &store) {
		drawn = store.Resource<engine::render::DrawList>()->Instances;
	});
	const size_t own = drawn.size();
	REQUIRE(client::AppendForeignPortalClones(universe, hereSeen, drawn) == 1);
	REQUIRE(drawn.size() == own + 1);
	CHECK(drawn.back().SourceWorld == universe.NameOf(thereSeen));
	CHECK(drawn.back().Tint.R == Catch::Approx(.4f));
}
