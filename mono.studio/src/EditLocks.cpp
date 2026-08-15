#include <algorithm>
#include <limits>
#include <studio/EditLocks.hpp>
#include <utility>

namespace studio {

	const char *Describe(Turn turn) {
		switch (turn) {
		case Turn::Granted:
			return "granted";
		case Turn::Queued:
			return "queued";
		case Turn::Refused:
			return "refused";
		}
		// No default label, so adding a turn is a warning here.
		return "?";
	}

	LockTable::LockTable(const LockSettings &settings) : Limits(settings) {
		if (Limits.GrantSeconds <= 0.0) {
			Limits.GrantSeconds = LockSettings{}.GrantSeconds;
		}
		if (Limits.MaximumHolds == 0) {
			Limits.MaximumHolds = LockSettings{}.MaximumHolds;
		}
		if (Limits.MaximumWaiting == 0) {
			Limits.MaximumWaiting = LockSettings{}.MaximumWaiting;
		}
	}

	bool LockTable::Busy(const InstancePath &path, EditorId holder, double nowSeconds) const {
		for (const Lease &lease : Leases) {
			if (lease.Holder == holder || nowSeconds >= lease.ExpiresAtSeconds) {
				// A grant whose guard has fired holds nobody up, and is left in
				// the table until `Expire` sweeps it - checking here as well
				// means a dead editor's turn does not stay in force between the
				// moment it lapses and the moment somebody notices.
				continue;
			}
			if (Overlaps(lease.Subject, path)) {
				return true;
			}
		}
		return false;
	}

	Turn LockTable::Request(const InstancePath &path, EditorId holder, double nowSeconds) {
		if (path.empty()) {
			// An edit that names nothing is not an edit anybody takes a turn
			// for. Whatever it is, the queue is not the thing that should order
			// it.
			return Turn::Granted;
		}

		// **Idempotent for an editor that already holds it.** An editor
		// publishing twice in a row asks twice, and the second ask has to be a
		// grant rather than a place in a queue behind itself.
		for (Lease &lease : Leases) {
			if (lease.Holder == holder && Contains(lease.Subject, path) &&
				nowSeconds < lease.ExpiresAtSeconds) {
				lease.ExpiresAtSeconds = nowSeconds + Limits.GrantSeconds;
				return Turn::Granted;
			}
		}

		if (Busy(path, holder, nowSeconds)) {
			// Already waiting for the same thing? Then they are already in
			// line, and asking again must not move them or duplicate them.
			for (const Waiting &waiting : Waiters) {
				if (waiting.Holder == holder && waiting.Subject == path) {
					return Turn::Queued;
				}
			}
			if (Waiters.size() >= Limits.MaximumWaiting) {
				return Turn::Refused;
			}
			Waiters.push_back(Waiting{path, holder});
			return Turn::Queued;
		}

		if (Leases.size() >= Limits.MaximumHolds) {
			return Turn::Refused;
		}

		Leases.push_back(Lease{path, holder, nowSeconds + Limits.GrantSeconds});
		return Turn::Granted;
	}

	std::vector<Waiting> LockTable::Wake(double nowSeconds) {
		// **In the order they asked, and that is the promise.** Whoever was
		// there first goes first, and the next edit lands on top of theirs.
		//
		// Walked repeatedly rather than once, because granting one request can
		// free nothing and granting another can free several: two waiters for
		// disjoint subtrees both become grantable the moment one lease goes.
		std::vector<Waiting> woken;
		bool moved = true;
		while (moved) {
			moved = false;
			for (size_t index = 0; index < Waiters.size(); ++index) {
				const Waiting &waiting = Waiters[index];
				if (Busy(waiting.Subject, waiting.Holder, nowSeconds)) {
					continue;
				}
				if (Leases.size() >= Limits.MaximumHolds) {
					break;
				}

				Leases.push_back(Lease{waiting.Subject, waiting.Holder, nowSeconds + Limits.GrantSeconds});
				woken.push_back(waiting);
				Waiters.erase(Waiters.begin() + static_cast<ptrdiff_t>(index));
				moved = true;
				break;
			}
		}
		return woken;
	}

	std::vector<Waiting> LockTable::Release(const InstancePath &path, EditorId holder, double nowSeconds) {
		Leases.erase(
			std::remove_if(
				Leases.begin(),
				Leases.end(),
				[&](const Lease &lease) {
					// Somebody else's turn is left alone. A release that could
					// end another editor's turn would be a queue anybody can
					// jump.
					return lease.Holder == holder && Contains(path, lease.Subject);
				}
			),
			Leases.end()
		);
		return Wake(nowSeconds);
	}

	std::vector<Waiting> LockTable::ReleaseAll(EditorId holder, double nowSeconds) {
		Leases.erase(
			std::remove_if(
				Leases.begin(), Leases.end(), [holder](const Lease &lease) { return lease.Holder == holder; }
			),
			Leases.end()
		);
		Waiters.erase(
			std::remove_if(
				Waiters.begin(),
				Waiters.end(),
				[holder](const Waiting &waiting) { return waiting.Holder == holder; }
			),
			Waiters.end()
		);
		return Wake(nowSeconds);
	}

	std::vector<Waiting> LockTable::Expire(double nowSeconds) {
		Leases.erase(
			std::remove_if(
				Leases.begin(),
				Leases.end(),
				[nowSeconds](const Lease &lease) { return nowSeconds >= lease.ExpiresAtSeconds; }
			),
			Leases.end()
		);
		return Wake(nowSeconds);
	}

	const Lease *LockTable::HolderOf(const InstancePath &path, double nowSeconds) const {
		for (const Lease &lease : Leases) {
			if (nowSeconds < lease.ExpiresAtSeconds && Overlaps(lease.Subject, path)) {
				return &lease;
			}
		}
		return nullptr;
	}

	void LockTable::Adopt(std::span<const Lease> leases) {
		Leases.assign(leases.begin(), leases.end());

		// **The guard belongs to the host's clock and means nothing here.** A
		// guest's copy is a picture, replaced whole by the next one the host
		// sends; giving it a deadline off this machine's clock would have every
		// row read as already lapsed, and the panel would show nobody holding
		// anything while the host held plenty.
		for (Lease &lease : Leases) {
			lease.ExpiresAtSeconds = std::numeric_limits<double>::infinity();
		}

		if (Leases.size() > Limits.MaximumHolds) {
			Leases.resize(Limits.MaximumHolds);
		}
	}

	void LockTable::Clear() {
		Leases.clear();
		Waiters.clear();
	}
}
