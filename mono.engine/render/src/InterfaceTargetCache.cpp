#include <engine/render/InterfaceTargetCache.hpp>

#include <algorithm>
#include <limits>

namespace engine::render {

	InterfaceTargetCache::InterfaceTargetCache(InterfaceTargetLimits limits) : Limits(limits) {}

	InterfaceTargetCache::~InterfaceTargetCache() {
		Clear();
	}

	size_t InterfaceTargetCache::KeyHash::operator()(const InterfaceTargetKey &key) const noexcept {
		uint64_t value = key.Collector.Id;
		value ^= key.Viewer + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
		value ^= (uint64_t{key.Width} << 32) | key.Height;
		value ^= key.FirstCommand + 0x517cc1b727220a95ull + (value << 6) + (value >> 2);
		value ^= key.CommandCount + 0x94d049bb133111ebull + (value << 6) + (value >> 2);
		value ^= key.Spatial ? 0xd6e8feb86659fd93ull : 0;
		return std::hash<uint64_t>{}(value);
	}

	InterfaceTargetCache::Entry *InterfaceTargetCache::Find(const InterfaceTargetKey &key) {
		auto found = Targets.find(key);
		return found != Targets.end() ? &found->second : nullptr;
	}

	const InterfaceTargetCache::Entry *InterfaceTargetCache::Find(const InterfaceTargetKey &key) const {
		const auto found = Targets.find(key);
		return found != Targets.end() ? &found->second : nullptr;
	}

	InterfaceTargetCache::Entry *InterfaceTargetCache::EnsureEntry(const InterfaceTargetKey &key) {
		if (Entry *existing = Find(key); existing != nullptr) return existing;
		if (Limits.Count == 0) return nullptr;
		while (Targets.size() >= Limits.Count) {
			auto victim =
				std::min_element(Targets.begin(), Targets.end(), [](const auto &left, const auto &right) {
					return left.second.LastUse < right.second.LastUse;
				});
			Release(victim);
		}
		return &Targets.emplace(key, Entry{}).first->second;
	}

	InterfaceTargetPlan InterfaceTargetCache::Begin(
		const InterfaceTargetKey &key,
		uint64_t signature,
		bool damageValid,
		std::span<const gui::Compiled::DamageRegion> damage
	) {
		Entry *found = EnsureEntry(key);
		if (found == nullptr) return {InterfaceTargetWork::Full, {}};
		Entry &entry = *found;
		entry.LastUse = ++Use;
		if (entry.PendingValid) {
			if (entry.Pending == signature) return entry.PendingPlan;
			// A newer compile subsumes work whose pixels never became visible.
			entry.PendingValid = false;
		}
		if (entry.Ready && entry.Baseline == signature) {
			return {InterfaceTargetWork::Skip, {}};
		}

		InterfaceTargetPlan plan;
		if (entry.Ready && damageValid) {
			for (const gui::Compiled::DamageRegion &region : damage) {
				if (region.Collector == key.Collector && region.Spatial == key.Spatial) {
					plan.Damage.push_back(region.Bounds);
				}
			}
			if (plan.Damage.empty()) {
				// Compile reports only collectors whose source changed. This range
				// therefore owns the new global compile stamp without new pixels.
				entry.Baseline = signature;
				return {InterfaceTargetWork::Skip, {}};
			}
		}
		plan.Work = plan.Damage.empty() ? InterfaceTargetWork::Full : InterfaceTargetWork::Partial;
		entry.Pending = signature;
		entry.PendingPlan = plan;
		entry.PendingValid = true;
		return plan;
	}

	bool InterfaceTargetCache::MakeRoom(const InterfaceTargetKey &keep, uint64_t bytes) {
		if (Limits.Count == 0 || bytes > Limits.Bytes) return false;
		const auto residentCount = [&] {
			return static_cast<size_t>(std::count_if(Targets.begin(), Targets.end(), [](const auto &entry) {
				return entry.second.Target != nullptr;
			}));
		};
		const auto fits = [&] { return residentCount() < Limits.Count && Bytes <= Limits.Bytes - bytes; };
		while (!fits()) {
			auto victim = Targets.end();
			for (auto candidate = Targets.begin(); candidate != Targets.end(); ++candidate) {
				if (candidate->first == keep || candidate->second.Target == nullptr) continue;
				if (victim == Targets.end() || candidate->second.LastUse < victim->second.LastUse)
					victim = candidate;
			}
			if (victim == Targets.end()) return false;
			Release(victim);
		}
		return true;
	}

	void InterfaceTargetCache::Release(Entries::iterator entry) {
		if (entry->second.Target != nullptr && entry->second.Release)
			entry->second.Release(entry->second.Target);
		Bytes -= entry->second.Bytes;
		Targets.erase(entry);
	}

	bool InterfaceTargetCache::Attach(
		const InterfaceTargetKey &key, void *target, uint64_t bytes, std::function<void(void *)> release
	) {
		if (target == nullptr || bytes == 0) return false;
		if (const Entry *existing = Find(key); existing != nullptr && existing->Target == target) return true;
		Entry *existing = Find(key);
		void *oldTarget = nullptr;
		uint64_t oldBytes = 0;
		std::function<void(void *)> oldRelease;
		if (existing != nullptr && existing->Target != nullptr) {
			oldTarget = existing->Target;
			oldBytes = existing->Bytes;
			oldRelease = std::move(existing->Release);
			Bytes -= oldBytes;
			existing->Target = nullptr;
			existing->Bytes = 0;
		}
		if (!MakeRoom(key, bytes)) {
			if (existing != nullptr) {
				existing->Target = oldTarget;
				existing->Bytes = oldBytes;
				existing->Release = std::move(oldRelease);
				Bytes += oldBytes;
			}
			if (release) release(target);
			return false;
		}
		Entry *found = EnsureEntry(key);
		if (found == nullptr) {
			if (release) release(target);
			return false;
		}
		Entry &entry = *found;
		if (oldTarget != nullptr && oldRelease) oldRelease(oldTarget);
		entry.Target = target;
		entry.Bytes = bytes;
		entry.Release = std::move(release);
		entry.LastUse = ++Use;
		Bytes += bytes;
		return true;
	}

	void InterfaceTargetCache::Complete(const InterfaceTargetKey &key, bool succeeded) {
		Entry *entry = Find(key);
		if (entry == nullptr || !entry->PendingValid) return;
		if (succeeded && entry->Target != nullptr) {
			entry->Baseline = entry->Pending;
			entry->Ready = true;
			entry->PendingValid = false;
		}
	}

	std::vector<InterfaceTargetComposite> InterfaceTargetCache::Composite(
		const gui::DrawList &list, uint64_t viewer, uint32_t width, uint32_t height
	) const {
		std::vector<InterfaceTargetComposite> output;
		output.reserve(list.CollectorRanges.size());
		for (const gui::CollectorRange &range : list.CollectorRanges) {
			const InterfaceTargetKey key{
				range.Collector, viewer, width, height, range.First, range.Count, range.Spatial
			};
			const Entry *entry = Find(key);
			if (entry != nullptr && entry->Ready && entry->Target != nullptr) {
				output.push_back({entry->Target, range});
			}
		}
		return output;
	}

	void *InterfaceTargetCache::Target(const InterfaceTargetKey &key) const {
		const Entry *entry = Find(key);
		return entry != nullptr ? entry->Target : nullptr;
	}

	void InterfaceTargetCache::Clear() {
		while (!Targets.empty())
			Release(Targets.begin());
	}

	size_t InterfaceTargetCache::TargetCount() const {
		return std::count_if(Targets.begin(), Targets.end(), [](const auto &entry) {
			return entry.second.Target != nullptr;
		});
	}

	uint64_t InterfaceTargetCache::TargetBytes() const {
		return Bytes;
	}
}
