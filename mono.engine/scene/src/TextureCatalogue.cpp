#include <engine/ecs/Store.hpp>
#include <engine/scene/TextureCatalogue.hpp>

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

		// A still image is stored rather than rejected, for `RecordMesh`'s
		// reason: it reads back identically to "not known", both mean the same
		// thing to a caller — there is nothing here to play — and a separate
		// "known to be a still" state would be a distinction nothing can act on.
		TexturesOf(store).Flipbooks[texture.Id()] = facts;
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
