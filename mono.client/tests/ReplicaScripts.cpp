// A script running in a world this process does not own, driven end to end.
//
// **Its own suite rather than more of `client.replicated`, because it stands a
// different world up.** That suite is the presentation seam: a bare store, a
// draw list and a snapshot buffer, and no class tree at all. This one needs the
// scene classes, the gui classes, the services, an occupant and two VMs' worth
// of registration - so sharing a file would make every interpolation case pay
// for a class table it never reads, which is the granularity `AGENTS.md` asks
// suites to be split at.
//
// **The order the cases build in is the order a client experiences.** The
// replica is created and its VM opened while the world is still empty, the tree
// arrives afterwards, and only then is the store adopt-only - which is exactly
// `Client::BeginConnecting` followed by the first snapshot. A fixture that
// populated the world before `BuildReplicatedWorld` would be testing
// `RunWorldScripts`, which is the host path and not this one.

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Replicated.hpp>
#include <memory>
#include <string>
#include <string_view>

TEST_SUITE_ID("client.replica.scripts")
TEST_DEPENDS("engine.script.runtime")
TEST_DEPENDS("engine.script.sourcecache")
TEST_DEPENDS("engine.gui.input")
TEST_DEPENDS("engine.gui.services")
TEST_DEPENDS("engine.scene.services")

using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::script::Runtime;

namespace {
	constexpr float FRAME_SECONDS = 1.0f / 60.0f;

	// The two spellings of "append to the log", which is all the programs below
	// differ by. `engine.script.guisurface`'s trick: a chunk's globals are its
	// own and `Run` reports whether it ran rather than what it evaluated to, so
	// the answer crosses as an attribute on the `Workspace`.
	//
	// **An attribute rather than a property, and that is not a workaround.**
	// `ecs::SetAttribute` writes a table on the world; `Store::SetProperty` is
	// refused in a replica. A client script that could log through the second one
	// would mean the trust boundary was not there.
	const char *NOTE = "local function note(mark)\n"
					   "	workspace:SetAttribute('log', (workspace:GetAttribute('log') or '') .. mark)\n"
					   "end\n";

	// A client's replica: the VM opens over an empty world, the world arrives,
	// and then the store belongs to somebody else.
	struct Replica {
		Store World{"client.replica.scripts"};
		Scheduler Systems;
		engine::script::SourceCache Programs;
		std::shared_ptr<Runtime> Scripts;

		Entity Local = NULL_ENTITY;
		Entity Other = NULL_ENTITY;

		Replica() {
			engine::parallel::Jobs::Start(1);

			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();
			engine::gui::RegisterGuiClasses();

			// The VM first, over nothing. This is `Client::BeginConnecting`:
			// the world is created, its systems are installed, and the snapshot
			// has not been asked for yet.
			Scripts = client::BuildReplicatedWorld(World, Systems, {});
		}

		~Replica() {
			engine::parallel::Jobs::Stop();
		}

		Replica(const Replica &) = delete;
		Replica &operator=(const Replica &) = delete;

		// What the authority sent: the fixtures, two occupants and their
		// containers. Written straight into the store because a snapshot is what
		// this stands in for, and this suite is about *which* scripts a replica
		// runs rather than about how a row got there - `client.replica.arrival`
		// is the one that carries every one of these over a real link, and it
		// exists because a hand-built replica cannot tell a component that
		// crosses from one that does not.
		void Arrive() {
			REQUIRE(engine::scene::InstallServices(World) != NULL_ENTITY);
			REQUIRE(engine::gui::InstallGuiServices(World) != NULL_ENTITY);

			Local = engine::scene::AddPlayer(World, "Ada", true, 1);
			Other = engine::scene::AddPlayer(World, "Bob", false, 2);
			REQUIRE(Local != NULL_ENTITY);
			REQUIRE(Other != NULL_ENTITY);
		}

		// The store stops being this process's the moment the authority owns it.
		void Adopt() {
			World.SetResource(Programs);
			World.SetAdoptOnly(true);
		}

		Entity ContainerOf(Entity player, std::string_view name) {
			const Entity found = World.FindFirstChild(player, name);
			REQUIRE(found != NULL_ENTITY);
			return found;
		}

		// One script instance, its program, and where it lives.
		Entity Program(std::string_view name, Entity parent, bool local, const std::string &source) {
			Programs.Set(Name(std::string(name) + ".luau"), source);

			const Entity instance =
				engine::script::MakeScript(World, std::string(name) + ".luau", name, local);
			REQUIRE(instance != NULL_ENTITY);
			if (parent != NULL_ENTITY) {
				REQUIRE(World.SetParent(instance, parent));
			}
			return instance;
		}

		// One tick: adopt what arrived, then beat.
		void Tick() {
			World.SetFrame(FRAME_SECONDS, 0.0f);
			Systems.RunPhases(World, Phase::PreSimulation, Phase::Simulation);
			World.FlushSignals();
		}

		std::string Log() {
			engine::ecs::AttributeValue value;
			if (!engine::ecs::GetAttribute(World, engine::scene::WorkspaceOf(World), Name("log"), value)) {
				return {};
			}
			return value.String;
		}

		std::string Read(const char *attribute) {
			engine::ecs::AttributeValue value;
			if (!engine::ecs::GetAttribute(
					World, engine::scene::WorkspaceOf(World), Name(attribute), value
				)) {
				return {};
			}
			return value.String;
		}
	};
}

TEST_CASE("a client's replica opens a VM of its own", "[client][replication][scripting]") {
	Replica replica;
	REQUIRE(replica.Scripts != nullptr);

	// **The role is what makes a `Script` stay unrun**, and it is the runtime's
	// rather than a filter applied at the call site.
	CHECK_FALSE(replica.Scripts->Role().Server);
	CHECK(replica.Scripts->Role().Client);
}

TEST_CASE(
	"a LocalScript in this client's own tree runs, and a Script does not", "[client][replication][scripting]"
) {
	Replica replica;
	replica.Arrive();

	const Entity mine = replica.ContainerOf(replica.Local, engine::scene::PLAYER_SCRIPTS_NAME);

	replica.Program("Mine", mine, true, std::string(NOTE) + "note('mine ')\n");
	replica.Program("Server", mine, false, std::string(NOTE) + "note('server ')\n");

	replica.Adopt();
	replica.Tick();

	CHECK(replica.Log() == "mine ");
}

TEST_CASE("a LocalScript outside this client's own tree does not run", "[client][replication][scripting]") {
	Replica replica;
	replica.Arrive();

	// Three containers a `LocalScript` can sit in and not be this viewer's:
	// somebody else's player, the template every player is copied from, and the
	// shared world every client can see.
	const Entity theirs = replica.ContainerOf(replica.Other, engine::scene::PLAYER_SCRIPTS_NAME);
	const Entity starterPlayer =
		engine::scene::ServiceOf(replica.World, engine::ecs::Classes::Find(Name("StarterPlayer")));
	REQUIRE(starterPlayer != NULL_ENTITY);
	const Entity template_ = engine::scene::ServiceUnder(
		replica.World, starterPlayer, engine::ecs::Classes::Find(Name("StarterPlayerScripts"))
	);
	REQUIRE(template_ != NULL_ENTITY);

	replica.Program("Theirs", theirs, true, std::string(NOTE) + "note('theirs ')\n");
	replica.Program("Template", template_, true, std::string(NOTE) + "note('template ')\n");
	replica.Program(
		"Loose", engine::scene::WorkspaceOf(replica.World), true, std::string(NOTE) + "note('loose ')\n"
	);

	// And one that must run, so an empty log cannot pass by the world being
	// broken rather than by the rule holding.
	const Entity mine = replica.ContainerOf(replica.Local, engine::scene::PLAYER_SCRIPTS_NAME);
	replica.Program("Mine", mine, true, std::string(NOTE) + "note('mine ')\n");

	replica.Adopt();
	replica.Tick();

	CHECK(replica.Log() == "mine ");
}

TEST_CASE(
	"a LocalScript under ReplicatedFirst runs before anybody is named", "[client][replication][scripting]"
) {
	Replica replica;
	replica.Arrive();

	const Entity first =
		engine::scene::ServiceOf(replica.World, engine::ecs::Classes::Find(Name("ReplicatedFirst")));
	REQUIRE(first != NULL_ENTITY);

	replica.Program("Early", first, true, std::string(NOTE) + "note('early ')\n");

	// The identity is deliberately taken away: `ReplicatedFirst` is everybody's,
	// and its whole point is running ahead of the world a player is put into.
	replica.World.SetResource(engine::scene::LocalPlayer{NULL_ENTITY});

	replica.Adopt();
	replica.Tick();

	CHECK(replica.Log() == "early ");
}

TEST_CASE("a script that arrived is started once and not once per tick", "[client][replication][scripting]") {
	Replica replica;
	replica.Arrive();

	const Entity mine = replica.ContainerOf(replica.Local, engine::scene::PLAYER_SCRIPTS_NAME);
	replica.Program("Once", mine, true, std::string(NOTE) + "note('x')\n");

	replica.Adopt();
	for (int tick = 0; tick < 5; tick++) {
		replica.Tick();
	}

	CHECK(replica.Log() == "x");
}

TEST_CASE("a press on a TextButton in a replica reaches its script", "[client][replication][scripting]") {
	Replica replica;
	replica.Arrive();

	// The interface a client is shown, under its own `PlayerGui`.
	const Entity playerGui = replica.ContainerOf(replica.Local, engine::gui::PLAYER_GUI);

	const Entity screen =
		replica.World.CreateInstance(engine::gui::GuiClass("ScreenGui"), std::string("ScreenGui"));
	REQUIRE(screen != NULL_ENTITY);
	REQUIRE(replica.World.SetParent(screen, playerGui));

	const Entity button =
		replica.World.CreateInstance(engine::gui::GuiClass("TextButton"), std::string("Button"));
	REQUIRE(button != NULL_ENTITY);
	REQUIRE(replica.World.SetParent(button, screen));

	engine::gui::Element element;
	element.Position = UDim2{0.0f, 0.0f, 0.0f, 0.0f};
	element.Size = UDim2{0.0f, 100.0f, 0.0f, 100.0f};
	replica.World.Set(button, element);

	const Entity mine = replica.ContainerOf(replica.Local, engine::scene::PLAYER_SCRIPTS_NAME);
	replica.Program(
		"Interface",
		mine,
		true,
		std::string(NOTE) +
			"local button = game:GetService('Players').LocalPlayer:FindFirstChild('Button', true)\n"
			"button.Activated:Connect(function() note('activated ') end)\n"
			"button.MouseButton1Click:Connect(function() note('clicked ') end)\n"
	);

	replica.Adopt();

	// The tick that starts it, so the connection exists before the pointer does.
	replica.Tick();
	REQUIRE(replica.Log().empty());

	// **The whole chain and not a hand-built `GuiEvent`.** A test that
	// synthesised events would pass against a client that never routed one,
	// which is the bug this closes wearing a different hat: the press was picked
	// correctly and handed to a VM that was not the button's.
	engine::gui::CompileRequest request;
	request.Display.Width = 800.0f;
	request.Display.Height = 600.0f;

	engine::gui::Compiled list;
	engine::gui::Router router;

	const auto frame = [&](float x, float y, bool down) {
		request.Hovered = router.Hovered();
		request.Pressed = router.Pressed();

		engine::gui::Layout(replica.World, request.Display);
		list.Rebuild(replica.World, request);

		engine::gui::Pointer pointer;
		pointer.Position = Vector2{x, y};
		pointer.Down = down;
		pointer.Inside = true;

		replica.Scripts->DeliverGuiEvents(router.Update(replica.World, list.Commands(), pointer));
		replica.Tick();
	};

	frame(50.0f, 50.0f, false);
	frame(50.0f, 50.0f, true);
	frame(50.0f, 50.0f, false);

	INFO(replica.Scripts->LastError());
	CHECK(replica.Log().find("activated ") != std::string::npos);
	CHECK(replica.Log().find("clicked ") != std::string::npos);
}

TEST_CASE(
	"a client script's write to replicated state is refused, and it can tell",
	"[client][replication][scripting]"
) {
	Replica replica;
	replica.Arrive();

	const Entity part = replica.World.CreateInstance(engine::scene::PartClass(), std::string("Rock"));
	REQUIRE(part != NULL_ENTITY);
	REQUIRE(replica.World.SetParent(part, engine::scene::WorkspaceOf(replica.World)));

	const Entity mine = replica.ContainerOf(replica.Local, engine::scene::PLAYER_SCRIPTS_NAME);
	replica.Program(
		"Writer",
		mine,
		true,
		"local ok, err = pcall(function() workspace.Rock.Transparency = 0.5 end)\n"
		"workspace:SetAttribute('refused', tostring(not ok))\n"
		"workspace:SetAttribute('why', tostring(err))\n"
		"local made = pcall(function() Instance.new('Part', workspace) end)\n"
		"workspace:SetAttribute('minted', tostring(made))\n"
	);

	replica.Adopt();
	replica.Tick();

	// **Raised rather than returned false.** A script author cannot tell a write
	// that was rejected from one that was applied and replaced by the next
	// delta, so the refusal has to arrive as an error at the line that made it.
	CHECK(replica.Read("refused") == "true");
	CHECK(replica.Read("why").find("replica") != std::string::npos);

	// And the value did not move.
	const auto *visual = replica.World.Get<engine::scene::Visual>(part);
	REQUIRE(visual != nullptr);
	CHECK(visual->Transparency == 0.0f);

	// `Instance.new` answers a null entity in an adopt-only store, which the
	// binding turns into an error of its own - a client script cannot mint a row
	// the authority is also handing indices out for.
	CHECK(replica.Read("minted") == "false");
}

TEST_CASE("tearing a replica down leaves no runtime alive", "[client][replication][scripting]") {
	std::weak_ptr<Runtime> watched;

	{
		Store world("client.replica.teardown");
		Scheduler systems;

		std::shared_ptr<Runtime> runtime = client::BuildReplicatedWorld(world, systems, {});
		REQUIRE(runtime != nullptr);
		watched = runtime;

		// The caller's and the scheduler's, which is the arrangement
		// `game::StartWorldScripts` describes: the scheduler holds the last one
		// and drops it with the world.
		CHECK(watched.use_count() >= 2);
	}

	// **`world::World` declares its store before its scheduler**, so the
	// scheduler - and every runtime its lambdas hold - is destroyed first and the
	// VM never outlives the storage it was opened over. This orders its locals
	// the same way on purpose: a runtime that survived its store would be a
	// use-after-free on the next beat.
	CHECK(watched.expired());
}
