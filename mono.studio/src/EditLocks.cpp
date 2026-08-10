#include <algorithm>
#include <studio/EditLocks.hpp>
#include <utility>

namespace studio {

	LockTable::LockTable(const LockSettings &settings) : Limits(settings) {
		if (Limits.HoldSeconds <= 0.0) {
			Limits.HoldSeconds = LockSettings{}.HoldSeconds;
		}
		if (Limits.MaximumHolds == 0) {
			Limits.MaximumHolds = LockSettings{}.MaximumHolds;
		}
	}

	std::optional<Blocked>
	LockTable::Blocking(const InstancePath &path, EditorId holder, double nowSeconds) const {
		if (path.empty()) {
			// An edit that names nothing is not an edit anybody can hold
			// against. Whatever it is, a lock is not the thing that should stop
			// it.
			return std::nullopt;
		}

		for (const Lease &lease : Leases) {
			if (lease.Holder == holder || nowSeconds >= lease.ExpiresAtSeconds) {
				// A lapsed hold blocks nobody, and is left in the table until
				// `Expire` sweeps it — checking here as well means a hold does
				// not stay in force between the moment it lapses and the moment
				// somebody notices.
				continue;
			}
			if (Overlaps(lease.Subject, path)) {
				return Blocked{lease.Subject, lease.Holder};
			}
		}
		return std::nullopt;
	}

	bool LockTable::Hold(const InstancePath &path, EditorId holder, double nowSeconds, bool claimed) {
		if (path.empty()) {
			return false;
		}
		if (Blocking(path, holder, nowSeconds).has_value()) {
			return false;
		}

		for (Lease &lease : Leases) {
			if (lease.Holder != holder) {
				continue;
			}
			// **Renewed when it covers what is being edited, not only when it
			// is the same path.** Somebody holding a model and then dragging a
			// part inside it is still working on the model, and a table that
			// took a second hold for the child would fill with one lease per
			// part they touched.
			if (Contains(lease.Subject, path)) {
				lease.ExpiresAtSeconds = nowSeconds + Limits.HoldSeconds;
				lease.Claimed = lease.Claimed || claimed;
				return true;
			}
		}

		// A hold this editor already has that sits *underneath* the new one is
		// replaced by it, rather than left to expire beside it: editing a model
		// after editing one of its parts is one interaction and should be one
		// lease.
		Leases.erase(
			std::remove_if(
				Leases.begin(),
				Leases.end(),
				[&](const Lease &lease) { return lease.Holder == holder && Contains(path, lease.Subject); }
			),
			Leases.end()
		);

		if (Leases.size() >= Limits.MaximumHolds) {
			// The new one is refused and the existing ones stand, which is the
			// way round `network::Directory` bounds its table and for the same
			// reason: a bound that lets a flood push out what somebody is
			// working on is not a bound.
			return false;
		}

		Leases.push_back(Lease{path, holder, nowSeconds + Limits.HoldSeconds, claimed});
		return true;
	}

	bool LockTable::Release(const InstancePath &path, EditorId holder) {
		const size_t before = Leases.size();
		Leases.erase(
			std::remove_if(
				Leases.begin(),
				Leases.end(),
				[&](const Lease &lease) {
					// Somebody else's hold is left alone. A release that could
					// take another editor's lock would be a lock anybody can
					// pick.
					return lease.Holder == holder && Contains(path, lease.Subject);
				}
			),
			Leases.end()
		);
		return Leases.size() != before;
	}

	size_t LockTable::ReleaseAll(EditorId holder) {
		const size_t before = Leases.size();
		Leases.erase(
			std::remove_if(
				Leases.begin(), Leases.end(), [holder](const Lease &lease) { return lease.Holder == holder; }
			),
			Leases.end()
		);
		return before - Leases.size();
	}

	size_t LockTable::Expire(double nowSeconds) {
		const size_t before = Leases.size();
		Leases.erase(
			std::remove_if(
				Leases.begin(),
				Leases.end(),
				[nowSeconds](const Lease &lease) { return nowSeconds >= lease.ExpiresAtSeconds; }
			),
			Leases.end()
		);
		return before - Leases.size();
	}

	void LockTable::Adopt(std::span<const Lease> leases) {
		Leases.assign(leases.begin(), leases.end());
		if (Leases.size() > Limits.MaximumHolds) {
			Leases.resize(Limits.MaximumHolds);
		}
	}

	const Lease *LockTable::HolderOf(const InstancePath &path) const {
		for (const Lease &lease : Leases) {
			if (Overlaps(lease.Subject, path)) {
				return &lease;
			}
		}
		return nullptr;
	}

	void LockTable::Clear() {
		Leases.clear();
	}
}
