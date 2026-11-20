// Quiet publishes of escaped, non-trivial components that name a content revision.
//
// The shape of a streamed world's EditableMesh rows: hundreds of whole payloads
// a mutable pointer has escaped from, so the authority cannot trust its change
// tracking and re-signs them every tick. A type whose descriptor names a
// `ContentRevision` lets the signer skip encoding rows whose revision repeats.
//
// @tier L9 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.revisioned")

namespace revisioned_bench {
	using engine::core::ByteReader;
	using engine::core::ByteWriter;
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::replication::Authority;
	using engine::replication::AuthoritySettings;
	using engine::replication::ClientId;

	constexpr size_t ENTITIES = 256;
	constexpr size_t WORDS = 4096;

	// A whole payload and the revision every edit bumps, as EditableMesh keeps.
	struct Blob {
		std::vector<uint32_t> Words;
		uint64_t Revision = 0;
	};

	void WriteBlobs(ByteWriter &writer, const void *source, size_t count) {
		const auto *rows = static_cast<const Blob *>(source);
		for (size_t index = 0; index < count; ++index) {
			writer.WriteUInt32(static_cast<uint32_t>(rows[index].Words.size()));
			for (const uint32_t word : rows[index].Words)
				writer.WriteUInt32(word);
			writer.WriteUInt64(rows[index].Revision);
		}
	}

	void ReadBlobs(ByteReader &reader, void *destination, size_t count) {
		auto *rows = static_cast<Blob *>(destination);
		for (size_t index = 0; index < count; ++index) {
			rows[index].Words.resize(reader.ReadUInt32());
			for (uint32_t &word : rows[index].Words)
				word = reader.ReadUInt32();
			rows[index].Revision = reader.ReadUInt64();
		}
	}

	uint64_t BlobRevision(const void *value) {
		return static_cast<const Blob *>(value)->Revision;
	}

	// One authority, one client and escaped rows, joined and settled once.
	struct Fixture {
		Store World{"engine.bench.revisioned"};
		Authority Server;
		ClientId Client{};
		uint64_t Tick = 1;

		// Publishes, then acknowledges what went out, as a client would.
		void Step() {
			Server.Publish(World, Tick);
			uint64_t revision = 0;
			for (const auto &bytes : Server.Outgoing(Client)) {
				ByteReader reader(bytes);
				engine::replication::Message message;
				if (!engine::replication::ReadMessage(reader, message)) continue;
				if (message.Kind == engine::replication::MessageKind::Delta)
					revision = std::max(revision, message.Delta.Revision);
				if (message.Kind == engine::replication::MessageKind::SnapshotChunk)
					revision = std::max(revision, message.Chunk.Revision);
			}
			if (revision != 0) {
				ByteWriter writer;
				engine::replication::WriteMessage(writer, engine::replication::Applied{Tick, 0, revision});
				Server.Receive(Client, writer.Bytes());
			}
			World.ClearChanges();
			Tick++;
		}
	};

	Fixture &Prepared() {
		static std::unique_ptr<Fixture> fixture;
		if (fixture) return *fixture;
		static const auto id = engine::ecs::Components::Register<Blob>(
			"bench.revisioned.Blob", WriteBlobs, ReadBlobs, 0, BlobRevision
		);
		AuthoritySettings settings;
		settings.ChunksPerTick = 65536;
		settings.JoinsPerTick = 0;
		settings.BytesPerTick = 64 * 1024 * 1024;
		fixture = std::make_unique<Fixture>();
		fixture->Server = Authority(settings);
		fixture->Server.Replicate(Name("bench.revisioned.Blob"));
		fixture->Server.SetInterest([](ClientId, Entity, const Store &) { return true; });
		Blob blob;
		blob.Words.resize(WORDS);
		for (size_t index = 0; index < ENTITIES; ++index) {
			const Entity entity = fixture->World.Create();
			for (size_t word = 0; word < WORDS; ++word)
				blob.Words[word] = static_cast<uint32_t>(index * 31 + word);
			blob.Revision = index + 1;
			fixture->World.SetComponent(entity, id, &blob);
		}
		fixture->World.ObserveComponent(id);
		fixture->Client = fixture->Server.Admit();
		fixture->Step();
		// A mutable pointer escapes from every row, as an editing API does.
		fixture->World.EachEntity([&](Entity entity) {
			(void)fixture->World.GetComponentMutable(entity, id);
		});
		for (int warm = 0; warm < 8; ++warm)
			fixture->Step();
		return *fixture;
	}
}

using namespace revisioned_bench;

BENCH("Quiet publish · 256 escaped 16 KiB revisioned rows", 1) {
	Prepared().Step();
}
