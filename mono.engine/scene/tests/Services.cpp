// The fixtures a world is furnished with, and the one property that matters
// about furnishing it: doing it twice changes nothing.
//
// **Idempotence is the whole contract.** The studio calls `InstallServices` on
// every world it makes *and* on every world it loads, because a game file
// written before services existed has none and one written after has them all.
// If the second call created a second `Workspace`, every author who opened an
// old game would get a duplicate holding none of their scene — and the first
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
		  "StarterPlayer"}) {
		CHECK(services_test::RootsNamed(store, name) == 1);
	}

	// **Not a root.** Roblox nests it under `StarterPlayer` and so does this;
	// a `StarterPlayerScripts` beside its owner is one a client would not find
	// where it looks for it.
	CHECK(services_test::RootsNamed(store, "StarterPlayerScripts") == 0);
	const Entity starterPlayer = store.FindFirstRoot("StarterPlayer");
	REQUIRE(starterPlayer != NULL_ENTITY);
	CHECK(store.FindFirstChild(starterPlayer, "StarterPlayerScripts") != NULL_ENTITY);

	// **No camera, and its absence is the contract.** A camera belongs to
	// whoever is looking rather than to the game: the editor makes one for its
	// viewport, a client makes one for its player, and several people editing
	// one game make one each. Furnishing a world with one would write somebody's
	// viewpoint into every file made from it — see `scene::TransientComponent`,
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
	// nothing else edited — a flag copied onto descendants would be the version
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
	// `Server` would hide it — and a client that skipped it while it was an
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
	// is connected — so the service is visible to everyone and what is under one
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
	// reason this predicate exists — see `Player.PlayerGui`.
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
