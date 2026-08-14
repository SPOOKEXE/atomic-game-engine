#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/Reliability.hpp>

#include <algorithm>
#include <cmath>

namespace engine::net {

	namespace {
		// How many sequences before Acknowledge the bitfield covers. Not a
		// choice made here: it is the width of PacketHeader::AcknowledgeBits,
		// and it is why the send window is what it is.
		constexpr uint16_t ACKNOWLEDGE_BITS = 32;
	}

	bool ReliabilitySettings::IsValid() const {
		return RetransmitTimeoutSeconds > 0.0 && MaximumUnacknowledged > 0 &&
			   MaximumUnacknowledged <= ACKNOWLEDGE_BITS && MaximumResends > 0 && MaximumOutOfOrder > 0;
	}

	ReliableSender::ReliableSender(const ReliabilitySettings &settings)
		: Paced(settings.IsValid() ? settings : ReliabilitySettings{}) {}

	bool ReliableSender::HasRoom() const {
		if (Overflowed != DisconnectReason::None) {
			return false;
		}
		if (Pending.empty()) {
			return true;
		}

		// From the oldest payload still waiting to the one about to be sent,
		// inclusive. The far side's window reaches 32 sequences back from the
		// newest it has seen, so anything wider than this leaves the oldest
		// unacknowledgeable and it is then resent until the connection ends.
		const uint16_t span = static_cast<uint16_t>(Newest + 2 - Pending.front().Sequence);
		return span <= Paced.MaximumUnacknowledged;
	}

	bool ReliableSender::Track(uint16_t sequence, std::span<const std::byte> payload, double nowSeconds) {
		if (!HasRoom()) {
			return false;
		}

		Pending.push_back(Held{sequence, 0, nowSeconds, {payload.begin(), payload.end()}});
		Newest = sequence;
		return true;
	}

	size_t ReliableSender::OnAcknowledge(const PacketHeader &header, double nowSeconds) {
		ENGINE_PROFILE_CAT("ReliableSender::OnAcknowledge", core::ProfileCategory::Network);

		// **Karn's rule, and it is the whole reason `Attempts` is consulted
		// here.** An acknowledgement of a resent packet does not say which
		// transmission it answers, so a sample from one is either the true trip
		// or the trip plus a retransmit timeout - and there is no way to tell.
		// Measuring them makes the estimate worst on exactly the links that
		// need it most.
		const auto sample = [this, nowSeconds](const Held &entry) {
			// **Zero, because `Attempts` counts *re*sends and not sends.**
			// `Track` starts it at zero and `OnResent` increments it, so a
			// packet that went out once and was never repeated has zero - and a
			// check for one measures precisely the packets Karn's rule exists
			// to exclude, which is the inversion this first shipped as.
			if (entry.Attempts != 0) {
				return;
			}

			const double trip = nowSeconds - entry.SentAtSeconds;
			if (trip < 0.0 || !std::isfinite(trip)) {
				// A clock that went backwards, or a caller passing nonsense.
				// Dropped rather than folded in: one bad sample survives in a
				// smoothed value for dozens of good ones.
				return;
			}

			// RFC 6298's weights, and the variance is not optional company for
			// the mean. **A congestion controller reading the smoothed value
			// alone cannot tell a path with fifteen milliseconds of queue on it
			// from a wireless link whose trip swings by fifteen milliseconds**,
			// and one of those is a reason to back off while the other is a
			// reason to do nothing.
			constexpr double MEAN_WEIGHT = 0.125;
			constexpr double VARIANCE_WEIGHT = 0.25;

			if (SmoothedRoundTrip <= 0.0) {
				// The first sample is taken whole, because smoothing towards
				// zero would make the estimate say "instant" for the first
				// several round trips of every connection. RFC 6298 seeds the
				// variance at half the first sample for the same reason: zero
				// would claim a certainty one sample cannot support.
				SmoothedRoundTrip = trip;
				RoundTripVariance = trip * 0.5;
				return;
			}

			// Against the estimate *before* it moves, which is the order RFC
			// 6298 states and is not interchangeable: updating the mean first
			// measures the deviation against a value that has already absorbed
			// part of the sample, and the variance then reads low on exactly
			// the samples that should widen it.
			const double deviation = std::abs(SmoothedRoundTrip - trip);
			RoundTripVariance = RoundTripVariance * (1.0 - VARIANCE_WEIGHT) + deviation * VARIANCE_WEIGHT;
			SmoothedRoundTrip = SmoothedRoundTrip * (1.0 - MEAN_WEIGHT) + trip * MEAN_WEIGHT;
		};

		const size_t before = Pending.size();
		std::erase_if(Pending, [&header, &sample](const Held &entry) {
			const auto retire = [&sample, &entry]() {
				sample(entry);
				return true;
			};
			if (entry.Sequence == header.Acknowledge) {
				return retire();
			}

			// Wrap-aware, and not optional: at 16 bits a plain comparison reads
			// everything sent before the wrap as newer than the acknowledgement
			// that clears it, and nothing is ever retired again.
			if (Packet::IsNewer(entry.Sequence, header.Acknowledge)) {
				return false;
			}

			const uint16_t behind = static_cast<uint16_t>(header.Acknowledge - entry.Sequence);
			if (behind > ACKNOWLEDGE_BITS) {
				return false;
			}
			if ((header.AcknowledgeBits & (1u << (behind - 1))) == 0) {
				return false;
			}
			return retire();
		});

		return before - Pending.size();
	}

	std::span<const ReliableSender::Unacknowledged> ReliableSender::Due(double nowSeconds) {
		Ready.clear();
		if (Overflowed != DisconnectReason::None) {
			return Ready;
		}

		for (const Held &entry : Pending) {
			if (nowSeconds - entry.SentAtSeconds >= Paced.RetransmitTimeoutSeconds) {
				Ready.push_back(Unacknowledged{entry.Sequence, entry.Payload});
			}
		}

		return Ready;
	}

	bool ReliableSender::OnResent(uint16_t sequence, double nowSeconds) {
		const auto at = std::find_if(Pending.begin(), Pending.end(), [sequence](const Held &entry) {
			return entry.Sequence == sequence;
		});
		if (at == Pending.end()) {
			return false;
		}

		at->SentAtSeconds = nowSeconds;
		++at->Attempts;
		++Resends;
		core::Metrics::Count("net.reliability.resend", 1.0);

		if (at->Attempts >= Paced.MaximumResends) {
			// Every one of those attempts was inside the far side's
			// acknowledgement window. A peer that answered none of them is
			// gone, whatever else is still arriving from it.
			Overflowed = DisconnectReason::TimedOut;
			core::Metrics::Count("net.reliability.overflow", 1.0);
		}
		return true;
	}

	ReliableReceiver::ReliableReceiver(const ReliabilitySettings &settings, uint16_t firstSequence)
		: Paced(settings.IsValid() ? settings : ReliabilitySettings{}), NextSequence(firstSequence),
		  Highest(static_cast<uint16_t>(firstSequence - 1)) {}

	PacketHeader ReliableReceiver::Acknowledging(PacketHeader header) const {
		header.Acknowledge = Highest;
		header.AcknowledgeBits = Bits;
		return header;
	}

	void ReliableReceiver::Record(uint16_t sequence) {
		if (Packet::IsNewer(sequence, Highest)) {
			const uint16_t shift = static_cast<uint16_t>(sequence - Highest);

			// A jump wider than the window leaves nothing the window can still
			// describe, so it is cleared rather than shifted into nonsense.
			Bits = shift >= ACKNOWLEDGE_BITS ? 0u : ((Bits << shift) | (1u << (shift - 1)));
			Highest = sequence;
			return;
		}

		const uint16_t behind = static_cast<uint16_t>(Highest - sequence);
		if (behind >= 1 && behind <= ACKNOWLEDGE_BITS) {
			Bits |= 1u << (behind - 1);
		}
	}

	bool ReliableReceiver::Accept(uint16_t sequence, std::span<const std::byte> payload) {
		if (Overflowed != DisconnectReason::None) {
			return false;
		}

		// Before anything else, and whatever happens below. A duplicate is a
		// resend, and the far side is resending precisely because it has not
		// heard that the original arrived.
		Record(sequence);

		if (sequence == NextSequence) {
			// The one the stream is waiting on. Taken whatever the bound says:
			// refusing it would refuse the packet that empties the queue the
			// bound is about.
			Held.emplace(sequence, std::vector<std::byte>(payload.begin(), payload.end()));
			return true;
		}

		if (!Packet::IsNewer(sequence, NextSequence)) {
			// Already delivered. A resend whose original arrived, which is what
			// a lost acknowledgement looks like from this end.
			++Repeats;
			return false;
		}

		if (Held.find(sequence) != Held.end()) {
			++Repeats;
			return false;
		}

		if (Held.size() >= Paced.MaximumOutOfOrder) {
			// A peer respecting its own send window cannot get here, so one
			// that does is spending this side's memory on its own behalf
			// without ever exceeding a per-tick budget.
			Overflowed = DisconnectReason::BudgetExceeded;
			core::Metrics::Count("net.reliability.overflow", 1.0);
			return false;
		}

		Held.emplace(sequence, std::vector<std::byte>(payload.begin(), payload.end()));
		return true;
	}

	std::span<const ReliableReceiver::Delivery> ReliableReceiver::Drain() {
		Ready.clear();

		// NextSequence is a uint16_t and wraps with the stream, so the run
		// spanning 65535 to 0 is the same loop as any other.
		for (auto at = Held.find(NextSequence); at != Held.end(); at = Held.find(NextSequence)) {
			Ready.push_back(Delivery{NextSequence, std::move(at->second)});
			Held.erase(at);
			++NextSequence;
		}

		return Ready;
	}
}
