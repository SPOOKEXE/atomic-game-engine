#include <engine/core/Random.hpp>
#include <engine/net/LossyTransport.hpp>

#include <algorithm>
#include <utility>

// The link that loses things, so that loss is a case rather than an argument.
//
// The whole file is deliberately small: it decides *which* arrivals survive and
// forwards everything else. Anything cleverer here would be a second transport
// to keep correct, and the point of a wrapper is that there is only one.
//
// The drain loop is the one part worth reading twice. `Receive` must not report
// `Empty` merely because the datagram it happened to pull was dropped - a caller
// polls until `Empty` and would stop early, leaving real traffic in the queue
// underneath for a tick. So it loops: take, decide, and only report what a
// caller can act on.

namespace engine::net {

	LossyTransport::LossyTransport(std::unique_ptr<Transport> beneath, const LossSettings &settings)
		: Inner(std::move(beneath)), Settings(settings) {}

	LossyTransport::~LossyTransport() {
		Close();
	}

	bool LossyTransport::Loses(uint64_t number) const {
		if (std::find(Settings.Drop.begin(), Settings.Drop.end(), number) != Settings.Drop.end()) {
			return true;
		}
		if (Settings.LossChance <= 0.0f) {
			return false;
		}

		// Indexed rather than streamed, so the answer for arrival `n` depends on
		// nothing but `n` and the seed - a case that fails at 3% loss on seed 7
		// fails again on seed 7, and reordering the cases in the file cannot move
		// it. A stateful generator would look equivalent and quietly not be.
		return core::Random::Float(static_cast<uint32_t>(number), Settings.Seed) < Settings.LossChance;
	}

	TransportStatus LossyTransport::Send(const Endpoint &to, std::span<const std::byte> datagram) {
		if (Inner == nullptr) {
			return TransportStatus::Closed;
		}
		return Inner->Send(to, datagram);
	}

	LossyTransport::Inbound LossyTransport::Receive(std::vector<std::byte> &datagram) {
		if (Inner == nullptr) {
			return {TransportStatus::Closed, {}};
		}

		while (true) {
			if (!Ready.empty()) {
				Waiting &next = Ready.front();
				datagram.assign(next.Bytes.begin(), next.Bytes.end());

				const Inbound handed{TransportStatus::Ok, next.From};
				Ready.pop_front();
				Counters.Delivered++;
				return handed;
			}

			// Straight into the caller's buffer, so a link losing nothing costs
			// no copy the transport underneath was not already making.
			const Inbound inbound = Inner->Receive(datagram);
			if (inbound.Status != TransportStatus::Ok) {
				if (Held.has_value()) {
					// Nothing came in behind it, so the reorder is a delay of one
					// poll. Releasing it here rather than holding for a datagram
					// that may never come is what stops a nominated reorder from
					// being an accidental drop.
					Ready.push_back(std::move(*Held));
					Held.reset();
					continue;
				}
				return inbound;
			}

			const uint64_t number = Counters.Arrived;
			Counters.Arrived++;

			if (Arming > 0) {
				// Armed by count rather than by number, because a test knows what
				// it just made the sender do and not which arrival that will be.
				Arming--;
				Counters.Dropped++;
				continue;
			}
			if (Loses(number)) {
				Counters.Dropped++;
				continue;
			}

			const bool reordering =
				std::find(Settings.Reorder.begin(), Settings.Reorder.end(), number) != Settings.Reorder.end();
			if (reordering && !Held.has_value()) {
				Held = Waiting{inbound.From, {datagram.begin(), datagram.end()}};
				Counters.Reordered++;
				continue;
			}

			Waiting survivor{inbound.From, {datagram.begin(), datagram.end()}};
			if (std::find(Settings.Duplicate.begin(), Settings.Duplicate.end(), number) !=
				Settings.Duplicate.end()) {
				// Back to back, which is what a resend looks like when the
				// acknowledgement for the original was the packet that got lost.
				Ready.push_back(survivor);
				Counters.Duplicated++;
			}
			Ready.push_back(std::move(survivor));

			if (Held.has_value()) {
				// Behind the one that overtook it, which is the whole point.
				Ready.push_back(std::move(*Held));
				Held.reset();
			}
		}
	}

	Endpoint LossyTransport::Local() const {
		return Inner == nullptr ? Endpoint{} : Inner->Local();
	}

	bool LossyTransport::Open() const {
		return Inner != nullptr && Inner->Open();
	}

	void LossyTransport::Close() {
		// Dropped rather than left to drain, matching what a closed transport
		// underneath does with its own queue.
		Ready.clear();
		Held.reset();
		if (Inner != nullptr) {
			Inner->Close();
		}
	}

	void LossyTransport::DropNext(size_t datagrams) {
		Arming += datagrams;
	}

	void LossyTransport::DropAt(uint64_t number) {
		Settings.Drop.push_back(number);
	}
}
