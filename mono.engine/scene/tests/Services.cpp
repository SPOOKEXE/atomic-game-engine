// The fixtures a world is furnished with, and the one property that matters
// about furnishing it: doing it twice changes nothing.
//
// **Idempotence is the whole contract.** The studio calls `InstallServices` on
// every world it makes *and* on every world it loads, because a game file
// written before services existed has none and one written after has them all.
// If the second call created a second `Workspace`, every author who opened an
// old game would get a duplicate holding none of their scene - and the first
// symptom would be a script resolving the empty one.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.services")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::scene::InstallServices;
using engine::scene::LightingServiceComponent;
using engine::scene::ServiceComponent;
using engine::scene::ServiceScope;
using engine::scene::WorkspaceOf;

namespace services_test {
	void Ready() {
		engine::scene::RegisterSceneClasses();
	}

	// How many children a container holds. `Store::HasChildren` answers whether
	// there are any and nothing answers how many, because nothing in the engine
	// needs the number - a test does, and counting the walk here is cheaper than
	// widening a public header for it.
	size_t Children(const Store &store, Entity container) {
		size_t found = 0;
		store.EachChild(container, [&](Entity) { found++; });
		return found;
	}

	size_t RootsNamed(const Store &store, std::string_view name) {
		size_t found = 0;
		store.EachRoot([&](Entity root) {
			if (store.InstanceNameOf(root) == Name(name)) {
				found++;
			}
		});
		return found;
	}
}

TEST_CASE("a furnished world has every fixture, once", "[scene][services]") {
	services_test::Ready();

	Store store("services.once");
	const Entity workspace = InstallServices(store);

	REQUIRE(workspace != NULL_ENTITY);
	CHECK(workspace == WorkspaceOf(store));

	for (const std::string_view name :
		 {"Workspace",
		  "Lighting",
		  "ReplicatedFirst",
		  "ReplicatedStorage",
		  "ServerScriptService",
		  "ServerStorage",
		  "StarterGui",
		  "StarterPack",
		  "StarterPlayer"}) {
		CHECK(services_test::RootsNamed(store, name) == 1);
	}

	// **Neither is a root.** Roblox nests both under `StarterPlayer` and so
	// does this; a `StarterPlayerScripts` beside its owner is one a client would
	// not find where it looks for it.
	CHECK(services_test::RootsNamed(store, "StarterPlayerScripts") == 0);
	CHECK(services_test::RootsNamed(store, "StarterCharacterScripts") == 0);
	const Entity starterPlayer = store.FindFirstRoot("StarterPlayer");
	REQUIRE(starterPlayer != NULL_ENTITY);
	CHECK(store.FindFirstChild(starterPlayer, "StarterPlayerScripts") != NULL_ENTITY);
	CHECK(store.FindFirstChild(starterPlayer, "StarterCharacterScripts") != NULL_ENTITY);

	// **No camera, and its absence is the contract.** A camera belongs to
	// whoever is looking rather than to the game: the editor makes one for its
	// viewport, a client makes one for its player, and several people editing
	// one game make one each. Furnishing a world with one would write somebody's
	// viewpoint into every file made from it - see `scene::TransientComponent`,
	// and `studio::Editor::EnsureViewerCamera` for who does make it.
	CHECK(store.FindFirstChild(workspace, "Camera") == NULL_ENTITY);
}

TEST_CASE("installing twice furnishes nothing twice", "[scene][services]") {
	services_test::Ready();

	Store store("services.twice");
	const Entity first = InstallServices(store);

	// Something an author put in the workspace, which is what a duplicate
	// `Workspace` would strand.
	store.SetParent(store.CreateInstance(engine::scene::PartClass(), "Floor"), first);

	const Entity second = InstallServices(store);

	CHECK(first == second);
	CHECK(services_test::RootsNamed(store, "Workspace") == 1);
	CHECK(services_test::RootsNamed(store, "Lighting") == 1);
	CHECK(store.FindFirstChild(first, "Floor") != NULL_ENTITY);

	// And still no camera: the viewer's, not the world's.
	size_t cameras = 0;
	store.EachChild(first, [&](Entity child) {
		if (store.InstanceNameOf(child) == Name("Camera")) {
			cameras++;
		}
	});
	CHECK(cameras == 0);
}

TEST_CASE("a service carries its scope and Lighting carries more", "[scene][services]") {
	services_test::Ready();

	Store store("services.components");
	InstallServices(store);

	// **The shared component is what makes "is this a fixture" a query.** Nine
	// class-name comparisons at each call site is the version that drifts the
	// first time a tenth service is added.
	const Entity storage = store.FindFirstRoot("ServerStorage");
	REQUIRE(storage != NULL_ENTITY);

	const ServiceComponent *service = store.Get<ServiceComponent>(storage);
	REQUIRE(service != nullptr);
	CHECK(service->Scope == ServiceScope::Server);
	CHECK(service->Fixture);

	const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
	REQUIRE(store.Get<ServiceComponent>(replicated) != nullptr);
	CHECK(store.Get<ServiceComponent>(replicated)->Scope == ServiceScope::Shared);

	CHECK(store.Get<ServiceComponent>(store.FindFirstRoot("StarterGui"))->Scope == ServiceScope::Client);

	// Lighting alone has the second component. Eight unused floats on the other
	// eight services is eight floats in every snapshot of every world.
	const Entity lighting = store.FindFirstRoot("Lighting");
	REQUIRE(lighting != NULL_ENTITY);
	REQUIRE(store.Get<LightingServiceComponent>(lighting) != nullptr);
	CHECK(store.Get<LightingServiceComponent>(lighting)->ClockTime == 14.0f);
	CHECK(store.Get<LightingServiceComponent>(storage) == nullptr);

	// The scope reads back as a word through the property surface, which is
	// what a script and a properties panel both see.
	Name scope;
	REQUIRE(store.GetProperty(storage, Name("Scope"), &scope, sizeof(scope)));
	CHECK(scope == Name("Server"));
}

TEST_CASE("a server-scoped service and everything under it is hidden from clients", "[scene][services]") {
	// **The thing `ServiceComponent::Scope` was declared for in v0.7 and that
	// nothing did until v0.15.** It round-tripped through save files and showed
	// in the properties panel while the wire ignored it, so a game's server
	// scripts and its unreleased content were replicated to every client. This
	// is the rule; `replication::Authority::Interest` is where a host hangs it.
	services_test::Ready();

	Store store("services.scope");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity serverStorage = store.FindFirstRoot("ServerStorage");
	const Entity serverScripts = store.FindFirstRoot("ServerScriptService");
	const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
	const Entity workspace = WorkspaceOf(store);

	REQUIRE(serverStorage != NULL_ENTITY);
	REQUIRE(serverScripts != NULL_ENTITY);
	REQUIRE(replicated != NULL_ENTITY);

	CHECK_FALSE(engine::scene::VisibleToClients(store, serverStorage));
	CHECK_FALSE(engine::scene::VisibleToClients(store, serverScripts));
	CHECK(engine::scene::VisibleToClients(store, replicated));
	CHECK(engine::scene::VisibleToClients(store, workspace));

	// **Containment and not the row**, which is the whole reason this walks. The
	// same instance moved from one service to another must change answer with
	// nothing else edited - a flag copied onto descendants would be the version
	// that goes stale on a reparent.
	const Entity secret = store.CreateInstance(Classes::Find(Name("Part")), "Secret");
	REQUIRE(secret != NULL_ENTITY);

	store.SetParent(secret, serverStorage);
	CHECK_FALSE(engine::scene::VisibleToClients(store, secret));

	store.SetParent(secret, workspace);
	CHECK(engine::scene::VisibleToClients(store, secret));
}

TEST_CASE("an orphan is shared rather than secret", "[scene][services]") {
	// **The safe answer is the permissive one here, which is worth stating
	// because it reads backwards.** An instance a script has created and not yet
	// parented is under no service, so it has no scope to read. Calling that
	// `Server` would hide it - and a client that skipped it while it was an
	// orphan would still be missing it after the script parented it into
	// `Workspace`, because the wire sends what changed and nothing changed on
	// the row. A missing part is a bug nobody can find; an orphan is not secret
	// yet, and becomes so the moment it is parented somewhere that says so.
	services_test::Ready();

	Store store("services.orphan");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity loose = store.CreateInstance(Classes::Find(Name("Part")), "Loose");
	REQUIRE(loose != NULL_ENTITY);
	CHECK(engine::scene::ScopeOfInstance(store, loose) == ServiceScope::Shared);
	CHECK(engine::scene::VisibleToClients(store, loose));
}

TEST_CASE("what is under a player belongs to that player", "[scene][services]") {
	// **Scope cannot say this, and that is why there are two predicates.**
	// `Players` is `Shared` because both halves of a game need the list of who
	// is connected - so the service is visible to everyone and what is under one
	// player's row is not. A second client shown somebody else's `PlayerGui`
	// gets an interface it cannot interact with, and a script writing into one
	// would be writing into everybody's.
	services_test::Ready();

	Store store("services.owned");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity first = engine::scene::AddPlayer(store, "First");
	const Entity second = engine::scene::AddPlayer(store, "Second");
	REQUIRE(first != NULL_ENTITY);
	REQUIRE(second != NULL_ENTITY);

	CHECK(engine::scene::PlayerOwning(store, first) == first);
	CHECK(engine::scene::PlayerOwning(store, second) == second);

	// The container `AddPlayer` makes beside every player, which is the whole
	// reason this predicate exists - see `Player.PlayerGui`.
	const Entity gui = store.FindFirstChild(first, engine::scene::PLAYER_GUI_NAME);
	REQUIRE(gui != NULL_ENTITY);
	CHECK(engine::scene::PlayerOwning(store, gui) == first);

	const Entity label = store.CreateInstance(Classes::Find(Name("Instance")), "Label");
	REQUIRE(label != NULL_ENTITY);
	store.SetParent(label, gui);
	CHECK(engine::scene::PlayerOwning(store, label) == first);

	// **Everything else answers nobody**, which is almost everything: a part in
	// `Workspace` is not any player's, and a predicate that said otherwise would
	// hide the world from every client but one.
	CHECK(engine::scene::PlayerOwning(store, WorkspaceOf(store)) == NULL_ENTITY);
	CHECK(engine::scene::PlayerOwning(store, store.FindFirstRoot("Players")) == NULL_ENTITY);
}

TEST_CASE("a renamed fixture is still the fixture", "[scene][services]") {
	// **The bug this closes, and it was live from v0.7 to v0.17.** Every fixture
	// lookup was `FindFirstRoot(name)`, so a script renaming the workspace made
	// `WorkspaceOf` answer nothing - and `InstallServices`, which used that same
	// lookup to decide what a world was missing, then minted a *second*
	// `Workspace` beside the one holding the scene. Two workspaces, one of which
	// holds everything and neither of which a script can be sure it has.
	//
	// Against the class instead of the name, the rename is a rename.
	services_test::Ready();

	Store store("services.renamed");
	const Entity workspace = InstallServices(store);
	REQUIRE(workspace != NULL_ENTITY);

	const Entity floor = store.CreateInstance(engine::scene::PartClass(), "Floor");
	store.SetParent(floor, workspace);

	REQUIRE(store.SetInstanceName(workspace, "TheWorld"));
	REQUIRE(store.SetInstanceName(store.FindFirstRoot("Players"), "Everybody"));

	// Both still resolve, which is what a class lookup buys.
	CHECK(WorkspaceOf(store) == workspace);
	CHECK(engine::scene::PlayersOf(store) != NULL_ENTITY);

	// And furnishing again finds them rather than building beside them.
	CHECK(InstallServices(store) == workspace);
	CHECK(services_test::RootsNamed(store, "Workspace") == 0);
	CHECK(services_test::RootsNamed(store, "TheWorld") == 1);
	CHECK(store.FindFirstChild(workspace, "Floor") == floor);

	// The nested pair is found the same way, so a renamed `StarterPlayer` does
	// not strand the two containers under it either.
	const Entity starterPlayer = store.FindFirstRoot("StarterPlayer");
	REQUIRE(starterPlayer != NULL_ENTITY);
	REQUIRE(store.SetInstanceName(starterPlayer, "Everyone"));
	REQUIRE(InstallServices(store) == workspace);
	CHECK(services_test::RootsNamed(store, "StarterPlayer") == 0);
	CHECK(services_test::Children(store, starterPlayer) == 2);
}

TEST_CASE("a join builds four containers and copies two templates", "[scene][services]") {
	// **The join half of the `Starter*` pipeline, and the two copies it makes
	// are once-only by design.** `StarterPlayerScripts` is the client's own code
	// and `StarterPack` is the gear a game starts somebody with; a respawn
	// refills `Backpack` from `StarterGear` and never touches either of these
	// again, which is what makes a tool granted at run time survive a death.
	services_test::Ready();

	Store store("services.join");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const auto plain = Classes::Find(Name("Instance"));
	const Entity starterPlayer = store.FindFirstRoot("StarterPlayer");
	const Entity playerScripts = store.FindFirstChild(starterPlayer, "StarterPlayerScripts");
	const Entity pack = store.FindFirstRoot("StarterPack");
	REQUIRE(playerScripts != NULL_ENTITY);
	REQUIRE(pack != NULL_ENTITY);

	store.SetParent(store.CreateInstance(plain, "CameraScript"), playerScripts);
	store.SetParent(store.CreateInstance(plain, "Sword"), pack);

	const Entity player = engine::scene::AddPlayer(store, "First");
	REQUIRE(player != NULL_ENTITY);

	for (const std::string_view container :
		 {engine::scene::PLAYER_GUI_NAME,
		  engine::scene::PLAYER_SCRIPTS_NAME,
		  engine::scene::BACKPACK_NAME,
		  engine::scene::STARTER_GEAR_NAME}) {
		CHECK(store.FindFirstChild(player, container) != NULL_ENTITY);
	}

	CHECK(store.FindFirstChild(store.FindFirstChild(player, "PlayerScripts"), "CameraScript") != NULL_ENTITY);
	CHECK(store.FindFirstChild(store.FindFirstChild(player, "StarterGear"), "Sword") != NULL_ENTITY);

	// **The backpack is empty on a join**, because filling it is the *spawn's*
	// step and a player who joins with `CharacterAutoLoads` off has no life yet.
	CHECK(services_test::Children(store, store.FindFirstChild(player, "Backpack")) == 0);

	// **Cloned rather than moved**, so the next occupant gets the same start.
	CHECK(services_test::Children(store, playerScripts) == 1);
	CHECK(services_test::Children(store, pack) == 1);

	const Entity second = engine::scene::AddPlayer(store, "Second");
	REQUIRE(second != NULL_ENTITY);

	// Their own copies rather than shared handles: a script editing one
	// player's gear must not edit everybody's.
	const Entity firstSword = store.FindFirstChild(store.FindFirstChild(player, "StarterGear"), "Sword");
	const Entity secondSword = store.FindFirstChild(store.FindFirstChild(second, "StarterGear"), "Sword");
	REQUIRE(secondSword != NULL_ENTITY);
	CHECK(firstSword != secondSword);
}

TEST_CASE("a player has an identity and the world is bounded", "[scene][services]") {
	// **Three properties that had no reader until they had one.** `MaxPlayers`
	// is enforced by the one door a player arrives through, `UserId` is handed
	// out by a counter on the world so two runs of a recording agree, and
	// `NumPlayers` is counted rather than kept - a field incremented on arrival
	// is the copy that goes wrong the first time a script destroys somebody.
	services_test::Ready();

	Store store("services.identity");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity players = engine::scene::PlayersOf(store);
	auto *settings = store.GetMutable<engine::scene::PlayersServiceComponent>(players);
	REQUIRE(settings != nullptr);
	settings->MaxPlayers = 2;
	settings->RespawnTime = 3.0f;

	const Entity first = engine::scene::AddPlayer(store, "First");
	const Entity second = engine::scene::AddPlayer(store, "Second", false, 4242);
	REQUIRE(first != NULL_ENTITY);
	REQUIRE(second != NULL_ENTITY);

	// **Refused rather than admitted past the cap**, which is what makes the
	// property a rule instead of a label.
	CHECK(engine::scene::AddPlayer(store, "Third") == NULL_ENTITY);
	CHECK(engine::scene::PlayerCount(store) == 2);

	const auto *identity = store.Get<engine::scene::PlayerIdentity>(first);
	REQUIRE(identity != nullptr);
	CHECK(identity->UserId == 1);
	CHECK(identity->DisplayName == Name("First"));

	// The world's respawn delay is what a new occupant starts from, and their
	// own copy is what they actually wait.
	CHECK(identity->RespawnTime == 3.0f);

	// A number the host supplied wins, and the counter is not spent on it.
	CHECK(store.Get<engine::scene::PlayerIdentity>(second)->UserId == 4242);
	CHECK(engine::scene::PlayerByUserId(store, 1) == first);
	CHECK(engine::scene::PlayerByUserId(store, 4242) == second);
	CHECK(engine::scene::PlayerByUserId(store, 7) == NULL_ENTITY);
}

TEST_CASE("only ReplicatedFirst is replicated first", "[scene][services]") {
	// **The container had no reader at all**, so a loading screen in it arrived
	// somewhere in the middle of the world it was meant to cover. The predicate
	// is containment rather than a flag, for `ScopeOfInstance`'s reason: moving
	// something out of the service has to change the answer with nothing else
	// being edited.
	services_test::Ready();

	Store store("services.first");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity first = store.FindFirstRoot("ReplicatedFirst");
	const Entity shared = store.FindFirstRoot("ReplicatedStorage");
	REQUIRE(first != NULL_ENTITY);

	const auto plain = Classes::Find(Name("Instance"));
	const Entity screen = store.CreateInstance(plain, "LoadingScreen");
	store.SetParent(screen, first);

	const Entity deep = store.CreateInstance(plain, "Logo");
	store.SetParent(deep, screen);

	CHECK(engine::scene::InReplicatedFirst(store, first));
	CHECK(engine::scene::InReplicatedFirst(store, screen));
	CHECK(engine::scene::InReplicatedFirst(store, deep));

	CHECK(!engine::scene::InReplicatedFirst(store, shared));
	CHECK(!engine::scene::InReplicatedFirst(store, WorkspaceOf(store)));

	// Moved out, it stops being first - with nothing else edited.
	store.SetParent(screen, shared);
	CHECK(!engine::scene::InReplicatedFirst(store, screen));
	CHECK(!engine::scene::InReplicatedFirst(store, deep));
}

TEST_CASE("every player container is that player's alone", "[scene][services]") {
	// **The scoping rule, asserted in both directions.** A test that only
	// checked a player can see their own container would pass against a
	// predicate that let everybody see everything, which is exactly the failure
	// this exists to prevent.
	services_test::Ready();

	Store store("services.private");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity mine = engine::scene::AddPlayer(store, "Mine");
	const Entity theirs = engine::scene::AddPlayer(store, "Theirs");
	REQUIRE(mine != NULL_ENTITY);
	REQUIRE(theirs != NULL_ENTITY);

	for (const std::string_view container :
		 {engine::scene::PLAYER_GUI_NAME,
		  engine::scene::PLAYER_SCRIPTS_NAME,
		  engine::scene::BACKPACK_NAME,
		  engine::scene::STARTER_GEAR_NAME}) {
		const Entity ours = store.FindFirstChild(mine, container);
		const Entity other = store.FindFirstChild(theirs, container);
		REQUIRE(ours != NULL_ENTITY);
		REQUIRE(other != NULL_ENTITY);

		CHECK(engine::scene::PlayerOwning(store, ours) == mine);
		CHECK(engine::scene::PlayerOwning(store, other) == theirs);

		// The half that fails if the predicate is too generous.
		CHECK(engine::scene::PlayerOwning(store, other) != mine);

		// Every one of them is still shared-scope, because the *service* is -
		// the two rules answer different questions and both have to hold.
		CHECK(engine::scene::VisibleToClients(store, ours));
	}

	// A player's own row is nobody's private business: `GetPlayers` is how a
	// game knows who is in it, and a client shown only its own row would think
	// it was alone.
	CHECK(engine::scene::PlayerOwning(store, mine) == mine);
	CHECK(engine::scene::PlayerOwning(store, theirs) == theirs);
}

TEST_CASE("an author may not destroy or reparent a fixture", "[scene][services][fixture]") {
	// **`ServiceComponent::Fixture` refused nothing until v0.15.** The field
	// carried the sentence - a world with no `Workspace` is not a world an
	// author meant to build, and deleting one turns every `game:GetService` in
	// the place into a runtime error a long way from the delete that caused it -
	// and a script could still `Destroy()` `Lighting`, and the editor could
	// still delete it with the Delete key. That is rule 6 in its plainest form:
	// the constraint was documentation.
	services_test::Ready();

	Store store("fixtures");
	const Entity workspace = InstallServices(store);
	REQUIRE(workspace != NULL_ENTITY);

	const Entity lighting = store.FindFirstRoot("Lighting");
	REQUIRE(lighting != NULL_ENTITY);

	CHECK(store.Protected(workspace));
	CHECK(store.Protected(lighting));

	// The authored doors refuse and change nothing.
	CHECK_FALSE(store.DestroyAuthored(lighting));
	CHECK(store.Alive(lighting));

	CHECK_FALSE(store.SetParentAuthored(lighting, workspace));
	CHECK(store.ParentOf(lighting) == NULL_ENTITY);

	// **And the engine's own move still goes through**, which is the half a
	// blanket guard would have broken: `studio::PlayLink` destroying a player,
	// `Debris` draining its queue and `RojoSync` rebuilding a subtree are all
	// this call, and every one of them is the engine moving its own furniture.
	store.DestroyInstance(lighting);
	CHECK_FALSE(store.Alive(lighting));
}

TEST_CASE("anything an author made is theirs to remove", "[scene][services][fixture]") {
	// The other direction, and the one that says the guard is narrow: a part is
	// not a fixture and nothing about this may make it feel like one.
	services_test::Ready();

	Store store("fixtures.ordinary");
	const Entity workspace = InstallServices(store);
	REQUIRE(workspace != NULL_ENTITY);

	const Entity part = store.CreateInstance(engine::scene::PartClass(), "Block");
	REQUIRE(part != NULL_ENTITY);
	REQUIRE(store.SetParentAuthored(part, workspace));

	CHECK_FALSE(store.Protected(part));
	CHECK(store.SetParentAuthored(part, NULL_ENTITY));
	CHECK(store.DestroyAuthored(part));
	CHECK_FALSE(store.Alive(part));
}

TEST_CASE("a world read out of a file is protected too", "[scene][services][fixture]") {
	// **The half that would have been missing, and the worst one to miss.** The
	// install loop *finds* a saved world's services rather than making them, so
	// protection applied only where an instance was minted would hold in a new
	// game and not in a saved one - and a rule that silently does not apply is
	// the failure this guard is about wearing a different hat.
	services_test::Ready();

	Store store("fixtures.reopened");
	REQUIRE(InstallServices(store) != NULL_ENTITY);

	const Entity lighting = store.FindFirstRoot("Lighting");
	REQUIRE(lighting != NULL_ENTITY);

	// A second install is what a reopened world gets: everything is found, not
	// created. Nothing new is protected and everything still is.
	REQUIRE(InstallServices(store) != NULL_ENTITY);
	CHECK(store.Protected(lighting));
	CHECK_FALSE(store.DestroyAuthored(lighting));
}
