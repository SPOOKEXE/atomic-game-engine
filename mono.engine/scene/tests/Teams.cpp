// Sides, and the pads that belong to them.
//
// **The case that decides whether this was worth building is the third one.**
// `docs/DEFERRED.md` D00119 refused `Player.Team` on the grounds that a team
// whose only effect is a coloured name is a field rather than a feature — so
// what has to be proved is not that the property stores an entity, but that
// joining a side changes where a body appears.
//
// The first case is the compatibility one, and it is the reason the class is
// called what it is: `scene::FindSpawn` looked for a part *named*
// `SpawnLocation` from v0.14 to v0.15, every scene in `mono.engine/examples`
// still builds one that way, and both spellings have to land a character on
// exactly the same face of exactly the same pad.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Teams.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.teams")
TEST_DEPENDS("engine.scene.part")

using Catch::Approx;
using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::AddPlayer;
using engine::scene::AddTeam;
using engine::scene::Character;
using engine::scene::FindSpawn;
using engine::scene::LoadCharacter;
using engine::scene::MakePart;
using engine::scene::PartDesc;
using engine::scene::SameTeamColour;
using engine::scene::SetPlayerTeam;
using engine::scene::SpawnLocation;
using engine::scene::SpawnLocationClass;
using engine::scene::Team;
using engine::scene::TeamOf;
using engine::scene::TeamsOf;
using engine::scene::Transform;
using engine::scene::WorkspaceOf;

namespace {
	// Two sides far enough apart in linear light that no tolerance could
	// confuse them, and neither of them the default grey.
	constexpr Color3 RED{0.72f, 0.05f, 0.04f};
	constexpr Color3 BLUE{0.04f, 0.16f, 0.68f};

	// A furnished world. The same fixture `Characters.cpp` uses, minus the two
	// control resources, because nothing here drives anything.
	struct World {
		Store Store_{"teams-test"};

		World() {
			engine::scene::RegisterSceneClasses();
			engine::scene::InstallServices(Store_);
		}
	};

	// A pad, built through the one constructor a part has.
	//
	// `PartDesc::Class` is what lets a `SpawnLocation` go through `MakePart`
	// rather than being assembled by hand — the second definition
	// `scene/AGENTS.md` refuses.
	Entity Pad(Store &store, const Vector3 &where, engine::ecs::ClassId klass, std::string_view name) {
		PartDesc desc;
		desc.Frame = CFrame(where);
		desc.Size = Vector3{12.0f, 2.0f, 12.0f};
		desc.Anchored = true;
		desc.Class = klass;

		const Entity pad = MakePart(store, desc);
		REQUIRE(pad != NULL_ENTITY);
		store.SetInstanceName(pad, name);
		store.SetParent(pad, WorkspaceOf(store));
		return pad;
	}

	// Which side a pad belongs to, and whether anybody else may use it.
	void Claim(Store &store, Entity pad, const Color3 &colour, bool neutral) {
		SpawnLocation spawn;
		spawn.TeamColour = colour;
		spawn.Neutral = neutral;
		store.Set(pad, spawn);
	}

	// Where a player's body actually ended up, which is the only answer that
	// matters — `FindSpawn` agreeing with itself proves nothing about the
	// pipeline that reads it.
	Vector3 SpawnedAt(Store &store, Entity player) {
		const Entity model = LoadCharacter(store, player);
		REQUIRE(model != NULL_ENTITY);

		const Character *rig = store.Get<Character>(model);
		REQUIRE(rig != nullptr);
		return store.Get<Transform>(rig->Root)->Frame.Position;
	}
}

TEST_CASE("a SpawnLocation class stands a character exactly where the named part did", "[scene][teams]") {
	// **The compatibility case, and it is first because it is the one a
	// regression would be silent in.** A world whose pad is a plain `Part`
	// called `SpawnLocation` predates the class by every scene in
	// `mono.engine/examples`; a class that resolved instead of the name would
	// drop all of them at the origin, and nothing in the file would say why.
	CFrame named;
	CFrame classed;

	{
		World world;
		Pad(world.Store_, Vector3{5.0f, 8.0f, 5.0f}, engine::ecs::ClassId{}, "SpawnLocation");
		named = FindSpawn(world.Store_);
	}

	{
		World world;
		const Entity pad = Pad(world.Store_, Vector3{5.0f, 8.0f, 5.0f}, SpawnLocationClass(), "Red Base");

		// The class carries the row and the name does not, which is the whole
		// of the difference — and the class is still a part, so it is drawn and
		// stood on like the block it replaces.
		CHECK(world.Store_.Get<SpawnLocation>(pad) != nullptr);
		CHECK(world.Store_.IsA(pad, engine::ecs::Classes::Find(Name("BasePart"))));

		classed = FindSpawn(world.Store_);
	}

	// The top face of a two-metre pad standing at eight.
	CHECK(named.Position.Y == Approx(9.0f));
	CHECK(classed.Position.X == Approx(named.Position.X));
	CHECK(classed.Position.Y == Approx(named.Position.Y));
	CHECK(classed.Position.Z == Approx(named.Position.Z));
}

TEST_CASE("a player on no team still spawns", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	Pad(store, Vector3{5.0f, 8.0f, 5.0f}, SpawnLocationClass(), "Lobby");

	const Entity player = AddPlayer(store, "Nobody");
	REQUIRE(player != NULL_ENTITY);
	REQUIRE(TeamOf(store, player) == NULL_ENTITY);

	// A default `SpawnLocation` is neutral, so the ordinary world — one pad, no
	// teams, nobody on a side — is unchanged by any of this.
	const Vector3 stood = SpawnedAt(store, player);
	CHECK(stood.Y == Approx(9.0f + engine::scene::CHARACTER_HEIGHT * 0.5f));
	CHECK(stood.X == Approx(5.0f));
}

TEST_CASE("a team spawns on its own colour and never on another's", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	const Entity red = AddTeam(store, "Red", RED);
	const Entity blue = AddTeam(store, "Blue", BLUE);
	REQUIRE(red != NULL_ENTITY);
	REQUIRE(blue != NULL_ENTITY);

	// **Blue's pad is first in tree order**, so a filter that ignored the
	// colour would put everybody on it — which is exactly what the engine did
	// before this existed.
	Claim(store, Pad(store, Vector3{20.0f, 0.0f, 0.0f}, SpawnLocationClass(), "Blue Base"), BLUE, false);
	Claim(store, Pad(store, Vector3{-20.0f, 0.0f, 0.0f}, SpawnLocationClass(), "Red Base"), RED, false);

	const Entity attacker = AddPlayer(store, "Attacker");
	const Entity defender = AddPlayer(store, "Defender");
	REQUIRE(SetPlayerTeam(store, attacker, red));
	REQUIRE(SetPlayerTeam(store, defender, blue));

	CHECK(SpawnedAt(store, attacker).X == Approx(-20.0f));
	CHECK(SpawnedAt(store, defender).X == Approx(20.0f));

	// **Neither pad is neutral, so somebody on no side has nowhere to go** and
	// gets the origin rather than somebody else's base. Roblox's rule, and the
	// honest one: a map with two locked bases has no lobby.
	const Entity spectator = AddPlayer(store, "Spectator");
	CHECK(FindSpawn(store, spectator).Position.X == Approx(0.0f));

	// A lobby added afterwards takes the spectator and takes nobody else: a
	// player's own colour beats a pad anybody may use.
	Claim(store, Pad(store, Vector3{0.0f, 0.0f, 40.0f}, SpawnLocationClass(), "Lobby"), RED, true);

	CHECK(FindSpawn(store, spectator).Position.Z == Approx(40.0f));
	CHECK(SpawnedAt(store, attacker).X == Approx(-20.0f));
}

TEST_CASE("a disabled pad is not a spawn", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	const Entity closed = Pad(store, Vector3{9.0f, 0.0f, 0.0f}, SpawnLocationClass(), "Closed");
	store.GetMutable<SpawnLocation>(closed)->Enabled = false;

	const Entity player = AddPlayer(store, "Nobody");
	CHECK(FindSpawn(store, player).Position.X == Approx(0.0f));

	// **Turned off rather than destroyed**, which is the point of the flag: the
	// geometry is still there and the pad starts working again with one write.
	store.GetMutable<SpawnLocation>(closed)->Enabled = true;
	CHECK(FindSpawn(store, player).Position.X == Approx(9.0f));
}

TEST_CASE("Player.Team is a reference and refuses anything that is not a team", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	const Entity team = AddTeam(store, "Red", RED);
	const Entity player = AddPlayer(store, "Somebody");
	REQUIRE(team != NULL_ENTITY);
	REQUIRE(player != NULL_ENTITY);

	Entity read = NULL_ENTITY;
	REQUIRE(store.GetProperty(player, Name("Team"), &read, sizeof(read)));
	CHECK(read == NULL_ENTITY);

	REQUIRE(store.SetProperty(player, Name("Team"), &team, sizeof(team)));
	REQUIRE(store.GetProperty(player, Name("Team"), &read, sizeof(read)));
	CHECK(read == team);
	CHECK(TeamOf(store, player) == team);

	// **Refused where the mistake was made.** A workspace part assigned here
	// would otherwise sit on the row until a respawn quietly matched nothing.
	const Entity block = MakePart(store, PartDesc{});
	CHECK_FALSE(store.SetProperty(player, Name("Team"), &block, sizeof(block)));
	CHECK(TeamOf(store, player) == team);

	// A destroyed side is nobody's, and the answer is nil rather than a handle
	// to a row that is gone.
	store.DestroyInstance(team);
	CHECK(TeamOf(store, player) == NULL_ENTITY);

	// Clearing is how a game takes somebody off every side.
	const Entity nothing = NULL_ENTITY;
	CHECK(store.SetProperty(player, Name("Team"), &nothing, sizeof(nothing)));
}

TEST_CASE("the Teams service is a protected fixture and installs once", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	const Entity teams = TeamsOf(store);
	REQUIRE(teams != NULL_ENTITY);

	// `InstallServices` is the only filler of `Store::Protect`, and a service
	// that arrived after that rule was written has to be covered by it.
	CHECK(store.Protected(teams));

	// Idempotent, which is what lets the studio run it on every load.
	CHECK(engine::scene::InstallServices(store) != NULL_ENTITY);
	CHECK(TeamsOf(store) == teams);

	size_t roots = 0;
	store.EachRoot([&](Entity root) { roots += store.InstanceNameOf(root) == Name("Teams") ? 1u : 0u; });
	CHECK(roots == 1);
}

TEST_CASE("a side and the pad that names it round-trip through a save", "[scene][teams]") {
	World world;
	Store &store = world.Store_;

	const Entity team = AddTeam(store, "Red", RED);
	const Entity player = AddPlayer(store, "Somebody");
	REQUIRE(SetPlayerTeam(store, player, team));

	const Entity pad = Pad(store, Vector3{-20.0f, 0.0f, 0.0f}, SpawnLocationClass(), "Red Base");
	Claim(store, pad, RED, false);

	ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored("teams-test.restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	// The service, the side, the membership and the pad's allegiance. A file
	// that carried three of the four would load and be wrong — a player back on
	// no team spawns in the lobby, which reads as a game bug rather than a
	// format one.
	REQUIRE(TeamsOf(restored) != NULL_ENTITY);
	REQUIRE(TeamOf(restored, player) == team);

	const Team *side = restored.Get<Team>(team);
	REQUIRE(side != nullptr);
	CHECK(SameTeamColour(side->Colour, RED));

	const SpawnLocation *spawn = restored.Get<SpawnLocation>(pad);
	REQUIRE(spawn != nullptr);
	CHECK(SameTeamColour(spawn->TeamColour, RED));
	CHECK_FALSE(spawn->Neutral);
	CHECK(spawn->Enabled);

	// And the filter still answers over the restored rows, which is the half a
	// component-by-component check would miss.
	CHECK(FindSpawn(restored, player).Position.X == Approx(-20.0f));
}

TEST_CASE("two colours are one side within a tolerance and not beyond it", "[scene][teams]") {
	// **A tolerance rather than an equality**, because a `Color3` reaches this
	// comparison through a property setter, a snapshot and — in the studio — a
	// JSON file that writes floats as text. Exact equality survives the first
	// two and not the third.
	CHECK(SameTeamColour(RED, RED));
	CHECK(SameTeamColour(RED, Color3{RED.R + 0.001f, RED.G, RED.B}));
	CHECK_FALSE(SameTeamColour(RED, Color3{RED.R + 0.02f, RED.G, RED.B}));
	CHECK_FALSE(SameTeamColour(RED, BLUE));
}

TEST_CASE("a team needs the service and a part is not a spawn", "[scene][teams]") {
	// A world nobody furnished. `AddTeam` refuses rather than minting the
	// service on the way past, which is `AddPlayer`'s rule: quietly creating it
	// would hide a caller who skipped `InstallServices`.
	Store bare("teams-test.bare");
	engine::scene::RegisterSceneClasses();
	CHECK(AddTeam(bare, "Red", RED) == NULL_ENTITY);

	// And `MakePart` refuses a class that is not a `BasePart`, so nothing can
	// build a `Model` wearing a part's components.
	PartDesc desc;
	desc.Class = engine::scene::ModelClass();
	CHECK(MakePart(bare, desc) == NULL_ENTITY);
}
