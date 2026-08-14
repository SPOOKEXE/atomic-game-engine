#include <engine/assets/HashTree.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/replication/Audit.hpp>

#include <vector>

namespace engine::replication {

	namespace {
		// The value a replica holds for one component, in its wire form.
		//
		// **The authority's own value goes round the wire trip first, and that
		// is the line this whole layer stands on.** A quantised value is not
		// the value the far side holds; the value the far side holds is what a
		// decode of the encoding produced. Writing that back out gives both
		// ends `encode(decode(encode(v)))` from one input, which is one
		// expression over one buffer rather than an assumption that the
		// quantiser is idempotent — and `replication/AGENTS.md` is explicit
		// that a hash with a tolerance is not a hash.
		void WriteHeldValue(
			core::ByteWriter &into,
			const ecs::TypeDescriptor &descriptor,
			const void *value,
			AuditSide side,
			core::ByteWriter &crossing,
			std::vector<std::byte> &decoded
		) {
			if (!descriptor.Wire.Present()) {
				descriptor.Write(into, value, 1);
				return;
			}

			if (side == AuditSide::Replica) {
				descriptor.Wire.Write(into, value, 1);
				return;
			}

			crossing.Clear();
			descriptor.Wire.Write(crossing, value, 1);

			core::ByteReader reader(crossing.Bytes());
			decoded.assign(descriptor.Size, std::byte{0});
			descriptor.DefaultConstruct(decoded.data(), 1);
			descriptor.Wire.Read(reader, decoded.data(), 1);
			descriptor.Wire.Write(into, decoded.data(), 1);
			descriptor.Destruct(decoded.data(), 1);
		}
	}

	assets::ContentHash AuditDigest(
		const ecs::Store &store,
		std::span<const ecs::ComponentId> components,
		std::span<const ecs::Entity> entities,
		AuditSide side
	) {
		ENGINE_PROFILE_CAT("replication.audit", core::ProfileCategory::Network);

		std::vector<assets::ContentHash> leaves;
		leaves.reserve(entities.size());

		core::ByteWriter leaf;
		core::ByteWriter body;
		core::ByteWriter crossing;
		std::vector<std::byte> decoded;

		for (const ecs::Entity entity : entities) {
			if (!store.Alive(entity)) {
				continue;
			}

			for (size_t ordinal = 0; ordinal < components.size(); ordinal++) {
				const ecs::ComponentId component = components[ordinal];
				if (!component.IsValid()) {
					continue;
				}

				const void *value = store.GetComponent(entity, component);
				if (value == nullptr) {
					continue;
				}

				const ecs::TypeDescriptor &descriptor = ecs::Components::Describe(component);
				if (descriptor.Size > 0 && !descriptor.Serialisable) {
					continue;
				}

				// **The ordinal rather than the name, because the far side was
				// handed the same list in the same order.** A name in the leaf
				// would be the same fact spelled twice, and two spellings of
				// one fact is how the two ends come to disagree about a
				// component neither of them changed.
				leaf.Clear();
				leaf.WriteUInt64(entity.Id);
				leaf.WriteUInt32(static_cast<uint32_t>(ordinal));

				body.Clear();
				if (descriptor.Size > 0) {
					WriteHeldValue(body, descriptor, value, side, crossing, decoded);
				}
				leaf.WriteUInt32(static_cast<uint32_t>(body.Bytes().size()));
				leaf.WriteRaw(body.Bytes().data(), body.Bytes().size());

				leaves.push_back(assets::Hasher::Of(leaf.Bytes()));
			}
		}

		return assets::HashTree::RootOf(leaves);
	}
}
