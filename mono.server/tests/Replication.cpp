// One process being the authority for another.
//
// `replication/tests/EndToEnd.cpp` stands both halves up inside one process
// over a loopback transport. That is the right shape for a suite and it is not
// a running game: the two stores share an allocator, a component registry and a
// clock, and none of the things that only cross a process boundary are crossed.
// **This is the case the roadmap said did not exist yet** - a real server
// binary, started with `--listen`, streaming a world over a real UDP socket to
// something that has only ever seen its bytes.
//
// What that catches which the in-process suite cannot: a component name that
// resolves in one build and not the other, a snapshot that depends on
// registration order, a chunk that fits a loopback queue and not a datagram, and
// a flag that was parsed into a field nothing read.
//
// Skipped rather than failed when the server binary is not beside the test
// binary, for the reason `HostMode.cpp` gives: a preset that builds tests
// without the program is a legal configuration, and a suite that failed on it
// would be reporting the build rather than the code.

#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Shooting.hpp>
#include <engine/game/Play.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tools.hpp>
#include <engine/script/Instances.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

TEST_SUITE_ID("server.replication")
TEST_DEPENDS("engine.scene.registration")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::net::Endpoint;
using engine::net::MakeUdpTransport;
using engine::net::Transport;
using engine::parallel::Process;
using engine::replication::Connector;
using engine::scene::Bounds;
using engine::scene::Character;
using engine::scene::Humanoid;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::scene::Visual;

namespace server_replication_test {
	// The components the server sends - `scene`'s, and the same call the server
	// itself makes.
	//
	// **This file used to declare its own `Position` and `Velocity` under the
	// server's wire names**, because a component crosses by name and there was
	// no shared set to name. That was a third copy beside the server's and the
	// client's, and it was worse than untidy: this binary already contained
	// `server::Position`, so registering a second type under the same string
	// aborted whenever both suites ran in one process. `testrunner` runs suites
	// separately and never saw it; `ctest` runs the binary whole and failed
	// every time. Registering the shared set is what removes the second
	// declaration rather than renaming around the collision.
	void RegisterTypes() {
		engine::scene::RegisterSceneComponents();

		// **And the two sets that started crossing at v0.15**, because this
		// replica is a client and a client is what receives them. A snapshot
		// naming a component the receiving build never registered is refused
		// whole rather than half-merged - the join simply never completes and
		// the log says which name - so a harness registering less than
		// `client::BuildReplicatedWorld` does is a harness testing a client that
		// does not exist. `gui.Element` on a `ScreenGui` and `script.Program` on
		// a `LocalScript` are what made that reachable.
		(void)engine::gui::RegisterGuiClasses();
		(void)engine::script::ScriptClass();
	}

	std::filesystem::path ServerProgram() {
		return engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	}

	bool ServerAvailable() {
		return std::filesystem::exists(ServerProgram());
	}

	// A port nothing is using, found by binding one and letting it go.
	//
	// There is a window between the release and the server's bind, and it is
	// accepted rather than closed: the alternative is the server reporting its
	// ephemeral port back over a pipe, which is a protocol invented for a test.
	// The caller retries, which turns a lost race into a slower test rather than
	// a failing one.
	uint16_t FreePort() {
		std::unique_ptr<Transport> probe = MakeUdpTransport(0);
		return probe == nullptr ? 0 : probe->Local().Port;
	}

	// A server process listening on a port, and a socket pointed at it.
	struct Remote {
		Process Child;
		std::unique_ptr<Transport> Socket;
		std::unique_ptr<Connector> Link;
		Store World{"replica"};
		double Now = 0.0;

		// The port the child bound, so a second client can be pointed at the
		// same server rather than starting one of its own.
		uint16_t Port = 0;

		// The `Player` the server said is this client's, or null until it does.
		//
		// **Recorded here because it is the one fact a replica cannot derive.**
		// Every other row in `World` arrived as replicated state; this arrives
		// as a per-client message, which is the whole point of `game/Play.hpp`.
		Entity Mine;

		// @param entities How many the placeholder world holds.
		// @param game     A scene to host instead, or empty for the placeholder.
		// @param extra    Further arguments for the child, for a case that is
		//                 about a setting rather than about a world.
		bool
		Start(uint32_t entities, const std::string &game = {}, const std::vector<std::string> &extra = {}) {
			RegisterTypes();

			for (int attempt = 0; attempt < 4 && !Child.Started(); attempt++) {
				const uint16_t port = FreePort();
				if (port == 0) {
					return false;
				}

				std::vector<std::string> arguments{
					"--listen",
					std::to_string(port),
					"--entities",
					std::to_string(entities),
					"--tick-rate",
					"60",
					"--seconds",
					"30",
				};
				if (!game.empty()) {
					arguments.emplace_back("--game");
					arguments.push_back(game);
				}

				arguments.insert(arguments.end(), extra.begin(), extra.end());

				if (!Child.Start(ServerProgram(), arguments)) {
					continue;
				}

				// The child may still exit immediately - the port went to
				// somebody else between the probe and the bind - so give it a
				// moment and check before committing to it.
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				if (!Child.Poll().Alive()) {
					Child = Process{};
					continue;
				}

				Socket = MakeUdpTransport(0);
				if (Socket == nullptr) {
					return false;
				}

				Port = port;
				Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now);
				ListenForJoinNotice();
				return true;
			}

			return false;
		}

		// A second client on somebody else's server. No child process: this one
		// is a socket and a connector, which is what a second player is.
		bool Connect(uint16_t port) {
			RegisterTypes();
			Socket = MakeUdpTransport(0);
			if (Socket == nullptr) {
				return false;
			}
			Port = port;
			Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now);
			ListenForJoinNotice();
			return true;
		}

		// What `client::Client` does with the same message, in three lines.
		//
		// **The resource is written here and not by the caller**, because
		// `Client::SubmitMove` refuses to send anything until the replica knows
		// which player is its own - so a harness that recorded `Mine` and left
		// the store ignorant of it could not exercise the keyboard path at all.
		void ListenForJoinNotice() {
			Link->OnUserMessage([this](std::span<const std::byte> message) {
				engine::game::JoinNotice notice;
				if (engine::game::DecodeJoinNotice(message, notice)) {
					Mine = notice.Player;
					World.SetResource(engine::scene::LocalPlayer{notice.Player});
				}
			});
		}

		// Sends a move, exactly as `Client::SubmitMove` does.
		void Walk(const engine::core::Vector3 &direction, bool jump = false) {
			engine::game::MoveInput move;
			move.Direction = direction;
			move.Jump = jump;
			Link->Submit(Link->Applied(), engine::game::EncodeMoveInput(move), Now);
		}

		// Holds a set of keys down on this client, and releases everything else.
		//
		// **`Previous` is kept, because a jump is an edge.** `ReadMoveIntent`
		// asks for the space bar's edge, which is the difference between the two
		// masks - so a harness that cleared both would make every frame look
		// like the first and every held key look like a fresh press.
		//
		// **And `LatchPresses` is called, because that is what a writer does.**
		// The edge a tick acts on lives in `InputState::Pressed`, and a frame
		// that only set bits in `Down` is a frame no writer ever recorded. This
		// is `Client::PumpInput`'s last line, and leaving it out here would make
		// the harness the one place in the engine where jump worked differently.
		void Press(std::initializer_list<engine::scene::KeyCode> keys) {
			auto *input = World.ResourceMutable<engine::scene::InputState>();
			if (input == nullptr) {
				World.SetResource(engine::scene::InputState{});
				input = World.ResourceMutable<engine::scene::InputState>();
			}

			input->Previous = input->Down;
			input->Down = {};
			input->Focused = true;
			for (const engine::scene::KeyCode key : keys) {
				input->Down.Set(key, true);
			}
			input->LatchPresses();
		}

		// Puts a camera in the replica at `from`, looking along `direction`.
		//
		// **A predicted entity, exactly as `client::AimReplicaViewer` mints
		// one.** A replica may not allocate an authoritative index - the
		// authority allocates from the same range and `Apply` would be right to
		// merge the two - and a camera nobody else can see is precisely what the
		// reserved high range is for. Building it by hand here rather than
		// calling that function keeps this suite out of `mono.client`.
		void AimFrom(const engine::core::Vector3 &from, const engine::core::Vector3 &direction) {
			auto *active = World.ResourceMutable<engine::scene::ActiveCamera>();
			if (active == nullptr || !World.Alive(active->Entity)) {
				engine::scene::ActiveCamera live;
				live.Entity = World.CreatePredicted("ReplicaViewer");
				World.SetResource(live);
				active = World.ResourceMutable<engine::scene::ActiveCamera>();
			}

			World.Set(
				active->Entity, engine::scene::Transform{engine::core::CFrame::LookAt(from, from + direction)}
			);
		}

		// Presses and releases the left mouse button, latching the edge.
		//
		// **The edge and not the hold**, which is the whole reason
		// `InputState::PressedButtons` exists: frames outnumber ticks, so a
		// click that began and ended between two of them lands on a frame no
		// tick ever looks at. `LatchPresses` is what a writer owes, exactly as
		// it does for a key.
		void Click() {
			auto *input = World.ResourceMutable<engine::scene::InputState>();
			if (input == nullptr) {
				World.SetResource(engine::scene::InputState{});
				input = World.ResourceMutable<engine::scene::InputState>();
			}

			input->Focused = true;
			input->PreviousButtons = input->Buttons;
			input->Buttons =
				static_cast<uint8_t>(1u << static_cast<uint8_t>(engine::scene::MouseButton::Left));
			input->LatchPresses();
			input->PreviousButtons = input->Buttons;
			input->Buttons = 0;
		}

		// `Client::SubmitMove`'s shot half, and deliberately not a paraphrase.
		//
		// The same guard, the same `ReadAimIntent`, the same codec, the same
		// channel - because the point of the assertion is that a *client* can
		// fire, and a hand-built `examples::Shot` would step straight over the
		// arithmetic that turns a camera into a ray.
		bool SubmitAim() {
			if (Mine == engine::ecs::NULL_ENTITY ||
				engine::scene::CharacterOf(World, Mine) == engine::ecs::NULL_ENTITY) {
				return false;
			}

			const engine::scene::AimIntent aim = engine::scene::ReadAimIntent(World);
			if (!aim.Aimed || !aim.Fired) {
				return false;
			}

			engine::examples::Shot shot;
			shot.Aim = aim.Ray;
			shot.Range = engine::examples::MAXIMUM_SHOT_RANGE;
			if (!Link->Submit(Link->Applied(), engine::examples::EncodeShot(shot), Now)) {
				return false;
			}
			if (auto *input = World.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
				input->ConsumeButtonTaps();
			}
			return true;
		}

		// Where the camera is pointing, which is what W is measured against.
		void LookAlong(float yawRadians) {
			auto *camera = World.ResourceMutable<engine::scene::CameraController>();
			if (camera == nullptr) {
				World.SetResource(engine::scene::CameraController{});
				camera = World.ResourceMutable<engine::scene::CameraController>();
			}
			camera->Angles.Y = yawRadians;
		}

		// **`Client::SubmitMove`'s body, and deliberately not a paraphrase of
		// it.** `Walk` above takes a direction already decided, which leaves the
		// interesting arithmetic - which world direction a key means, and what
		// the camera has to do with it - on the far side of the assertion. This
		// is the client half of "press W and the character walks": the same
		// guard, the same `ReadMoveIntent`, the same codec, the same channel.
		void SubmitMove() {
			if (Mine == engine::ecs::NULL_ENTITY ||
				engine::scene::CharacterOf(World, Mine) == engine::ecs::NULL_ENTITY) {
				return;
			}

			const engine::scene::MoveIntent intent = engine::scene::ReadMoveIntent(World);

			engine::game::MoveInput move;
			move.Direction = intent.Direction;
			move.Jump = intent.Jump;
			if (Link->Submit(Link->Applied(), engine::game::EncodeMoveInput(move), Now)) {
				// Once it is on the wire, and not before - the same rule
				// `Client::SubmitMove` follows, for the same reason: a tap
				// forgotten after a failed send is a jump the server never
				// heard about.
				if (auto *input = World.ResourceMutable<engine::scene::InputState>(); input != nullptr) {
					input->ConsumeTaps();
				}
			}
		}

		// Where this client believes its own character's body is.
		engine::core::Vector3 MyPosition() {
			const Character *rig = World.Get<Character>(engine::scene::CharacterOf(World, Mine));
			if (rig == nullptr) {
				return {};
			}
			const Transform *placement = World.Get<Transform>(rig->Root);
			return placement == nullptr ? engine::core::Vector3{} : placement->Frame.Position;
		}

		// Holds `keys` for `ticks` client frames and reports where the body ended
		// up. The keyboard is re-pressed every frame because that is what a held
		// key *is* to `Client::WriteInput` - it rewrites the whole state each
		// frame from the window.
		engine::core::Vector3
		Hold(std::initializer_list<engine::scene::KeyCode> keys, int ticks, Remote *other = nullptr) {
			for (int tick = 0; tick < ticks; tick++) {
				Press(keys);
				SubmitMove();
				Tick();
				if (other != nullptr) {
					other->Tick();
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
			return MyPosition();
		}

		// One client tick: say something so the server knows where to send, then
		// take whatever came back.
		void Tick() {
			Now += 1.0 / 60.0;

			// **Deliberately nothing but `Poll`.** A server on a datagram socket
			// cannot stream to an address it has never heard from, so the client
			// has to speak first - and making the *test* speak first, with an
			// input the real client never sends, is how this suite passed while
			// the actual client sat silent and never joined. `Poll` announces
			// itself, and this is the case that says so.
			Link->Poll(World, Now);
			Link->Advance(Now);
		}

		// Ticks until `ready`, giving up after what `ticks` is worth in real
		// time on a machine that is not busy.
		//
		// **The bound is a deadline and not a count of iterations, and that is
		// not a detail.** The far side is a process competing for the same
		// cores, so N iterations buys a different amount of *its* progress on a
		// loaded machine than on an idle one - the join `D00122`'s case waits
		// for finished inside 1200 iterations on a quiet box and needed about
		// 1400 under a saturating build, so the case passed or failed on what
		// else happened to be running. A count cannot express "long enough for
		// the server to have got there"; a clock can.
		//
		// The multiplier is the room a busy machine needs, and it costs nothing
		// on a quiet one because the loop stops the moment `ready` is true.
		//
		// @param ready What is being waited for.
		// @param ticks The wait's length in ticks, as an idle machine would run
		//        them.
		// @return Whatever `ready` says at the end.
		bool Wait(const std::function<bool()> &ready, int ticks) {
			constexpr int STEP_MILLISECONDS = 4;
			constexpr int SLACK = 4;

			const auto deadline = std::chrono::steady_clock::now() +
								  std::chrono::milliseconds(ticks * STEP_MILLISECONDS * SLACK);

			while (!ready() && std::chrono::steady_clock::now() < deadline) {
				Tick();

				// Real time, because the far side is a real process running at
				// its own rate rather than a function this loop calls.
				std::this_thread::sleep_for(std::chrono::milliseconds(STEP_MILLISECONDS));
			}

			return ready();
		}

		bool Join(int ticks) {
			return Wait([this] { return Link->Joined(); }, ticks);
		}

		size_t Entities() {
			size_t count = 0;
			World.EachEntity([&count](Entity) { count++; });
			return count;
		}

		~Remote() {
			Link.reset();
			if (Socket != nullptr) {
				Socket->Close();
			}
			if (Child.Started()) {
				Child.Kill();
			}
		}
	};
}

using namespace server_replication_test;

namespace server_replication_test {
	// Client ticks until whatever the server just did has reached this replica.
	// Real sleeps, because the far side is a real process running at its own
	// rate rather than a function this loop calls.
	void Settle(Remote &remote) {
		for (int tick = 0; tick < 200; tick++) {
			remote.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}
}

TEST_CASE("a client joins a server that is a separate process", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(64));

	// Sixteen hundred milliseconds of client ticks. Generous rather than tight:
	// the snapshot is spread across ticks on purpose, and a tight budget here
	// would make this suite fail on a loaded machine rather than on a bug.
	REQUIRE(remote.Join(400));

	// The world arrived, and it is the one that was asked for.
	REQUIRE(remote.Entities() == 64);
	REQUIRE(remote.Link->Applied() > 0);
}

TEST_CASE("the replicated world holds the components the server sent", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(32));
	REQUIRE(remote.Join(400));

	// Every replicated component resolved by name in a process that never saw
	// the server's header. A name that failed to resolve is a component that
	// silently does not arrive, which is why this is asserted rather than
	// assumed from the entity count.
	REQUIRE(remote.World.CountMatching<Transform>() == 32);
	REQUIRE(remote.World.CountMatching<Motion>() == 32);

	// The two that are sent once in the snapshot and never again, because
	// nothing in that world resizes or recolours anything. A client that
	// received a position and no size has a scene it cannot draw, and this is
	// the assertion that says they arrived rather than being assumed from the
	// two above.
	REQUIRE(remote.World.CountMatching<Bounds>() == 32);
	REQUIRE(remote.World.CountMatching<Visual>() == 32);
}

TEST_CASE("the world keeps moving after the join", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	Remote remote;
	REQUIRE(remote.Start(32));
	REQUIRE(remote.Join(400));

	const uint64_t joinedAt = remote.Link->Applied();

	// A snapshot arriving and nothing after it is a join that worked and a
	// stream that did not, and the two look identical from the entity count
	// alone. What separates them is the applied tick advancing.
	for (int tick = 0; tick < 120 && remote.Link->Applied() == joinedAt; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	// **Read through `LinkStats` rather than through `Link()`.** A QUIC
	// connection owns no `net::Link`, and QUIC is what a server serves by
	// default - so a diagnostic that reached for the concrete type would be a
	// null dereference on the default transport, which is what it was.
	const engine::net::ConnectionStats link = remote.Link->LinkStats();
	INFO(
		"applied=" << remote.Link->Applied() << " joinedAt=" << joinedAt << " refused="
				   << remote.Link->Stats().Refused << " appliedMsgs=" << remote.Link->Stats().Applied
				   << " wire=" << engine::net::Describe(remote.Link->Wire())
				   << " attempts=" << remote.Link->Attempts() << " lost=" << link.PacketsLost
				   << " recv=" << link.PacketsReceived
	);
	REQUIRE(remote.Link->Applied() > joinedAt);
}

TEST_CASE("a replication rate thins the stream without slowing the world", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **The one thing only a real server proves: that the rate reaches the
	// wire.** `engine.world.universe` proves the clock and `server.host` proves
	// the option reaches the world's settings; what neither can see is
	// `Server::ServeClients` skipping the publish and pumping the link instead.
	//
	// **Measured as the gap between applied ticks, never as snapshots per
	// wall-second.** A count over a wall-clock window is a test that fails on a
	// busy machine. The gap is the ratio between the two rates and does not
	// depend on how long anything took: at 60 Hz simulating and 5 Hz
	// publishing, each snapshot carries about twelve ticks of movement where an
	// ungated stream carries one.
	Remote remote;
	REQUIRE(remote.Start(32, {}, {"--replication-tick-rate", "5"}));

	// The join is the first half of the claim. Every message the server queues
	// waits for a published tick, so a client joining through a throttled
	// stream is exactly the case the `Flush`-instead-of-`Publish` split could
	// have broken - it would hang here rather than fail an assertion below.
	REQUIRE(remote.Join(600));

	uint64_t previous = remote.Link->Applied();
	uint64_t advanced = 0;
	int snapshots = 0;

	for (int tick = 0; tick < 900 && snapshots < 6; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));

		const uint64_t applied = remote.Link->Applied();
		if (applied > previous) {
			advanced += applied - previous;
			previous = applied;
			snapshots++;
		}
	}

	REQUIRE(snapshots >= 3);

	const double ticksPerSnapshot = static_cast<double>(advanced) / snapshots;
	INFO("advanced=" << advanced << " snapshots=" << snapshots << " ratio=" << ticksPerSnapshot);

	// **A floor of six against an expected twelve, and the slack only ever
	// helps.** A snapshot lost to the loopback widens the gap rather than
	// narrowing it, so the failures this can have are the ones worth having: an
	// ungated stream sits at one tick per snapshot and misses by a factor of
	// six.
	CHECK(ticksPerSnapshot >= 6.0);

	// And the world did not slow down with the stream. Sixty ticks a second
	// against three snapshots means the simulation is still ahead of what is
	// being sent, which is the whole point of separating the two rates.
	CHECK(advanced >= 24);
}

TEST_CASE("a client announces itself without being told to", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// The regression this exists for: the connector only ever replied. On a
	// datagram socket a server cannot send to an address nobody has written
	// from, so a client that waited to be spoken to waited forever - and the
	// suite missed it by having the *test* send an input first, which the real
	// client never does. Nothing here calls `Submit`.
	Remote remote;
	REQUIRE(remote.Start(16));

	for (int attempt = 0; attempt < 400 && !remote.Link->Joined(); attempt++) {
		remote.Link->Poll(remote.World, remote.Now);
		remote.Link->Advance(remote.Now);
		remote.Now += 1.0 / 60.0;
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(remote.Link->Joined());
}

TEST_CASE("a world too big for one datagram still streams", "[server][replication]") {
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **The other regression, and the reason 32 entities was not enough to
	// catch it by accident.** A tick's delta for this many entities is many
	// times a datagram, and it used to be built as one message that
	// `Link::Reserve` refused every tick - silently, because a refusal is
	// ordinary backpressure. The join worked and the world then never moved.
	Remote remote;
	REQUIRE(remote.Start(2000));
	REQUIRE(remote.Join(1200));
	REQUIRE(remote.Entities() == 2000);

	const uint64_t joinedAt = remote.Link->Applied();
	for (int tick = 0; tick < 240 && remote.Link->Applied() == joinedAt; tick++) {
		remote.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(remote.Link->Applied() > joinedAt);

	// Every entity carries what it was sent, not merely the first datagram's
	// worth. A split that dropped its tail would show as a short count here and
	// as nothing at all anywhere else.
	REQUIRE(remote.World.CountMatching<Transform>() == 2000);
}

TEST_CASE("what ReplicatedFirst holds arrives before the world it covers", "[server][replication]") {
	// **`D00122`, and it is only observable over a wire.** `ReplicatedFirst`
	// outranking every distance score orders the *stream*, and a client's first
	// view of a world is the join - one `ecs::Store::Save` chunked across ticks
	// in whatever order the store wrote its archetypes, which no score reaches
	// inside. So a loading screen was ranked first for every tick after the one
	// it was for.
	//
	// **What is asserted is *before* and not *eventually*.** At the moment the
	// preface lands this replica holds the screen and holds almost nothing else;
	// a case that only waited for the screen to turn up would pass against the
	// single-blob join this exists to replace.
	//
	// Identified by its size rather than by its name, because a name does not
	// cross: `ecs.InstanceName` and the class are not in
	// `DefaultReplicatedComponents`, so what a replica holds of the screen is
	// its `scene::Bounds` and its frame. Nine studs is nothing else in the
	// scene.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_replicatedfirst.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local screen = Instance.new(\"Part\")\n"
			 << "screen.Name = \"LoadingScreen\"\n"
			 << "screen.Anchored = true\n"
			 << "screen.Size = Vector3.new(9, 9, 9)\n"
			 << "screen.Parent = game:GetService(\"ReplicatedFirst\")\n"
			 // A world whose own snapshot takes tens of ticks. Without that
			 // there is no window for anything to be first in, and the case
			 // would be measuring the packet rate.
			 << "for index = 1, 1500 do\n"
			 << "\tlocal block = Instance.new(\"Part\")\n"
			 << "\tblock.Anchored = true\n"
			 << "\tblock.Position = Vector3.new(index % 40, 1, index // 40)\n"
			 << "\tblock.Parent = workspace\n"
			 << "end\n";
	}

	const auto holdsScreen = [](Store &world) {
		bool found = false;
		world.EachEntity([&](Entity entity) {
			const Bounds *bounds = world.Get<Bounds>(entity);
			found = found || (bounds != nullptr && bounds->HalfExtent.X == 4.5f);
		});
		return found;
	};

	Remote remote;
	REQUIRE(remote.Start(0, scene.string()));

	int prefaces = 0;
	bool joinedThen = true;
	bool screenThen = false;
	size_t entitiesThen = 0;

	// **Sampled from inside the apply rather than between polls, and that is
	// the difference between a check and a coin toss.** One `Connector::Poll`
	// drains the socket and applies every message that was in it, so a machine
	// with no spare core hands the preface and the world behind it to the same
	// call - and this case used to read its four facts *after* that call, by
	// which time the window it is about had closed. It failed six times in six
	// under a saturating build and passed on an idle one, which is a case
	// measuring the scheduler rather than the stream.
	//
	// `Replica::OnPreface` runs before anything of the world has been applied,
	// so what the closure sees is the preface and nothing else by construction.
	remote.Link->OnPreface([&](Store &world) {
		prefaces++;
		joinedThen = remote.Link->Joined();
		screenThen = holdsScreen(world);

		entitiesThen = 0;
		world.EachEntity([&](Entity) { entitiesThen++; });
	});

	REQUIRE(remote.Join(1200));

	// Once. A second would mean the server re-declared a preface mid-join,
	// which would put a loading screen back over a world already being drawn.
	REQUIRE(prefaces == 1);

	INFO("the preface landed with " << entitiesThen << " entities in the replica");
	CHECK(screenThen);

	// The ordering the stage enum is for, read where it is decided. `Joined`
	// is set by the world blob and this runs on the preface, so a build where
	// this is true is one where the two blobs have been merged back into one.
	CHECK_FALSE(joinedThen);

	// The container and what is under it, and none of the fifteen hundred parts
	// it was meant to be in front of.
	CHECK(entitiesThen >= 2);
	CHECK(entitiesThen < 32);

	// And the world blob behind it is still the complete picture, whose
	// authoritative sweep must leave what the preface put there alone.
	CHECK(remote.World.CountMatching<Transform>() > 1500);
	CHECK(holdsScreen(remote.World));

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

TEST_CASE("a connecting client becomes a player in the hosted world", "[server][replication]") {
	// **The thing that had to exist before ownership could be assigned to
	// anybody.** `scene::AddPlayer` had no production caller: every world this
	// engine ran had a `Players` service with nobody in it, so a server script
	// asking for the player to hand a body to had nothing to be handed.
	//
	// Observed from a *second* client's replica rather than from the server's
	// log, because a log line is not a fact a test can hold: client one joins, is
	// counted, and then client two joins - and what client one's world gains is
	// exactly the one entity that is client two's player.
	//
	// The leaving half is `engine.scene.ownership`'s and is asserted there
	// directly. It is not asserted here because it would mean waiting out a link
	// timeout, which is a slow test measuring the drop interval rather than the
	// behaviour.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	// **A scene with a part in it, which is not incidental.** An empty world
	// replicates only services, whose archetypes are small enough that the two
	// processes happen to agree about column order. A `Part` is nine components
	// wide and is what caught `Archetype::Read` reading columns in the reader's
	// id order rather than the writer's - the client could not join at all, and
	// the same mismatch on a narrower row loads silently wrong.
	//
	// Anchored, so it stays where it was put and the entity count below is not
	// racing a body falling out of the world.
	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_players.luau";
	{
		std::ofstream file(scene, std::ios::trunc);
		REQUIRE(file);
		file << "local block = Instance.new(\"Part\")\n"
			 << "block.Anchored = true\n"
			 << "block.Parent = workspace\n";
	}

	Remote first;
	REQUIRE(first.Start(0, scene.string()));
	REQUIRE(first.Join(400));
	Settle(first);

	const size_t alone = first.Entities();
	REQUIRE(alone > 0);

	// **Measured twice, so what is asserted is the rule and not the number.** A
	// `Player` is not one entity - `AddPlayer` furnishes it with a `PlayerGui`
	// - and pinning the count here would be pinning that arrangement from the
	// far side of a wire. Two more clients say the useful thing instead: each
	// arrival costs the same, and it is not nothing.
	Remote second;
	REQUIRE(second.Connect(first.Port));
	REQUIRE(second.Join(400));
	Settle(first);
	const size_t withTwo = first.Entities();

	Remote third;
	REQUIRE(third.Connect(first.Port));
	REQUIRE(third.Join(400));
	Settle(first);
	const size_t withThree = first.Entities();

	CHECK(withTwo > alone);
	CHECK(withThree - withTwo == withTwo - alone);

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

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
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	const engine::core::Vector3 after = first.World.Get<Transform>(rig->Root)->Frame.Position;

	// **Along X and not merely "somewhere else".** A character that fell through
	// the floor also moved, and asserting a bare inequality would pass for it.
	CHECK(after.X > before.X + 1.0f);

	// And the *other* client sees the same body in the new place, which is the
	// half that separates "the server moved it" from "everybody was told".
	Settle(second);
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
	Settle(client);

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

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}

TEST_CASE("a client's click is a shot the server resolves and everybody sees", "[server][replication]") {
	// **The other half of `D00109`, and the half that had never had a caller.**
	// `Server::ApplyInputs` decodes an `examples::Shot`, rewinds the client's
	// view with `Rewind::TickSeenBy`, hit-tests against the recorded history and
	// recolours what was struck - a complete server-authoritative feature whose
	// `examples::EncodeShot` was reachable from nothing in the tree. That entry
	// calls it two finished halves connected to nothing; this is the wire
	// between them.
	//
	// **Two clients, because one would prove less than it looks.** A client
	// shooting a prop would exercise the same code and would leave the
	// interesting property untested: the verdict is the *server's*, and what
	// makes that visible is a second client learning about a hit it did not
	// witness and did not decide. The shooter never says what it struck.
	//
	// **The target is a character and not a prop, and that is not a preference.**
	// `Server::ServeClients` records the rewind history over `Transform` and
	// `RigidBody` **filtered by `scene::Simulated`**, deliberately, so that
	// static geometry does not fill the ring with rows whose answer is the same
	// at every tick - a scripted anchored part is therefore invisible to the hit
	// test. An unanchored one that has come to rest is *not*: it keeps
	// `Simulated` when it sleeps and only loses `Motion`, which is exactly the
	// v0.15 bug about a standing player being unhittable, fixed. What is being
	// aimed at here is a character because that is what a shot means.
	if (!ServerAvailable()) {
		SKIP("the server program is not built into this preset");
	}

	const std::filesystem::path scene =
		std::filesystem::temp_directory_path() / "server_replication_shot.luau";
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

	Remote shooter;
	REQUIRE(shooter.Start(0, scene.string()));
	REQUIRE(shooter.Join(400));

	Remote victim;
	REQUIRE(victim.Connect(shooter.Port));
	REQUIRE(victim.Join(400));

	for (int tick = 0; tick < 200; tick++) {
		shooter.Tick();
		victim.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	REQUIRE(shooter.Mine != engine::ecs::NULL_ENTITY);
	REQUIRE(victim.Mine != engine::ecs::NULL_ENTITY);

	// **The shooter walks away first**, so that its own body is nowhere near the
	// ray. Both characters spawn at the same `SpawnLocation`, and the shooter's
	// root is in the rewind history exactly as the victim's is - a shot fired
	// from where they are standing could strike the shooter and the assertion
	// below would never know the difference.
	shooter.LookAlong(0.0f);
	shooter.Hold({engine::scene::KeyCode::W}, 120, &victim);

	// The victim's body, as the shooter's replica holds it. Found through the
	// join notice rather than by position, because a `Player` is what a client
	// is told about and the character hangs off it.
	const Entity victimCharacter = engine::scene::CharacterOf(shooter.World, victim.Mine);
	REQUIRE(victimCharacter != engine::ecs::NULL_ENTITY);

	const Character *rig = shooter.World.Get<Character>(victimCharacter);
	REQUIRE(rig != nullptr);
	const Entity victimRoot = rig->Root;
	REQUIRE(shooter.World.Get<Visual>(victimRoot) != nullptr);

	const engine::core::Color3 before = shooter.World.Get<Visual>(victimRoot)->Tint;

	bool recoloured = false;
	for (int attempt = 0; attempt < 200 && !recoloured; attempt++) {
		// Re-read every attempt: the victim is a body on a floor and drifts a
		// little, and a ray aimed once at where it used to be is a test that
		// depends on it not having settled.
		const Transform *placement = shooter.World.Get<Transform>(victimRoot);
		if (placement == nullptr) {
			break;
		}

		const engine::core::Vector3 at = placement->Frame.Position;
		const engine::core::Vector3 eye = at + engine::core::Vector3{20.0f, 0.0f, 0.0f};
		shooter.AimFrom(eye, (at - eye).Unit());

		// Fired repeatedly, because the input channel is unreliable by design:
		// `net/AGENTS.md` makes structure reliable and values not, so one lost
		// datagram is one click that never happened. A player clicking again is
		// what this models.
		shooter.Click();
		shooter.SubmitAim();
		shooter.Tick();
		victim.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));

		if (const Visual *visual = shooter.World.Get<Visual>(victimRoot); visual != nullptr) {
			// **The green channel, not the red.** A character's root is
			// already white, so "the red went up" cannot distinguish the
			// server's `{1, 0.2, 0.1}` from where it started - the first
			// version of this case asserted exactly that and passed nothing
			// while the server was striking every shot.
			recoloured = visual->Tint.G < before.G - 0.3f && visual->Tint.R > 0.9f;
		}
	}

	CHECK(recoloured);

	// **A hit takes health off, and this is the consequence
	// `docs/retired/DEFERRED.md` D00121 was waiting for.** Its reopen trigger was "anything in the engine
	// that damages a character", on the reading that a hit test with no
	// consequence does not need "dead" to mean anything - so the shot landing is
	// what makes the health model load-bearing rather than decorative, and this
	// is the assertion that says the two are wired together.
	const Entity victimHumanoid = [&] {
		const Character *now = shooter.World.Get<Character>(victimCharacter);
		return now == nullptr ? engine::ecs::NULL_ENTITY : now->Humanoid;
	}();
	REQUIRE(victimHumanoid != engine::ecs::NULL_ENTITY);

	const Humanoid *wounded = shooter.World.Get<Humanoid>(victimHumanoid);
	REQUIRE(wounded != nullptr);
	CHECK(wounded->Health < 100.0f);

	// **And the shooter cannot put it back**, which is the authority half and
	// the one only a real replica can prove: this store was made adopt-only by
	// `replication::Connector`, so `Store::SetProperty` refuses by name rather
	// than letting the write stand until the next delta overwrites it. A suite
	// that only called `SetAdoptOnly` by hand would be asserting its own setup.
	const engine::ecs::PropertyDescriptor *health = nullptr;
	for (const auto &property : engine::ecs::Classes::Describe(engine::scene::HumanoidClass()).Properties) {
		if (property.Name == Name("Health")) {
			health = &property;
		}
	}
	REQUIRE(health != nullptr);

	const float full = 100.0f;
	CHECK_FALSE(shooter.World.SetProperty(victimHumanoid, *health, &full, sizeof(full)));
	CHECK(shooter.World.Get<Humanoid>(victimHumanoid)->Health < 100.0f);

	// **And the victim sees the same verdict**, which is the half a single
	// client cannot assert. It did not fire, it was not asked, and the colour
	// reaches it as ordinary replicated `scene.Visual` - so a client that had
	// decided its own hits would be a client nothing downstream could
	// second-guess.
	const Entity ownRoot = [&] {
		const Entity character = engine::scene::CharacterOf(victim.World, victim.Mine);
		const Character *own = victim.World.Get<Character>(character);
		return own == nullptr ? engine::ecs::NULL_ENTITY : own->Root;
	}();
	REQUIRE(ownRoot != engine::ecs::NULL_ENTITY);

	bool seenByVictim = false;
	for (int tick = 0; tick < 200 && !seenByVictim; tick++) {
		victim.Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		if (const Visual *visual = victim.World.Get<Visual>(ownRoot); visual != nullptr) {
			seenByVictim = visual->Tint.G < before.G - 0.3f && visual->Tint.R > 0.9f;
		}
	}
	CHECK(seenByVictim);

	// The same for the health, and on the machine that lost it. A client draws
	// its own bar off this row, so a `scene.Humanoid` that stopped crossing
	// would leave every client watching a number that never moves.
	const Entity ownHumanoid = [&] {
		const Entity character = engine::scene::CharacterOf(victim.World, victim.Mine);
		const Character *own = victim.World.Get<Character>(character);
		return own == nullptr ? engine::ecs::NULL_ENTITY : own->Humanoid;
	}();
	REQUIRE(ownHumanoid != engine::ecs::NULL_ENTITY);
	REQUIRE(victim.World.Get<Humanoid>(ownHumanoid) != nullptr);
	CHECK(victim.World.Get<Humanoid>(ownHumanoid)->Health < 100.0f);

	std::error_code ignored;
	std::filesystem::remove(scene, ignored);
}
