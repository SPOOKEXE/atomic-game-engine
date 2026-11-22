#pragma once

// The `Remote` harness for the server replication suites: one real server child
// process, one UDP client and its replica store. See Replication.cpp for why.
//
#include <engine/assets/Signature.hpp>
#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/game/Play.hpp>
#include <engine/game/PortalSession.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/net/Transport.hpp>
#include <engine/net/Wire.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Observation.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/characters/Characters.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/characters/Input.hpp>
#include <engine/scene/characters/Tools.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/RemoteEvent.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

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
	inline void RegisterTypes() {
		engine::scene::RegisterSceneClasses();

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

	inline std::string RedParts(Store &world) {
		std::string names;
		world.EachEntity([&](Entity entity) {
			const auto *visual = world.Get<Visual>(entity);
			if (visual == nullptr || visual->Tint.R < 0.99f || visual->Tint.G >= 0.3f ||
				visual->Tint.B >= 0.2f) {
				return;
			}
			const Name name = world.InstanceNameOf(entity);
			if (!name.IsValid()) return;
			if (!names.empty()) names += ", ";
			names += name.Text();
		});
		return names;
	}

	inline std::filesystem::path ServerProgram() {
		return engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	}

	inline bool ServerAvailable() {
		return std::filesystem::exists(ServerProgram());
	}

	inline engine::assets::SigningKey Identity(uint8_t fill) {
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		seed.fill(static_cast<std::byte>(fill));
		std::optional<engine::assets::SigningKey> key = engine::assets::SigningKey::FromSeed(seed);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	// A port nothing is using, found by binding one and letting it go.
	//
	// There is a window between the release and the server's bind, and it is
	// accepted rather than closed: the alternative is the server reporting its
	// ephemeral port back over a pipe, which is a protocol invented for a test.
	// The caller retries, which turns a lost race into a slower test rather than
	// a failing one.
	inline uint16_t FreePort() {
		std::unique_ptr<Transport> probe = MakeUdpTransport(0);
		return probe == nullptr ? 0 : probe->Local().Port;
	}

	// A server process listening on a port, and a socket pointed at it.
	struct Remote {
		Process Child;
		std::unique_ptr<Transport> Socket;
		std::unique_ptr<Connector> Link;
		engine::replication::ReplicationObservations Observations;
		Store World{"replica"};
		double Now = engine::core::Clock::Seconds();

		// The port the child bound, so a second client can be pointed at the
		// same server rather than starting one of its own.
		uint16_t Port = 0;

		// The `Player` the server said is this client's, or null until it does.
		//
		// **Recorded here because it is the one fact a replica cannot derive.**
		// Every other row in `World` arrived as replicated state; this arrives
		// as a per-client message, which is the whole point of `game/Play.hpp`.
		Entity Mine;
		std::optional<engine::game::PlayerMotion> NativeMotion;
		bool AutomaticFreshAdmission = true;
		bool FreshAdmissionSent = false;

		// @param entities How many Parts the default scene builds.
		// @param game     A scene to host instead, or empty for the default Luau scene.
		// @param extra    Further arguments for the child, for a case that is
		//                 about a setting rather than about a world.
		bool Start(
			uint32_t entities,
			const std::string &game = {},
			const std::vector<std::string> &extra = {},
			const engine::assets::SigningKey *identity = nullptr
		) {
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
				engine::replication::ConnectorSettings connecting;
				connecting.ClientIdentity = identity;
				Now = engine::core::Clock::Seconds();
				Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now, connecting);
				Link->SetObservations(Name("replica"), Name("server"), {}, &Observations);
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
			Now = engine::core::Clock::Seconds();
			Link = std::make_unique<Connector>(*Socket, Endpoint::LoopbackIPv4(port), Now);
			Link->SetObservations(Name("replica"), Name("server"), {}, &Observations);
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
				engine::game::PlayerMotion motion;
				if (engine::game::DecodePlayerMotion(message, motion)) {
					NativeMotion = motion;
					return;
				}
				engine::game::PortalSessionMessage admission;
				if (engine::game::DecodePortalSession(message, admission) &&
					admission.Kind == engine::game::PortalSessionKind::Ready && admission.Attempt == 1) {
					Mine = admission.Player;
					World.SetResource(engine::scene::LocalPlayer{Mine});
					return;
				}
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
			// Network deadlines follow the real server process. A fixed step per
			// millisecond poll expires handshakes before a busy server can answer.
			Now = engine::core::Clock::Seconds();

			// Poll performs transport admission. The application then explicitly
			// asks for a fresh player, as the real client does; a portal resume
			// uses the same point without creating another character.
			Link->Poll(World, Now);
			if (AutomaticFreshAdmission && !FreshAdmissionSent && Link->Admitted()) {
				engine::game::PortalSessionMessage fresh{};
				fresh.Kind = engine::game::PortalSessionKind::Fresh;
				fresh.Attempt = 1;
				FreshAdmissionSent = Link->SendUser(engine::game::EncodePortalSession(fresh), Now);
			}
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
			constexpr int STEP_MILLISECONDS = 1;
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
	inline void Settle(Remote &remote) {
		for (int tick = 0; tick < 200; tick++) {
			remote.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
}
