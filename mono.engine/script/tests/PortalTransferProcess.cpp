#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

TEST_SUITE_ID("engine.script.portaltransferprocess")
TEST_DEPENDS("engine.script.portaltransfer")
TEST_DEPENDS("engine.world.hostlink")

using namespace engine;

namespace {
	bool Await(world::HostLink &link, std::vector<world::HostFrame> &frames) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (link.Receive(frames) != 0) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
	world::WorldSettings Settings(const char *name) {
		world::WorldSettings settings;
		settings.Name = core::Name(name);
		settings.TickRate = 60;
		return settings;
	}
}

TEST_CASE("portal transfer process child", "[.portal-transfer-child]") {
	auto channel = parallel::AdoptInheritedChannel();
	REQUIRE(channel != nullptr);
	world::HostLink link(std::move(channel), core::Name("portal-host"));
	world::UniverseSettings settings;
	settings.Federated = true;
	world::Universe worlds(settings);
	scene::RegisterSceneClasses();
	const auto destination = worlds.Create(Settings("destination"));
	worlds.Enter(destination, [](ecs::Store &store, ecs::Scheduler &scheduler) {
		scene::InstallServices(store);
		REQUIRE(script::ConfigurePortalTransfers(store, 202));
		script::RegisterTeleportAdmission(scheduler);
	});
	bool finished = false;
	while (!finished) {
		std::vector<world::HostFrame> frames;
		REQUIRE(Await(link, frames));
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Stop) {
				finished = true;
				break;
			}
			if (frame.Signal == world::HostSignal::Deliveries) {
				for (const auto &delivery : frame.Deliveries)
					REQUIRE(worlds.Deliver(delivery.World, delivery.Message));
			}
			if (frame.Signal != world::HostSignal::Heartbeat) continue;
			worlds.Tick(1.0f / 60);
			world::HostFrame reply;
			reply.Signal = world::HostSignal::Traffic;
			reply.Tick = frame.Tick;
			reply.Traffic.assign(worlds.LastTraffic().begin(), worlds.LastTraffic().end());
			REQUIRE(link.Send(reply));
		}
	}
	worlds.Enter(destination, [](ecs::Store &store) {
		const auto object = script::PortalTransferObject(store, {"source", 101, 2});
		REQUIRE(object != ecs::NULL_ENTITY);
		CHECK(store.Get<scene::Motion>(object)->Linear == core::Vector3{6, 8, 10});
		CHECK(store.Get<scene::Motion>(object)->Angular == core::Vector3{1, 2, 3});
		CHECK(store.Get<scene::Transform>(object)->Frame.Position == core::Vector3{12, 24, 36});
		CHECK(store.Get<scene::Collider>(object)->Extent == core::Vector3{1, 1, 1});
		const script::PortalTransferId id{"source", 101, 1};
		const auto player = script::PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 1);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->Health == 57);
		CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->MoveDirection == core::Vector3{0, 0, 1});
		CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->JumpRequested);
		CHECK(store.Get<scene::Motion>(rig.Root)->Linear == core::Vector3{3, 4, -8});
		CHECK(store.Get<scene::Motion>(rig.Root)->Angular == core::Vector3{1, 2, 3});
		CHECK(store.Get<scene::Transform>(rig.Root)->Frame.Position == core::Vector3{100, 12.5f, 20});
		CHECK(store.Get<scene::NetworkOwner>(rig.Root)->Player == player);
		const auto hat = store.FindFirstChild(scene::CharacterOf(store, player), "hat");
		REQUIRE(hat != ecs::NULL_ENTITY);
		const auto points = *store.Get<scene::Accessory>(hat);
		REQUIRE(store.ParentOf(points.CharacterAttachment) == rig.Root);
		const auto handle = store.ParentOf(points.HandleAttachment);
		REQUIRE(store.Get<scene::CharacterLimb>(handle)->Root == rig.Root);
		REQUIRE_FALSE(store.Has<scene::Motion>(handle));
		scene::PoseCharacters(store);
		CHECK(
			(scene::ResolveAttachment(store, points.HandleAttachment).Position -
			 scene::ResolveAttachment(store, points.CharacterAttachment).Position)
				.Magnitude() < .0001f
		);

		const auto animator = scene::AnimatorFor(store, rig.Root);
		REQUIRE(animator != ecs::NULL_ENTITY);
		bool hasTrack = false;
		store.EachDescendant(animator, [&](ecs::Entity entity) {
			const auto *track = store.Get<scene::AnimationTrack>(entity);
			if (!track) return;
			CHECK(track->TimePosition == 2.25f);
			CHECK(track->Weight == .4f);
			CHECK(track->Playing);
			const auto *clip = store.Get<scene::AnimationClip>(track->Clip);
			REQUIRE(clip != nullptr);
			CHECK(clip->Asset == core::Name("shared.animation"));
			const auto *buffer = store.Get<scene::AnimationBuffer>(clip->Buffer);
			REQUIRE(buffer != nullptr);
			CHECK(buffer->Data == std::vector<std::byte>{std::byte{1}, std::byte{2}});
			hasTrack = true;
		});
		REQUIRE(hasTrack);
	});
}

TEST_CASE(
	"actual portal rig and object commit through authenticated host traffic in a separate process",
	"[script][portal-transfer][process]"
) {
	scene::RegisterSceneClasses();
	auto channels = parallel::MakeProcessChannel();
	REQUIRE(channels.Valid());
	parallel::Process child;
	REQUIRE(child.Start(
		core::Paths::Base() / core::Paths::Program("test_script"),
		{"portal transfer process child"},
		std::move(channels.Remote)
	));
	world::HostLink link(std::move(channels.Local), core::Name("driver"));
	world::Universe worlds;
	const auto source = worlds.Create(Settings("source"));
	REQUIRE(worlds.CreateRemote(Settings("destination"), core::Name("portal-host")).IsValid());
	ecs::Entity player, object, sharedClip, sharedBuffer;
	worlds.Enter(source, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
		scene::InstallServices(store);
		REQUIRE(script::ConfigurePortalTransfers(store, 101));
		script::RegisterTeleportAdmission(scheduler);
		player = scene::AddPlayer(store, "duplicate display label", false, 999);
		const auto model = scene::LoadCharacter(store, player);
		const auto rig = *store.Get<scene::Character>(model);
		const auto hat = store.CreateInstance(scene::AccessoryClass(), "hat");
		const auto handle = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "Handle");
		store.SetParent(handle, hat);
		const auto headPoint = store.CreateInstance(scene::AttachmentClass(), "HatAttachment");
		store.SetParent(headPoint, rig.Root);
		store.Set(headPoint, scene::Attachment{core::CFrame({0, 2, 0}), {}});
		const auto hatPoint = store.CreateInstance(scene::AttachmentClass(), "HatAttachment");
		store.SetParent(hatPoint, handle);
		store.Set(hatPoint, scene::Attachment{core::CFrame({0, .25f, 0}), {}});
		REQUIRE(scene::EquipAccessory(store, model, hat));
		scene::PoseCharacters(store);
		scene::ResolveAttachments(store);

		store.GetMutable<scene::Humanoid>(rig.Humanoid)->Health = 57;
		store.Set(rig.Root, scene::Motion{{3, 4, -8}, {1, 2, 3}});
		store.Set(rig.Root, scene::Skeleton{});
		const auto animator = store.CreateInstance(scene::AnimatorClass(), "animator");
		store.SetParent(animator, rig.Root);
		scene::Animator driver;
		driver.Rig = rig.Root;
		store.Set(animator, driver);
		sharedClip = store.CreateInstance(ecs::Classes::Find(core::Name("Animation")), "shared clip");
		sharedBuffer =
			store.CreateInstance(ecs::Classes::Find(core::Name("AnimationBuffer")), "shared buffer");
		store.Set(sharedBuffer, scene::AnimationBuffer{{std::byte{1}, std::byte{2}}, 7});
		store.Set(sharedClip, scene::AnimationClip{core::Name("shared.animation"), {}, sharedBuffer});
		const auto track = store.CreateInstance(ecs::Classes::Find(core::Name("AnimationTrack")), "track");
		store.SetParent(track, animator);
		scene::AnimationTrack playing;
		playing.Clip = sharedClip;
		playing.TimePosition = 2.25f;
		playing.Weight = .4f;
		playing.Playing = true;
		store.Set(track, playing);

		object = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "cargo");
		store.SetParent(object, scene::WorkspaceOf(store));
		store.Set(object, scene::Transform{core::CFrame({1, 2, 3})});
		store.Set(object, scene::Motion{{3, 4, 5}, {1, 2, 3}});
		store.Set(object, scene::Simulated{});
	});
	const auto tick = [&](uint64_t serial) {
		worlds.Tick(1.0f / 60);
		std::vector<world::HostDelivery> deliveries;
		for (const auto &delivery : worlds.TakeOutbound())
			deliveries.push_back({delivery.World, delivery.Message});
		REQUIRE(link.SendDeliveries(deliveries));
		REQUIRE(link.Heartbeat(serial));
		bool answered = false;
		while (!answered) {
			std::vector<world::HostFrame> frames;
			REQUIRE(Await(link, frames));
			for (const auto &frame : frames) {
				REQUIRE(frame.Signal == world::HostSignal::Traffic);
				REQUIRE(frame.Tick == serial);
				REQUIRE(
					worlds.IngestTraffic(core::Name("portal-host"), frame.Traffic) == frame.Traffic.size()
				);
				answered = true;
			}
		}
	};
	for (uint64_t serial = 0; serial < 5; ++serial)
		tick(serial);
	worlds.Enter(source, [&](ecs::Store &store) {
		script::PortalTransferId id;
		std::string failure;
		REQUIRE(
			script::BeginPortalTransfer(
				store, player, "destination", {core::CFrame({100, 10, 20}), {}, 1}, id, failure
			)
		);
		REQUIRE(
			script::BeginPortalObjectTransfer(
				store, object, "destination", {core::CFrame({10, 20, 30}), {}, 2}, id, failure
			)
		);
		REQUIRE(id.Sequence == 2);
		REQUIRE(script::ForwardPortalPlayerMove(store, player, {1, 0, 0}, false));
	});
	for (uint64_t serial = 5; serial < 18; ++serial)
		tick(serial);
	worlds.Enter(source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(player));
		REQUIRE(script::ForwardPortalPlayerMove(store, player, {0, 0, 1}, true));
	});
	for (uint64_t serial = 18; serial < 26; ++serial)
		tick(serial);
	worlds.Enter(source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(player));
		REQUIRE(store.Alive(sharedClip));
		REQUIRE(store.Alive(sharedBuffer));
		REQUIRE_FALSE(store.Alive(object));
		REQUIRE(
			script::PortalTransferOfObject(store, object)->Stage == script::PortalTransferStage::Committed
		);
		REQUIRE(
			script::PortalTransferOfPlayer(store, player)->Stage == script::PortalTransferStage::Committed
		);
	});
	world::HostFrame stop;
	stop.Signal = world::HostSignal::Stop;
	REQUIRE(link.Send(stop));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto ended = child.Poll();
	while (ended.Alive() && std::chrono::steady_clock::now() < deadline) {
		std::vector<world::HostFrame> ignored;
		link.Receive(ignored);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		ended = child.Poll();
	}
	REQUIRE_FALSE(ended.Alive());
	CHECK(ended.Reason == parallel::ExitReason::Exited);
	CHECK(ended.Code == 0);
}
