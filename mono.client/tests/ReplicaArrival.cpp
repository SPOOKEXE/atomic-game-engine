// A world authored on one machine, arriving on another complete enough to run
// and to draw.
//
// **Its own suite rather than more of `client.replica.scripts`, because that
// one hand-builds the replica.** There, the tree is written straight into the
// client's store and the case is about which scripts a replica may run; here
// nothing is written into the client's store at all - a `Listener` and a
// `Connector` over a loopback transport carry every row, and what is asserted is
// that the row *arrived*. A case that populated the replica itself would pass
// against the engine this closes the gap in, where a `LocalScript` crossed as a
// name and a class with no program on it and a `ScreenGui` with no rectangle.
//
// **The programs are in the world's `SourceCache` and on no filesystem**, which
// is what makes "the text crossed" the only explanation for the client running
// them. A test whose scripts were files beside the binary would pass with
// nothing on the wire, because both ends can open the same file.

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
#include <engine/gui/Typing.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Replicated.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.replica.arrival")
TEST_DEPENDS("client.replica.scripts")
TEST_DEPENDS("engine.replication.defaults")
TEST_DEPENDS("engine.gui.services")

using engine::core::Name;
using engine::core::UDim2;
using engine::core::Vector2;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::net::MakeLoopbackTransport;
using engine::net::Transport;
using engine::replication::Connector;
using engine::replication::Listener;
using engine::script::Runtime;

namespace {
	constexpr float FRAME_SECONDS = 1.0f / 60.0f;

	// The two halves of a connection, each with the world its side owns.
	//
	// The authority beats a real `script::Runtime` because that is what runs
	// `MirrorSourcePrograms` - a fixture calling the mirror itself would be
	// asserting against a pass no host invokes, which is the shape of test that
	// let `PumpGuiEvents` ship unreachable.
	struct Link {
		std::vector<std::unique_ptr<Transport>> Transports;

		Store World{"arrival.authority"};
		Scheduler WorldSystems;
		std::shared_ptr<Runtime> WorldScripts;

		Store Replica{"arrival.replica"};
		Scheduler ReplicaSystems;
		std::shared_ptr<Runtime> ReplicaScripts;

		std::unique_ptr<Listener> Server;
		std::unique_ptr<Connector> Client;

		double Now = 0.0;
		uint64_t Beat = 0;

		Entity Occupant = NULL_ENTITY;

		explicit Link(bool audit = true) {
			engine::parallel::Jobs::Start(1);

			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			engine::scene::RegisterSceneClasses();
			(void)engine::gui::RegisterGuiClasses();
			(void)engine::script::ScriptClass();

			REQUIRE(engine::scene::InstallServices(World) != NULL_ENTITY);
			REQUIRE(engine::gui::InstallGuiServices(World) != NULL_ENTITY);

			// Not local: this world is a server's, and `Players.LocalPlayer` is
			// nil where `IsClient()` is false.
			Occupant = engine::scene::AddPlayer(World, "Ada", false, 1);
			REQUIRE(Occupant != NULL_ENTITY);

			// **The authority's VM runs as a server**, so it beats - and mirrors
			// - without running the `LocalScript`s under test. A client running
			// them here would make every case below pass on the wrong machine.
			engine::script::RuntimeLimits limits;
			limits.Role = engine::script::HostRole::OfServer();
			WorldScripts = engine::script::MakeRuntime(World, engine::script::Language::Luau, limits);
			REQUIRE(WorldScripts != nullptr);

			Transports = MakeLoopbackTransport(2);
			REQUIRE(Transports.size() == 2);

			// **The anti-entropy audit is on, exactly as `mono.server` turns it
			// on.** It is what makes the typing case below mean something: the
			// audit hashes what a replica holds and repairs whatever disagrees,
			// so a `TextBox` somebody has typed into is precisely the row it
			// would report as divergent every sweep. A suite that left it off
			// would be asserting against the one configuration where the
			// question cannot arise.
			engine::replication::ListenerSettings streaming;
			streaming.Authority.Audit.Enabled = audit;

			Server = std::make_unique<Listener>(*Transports[0], streaming);
			for (const engine::replication::ReplicatedComponent &component :
				 engine::replication::DefaultReplicatedComponents()) {
				Server->Authority().Replicate(Name(component.Name), component.Detection);
				if (!component.Suppressor.empty()) {
					Server->Authority().SuppressWhenTagged(Name(component.Name), Name(component.Suppressor));
				}
			}

			ReplicaScripts = client::BuildReplicatedWorld(Replica, ReplicaSystems, {});
			REQUIRE(ReplicaScripts != nullptr);
			Replica.SetAdoptOnly(true);

			Client = std::make_unique<Connector>(*Transports[1], Transports[0]->Local(), Now);
		}

		~Link() {
			engine::parallel::Jobs::Stop();
		}

		Link(const Link &) = delete;
		Link &operator=(const Link &) = delete;

		// One tick of each side, in the order two programs run them.
		void Tick() {
			Now += FRAME_SECONDS;
			Beat++;

			// Cleared at the head, exactly as `world::World::Tick` does it: the
			// bits are the delta's source and `Publish` reads them at the end.
			World.ClearChanges();
			World.SetFrame(FRAME_SECONDS, 0.0f);
			WorldScripts->Heartbeat(FRAME_SECONDS);

			Server->Poll(Now);
			Server->Publish(World, Beat, Now);
			Server->Advance(Now);

			Client->Poll(Replica, Now);
			Replica.SetFrame(FRAME_SECONDS, 0.0f);
			ReplicaSystems.RunPhases(Replica, Phase::PreSimulation, Phase::Simulation);
			Replica.FlushSignals();
			Client->Advance(Now);
		}

		// Runs until the world has arrived, then names the viewer.
		//
		// **The identity is written here because it cannot be replicated.**
		// `scene::LocalPlayer` says which of many players this machine is
		// looking through, which is per-client and arrives as a message in a
		// real client - `game::JoinNotice`. Three lines of what `client::Client`
		// does with it.
		bool Join(int ticks = 400) {
			for (int step = 0; step < ticks && !Client->Joined(); step++) {
				Tick();
			}
			if (!Client->Joined()) {
				return false;
			}

			const Entity players =
				engine::scene::ServiceOf(Replica, engine::ecs::Classes::Find(Name("Players")));
			if (players == NULL_ENTITY) {
				return false;
			}

			const Entity mine = Replica.FindFirstChild(players, "Ada");
			if (mine == NULL_ENTITY) {
				return false;
			}

			Replica.SetResource(engine::scene::LocalPlayer{mine});
			return true;
		}

		void Settle(int ticks = 60) {
			for (int step = 0; step < ticks; step++) {
				Tick();
			}
		}

		// A script instance whose program exists only in this world's table.
		Entity Program(std::string_view name, Entity parent, const std::string &source) {
			engine::script::SourceCache cache;
			if (const auto *held = World.Resource<engine::script::SourceCache>()) {
				cache = *held;
			}
			cache.Set(Name(std::string(name) + ".luau"), source);
			World.SetResource(cache);

			const Entity instance =
				engine::script::MakeScript(World, std::string(name) + ".luau", name, true);
			REQUIRE(instance != NULL_ENTITY);
			REQUIRE(World.SetParent(instance, parent));
			return instance;
		}

		Entity ContainerOf(std::string_view name) {
			const Entity found = World.FindFirstChild(Occupant, name);
			REQUIRE(found != NULL_ENTITY);
			return found;
		}

		// What the client's scripts wrote down, which is the only thing a
		// replica can say without writing a property.
		std::string Log() {
			engine::ecs::AttributeValue value;
			if (!engine::ecs::GetAttribute(
					Replica, engine::scene::WorkspaceOf(Replica), Name("log"), value
				)) {
				return {};
			}
			return value.String;
		}
	};

	// The client's own copy of an instance, found by name under a parent.
	Entity Mirrored(Store &replica, Entity parent, std::string_view name) {
		return replica.FindFirstChild(parent, name);
	}

	// What the default table says about one name, or nothing when it says
	// nothing.
	const engine::replication::ReplicatedComponent *Row(std::string_view name) {
		for (const auto &component : engine::replication::DefaultReplicatedComponents()) {
			if (component.Name == name) {
				return &component;
			}
		}
		return nullptr;
	}

	// Every name this process has registered, so a walk can ask about all of
	// them.
	//
	// **Called before the table is read, because the table is a function-local
	// static built on first use.** In a host it is built after start-up has
	// registered everything; here it is built by whichever case runs first, and
	// a case that asked before registering would assert over an empty span.
	void RegisterEverything() {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		(void)engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();
	}
}

TEST_CASE("every interface and script component is classified", "[client][replication][defaults]") {
	RegisterEverything();

	// **`AGENTS.md` rule 6 for the two sets that started crossing at v0.15**, and
	// the same check `engine.replication.defaults` makes for `ecs.` - here rather
	// than there because `replication` links neither `gui` nor `script` and must
	// not, so that suite can only ask about spellings it registers itself.
	//
	// A component added to either module is a red suite until somebody decides
	// whether a client should be shown it. That decision is the one this whole
	// change is about: a `gui.Resolved` that crossed would be the server's window
	// deciding a client's layout, and a `script.SourceCache` that crossed would
	// be every client holding the server's programs.
	for (size_t index = 0; index < engine::ecs::Components::Count(); index++) {
		const engine::ecs::TypeDescriptor &type =
			engine::ecs::Components::Describe(engine::ecs::ComponentId{static_cast<uint32_t>(index)});

		const std::string_view name = type.Name.Text();
		if (!name.starts_with("gui.") && !name.starts_with("script.")) {
			continue;
		}

		INFO("component: " << name);

		// Four interface rows are what the machine looking at the world works
		// out for itself, and the world's script table is a resource that could
		// only ever cross whole. Everything else is authored and crosses.
		//
		// `gui.ScrollState` joined the four at v0.18 and is `gui.Resolved`'s case
		// for one class: the pixel canvas, the visible window and the thumb
		// rectangles are all derived from an `AbsoluteSize` that belongs to the
		// display doing the looking.
		//
		// **`gui.PageMotion` and `gui.ScrollMotion` joined at v0.17 on a
		// stronger ground than any of those.** The others are local because
		// they are re-derived from what was sent; these two hold a reading of
		// *this* process's monotonic clock, which is not a quantity that means
		// anything anywhere else. A client handed the authority's `StartedAt`
		// would evaluate a page tween against that machine's uptime. What
		// crosses is the destination - `CurrentPage` and `CanvasPosition` - and
		// each end animates to it on its own clock, which is also what makes a
		// client that dropped a packet arrive rather than stutter.
		//
		// **`gui.Canvas` joined at v0.19 and it should have joined with
		// `gui.Resolved`.** `gui::Layout` writes the two on the same collector
		// in the same block - the canvas rectangle, and then
		// `Resolved::AbsolutePosition` and `::AbsoluteSize` taken straight off
		// it - and `Components.hpp` says of `Canvas` in its own words that it
		// "holds the resolved rectangle rather than any authored field". Only
		// the exclusion was missing, so for four versions the authority's screen
		// rectangle crossed to every client and was overwritten by that client's
		// next layout pass. This case is what would have caught it and did not,
		// because the list it compares against was written from the same
		// oversight.
		const bool excluded = name == "gui.Canvas" || name == "gui.Resolved" || name == "gui.SpatialCanvas" ||
							  name == "gui.GuiServiceState" || name == "gui.ScrollState" ||
							  name == "gui.PageMotion" || name == "gui.ScrollMotion" ||
							  name == "gui.SettingsMenuExtensions" || name == "script.SourceCache";

		CHECK((excluded == (Row(name) == nullptr)));
	}
}

TEST_CASE(
	"the three a hash cannot cover are observed and the rest are signed", "[client][replication][defaults]"
) {
	RegisterEverything();

	// A `std::string`'s object representation is a pointer, so a signature over
	// one answers about the allocation rather than about the text. These are the
	// rows that hold one, and `Authority::Survey` is what turns the observation
	// on for them - a detector declared without it sends nothing and says
	// nothing.
	REQUIRE(Row("gui.Label") != nullptr);
	REQUIRE(Row("gui.Entry") != nullptr);
	REQUIRE(Row("script.Program") != nullptr);

	CHECK(Row("gui.Label")->Detection == engine::replication::ChangeDetection::Observed);
	CHECK(Row("gui.Entry")->Detection == engine::replication::ChangeDetection::Observed);
	CHECK(Row("script.Program")->Detection == engine::replication::ChangeDetection::Observed);

	// And the ordinary interface row is signed, because a `UDim2` is bytes and a
	// script writes it once.
	REQUIRE(Row("gui.Element") != nullptr);
	CHECK(Row("gui.Element")->Detection == engine::replication::ChangeDetection::Signature);

	// **The suppressor, which is what keeps a person's typing.** A `TextBox` is
	// the only class carrying a `gui.Entry`, so it is the tag that says "the two
	// ends are meant to disagree about this row" - and an entity carrying it is
	// out of the audit entirely, which is the other half of the same answer.
	CHECK(Row("gui.Label")->Suppressor == "gui.Entry");
	CHECK(Row("gui.Element")->Suppressor.empty());
}

TEST_CASE("a LocalScript authored on the authority runs on the client", "[client][replication][scripting]") {
	Link link;

	// **The program is in the authority's `SourceCache` and nowhere else.**
	// `MakeScript` names a path; no file of that name exists beside this binary,
	// so a client that could not read the text would run nothing and the log
	// would be empty. That is the whole assertion.
	link.Program(
		"Authored",
		link.ContainerOf(engine::scene::PLAYER_SCRIPTS_NAME),
		"workspace:SetAttribute('log', 'ran')\n"
	);

	REQUIRE(link.Join());
	link.Settle();

	INFO(link.ReplicaScripts->LastError());
	CHECK(link.Log() == "ran");
}

TEST_CASE("the program crosses and not a path the client could open", "[client][replication][scripting]") {
	Link link;

	const Entity authored =
		link.Program("Carried", link.ContainerOf(engine::scene::PLAYER_SCRIPTS_NAME), "return 1\n");

	REQUIRE(link.Join());
	link.Settle();

	// The row itself, on the client's copy of the instance - reached by the
	// *same handle*, because an `ecs::Entity` is the one thing
	// `replication/AGENTS.md` says crosses as a number rather than as a name.
	const auto *program = link.Replica.Get<engine::script::Program>(authored);
	REQUIRE(program != nullptr);
	CHECK(program->Path == Name("Carried.luau"));
	CHECK(program->Text == "return 1\n");

	// And the container it names, so the client knows *which* program the
	// instance runs rather than only what one of them says.
	const auto *container = link.Replica.Get<engine::script::LuaSourceContainer>(authored);
	REQUIRE(container != nullptr);
	CHECK(container->Path == Name("Carried.luau"));
}

TEST_CASE("a Script's program never becomes a row at all", "[client][replication][scripting]") {
	Link link;

	// A server script, in a container a client can see. The class is the whole
	// of the rule: `MirrorSourcePrograms` writes a row for a `LocalScript` and a
	// `ModuleScript` and for nothing else, so a server's program cannot cross
	// however interest is configured.
	{
		engine::script::SourceCache cache;
		cache.Set(Name("Secret.luau"), "-- the server's business\n");
		link.World.SetResource(cache);
	}

	const Entity secret = engine::script::MakeScript(link.World, "Secret.luau", "Secret", false);
	REQUIRE(secret != NULL_ENTITY);
	REQUIRE(link.World.SetParent(secret, engine::scene::WorkspaceOf(link.World)));

	REQUIRE(link.Join());
	link.Settle();

	CHECK(link.World.Get<engine::script::Program>(secret) == nullptr);
	CHECK(link.Replica.Get<engine::script::Program>(secret) == nullptr);
}

TEST_CASE("a disabled script arrives disabled", "[client][replication][scripting]") {
	Link link;

	const Entity off = link.Program(
		"Stopped",
		link.ContainerOf(engine::scene::PLAYER_SCRIPTS_NAME),
		"workspace:SetAttribute('log', 'ran')\n"
	);
	link.World.Set(off, engine::script::Disabled{});

	REQUIRE(link.Join());
	link.Settle();

	// **A tag is a component with no bytes, and that is not the same as a
	// component with no serialisation.** It was read as one until v0.15, so this
	// row was declared and never sent - and a client ran a script its author had
	// switched off.
	CHECK(link.Replica.Has<engine::script::Disabled>(off));
	CHECK(link.Log().empty());
}

TEST_CASE("a ScreenGui authored on the authority arrives drawable", "[client][replication][gui]") {
	Link link;

	const Entity playerGui = link.ContainerOf(engine::gui::PLAYER_GUI);

	const Entity screen = link.World.CreateInstance(engine::gui::GuiClass("ScreenGui"), std::string("Hud"));
	REQUIRE(screen != NULL_ENTITY);
	REQUIRE(link.World.SetParent(screen, playerGui));

	const Entity label = link.World.CreateInstance(engine::gui::GuiClass("TextLabel"), std::string("Score"));
	REQUIRE(label != NULL_ENTITY);
	REQUIRE(link.World.SetParent(label, screen));

	engine::gui::Element element;
	element.Position = UDim2{0.0f, 10.0f, 0.0f, 10.0f};
	element.Size = UDim2{0.0f, 120.0f, 0.0f, 40.0f};
	link.World.Set(label, element);

	engine::gui::Label words;
	words.Text = "1000 points";
	link.World.Set(label, words);

	REQUIRE(link.Join());
	link.Settle();

	const Entity arrived = Mirrored(link.Replica, Mirrored(link.Replica, playerGui, "Hud"), "Score");
	REQUIRE(arrived != NULL_ENTITY);

	const auto *arrivedElement = link.Replica.Get<engine::gui::Element>(arrived);
	REQUIRE(arrivedElement != nullptr);
	CHECK(arrivedElement->Size.X.Offset == 120.0f);

	const auto *arrivedLabel = link.Replica.Get<engine::gui::Label>(arrived);
	REQUIRE(arrivedLabel != nullptr);
	CHECK(arrivedLabel->Text == "1000 points");

	// **Drawable, not merely present.** The rectangle a client draws is
	// `gui::Resolved`, which is deliberately *not* replicated - the layout that
	// produces it runs against this machine's display - so the row arriving is
	// only half the claim. The other half is that the client's own layout pass
	// turns it into a draw command.
	engine::gui::CompileRequest request;
	request.Display.Width = 800.0f;
	request.Display.Height = 600.0f;

	engine::gui::Layout(link.Replica, request.Display);

	engine::gui::Compiled list;
	list.Rebuild(link.Replica, request);

	bool drawn = false;
	for (const engine::gui::DrawCommand &command : list.Commands().Commands) {
		drawn = drawn || command.Text == "1000 points";
	}
	CHECK(drawn);
}

TEST_CASE("the client's own keyboard focus does not cross", "[client][replication][gui]") {
	Link link;

	const Entity playerGui = link.ContainerOf(engine::gui::PLAYER_GUI);

	const Entity screen = link.World.CreateInstance(engine::gui::GuiClass("ScreenGui"), std::string("Hud"));
	REQUIRE(link.World.SetParent(screen, playerGui));

	const Entity box = link.World.CreateInstance(engine::gui::GuiClass("TextBox"), std::string("Entry"));
	REQUIRE(link.World.SetParent(box, screen));

	engine::gui::Element element;
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 40.0f};
	link.World.Set(box, element);

	REQUIRE(link.Join());
	link.Settle();

	const Entity arrived = Mirrored(link.Replica, Mirrored(link.Replica, playerGui, "Hud"), "Entry");
	REQUIRE(arrived != NULL_ENTITY);

	// The authority focuses its own copy. `gui.GuiServiceState` is one row per
	// world, so replicating it would hand every client whichever box the server
	// - or another player - happened to be typing into.
	REQUIRE(engine::gui::Focus(link.World, box));
	link.Settle();

	CHECK(engine::gui::FocusedTextBox(link.World) == box);
	CHECK(engine::gui::FocusedTextBox(link.Replica) == NULL_ENTITY);
}

TEST_CASE("typing into a replicated TextBox survives what arrives next", "[client][replication][gui]") {
	Link link;

	const Entity playerGui = link.ContainerOf(engine::gui::PLAYER_GUI);

	const Entity root = link.World.CreateInstance(engine::gui::GuiClass("ScreenGui"), std::string("Hud"));
	REQUIRE(link.World.SetParent(root, playerGui));

	const Entity box = link.World.CreateInstance(engine::gui::GuiClass("TextBox"), std::string("Entry"));
	REQUIRE(link.World.SetParent(box, root));

	engine::gui::Element element;
	element.Size = UDim2{0.0f, 200.0f, 0.0f, 40.0f};
	link.World.Set(box, element);

	engine::gui::Label authored;
	authored.Text = "";
	link.World.Set(box, authored);

	REQUIRE(link.Join());
	link.Settle();

	const Entity arrived = Mirrored(link.Replica, Mirrored(link.Replica, playerGui, "Hud"), "Entry");
	REQUIRE(arrived != NULL_ENTITY);

	// Somebody types. `gui::Type` writes `Label::Text` in the replica - there is
	// no edit buffer anywhere else, which is `gui/AGENTS.md`'s rule - and it is
	// a component write rather than a property write, so the adopt-only store
	// permits it.
	REQUIRE(engine::gui::Focus(link.Replica, arrived));

	engine::gui::Typing typing;
	typing.Text = "hello";

	const engine::gui::TypeResult typed = engine::gui::Type(link.Replica, typing);
	CHECK(typed.Instance == arrived);
	CHECK(typed.Changed);

	// Long enough for several audit sweeps at the default cadence. Without the
	// `gui.Entry` suppressor this is where the typing goes: the authority holds
	// an empty string for the same row, the audit reports the box as divergent,
	// and the repair puts the authority's answer back on top of the person's.
	link.Settle(200);

	{
		const auto *held = link.Replica.Get<engine::gui::Label>(arrived);
		REQUIRE(held != nullptr);
		CHECK(held->Text == "hello");
	}

	// And the authority is not told, because a replica may not write to a bus.
	{
		const auto *authoritys = link.World.Get<engine::gui::Label>(box);
		REQUIRE(authoritys != nullptr);
		CHECK(authoritys->Text.empty());
	}

	// **Now the other direction, which is the sharper half.** A server script
	// writing `TextBox.Text` is an ordinary property write and marks the row
	// changed, so without the suppressor the next delta lands on top of what
	// somebody is in the middle of typing. With it, the authority's own copy
	// moves and the client's does not - and that is the trade being made rather
	// than an accident: a box a person is typing into belongs to the person.
	engine::gui::Label authoritys;
	authoritys.Text = "from the server";
	link.World.Set(box, authoritys);

	link.Settle(120);

	CHECK(link.World.Get<engine::gui::Label>(box)->Text == "from the server");

	const auto *stillTyped = link.Replica.Get<engine::gui::Label>(arrived);
	REQUIRE(stillTyped != nullptr);
	CHECK(stillTyped->Text == "hello");
}

TEST_CASE("an unchanged script costs nothing per tick", "[client][replication][scripting]") {
	// **The audit is off for this one, and that is the measurement being
	// isolated rather than a thumb on the scale.** Anti-entropy sends a digest
	// per sweep whatever the world holds, so leaving it on would measure its
	// cadence; what this case is about is the delta path, which is where a
	// signature over a program would have shown up and where `Observed` costs a
	// dirty bit that nothing has set.
	Link link(false);

	// Ten programs of about a kilobyte each: the case the per-tick cost
	// question is about. A signature over these would be ten kilobytes of
	// hashing sixty times a second to learn that nobody has typed anything.
	const std::string body(1024, 'x');
	for (int index = 0; index < 10; index++) {
		link.Program(
			"Bulk" + std::to_string(index),
			link.ContainerOf(engine::scene::PLAYER_SCRIPTS_NAME),
			"--[[" + body + "]]\n"
		);
	}

	REQUIRE(link.Join());
	link.Settle(120);

	// **Zero, and it has to be zero rather than small.** `Observed` means the
	// dirty bits decide, and nothing has written a program since the mirror
	// filled the rows - so a quiet world with ten kilobytes of Luau in it sends
	// nothing at all. Measured over ticks after everything has been confirmed,
	// which is where `Signature` would still be hashing.
	size_t bytes = 0;
	for (int step = 0; step < 60; step++) {
		link.Tick();
		bytes += link.Server->Authority().Stats().Bytes;
	}

	CHECK(bytes == 0);
}
