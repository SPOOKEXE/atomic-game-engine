#include "SequencePlayback.hpp"

#include <engine/render/Flipbook.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::render {
	uint64_t SequencePlayback::Key(core::Name name, core::Name owner) {
		return (uint64_t(owner.Id()) << 32) | name.Id();
	}

	size_t SequencePlayback::BytesOf(const Sequence &sequence) {
		return sequence.Data.Pixels.size() + sequence.Data.FrameDurations.size() * sizeof(float) +
			   sequence.CumulativeEnds.size() * sizeof(float);
	}

	bool SequencePlayback::Admit(core::Name name, core::Name owner, assets::TextureSequenceData sequence) {
		if (!name.IsValid() || !owner.IsValid() || !sequence.IsValid() ||
			sequence.FrameDurations.size() <= 256)
			return false;
		Sequence candidate;
		candidate.Data = std::move(sequence);
		candidate.CumulativeEnds.reserve(candidate.Data.FrameDurations.size());
		float end = 0.0f;
		for (float duration : candidate.Data.FrameDurations) {
			end += duration;
			candidate.CumulativeEnds.push_back(end);
		}
		const uint64_t key = Key(name, owner);
		auto found = Entries.find(key);
		if (found == Entries.end() && Entries.size() >= MAXIMUM_ENTRIES) return false;
		const size_t replaced =
			found == Entries.end() || !found->second.Pending ? 0 : BytesOf(*found->second.Pending);
		const size_t bytes = BytesOf(candidate);
		if (HeldBytes < replaced || HeldBytes - replaced > MaximumBytes ||
			bytes > MaximumBytes - (HeldBytes - replaced))
			return false;
		if (found == Entries.end()) {
			Entry entry;
			entry.Name = name;
			entry.Owner = owner;
			found = Entries.emplace(key, std::move(entry)).first;
		}
		found->second.Pending = std::move(candidate);
		HeldBytes = HeldBytes - replaced + bytes;
		return true;
	}

	size_t SequencePlayback::Advance(double seconds, size_t byteBudget, const Publish &publish) {
		if (Entries.empty() || !publish || !std::isfinite(seconds)) return 0;
		byteBudget = std::min(byteBudget, FRAME_UPLOAD_BUDGET);
		auto current = Entries.upper_bound(LastAttempted);
		if (current == Entries.end()) current = Entries.begin();
		size_t published = 0;
		for (size_t checked = 0; checked < Entries.size(); ++checked) {
			const uint64_t key = current->first;
			Entry &entry = current->second;
			Sequence *candidate = entry.Pending ? &*entry.Pending : &*entry.Active;
			const uint32_t frame = FlipbookFrameAt(candidate->CumulativeEnds, seconds);
			const size_t frameBytes = uint64_t(candidate->Data.Width) * candidate->Data.Height * 4;
			if ((!candidate->Published || candidate->PublishedFrame != frame) && frameBytes <= byteBudget) {
				LastAttempted = key;
				byteBudget -= frameBytes;
				if (publish(entry.Name, entry.Owner, candidate->Data, frame)) {
					candidate->Published = true;
					candidate->PublishedFrame = frame;
					if (entry.Pending) {
						if (entry.Active) HeldBytes -= BytesOf(*entry.Active);
						entry.Active = std::move(entry.Pending);
						entry.Pending.reset();
					}
					++published;
				}
			}
			++current;
			if (current == Entries.end()) current = Entries.begin();
		}
		return published;
	}

	bool SequencePlayback::Drop(core::Name name, core::Name owner) {
		const auto found = Entries.find(Key(name, owner));
		if (found == Entries.end()) return false;
		if (found->second.Active) HeldBytes -= BytesOf(*found->second.Active);
		if (found->second.Pending) HeldBytes -= BytesOf(*found->second.Pending);
		Entries.erase(found);
		return true;
	}

	size_t SequencePlayback::DropOwner(core::Name owner) {
		if (!owner.IsValid()) return 0;
		size_t removed = 0;
		for (auto item = Entries.begin(); item != Entries.end();) {
			if (item->second.Owner != owner) {
				++item;
				continue;
			}
			if (item->second.Active) HeldBytes -= BytesOf(*item->second.Active);
			if (item->second.Pending) HeldBytes -= BytesOf(*item->second.Pending);
			item = Entries.erase(item);
			++removed;
		}
		return removed;
	}

	bool SequencePlayback::Contains(core::Name name, core::Name owner) const {
		return Entries.contains(Key(name, owner));
	}

	uint64_t SequencePlayback::PublishedSignature() const {
		uint64_t signature = 0;
		for (const auto &[key, entry] : Entries) {
			if (!entry.Active || !entry.Active->Published) continue;
			signature ^= (key * 0x9E3779B97F4A7C15ull) ^ entry.Active->PublishedFrame;
		}
		return signature;
	}
}
