#pragma once

// What a driver and a supervised host say to each other.
//
// A host is not a new program: it is the server started in host mode, holding
// some of the universe's worlds in its own address space so that a hard fault
// in one takes that process rather than the server. The driver keeps the buses,
// the directory and the recording; the host keeps worlds and ticks them.
//
// **The protocol is small on purpose.** Four kinds of frame, none of them a
// request that expects an answer, because a driver that blocked waiting for a
// host would have handed that host the power to stop the universe. Everything
// here is fire-and-forget in exactly the way `Postbox` already is - a world
// posting to a bus does not wait either, and for the same reason.
//
//     driver                                   host
//       Traffic  ───────── envelopes ────────▶   applied at the host's barrier
//       Stop     ───────── shut down ────────▶
//                ◀──────── Ready ───────────    worlds built, first tick due
//                ◀──────── Heartbeat ───────    still alive, tick N
//                ◀──────── Traffic ─────────    what its worlds posted
//
// **Names, never ids.** A `WorldId` is an index into one process's registry and
// means something else in the other. Everything addressed here is a
// `core::Name`, which is the same rule a snapshot follows.
//
// **The link is not the failure detector.** A closed channel says a host is
// gone; a missed heartbeat says one is stuck. Both matter and they are not the
// same event, which is why the heartbeat exists at all when the channel already
// reports a death.
//
// @tier L4 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/parallel/Channel.hpp>
#include <engine/world/Bus.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::world {

	// What a frame is.
	//
	// @since v0.2
	enum class HostSignal : uint8_t {
		// Host to driver: the worlds are built and the first tick is due.
		//
		// Sent once. A driver that never sees it has a host that started and
		// could not do the one thing it was started for - which is a different
		// problem from one that died, and worth telling apart.
		Ready,

		// Host to driver: still alive, at this tick.
		Heartbeat,

		// Host to driver: bus envelopes its worlds posted.
		Traffic,

		// Driver to host: what the buses answered, for its worlds' inboxes.
		//
		// The other half of `Traffic`, and a separate signal rather than a
		// direction-dependent reading of one - a frame whose meaning depends on
		// who is holding it is a frame somebody eventually reads from the wrong
		// side.
		Deliveries,

		// Driver to host: shut down cleanly.
		//
		// Asked rather than signalled, so a host gets to finish the tick it is
		// in and flush what it owes. A supervisor follows it with a deadline
		// and then a kill.
		Stop,

		// Host to driver: one of my worlds faulted and is held down.
		//
		// Soft faults are quarantined by the host and the driver never needs to
		// know - except that a world held down by the crash-loop cutoff has
		// stopped simulating, and something outside the host has to be able to
		// say so.
		Faulted,
	};

	// Returns a stable, human-readable name for a signal.
	//
	// @param signal The signal to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(HostSignal signal);

	// One delivery, and the world in the host it belongs to.
	//
	// By name, because a `WorldId` is an index into one process's registry.
	//
	// @since v0.2
	struct HostDelivery {
		// The world whose inbox this goes into.
		core::Name World;

		// What arrived for it.
		Delivery Message;
	};

	// One envelope, and the host that handed it over.
	//
	// The attribution has to survive the trip from the link to the router,
	// because the router's check is *this host holds that world*. Resolving the
	// host from the envelope's own sender field instead would be circular - it
	// would verify the claim against itself.
	//
	// @since v0.2
	struct HostTraffic {
		// The host that sent it.
		core::Name Host;

		// What one of its worlds posted.
		Envelope Message;
	};

	// One message on the link.
	//
	// @since v0.2
	struct HostFrame {
		// What this frame is.
		HostSignal Signal = HostSignal::Heartbeat;

		// The host that sent it, or is meant to receive it.
		core::Name Host;

		// The world a `Faulted` frame is about. Unset otherwise.
		core::Name World;

		// The sender's tick count, for `Heartbeat` and `Ready`.
		//
		// A number that stops moving is a host that is stuck rather than dead,
		// which a heartbeat alone cannot distinguish - the heartbeat says the
		// link is being serviced, and this says the simulation is.
		uint64_t Tick = 0;

		// What the sender's last tick cost, in milliseconds.
		//
		// **A driver cannot time a host.** The work happened in another address
		// space, and the only honest number is the one the host measured and
		// sent - which is what `FrameGraph::Report` exists to plot. A driver
		// that timed the link instead would be graphing its own poll interval.
		float Milliseconds = 0.0f;

		// The UDP replication port this host bound, for `Ready`.
		//
		// Zero means the host is not serving clients. The actual bound port is
		// sent rather than the requested one because an ephemeral request is the
		// normal way supervised hosts avoid racing over a configured range.
		uint16_t Port = 0;

		// The envelopes a `Traffic` frame carries.
		std::vector<Envelope> Traffic;

		// What a `Deliveries` frame carries.
		std::vector<HostDelivery> Deliveries;
	};

	// Writes a frame.
	//
	// @param writer The writer to append to.
	// @param frame  The frame to write.
	// @since v0.2
	void WriteHostFrame(core::ByteWriter &writer, const HostFrame &frame);

	// Reads a frame.
	//
	// @param reader The reader to consume.
	// @param frame  Filled in on success, untouched otherwise.
	// @return `false` on a truncated or unrecognised frame.
	// @since v0.2
	bool ReadHostFrame(core::ByteReader &reader, HostFrame &frame);

	// One end of the link between a driver and one host.
	//
	// Owns the channel and the encoding, so that neither side writes framing of
	// its own. Both sides use this class; which one you are is decided by which
	// signals you send, not by a different type - a second type would be a
	// second encoder, and two encoders agree until they do not.
	//
	// @since v0.2
	class HostLink {
	  public:
		// Takes over a channel.
		//
		// @param channel The transport. Null makes every call a no-op, which is
		//                what an unsupervised process holds.
		// @param name    This end's host name, stamped on everything sent.
		explicit HostLink(std::unique_ptr<parallel::Channel> channel, core::Name name = {});

		// Whether the other end is still there.
		//
		// @return `true` while the link can carry anything.
		bool Connected() const;

		// Sends a frame.
		//
		// Never blocks. A frame that does not fit is dropped and counted rather
		// than queued forever, because the queue is what a slow peer would
		// otherwise turn into a leak.
		//
		// @param frame The frame to send.
		// @return `false` when it could not be sent.
		bool Send(const HostFrame &frame);

		// Sends a heartbeat.
		//
		// @param tick         The sender's current tick count.
		// @param milliseconds What its last tick cost. The driver plots this
		//                     rather than timing the link, which would graph
		//                     its own poll interval.
		// @return `false` when it could not be sent.
		bool Heartbeat(uint64_t tick, float milliseconds = 0.0f);

		// Sends whatever a set of worlds posted.
		//
		// An empty list sends nothing at all rather than an empty frame: a
		// universe is quiet most ticks and a frame per quiet tick is a frame
		// per tick.
		//
		// @param traffic The envelopes to hand over.
		// @return `false` when there was something to send and it could not be.
		bool SendTraffic(std::span<const Envelope> traffic);

		// Sends what the buses answered, for a host's worlds.
		//
		// Empty sends nothing, for the same reason `SendTraffic` does.
		//
		// @param deliveries What to hand over.
		// @return `false` when there was something to send and it could not be.
		bool SendDeliveries(std::span<const HostDelivery> deliveries);

		// Takes every frame waiting.
		//
		// Drained in full rather than one per call, because the caller is a
		// barrier that runs once a tick and a backlog left behind is a backlog
		// that grows.
		//
		// @param frames Appended to. Not cleared, so a caller may accumulate.
		// @return The number of frames taken.
		size_t Receive(std::vector<HostFrame> &frames);

		// Closes this end.
		void Close();

		// How many frames were dropped because the channel refused them.
		//
		// Worth surfacing rather than swallowing: a link that is dropping
		// traffic is a universe quietly losing bus operations, which looks like
		// a game bug from every other angle.
		//
		// @return The dropped frame count.
		uint64_t Dropped() const {
			return Dropped_;
		}

		// How many frames arrived unreadable.
		//
		// @return The malformed frame count.
		uint64_t Malformed() const {
			return Malformed_;
		}

	  private:
		std::unique_ptr<parallel::Channel> Channel_;
		core::Name Name_;

		// Reused across calls so a link polled every tick stops allocating.
		std::vector<std::byte> Scratch;

		uint64_t Dropped_ = 0;
		uint64_t Malformed_ = 0;
	};
}
