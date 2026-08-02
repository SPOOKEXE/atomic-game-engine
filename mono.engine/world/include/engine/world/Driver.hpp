#pragma once

// One barrier over worlds here and worlds elsewhere.
//
// `Universe` routes buses. `Supervisor` starts hosts and watches them. Neither
// knows about the other, and this is the twenty lines that join them — kept as
// its own type rather than folded into either, because a `Universe` that knew
// how to spawn a process would be a simulation layer that knew about processes,
// and a `Supervisor` that knew about envelopes would be a second router.
//
// **The order in a tick is the whole design.**
//
//     1. pump the links      — what hosts said since the last barrier
//     2. ingest their traffic — checked against the host that sent it
//     3. tick the universe   — one barrier, local and remote worlds together
//     4. dispatch outward    — what the barrier decided for remote worlds
//     5. poll the hosts      — restart what died
//
// Step 3 is the claim: a world's bus behaviour does not depend on which process
// holds it, because there is one router and it runs once. Step 2 before it and
// step 4 after are what make that true rather than aspirational.
//
// **A host is granted worlds, and only those.** The driver's directory is the
// only thing that knows which host holds which world; a host is told about its
// own and nothing else. That is what makes `IngestTraffic`'s check possible,
// and it is why a host cannot address a neighbour except through a bus.
//
// @tier L4 · shared

#include <engine/core/Name.hpp>
#include <engine/world/Supervisor.hpp>
#include <engine/world/Universe.hpp>
#include <engine/world/World.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace engine::world {

	// How a driver splits its worlds between itself and its hosts.
	//
	// @since v0.2
	struct DriverSettings {
		// The universe the driver runs locally.
		UniverseSettings Universe;

		// How hosts are spawned and watched.
		SupervisorSettings Hosts;
	};

	// What one driver barrier did.
	//
	// @since v0.2
	struct DriverStatistics {
		// Frames taken off host links.
		size_t FramesReceived = 0;

		// Envelopes hosts handed over and the driver accepted.
		size_t TrafficAccepted = 0;

		// Envelopes refused because the host does not hold the world they
		// claimed to come from.
		//
		// **Not zero is a bug or an attack, never a load figure.** A host only
		// ever posts on behalf of worlds it was granted, so anything here means
		// either the driver's directory and the host's disagree, or something
		// is pretending.
		size_t TrafficRefused = 0;

		// Deliveries sent out to hosts.
		size_t DeliveriesSent = 0;

		// Deliveries that could not be sent because a link was gone.
		size_t DeliveriesDropped = 0;

		// Hosts restarted by this barrier.
		size_t Restarted = 0;
	};

	// A universe, its hosts, and one barrier over both.
	//
	// @since v0.2
	class Driver {
	  public:
		// Creates a driver.
		//
		// @param settings How to run the universe and its hosts.
		explicit Driver(const DriverSettings &settings = {});

		// Stops every host.
		~Driver();

		// A driver owns processes and is never copied.
		Driver(const Driver &) = delete;

		// A driver owns processes and is never copy-assigned.
		Driver &operator=(const Driver &) = delete;

		// The worlds this process holds, plus the record of the ones it does
		// not.
		//
		// @return The universe.
		Universe &Worlds() {
			return Universe_;
		}

		// The worlds this process holds, plus the record of the ones it does
		// not.
		//
		// @return The universe.
		const Universe &Worlds() const {
			return Universe_;
		}

		// The hosts.
		//
		// @return The supervisor.
		Supervisor &Hosts() {
			return Supervisor_;
		}

		// The hosts.
		//
		// @return The supervisor.
		const Supervisor &Hosts() const {
			return Supervisor_;
		}

		// Places worlds into hosts, starts them, and records them.
		//
		// The grouping is `PlanHosts`', which is deterministic — the same
		// worlds in the same order produce the same plan, so a restarted driver
		// rebuilds what was there.
		//
		// Every world placed is registered in the universe as `Remote`, so the
		// buses can address it from the first barrier rather than from whenever
		// its host happens to answer.
		//
		// @param remote The worlds to give to hosts.
		// @return The number of hosts started.
		size_t Start(const std::vector<WorldSettings> &remote);

		// One barrier over everything.
		//
		// @param frameSeconds Wall seconds since the last call.
		// @param now          The current time, in seconds, on whatever clock
		//                     the caller is using consistently. Passed in
		//                     rather than read, so a heartbeat deadline is
		//                     testable in a microsecond.
		void Tick(float frameSeconds, double now);

		// Runs a world's presentation phase, if this process holds it.
		//
		// A remote world draws in its own host, or does not draw at all.
		//
		// @param id           The world to present.
		// @param frameSeconds Wall seconds the frame took.
		// @param alpha        How far the frame sits between two ticks.
		// @return `NoSuchWorld` for a world this process does not hold.
		WorldStatus Present(WorldId id, float frameSeconds, float alpha);

		// Asks every host to stop, then stops them.
		void Stop();

		// What the last barrier did.
		//
		// @return The statistics.
		const DriverStatistics &Statistics() const {
			return Stats;
		}

	  private:
		DriverSettings Settings_;
		Universe Universe_;
		Supervisor Supervisor_;
		DriverStatistics Stats;

		// Reused between barriers so a driver stops allocating.
		std::vector<HostDelivery> Batch;
		std::vector<Envelope> Envelopes;
	};
}
