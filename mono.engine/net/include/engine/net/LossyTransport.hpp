#pragma once

// A link that loses datagrams, so that losing one is something a suite states.
//
// Every transport in this module either delivers or refuses **here and now**:
// the loopback routes into a queue it owns and the socket hands the bytes to a
// kernel, and both answer `Ok` for anything they let go of. The third outcome -
// a datagram that left, was accepted, and never arrived - is the one the whole
// design above this layer is built around, and nothing in the tree had ever
// produced it. This produces it, under the caller's control.
//
// **A wrapper, not a third implementation of the interface.** It holds another
// `Transport` and answers every call by delegating, so what it loses is real
// datagrams carrying real framing over the loopback or a real socket. A third
// implementation would be a third set of bugs, and the two paths only a routed
// network produces - a sender this end has never heard of, and an address
// nobody is listening on - would have to be built a second time to get them.
//
// **Loss is applied on the way in, not on the way out, and that is what keeps
// `Send` honest.** `Transport::Send` promises `Ok` means the datagram left and
// distinguishes that from `Full`, `TooLarge`, `Unreachable` and `Closed`, which
// are the sender's own business; a wrapper that decided to lose a datagram
// before offering it would have to invent one of those statuses or hide one the
// transport underneath would have given. So `Send` is pure delegation and the
// datagram is discarded in flight, which is also where a network discards it.
// **Wrap the end that receives.** To lose traffic in the other direction, wrap
// the other end.
//
// **Deterministic, and that is this module's whole discipline.** No clock, no
// `std::random_device`, no unordered iteration. A datagram is numbered by the
// order it arrived and whether it is lost is a pure function of that number and
// a seed the caller states - `core::Random` is indexed rather than streamed for
// exactly this reason. A failing case is reproducible from its seed alone, and
// nothing here can reach a recorded run.
//
// **Nominating one datagram is worth more than a percentage.** "The third
// datagram never arrived" is a test; "ten percent loss" is a flake with a
// plausible story attached. Both are offered and the first is the one to reach
// for - `LossSettings::Drop` when the number is known in advance, `DropNext`
// when the test knows what it just made the server send and not which arrival
// that will be.
//
// @tier L11 · shared

#include <engine/net/Transport.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace engine::net {

	// Which arriving datagrams a lossy link mistreats, and how.
	//
	// Every number counts arrivals at this end from zero, so a nomination is a
	// fact about the run rather than about the wall clock.
	//
	// @since v0.5
	struct LossSettings {
		// Arrival numbers to discard. The sender is never told.
		std::vector<uint64_t> Drop;

		// Arrival numbers to deliver twice, back to back.
		//
		// A duplicate is what a resend looks like when the acknowledgement for
		// the original was the packet that got lost, and it is the case a
		// receiver must not act on twice.
		std::vector<uint64_t> Duplicate;

		// Arrival numbers to hold back until one more datagram has been taken,
		// so that the two are delivered in the other order.
		//
		// A held datagram is released as soon as the transport underneath has
		// nothing more waiting, so a nomination with nothing behind it is a
		// delay of one poll rather than a drop.
		std::vector<uint64_t> Reorder;

		// The chance that any one arrival is dropped, in `[0, 1)`.
		//
		// **Reach for `Drop` first.** This exists for the case where the point
		// is that the link is bad rather than that one particular datagram went
		// missing, and a suite that uses it is only reproducible because the
		// answer comes from `Seed` and the arrival number and from nothing else.
		// Zero, the default, loses nothing.
		float LossChance = 0.0f;

		// The seed the chance is drawn against.
		//
		// Two runs with the same seed lose the same datagrams, so a failure is
		// reported as a seed and reproduced from it.
		uint32_t Seed = 0;
	};

	// What a lossy link did to the traffic that reached it.
	//
	// @since v0.5
	struct LossStatistics {
		// Datagrams taken from the transport underneath.
		uint64_t Arrived = 0;

		// Datagrams handed to the caller, counting a duplicate twice.
		uint64_t Delivered = 0;

		// Datagrams discarded in flight.
		//
		// **Assert this moved.** A case that meant to lose something and lost
		// nothing passes for the wrong reason, which is the failure the loss
		// counters in this repository exist to make visible.
		uint64_t Dropped = 0;

		// Datagrams delivered a second time.
		uint64_t Duplicated = 0;

		// Datagrams held back and delivered behind a later one.
		uint64_t Reordered = 0;
	};

	// A transport that discards some of what arrives at it.
	//
	// Owns the transport underneath, because a lossy link that borrowed one
	// would let a caller keep a reference that bypasses the loss - and the
	// bypass would be invisible at the call site that mattered.
	//
	// @since v0.5
	class LossyTransport final : public Transport {
	  public:
		// Wraps a transport so that some of what arrives at it is lost.
		//
		// @param beneath The real link. Takes ownership; a null one makes every
		//        call answer `Closed`, which is what a caller with no socket
		//        already has to handle.
		// @param settings Which arrivals to mistreat. The default loses
		//        nothing, so a wrapper with no settings is the link underneath.
		explicit LossyTransport(std::unique_ptr<Transport> beneath, const LossSettings &settings = {});

		~LossyTransport() override;

		// Sends one datagram, losing nothing.
		//
		// **Pure delegation, deliberately** - see the note at the top of this
		// file on why loss belongs on the receiving end.
		//
		// @param to The destination.
		// @param datagram The bytes to send.
		// @return Whatever the transport underneath answered.
		TransportStatus Send(const Endpoint &to, std::span<const std::byte> datagram) override;

		// Takes the next datagram that survived.
		//
		// Drains the transport underneath past anything dropped, so a caller
		// polling until `Empty` sees exactly the surviving traffic and never
		// has to know that something was lost.
		//
		// @param[out] datagram Filled with the bytes. Untouched unless the
		//        status is `Ok`.
		// @return `Ok` and the sender, `Empty`, or `Closed`.
		Inbound Receive(std::vector<std::byte> &datagram) override;

		// The address this end receives on.
		//
		// @return The local endpoint of the transport underneath.
		Endpoint Local() const override;

		// Whether this end can still be used.
		//
		// @return `false` once closed, and for a null transport underneath.
		bool Open() const override;

		// Closes this end, dropping anything held back for reordering.
		void Close() override;

		// Loses the next `datagrams` arrivals, whatever their numbers turn out
		// to be.
		//
		// **The form most cases want.** A test knows it has just made the
		// server publish a creation; it does not know that the creation will be
		// the ninth thing to reach the client, and a case that had to work that
		// out would break the moment the join spent one more chunk.
		//
		// @param datagrams How many of the next arrivals to discard. Adds to
		//        whatever is already armed.
		void DropNext(size_t datagrams);

		// Loses the arrival with this number.
		//
		// The same nomination `LossSettings::Drop` carries, for a case that
		// learns the number after the link is built.
		//
		// @param number The arrival to discard, counting from zero.
		void DropAt(uint64_t number);

		// Datagrams taken from the transport underneath so far.
		//
		// The number the next arrival will have.
		//
		// @return The count.
		uint64_t Arrived() const {
			return Counters.Arrived;
		}

		// What this link did to the traffic that reached it.
		//
		// @return The statistics.
		const LossStatistics &Stats() const {
			return Counters;
		}

		// The transport underneath.
		//
		// For the calls this does not forward - a concrete implementation's own
		// accessors. Sending through it bypasses nothing, since loss is applied
		// on arrival.
		//
		// @return The wrapped transport, or null if there never was one.
		Transport *Beneath() const {
			return Inner.get();
		}

	  private:
		// One datagram waiting to be handed over, and who sent it.
		struct Waiting {
			Endpoint From;
			std::vector<std::byte> Bytes;
		};

		bool Loses(uint64_t number) const;

		std::unique_ptr<Transport> Inner;
		LossSettings Settings;
		LossStatistics Counters;

		// Arrivals to drop whatever their number, from `DropNext`. Counted down
		// rather than turned into numbers at the call, because the number the
		// next arrival will have is not the number it will still have once
		// something before it is dropped.
		size_t Arming = 0;

		// Survivors in the order they go out, which is not the order they
		// arrived once anything is duplicated or reordered.
		std::deque<Waiting> Ready;

		// The one datagram being held back behind the next.
		std::optional<Waiting> Held;
	};
}
