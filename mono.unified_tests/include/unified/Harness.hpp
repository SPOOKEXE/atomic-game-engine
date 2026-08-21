#pragma once

// A server and a client in one process, with `net` cut out of the middle.
//
// **This exists because "the replicated world draws nothing" has three
// suspects and no way to tell them apart from outside.** Running `server
// --listen` beside `client --connect` puts a handshake, a UDP socket, packet
// framing, an encrypted stream, a reliability window and a bandwidth budget
// between the thing that serialises and the thing that draws - so a blank scene
// is equally consistent with a component that never got named, a datagram that
// never arrived, and a draw list that was filled and never read. Two programs
// and no shared address space is the worst possible place to find out which.
//
// So this holds both halves and joins them at the only seam that matters:
// `replication::Authority::Outgoing` hands its byte vectors **directly** to
// `replication::Replica::Receive`. Serialise straight into deserialise. There
// is no socket, no `net::Packet` header, no `net::Session`, no cipher, no
// acknowledgement window and no MTU - a message the authority produced is a
// message the replica sees, in the order it was produced, complete.
//
// **What that buys is a bisection, and it is the only thing it buys.** A
// failure that reproduces here is above `net`: a component nobody called
// `Replicate` on, a name the two ends spell differently, a snapshot the replica
// refused, a store the draw pass never walked. A failure that does *not*
// reproduce here is below it, and `mono.engine/replication/tests/Wire.hpp` -
// which runs this same exchange over a real loopback with real framing, real
// encryption and `net::LossyTransport` losing a seeded share of it - is where
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
// **The other arrangements are `unified/Crossing.hpp`'s.** This class is one
// point of a matrix - `Arrangement{}`, which is direct, no content and no
// discovery - and the reason it keeps a name of its own is that it is the point
// a bisection starts from. Everything below is that class's, unchanged.
//
// @tier client · escapes to server

#include <unified/Arrangement.hpp>
#include <unified/Crossing.hpp>

namespace unified {

	// A server, a client, and the serialiser wired to the deserialiser.
	//
	// Build it, `Join`, then `Step` in a loop and read the reports.
	//
	// @since v0.5
	class Harness final : public Crossing {
	  public:
		// Builds both worlds and admits the client. Starts the job system.
		//
		// @param settings How big, how fast, and what to drop.
		explicit Harness(const Settings &settings = {}) : Crossing(settings, Arrangement{}) {}
	};
}
