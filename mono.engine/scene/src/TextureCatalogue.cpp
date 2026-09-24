#include <engine/ecs/Store.hpp>
#include <engine/scene/TextureCatalogue.hpp>

#include <cmath>

namespace engine::scene {

	FlipbookFacts TextureCatalogue::Find(const core::Name &texture) const {
		if (!texture.IsValid()) {
			return {};
		}
		const auto found = Flipbooks.find(texture.Id());
		return found == Flipbooks.end() ? FlipbookFacts{} : found->second;
	}

	TextureCatalogue &TexturesOf(ecs::Store &store) {
		if (!store.HasResource<TextureCatalogue>()) {
			store.SetResource(TextureCatalogue{});
		}
		return *store.ResourceMutable<TextureCatalogue>();
	}

	bool RecordTexture(ecs::Store &store, const core::Name &texture, const FlipbookFacts &facts) {
		if (!texture.IsValid()) {
			return false;
		}
		FlipbookFacts recorded = facts;
		recorded.CumulativeEnds.clear();
		recorded.TotalDuration = 0.0f;
		const bool sequence = recorded.Side == 0 && recorded.Frames > 256 && recorded.Frames <= 4096;
		if (recorded.Side == 0 && recorded.Frames != 0 && !sequence) return false;
		if (!recorded.FrameDurations.empty()) {
			if (recorded.FrameDurations.size() != recorded.Frames || recorded.FrameRate != 0.0f ||
				(!sequence && (recorded.Frames > 256 || recorded.Side == 0 ||
							   recorded.Frames > static_cast<uint32_t>(recorded.Side) * recorded.Side))) {
				return false;
			}
			recorded.CumulativeEnds.reserve(recorded.Frames);
			for (float duration : recorded.FrameDurations) {
				const float next = recorded.TotalDuration + duration;
				if (!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(next) ||
					next <= recorded.TotalDuration) {
					return false;
				}
				recorded.CumulativeEnds.push_back(next);
				recorded.TotalDuration = next;
			}
		}
		if (sequence && recorded.FrameDurations.empty()) return false;

		// A still image is stored rather than rejected, for `RecordMesh`'s
		// reason: it reads back identically to "not known", both mean the same
		// thing to a caller - there is nothing here to play - and a separate
		// "known to be a still" state would be a distinction nothing can act on.
		TextureCatalogue &catalogue = TexturesOf(store);
		catalogue.Flipbooks[texture.Id()] = std::move(recorded);
		catalogue.Revision++;
		return true;
	}

	FlipbookFacts FlipbookOf(const ecs::Store &store, const core::Name &texture) {
		// **Never creates the resource**, unlike `TexturesOf`. This is what a
		// system's refresh pass calls, and a read that mutated the world would
		// put a structural change inside iteration.
		const TextureCatalogue *catalogue = store.Resource<TextureCatalogue>();
		return catalogue == nullptr ? FlipbookFacts{} : catalogue->Find(texture);
	}
}
