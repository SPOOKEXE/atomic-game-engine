#pragma once

// A server and a client in one process, with `net` cut out of the middle.
//
// **This exists because "the replicated world draws nothing" has three
// suspects and no way to tell them apart from outside.** Running `server
// --listen` beside `client --connect` puts a handshake, a UDP socket, packet
// framing, an encrypted stream, a reliability window and a bandwidth budget
// between the thing that serialises and the thing that draws — so a blank scene
// is equally consistent with a component that never got named, a datagram that
// never arrived, and a draw list that was filled and never read. Two programs
// and no shared address space is the worst possible place to find out which.
//
// So this holds both halves and joins them at the only seam that matters:
// `replication::Authority::Outgoing` hands its byte vectors **directly** to
// `replication::Replica::Receive`. Serialise straight into deserialise. There
// is no socket, no `net::Packet` header, no `net::Session`, no cipher, no
// acknowledgement window and no MTU — a message the authority produced is a
// message the replica sees, in the order it was produced, complete.
//
// **What that buys is a bisection, and it is the only thing it buys.** A
// failure that reproduces here is above `net`: a component nobody called
// `Replicate` on, a name the two ends spell differently, a snapshot the replica
// refused, a store the draw pass never walked. A failure that does *not*
// reproduce here is below it, and `mono.engine/replication/tests/Wire.hpp` —
// which runs this same exchange over a real loopback with real framing, real
// encryption and `net::LossyTransport` losing a seeded share of it — is where
// that one gets cornered. **Neither replaces the other.** This one cannot see a
// message that did not fit in a datagram, and that class of bug has bitten this
// module four times.
//
// **It draws through `mono.client`'s own seam rather than a copy of it.**
// `client::BuildReplicatedWorld` and `client::RecordReplicatedTick` are the
// functions `--connect` runs, so what this reports is what a real client would
// draw. A harness that filled its own draw list would prove the harness.
//
// **Headless.** Nothing here opens a window or touches a device. It links
// `Mono::client` for the two functions above, which drags the renderer onto the
// link line and the shaders into the staged directory; that is a real cost and
// it is the price of testing the client's seam rather than an imitation of it.
//
// **Time is passed in, never read.** A tick is a call and a frame is a call, so
// a run of this is reproducible from its settings alone and a stall is
// something a caller states. Same rule as `net/AGENTS.md` and
// `replication/AGENTS.md`.
//
// @tier client · escapes to server

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/SnapshotBuffer.hpp>

#include <client/Demo.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace unified {

	// How the two halves are built and how hard the middle is made to be.
	//
	// @since v0.5
	struct Settings {
		// Entities in the server's placeholder world.
		uint32_t Entities = 64;

		// The authority's tick rate, in ticks per second. The server's own
		// default, because that is what a client meets in practice and the
		// mismatch with the client's 60 is a thing worth being able to see.
		double TickRate = 30.0;

		// Frames drawn per tick.
		//
		// Four is enough that "moved once per tick" and "moved every frame" are
		// far apart: judder shows as three identical frames in every four.
		int FramesPerTick = 4;

		// Ordinals of outgoing messages to discard without telling the
		// authority.
		//
		// Counted across the whole run from zero. **Silent, which is what makes
		// it loss** — `Authority::Unsent` is a refusal the sender knows about
		// and is repaired next tick, and the interesting failure is the one
		// nobody is told about. There is no percentage here on purpose: a
		// nominated ordinal is a test and a percentage is a flake with a story
		// attached, which is the argument `net::LossSettings` already makes.
		std::vector<uint64_t> Drop;

		// How the replicated world is interpolated.
		//
		// Defaulted from `TickRate` by the constructor when left at zero, so
		// that a harness told the server runs at 30 does not quietly measure
		// its delay against 60.
		engine::replication::InterpolationSettings Interpolation;

		// Workers the job system runs with.
		//
		// One, because a diagnostic that reports a different number on every
		// run is not a diagnostic. The placeholder world's motion goes through
		// `EachParallel`, so this cannot be zero.
		unsigned Workers = 1;
	};

	// What crossed in one tick, and what came out the other side.
	//
	// **Every field is a fact about one of the four stages** — produced, sent,
	// applied, drawn — because the whole point is to say which stage lost it.
	//
	// @since v0.5
	struct Report {
		// The server tick this describes.
		uint64_t Tick = 0;

		// Messages the authority produced for this client.
		size_t Messages = 0;

		// Bytes in them, before anything would have framed or sealed them.
		//
		// **Not what would go on a wire.** `net` adds a header and a tag per
		// datagram and this counts neither, so a figure near
		// `net::MAXIMUM_MESSAGE_BYTES` here is already a message that would not
		// fit — which is the one thing this harness can say about the wire
		// without having one.
		size_t Bytes = 0;

		// The largest single message, which is the number that matters when
		// asking whether a tick would have crossed a real link.
		size_t LargestMessage = 0;

		// Messages discarded by `Settings::Drop`.
		size_t Dropped = 0;

		// The last tick the replica holds in full.
		//
		// Behind `Tick` by design. Equal to zero long after the join means the
		// snapshot never finished.
		uint64_t Applied = 0;

		// Entities carrying a `scene::Transform` on each side.
		//
		// **The first place a blank scene shows.** Equal counts and an empty
		// draw list is a drawing problem; unequal counts is a replication one.
		size_t ServerEntities = 0;
		size_t ClientEntities = 0;

		// Rows the client's draw pass produced on the last frame of this tick.
		//
		// Below `ClientEntities` means rows arrived without a `Bounds` or a
		// `Visual` — the components that are sent once in the snapshot and
		// never again, so losing them is permanent and invisible everywhere
		// else.
		size_t Drawn = 0;

		// Where the probe entity is on the server, where the client's store
		// says it is, and where it was actually drawn.
		//
		// The three differ, and each difference is meant: server against store
		// is the network's lag, and store against drawn is the snapshot
		// buffer's delay. Store equal to drawn on every frame is the judder
		// `D00010` was opened for.
		float ServerX = 0.0f;
		float ClientX = 0.0f;
		float DrawnX = 0.0f;

		// How far behind the newest received tick the world was drawn.
		double Behind = 0.0;

		// Frames in this tick on which the drawn position did not move.
		//
		// **The judder counter, and the number to read first.** Zero is a world
		// being interpolated. Equal to `Settings::FramesPerTick - 1` is a world
		// stepping once per tick, which is what this looks like with the
		// snapshot buffer taken out.
		int FrozenFrames = 0;
	};

	// A server, a client, and the serialiser wired to the deserialiser.
	//
	// Build it, `Join`, then `Step` in a loop and read the reports.
	//
	// @since v0.5
	class Harness {
	  public:
		// Builds both worlds and admits the client. Starts the job system.
		//
		// @param settings How big, how fast, and what to drop.
		explicit Harness(const Settings &settings = {});

		// Stops the job system.
		~Harness();

		Harness(const Harness &) = delete;
		Harness &operator=(const Harness &) = delete;

		// Steps until the joining snapshot has been applied.
		//
		// @param limit How many ticks to allow before giving up.
		// @return `true` once the client holds the world.
		bool Join(int limit = 512);

		// One tick: simulate, publish, hand over, apply, record, draw.
		//
		// @return What crossed and what came out.
		Report Step();

		// The authority's world.
		engine::ecs::Store &ServerWorld() {
			return Server;
		}

		// The replicated world, with its draw list and its snapshot buffer.
		engine::ecs::Store &ClientWorld() {
			return Client;
		}

		// The entity whose position the reports follow.
		//
		// The lowest-numbered row the placeholder world made, so it is the same
		// entity across runs with the same entity count.
		engine::ecs::Entity Probe() const {
			return Probe_;
		}

		// What the client's half has seen.
		const engine::replication::Replica &Replica() const {
			return Replica_;
		}

		// What the server's half has sent.
		const engine::replication::Authority &Authority() const {
			return Authority_;
		}

		// The tick the server is on.
		uint64_t Tick() const {
			return Tick_;
		}

		// Messages handed over since the harness was built.
		uint64_t Handed() const {
			return Handed_;
		}

	  private:
		// One entity's X in one store, or zero when it holds no such row.
		float PositionOf(engine::ecs::Store &store, engine::ecs::Entity entity) const;

		// One entity's X as it was actually drawn, by its ordinal in the walk
		// that filled the list.
		float DrawnPositionOf(const client::DrawList &drawList, engine::ecs::Entity entity);

		Settings Options;

		engine::ecs::Store Server;
		engine::ecs::Scheduler ServerSystems;

		engine::ecs::Store Client;
		engine::ecs::Scheduler ClientSystems;

		engine::replication::Authority Authority_;
		engine::replication::Replica Replica_;
		engine::replication::ClientId Handle;

		engine::ecs::Entity Probe_;
		uint64_t Tick_ = 0;
		uint64_t Handed_ = 0;
	};
}
