#include <engine/ecs/Classes.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/BodyMotion.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/script/PortalObservation.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.script.portaltransfer")
TEST_DEPENDS("engine.scene.portaltransfer")

using namespace engine;
using namespace engine::script;

namespace {
	struct Pair {
		world::Universe Worlds;
		world::WorldId Source;
		world::WorldId Destination;
		ecs::Entity Player;
		ecs::Entity Root;
		ecs::Entity Humanoid;
		explicit Pair(
			const world::UniverseSettings &settings = {},
			bool requireAdmission = false,
			PortalTransferDurability durability = PortalTransferDurability::InMemoryOnly
		)
			: Worlds(settings) {
			scene::RegisterSceneClasses();
			RegisterPortalTransferComponents();
			world::WorldSettings source;
			source.Name = core::Name("source");
			source.TickRate = 60;
			world::WorldSettings destination;
			destination.Name = core::Name("destination");
			destination.TickRate = 60;
			Source = Worlds.Create(source);
			Destination = Worlds.Create(destination);
			Worlds.Enter(Source, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
				scene::InstallServices(store);
				REQUIRE(ConfigurePortalTransfers(store, 101, requireAdmission, durability));
				RegisterTeleportAdmission(scheduler);
				Player = scene::AddPlayer(store, "shared label", false, 71);
				const auto model = scene::LoadCharacter(store, Player);
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

				Root = rig.Root;
				Humanoid = rig.Humanoid;
				store.Set(Root, scene::Motion{{3, 1, -8}, {2, 3, 4}});
				store.GetMutable<scene::Humanoid>(Humanoid)->Health = 23;
			});
			Worlds.Enter(Destination, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
				scene::InstallServices(store);
				REQUIRE(ConfigurePortalTransfers(store, 202, false, durability));
				RegisterTeleportAdmission(scheduler);
			});
			Tick(3);
		}
		void Tick(int count = 1) {
			for (int index = 0; index < count; ++index)
				Worlds.Tick(1.0f / 60);
		}
		PortalTransferId Begin(const char *destination = "destination") {
			PortalTransferId id;
			Worlds.Enter(Source, [&](ecs::Store &store) {
				std::string failure;
				const bool begun = BeginPortalTransfer(
					store, Player, destination, {core::CFrame({100, 10, 20}), {}, 1}, id, failure
				);
				INFO(failure);
				REQUIRE(begun);
			});
			return id;
		}
	};
}

TEST_CASE(
	"installed portal transfer retires source before admitting one actual destination rig",
	"[script][portal-transfer]"
) {
	Pair pair;
	const auto id = pair.Begin();
	REQUIRE(id.SourceWorld == "source");
	REQUIRE(id.SourceIncarnation == 101);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Alive(pair.Player));
		REQUIRE(store.Has<scene::Motion>(pair.Root));
		REQUIRE(store.Has<scene::Simulated>(pair.Root));
		REQUIRE(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
	});
	for (int tick = 0; tick < 8; ++tick) {
		pair.Tick();
		size_t authorities = 0;
		for (const auto world : {pair.Source, pair.Destination})
			pair.Worlds.Enter(world, [&](ecs::Store &store) {
				store.Each<const scene::Character>([&](ecs::Entity, const scene::Character &rig) {
					authorities += store.Has<scene::Simulated>(rig.Root);
				});
			});
		REQUIRE(authorities <= 1);
	}
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(pair.Player));
		const auto receipt = PortalTransferOfPlayer(store, pair.Player);
		REQUIRE(receipt.has_value());
		REQUIRE(receipt->Id == id);
		REQUIRE(receipt->Stage == PortalTransferStage::Committed);
	});
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 1);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		REQUIRE(store.Get<scene::Humanoid>(rig.Humanoid)->Health == 23);
		REQUIRE(store.Get<scene::Motion>(rig.Root)->Linear == core::Vector3{3, 1, -8});
		REQUIRE(store.Get<scene::Motion>(rig.Root)->Angular == core::Vector3{2, 3, 4});
		REQUIRE(store.Get<scene::Transform>(rig.Root)->Frame.Position == core::Vector3{100, 12.5f, 20});
	});
}

TEST_CASE(
	"portal seals the final H pose and velocity instead of its offer snapshot", "[script][portal-transfer]"
) {
	Pair pair;
	const auto id = pair.Begin();
	pair.Tick(2);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		store.Set(pair.Root, scene::Transform{core::CFrame({7, 8, 9})});
		store.Set(pair.Root, scene::Motion{{11, 12, 13}, {2, 4, 6}});
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		CHECK(store.Get<scene::Transform>(rig.Root)->Frame.Position == core::Vector3{107, 18, 29});
		CHECK(store.Get<scene::Motion>(rig.Root)->Linear == core::Vector3{11, 12, 13});
		CHECK(store.Get<scene::Motion>(rig.Root)->Angular == core::Vector3{2, 4, 6});
	});
}

TEST_CASE(
	"portal precommit refusal restores exact source motion and humanoid while budget refusal never fences",
	"[script][portal-transfer]"
) {
	Pair pair;
	pair.Begin("missing-world");
	pair.Tick(6);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Alive(pair.Player));
		REQUIRE(store.Has<scene::Simulated>(pair.Root));
		REQUIRE(store.Get<scene::Motion>(pair.Root)->Linear == core::Vector3{3, 1, -8});
		REQUIRE(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Refused);
		store.SetResource(world::BusBudget{0, 0});
		PortalTransferId id;
		std::string failure;
		REQUIRE_FALSE(BeginPortalTransfer(store, pair.Player, "destination", {}, id, failure));
		REQUIRE(store.Has<scene::Simulated>(pair.Root));
		REQUIRE(store.Has<scene::Motion>(pair.Root));
		REQUIRE(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
	});
}

TEST_CASE("portal durable decision waits for host journal acknowledgement", "[script][portal-transfer]") {
	Pair pair({}, false, PortalTransferDurability::RequirePrepareCommit);
	const auto id = pair.Begin();
	pair.Tick(8);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Alive(pair.Player));
		const auto pending = PortalTransferPendingDecisions(store);
		REQUIRE(pending.size() == 1);
		CHECK(pending.front().Receipt.Id == id);
		CHECK(pending.front().Receipt.Fence.BaselineId != 0);
		CHECK_FALSE(pending.front().Receipt.Fence.BaselineHash.IsZero());
		CHECK(pending.front().Body.Key.IsValid());
		CHECK_FALSE(MarkPortalTransferDurable(store, id, {}));
		REQUIRE(MarkPortalTransferDurable(store, id, pending.front().Receipt.Fence.BaselineHash));
		CHECK(MarkPortalTransferDurable(store, id, pending.front().Receipt.Fence.BaselineHash));
		CHECK(PortalTransferPendingDecisions(store).empty());
	});
	pair.Tick(20);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		CHECK_FALSE(store.Alive(pair.Player));
		CHECK(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Committed);
	});
}

TEST_CASE("portal durable acknowledgement reopens a sealed snapshot receipt", "[script][portal-transfer]") {
	Pair pair({}, false, PortalTransferDurability::RequirePrepareCommit);
	const auto id = pair.Begin();
	pair.Tick(8);
	core::ByteWriter snapshot;
	REQUIRE(pair.Worlds.Save(snapshot));

	world::Universe restored;
	core::ByteReader reader(snapshot.Bytes());
	REQUIRE(restored.Load(reader));
	for (const auto world : restored.Worlds())
		restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
			RegisterTeleportAdmission(scheduler);
		});
	const auto source = restored.Find(core::Name("source"));
	restored.Enter(source, [&](ecs::Store &store) {
		const auto pending = PortalTransferPendingDecisions(store);
		REQUIRE(pending.size() == 1);
		REQUIRE(pending.front().Receipt.Id == id);
		REQUIRE(MarkPortalTransferDurable(store, id, pending.front().Receipt.Fence.BaselineHash));
	});
	for (int tick = 0; tick < 20; ++tick)
		restored.Tick(1.0f / 60);
	restored.Enter(source, [&](ecs::Store &store) {
		const auto receipt = PortalTransferOfPlayer(store, pair.Player);
		REQUIRE(receipt);
		CHECK(receipt->Stage == PortalTransferStage::Committed);
	});
}

TEST_CASE(
	"portal journal replays a sealed decision into a snapshot before the crossing",
	"[script][portal-transfer]"
) {
	Pair pair({}, false, PortalTransferDurability::RequirePrepareCommit);
	core::ByteWriter before;
	REQUIRE(pair.Worlds.Save(before));
	const auto id = pair.Begin();
	pair.Tick(8);
	PortalTransferDecision decision;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto pending = PortalTransferPendingDecisions(store);
		REQUIRE(pending.size() == 1);
		decision = pending.front();
	});

	world::Universe restored;
	core::ByteReader reader(before.Bytes());
	REQUIRE(restored.Load(reader));
	for (const auto world : restored.Worlds())
		restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
			RegisterTeleportAdmission(scheduler);
		});
	const auto source = restored.Find(core::Name("source"));
	restored.Enter(source, [&](ecs::Store &store) {
		REQUIRE(RestorePortalTransferDecision(store, decision));
		CHECK(RestorePortalTransferDecision(store, decision));
		REQUIRE(MarkPortalTransferDurable(store, id, decision.Receipt.Fence.BaselineHash));
	});
	for (int tick = 0; tick < 24; ++tick)
		restored.Tick(1.0f / 60);
	restored.Enter(restored.Find(core::Name("destination")), [&](ecs::Store &store) {
		CHECK(scene::PlayerCount(store) == 1);
		CHECK(PortalTransferPlayer(store, id) != ecs::NULL_ENTITY);
	});
}

TEST_CASE(
	"player portal return trip keeps one live body through both committed handoffs",
	"[script][portal-transfer][portal-return]"
) {
	Pair pair;
	const auto outward = pair.Begin();
	pair.Tick(8);
	ecs::Entity destinationPlayer;
	core::Vector3 destinationPosition;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		destinationPlayer = PortalTransferPlayer(store, outward);
		REQUIRE(destinationPlayer != ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 1);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, destinationPlayer));
		destinationPosition = store.Get<scene::Transform>(rig.Root)->Frame.Position;
	});
	const scene::SeamTransform returnThrough{core::CFrame({-20, 4, 7}), {}, 2};
	PortalTransferId returnId;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		std::string failure;
		REQUIRE(BeginPortalTransfer(store, destinationPlayer, "source", returnThrough, returnId, failure));
	});
	for (int tick = 0; tick < 6; ++tick) {
		pair.Tick();
		size_t authorities = 0;
		for (const auto world : {pair.Source, pair.Destination})
			pair.Worlds.Enter(world, [&](ecs::Store &store) {
				store.Each<const scene::Character>([&](ecs::Entity, const scene::Character &rig) {
					authorities += store.Has<scene::Simulated>(rig.Root);
				});
			});
		CHECK(authorities <= 1);
	}
	pair.Tick(2);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		CHECK_FALSE(store.Alive(destinationPlayer));
		CHECK(scene::PlayerCount(store) == 0);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto returned = PortalTransferPlayer(store, returnId);
		REQUIRE(returned != ecs::NULL_ENTITY);
		CHECK(scene::PlayerCount(store) == 1);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, returned));
		CHECK(
			store.Get<scene::Transform>(rig.Root)->Frame.Position == returnThrough.Point(destinationPosition)
		);
		CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->Health == 23);
	});
}

TEST_CASE(
	"duplicate portal commit cannot admit a second body and snapshot replay resumes each handoff phase",
	"[script][portal-transfer]"
) {
	for (int phase = 0; phase < 4; ++phase) {
		Pair pair;
		const auto id = pair.Begin();
		pair.Tick(phase);
		core::ByteWriter snapshot;
		REQUIRE(pair.Worlds.Save(snapshot));
		world::Universe restored;
		core::ByteReader reader(snapshot.Bytes());
		REQUIRE(restored.Load(reader));
		for (auto world : restored.Worlds())
			restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
				RegisterTeleportAdmission(scheduler);
			});
		for (int tick = 0; tick < 7; ++tick) {
			pair.Tick();
			restored.Tick(1.0f / 60);
		}
		core::ByteWriter expected, actual;
		REQUIRE(pair.Worlds.Save(expected));
		REQUIRE(restored.Save(actual));
		INFO(phase);

		REQUIRE(std::ranges::equal(expected.Bytes(), actual.Bytes()));
		restored.Enter(restored.Find(core::Name("destination")), [&](ecs::Store &store) {
			REQUIRE(scene::PlayerCount(store) == 1);
			REQUIRE(PortalTransferPlayer(store, id) != ecs::NULL_ENTITY);
		});
	}
	Pair pair;
	const auto id = pair.Begin();
	pair.Tick(6);
	world::Envelope commit;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		auto *outbox = store.ResourceMutable<world::Outbox>();
		REQUIRE(outbox != nullptr);
		REQUIRE_FALSE(outbox->Pending.empty());
		commit = outbox->Pending.back();
		auto repeated = commit;
		repeated.Sequence = outbox->NextSequence++;
		outbox->Pending.push_back(repeated);
	});
	pair.Tick(4);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		REQUIRE(scene::PlayerCount(store) == 1);
		REQUIRE(PortalTransferPlayer(store, id) != ecs::NULL_ENTITY);
	});
}

TEST_CASE(
	"installed nearest foreign sweep transfers a moving rig and resolves the destination suffix wall",
	"[script][portal-transfer]"
) {
	Pair pair;
	pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) {
		physics::PreparePhysicsWorld(store);
		const auto wall = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "wall");
		store.Set(wall, scene::Transform{core::CFrame({100, 2.5f, -5})});
		scene::Collider collider;
		collider.Extent = {5, 5, .1f};
		collider.CanQuery = false;
		store.Set(wall, collider);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto pane = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "pane");
		store.Set(pane, scene::Transform{core::CFrame({0, 2.5f, 0})});
		store.Set(pane, scene::Bounds{{5, 5, .1f}});
		const auto beyond = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "beyond");
		store.Set(beyond, scene::Transform{core::CFrame::LookAt({100, 2.5f, 0}, {100, 2.5f, 1})});
		store.Set(beyond, scene::Bounds{{5, 5, .1f}});
		const auto surface = store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "portal");
		scene::SurfaceCamera camera;
		camera.Face = scene::NormalId::Back;
		camera.Surface = 0;
		store.Set(surface, camera);
		scene::Portal portal{beyond};
		portal.DestinationWorld = core::Name("destination");
		store.Set(surface, portal);
		store.SetParent(surface, pane);
		store.Set(pair.Root, scene::PreviousTransform{core::CFrame({0, 2.5f, 10})});
		store.Set(pair.Root, scene::Transform{core::CFrame({0, 2.5f, -10})});
		store.Set(pair.Root, scene::Motion{{0, 0, -1200}, {}});
	});
	pair.Tick();
	PortalTransferId id;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto receipt = PortalTransferOfPlayer(store, pair.Player);
		REQUIRE(receipt.has_value());
		id = receipt->Id;
		REQUIRE(store.Has<scene::NetworkOwner>(pair.Root));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		const auto position = store.Get<scene::Transform>(rig.Root)->Frame.Position;
		INFO(position.X << ", " << position.Y << ", " << position.Z);
		CHECK(position.X == Catch::Approx(100));
		CHECK(position.Z > -5);
		CHECK(position.Z < 0);
		CHECK(store.Get<scene::Motion>(rig.Root)->Linear.Z == Catch::Approx(-1200));
	});
}

TEST_CASE(
	"portal retries lost ready and done without duplicating or restoring committed authority",
	"[script][portal-transfer]"
) {
	Pair pair;
	const auto id = pair.Begin();
	SECTION("ready lost before commitment") {
		pair.Tick();
		pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) {
			store.ResourceMutable<world::Outbox>()->Pending.clear();
		});
	}
	SECTION("done lost after destination admission") {
		pair.Tick(6);
		pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) {
			store.ResourceMutable<world::Outbox>()->Pending.clear();
		});
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE_FALSE(store.Alive(pair.Player));
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Committing);
		});
	}
	pair.Tick(25);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(pair.Player));
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Committed);
	});
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		REQUIRE(scene::PlayerCount(store) == 1);
		REQUIRE(PortalTransferPlayer(store, id) != ecs::NULL_ENTITY);
	});
}

TEST_CASE(
	"portal transfer preserves a sleeping root through duplicate delivery and wakes it only on demand",
	"[script][portal-transfer]"
) {
	Pair pair;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		physics::PreparePhysicsWorld(store);
		REQUIRE(physics::SetSleeping(store, pair.Root, true));
		REQUIRE(physics::Sleeping(store, pair.Root));
		REQUIRE_FALSE(store.Has<scene::Motion>(pair.Root));
	});
	pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) { physics::PreparePhysicsWorld(store); });
	const auto id = pair.Begin();
	pair.Tick(2);
	pair.Worlds.Enter(pair.Source, [](ecs::Store &store) {
		auto *outbox = store.ResourceMutable<world::Outbox>();
		REQUIRE(outbox != nullptr);
		REQUIRE_FALSE(outbox->Pending.empty());
		auto repeated = outbox->Pending.back();
		repeated.Sequence = outbox->NextSequence++;
		outbox->Pending.push_back(std::move(repeated));
	});
	pair.Tick(30);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 1);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		REQUIRE(physics::Sleeping(store, rig.Root));
		REQUIRE_FALSE(store.Has<scene::Motion>(rig.Root));
		REQUIRE(physics::SetSleeping(store, rig.Root, false));
		CHECK_FALSE(physics::Sleeping(store, rig.Root));
		REQUIRE(store.Has<scene::Motion>(rig.Root));
	});
}

TEST_CASE(
	"portal source ignores a forged responder and a late refusal after commit", "[script][portal-transfer]"
) {
	Pair pair;
	pair.Begin();
	pair.Tick();
	world::Envelope ready;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
		REQUIRE_FALSE(pending.empty());
		ready = pending.back();
		pending.clear();
	});
	world::Delivery forged;
	forged.Bus = world::BusKind::Channel;
	forged.Key = core::Name("engine.portal.transfer");
	forged.From = core::Name("unrelated-world");
	forged.Payload = ready.Payload;
	REQUIRE(pair.Worlds.Deliver(core::Name("source"), forged));
	pair.Tick();
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Alive(pair.Player));
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Preparing);
	});
	forged.From = core::Name("destination");
	REQUIRE(pair.Worlds.Deliver(core::Name("source"), forged));
	pair.Tick(6);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) { REQUIRE_FALSE(store.Alive(pair.Player)); });
	forged.Payload[4] = std::byte{4}; // The wire discriminator is Refuse.
	REQUIRE(pair.Worlds.Deliver(core::Name("source"), forged));
	pair.Tick(4);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(pair.Player));
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Committed);
	});
}

TEST_CASE(
	"portal physical ownership is byte-identical across joined worker lanes and inline worlds",
	"[script][portal-transfer]"
) {
	struct Workers {
		Workers() {
			parallel::Jobs::Start(2);
		}
		~Workers() {
			parallel::Jobs::Stop();
		}
	} workers;
	world::UniverseSettings settings;
	settings.WorldParallelFloorMilliseconds = 0;
	Pair parallel(settings);
	settings.Mode = world::ExecutionMode::WorldSerial;
	Pair serial(settings);
	const auto first = parallel.Begin();
	const auto second = serial.Begin();
	REQUIRE(first == second);
	for (int tick = 0; tick < 8; ++tick) {
		parallel.Tick();
		serial.Tick();
	}
	parallel.Worlds.SetMode(world::ExecutionMode::WorldSerial);
	core::ByteWriter expected, actual;
	REQUIRE(serial.Worlds.Save(expected));
	REQUIRE(parallel.Worlds.Save(actual));
	REQUIRE(std::ranges::equal(expected.Bytes(), actual.Bytes()));
}

TEST_CASE(
	"ordinary portal object transaction preserves motion and shape across a scaled round trip",
	"[script][portal-transfer][portal-object]"
) {
	Pair pair;
	ecs::Entity object;
	PortalTransferId outward, inward;
	const scene::SeamTransform through{core::CFrame::LookAt({40, 20, 10}, {41, 20, 10}), {}, 2};
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		object = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "shared label");
		store.SetParent(object, scene::WorkspaceOf(store));
		store.Set(object, scene::Transform{core::CFrame({1, 2, 3})});
		store.Set(object, scene::Motion{{3, 4, 5}, {1, 2, 3}});
		store.Set(object, scene::Simulated{});
		scene::Collider shape;
		shape.Shape = scene::ShapeKind::Sphere;
		shape.Extent = {2, 0, 0};
		store.Set(object, shape);
		std::string failure;
		const bool begun = BeginPortalObjectTransfer(store, object, "destination", through, outward, failure);
		INFO(failure);
		REQUIRE(begun);
		REQUIRE(store.Has<scene::Motion>(object));
		REQUIRE_FALSE(PortalTransferOfPlayer(store, object));
		REQUIRE(PortalTransferOfObject(store, object)->Kind == scene::PortalBodyKind::Object);
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(object));
		REQUIRE(store.Alive(pair.Player));
		REQUIRE(PortalTransferOfObject(store, object)->Stage == PortalTransferStage::Committed);
	});
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto arrived = PortalTransferObject(store, outward);
		REQUIRE(arrived != ecs::NULL_ENTITY);
		REQUIRE(PortalTransferPlayer(store, outward) == ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 0);
		REQUIRE(store.Get<scene::Collider>(arrived)->Extent.X == Catch::Approx(4));
		REQUIRE((store.Get<scene::Motion>(arrived)->Linear - through.Carry({3, 4, 5})).Magnitude() < 1e-5f);
		REQUIRE((store.Get<scene::Motion>(arrived)->Angular - through.Rotate({1, 2, 3})).Magnitude() < 1e-5f);
		const scene::SeamTransform reverse{through.Frame.Inverse(), through.Point(through.Origin), .5f};
		std::string failure;
		REQUIRE(BeginPortalObjectTransfer(store, arrived, "source", reverse, inward, failure));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto returned = PortalTransferObject(store, inward);
		REQUIRE(returned != ecs::NULL_ENTITY);
		REQUIRE(returned != object);
		REQUIRE(
			(store.Get<scene::Transform>(returned)->Frame.Position - core::Vector3{1, 2, 3}).Magnitude() <
			1e-4f
		);
		REQUIRE((store.Get<scene::Motion>(returned)->Linear - core::Vector3{3, 4, 5}).Magnitude() < 1e-4f);
		REQUIRE((store.Get<scene::Motion>(returned)->Angular - core::Vector3{1, 2, 3}).Magnitude() < 1e-4f);
		REQUIRE(store.Get<scene::Collider>(returned)->Extent.X == Catch::Approx(2));
	});
}

TEST_CASE(
	"ordinary portal object rejects external ownership before fencing and replays pending admission",
	"[script][portal-transfer][portal-object]"
) {
	for (int phase = 0; phase < 4; ++phase) {
		Pair pair;
		PortalTransferId id;
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			const auto object = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "cargo");
			store.SetParent(object, scene::WorkspaceOf(store));
			store.Set(object, scene::Motion{{1, 2, 3}, {3, 2, 1}});
			store.Set(object, scene::Simulated{});
			store.Set(object, scene::NetworkOwner{pair.Player});
			std::string failure;
			REQUIRE_FALSE(BeginPortalObjectTransfer(store, object, "destination", {}, id, failure));
			REQUIRE(failure.find("scene.NetworkOwner") != std::string::npos);
			REQUIRE(store.Has<scene::Motion>(object));
			REQUIRE(store.Has<scene::Simulated>(object));
			store.Remove<scene::NetworkOwner>(object);
			REQUIRE(BeginPortalObjectTransfer(store, object, "destination", {}, id, failure));
		});
		pair.Tick(phase);
		core::ByteWriter snapshot;
		REQUIRE(pair.Worlds.Save(snapshot));
		world::Universe restored;
		core::ByteReader reader(snapshot.Bytes());
		REQUIRE(restored.Load(reader));
		for (auto world : restored.Worlds())
			restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
				RegisterTeleportAdmission(scheduler);
			});
		for (int tick = 0; tick < 7; ++tick) {
			pair.Tick();
			restored.Tick(1.0f / 60);
		}
		core::ByteWriter expected, actual;
		REQUIRE(pair.Worlds.Save(expected));
		REQUIRE(restored.Save(actual));
		INFO(phase);
		REQUIRE(std::ranges::equal(expected.Bytes(), actual.Bytes()));
		restored.Enter(restored.Find(core::Name("destination")), [&](ecs::Store &store) {
			REQUIRE(PortalTransferObject(store, id) != ecs::NULL_ENTITY);
			REQUIRE(scene::PlayerCount(store) == 0);
		});
	}
}

TEST_CASE(
	"installed nearest foreign sweep transfers a moving object and resolves the destination suffix wall",
	"[script][portal-transfer][portal-object]"
) {
	Pair pair;
	ecs::Entity object;
	pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) {
		physics::PreparePhysicsWorld(store);
		const auto pane = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "reciprocal pane");
		store.Set(pane, scene::Transform{core::CFrame({100, 2.5f, 0})});
		store.Set(pane, scene::Bounds{{5, 5, .1f}});
		const auto beyond = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "source stand-in");
		store.Set(beyond, scene::Transform{core::CFrame::LookAt({0, 2.5f, 0}, {0, 2.5f, 1})});
		store.Set(beyond, scene::Bounds{{5, 5, .1f}});
		const auto surface =
			store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "reciprocal");
		scene::SurfaceCamera camera;
		camera.Face = scene::NormalId::Back;
		camera.Surface = 0;
		store.Set(surface, camera);
		scene::Portal portal{beyond};
		portal.DestinationWorld = core::Name("source");
		store.Set(surface, portal);
		store.SetParent(surface, pane);
		store.Remove<scene::Collider>(pane);
		store.Remove<scene::Collider>(beyond);

		const auto wall = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "wall");
		store.Set(wall, scene::Transform{core::CFrame({100, 2.5f, -5})});
		scene::Collider collider;
		collider.Extent = {5, 5, .1f};
		collider.CanQuery = false;
		store.Set(wall, collider);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto pane = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "pane");
		store.Set(pane, scene::Transform{core::CFrame({0, 2.5f, 0})});
		store.Set(pane, scene::Bounds{{5, 5, .1f}});
		const auto beyond = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "beyond");
		store.Set(beyond, scene::Transform{core::CFrame::LookAt({100, 2.5f, 0}, {100, 2.5f, 1})});
		store.Set(beyond, scene::Bounds{{5, 5, .1f}});
		const auto surface = store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "portal");
		scene::SurfaceCamera camera;
		camera.Face = scene::NormalId::Back;
		camera.Surface = 0;
		store.Set(surface, camera);
		scene::Portal portal{beyond};
		portal.DestinationWorld = core::Name("destination");
		store.Set(surface, portal);
		store.SetParent(surface, pane);
		object = store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "cargo");
		store.SetParent(object, scene::WorkspaceOf(store));
		store.Set(object, scene::Simulated{});
		store.Set(object, scene::PreviousTransform{core::CFrame({0, 2.5f, 10})});
		store.Set(object, scene::Transform{core::CFrame({0, 2.5f, -10})});
		store.Set(object, scene::Motion{{0, 0, -1200}, {}});
	});
	pair.Tick();
	PortalTransferId id;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto receipt = PortalTransferOfObject(store, object);
		REQUIRE(receipt.has_value());
		id = receipt->Id;
		REQUIRE(store.Has<scene::Motion>(object));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto arrived = PortalTransferObject(store, id);
		REQUIRE(arrived != ecs::NULL_ENTITY);
		const auto position = store.Get<scene::Transform>(arrived)->Frame.Position;
		INFO(position.X << ", " << position.Y << ", " << position.Z);
		CHECK(position.X == Catch::Approx(100));
		CHECK(position.Z > -5);
		CHECK(position.Z < 0);
		REQUIRE(store.Get<scene::Motion>(arrived) != nullptr);
		CHECK(store.Get<scene::Motion>(arrived)->Linear.Z == Catch::Approx(-1200));
	});
}

TEST_CASE(
	"portal preparation freezes animation and restores its exact rows after refusal and replay",
	"[script][portal-transfer][portal-animation]"
) {
	Pair pair;
	ecs::Entity animator, track;
	scene::AnimationTrack playing;
	playing.TimePosition = 2.25f;
	playing.Speed = -.75f;
	playing.Weight = .4f;
	playing.WeightTarget = .8f;
	playing.FadeTime = .3f;
	playing.Playing = true;
	playing.Looped = true;
	playing.Reserved[0] = 7;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		store.Set(pair.Root, scene::Skeleton{});
		animator = store.CreateInstance(scene::AnimatorClass(), "animator");
		store.SetParent(animator, pair.Root);
		scene::Animator driver;
		driver.Rig = pair.Root;
		driver.Reserved[0] = 9;
		store.Set(animator, driver);
		track = store.CreateInstance(ecs::Classes::Find(core::Name("AnimationTrack")), "playing");
		store.SetParent(track, animator);
		store.Set(track, playing);
	});
	pair.Begin("missing-world");
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Has<scene::Animator>(animator));
		REQUIRE(store.Has<scene::AnimationTrack>(track));
		REQUIRE(scene::AdvanceAnimationTracks(store) != 0);
	});
	core::ByteWriter saved;
	REQUIRE(pair.Worlds.Save(saved));
	world::Universe restored;
	core::ByteReader reader(saved.Bytes());
	REQUIRE(restored.Load(reader));
	for (auto world : restored.Worlds())
		restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
			RegisterTeleportAdmission(scheduler);
		});
	for (int step = 0; step < 2; ++step) {
		pair.Tick();
		restored.Tick(1.0f / 60);
	}
	core::ByteWriter original, replay;
	REQUIRE(pair.Worlds.Save(original));
	REQUIRE(restored.Save(replay));
	REQUIRE(std::ranges::equal(original.Bytes(), replay.Bytes()));
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(store.Get<scene::Animator>(animator)->Rig == pair.Root);
		CHECK(store.Get<scene::Animator>(animator)->Reserved[0] == 9);
		const auto *actual = store.Get<scene::AnimationTrack>(track);
		REQUIRE(actual != nullptr);
		CHECK(actual->TimePosition == playing.TimePosition);
		CHECK(actual->Speed == playing.Speed);
		CHECK(actual->Weight == playing.Weight);
		CHECK(actual->WeightTarget == playing.WeightTarget);
		CHECK(actual->FadeTime == playing.FadeTime);
		CHECK(actual->Reserved[0] == 7);
		CHECK(actual->Playing == playing.Playing);
		CHECK(actual->Looped == playing.Looped);
	});
}

TEST_CASE(
	"portal accessory dangling reference refuses before fencing and missing world restores exact rig",
	"[script][portal-transfer][accessory]"
) {
	Pair pair;
	SECTION("dangling reference") {
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			const auto hat = store.FindFirstChild(scene::CharacterOf(store, pair.Player), "hat");
			const auto point = store.Get<scene::Accessory>(hat)->CharacterAttachment;
			store.DestroyInstance(point);
			core::ByteWriter before, after;
			REQUIRE(store.Save(before));
			PortalTransferId id;
			std::string failure;
			REQUIRE_FALSE(BeginPortalTransfer(store, pair.Player, "destination", {}, id, failure));
			REQUIRE(store.Save(after));
			REQUIRE(std::ranges::equal(before.Bytes(), after.Bytes()));
			REQUIRE(store.Has<scene::Motion>(pair.Root));
		});
	}
	SECTION("precommit refusal") {
		core::ByteWriter before;
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			scene::PortalBodyCopy body;
			std::string failure;
			REQUIRE(scene::CapturePortalBody(store, pair.Player, body, failure));
			REQUIRE(scene::WritePortalBody(before, body));
		});
		pair.Begin("missing");
		pair.Tick(5);
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			scene::PortalBodyCopy body;
			std::string failure;
			core::ByteWriter after;
			REQUIRE(scene::CapturePortalBody(store, pair.Player, body, failure));
			REQUIRE(scene::WritePortalBody(after, body));
			REQUIRE(std::ranges::equal(before.Bytes(), after.Bytes()));
		});
	}
}

TEST_CASE(
	"player transfer waits for admission to the exact destination incarnation",
	"[script][portal-transfer][player-admission]"
) {
	for (const uint64_t admission : {uint64_t(0), uint64_t(202), uint64_t(203)}) {
		CAPTURE(admission);
		Pair pair({}, true);
		const auto id = pair.Begin();
		pair.Tick(40);
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(store.Alive(pair.Player));
			REQUIRE_FALSE(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Prepared);
			CHECK_FALSE(AdmitPortalPlayerTransfer(store, id, 0));
			if (admission != 0) {
				REQUIRE(AdmitPortalPlayerTransfer(store, id, admission));
				CHECK(AdmitPortalPlayerTransfer(store, id, admission));
				CHECK_FALSE(AdmitPortalPlayerTransfer(store, id, admission + 1));
			}
		});
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			CHECK(scene::PlayerCount(store) == 0);
			CHECK(PortalTransferPlayer(store, id) == ecs::NULL_ENTITY);
		});
		core::ByteWriter saved;
		REQUIRE(pair.Worlds.Save(saved));
		world::Universe restored;
		core::ByteReader reader(saved.Bytes());
		REQUIRE(restored.Load(reader));
		for (const auto world : restored.Worlds())
			restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
				RegisterTeleportAdmission(scheduler);
			});
		for (int tick = 0; tick < 20; tick++) {
			pair.Tick();
			restored.Tick(1.0f / 60);
		}
		core::ByteWriter expected, actual;
		REQUIRE(pair.Worlds.Save(expected));
		REQUIRE(restored.Save(actual));
		CHECK(std::ranges::equal(expected.Bytes(), actual.Bytes()));
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			CHECK(store.Alive(pair.Player) == (admission != 202));
			CHECK_FALSE(AdmitPortalPlayerTransfer(store, id, 0));
		});
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			CHECK(scene::PlayerCount(store) == (admission == 202 ? 1 : 0));
		});
	}
}

TEST_CASE(
	"cancelled player transfer waits for acknowledgement and replays lost replies",
	"[script][portal-transfer][portal-cancellation]"
) {
	for (const int phase : {0, 4}) {
		CAPTURE(phase);
		Pair pair({}, true);
		const auto id = pair.Begin();
		pair.Tick(phase);
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(CancelPortalPlayerTransfer(store, id, "lease refused"));
			CHECK(CancelPortalPlayerTransfer(store, id, "duplicate cancellation"));
			CHECK_FALSE(AdmitPortalPlayerTransfer(store, id, 202));
			CHECK(store.Alive(pair.Player));
			CHECK(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled == (phase == 0));
		});
		bool discard = true;
		size_t dropped = 0;
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &, ecs::Scheduler &scheduler) {
			scheduler.Add(
				"test.discard-cancel-replies",
				ecs::Phase::PreSimulation,
				[&](ecs::Store &store) {
					if (!discard) return;
					auto *inbox = store.ResourceMutable<world::Inbox>();
					if (!inbox) return;
					dropped += std::erase_if(inbox->Arrived, [](const auto &delivery) {
						return delivery.Key.Text() == "engine.portal.transfer" && !delivery.Reply.Expected();
					});
				},
				ecs::SystemOrder{{"portal.transfer.pump"}, {}}
			);
		});
		pair.Tick(40);
		REQUIRE(dropped >= 2);
		discard = false;
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Cancelling);
			CHECK(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled == (phase == 0));
		});
		core::ByteWriter saved;
		REQUIRE(pair.Worlds.Save(saved));
		world::Universe restored;
		core::ByteReader reader(saved.Bytes());
		REQUIRE(restored.Load(reader));
		for (const auto world : restored.Worlds())
			restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
				RegisterTeleportAdmission(scheduler);
			});
		for (int tick = 0; tick < 20; tick++) {
			pair.Tick();
			restored.Tick(1.0f / 60);
		}
		core::ByteWriter expected, actual;
		REQUIRE(pair.Worlds.Save(expected));
		REQUIRE(restored.Save(actual));
		CHECK(std::ranges::equal(expected.Bytes(), actual.Bytes()));
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(store.Alive(pair.Player));
			CHECK(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
			CHECK(store.Get<scene::Humanoid>(pair.Humanoid)->Health == 23);
			CHECK(store.Get<scene::Motion>(pair.Root)->Linear == core::Vector3(3, 1, -8));
			const auto receipt = PortalTransferOfPlayer(store, pair.Player);
			REQUIRE(receipt.has_value());
			CHECK(receipt->Stage == PortalTransferStage::Refused);
			CHECK(receipt->Diagnostic == "lease refused");
			CHECK_FALSE(CancelPortalPlayerTransfer(store, id, "already cancelled"));
		});
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			CHECK(scene::PlayerCount(store) == 0);
			CHECK(PortalTransferPlayer(store, id) == ecs::NULL_ENTITY);
		});
	}
}

TEST_CASE(
	"cancelled reservations release capacity and reject late offers",
	"[script][portal-transfer][portal-cancellation]"
) {
	Pair pair({}, true);
	std::optional<world::Delivery> late;
	bool dropOffer = false;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &, ecs::Scheduler &scheduler) {
		scheduler.Add(
			"test.capture-offer",
			ecs::Phase::PreSimulation,
			[&](ecs::Store &store) {
				if (late) return;
				auto *inbox = store.ResourceMutable<world::Inbox>();
				if (!inbox) return;
				for (auto entry = inbox->Arrived.begin(); entry != inbox->Arrived.end(); ++entry) {
					if (entry->Key.Text() != "engine.portal.transfer" || entry->Reply.Expected()) continue;
					late = *entry;
					if (dropOffer) inbox->Arrived.erase(entry);
					return;
				}
			},
			ecs::SystemOrder{{"portal.transfer.pump"}, {}}
		);
	});

	for (int attempt = 0; attempt < 70; attempt++) {
		dropOffer = attempt == 0;
		late.reset();
		const auto id = pair.Begin();
		pair.Tick(4);
		REQUIRE(late.has_value());
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(CancelPortalPlayerTransfer(store, id, "lease expired"));
		});
		pair.Tick(4);
		REQUIRE(pair.Worlds.Deliver(core::Name("destination"), *late));
		pair.Tick(3);
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Refused);
			REQUIRE(store.Get<scene::Humanoid>(pair.Humanoid)->Enabled);
		});
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			CHECK(scene::PlayerCount(store) == 0);
		});
	}
	const auto accepted = pair.Begin();
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(AdmitPortalPlayerTransfer(store, accepted, 202));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		CHECK(scene::PlayerCount(store) == 1);
		CHECK(PortalTransferPlayer(store, accepted) != ecs::NULL_ENTITY);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		CHECK_FALSE(CancelPortalPlayerTransfer(store, accepted, "too late"));
		CHECK_FALSE(store.Alive(pair.Player));
	});
}

TEST_CASE(
	"portal movement survives retirement and yields to native input", "[script][portal-transfer][portal-move]"
) {
	Pair pair;
	const scene::SeamTransform through{core::CFrame::LookAt({40, 20, 10}, {41, 20, 10}), {}, 2};
	PortalTransferId id;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false));
		std::string failure;
		REQUIRE(BeginPortalTransfer(store, pair.Player, "destination", through, id, failure));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, -1}, true, 9000));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, 9001));
	});
	pair.Tick(9);
	ecs::Entity humanoid;
	ecs::Entity player;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		humanoid = store.Get<scene::Character>(scene::CharacterOf(store, player))->Humanoid;
		auto *state = store.GetMutable<scene::Humanoid>(humanoid);
		REQUIRE(state->MoveDirection == through.Rotate({1, 0, 0}));
		REQUIRE(state->MoveDirection.Magnitude() == Catch::Approx(1));
		REQUIRE(state->WalkSpeed == Catch::Approx(32));
		REQUIRE(state->JumpRequested);
		state->JumpRequested = false;
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(pair.Player));
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->AcknowledgedInputTick == 9001);
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {}, false, 9002));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, true, 9001));
		REQUIRE_FALSE(ForwardPortalPlayerMove(store, pair.Player, {2, 0, 0}, false));
		REQUIRE_FALSE(ForwardPortalPlayerMove(store, ecs::NULL_ENTITY, {}, false));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		auto *state = store.GetMutable<scene::Humanoid>(humanoid);
		REQUIRE(state->MoveDirection == core::Vector3{});
		REQUIRE_FALSE(state->JumpRequested);
		ClosePortalPlayerMoveForwarding(store, player);
		state->MoveDirection = {0, 0, 1};
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, true));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto *state = store.Get<scene::Humanoid>(humanoid);
		REQUIRE(state->MoveDirection == core::Vector3{0, 0, 1});
		REQUIRE_FALSE(state->JumpRequested);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(ForwardPortalPlayerMove(store, pair.Player, {}, false));
	});
}

TEST_CASE(
	"native portal input catches up when the authenticated client is ahead of forwarded input",
	"[script][portal-transfer][portal-move][portal-native-catchup]"
) {
	Pair pair;
	const auto id = pair.Begin();
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, -1}, false, 40, 1.0 / 60));
	});
	pair.Tick(9);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		REQUIRE(store.Get<scene::Humanoid>(rig.Humanoid)->MoveDirection == core::Vector3{0, 0, -1});
		CHECK(
			SchedulePortalPlayerMove(store, player, {0, 0, 1}, false, 119, 1.0 / 30) ==
			PortalInputDisposition::Refused
		);
		CHECK(
			SchedulePortalPlayerMove(store, player, {0, 0, 1}, false, 119, 1.0 / 60) ==
			PortalInputDisposition::Queued
		);
		CHECK(
			SchedulePortalPlayerMove(store, player, {1, 0, 0}, false, 5000, 1.0 / 60) ==
			PortalInputDisposition::Refused
		);
	});
	pair.Tick();
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		CHECK(store.Get<scene::Humanoid>(rig.Humanoid)->MoveDirection == core::Vector3{0, 0, 1});
	});
}

TEST_CASE(
	"portal movement and acknowledgements replay across snapshots", "[script][portal-transfer][portal-move]"
) {
	for (int snapshotTick = 0; snapshotTick < 12; ++snapshotTick) {
		Pair pair;
		const auto id = pair.Begin();
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, true, 9100, 1.0 / 120));
		});
		pair.Tick(std::min(snapshotTick, 7));
		if (snapshotTick >= 7) {
			pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
				ClosePortalPlayerMoveForwarding(store, PortalTransferPlayer(store, id));
			});
			pair.Tick(snapshotTick - 7);
		}
		core::ByteWriter saved;
		REQUIRE(pair.Worlds.Save(saved));
		world::Universe restored;
		core::ByteReader reader(saved.Bytes());
		REQUIRE(restored.Load(reader));
		for (const auto world : restored.Worlds())
			restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
				RegisterTeleportAdmission(scheduler);
			});
		for (int tick = snapshotTick; tick < 12; ++tick) {
			pair.Tick();
			restored.Tick(1.0f / 60);
		}
		core::ByteWriter expected;
		core::ByteWriter actual;
		REQUIRE(pair.Worlds.Save(expected));
		REQUIRE(restored.Save(actual));
		REQUIRE(std::ranges::equal(expected.Bytes(), actual.Bytes()));
	}
}

TEST_CASE(
	"portal movement retries preserve one jump and stop after acknowledgement",
	"[script][portal-transfer][portal-move]"
) {
	Pair pair;
	const auto id = pair.Begin();
	pair.Tick(8);
	world::Envelope firstMove;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE_FALSE(ForwardPortalPlayerMove(
			store, pair.Player, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, false
		));
		REQUIRE_FALSE(
			ForwardPortalPlayerMove(store, pair.Player, {0, std::numeric_limits<float>::infinity(), 0}, false)
		);
		for (double step :
			 {-1.0,
			  -std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::quiet_NaN()})
			REQUIRE_FALSE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, true, 12000, step));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, true, 12000, 1.0 / 120));
		PumpPortalTransfers(store);
		auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
		REQUIRE_FALSE(pending.empty());
		firstMove = pending.back();
		REQUIRE(firstMove.Payload[4] == std::byte{7});
		pending.clear();
	});
	// Drop acknowledgements until the source has sent the same state repeatedly.
	ecs::Entity humanoid;
	bool sawJump = false;
	for (int tick = 0; tick < 8; ++tick) {
		pair.Tick();
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			const auto player = PortalTransferPlayer(store, id);
			humanoid = store.Get<scene::Character>(scene::CharacterOf(store, player))->Humanoid;
			auto *state = store.GetMutable<scene::Humanoid>(humanoid);
			if (sawJump) REQUIRE_FALSE(state->JumpRequested);
			if (state->JumpRequested) sawJump = true;
			state->JumpRequested = false;
			store.ResourceMutable<world::Outbox>()->Pending.clear();
		});
	}
	REQUIRE(sawJump);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {}, false, 12001));
	});
	pair.Tick(8);
	world::Delivery delayed;
	delayed.Bus = world::BusKind::Channel;
	delayed.Key = core::Name("engine.portal.transfer");
	delayed.From = core::Name("source");
	delayed.Payload = firstMove.Payload;
	REQUIRE(pair.Worlds.Deliver(core::Name("destination"), delayed));
	pair.Tick(4);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto *state = store.Get<scene::Humanoid>(humanoid);
		REQUIRE(state->MoveDirection == core::Vector3{});
		REQUIRE_FALSE(state->JumpRequested);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
		pending.clear();
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {}, false));
		PumpPortalTransfers(store);
		REQUIRE(pending.empty());
		CHECK(PortalTransferOfPlayer(store, pair.Player)->AcknowledgedInputTick == 12001);
	});
}

TEST_CASE(
	"portal move segments reject gaps and deduplicate a jump", "[script][portal-transfer][portal-move]"
) {
	Pair pair;
	const auto id = pair.Begin();
	pair.Tick(8);
	world::Envelope segment;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, 210));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, 1}, true, 211));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {}, false, 212));
		PumpPortalTransfers(store);
		auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
		REQUIRE_FALSE(pending.empty());
		segment = pending.back();
		pending.clear();
	});
	// The Move body ends with u32 count followed by fixed 44-byte move rows.
	auto skipped = segment;
	const size_t rowBytes = 44;
	const size_t countOffset = skipped.Payload.size() - 4 - 3 * rowBytes;
	skipped.Payload[countOffset] = std::byte{2};
	skipped.Payload.erase(
		skipped.Payload.begin() + static_cast<std::ptrdiff_t>(countOffset + 4),
		skipped.Payload.begin() + static_cast<std::ptrdiff_t>(countOffset + 4 + rowBytes)
	);
	world::Delivery delivery;
	delivery.Bus = world::BusKind::Channel;
	delivery.Key = core::Name("engine.portal.transfer");
	delivery.From = core::Name("source");
	delivery.Payload = skipped.Payload;
	REQUIRE(pair.Worlds.Deliver(core::Name("destination"), delivery));
	pair.Tick();
	bool jumped = false;
	std::vector<core::Vector3> directions;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		const auto *humanoid = store.Get<scene::Humanoid>(rig.Humanoid);
		CHECK(humanoid->MoveDirection == core::Vector3{});
		CHECK_FALSE(humanoid->JumpRequested);
	});
	delivery.Payload = segment.Payload;
	REQUIRE(pair.Worlds.Deliver(core::Name("destination"), delivery));
	for (int tick = 0; tick < 3; ++tick) {
		pair.Tick();
		pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
			const auto player = PortalTransferPlayer(store, id);
			auto *humanoid = store.GetMutable<scene::Humanoid>(
				store.Get<scene::Character>(scene::CharacterOf(store, player))->Humanoid
			);
			directions.push_back(humanoid->MoveDirection);
			jumped |= humanoid->JumpRequested;
			humanoid->JumpRequested = false;
		});
	}
	CHECK(directions == std::vector<core::Vector3>{{1, 0, 0}, {0, 0, 1}, {}});
	CHECK(jumped);
	// Replaying the full segment cannot recreate its discrete jump.
	REQUIRE(pair.Worlds.Deliver(core::Name("destination"), delivery));
	pair.Tick();
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, id);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		CHECK_FALSE(store.Get<scene::Humanoid>(rig.Humanoid)->JumpRequested);
	});
}

TEST_CASE(
	"refused portal restores current movement instead of captured movement",
	"[script][portal-transfer][portal-move]"
) {
	Pair pair;
	pair.Begin("missing-world");
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, 1}, true));
	});
	pair.Tick(4);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Stage == PortalTransferStage::Refused);
		const auto *humanoid = store.Get<scene::Humanoid>(pair.Humanoid);
		REQUIRE(humanoid->Enabled);
		REQUIRE(humanoid->MoveDirection == core::Vector3{0, 0, 1});
		REQUIRE(humanoid->JumpRequested);
		REQUIRE_FALSE(ForwardPortalPlayerMove(store, pair.Player, {}, false));
	});
}

TEST_CASE(
	"active player input routes survive transfer receipt pressure", "[script][portal-transfer][portal-move]"
) {
	Pair pair;
	pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) {
		store.GetMutable<scene::PlayersServiceComponent>(scene::PlayersOf(store))->MaxPlayers = 128;
	});
	const auto first = pair.Begin();
	pair.Tick(8);
	for (int index = 1; index < 64; ++index) {
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			const auto player = scene::AddPlayer(store, "walker", false, 100 + index);
			REQUIRE(scene::LoadCharacter(store, player) != ecs::NULL_ENTITY);
			PortalTransferId id;
			std::string failure;
			REQUIRE(BeginPortalTransfer(store, player, "destination", {}, id, failure));
		});
		pair.Tick(8);
	}
	ecs::Entity nextPlayer;
	world::Delivery delayed;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		nextPlayer = scene::AddPlayer(store, "next", false, 200);
		REQUIRE(scene::LoadCharacter(store, nextPlayer) != ecs::NULL_ENTITY);
		PortalTransferId id;
		std::string failure;
		REQUIRE_FALSE(BeginPortalTransfer(store, nextPlayer, "destination", {}, id, failure));
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false));
		PumpPortalTransfers(store);
		delayed.Bus = world::BusKind::Channel;
		delayed.Key = core::Name("engine.portal.transfer");
		delayed.From = core::Name("source");
		delayed.Payload = store.Resource<world::Outbox>()->Pending.back().Payload;
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto player = PortalTransferPlayer(store, first);
		REQUIRE(player != ecs::NULL_ENTITY);
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		REQUIRE(store.Get<scene::Humanoid>(rig->Humanoid)->MoveDirection == core::Vector3{1, 0, 0});
		ClosePortalPlayerMoveForwarding(store, player);
	});
	pair.Tick(4);
	PortalTransferId next;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		std::string failure;
		REQUIRE(BeginPortalTransfer(store, nextPlayer, "destination", {}, next, failure));
	});
	pair.Tick(8);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		REQUIRE(PortalTransferPlayer(store, next) != ecs::NULL_ENTITY);
		REQUIRE(scene::PlayerCount(store) == 65);
	});
	REQUIRE(pair.Worlds.Deliver(core::Name("destination"), delayed));
	pair.Tick();
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto &pending = store.Resource<world::Outbox>()->Pending;
		REQUIRE_FALSE(pending.empty());
		REQUIRE(pending.back().Payload[4] == std::byte{8});
		REQUIRE(pending.back().Payload[pending.back().Payload.size() - 2] == std::byte{1});
		REQUIRE(pending.back().Payload.back() == std::byte{0});
	});
}

TEST_CASE(
	"unchanged forwarded movement acknowledges each newer client stamp",
	"[script][portal-transfer][portal-move][portal-input-stamp]"
) {
	Pair pair;
	pair.Begin();
	for (uint64_t inputTick : {uint64_t{2}, uint64_t{9000}, uint64_t{9001}}) {
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, inputTick));
		});
		pair.Tick(8);
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->AcknowledgedInputTick == inputTick);
			auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
			pending.clear();
			REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, 1}, true, inputTick));
			PumpPortalTransfers(store);
			CHECK(pending.empty());
		});
	}
}

TEST_CASE(
	"portal motion acknowledges a completed physics pose with its originating input",
	"[script][portal-transfer][portal-completed-motion]"
) {
	struct Completed {
		uint64_t Tick;
		double Seconds;
		scene::Transform Root;
		scene::Motion Velocity;
		scene::Humanoid Humanoid;
	};
	std::vector<Completed> completed;
	Pair pair;
	const auto id = pair.Begin();
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
		physics::PreparePhysicsWorld(store);
		physics::RegisterPhysicsSystems(scheduler);
		scheduler.Add(
			"test.completed-pose",
			ecs::Phase::Replication,
			[&](ecs::Store &world) {
				const auto player = PortalTransferPlayer(world, id);
				const auto *rig = world.Get<scene::Character>(scene::CharacterOf(world, player));
				if (rig)
					completed.push_back(
						{world.Time().Tick,
						 world.Time().Elapsed,
						 *world.Get<scene::Transform>(rig->Root),
						 *world.Get<scene::Motion>(rig->Root),
						 *world.Get<scene::Humanoid>(rig->Humanoid)}
					);
			},
			ecs::SystemOrder{{}, {"portal.transfer.motion"}}
		);
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, 9000));
	});
	pair.Tick(12);
	REQUIRE(completed.size() > 2);
	CHECK((completed.back().Root.Frame.Position - completed.front().Root.Frame.Position).Magnitude() > .01f);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		const auto receipt = PortalTransferOfPlayer(store, pair.Player);
		REQUIRE(receipt);
		REQUIRE(receipt->Motion);
		const auto &motion = *receipt->Motion;
		CHECK(motion.DestinationIncarnation == 202);
		CHECK(motion.InputTick == 9000);
		CHECK(motion.DestinationTick != motion.InputTick);
		const auto found = std::find_if(completed.begin(), completed.end(), [&](const auto &entry) {
			return entry.Tick == motion.DestinationTick;
		});
		REQUIRE(found != completed.end());
		CHECK(motion.SimulationSeconds == found->Seconds);
		CHECK(motion.Frame.Position == found->Root.Frame.Position);
		CHECK(motion.Frame.Rotation() == found->Root.Frame.Rotation());
		CHECK(motion.WalkSpeed == Catch::Approx(16));
		CHECK(motion.JumpSpeed > 0);
		CHECK(motion.Linear == found->Velocity.Linear);
		CHECK(motion.Angular == found->Velocity.Angular);
		CHECK(motion.Grounded == found->Humanoid.Grounded);
		CHECK(motion.WalkSpeed == found->Humanoid.WalkSpeed);
		CHECK(motion.JumpSpeed == found->Humanoid.JumpSpeed);
	});
}

TEST_CASE(
	"invalid completed portal poses cannot acknowledge input or replace the receipt",
	"[script][portal-transfer][portal-completed-motion]"
) {
	for (int fault = 0; fault < 5; ++fault) {
		CAPTURE(fault);
		Pair pair;
		pair.Begin();
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, 9000));
		});
		std::vector<std::byte> acknowledgement;
		for (int tick = 0; tick < 12 && acknowledgement.empty(); ++tick) {
			pair.Tick();
			pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
				auto &pending = store.ResourceMutable<world::Outbox>()->Pending;
				for (const auto &message : pending) {
					if (message.Payload.size() > 103 && message.Payload[4] == std::byte{8})
						acknowledgement = message.Payload;
				}
				std::erase_if(pending, [](const auto &message) {
					return message.Payload.size() > 4 && message.Payload[4] == std::byte{8};
				});
			});
		}
		REQUIRE_FALSE(acknowledgement.empty());
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE_FALSE(PortalTransferOfPlayer(store, pair.Player)->Motion);
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->AcknowledgedInputTick == 0);
		});
		const auto good = acknowledgement;
		// MoveAck ends with 3 u64, 15 floats, one bool and simulation seconds.
		const size_t sample = acknowledgement.size() - 93;
		if (fault == 0) acknowledgement[sample + 84] = std::byte{2};
		if (fault == 1) acknowledgement[sample] ^= std::byte{1};
		if (fault == 2) std::fill_n(acknowledgement.begin() + sample + 16, 8, std::byte{255});
		if (fault == 3) std::fill_n(acknowledgement.begin() + sample + 36, 16, std::byte{0});
		if (fault == 4) std::fill_n(acknowledgement.end() - 8, 8, std::byte{255});
		world::Delivery delivery;
		delivery.Bus = world::BusKind::Channel;
		delivery.Key = core::Name("engine.portal.transfer");
		delivery.From = core::Name("destination");
		delivery.Payload = acknowledgement;
		REQUIRE(pair.Worlds.Deliver(core::Name("source"), delivery));
		pair.Tick();
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE_FALSE(PortalTransferOfPlayer(store, pair.Player)->Motion);
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->AcknowledgedInputTick == 0);
		});
		delivery.Payload = good;
		REQUIRE(pair.Worlds.Deliver(core::Name("source"), delivery));
		pair.Tick();
		pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Motion);
			REQUIRE(PortalTransferOfPlayer(store, pair.Player)->Motion->InputTick == 9000);
		});
	}
}

TEST_CASE(
	"portal observations follow one traced transfer across both worlds",
	"[script][portal-transfer][portal-observation]"
) {
	CHECK(
		std::vector<std::string_view>(PortalObservationHooks().begin(), PortalObservationHooks().end()) ==
		std::vector<std::string_view>{"portal.crossing", "portal.arrival", "portal.input", "portal.handoff"}
	);
	Pair pair;
	constexpr uint64_t TRACE = 0xabc;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		store.Set(pair.Root, scene::ObservationTrace{TRACE});
	});
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) { SetPortalObservationTrace(store, 7); });
	const auto id = pair.Begin();
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		// The source body is live until H, so 9000 is already in the baseline.
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, -1}, false, 9000, 1.0 / 60));
		NotePortalPlayerInputApplied(store, pair.Player, 9000);
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {1, 0, 0}, false, 9001, 1.0 / 60));
	});
	pair.Tick(12);

	pair.Worlds.Enter(pair.Source, [&](const ecs::Store &store) {
		const auto observed = CopyPortalObservations(store);
		std::vector<PortalHandoffEvent> events;
		for (const auto &record : observed.Handoffs) {
			CHECK(record.Stamp.Trace == TRACE);
			CHECK(record.Transfer == id.Sequence);
			CHECK(record.Peer.View() == "destination");
			if (record.Event == PortalHandoffEvent::Sealed) CHECK(record.InputTick == 9000);
			events.push_back(record.Event);
		}
		REQUIRE(events.size() >= 3);
		CHECK(events[0] == PortalHandoffEvent::Offered);
		CHECK(events[1] == PortalHandoffEvent::Sealed);
		CHECK(events[2] == PortalHandoffEvent::Committing);
		for (size_t index = 1; index < observed.Handoffs.size(); ++index)
			CHECK(observed.Handoffs[index].Stamp.Sequence > observed.Handoffs[index - 1].Stamp.Sequence);
	});
	pair.Worlds.Enter(pair.Destination, [&](const ecs::Store &store) {
		const auto observed = CopyPortalObservations(store);
		CHECK(observed.Overwritten == 0);
		REQUIRE(observed.Arrivals.size() == 1);
		CHECK(observed.Arrivals[0].Stamp.Trace == TRACE);
		CHECK(observed.Arrivals[0].Transfer == id.Sequence);
		CHECK(observed.Arrivals[0].BaselineInputTick == 9000);
		CHECK(observed.Arrivals[0].Source.View() == "source");
		REQUIRE_FALSE(observed.Handoffs.empty());
		CHECK(observed.Handoffs[0].Event == PortalHandoffEvent::Committed);
		std::vector<std::pair<PortalInputRoute, uint64_t>> inputs;
		for (const auto &record : observed.Inputs) {
			CHECK(record.Stamp.Trace == TRACE);
			inputs.emplace_back(record.Route, record.InputTick);
		}
		CHECK(
			inputs == std::vector<std::pair<PortalInputRoute, uint64_t>>{
						  {PortalInputRoute::Included, 9000}, {PortalInputRoute::Forwarded, 9001}
					  }
		);
	});
}

TEST_CASE(
	"a committed body catches up with input forwarded while its source was frozen",
	"[script][portal-transfer][portal-observation][portal-catch-up]"
) {
	Pair pair;
	pair.Worlds.Enter(pair.Destination, [](ecs::Store &store) { physics::PreparePhysicsWorld(store); });
	const auto id = pair.Begin();
	pair.Tick(9);
	ecs::Entity player, root;
	core::Vector3 before;
	float walkSpeed = 0;
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		player = PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		const auto rig = *store.Get<scene::Character>(scene::CharacterOf(store, player));
		root = rig.Root;
		before = store.Get<scene::Transform>(root)->Frame.Position;
		walkSpeed = store.Get<scene::Humanoid>(rig.Humanoid)->WalkSpeed;
	});
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		// Twelve rows of one client step each reach the destination together, as
		// the rows for a sealed source's frozen window do.
		for (uint64_t tick = 9100; tick < 9112; ++tick)
			REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, -1}, false, tick, 1.0 / 60));
	});
	pair.Tick(2);
	pair.Worlds.Enter(pair.Destination, [&](ecs::Store &store) {
		const auto after = store.Get<scene::Transform>(root)->Frame.Position;
		// All but the newest step's worth move at once, along the rotated direction.
		const auto moved = after - before;
		CHECK(moved.Z == Catch::Approx(-11 * walkSpeed / 60).margin(.01f));
		CHECK(std::abs(moved.X) < .01f);
		CHECK(AppliedPortalPlayerInput(store, player) == 9111);
		std::vector<std::pair<PortalInputRoute, uint64_t>> inputs;
		for (const auto &record : CopyPortalObservations(store).Inputs)
			if (record.InputTick >= 9100) inputs.emplace_back(record.Route, record.InputTick);
		REQUIRE(inputs.size() == 12);
		for (uint64_t index = 0; index < 11; ++index)
			CHECK(inputs[index] == std::pair{PortalInputRoute::CaughtUp, 9100 + index});
		CHECK(inputs[11] == std::pair{PortalInputRoute::Forwarded, uint64_t{9111}});
	});
}

TEST_CASE(
	"a frozen source body reports the input it holds",
	"[script][portal-transfer][portal-move][portal-catch-up]"
) {
	Pair pair;
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		CHECK_FALSE(FrozenPortalPlayerInput(store, pair.Player));
	});
	(void)pair.Begin();
	pair.Tick(9);
	pair.Worlds.Enter(pair.Source, [&](ecs::Store &store) {
		// Sealed and committed: later input is forwarded, never applied here.
		const auto frozen = FrozenPortalPlayerInput(store, pair.Player);
		REQUIRE(frozen);
		REQUIRE(ForwardPortalPlayerMove(store, pair.Player, {0, 0, -1}, false, *frozen + 5, 1.0 / 60));
		CHECK(FrozenPortalPlayerInput(store, pair.Player) == frozen);
	});
}
