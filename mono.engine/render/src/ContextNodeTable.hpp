#pragma once

#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <array>
#include <stdexcept>

namespace engine::render {
	// Immutable member descriptors borrow the current recording only when invoked.
	// A bounded process-local index selects a kind; its registration chain keeps
	// family precedence without scanning unrelated kinds or storing live contexts.
	template <class Recording> class ContextNodeTable {
	  public:
		using Handler = bool (Recording::*)(const graph::RunContext &);

		struct Resolution {
			Handler Run = nullptr;
			bool Known = false;
		};

		void SelectFamily(uint16_t family) {
			Family = family;
		}

		bool Set(core::Name kind, Handler handler) {
			if (!kind.IsValid() || handler == nullptr) return false;
			if (Count == Entries.size()) throw std::length_error("render node descriptor capacity exceeded");
			const size_t bucket = BucketFor(kind.Id());
			Entries[Count] = {kind.Id(), handler, Family, Heads[bucket]};
			Heads[bucket] = static_cast<uint16_t>(++Count);
			return true;
		}

		bool Has(core::Name kind) const {
			return Heads[BucketFor(kind.Id())] != 0;
		}

		Handler Find(core::Name kind, uint16_t activeFamilies) const {
			return Resolve(kind, activeFamilies).Run;
		}

		Resolution Resolve(core::Name kind, uint16_t activeFamilies) const {
			uint16_t current = Heads[BucketFor(kind.Id())];
			if (current == 0) return {};
			while (current != 0) {
				const auto &entry = Entries[current - 1];
				if (entry.Family == 0 || (entry.Family & activeFamilies) != 0) return {entry.Run, true};
				current = entry.Previous;
			}
			return {nullptr, true};
		}

	  private:
		struct Entry {
			uint32_t KindId = 0;
			Handler Run = nullptr;
			uint16_t Family = 0;
			uint16_t Previous = 0;
		};

		// At most 128 distinct kinds occupy 256 buckets, so every failed probe
		// reaches an empty bucket. Duplicate registrations share a bucket and
		// chain backward, preserving the last active family's override.
		size_t BucketFor(uint32_t identifier) const {
			size_t bucket = (identifier * 0x9e3779b9u) >> 24u;
			while (Heads[bucket] != 0 && Entries[Heads[bucket] - 1].KindId != identifier)
				bucket = (bucket + 1) & (Heads.size() - 1);
			return bucket;
		}

		std::array<Entry, 128> Entries{};
		std::array<uint16_t, 256> Heads{};
		size_t Count = 0;
		uint16_t Family = 0;
	};
}
