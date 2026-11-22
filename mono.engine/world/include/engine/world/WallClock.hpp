#pragma once

// Real time, anchored so a world can read it and still replay.
//
// **A world never reads the machine's clock while it runs.** The server reads
// the Unix time once, when a world is created, and keeps it in `WallClock`.
// Everything a script calls a date is that anchor plus the world's simulated
// elapsed time, so two runs that start from the same snapshot agree on every
// date they compute. The anchor is in the snapshot, so a replay restores it.
//
// `os.clock()` is the one real timer a script gets. The universe samples it
// once per driver tick and every world reads that sample for the whole tick,
// so every script in a tick agrees, and a recording keeps the sample per
// barrier so a replay hands back the same values.
//
// A client replica reads the server's date from `ServerClock`, which
// replication carries, and the latest authority tick it applied. So
// `os.time()` on a client and on its server name the same second.
//
// @tier L4 · shared

#include <cstdint>

namespace engine::world {

	// One world's anchor to real time. Not replicated: each process keeps its
	// own, so a client's local time zone stays the client's.
	struct WallClock {
		// Unix seconds when the world was created. What simulated elapsed time
		// is added to.
		double UnixEpoch = 0.0;

		// The real timer sample of the driver tick being run, in seconds from
		// an arbitrary start. What `os.clock()` returns. Left out of a
		// snapshot, which a recording's per-barrier samples stand in for.
		double TickClock = 0.0;

		// Seconds east of UTC of the machine's local time zone, read with the
		// epoch. What local time is UTC plus.
		int32_t UtcOffsetSeconds = 0;

		// Explicit padding, so a copy of this struct holds no uninitialised
		// bytes for a byte comparison to trip over.
		uint32_t Reserved = 0;
	};

	// The authority's clock, carried to replicas: its Unix time is
	// `UnixAtTickZero + tick * TickDelta`.
	//
	// **Constant while the authority's tick rate is.** A value that changed
	// every tick would be a delta to every client every tick on a world where
	// nothing else moved. The authority rebases and rewrites it only when its
	// rate changes, and a replica supplies the tick from `AuthorityTick`.
	struct ServerClock {
		// Unix seconds the authority's tick zero stands for.
		double UnixAtTickZero = 0.0;

		// The authority's tick delta, in seconds.
		double TickDelta = 0.0;
	};

	// The authority tick a replica last applied, and the replica's own
	// simulated elapsed time when it did. Written by `replication::Replica`;
	// local to the replica. Between messages the replica's own clock carries
	// the server's forward, so a quiet server's time still advances.
	struct AuthorityTick {
		uint64_t Tick = 0;
		double LocalElapsed = 0.0;
	};

	// Registers `world.WallClock`, `world.ServerClock` and
	// `world.AuthorityTick`. Idempotent. Called
	// with the mailbox types, so everything that touches a world has them.
	void RegisterWallClockTypes();

	// The machine's Unix time now, in seconds with millisecond resolution.
	double SampleUnixSeconds();

	// A monotonic timer in seconds from an arbitrary start. What a tick's
	// `TickClock` is sampled from.
	double SampleClockSeconds();

	// Seconds east of UTC of the machine's local time zone at `unixSeconds`.
	int32_t SampleUtcOffsetSeconds(double unixSeconds);
}
