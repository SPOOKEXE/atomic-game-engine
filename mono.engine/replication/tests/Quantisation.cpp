// A component that crosses in a compact form, and the four things that has to
// leave alone.
//
// **The seam rather than the codec.** `engine.scene.wire` is where the grid and
// its error bound are measured; this module does not know what a transform is
// and must not learn. What it owns is the choice between a type's file
// serialisation and its wire one, and the whole risk of that choice is making
// it in the wrong place:
//
// - the server's own store, which must be untouched by the fact that somebody
//   replicated it, or `just determinism` stops meaning anything,
// - `Store::Save`, which is what a recording is made of and must stay lossless,
//   or `just replay-check` compares one lossy file against another and passes,
// - the join snapshot and the delta stream, which must deliver the same decoded
//   value or a client's world depends on when it joined,
// - a component with no compact form, which must go on crossing verbatim.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.quantisation")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::ApplyMode;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::ecs::WireFormat;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::Replica;

namespace quantisation_test {

	// **Twenty-eight bytes in the store and ten on the wire, which is the
	// `scene::Transform` figure and is why those are the numbers.** Three
	// coarse axes and four fine ones, standing in for a position and an
	// orientation without this module learning that either exists - it carries
	// named components and has no idea which of them is a position, and a test
	// that borrowed `scene::Transform` to make the point would be the
	// dependency the module refuses.
	struct Pose {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float A = 0.0f;
		float B = 0.0f;
		float C = 0.0f;
		float D = 0.0f;
	};

	// The same twenty-eight bytes with no wire form at all, so the verbatim
	// path stays covered by a case rather than by the absence of one - and so
	// the entities-per-datagram measurement has a real control to measure
	// against instead of an arithmetic guess.
	struct Plain {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		float A = 0.0f;
		float B = 0.0f;
		float C = 0.0f;
		float D = 0.0f;
	};

	// **Larger in a store than a whole delta message, and two bytes on a
	// wire.** The one shape that tells the two sizes apart: a component is
	// refused before it is packed when one entity of it cannot fit an empty
	// message, and measuring that against `sizeof` rather than against the form
	// it actually crosses in refuses a component that would have fitted
	// comfortably. Nothing smaller can show it, because for anything under
	// `ChunkBytes` the two answers agree.
	struct Bulky {
		float Anchor = 0.0f;
		float Ballast[399] = {};
	};

	// Metres per code on the coarse axes, and codes per unit on the fine ones.
	constexpr float GRID_SCALE = 512.0f;
	constexpr int32_t GRID_STEPS = 32767;
	constexpr float FINE_SCALE = 127.0f;
	constexpr int32_t FINE_STEPS = 127;

	int16_t Quantise(float value) {
		const double limit = GRID_STEPS;
		return static_cast<int16_t>(
			std::lround(std::clamp(static_cast<double>(value) * GRID_SCALE, -limit, limit))
		);
	}

	float Dequantise(int16_t code) {
		return static_cast<float>(std::clamp<int32_t>(code, -GRID_STEPS, GRID_STEPS)) / GRID_SCALE;
	}

	int8_t QuantiseFine(float value) {
		const double limit = FINE_STEPS;
		return static_cast<int8_t>(
			std::lround(std::clamp(static_cast<double>(value) * FINE_SCALE, -limit, limit))
		);
	}

	float DequantiseFine(int8_t code) {
		return static_cast<float>(std::clamp<int32_t>(code, -FINE_STEPS, FINE_STEPS)) / FINE_SCALE;
	}

	void WritePoses(ByteWriter &writer, const void *source, size_t count) {
		const auto *poses = static_cast<const Pose *>(source);
		for (size_t index = 0; index < count; index++) {
			const Pose &pose = poses[index];
			writer.WriteInt16(Quantise(pose.X));
			writer.WriteInt16(Quantise(pose.Y));
			writer.WriteInt16(Quantise(pose.Z));
			writer.WriteInt8(QuantiseFine(pose.A));
			writer.WriteInt8(QuantiseFine(pose.B));
			writer.WriteInt8(QuantiseFine(pose.C));
			writer.WriteInt8(QuantiseFine(pose.D));
		}
	}

	void ReadPoses(ByteReader &reader, void *destination, size_t count) {
		auto *poses = static_cast<Pose *>(destination);
		for (size_t index = 0; index < count; index++) {
			Pose &pose = poses[index];
			pose.X = Dequantise(reader.ReadInt16());
			pose.Y = Dequantise(reader.ReadInt16());
			pose.Z = Dequantise(reader.ReadInt16());
			pose.A = DequantiseFine(reader.ReadInt8());
			pose.B = DequantiseFine(reader.ReadInt8());
			pose.C = DequantiseFine(reader.ReadInt8());
			pose.D = DequantiseFine(reader.ReadInt8());
		}
	}

	void WriteBulky(ByteWriter &writer, const void *source, size_t count) {
		const auto *values = static_cast<const Bulky *>(source);
		for (size_t index = 0; index < count; index++) {
			writer.WriteInt16(Quantise(values[index].Anchor));
		}
	}

	void ReadBulky(ByteReader &reader, void *destination, size_t count) {
		auto *values = static_cast<Bulky *>(destination);
		for (size_t index = 0; index < count; index++) {
			values[index].Anchor = Dequantise(reader.ReadInt16());
		}
	}

	// Ten bytes: three sixteen-bit axes and four eight-bit ones.
	constexpr uint32_t POSE_WIRE_BYTES = 3 * sizeof(int16_t) + 4 * sizeof(int8_t);

	void RegisterTypes() {
		static bool once = [] {
			Components::Register<Pose>(
				"quantisation_test.Pose", WireFormat{WritePoses, ReadPoses, POSE_WIRE_BYTES}
			);
			Components::Register<Plain>("quantisation_test.Plain");
			Components::Register<Bulky>(
				"quantisation_test.Bulky", WireFormat{WriteBulky, ReadBulky, sizeof(int16_t)}
			);
			return true;
		}();
		(void)once;
	}

	// A value on no grid, in every field, so a path that quantised it is
	// visible and a path that did not is exact.
	Pose OffGrid(int index) {
		const auto step = static_cast<float>(index);
		return Pose{
			0.10203041f + step,
			-7.7654321f - step,
			33.3333333f + step * 0.001f,
			0.123456789f,
			-0.987654321f,
			0.314159265f,
			-0.271828182f
		};
	}

	Plain PlainValue() {
		return Plain{1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f};
	}

	// The object representation of one component, which is what a snapshot
	// writes and what `just determinism` compares.
	std::vector<std::byte> RawBytes(const Store &store, Entity entity, ComponentId id) {
		const auto *value = static_cast<const std::byte *>(store.GetComponent(entity, id));
		REQUIRE(value != nullptr);
		return {value, value + Components::Describe(id).Size};
	}

	std::vector<std::byte> Saved(Store &store) {
		ByteWriter writer;
		REQUIRE(store.Save(writer));
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	// A server with however many clients, driven a tick at a time.
	struct Server {
		explicit Server(
			const AuthoritySettings &settings = {},
			const std::vector<const char *> &replicated =
				{"quantisation_test.Pose", "quantisation_test.Plain"}
		)
			: World("authority"), Authority_(settings) {
			RegisterTypes();
			for (const char *component : replicated) {
				Authority_.Replicate(Name(component));
			}
			World.Observe<Pose>();
			World.Observe<Plain>();
			World.Observe<Bulky>();
		}

		// One client's world and what it has applied.
		struct Peer {
			explicit Peer(const char *name) : World(name) {}

			Store World;
			Replica Replica_;
			ClientId Handle;
		};

		Peer &Admit(const char *name) {
			Peers.push_back(std::make_unique<Peer>(name));
			Peers.back()->Handle = Authority_.Admit();
			return *Peers.back();
		}

		void Tick() {
			Now++;
			Authority_.Publish(World, Now);

			for (const std::unique_ptr<Peer> &peer : Peers) {
				for (const std::vector<std::byte> &message : Authority_.Outgoing(peer->Handle)) {
					peer->Replica_.Receive(peer->World, message);
				}
			}

			World.ClearChanges();

			for (const std::unique_ptr<Peer> &peer : Peers) {
				const std::vector<std::byte> acknowledgement = peer->Replica_.Acknowledge();
				if (!acknowledgement.empty()) {
					Authority_.Receive(peer->Handle, acknowledgement);
				}
			}
		}

		bool Join(Peer &peer, int limit = 256) {
			for (int attempt = 0; attempt < limit && !peer.Replica_.Joined(); attempt++) {
				Tick();
			}
			return peer.Replica_.Joined();
		}

		Store World;
		Authority Authority_;
		std::vector<std::unique_ptr<Peer>> Peers;
		uint64_t Now = 0;
	};
}

using namespace quantisation_test;

// --- the store the server simulates in -------------------------------------

TEST_CASE("replicating a world does not change the world", "[replication][quantisation]") {
	// **The single thing most likely to go wrong**, and it is asserted on the
	// bytes rather than argued from where the code sits. A compact form is
	// lossy, and the natural implementation is a codec per component with the
	// natural mistake of round-tripping the authority's own values through it
	// - after which the server is simulating the client's approximation of its
	// world and `just determinism` is comparing two runs of that.
	// **Two worlds ticked identically, one of them served.** The same shape
	// `just determinism` uses, in one process: every float operation happens on
	// both in the same order, so any difference between them at the end was put
	// there by replication and by nothing else.
	Server served;
	Store control("authority");
	control.Observe<Pose>();
	control.Observe<Plain>();
	control.Observe<Bulky>();

	std::vector<Entity> all;
	for (int index = 0; index < 64; index++) {
		const Entity entity = served.World.Create();
		served.World.Set<Pose>(entity, OffGrid(index));
		served.World.Set<Plain>(entity, PlainValue());
		all.push_back(entity);

		const Entity mirror = control.Create();
		REQUIRE(mirror == entity);
		control.Set<Pose>(mirror, OffGrid(index));
		control.Set<Plain>(mirror, PlainValue());
	}

	Server::Peer &peer = served.Admit("replica");
	REQUIRE(served.Join(peer));
	control.ClearChanges();

	// Through the delta path as well as the snapshot one, because the two read
	// the store in different ways and only one of them is the join.
	for (int tick = 0; tick < 8; tick++) {
		for (const Entity entity : all) {
			served.World.GetMutable<Pose>(entity)->X += 0.00071f;
			control.GetMutable<Pose>(entity)->X += 0.00071f;
		}
		served.Tick();
		control.ClearChanges();
	}

	// Nothing here is `Approx`. The claim is that the bytes are the ones the
	// simulation wrote, and a quantised value is a different float rather than
	// a nearly equal one.
	const ComponentId pose = Components::Find(Name("quantisation_test.Pose"));
	for (const Entity entity : all) {
		REQUIRE(RawBytes(served.World, entity, pose) == RawBytes(control, entity, pose));
	}

	// And the whole world as a file, which is the form `just determinism` and
	// `just replay-check` compare two runs in.
	REQUIRE(Saved(served.World) == Saved(control));
}

TEST_CASE("a recording of a quantised component is lossless", "[replication][quantisation]") {
	// **The reason the wire form is a second pair of hooks rather than a codec
	// fitted over `TypeDescriptor::Write`.** That codec would have made every
	// recording lossy, and `just replay-check` would have gone on passing -
	// comparing one lossy file against another, which is a check that passes
	// and means nothing. So the file path is asserted to be exact for the same
	// values the wire path visibly rounds.
	RegisterTypes();

	Store source("source");
	std::vector<Entity> all;
	for (int index = 0; index < 32; index++) {
		const Entity entity = source.Create();
		source.Set<Pose>(entity, OffGrid(index));
		all.push_back(entity);
	}

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Apply(reader, ApplyMode::Authoritative));

	for (size_t index = 0; index < all.size(); index++) {
		const Pose *value = restored.Get<Pose>(all[index]);
		REQUIRE(value != nullptr);

		const Pose expected = OffGrid(static_cast<int>(index));
		REQUIRE(value->X == expected.X);
		REQUIRE(value->Y == expected.Y);
		REQUIRE(value->Z == expected.Z);
		REQUIRE(value->A == expected.A);
		REQUIRE(value->D == expected.D);
	}

	// And the wire form of the same value is not exact, or the case above is
	// asserting that nothing is happening.
	const WireFormat wire = Components::Describe(Components::Find(Name("quantisation_test.Pose"))).Wire;
	REQUIRE(wire.Present());

	ByteWriter compact;
	const Pose awkward = OffGrid(1);
	wire.Write(compact, &awkward, 1);
	REQUIRE(compact.Size() == POSE_WIRE_BYTES);

	Pose decoded;
	ByteReader back(compact.Bytes());
	wire.Read(back, &decoded, 1);
	REQUIRE(decoded.X != awkward.X);
	REQUIRE(std::abs(decoded.X - awkward.X) < 1.0f / GRID_SCALE);
}

// --- the two ways a client learns a value ----------------------------------

TEST_CASE("the snapshot and the delta deliver the same value", "[replication][quantisation]") {
	// **If one path quantises and the other does not, a client's world depends
	// on when it joined** - which never shows up as a failure and always shows
	// up as two clients drifting apart. The join snapshot is built from a
	// scratch store and the delta is built from the dirty bits, so they are two
	// separate places that have to make the same choice.
	Server server;

	std::vector<Entity> all;
	for (int index = 0; index < 24; index++) {
		const Entity entity = server.World.Create();
		server.World.Set<Pose>(entity, OffGrid(index));
		all.push_back(entity);
	}

	// The first client learns everything from the snapshot, then follows the
	// world through deltas.
	Server::Peer &early = server.Admit("early");
	REQUIRE(server.Join(early));

	for (int tick = 0; tick < 4; tick++) {
		for (const Entity entity : all) {
			server.World.GetMutable<Pose>(entity)->X += 0.0031f;
		}
		server.Tick();
	}

	// The second learns the moved world from a snapshot instead.
	Server::Peer &late = server.Admit("late");
	REQUIRE(server.Join(late));

	// A quiet tick so the first client is not mid-delta.
	server.Tick();

	for (const Entity entity : all) {
		const Pose *fromDelta = early.World.Get<Pose>(entity);
		const Pose *fromSnapshot = late.World.Get<Pose>(entity);
		REQUIRE(fromDelta != nullptr);
		REQUIRE(fromSnapshot != nullptr);

		// Exactly, not nearly. Two clients holding values a step apart is the
		// drift this case exists to refuse, and it is also what would stop a
		// group signature over replicated state from being a hash at all.
		REQUIRE(fromDelta->X == fromSnapshot->X);
		REQUIRE(fromDelta->Y == fromSnapshot->Y);
		REQUIRE(fromDelta->Z == fromSnapshot->Z);
		REQUIRE(fromDelta->A == fromSnapshot->A);
		REQUIRE(fromDelta->D == fromSnapshot->D);
	}
}

TEST_CASE("a decoded value is inside the grid it was encoded on", "[replication][quantisation]") {
	Server server;

	std::vector<Entity> all;
	for (int index = 0; index < 32; index++) {
		const Entity entity = server.World.Create();
		server.World.Set<Pose>(entity, OffGrid(index));
		all.push_back(entity);
	}

	Server::Peer &peer = server.Admit("replica");
	REQUIRE(server.Join(peer));
	server.Tick();

	const float bound = 0.5f / GRID_SCALE;
	for (size_t index = 0; index < all.size(); index++) {
		const Pose *held = peer.World.Get<Pose>(all[index]);
		const Pose expected = OffGrid(static_cast<int>(index));
		REQUIRE(held != nullptr);
		CHECK(std::abs(held->X - expected.X) <= bound);
		CHECK(std::abs(held->Y - expected.Y) <= bound);
		CHECK(std::abs(held->Z - expected.Z) <= bound);
		CHECK(std::abs(held->A - expected.A) <= 0.5f / FINE_SCALE);
		CHECK(std::abs(held->D - expected.D) <= 0.5f / FINE_SCALE);
	}
}

TEST_CASE("a component with no compact form still crosses verbatim", "[replication][quantisation]") {
	// The other half of the choice, and the one that would break silently: a
	// type with no wire form has to keep using its file serialisation, exactly.
	Server server;

	const Entity entity = server.World.Create();
	const Plain exact{0.10203041f, -7.7654321f, 33.3333333f, 0.5f, -0.25f, 0.125f, -0.0625f};
	server.World.Set<Plain>(entity, exact);

	Server::Peer &peer = server.Admit("replica");
	REQUIRE(server.Join(peer));

	server.World.GetMutable<Plain>(entity)->X = 0.987654321f;
	server.Tick();

	const Plain *held = peer.World.Get<Plain>(entity);
	REQUIRE(held != nullptr);
	REQUIRE(held->X == 0.987654321f);
	REQUIRE(held->Y == exact.Y);
	REQUIRE(held->Z == exact.Z);
}

// --- what it bought --------------------------------------------------------

TEST_CASE("a compact component puts more entities in a datagram", "[replication][quantisation]") {
	// **Measured rather than estimated**, and measured as entity values per
	// message against the real limit rather than as a ratio of two struct
	// sizes. `ChunkBytes` is asked for above what can ever fit so that what is
	// being filled is the cap.
	AuthoritySettings settings;
	settings.ChunkBytes = engine::net::Packet::MAXIMUM_MESSAGE_BYTES * 2;

	const auto rowsPerMessage = [&settings](const char *component) {
		// Only the one being measured is replicated, so the rows counted are
		// that component's rows and nothing else's.
		Server measured(settings, {component});

		std::vector<Entity> all;
		for (int index = 0; index < 400; index++) {
			const Entity entity = measured.World.Create();
			measured.World.Set<Pose>(entity, OffGrid(index));
			measured.World.Set<Plain>(entity, PlainValue());
			all.push_back(entity);
		}

		Server::Peer &peer = measured.Admit("replica");
		REQUIRE(measured.Join(peer));

		// A tick where everything moved, which is the tick a delta is sized by.
		for (const Entity entity : all) {
			measured.World.GetMutable<Pose>(entity)->X += 1.0f;
			measured.World.GetMutable<Plain>(entity)->X += 1.0f;
		}

		measured.Now++;
		measured.Authority_.Publish(measured.World, measured.Now);

		size_t messages = 0;
		size_t largest = 0;
		for (const std::vector<std::byte> &message : measured.Authority_.Outgoing(peer.Handle)) {
			messages++;
			largest = std::max(largest, message.size());
			REQUIRE(message.size() <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
		}

		REQUIRE(messages > 0);
		return std::pair<size_t, size_t>{all.size() / messages, largest};
	};

	const auto [compact, compactLargest] = rowsPerMessage("quantisation_test.Pose");
	const auto [verbatim, verbatimLargest] = rowsPerMessage("quantisation_test.Plain");

	INFO(
		"entities per datagram: verbatim " << verbatim << ", compact " << compact
										   << " · largest message: verbatim " << verbatimLargest
										   << ", compact " << compactLargest
	);

	// **Twenty-eight bytes plus an eight-byte handle becomes ten plus eight**,
	// so the ceiling is 36/18 and the measurement lands on it: 25 entity values
	// a datagram becomes 50. The handle is what stops it being 2.8x, and it is
	// also why quantising a component smaller than a handle would buy almost
	// nothing.
	//
	// A floor rather than an equality, because the exact count moves with this
	// layer's per-message header. Set close under the measured 2.0x, so a
	// change that halved the benefit fails here rather than being absorbed.
	CHECK(compact > verbatim);
	CHECK(compact * 100 >= verbatim * 180);

	// And no message got larger doing it.
	CHECK(compactLargest <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
	CHECK(verbatimLargest <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
}

TEST_CASE("a truncated compact value is refused rather than half applied", "[replication][quantisation]") {
	// Every field of an inbound message is hostile, and a compact form is a
	// different number of bytes per entity - so a message claiming more
	// entities than it carries values for is the shape of malformed this change
	// introduced.
	Server server;
	const Entity entity = server.World.Create();
	server.World.Set<Pose>(entity, OffGrid(3));

	Server::Peer &peer = server.Admit("replica");
	REQUIRE(server.Join(peer));

	engine::replication::Delta delta;
	delta.Tick = server.Now + 1;
	engine::replication::ComponentDelta component;
	component.Component = Name("quantisation_test.Pose");
	component.Entities.push_back(entity);
	component.Values.assign(POSE_WIRE_BYTES / 2, std::byte{0}); // Half of one value.
	delta.Components.push_back(std::move(component));

	ByteWriter writer;
	WriteMessage(writer, delta);

	const std::vector<std::byte> message(writer.Bytes().begin(), writer.Bytes().end());
	CHECK(peer.Replica_.Receive(peer.World, message) == engine::replication::ApplyStatus::Malformed);
}

TEST_CASE("a component too large for a message crosses in its compact form", "[replication][quantisation]") {
	// **The check that refuses an unpackable component has to measure the form
	// that is packed.** One entity of a sixteen-hundred-byte component cannot
	// fit an empty message and is refused before packing begins - correctly,
	// because dropping it quietly would be a component that never arrives. Two
	// bytes fits with room to spare, so measuring the stored size instead is a
	// component silently refused for a size nothing ever sends.
	AuthoritySettings settings;
	settings.ChunkBytes = 512;

	Server server(settings, {"quantisation_test.Bulky"});
	REQUIRE(sizeof(Bulky) > settings.ChunkBytes);

	const Entity entity = server.World.Create();
	server.World.Set<Bulky>(entity, Bulky{3.5f, {}});

	Server::Peer &peer = server.Admit("replica");
	REQUIRE(server.Join(peer));

	// Through the delta path too, which is the one the size check gates.
	server.World.GetMutable<Bulky>(entity)->Anchor = -12.25f;
	server.Tick();

	const Bulky *held = peer.World.Get<Bulky>(entity);
	REQUIRE(held != nullptr);
	CHECK(held->Anchor == -12.25f);
}
