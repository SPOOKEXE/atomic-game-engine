#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Process.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <server/Server.hpp>
#include <server/Simulation.hpp>
#include <thread>

namespace server {

	namespace {
		// Read between ticks by the loop, written by Stop from anywhere.
		std::atomic<bool> StopRequested{false};

		// The primary world's name.
		//
		// A name rather than an index, because that is what a bus envelope, a
		// snapshot and a supervisor all carry. An index means something
		// different in every process that reads it.
		constexpr const char *PRIMARY = "server.world";

		// The one topic every chattering world uses.
		constexpr const char *CHATTER_TOPIC = "placeholder.chatter";

		// This executable, for spawning hosts of itself.
		//
		// `Paths::Base()` is the directory the binary sits in, which is where
		// it is staged. Deriving it rather than taking it from `argv[0]` means
		// a server started through a shell alias or a symlink still finds
		// itself.
		std::filesystem::path ThisProgram() {
#if defined(_WIN32)
			return engine::core::Paths::Base() / "server.exe";
#else
			return engine::core::Paths::Base() / "server";
#endif
		}

		bool ReadFile(const std::filesystem::path &path, std::vector<std::byte> &bytes) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return false;
			}

			const auto size = static_cast<size_t>(file.tellg());
			bytes.resize(size);
			file.seekg(0);
			file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
			return static_cast<bool>(file);
		}

		bool WriteFile(const std::filesystem::path &path, std::span<const std::byte> bytes) {
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				return false;
			}
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
			return static_cast<bool>(file);
		}
	}

	bool Server::Initialise(const Options &options) {
		Settings = options;

		if (!Settings.AssetsDirectory.empty()) {
			engine::core::Paths::SetAssetsOverride(Settings.AssetsDirectory);
			ENGINE_INFO("assets from {}", Settings.AssetsDirectory.string());
		}

		if (!Settings.GamePath.empty()) {
			// TODO(v0.5+): hand this to gamefile::Reader. Refusing loudly beats
			// accepting a path and hosting something else.
			ENGINE_WARN(
				"--game is accepted but has no effect until the game file format lands. "
				"Hosting the placeholder world instead of '{}'.",
				Settings.GamePath
			);
		}

		if (Settings.TickRate <= 0.0) {
			ENGINE_ERROR("--tick-rate must be greater than zero");
			return false;
		}

		// How many processes will share this machine, this one included. A
		// driver works it out from the hosts it is about to spawn and passes
		// the answer down, because it is the only process that knows.
		const unsigned processes =
			Settings.Processes > 0 ? Settings.Processes : 1u + static_cast<unsigned>(PlannedHosts());

		engine::parallel::Jobs::Start(engine::parallel::WorkersPerHost(processes));

		// Before anything builds or loads a world. A snapshot names its
		// components, so a process that has not registered them cannot resolve
		// them — and registration order fixes iteration order, so it belongs at
		// startup rather than wherever a type is first touched.
		RegisterPlaceholderComponents();

		engine::world::UniverseSettings universe;

		// A host owns worlds and no bus backend. The topics, the MemoryStore
		// map and the DataStore records belong to the driver, because two
		// processes each holding a copy would be two answers to the same key.
		universe.Federated = IsHost();

		engine::world::DriverSettings driver;
		driver.Universe = universe;
		driver.Hosts.WorldsPerHost = Settings.WorldsPerHost;

		// A host is not a different executable. Defaulting to this one is what
		// makes the grouping a deployment decision rather than an engine one —
		// there is no `mono.host` to keep in step.
		driver.Hosts.Program = Settings.HostProgram.empty() ? ThisProgram() : Settings.HostProgram;

		// Passed through so a host builds the same scene this process would
		// have. A game file replaces this at v0.5.
		driver.Hosts.Arguments = {
			"--unpaced",
			"--entities",
			std::to_string(Settings.Entities),
			"--tick-rate",
			std::to_string(Settings.TickRate),

			// The worker budget, worked out once by the process that knows how
			// many there will be.
			"--processes",
			std::to_string(1u + PlannedHosts()),
		};

		if (Settings.Chatter) {
			driver.Hosts.Arguments.emplace_back("--chatter");
		}

		Driver_ = std::make_unique<engine::world::Driver>(driver);
		StopRequested.store(false);

		if (IsHost()) {
			return InitialiseHost();
		}

		if (!Settings.ReplayPath.empty()) {
			std::vector<std::byte> bytes;
			if (!ReadFile(Settings.ReplayPath, bytes)) {
				ENGINE_ERROR("could not read the recording at '{}'", Settings.ReplayPath.string());
				return false;
			}

			Replayer_ = std::make_unique<engine::world::Replayer>();
			engine::core::ByteReader reader(bytes);
			if (!Replayer_->Load(reader)) {
				ENGINE_ERROR("'{}' is not a recording this build reads", Settings.ReplayPath.string());
				return false;
			}

			// A snapshot carries state, never code, so the systems are
			// registered again here — exactly as this program does on a normal
			// start, because it *is* the same program.
			const bool restored =
				Replayer_->Restore(Worlds(), [](engine::world::Universe &into, engine::world::WorldId id) {
					into.Enter(id, [](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
						RegisterPlaceholderSystems(store, systems);
					});
				});

			if (!restored) {
				ENGINE_ERROR("the recording's snapshot could not be restored");
				return false;
			}

			PrimaryWorld = Worlds().Find(engine::core::Name(PRIMARY));

			// Recording a *replay* is what proves the replay path reproduces
			// the run rather than merely surviving it: the two files are
			// compared byte for byte by `just replay-check`. Without this the
			// flag combination was accepted and silently ignored, which is the
			// worst of the three possible behaviours.
			if (!BeginRecording()) {
				return false;
			}

			Running = true;

			ENGINE_INFO(
				"replaying {} barrier(s) from {}", Replayer_->Barriers(), Settings.ReplayPath.string()
			);
			return true;
		}

		engine::world::WorldSettings world;
		world.Name = engine::core::Name(PRIMARY);
		world.TickRate = Settings.TickRate;

		PrimaryWorld = Worlds().Create(world);
		if (!PrimaryWorld.IsValid()) {
			ENGINE_ERROR("could not create the primary world");
			return false;
		}

		Worlds().Enter(PrimaryWorld, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
			BuildPlaceholderWorld(store, systems, Settings.Entities);
			if (Settings.Chatter) {
				store.SetResource(Chatter{engine::core::Name(CHATTER_TOPIC)});
			}
		});

		if (!StartHosts()) {
			return false;
		}

		if (!BeginRecording()) {
			return false;
		}

		Running = true;
		ENGINE_INFO("server ready at {:.1f} Hz", Settings.TickRate);
		return true;
	}

	size_t Server::PlannedHosts() const {
		if (Settings.RemoteWorlds.empty()) {
			return 0;
		}

		// The same planner the driver uses, run early for its count alone. Two
		// different answers to "how many hosts" is how a worker budget stops
		// matching the processes it was budgeted for.
		std::vector<engine::world::WorldSettings> remote;
		remote.reserve(Settings.RemoteWorlds.size());
		for (const std::string &name : Settings.RemoteWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			remote.push_back(world);
		}

		return engine::world::PlanHosts(remote, Settings.WorldsPerHost).size();
	}

	bool Server::StartHosts() {
		if (Settings.RemoteWorlds.empty()) {
			return true;
		}

		std::vector<engine::world::WorldSettings> remote;
		remote.reserve(Settings.RemoteWorlds.size());

		for (const std::string &name : Settings.RemoteWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			world.TickRate = Settings.TickRate;
			remote.push_back(world);
		}

		const size_t started = Driver_->Start(remote);
		if (started == 0) {
			ENGINE_ERROR("no host could be started for {} world(s)", remote.size());
			return false;
		}

		ENGINE_INFO("started {} host(s) for {} world(s)", started, remote.size());
		return true;
	}

	bool Server::InitialiseHost() {
		auto channel = engine::parallel::AdoptInheritedChannel();
		if (channel == nullptr) {
			// A host with no driver is a process that would tick worlds nobody
			// asked for and answer to nobody. Refusing is the honest outcome,
			// and it is also what stops `--host` from looking like a way to run
			// the server with a nicer name.
			ENGINE_ERROR("--host was given but this process was not started with a channel to a driver");
			return false;
		}

		Link = std::make_unique<engine::world::HostLink>(
			std::move(channel), engine::core::Name(Settings.HostName)
		);

		if (Settings.HostWorlds.empty()) {
			ENGINE_ERROR("host '{}' was granted no worlds", Settings.HostName);
			return false;
		}

		for (const std::string &name : Settings.HostWorlds) {
			engine::world::WorldSettings world;
			world.Name = engine::core::Name(name);
			world.TickRate = Settings.TickRate;

			const engine::world::WorldId id = Worlds().Create(world);
			if (!id.IsValid()) {
				ENGINE_ERROR("host '{}' could not create world '{}'", Settings.HostName, name);
				return false;
			}

			Worlds().Enter(id, [this](engine::ecs::Store &store, engine::ecs::Scheduler &systems) {
				BuildPlaceholderWorld(store, systems, Settings.Entities);
				if (Settings.Chatter) {
					store.SetResource(Chatter{engine::core::Name(CHATTER_TOPIC)});
				}
			});

			// The first world granted is the one `Enter` reaches. A host holds
			// several and a caller outside it addresses them by name, so this
			// is a convenience rather than a distinction the host makes.
			if (!PrimaryWorld.IsValid()) {
				PrimaryWorld = id;
			}
		}

		engine::world::HostFrame ready;
		ready.Signal = engine::world::HostSignal::Ready;
		Link->Send(ready);

		Running = true;
		ENGINE_INFO(
			"host '{}' holding {} world(s) at {:.1f} Hz",
			Settings.HostName,
			Settings.HostWorlds.size(),
			Settings.TickRate
		);
		return true;
	}

	bool Server::ServiceLink() {
		if (Link == nullptr) {
			return true;
		}

		Frames.clear();
		Link->Receive(Frames);

		bool asked = false;
		for (const engine::world::HostFrame &frame : Frames) {
			switch (frame.Signal) {
			case engine::world::HostSignal::Stop:
				// Noted rather than acted on here: the tick that is already
				// underway finishes, and what this host owes goes up with it.
				asked = true;
				break;

			case engine::world::HostSignal::Deliveries:
				for (const engine::world::HostDelivery &delivery : frame.Deliveries) {
					if (!Worlds().Deliver(delivery.World, delivery.Message)) {
						ENGINE_WARN(
							"host '{}': a delivery for '{}', which is not one of mine.",
							Settings.HostName,
							delivery.World.Text()
						);
					}
				}
				break;

			case engine::world::HostSignal::Traffic:
			case engine::world::HostSignal::Ready:
			case engine::world::HostSignal::Heartbeat:
			case engine::world::HostSignal::Faulted:
				// Host to driver only. A driver sending one is a driver with a
				// bug, and acting on it would hide that.
				ENGINE_WARN(
					"host '{}': a driver sent a {} frame, which only a host sends.",
					Settings.HostName,
					engine::world::Describe(frame.Signal)
				);
				break;
			}
		}

		if (!Link->Connected()) {
			// The driver is gone. Continuing would leave worlds ticking with
			// nobody to answer their bus requests and nobody to stop them,
			// which is the orphan a supervisor exists to prevent.
			ENGINE_WARN("host '{}': the driver went away.", Settings.HostName);
			return false;
		}

		return !asked;
	}

	bool Server::BeginRecording() {
		if (Settings.RecordPath.empty()) {
			return true;
		}

		Recorder_ = std::make_unique<engine::world::Recorder>();
		if (!Recorder_->Begin(Worlds())) {
			ENGINE_ERROR("the world cannot be snapshotted, so it cannot be recorded");
			return false;
		}

		ENGINE_INFO("recording to {}", Settings.RecordPath.string());
		return true;
	}

	void Server::Shutdown() {
		if (Recorder_ && Recorder_->Recording_() && !Settings.RecordPath.empty()) {
			engine::core::ByteWriter writer;
			if (Recorder_->Write(writer) && WriteFile(Settings.RecordPath, writer.Bytes())) {
				ENGINE_INFO("wrote {} barrier(s) to {}", Recorder_->Barriers(), Settings.RecordPath.string());
			} else {
				ENGINE_ERROR("could not write the recording to '{}'", Settings.RecordPath.string());
			}
		}

		if (Link != nullptr) {
			Link->Close();
			Link.reset();
		}

		Running = false;
		Recorder_.reset();
		Replayer_.reset();
		Driver_.reset();
		engine::parallel::Jobs::Stop();
	}

	void Server::Stop() {
		StopRequested.store(true);
	}

	bool Server::Enter(const std::function<void(engine::ecs::Store &)> &body) {
		if (!Driver_ || !PrimaryWorld.IsValid()) {
			return false;
		}
		return Worlds().Enter(PrimaryWorld, body) == engine::world::WorldStatus::Ok;
	}

	RunSummary Server::Run() {
		RunSummary summary;
		if (!Running) {
			return summary;
		}

		const double budget = 1.0 / Settings.TickRate;
		const auto budgetNanoseconds = static_cast<uint64_t>(budget * 1e9);
		const auto delta = static_cast<float>(budget);

		const uint64_t started = engine::core::Clock::Nanoseconds();
		uint64_t nextTickAt = started;
		double totalTickSeconds = 0.0;

		const auto ticksSoFar = [this] { return Worlds().StatisticsOf(PrimaryWorld).Ticks; };

		while (Running && !StopRequested.load()) {
			const uint64_t tickStarted = engine::core::Clock::Nanoseconds();

			engine::core::FrameGraph::BeginFrame();

			if (Replayer_) {
				// The recording decides the frame time as well as the traffic:
				// replaying with a different one would run a different number
				// of ticks and stop being the same run.
				if (!Replayer_->Step(Worlds())) {
					engine::core::FrameGraph::EndFrame();
					break;
				}
				if (Recorder_) {
					// The recorded delta, not this run's. Capturing a measured
					// one would write a file that differs from its source in
					// every barrier for a reason that has nothing to do with
					// whether the replay was faithful.
					Recorder_->Capture(Worlds(), Replayer_->LastFrameSeconds());
				}
			} else {
				// Before the tick, so what the driver decided last barrier is
				// in the inboxes by the time the systems that read them run.
				if (!ServiceLink()) {
					engine::core::FrameGraph::EndFrame();
					break;
				}

				// A fixed delta, not the measured one. A tick is a function of
				// its state and its inbox; feeding it real elapsed time would
				// make a recorded run unreplayable and every physics result
				// machine-dependent.
				// The driver's barrier rather than the universe's, so worlds
				// held by a host route through the same buses in the same
				// order as the ones held here. With no hosts the two are the
				// same call plus five lines of nothing, which is why there is
				// only one path.
				Driver_->Tick(delta, static_cast<double>(engine::core::Clock::Nanoseconds()) / 1e9);
				if (Recorder_) {
					Recorder_->Capture(Worlds(), delta);
				}

				if (Link != nullptr) {
					// A federated universe collects and orders its worlds'
					// requests without applying them, so this is exactly what a
					// driver's barrier would have had.
					Link->SendTraffic(Worlds().LastTraffic());

					// The world's own cumulative count, not the universe's
					// per-barrier one. A driver watching this for a host that
					// is wedged rather than dead needs a number that only ever
					// goes up.
					Link->Heartbeat(Worlds().StatisticsOf(PrimaryWorld).Ticks);
				}
			}

			// Presentation is a separate call because a *client* renders one
			// world while the rest keep simulating. A headless server has no
			// such choice: `PreRender` is where deriving what to send lives —
			// the same shape as deriving what to draw — so it runs every tick,
			// with an alpha of zero because nothing here interpolates.
			Worlds().Present(PrimaryWorld, delta, 0.0f);

			engine::core::FrameGraph::EndFrame();
			ENGINE_PROFILE_FRAME();

			const uint64_t tickEnded = engine::core::Clock::Nanoseconds();
			const auto spent = static_cast<double>(tickEnded - tickStarted) / 1e9;

			totalTickSeconds += spent;
			summary.SlowestTickMilliseconds =
				std::max(summary.SlowestTickMilliseconds, static_cast<float>(spent * 1000.0));

			// From the world, which counted the tick it just ran. The loop does
			// not keep its own tally.
			const uint64_t ticks = ticksSoFar();

			if (Settings.MaximumTicks >= 0 && ticks >= static_cast<uint64_t>(Settings.MaximumTicks)) {
				break;
			}

			const double elapsed = static_cast<double>(tickEnded - started) / 1e9;
			if (Settings.Seconds > 0.0 && elapsed >= Settings.Seconds) {
				break;
			}

			if (Settings.Unpaced || Replayer_) {
				// A replay is not paced: it reproduces a run rather than
				// performing one, and sleeping between barriers would only make
				// it take as long as the original did.
				continue;
			}

			// Pace against an absolute schedule rather than sleeping for
			// "budget minus spent". The latter accumulates the sleep's own
			// overshoot, so a server drifts slower than its stated tick rate
			// and nothing in the numbers says why.
			nextTickAt += budgetNanoseconds;
			const uint64_t now = engine::core::Clock::Nanoseconds();
			if (now < nextTickAt) {
				std::this_thread::sleep_for(std::chrono::nanoseconds(nextTickAt - now));
			} else {
				summary.Overruns++;
				// Far behind: give up on catching the missed ticks rather than
				// spiralling. A server that tries to make up a lost second by
				// running thirty ticks back to back falls further behind.
				if (now - nextTickAt > budgetNanoseconds * 4) {
					nextTickAt = now;
				}
			}
		}

		summary.Ticks = ticksSoFar();
		summary.Seconds = static_cast<double>(engine::core::Clock::Nanoseconds() - started) / 1e9;
		summary.MeanTickMilliseconds =
			summary.Ticks > 0
				? static_cast<float>(totalTickSeconds / static_cast<double>(summary.Ticks) * 1000.0)
				: 0.0f;

		ENGINE_INFO(
			"{} tick(s) over {:.2f}s · mean {:.3f} ms · slowest {:.3f} ms · {} overrun(s)",
			summary.Ticks,
			summary.Seconds,
			summary.MeanTickMilliseconds,
			summary.SlowestTickMilliseconds,
			summary.Overruns
		);

		return summary;
	}
}
