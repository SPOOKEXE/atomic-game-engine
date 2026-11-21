// A client walks its own character on a real server and holds only its own
// containers. Split from Replication.cpp so the runner can run the two side
// by side; each case runs a real 60 Hz server process.

#include "fixtures/ReplicationRemote.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("server.replication-players")
TEST_DEPENDS("engine.scene.registration")

using namespace server_replication_test;

TEST_CASE("a client is told which player is theirs, and can walk it", "[server][replication]") {
	// **The three things that had to be true before "local server, several
	// clients" meant anything**, and none of them could be checked in process:
	//
	//   * the client learns which `Player` is its own, which is per-client state
	//     and therefore cannot be replicated;
	//   * a body is spawned for it, and arrives whole over the wire;
	//   * pressing a key here moves that body *there*, and the movement comes
	//     back as replicated state rather than being simulated twice.
	//
	// Two clients, because one client cannot show the interesting half: each has
	// to be told about a *different* player, and a bug that assigned both to the
	// same one would pass every single-client check ever written.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// A floor, so the character has somewhere to stand and does not simply fall
	// out of the world while the test is watching it.
	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_characters.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local floor = Instance.new(\"Part\")\n"
			 << "floor.Size = Vector3.new(400, 4, 400)\n"
			 << "floor.Position = Vector3.new(0, -2, 0)\n"
			 << "floor.Anchored = true\n"
			 << "floor.Parent = workspace\n";
	}

	Remote first;
	REQUIRE(first.Start(0, scene.string()));
	REQUIRE(first.Join(400));
	Settle(first);

	Remote second;
	REQUIRE(second.Connect(first.Port));
	REQUIRE(second.Join(400));
	Settle(second);
	Settle(first);

	// Each client was told about a player, and never about the same one.
	REQUIRE(first.Mine != engine::ecs::NULL_ENTITY);
	REQUIRE(second.Mine != engine::ecs::NULL_ENTITY);
	CHECK(first.Mine != second.Mine);

	// **The character arrived whole**, which is a stronger claim than "some rows
	// arrived": `Character` names two entities, and a replica that received the
	// component without the rows it points at would be a character with no body.
	const Entity mine = engine::scene::CharacterOf(first.World, first.Mine);
	REQUIRE(mine != engine::ecs::NULL_ENTITY);

	const Character *rig = first.World.Get<Character>(mine);
	REQUIRE(rig != nullptr);
	REQUIRE(first.World.Alive(rig->Root));
	REQUIRE(first.World.Get<Transform>(rig->Root) != nullptr);

	// Both characters are in both replicas - this is what makes it a game with
	// two players in it rather than two games.
	CHECK(engine::scene::CharacterOf(first.World, second.Mine) != engine::ecs::NULL_ENTITY);
	CHECK(engine::scene::CharacterOf(second.World, first.Mine) != engine::ecs::NULL_ENTITY);

	const engine::core::Vector3 before = first.World.Get<Transform>(rig->Root)->Frame.Position;
	const uint64_t priorReplicaTick = second.Link->Applied();
	second.Observations.Clear();
	bool sawMoveExchange = false;

	// **Walk, and keep walking.** An input channel is unreliable by design and
	// the server clears what it has applied every tick, so a single submission
	// is a single tick of movement - which is under a tenth of a metre and inside
	// the wire's own quantisation. A client that stops sending stops moving, and
	// that is the behaviour, not a limitation of the test.
	//
	// **Both clients tick throughout.** A client that stops polling stops
	// acknowledging, and an authority with nothing acknowledged has no baseline
	// to build the next delta against - so a second client parked for a second
	// is a second client whose world is a second stale, which is a fact about
	// this test's pacing rather than about the feature.
	for (int tick = 0; tick < 200; tick++) {
		first.Walk(engine::core::Vector3{1.0f, 0.0f, 0.0f});
		first.Tick();
		second.Tick();
		while (const auto record = second.Observations.Poll()) {
			if (record->Hook == engine::replication::ReplicationHook::ReplicaApplied) {
				const auto *applied =
					std::get_if<engine::replication::ReplicaAppliedObservation>(&record->Context);
				if (applied != nullptr && applied->Identity.TickAvailable &&
					applied->Identity.Tick > priorReplicaTick)
					sawMoveExchange = true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	CHECK(sawMoveExchange);

	const engine::core::Vector3 after = first.World.Get<Transform>(rig->Root)->Frame.Position;

	// **Along X and not merely "somewhere else".** A character that fell through
	// the floor also moved, and asserting a bare inequality would pass for it.
	CHECK(after.X > before.X + 1.0f);
	REQUIRE(first.NativeMotion);
	CHECK(first.NativeMotion->Player == first.Mine);
	CHECK(first.NativeMotion->Root == rig->Root);
	CHECK(first.NativeMotion->Motion.InputTick != 0);
	CHECK(first.NativeMotion->Motion.DestinationTick != 0);
	CHECK(first.NativeMotion->Motion.Frame.Position.X > before.X + 1.0f);
	CHECK_FALSE(second.NativeMotion);
	REQUIRE(first.Link->Submit(900000, engine::game::EncodeMoveInput({}), first.Now));
	for (int tick = 0; tick < 300 && first.NativeMotion->Motion.InputTick != 900000; ++tick) {
		first.Tick();
		second.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	CHECK(first.NativeMotion->Motion.InputTick == 900000);
	CHECK(first.NativeMotion->Motion.DestinationTick < 900000);

	// And the *other* client sees the same body in the new place, which is the
	// half that separates "the server moved it" from "everybody was told".
	REQUIRE(second.Wait(
		[&] {
			const Entity character = engine::scene::CharacterOf(second.World, first.Mine);
			const Character *current = second.World.Get<Character>(character);
			const Transform *position =
				current == nullptr ? nullptr : second.World.Get<Transform>(current->Root);
			return position != nullptr && position->Frame.Position.X > before.X + 1.0f;
		},
		400
	));
	const Entity theirs = engine::scene::CharacterOf(second.World, first.Mine);
	const Character *seen = second.World.Get<Character>(theirs);
	REQUIRE(seen != nullptr);
	CHECK(second.World.Get<Transform>(seen->Root)->Frame.Position.X > before.X + 1.0f);

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

TEST_CASE("WASD on a client walks its character on the server", "[server][replication]") {
	// **The case above sends a direction; this one presses a key.** `Remote::
	// Walk` hands `EncodeMoveInput` a vector the test chose, which leaves the
	// whole client half untested: which world direction W means, that the camera
	// is what it is measured against, that an unfocused window walks nobody, and
	// that the space bar leaves the ground. Every one of those is arithmetic in
	// `scene::ReadMoveIntent` that a hand-built vector steps straight over -
	// `Remote::SubmitMove` runs `Client::SubmitMove`'s body instead.
	//
	// **One case and not five, because the five share a server.** Standing a
	// process up and joining it costs about a second; the keys do not interact,
	// so what is worth paying for once is the world and not the assertion.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// A floor wide enough that a character walking for a second in any of four
	// directions is still on it, and a spawn to start from.
	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_wasd.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local floor = Instance.new(\"Part\")\n"
			 << "floor.Name = \"SpawnLocation\"\n"
			 << "floor.Size = Vector3.new(400, 4, 400)\n"
			 << "floor.Position = Vector3.new(0, -2, 0)\n"
			 << "floor.Anchored = true\n"
			 << "floor.Parent = workspace\n";
	}

	Remote client;
	REQUIRE(client.Start(0, scene.string()));
	REQUIRE(client.Join(400));
	REQUIRE(client.Wait(
		[&] {
			return client.Mine != engine::ecs::NULL_ENTITY &&
				   engine::scene::CharacterOf(client.World, client.Mine) != engine::ecs::NULL_ENTITY;
		},
		400
	));

	REQUIRE(client.Mine != engine::ecs::NULL_ENTITY);
	REQUIRE(engine::scene::CharacterOf(client.World, client.Mine) != engine::ecs::NULL_ENTITY);

	// Looking down -Z, which is the camera's rest heading and the one
	// `ReadMoveIntent` builds its basis from. Stated rather than assumed: every
	// expectation below is a direction in *this* frame.
	client.LookAlong(0.0f);

	using engine::scene::KeyCode;

	// How far counts as walking. **A metre**, which is a fifteenth of a second
	// at `WalkSpeed` and hundreds of times the wire's own quantisation - so it
	// separates "walked" from both "stood still" and "drifted".
	constexpr float WALKED = 1.0f;

	// Long enough for the round trip to matter: the input crosses a socket, the
	// server applies it on its own tick, and the new pose comes back as ordinary
	// replicated state. Nothing here simulates the character locally.
	constexpr int HELD = 90;

	const engine::core::Vector3 start = client.MyPosition();

	// **W is away from the camera**, which with a yaw of zero is -Z. This is the
	// assertion that would catch a keycode table wired to the wrong axis, and it
	// is the one a hand-built direction can never make.
	const engine::core::Vector3 north = client.Hold({KeyCode::W}, HELD);
	CHECK(north.Z < start.Z - WALKED);
	CHECK(std::abs(north.X - start.X) < WALKED);

	// S is back the other way, and past where it started rather than merely
	// slower - a character that only ever walked forwards would pass a test that
	// asked for "moved".
	const engine::core::Vector3 south = client.Hold({KeyCode::S}, HELD * 2);
	CHECK(south.Z > north.Z + WALKED);

	// D is the camera's right, which at this yaw is +X, and A is its mirror.
	const engine::core::Vector3 east = client.Hold({KeyCode::D}, HELD);
	CHECK(east.X > south.X + WALKED);

	const engine::core::Vector3 west = client.Hold({KeyCode::A}, HELD * 2);
	CHECK(west.X < east.X - WALKED);

	// **Nothing held is nothing moved**, and this is not a tautology about the
	// keyboard: `Humanoid::MoveDirection` is a field the server keeps, so a
	// client that stopped sending - or one whose released keys still read as
	// held - leaves a character walking for ever. `ReadMoveIntent` returning a
	// zero every tick is what stops it.
	const engine::core::Vector3 released = client.Hold({}, HELD);
	const engine::core::Vector3 stillThere = client.Hold({}, HELD);
	CHECK(std::abs(stillThere.X - released.X) < WALKED);
	CHECK(std::abs(stillThere.Z - released.Z) < WALKED);

	// **A quarter turn, and W now means what the camera means.** This is the
	// whole reason `ReadMoveIntent` reads the `CameraController` at all - a game
	// whose forward key stopped meaning forward when the camera turned is the
	// one thing every player notices immediately. Yaw is measured about +Y, so a
	// positive quarter turn puts "away from the camera" along -X.
	client.LookAlong(std::numbers::pi_v<float> * 0.5f);
	const engine::core::Vector3 turned = client.Hold({KeyCode::W}, HELD);
	CHECK(turned.X < stillThere.X - WALKED);

	// **And the space bar leaves the floor.** Jumping is the half that fails on
	// its own: walking writes a horizontal velocity whatever the ground says,
	// while a jump reads `Humanoid::Grounded` and does nothing without it - so a
	// broken ground query is invisible to every check above and is the entire
	// bug to somebody holding space. Sampled at every tick because the apex is
	// between two of them.
	client.LookAlong(0.0f);
	client.Press({});
	client.Tick();

	const float ground = client.MyPosition().Y;

	// **Tapped rather than held, and the alternation is the whole of it.** A
	// jump is an edge - `ReadMoveIntent` asks `WasKeyPressed` - so space held
	// down is one edge and therefore one submission, and the input channel is
	// unreliable by design: a single datagram that does not arrive is a jump
	// that silently did not happen. Releasing between presses is what a player
	// does with a jump key anyway, and it makes the test measure the behaviour
	// instead of the loss rate.
	float apex = ground;
	for (int tick = 0; tick < HELD; tick++) {
		client.Press(
			tick % 2 == 0 ? std::initializer_list<KeyCode>{KeyCode::Space} : std::initializer_list<KeyCode>{}
		);
		client.SubmitMove();
		client.Tick();
		apex = std::max(apex, client.MyPosition().Y);
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	CHECK(apex > ground + WALKED);

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

TEST_CASE("a client holds its own containers and none of anybody else's", "[server][replication]") {
	// **The interest predicate, asserted in both directions over a real wire.**
	// A check that only proved a client can see its own `PlayerGui` would pass
	// against a server that sent everybody everything, which is exactly the
	// state this repository was in until v0.15 and exactly what the four
	// containers made worse: a `Backpack` is what a game keys "what am I
	// holding" off, and one client reading another's would be reading their
	// inventory.
	//
	// **From the replica rather than from the server's log**, for the reason the
	// case above gives: what a client holds is the only fact about this that a
	// test can hold.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_private.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local block = Instance.new(\"Part\")\n"
			 << "block.Anchored = true\n"
			 << "block.Parent = workspace\n"
			 << "local secret = Instance.new(\"Part\")\n"
			 << "secret.Name = \"ServerOnlySecret\"\n"
			 << "secret.Parent = game:GetService(\"ServerStorage\")\n"

			 // **A `Tool` in `StarterPack`, because a container is only private
			 // if what is *in* it is.** The four containers arriving empty proved
			 // the predicate hides a row under a player; a game keys "what am I
			 // holding" off the contents, and a client reading another's
			 // `Backpack` reads their inventory. `AddPlayer` clones this into
			 // each occupant's `StarterGear` and the spawn refills `Backpack`
			 // from that, so each client ends up with a copy of its own.
			 << "local tool = Instance.new(\"Tool\")\n"
			 << "tool.Name = \"Sword\"\n"
			 << "local handle = Instance.new(\"Part\")\n"
			 << "handle.Name = \"Handle\"\n"
			 << "handle.Parent = tool\n"
			 << "tool.Parent = game:GetService(\"StarterPack\")\n";
	}

	Remote first;
	REQUIRE(first.Start(0, scene.string()));
	REQUIRE(first.Join(400));

	Remote second;
	REQUIRE(second.Connect(first.Port));
	REQUIRE(second.Join(400));

	Settle(first);
	Settle(second);

	REQUIRE(first.Mine != engine::ecs::NULL_ENTITY);
	REQUIRE(second.Mine != engine::ecs::NULL_ENTITY);
	REQUIRE(first.Mine != second.Mine);

	const auto children = [](const Store &world, Entity parent) {
		size_t found = 0;
		world.EachChild(parent, [&](Entity) { found++; });
		return found;
	};

	// **Both `Player` rows are public**, which is the half a stricter predicate
	// would get wrong: `GetPlayers` is how a game knows who is in it, and a
	// client shown only its own row would think it was alone.
	CHECK(first.World.Alive(first.Mine));
	CHECK(first.World.Alive(second.Mine));

	// **And what is under one is not.** Four containers of its own, none of
	// theirs - and they are asked for **by name**, which is the assertion this
	// case could not make until v0.15.
	//
	// The containers are built by `AddPlayer` when a client joins, so they are
	// exactly the case `replication::DefaultReplicatedComponents` used to get
	// wrong: it admitted `ecs.Hierarchy` and the `scene.` prefix and nothing else
	// from `ecs.`, so `ecs.InstanceName` crossed in a join snapshot and never in
	// a delta and every one of these arrived called nothing at all. Counting
	// anonymous handles was the workaround; a name is what a game actually holds
	// them by.
	for (const std::string_view container : {"PlayerGui", "PlayerScripts", "Backpack", "StarterGear"}) {
		INFO("container: " << container);
		CHECK(first.World.FindFirstChild(first.Mine, container) != engine::ecs::NULL_ENTITY);
		CHECK(second.World.FindFirstChild(second.Mine, container) != engine::ecs::NULL_ENTITY);
	}

	CHECK(children(first.World, first.Mine) == 4);
	CHECK(children(first.World, second.Mine) == 0);

	// The same from the other side, so the answer is per client rather than
	// "the first one to ask wins".
	CHECK(children(second.World, second.Mine) == 4);
	CHECK(children(second.World, first.Mine) == 0);

	// The service itself and all of its descendants stay out of every replica.
	// A named row makes this an end-to-end secrecy check rather than a count that
	// would pass if the server sent the service but not its contents.
	const auto named = [](Store &world, std::string_view name) {
		bool found = false;
		world.EachEntity([&](Entity entity) {
			if (world.InstanceNameOf(entity) == Name(name)) found = true;
		});
		return found;
	};
	CHECK_FALSE(named(first.World, "ServerOnlySecret"));
	CHECK_FALSE(named(second.World, "ServerOnlySecret"));

	// **And the gear inside them, which is what a `Tool` made worth asserting.**
	// Four empty containers prove the predicate hides a *row* under a player; a
	// game keys "what am I holding" off the contents, so what has to be proved is
	// that one client cannot count the other's. Each occupant has two copies of
	// the template - one in `StarterGear` and the one the spawn refilled
	// `Backpack` with - and none of anybody else's.
	//
	// **Named at every step, container and tool alike.** `Sword` is what the
	// scene called it, so a client holding a `Tool` under an unnamed parent, or
	// a named container holding an unnamed tool, is a failure here rather than a
	// count that still comes to two.
	const auto swordIn = [](Store &world, Entity player, std::string_view container) {
		const Entity holder = world.FindFirstChild(player, container);
		return holder != engine::ecs::NULL_ENTITY &&
			   world.FindFirstChild(holder, "Sword") != engine::ecs::NULL_ENTITY;
	};

	CHECK(swordIn(first.World, first.Mine, "Backpack"));
	CHECK(swordIn(first.World, first.Mine, "StarterGear"));

	// The same from the other side, so the answer is per client here too.
	CHECK(swordIn(second.World, second.Mine, "Backpack"));
	CHECK(swordIn(second.World, second.Mine, "StarterGear"));

	// And neither can reach into the other's, which is the half the naming does
	// not change: the interest predicate is what keeps one client's inventory out
	// of another's world, and asking by name would find it if it were there.
	CHECK_FALSE(swordIn(first.World, second.Mine, "Backpack"));
	CHECK_FALSE(swordIn(second.World, first.Mine, "Backpack"));

	// The count stays, because "two, and not one or three" is a different claim
	// from "the named one is there" - a spawn that refilled `Backpack` twice
	// would pass every assertion above.
	const auto toolsUnder = [](Store &world, Entity player) {
		size_t found = 0;
		world.Each<const engine::scene::Tool>([&](Entity tool, const engine::scene::Tool &) {
			for (Entity at = tool; at != engine::ecs::NULL_ENTITY; at = world.ParentOf(at)) {
				if (at == player) {
					found++;
					return;
				}
			}
		});
		return found;
	};

	CHECK(toolsUnder(first.World, first.Mine) == 2);
	CHECK(toolsUnder(first.World, second.Mine) == 0);
	CHECK(toolsUnder(second.World, second.Mine) == 2);
	CHECK(toolsUnder(second.World, first.Mine) == 0);

	// Drop the second connection, then admit a new one. The listener owns the
	// transport timeout, so this case only relies on the new connection getting
	// its own private state rather than on a particular time for slot reuse.
	second.Link.reset();
	second.Socket->Close();
	for (int tick = 0; tick < 60; tick++) {
		first.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	Remote reconnected;
	REQUIRE(reconnected.Connect(first.Port));
	REQUIRE(reconnected.Join(400));
	Settle(first);
	Settle(reconnected);

	REQUIRE(reconnected.Mine != engine::ecs::NULL_ENTITY);
	CHECK(swordIn(reconnected.World, reconnected.Mine, "Backpack"));
	CHECK_FALSE(swordIn(reconnected.World, first.Mine, "Backpack"));

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}
