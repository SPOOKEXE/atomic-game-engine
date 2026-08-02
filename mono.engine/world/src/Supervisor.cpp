#include <engine/core/Log.hpp>
#include <engine/world/Supervisor.hpp>

#include <algorithm>
#include <string>

namespace engine::world {

	const char *Describe(HostState state) {
		switch (state) {
		case HostState::Idle:
			return "idle";
		case HostState::Running:
			return "running";
		case HostState::Silent:
			return "silent";
		case HostState::Restarting:
			return "restarting";
		case HostState::Failed:
			return "failed";
		}
		// No default label, so adding a state is a compiler warning here.
		return "?";
	}

	std::vector<HostPlan> PlanHosts(const std::vector<WorldSettings> &worlds, uint32_t perHost) {
		std::vector<HostPlan> plans;
		if (perHost == 0) {
			perHost = 1;
		}

		// Dedicated worlds first, each alone.
		//
		// Placed before the shared ones rather than interleaved, so the plan is
		// a function of the input and not of the order two different kinds of
		// world happened to appear in. A supervisor rebuilding after a restart
		// has to produce the same grouping.
		for (const WorldSettings &world : worlds) {
			if (world.IsolationLevel != Isolation::Dedicated || !world.Name.IsValid()) {
				continue;
			}

			HostPlan plan;
			plan.Name = core::Name("host." + std::string(world.Name.Text()));
			plan.Worlds.push_back(world.Name);
			plan.Dedicated = true;
			plans.push_back(std::move(plan));
		}

		// Then the shared ones, packed.
		size_t index = 0;
		HostPlan current;

		const auto flush = [&plans, &current, &index] {
			if (current.Worlds.empty()) {
				return;
			}
			current.Name = core::Name("host.shared." + std::to_string(index++));
			plans.push_back(std::move(current));
			current = HostPlan{};
		};

		for (const WorldSettings &world : worlds) {
			if (world.IsolationLevel == Isolation::Dedicated || !world.Name.IsValid()) {
				continue;
			}

			current.Worlds.push_back(world.Name);
			if (current.Worlds.size() >= perHost) {
				flush();
			}
		}
		flush();

		return plans;
	}

	Supervisor::Supervisor(const SupervisorSettings &settings) : Settings_(settings) {}

	Supervisor::~Supervisor() {
		// A supervisor that goes away must not leave hosts behind. An orphaned
		// host holds its worlds, its memory and its port, and nothing is left
		// that knows to stop it.
		StopAll();
	}

	void Supervisor::SetLauncher(Launcher launcher) {
		Launch_ = std::move(launcher);
	}

	bool Supervisor::Launch(Entry &entry) {
		if (Launch_) {
			return Launch_(entry.Plan, entry.Child);
		}

		if (Settings_.Program.empty()) {
			// No program means the hosts are in this process. Nothing to spawn,
			// and reporting failure would make an in-process universe look
			// broken.
			return true;
		}

		std::vector<std::string> arguments = Settings_.Arguments;
		arguments.emplace_back("--host");
		arguments.emplace_back(std::string(entry.Plan.Name.Text()));

		for (const core::Name world : entry.Plan.Worlds) {
			arguments.emplace_back("--world");
			arguments.emplace_back(std::string(world.Text()));
		}

		// Created before the spawn, because the child has to be holding its end
		// by the time it runs. A host that had to connect to something would
		// need an address, a retry and a timeout; an inherited handle needs
		// none of the three.
		parallel::ProcessChannel pair = parallel::MakeProcessChannel();
		if (!pair.Valid()) {
			return false;
		}

		if (!entry.Child.Start(Settings_.Program, arguments, std::move(pair.Remote))) {
			return false;
		}

		entry.Link = std::make_unique<HostLink>(std::move(pair.Local), entry.Plan.Name);
		return true;
	}

	bool Supervisor::Attach(core::Name host, std::unique_ptr<parallel::Channel> channel) {
		Entry *entry = Find(host);
		if (entry == nullptr) {
			return false;
		}

		entry->Link = std::make_unique<HostLink>(std::move(channel), host);
		return true;
	}

	size_t Supervisor::Pump(double now) {
		size_t handled = 0;

		for (Entry &entry : Entries) {
			if (entry.Link == nullptr) {
				continue;
			}

			Frames.clear();
			entry.Link->Receive(Frames);

			for (HostFrame &frame : Frames) {
				handled++;

				switch (frame.Signal) {
				case HostSignal::Ready:
					entry.Ready = true;
					entry.Tick = frame.Tick;
					Heartbeat(entry.Plan.Name, now);
					break;

				case HostSignal::Heartbeat:
					entry.Tick = frame.Tick;
					Heartbeat(entry.Plan.Name, now);
					break;

				case HostSignal::Traffic:
					// Tagged with the entry's own name rather than the frame's.
					// A host that stamped somebody else's name on a frame would
					// otherwise have its traffic checked against the host it
					// named, which is the check answering to the thing it is
					// checking.
					//
					// Appended in arrival order and not sorted here. The
					// driver's barrier is the one place that decides ordering,
					// and a second sort on the way in would be this file having
					// an opinion about it.
					for (Envelope &envelope : frame.Traffic) {
						Inbound.push_back(HostTraffic{entry.Plan.Name, std::move(envelope)});
					}
					break;

				case HostSignal::Faulted:
					if (frame.World.IsValid() &&
						std::find(Downed.begin(), Downed.end(), frame.World) == Downed.end()) {
						Downed.push_back(frame.World);
						ENGINE_WARN(
							"host '{}' reports world '{}' held down.",
							entry.Plan.Name.Text(),
							frame.World.Text()
						);
					}
					break;

				case HostSignal::Deliveries:
					// Driver to host only. A host answering its own worlds'
					// bus requests would be the second source of truth this
					// design exists to avoid, so it is refused rather than
					// merged.
					ENGINE_WARN(
						"host '{}' sent deliveries, which only a driver issues.", entry.Plan.Name.Text()
					);
					break;

				case HostSignal::Stop:
					// A host asking its driver to stop is not part of the
					// protocol. Counted as handled and ignored, rather than
					// treated as an instruction from something the driver
					// supervises.
					ENGINE_WARN(
						"host '{}' sent a stop, which a driver does not take.", entry.Plan.Name.Text()
					);
					break;
				}
			}
		}

		return handled;
	}

	std::vector<HostTraffic> Supervisor::TakeTraffic() {
		std::vector<HostTraffic> taken;
		taken.swap(Inbound);
		return taken;
	}

	bool Supervisor::SendTo(core::Name host, std::span<const Envelope> traffic) {
		Entry *entry = Find(host);
		if (entry == nullptr || entry->Link == nullptr) {
			return false;
		}
		return entry->Link->SendTraffic(traffic);
	}

	bool Supervisor::DeliverTo(core::Name host, std::span<const HostDelivery> deliveries) {
		Entry *entry = Find(host);
		if (entry == nullptr || entry->Link == nullptr) {
			return false;
		}
		return entry->Link->SendDeliveries(deliveries);
	}

	bool Supervisor::AskToStop(core::Name host) {
		Entry *entry = Find(host);
		if (entry == nullptr || entry->Link == nullptr) {
			return false;
		}

		HostFrame frame;
		frame.Signal = HostSignal::Stop;
		return entry->Link->Send(frame);
	}

	size_t Supervisor::Start(const std::vector<HostPlan> &plans) {
		size_t started = 0;

		for (const HostPlan &plan : plans) {
			Entry entry;
			entry.Plan = plan;

			if (Launch(entry)) {
				entry.State = HostState::Running;
				started++;
			} else {
				ENGINE_ERROR("could not start host '{}'", plan.Name.Text());
				entry.State = HostState::Failed;
			}

			Entries.push_back(std::move(entry));
		}

		return started;
	}

	Supervisor::Entry *Supervisor::Find(core::Name host) {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [host](const Entry &entry) {
			return entry.Plan.Name == host;
		});
		return found == Entries.end() ? nullptr : &*found;
	}

	const Supervisor::Entry *Supervisor::Find(core::Name host) const {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [host](const Entry &entry) {
			return entry.Plan.Name == host;
		});
		return found == Entries.end() ? nullptr : &*found;
	}

	void Supervisor::Heartbeat(core::Name host, double now) {
		Entry *entry = Find(host);
		if (entry == nullptr) {
			return;
		}

		entry->LastHeartbeat = now;
		entry->EverBeat = true;

		if (entry->State == HostState::Silent) {
			// It was overdue and then spoke. Believing the heartbeat over the
			// deadline is right: the deadline is a guess about a host that has
			// stopped, and a host that answers has not.
			entry->State = HostState::Running;
		}
	}

	size_t Supervisor::Poll(double now) {
		size_t restarted = 0;

		for (Entry &entry : Entries) {
			if (entry.State == HostState::Failed || entry.State == HostState::Idle) {
				continue;
			}

			// A process that ended is dead whatever its heartbeat said.
			bool dead = false;
			if (entry.Child.Started()) {
				const parallel::ProcessStatus status = entry.Child.Poll();
				dead = !status.Alive();
			}

			// A closed link says the same thing sooner, and says it for an
			// in-process host that has no child to reap. The heartbeat deadline
			// stays as the answer for a host that is wedged rather than gone —
			// those are two different failures and only one of them shows here.
			if (!dead && entry.Link != nullptr && !entry.Link->Connected()) {
				dead = true;
			}

			if (!dead && entry.EverBeat && Settings_.HeartbeatSeconds > 0.0) {
				const double silent = now - entry.LastHeartbeat;
				if (silent > Settings_.HeartbeatSeconds) {
					// Overdue rather than confirmed dead. A host that has
					// stopped answering may be wedged rather than gone, and
					// killing it is what makes the two the same case.
					entry.State = HostState::Silent;
					dead = true;
				}
			}

			if (!dead) {
				continue;
			}

			if (entry.Restarts >= Settings_.RestartLimit) {
				if (entry.State != HostState::Failed) {
					ENGINE_WARN(
						"host '{}' has been restarted {} times and is being held down.",
						entry.Plan.Name.Text(),
						entry.Restarts
					);
				}
				entry.State = HostState::Failed;
				continue;
			}

			entry.State = HostState::Restarting;
			entry.Restarts++;
			entry.Ready = false;

			// The old link belongs to a process that is gone. Kept, it would
			// report `Connected()` until the kernel noticed, and the respawn
			// would then write into a socket nobody reads.
			entry.Link.reset();

			// Killed before respawning, because a silent host may still be
			// running and two processes owning one world is worse than none.
			if (entry.Child.Started()) {
				entry.Child.Kill();
				entry.Child.Wait();
			}

			if (Launch(entry)) {
				entry.State = HostState::Running;
				entry.LastHeartbeat = now;
				entry.EverBeat = false;
				restarted++;

				ENGINE_INFO(
					"restarted host '{}' ({} of {})",
					entry.Plan.Name.Text(),
					entry.Restarts,
					Settings_.RestartLimit
				);
			} else {
				entry.State = HostState::Failed;
			}
		}

		return restarted;
	}

	void Supervisor::StopAll() {
		// Asked over the link first, all of them, before any waiting. Asking
		// one and waiting for it before asking the next would make shutting
		// down N hosts take N deadlines.
		for (Entry &entry : Entries) {
			if (entry.Link != nullptr) {
				HostFrame frame;
				frame.Signal = HostSignal::Stop;
				entry.Link->Send(frame);
			}
		}

		for (Entry &entry : Entries) {
			if (entry.Child.Started()) {
				entry.Child.RequestStop();
				entry.Child.Kill();
				entry.Child.Wait();
			}
			if (entry.Link != nullptr) {
				entry.Link->Close();
				entry.Link.reset();
			}
			entry.State = HostState::Idle;
			entry.Ready = false;
		}
	}

	std::vector<HostStatus> Supervisor::Hosts() const {
		std::vector<HostStatus> found;
		found.reserve(Entries.size());

		for (const Entry &entry : Entries) {
			HostStatus status;
			status.Name = entry.Plan.Name;
			status.State = entry.State;
			status.Restarts = entry.Restarts;
			status.SinceHeartbeat = entry.EverBeat ? entry.LastHeartbeat : 0.0;
			status.Tick = entry.Tick;
			status.Linked = entry.Link != nullptr && entry.Link->Connected();
			status.Ready = entry.Ready;
			status.Worlds = entry.Plan.Worlds;
			found.push_back(std::move(status));
		}

		return found;
	}

	HostStatus Supervisor::StatusOf(core::Name host) const {
		const Entry *entry = Find(host);
		if (entry == nullptr) {
			return {};
		}

		HostStatus status;
		status.Name = entry->Plan.Name;
		status.State = entry->State;
		status.Restarts = entry->Restarts;
		status.SinceHeartbeat = entry->EverBeat ? entry->LastHeartbeat : 0.0;
		status.Tick = entry->Tick;
		status.Linked = entry->Link != nullptr && entry->Link->Connected();
		status.Ready = entry->Ready;
		status.Worlds = entry->Plan.Worlds;
		return status;
	}
}
