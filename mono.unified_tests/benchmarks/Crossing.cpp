// One multiplayer tick, end to end, and what each layer of it costs.
//
// **Every other benchmark in this repository measures one module and this one
// measures the seam between all of them**, which is the same reason the module
// exists at all. `engine.replication.bench.protocol` knows what serialising a
// delta costs and `engine.net.bench.framing` knows what framing a packet costs,
// and neither of them can say what a *tick* costs - because a tick is a world
// simulated, an authority published, messages carried, a replica applied, a
// snapshot buffer recorded and a draw list filled, and no module links enough
// of the others to run one.
//
// **The arrangements are the measurement.** A row is not interesting on its own;
// the difference between two rows is. `direct` has nothing between the halves,
// so it is the cost of the game with the network removed. `loopback` is the same
// tick with a real `net` link - handshake done, framing on, encryption on - so
// the difference between the two is precisely what the wire costs per tick, and
// it is a number nothing else in this repository can produce. `lossy` adds a
// link that drops datagrams, so its difference from `loopback` is what
// retransmission costs when something actually goes missing. `relayed` adds
// content moving underneath the game, and `advertised` adds discovery beside it.
//
// **The entity rows are the scaling question a server operator asks.** Doubling
// the world doubles what the authority surveys and what the replica applies, and
// it does not double the wire's per-packet cost - so the shape of the growth
// tells an operator whether they are limited by the simulation or by the link,
// which is the difference between buying a bigger machine and buying a better
// one.
//
// **What this cannot do is bisect**, and it is not trying to. A row that got
// slower says a tick got slower and says nothing about which module did it; the
// per-module suites are what answer that, and they are better at it than
// anything here could be. The claim this file makes is narrower and is not made
// anywhere else: that the layers add up to what they are supposed to add up to.
//
// Time is passed in, as everywhere in this module. `Crossing::Now` advances by
// one tick period per `Step`, so a run is reproducible from its settings and the
// figures are the code's rather than the machine's clock's.

#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unified/Arrangement.hpp>
#include <unified/Crossing.hpp>

TEST_SUITE_ID("unified.bench.crossing")

using engine::testing::Consume;
using unified::Arrangement;
using unified::Content;
using unified::Crossing;
using unified::Discovery;
using unified::Report;
using unified::Settings;
using unified::Transport;

namespace crossing_bench {
	// Ticks per sample. Thirty is a second of server time at the default rate,
	// which is long enough that the joining snapshot is behind us and the rows
	// measure a steady tick rather than a connection.
	constexpr size_t TICKS = 30;

	// A world the size a small session has.
	constexpr uint32_t ENTITIES = 64;

	// The crossing a row is measured against, rebuilt when the row changes.
	//
	// **Exactly one is alive at a time, and that is not an economy.** A
	// crossing owns transports, sessions, a published content store and a
	// dependency on the process-wide job pool; nine of them alive together and
	// then destroyed in one go at exit is a teardown order nothing else in this
	// repository produces, and it hangs. Holding one matches what every test
	// here does and what a program does.
	//
	// Building one publishes content to a disk and completes a handshake, and
	// that lands inside the first sample of each row. It does not reach the
	// report: `BenchMain` takes the *minimum* across samples and runs a row's
	// samples consecutively, so the row that pays for the build is never the
	// row that is printed.
	Crossing &Running(const Arrangement &arrangement, uint32_t entities = ENTITIES) {
		// **Started here, before the static below is constructed, and that
		// ordering is the whole reason this line exists.** A `Crossing`'s
		// destructor stops the job pool when the last one goes; the pool's own
		// state is a static inside `Engine::parallel` created by the first
		// `Start`. Function-local statics are destroyed in reverse order of
		// construction, so a crossing held in a static that was constructed
		// *before* the pool would be destroyed after it - and the `Stop` in its
		// destructor would join workers whose pool no longer exists. That hangs
		// at exit, after every row has already printed, which is a spectacularly
		// unhelpful way to fail. Touching the pool first puts it earlier in the
		// order and therefore later in the teardown.
		engine::parallel::Jobs::Start(1);

		static Arrangement wiring;
		static uint32_t held = 0;
		static std::unique_ptr<Crossing> live;

		if (live != nullptr && wiring == arrangement && held == entities) {
			return *live;
		}

		Settings settings;
		settings.Entities = entities;
		// One worker, for the reason `Settings::Workers` gives: a diagnostic
		// that reports a different number on every run is not a diagnostic. It
		// also means these rows are a *serial* tick, which is the honest
		// baseline - the parallel win is `engine.parallel.bench.contention`'s
		// subject and would otherwise be mixed into every figure here.
		settings.Workers = 1;

		// Released before the next is built, not after.
		live.reset();
		live = std::make_unique<Crossing>(settings, arrangement);
		live->Join();
		wiring = arrangement;
		held = entities;
		return *live;
	}

	// Runs `TICKS` ticks and reports what crossed, so nothing can be elided and
	// a row that stopped carrying anything is visible rather than fast.
	size_t Run(Crossing &crossing) {
		size_t carried = 0;
		for (size_t tick = 0; tick < TICKS; tick++) {
			const Report report = crossing.Step();
			carried += report.Messages + report.ClientEntities;
		}
		return carried;
	}
}

using namespace crossing_bench;

// --- what the wire costs ------------------------------------------------------
//
// Three rows, one subtraction each. Read them in order and the differences are
// the layers: `loopback` minus `direct` is `net`, and `lossy` minus `loopback`
// is repairing what a link lost.

BENCH("Step · direct · 64 entities", TICKS) {
	// The game with the network removed. `Authority::Outgoing` hands its byte
	// vectors straight to `Replica::Receive` - no socket, no header, no cipher,
	// no acknowledgement window - so this is simulate, publish, apply, record
	// and draw, and nothing else.
	Consume(Run(Running(Arrangement{})));
}

BENCH("Step · loopback · 64 entities", TICKS) {
	// The same tick over a real `net` link: a completed handshake, `Packet`
	// framing, a sealed stream and a reliability window. **The difference from
	// the row above is what the wire costs per tick**, and it is the figure no
	// module's own suite can produce, because `replication` does not know it is
	// on a socket and `net` does not know what it is carrying.
	Consume(Run(Running(Arrangement{.Carrying = Transport::Loopback})));
}

BENCH("Step · lossy · 64 entities", TICKS) {
	// The same link with datagrams going missing on the client's end. The
	// difference from `loopback` is retransmission and the receiver's stall
	// while a gap is filled - the cost of the link working, rather than of the
	// link existing.
	Consume(Run(Running(Arrangement{.Carrying = Transport::Lossy})));
}

// --- what runs beside the game ------------------------------------------------

BENCH("Step · direct+relayed · 64 entities", TICKS) {
	// Content moving underneath the game: a `server::ContentRelay` rationing
	// routes out of a published store and a `client::ContentLink` reassembling
	// them. **This is the row that says whether streaming a level competes with
	// replicating it**, which is a question about two modules and therefore has
	// nowhere else it could be asked.
	Consume(Run(Running(Arrangement{.Serving = Content::Relayed})));
}

BENCH("Step · direct+advertised · 64 entities", TICKS) {
	// Discovery beside the game: a beacon announcing and a directory
	// collecting, every few ticks rather than every tick. Expected to be near
	// free, and the row is here because "expected to be near free" is how a
	// thing that is not gets shipped.
	Consume(Run(Running(Arrangement{.Finding = Discovery::Advertised})));
}

BENCH("Step · lossy+relayed+advertised · 64 entities", TICKS) {
	// Everything at once, which is what a real session on a real network is.
	// Against the sum of the individual differences above, this row is whether
	// the layers add or interact - content over a lossy link is a reliable
	// ordered channel with a hole in it, and that is neither the relay's case
	// nor the link's.
	Consume(Run(Running(
		Arrangement{
			.Carrying = Transport::Lossy, .Serving = Content::Relayed, .Finding = Discovery::Advertised
		}
	)));
}

// --- what the world size costs ------------------------------------------------
//
// Eight times the entities across these three rows. The authority surveys and
// the replica applies per entity, so those grow; the link's per-packet cost does
// not. Which of the two dominates is what decides whether a server operator
// needs a faster machine or a better connection.

BENCH("Step · direct · 512 entities", TICKS) {
	Consume(Run(Running(Arrangement{}, 512)));
}

BENCH("Step · direct · 4096 entities", TICKS) {
	Consume(Run(Running(Arrangement{}, 4096)));
}

BENCH("Step · loopback · 4096 entities", TICKS) {
	// The wire at a world size where a tick's delta is large enough to be cut
	// into several datagrams. Against `direct · 4096` the difference is the
	// wire again, and against `loopback · 64` it is how much of the wire's cost
	// scales with what it carries rather than with how often it is asked.
	Consume(Run(Running(Arrangement{.Carrying = Transport::Loopback}, 4096)));
}
