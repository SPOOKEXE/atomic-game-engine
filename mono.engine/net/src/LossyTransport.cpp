#include <engine/core/Log.hpp>
#include <engine/core/Random.hpp>
#include <engine/net/LossyTransport.hpp>

#include <algorithm>
#include <chrono>
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

	namespace {
		// Distinct draws per decision, so one arrival's loss, duplicate, reorder
		// and jitter are independent even under one seed.
		constexpr uint32_t DUPLICATE_SALT = 0x6A09E667u;
		constexpr uint32_t REORDER_SALT = 0xBB67AE85u;
		constexpr uint32_t JITTER_SALT = 0x3C6EF372u;
	}

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

	bool LossyTransport::Chance(uint64_t number, float chance, uint32_t salt) const {
		return chance > 0.0f &&
			   core::Random::Float(static_cast<uint32_t>(number), Settings.Seed ^ salt) < chance;
	}

	double LossyTransport::Clock() const {
		if (Settings.Now != nullptr) return Settings.Now();
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// Hands one survivor on, now or after its seeded wait.
	void LossyTransport::Deliver(Waiting survivor, uint64_t number) {
		if (Settings.DelaySeconds <= 0.0 && Settings.JitterSeconds <= 0.0) {
			Ready.push_back(std::move(survivor));
			return;
		}
		const double jitter =
			Settings.JitterSeconds > 0.0
				? Settings.JitterSeconds *
					  core::Random::Float(static_cast<uint32_t>(number), Settings.Seed ^ JITTER_SALT)
				: 0.0;
		Delaying.push_back(
			{Clock() + std::max(Settings.DelaySeconds, 0.0) + jitter, number, std::move(survivor)}
		);
		Counters.Delayed++;
	}

	// Moves every wait that is over to `Ready`, earliest due first. Equal due
	// times keep arrival order, so a duplicate stays behind its original.
	void LossyTransport::ReleaseDue() {
		if (Delaying.empty()) return;
		const double now = Clock();
		const auto due =
			std::stable_partition(Delaying.begin(), Delaying.end(), [now](const Pending &pending) {
				return pending.DueSeconds <= now;
			});
		std::stable_sort(Delaying.begin(), due, [](const Pending &left, const Pending &right) {
			return left.DueSeconds < right.DueSeconds;
		});
		for (auto pending = Delaying.begin(); pending != due; ++pending)
			Ready.push_back(std::move(pending->Datagram));
		Delaying.erase(Delaying.begin(), due);
	}

	LossyTransport::Inbound LossyTransport::Receive(std::vector<std::byte> &datagram) {
		if (Inner == nullptr) {
			return {TransportStatus::Closed, {}};
		}

		while (true) {
			ReleaseDue();
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
					const uint64_t heldNumber = Held->Number;
					Deliver(std::move(*Held), heldNumber);
					Held.reset();
					continue;
				}
				// Survivors still waiting are not an arrival yet, so this is
				// `Empty` even while some are pending; the next poll releases them.
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

			const bool reordering = ReorderArming > 0 ||
									std::find(Settings.Reorder.begin(), Settings.Reorder.end(), number) !=
										Settings.Reorder.end() ||
									Chance(number, Settings.ReorderChance, REORDER_SALT);
			if (ReorderArming > 0) --ReorderArming;
			if (reordering && !Held.has_value()) {
				Held = Waiting{inbound.From, {datagram.begin(), datagram.end()}, number};
				Counters.Reordered++;
				continue;
			}

			Waiting survivor{inbound.From, {datagram.begin(), datagram.end()}, number};
			const bool duplicate = DuplicateArming > 0 ||
								   std::find(Settings.Duplicate.begin(), Settings.Duplicate.end(), number) !=
									   Settings.Duplicate.end() ||
								   Chance(number, Settings.DuplicateChance, DUPLICATE_SALT);
			if (DuplicateArming > 0) --DuplicateArming;
			if (duplicate) {
				// Back to back, which is what a resend looks like when the
				// acknowledgement for the original was the packet that got lost.
				Deliver(survivor, number);
				Counters.Duplicated++;
			}
			Deliver(std::move(survivor), number);

			if (Held.has_value()) {
				// Behind the one that overtook it, which is the whole point.
				const uint64_t heldNumber = Held->Number;
				Deliver(std::move(*Held), heldNumber);
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
		// A seeded impairment reports what it did once, so an acceptance run can
		// show that its loss, duplicates, reorders and delays really happened.
		// Nominated drops stay quiet; their suites assert the counters directly.
		const bool seeded = Settings.LossChance > 0.0f || Settings.DuplicateChance > 0.0f ||
							Settings.ReorderChance > 0.0f || Settings.DelaySeconds > 0.0 ||
							Settings.JitterSeconds > 0.0;
		if (seeded && !Reported && Counters.Arrived > 0) {
			Reported = true;
			ENGINE_INFO(
				"impaired link closed: arrived={} delivered={} dropped={} duplicated={} reordered={} "
				"delayed={}",
				Counters.Arrived,
				Counters.Delivered,
				Counters.Dropped,
				Counters.Duplicated,
				Counters.Reordered,
				Counters.Delayed
			);
		}
		// Dropped rather than left to drain, matching what a closed transport
		// underneath does with its own queue.
		Ready.clear();
		Held.reset();
		Delaying.clear();
		if (Inner != nullptr) {
			Inner->Close();
		}
	}

	void LossyTransport::DropNext(size_t datagrams) {
		Arming += datagrams;
	}

	void LossyTransport::DuplicateNext(size_t datagrams) {
		DuplicateArming += datagrams;
	}

	void LossyTransport::ReorderNext(size_t datagrams) {
		ReorderArming += datagrams;
	}

	void LossyTransport::DropAt(uint64_t number) {
		Settings.Drop.push_back(number);
	}
}
