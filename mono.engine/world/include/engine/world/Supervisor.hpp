#pragma once

// Which worlds run in which process, and what happens when one dies.
//
// **Processes are for crash isolation, not for speed.** Two processes
// simulating two worlds are not faster than two threads doing the same; they
// are more survivable. A world that aborts on an affinity violation takes down
// one host rather than the server.
//
// **Grouping is the whole policy.** Soft faults — a system throwing, a script
// erroring, a budget overrun — are quarantined per world by `World::Tick` and
// never need a process. A hard fault takes the address space, and nothing
// inside a process can arrange otherwise. So the only question a supervisor
// answers is *who shares an address space with whom*, and a world that cannot
// tolerate a neighbour's hard fault declares `Isolation::Dedicated`.
//
// **What restarting means.** A host that dies is respawned and its worlds are
// restored from their last snapshot. That is exact rather than approximate
// because a world is deterministic given its state and its inbox — the same
// property that makes a recording a faithful log. A host that keeps dying is
// held down rather than restarted forever, because a crash loop that looks
// alive on every dashboard is worse than one that stops.
//
// **How the driver talks to one.** Each host gets a `HostLink` over a channel
// created when it is spawned, and `Pump` drains every link once a barrier. A
// heartbeat arriving on the link is the same event as one reported by hand, so
// nothing downstream distinguishes an in-process host from a real one. The
// traffic a host's worlds posted arrives the same way and is handed to the
// driver's barrier as though those worlds were local — which is the whole
// claim: where a world runs is a deployment decision, not a design one.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/world/Enums.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/World.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace engine::world {

	// One process, and the worlds it was given.
	//
	// @since v0.2
	struct HostPlan {
		// What this host is called, for logs and for the directory.
		core::Name Name;

		// The worlds it hosts, by name. Names rather than ids, because an id
		// means something different in the process that reads it.
		std::vector<core::Name> Worlds;

		// Whether this host was created for one world that asked to be alone.
		bool Dedicated = false;
	};

	// How a supervisor spawns and watches hosts.
	//
	// @since v0.2
	struct SupervisorSettings {
		// The program a host runs. Empty means hosts are not spawned at all,
		// which is the in-process case.
		std::filesystem::path Program;

		// Arguments every host is started with, before its own.
		std::vector<std::string> Arguments;

		// How many `Shared` worlds may sit in one host.
		//
		// Several rather than one, because a process per subarea does not scale
		// to hundreds of them — and soft faults are quarantined per world
		// whatever the grouping.
		uint32_t WorldsPerHost = 8;

		// How long a host may go without a heartbeat before it is presumed
		// dead.
		double HeartbeatSeconds = 5.0;

		// How many times a host may be restarted before it is held down.
		//
		// A host that dies deterministically would otherwise respawn forever,
		// burning the machine while looking alive.
		uint32_t RestartLimit = 3;
	};

	// What a supervised host is doing.
	//
	// @since v0.2
	enum class HostState : uint8_t {
		// Not started.
		Idle,

		// Started and answering.
		Running,

		// Started and overdue on its heartbeat.
		Silent,

		// Stopped, and being restarted.
		Restarting,

		// Stopped too many times. Held down deliberately.
		Failed,
	};

	// Returns a stable, human-readable name for a host state.
	//
	// @param state The state to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(HostState state);

	// One host as the supervisor sees it.
	//
	// @since v0.2
	struct HostStatus {
		// What the host is called. A name rather than an index, because this
		// crosses a process boundary and an index derived from start order does
		// not survive one.
		core::Name Name;

		// Where the host is in its lifecycle.
		HostState State = HostState::Idle;

		// How many times it has been restarted.
		uint32_t Restarts = 0;

		// Seconds since its last heartbeat, or zero if it has never sent one.
		double SinceHeartbeat = 0.0;

		// The tick its last heartbeat reported.
		//
		// A heartbeat says the link is being serviced; this says the simulation
		// is. A host answering promptly while this number sits still is stuck
		// in a way a deadline alone would never notice.
		uint64_t Tick = 0;

		// What its last reported tick cost, in milliseconds.
		//
		// Measured in the host and sent, because a driver cannot time another
		// address space. This is the latest figure received, so it is one
		// barrier behind — which is worth far more than the nothing a driver
		// could otherwise say about where a host's time goes.
		float Milliseconds = 0.0f;

		// Whether it has a link the driver can speak on.
		bool Linked = false;

		// Whether it has said its worlds are built.
		bool Ready = false;

		// The worlds it was given.
		std::vector<core::Name> Worlds;
	};

	// Assigns worlds to hosts.
	//
	// Deterministic: the same worlds in the same order produce the same plan,
	// so two runs of a universe group identically and a restarted supervisor
	// rebuilds what was there.
	//
	// @param worlds  The worlds to place, with their isolation levels.
	// @param perHost How many `Shared` worlds may share one process.
	// @return One plan per host, in the order the hosts should be started.
	// @since v0.2
	std::vector<HostPlan> PlanHosts(const std::vector<WorldSettings> &worlds, uint32_t perHost);

	// Spawns hosts, watches them, and restarts the ones that die.
	//
	// @since v0.2
	class Supervisor {
	  public:
		// How a host is started.
		//
		// Injected so that the policy can be exercised without spawning real
		// processes, and so that the cross-process transport lands as a
		// different launcher rather than a change to this class.
		//
		// Returns `false` when the host could not be started.
		using Launcher = std::function<bool(const HostPlan &, parallel::Process &)>;

		// Creates a supervisor.
		//
		// @param settings How to spawn and watch.
		explicit Supervisor(const SupervisorSettings &settings = {});

		// Stops every host it started.
		~Supervisor();

		// A supervisor owns processes and is never copied.
		Supervisor(const Supervisor &) = delete;

		// A supervisor owns processes and is never copy-assigned.
		Supervisor &operator=(const Supervisor &) = delete;

		// Replaces how hosts are started.
		//
		// @param launcher The launcher to use, or an empty function to restore
		//                 the default one that spawns `Settings().Program`.
		void SetLauncher(Launcher launcher);

		// Starts a host for each plan.
		//
		// @param plans The plans to run.
		// @return The number of hosts that started.
		size_t Start(const std::vector<HostPlan> &plans);

		// Gives a host a link, in place of the one a spawn would have made.
		//
		// For an injected launcher — a test, or an in-process host — so that
		// the protocol can be exercised without a second process. The default
		// launcher calls this itself.
		//
		// @param host    The host the link belongs to.
		// @param channel This end of it.
		// @return `false` for an unknown host.
		bool Attach(core::Name host, std::unique_ptr<parallel::Channel> channel);

		// Drains every host's link.
		//
		// Call once a barrier, before `Poll`. A heartbeat arriving here is the
		// same event as one reported by hand, so an in-process host and a
		// spawned one are indistinguishable to everything downstream.
		//
		// Traffic a host's worlds posted accumulates in `Traffic()` for the
		// driver's barrier to apply.
		//
		// @param now The current time, in seconds.
		// @return The number of frames handled.
		size_t Pump(double now);

		// The bus traffic hosts have handed over since it was last taken.
		//
		// Each envelope carries the host that sent it, not just the world it
		// claims to come from — the router's check is *this host holds that
		// world*, and it cannot be made against a field the sender wrote.
		//
		// @return The traffic, in arrival order.
		std::span<const HostTraffic> Traffic() const {
			return Inbound;
		}

		// Takes the accumulated traffic, leaving none.
		//
		// @return The traffic, in arrival order.
		std::vector<HostTraffic> TakeTraffic();

		// Hands envelopes to one host.
		//
		// @param host    The host to send to.
		// @param traffic The envelopes it should apply at its next barrier.
		// @return `false` when the host is unknown or its link is gone.
		bool SendTo(core::Name host, std::span<const Envelope> traffic);

		// Hands a host what the buses answered for its worlds.
		//
		// The return half of `SendTo`. A host owns no bus backend, so every
		// answer its worlds get comes through here.
		//
		// @param host       The host to send to.
		// @param deliveries What to put in its worlds' inboxes.
		// @return `false` when the host is unknown or its link is gone.
		bool DeliverTo(core::Name host, std::span<const HostDelivery> deliveries);

		// Asks a host to shut down cleanly.
		//
		// Over the link rather than by signal, so the host finishes the tick it
		// is in and flushes what it owes. `StopAll` follows this with a
		// deadline and then a kill.
		//
		// @param host The host to ask.
		// @return `false` when the host is unknown or its link is gone.
		bool AskToStop(core::Name host);

		// The worlds hosts have reported as held down.
		//
		// A world past its own crash-loop cutoff has stopped simulating inside
		// a host that is otherwise healthy, and nothing outside that process
		// would know unless it said so.
		//
		// @return The world names, in the order they were reported.
		std::span<const core::Name> HeldDown() const {
			return Downed;
		}

		// Records that a host is alive.
		//
		// Called by `Pump` when a heartbeat arrives, and callable directly for
		// a host with no link of its own. The supervisor has no clock of its
		// own beyond the one it is given, so time is passed in rather than
		// read — which is what lets a test drive a five-second deadline in a
		// microsecond, and what keeps a wall clock out of anything that has to
		// be reproducible.
		//
		// @param host The host that reported in.
		// @param now  The current time, in seconds, on whatever clock the
		//             caller is using consistently.
		void Heartbeat(core::Name host, double now);

		// Checks every host, restarting the ones that have died or gone silent.
		//
		// @param now The current time, in seconds.
		// @return The number of hosts restarted by this call.
		size_t Poll(double now);

		// Stops every host.
		void StopAll();

		// What every host is doing.
		//
		// @return One record per host, in start order.
		std::vector<HostStatus> Hosts() const;

		// One host's record.
		//
		// @param host The host to ask about.
		// @return The record, or an empty one for an unknown host.
		HostStatus StatusOf(core::Name host) const;

		// The number of hosts being supervised.
		//
		// @return The host count.
		size_t Count() const {
			return Entries.size();
		}

		// How this supervisor was configured.
		//
		// @return The settings.
		const SupervisorSettings &Settings() const {
			return Settings_;
		}

	  private:
		struct Entry {
			HostPlan Plan;
			HostState State = HostState::Idle;
			parallel::Process Child;
			std::unique_ptr<HostLink> Link;
			uint32_t Restarts = 0;
			double LastHeartbeat = 0.0;
			uint64_t Tick = 0;
			float Milliseconds = 0.0f;
			bool EverBeat = false;
			bool Ready = false;
		};

		bool Launch(Entry &entry);
		Entry *Find(core::Name host);
		const Entry *Find(core::Name host) const;

		SupervisorSettings Settings_;
		Launcher Launch_;
		std::vector<Entry> Entries;

		// What hosts have handed over, awaiting the driver's barrier.
		std::vector<HostTraffic> Inbound;

		// Worlds reported past their own crash-loop cutoff.
		std::vector<core::Name> Downed;

		// Reused across barriers so a supervisor polled every tick stops
		// allocating.
		std::vector<HostFrame> Frames;
	};
}
