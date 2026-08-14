// The anti-entropy audit: a replica that has quietly diverged, and a client
// that says everything has.
//
// **The two cases that matter are the two ends of the same message.** A replica
// whose copy is wrong in a way no delta would ever mention has to be found and
// put right; a peer that answers by claiming the whole world is wrong has to be
// refused, by the server, without the server having to trust a number the peer
// chose. Everything else here exists to show that a replica in *agreement* is
// never disturbed — a false mismatch is worse than no audit, because it is a
// repair loop nobody asked for.
//
// No transport, for `Replication.cpp`'s reason: `Authority` produces messages
// and `Replica` consumes them, so a divergence is something a case *states*
// rather than arranges.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.replication.audit")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::Disputed;
using engine::replication::Replica;

namespace audit_test {

	// A plain replicated value, carried on the wire exactly as it is stored.
	struct Spot {
		float X = 0.0f;
		float Y = 0.0f;
	};

	// A unit direction, crossing as smallest-two.
	//
	// **Modelled on `scene::Transform`'s rotation rather than on a plain
	// scale, and that is the whole point of it.** A scalar grid is idempotent —
	// re-encoding a decoded value gives the code back — so it cannot tell an
	// authority that hashes its own value from one that hashes what the far
	// side holds. Dropping the largest component and recovering it from unit
	// length is not idempotent: the recovered component can come back a hair
	// *below* one of the two that were sent, and the next encode drops a
	// different one. Measured on the real four-component codec at **1666 of 2
	// million uniformly random orientations**, which on a world of a few
	// thousand parts is a false mismatch reported for ever, every sweep.
	struct Facing {
		float A = 0.0f;
		float B = 0.0f;
		float C = 0.0f;
	};

	// The tag that takes `Spot`'s deltas off the wire for one entity.
	struct Derived {
		uint8_t Value = 0;
	};

	// The largest a component that is *not* dropped can be, which is sqrt(2/3).
	constexpr float FACING_LIMIT = 0.81649658f;
	constexpr int32_t FACING_STEPS = 511;
	constexpr float FACING_SCALE = static_cast<float>(FACING_STEPS) / FACING_LIMIT;

	void WriteFacing(engine::core::ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const Facing *>(source);
		for (size_t index = 0; index < count; index++) {
			const float axes[3] = {values[index].A, values[index].B, values[index].C};

			uint8_t largest = 0;
			for (uint8_t axis = 1; axis < 3; axis++) {
				if (std::abs(axes[axis]) > std::abs(axes[largest])) {
					largest = axis;
				}
			}

			const float sign = axes[largest] < 0.0f ? -1.0f : 1.0f;
			writer.WriteUInt8(largest);
			for (uint8_t axis = 0; axis < 3; axis++) {
				if (axis == largest) {
					continue;
				}
				const double scaled = static_cast<double>(axes[axis] * sign) * FACING_SCALE;
				const double limit = static_cast<double>(FACING_STEPS);
				writer.WriteInt16(static_cast<int16_t>(std::lround(std::clamp(scaled, -limit, limit))));
			}
		}
	}

	void ReadFacing(engine::core::ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<Facing *>(destination);
		for (size_t index = 0; index < count; index++) {
			const uint8_t largest = static_cast<uint8_t>(reader.ReadUInt8() % 3u);

			float axes[3] = {0.0f, 0.0f, 0.0f};
			float sum = 0.0f;
			for (uint8_t axis = 0; axis < 3; axis++) {
				if (axis == largest) {
					continue;
				}
				const int32_t code = std::clamp<int32_t>(reader.ReadInt16(), -FACING_STEPS, FACING_STEPS);
				axes[axis] = static_cast<float>(code) / FACING_SCALE;
				sum += axes[axis] * axes[axis];
			}
			axes[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));

			const float length = std::sqrt(axes[0] * axes[0] + axes[1] * axes[1] + axes[2] * axes[2]);
			values[index] = Facing{axes[0] / length, axes[1] / length, axes[2] / length};
		}
	}

	constexpr uint32_t FACING_BYTES = sizeof(uint8_t) + 2 * sizeof(int16_t);

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Spot>("audit_test.Spot");
			engine::ecs::Components::Register<Facing>(
				"audit_test.Facing", engine::ecs::WireFormat{WriteFacing, ReadFacing, FACING_BYTES}
			);
			engine::ecs::Components::Register<Derived>("audit_test.Derived");
			return true;
		}();
		(void)once;
	}

	// One of the directions the codec above does not encode idempotently.
	//
	// Found by sweep rather than reasoned to, and pinned here so the case is a
	// statement rather than a probability: `(-0.675540149, -0.676076293,
	// 0.294221431)` re-encodes with a different component dropped.
	constexpr Facing UNSTABLE{-0.675540149f, -0.676076293f, 0.294221431f};

	// A server and a client with the audit switched on, and the answer routed
	// back the way `Connector::Poll` routes it.
	struct Pair {
		explicit Pair(AuthoritySettings settings = Enabled())
			: Server("server"), Client("client"), Authority_(settings) {
			RegisterTypes();
			Authority_.Replicate(Name("audit_test.Spot"));
			Authority_.Replicate(Name("audit_test.Facing"));
			Server.Observe<Spot>();
			Server.Observe<Facing>();
		}

		static AuthoritySettings Enabled() {
			AuthoritySettings settings;
			settings.Audit.Enabled = true;
			return settings;
		}

		// `strand` keeps a tick's values off the client while letting everything
		// else through, which is what a lost delta, a deferred one and a
		// reordered one all leave behind on the server: an unconfirmed row.
		void Tick(bool strand = false) {
			Now++;
			Authority_.Publish(Server, Now);

			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				const auto kind = engine::replication::PeekMessageKind(message);
				if (strand && kind == engine::replication::MessageKind::Delta) {
					continue;
				}

				// How wide the slice was, so a case can name a group just past
				// the end of it without knowing the settings.
				if (kind == engine::replication::MessageKind::GroupSignatures) {
					engine::core::ByteReader reader(message);
					engine::replication::Message read;
					REQUIRE(ReadMessage(reader, read));
					Groups = read.Signatures.Groups.size();
				}

				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				Authority_.Receive(Handle, ack);
			}

			const std::vector<std::byte> dispute = Replica_.Dispute();
			if (!dispute.empty()) {
				Disputes++;
				Authority_.Receive(Handle, dispute);
			}
		}

		bool Join(int limit = 64) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		void Run(int ticks) {
			for (int index = 0; index < ticks; index++) {
				Tick();
			}
		}

		// Ticks until an audit goes out, and answers the tick it went out on.
		//
		// `Statistics::Audits` is what the last `Publish` did, so this is the
		// tick number rather than a guess at the cadence — a case that guessed
		// would be answering an audit the server never asked.
		uint64_t Audit(int limit = 32) {
			for (int attempt = 0; attempt < limit; attempt++) {
				Tick();
				if (Authority_.Stats().Audits > 0) {
					return Now;
				}
			}
			return 0;
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle = Authority_.Admit();
		uint64_t Now = 0;
		size_t Disputes = 0;
		size_t Groups = 0;
	};

	std::vector<std::byte> Encode(const Disputed &disputed) {
		engine::core::ByteWriter writer;
		WriteMessage(writer, disputed);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}
}

using namespace audit_test;

// --- the audit finds what a delta cannot -------------------------------------

TEST_CASE("a deliberately diverged replica is found and repaired", "[replication][audit]") {
	// **The assertion the whole layer is for.** The world is still, so the
	// delta path has nothing to say and would never say anything again; the
	// client's copy is wrong; and it is put right anyway.
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 10; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(pair.Join());
	pair.Run(4);

	// A still world, agreed on by both ends.
	REQUIRE(pair.Client.Get<Spot>(entities[3])->X == 3.0f);
	REQUIRE(pair.Replica_.Stats().Mismatched == 0);

	// The divergence. Written straight into the replica's store, which is
	// exactly what a lost value, a stranded row or a half-applied tick leaves
	// behind — and none of those move a dirty bit on the server.
	pair.Client.GetMutable<Spot>(entities[3])->X = -1.0f;

	pair.Run(24);

	REQUIRE(pair.Replica_.Stats().Audits > 0);
	REQUIRE(pair.Replica_.Stats().Mismatched > 0);
	REQUIRE(pair.Authority_.Stats().Disputed > 0);
	REQUIRE(pair.Authority_.Stats().Repaired > 0);

	// And the repair landed, from the recovery walk that already existed.
	REQUIRE(pair.Client.Get<Spot>(entities[3])->X == 3.0f);
	REQUIRE(pair.Authority_.Stats().DisputesRefused == 0);
}

TEST_CASE("an entity missing from a replica is found too", "[replication][audit]") {
	// The lost creation, which is the other half of the class this catches:
	// the server is certain the client holds something it has never had, and
	// nothing that counts ticks can see it.
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 8; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(pair.Join());
	pair.Run(4);

	pair.Client.Destroy(entities[5]);
	pair.Run(24);

	REQUIRE(pair.Replica_.Stats().Mismatched > 0);
	REQUIRE(pair.Authority_.Stats().Disputed > 0);
}

TEST_CASE("a replica in agreement is never disturbed", "[replication][audit]") {
	// **A false mismatch is worse than no audit**, because it is a repair loop
	// nobody asked for and nothing in the ordinary numbers would explain. The
	// world here moves, stops, gains an entity and loses one, and the audit
	// says nothing throughout.
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 20; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		pair.Server.Set<Facing>(entity, Facing{0.6f, 0.8f, 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(pair.Join());

	for (int round = 0; round < 60; round++) {
		if (round % 7 == 0) {
			pair.Server.Set<Spot>(
				entities[static_cast<size_t>(round % 20)], Spot{static_cast<float>(round), 1.0f}
			);
		}
		if (round == 20) {
			const Entity arrival = pair.Server.Create();
			pair.Server.Set<Spot>(arrival, Spot{99.0f, 0.0f});
			pair.Server.Set<Facing>(arrival, UNSTABLE);
		}
		if (round == 40) {
			pair.Server.Destroy(entities[1]);
		}
		pair.Tick();
	}

	REQUIRE(pair.Replica_.Stats().Audits > 1);
	REQUIRE(pair.Replica_.Stats().Mismatched == 0);
	REQUIRE(pair.Authority_.Stats().Disputed == 0);
	REQUIRE(pair.Disputes == 0);
}

TEST_CASE("a value still in flight is not a disagreement", "[replication][audit]") {
	// **The audit only ever catches *stale* divergence, and this is where that
	// stops being a slogan.** A value the delta path has not landed yet is not
	// a value the two ends should agree on — the server is simply ahead. An
	// audit that hashed it would report a mismatch every time the byte budget
	// deferred something, on exactly the servers the budget exists for, and the
	// repair would ask for the value that was already on its way.
	Pair pair;

	std::vector<Entity> entities;
	for (int index = 0; index < 12; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}

	REQUIRE(pair.Join());
	pair.Run(4);

	// The world moves and the values never arrive, so the server holds a row
	// this client has not confirmed for as long as the case runs.
	for (int round = 0; round < 40; round++) {
		pair.Server.Set<Spot>(entities[2], Spot{static_cast<float>(round), 7.0f});
		pair.Tick(true);
	}

	REQUIRE(pair.Replica_.Stats().Audits > 1);
	REQUIRE(pair.Replica_.Stats().Mismatched == 0);
	REQUIRE(pair.Authority_.Stats().Disputed == 0);
}

TEST_CASE("a quantised value is hashed as the far side holds it", "[replication][audit]") {
	// **An authority hashing its own value would disagree with a replica for
	// ever here**, on a world nothing is touching, because this codec does not
	// encode its own decoding back to the same bytes — see `Facing`. The
	// authority puts its value through the same encode-and-decode a join
	// snapshot goes through, so both ends hash one expression over one buffer
	// and agreement is by construction rather than by the quantiser happening
	// to be idempotent.
	Pair pair;

	for (int index = 0; index < 12; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Facing>(entity, UNSTABLE);
	}

	REQUIRE(pair.Join());
	pair.Run(40);

	REQUIRE(pair.Replica_.Stats().Audits > 1);
	REQUIRE(pair.Replica_.Stats().Mismatched == 0);
}

TEST_CASE("an entity the receiver derives for itself is not audited", "[replication][audit]") {
	// `SuppressWhenTagged` stops a component's *deltas* for a tagged entity
	// because the receiver recomputes that row — so the two ends are meant to
	// disagree about it, and an audit that hashed it would report every
	// character in the world as a mismatch on every sweep.
	//
	// The whole entity is left out rather than the row, which is what keeps
	// suppression off the wire: membership is listed, so the cheap answer is
	// not to name it.
	Pair pair;
	pair.Authority_.SuppressWhenTagged(Name("audit_test.Spot"), Name("audit_test.Derived"));

	std::vector<Entity> entities;
	for (int index = 0; index < 8; index++) {
		const Entity entity = pair.Server.Create();
		pair.Server.Set<Spot>(entity, Spot{static_cast<float>(index), 0.0f});
		entities.push_back(entity);
	}
	pair.Server.Set<Derived>(entities[2], Derived{1});

	REQUIRE(pair.Join());
	pair.Run(4);

	// What a local derivation looks like: the replica's own answer for a row it
	// is responsible for, which is not the authority's.
	pair.Client.GetMutable<Spot>(entities[2])->X = 1234.0f;

	pair.Run(40);

	REQUIRE(pair.Replica_.Stats().Audits > 1);
	REQUIRE(pair.Replica_.Stats().Mismatched == 0);
}

// --- the answer is upstream traffic from a peer -------------------------------

TEST_CASE("a client claiming everything mismatches is refused", "[replication][audit]") {
	// **The rate limit is the server's and none of it is taken from the
	// client.** An answer may only ever name groups out of the slice this
	// server chose to ask about, for the tick it asked on, once. Everything a
	// peer could inflate is checked here.
	Pair pair;

	for (int index = 0; index < 12; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(pair.Join());

	// **The tick an audit actually went out on, rather than any tick.** An
	// answer naming the wrong one is refused before the group checks are
	// reached, so a case that guessed would be exercising the cheapest check
	// three times and none of the others.
	const uint64_t audited = pair.Audit();
	REQUIRE(audited != 0);

	const size_t repaired = pair.Authority_.Stats().Repaired;

	// The whole space of group labels, which is what "everything mismatches"
	// looks like on the wire.
	Disputed everything;
	everything.Tick = audited;
	for (uint32_t group = 0; group < engine::replication::MAXIMUM_AUDIT_GROUPS; group++) {
		everything.Groups.push_back(group);
	}
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(everything)));

	// One group named a hundred times, which is the cheapest way to ask for a
	// hundred repairs out of a slice holding one.
	Disputed repeated;
	repeated.Tick = audited;
	repeated.Groups.assign(100, 0);
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(repeated)));

	// A group just past the end of the slice, which is the same claim without
	// the volume.
	Disputed beyond;
	beyond.Tick = audited;
	beyond.Groups.push_back(static_cast<uint32_t>(pair.Groups));
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(beyond)));

	// A tick this server never audited.
	Disputed invented;
	invented.Tick = audited + 5000;
	invented.Groups.push_back(0);
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(invented)));

	// Nothing was refused into a repair, and every refusal is counted.
	REQUIRE(pair.Authority_.Stats().Repaired == repaired);
	REQUIRE(pair.Authority_.Stats().DisputesRefused == 4);

	// And the audit is still answerable, because a refusal is not an answer.
	Disputed honest;
	honest.Tick = audited;
	honest.Groups.push_back(0);
	REQUIRE(pair.Authority_.Receive(pair.Handle, Encode(honest)));
}

TEST_CASE("an audit may be answered once", "[replication][audit]") {
	// A second answer to one question is a client repeating itself or a client
	// pushing, and the two are refused the same way.
	Pair pair;

	for (int index = 0; index < 12; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(pair.Join());

	const uint64_t audited = pair.Audit();
	REQUIRE(audited != 0);

	Disputed answer;
	answer.Tick = audited;
	answer.Groups.push_back(0);

	REQUIRE(pair.Authority_.Receive(pair.Handle, Encode(answer)));
	const size_t once = pair.Authority_.Stats().Repaired;
	REQUIRE(once > 0);

	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(answer)));
	REQUIRE(pair.Authority_.Stats().Repaired == once);
	REQUIRE(pair.Authority_.Stats().DisputesRefused == 1);
}

TEST_CASE("an audit the link refused may not be answered", "[replication][audit]") {
	// A question that was never asked is the one place an answer would not be
	// bounded by what the server had chosen to look at, so `Unsent` takes the
	// record away with the message.
	Pair pair;

	for (int index = 0; index < 12; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(pair.Join());

	uint64_t audited = 0;
	for (int attempt = 0; attempt < 32 && audited == 0; attempt++) {
		pair.Now++;
		pair.Authority_.Publish(pair.Server, pair.Now);
		if (pair.Authority_.Stats().Audits > 0) {
			audited = pair.Now;

			// The audit is built last, so it is the final message of the tick.
			pair.Authority_.Unsent(pair.Handle, pair.Authority_.Outgoing(pair.Handle).size() - 1);
		}
		pair.Server.ClearChanges();
	}
	REQUIRE(audited != 0);

	Disputed answer;
	answer.Tick = audited;
	answer.Groups.push_back(0);

	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(answer)));
	REQUIRE(pair.Authority_.Stats().Repaired == 0);
}

TEST_CASE("an authority with the audit off says nothing about it", "[replication][audit]") {
	// The library default, and the property `replication/AGENTS.md` states: a
	// quiet world sends nothing.
	Pair pair(AuthoritySettings{});

	for (int index = 0; index < 12; index++) {
		pair.Server.Set<Spot>(pair.Server.Create(), Spot{static_cast<float>(index), 0.0f});
	}

	REQUIRE(pair.Join());
	pair.Run(40);

	REQUIRE(pair.Authority_.Stats().Audits == 0);
	REQUIRE(pair.Replica_.Stats().Audits == 0);

	Disputed answer;
	answer.Tick = pair.Now;
	answer.Groups.push_back(0);
	REQUIRE_FALSE(pair.Authority_.Receive(pair.Handle, Encode(answer)));
}
