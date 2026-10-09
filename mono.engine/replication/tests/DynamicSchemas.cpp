#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/QuicSession.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/replication/Session.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

TEST_SUITE_ID("engine.replication.dynamicschemas")
TEST_DEPENDS("engine.ecs.schema")

namespace {
	using namespace engine;
	struct Anchor {
		double Value = 0;
	};
	struct BulkValue {
		std::string Text;
	};
	struct Link {
		ecs::Store Server{"dynamic-authority"};
		ecs::Store Client{"dynamic-replica"};
		replication::Authority Authority;
		replication::Replica Replica;
		replication::ClientId Peer;
		uint64_t Tick = 0;
		std::vector<std::vector<std::byte>> Delivered;
		explicit Link(const replication::AuthoritySettings &settings = {}) : Authority(settings) {
			static const auto anchor = ecs::Components::Register<Anchor>("replication_dynamic.Anchor");
			(void)anchor;
			Authority.Replicate(core::Name("replication_dynamic.Anchor"));
			Peer = Authority.Admit();
		}
		void Step(bool refuseSchema = false) {
			Authority.Publish(Server, ++Tick);
			const auto outgoing = Authority.Outgoing(Peer);
			for (size_t index = 0; index < outgoing.size(); ++index) {
				if (refuseSchema &&
					replication::PeekMessageKind(outgoing[index]) == replication::MessageKind::Schemas) {
					Authority.Unsent(Peer, index);
					continue;
				}
				Delivered.push_back(outgoing[index]);
				REQUIRE(Replica.Receive(Client, outgoing[index]) == replication::ApplyStatus::Ok);
			}
			Server.ClearChanges();
			const auto acknowledgement = Replica.Acknowledge();
			if (!acknowledgement.empty()) REQUIRE(Authority.Receive(Peer, acknowledgement));
		}
		void Steps(size_t count = 24) {
			for (size_t index = 0; index < count; ++index)
				Step();
		}
	};
	ecs::ComponentId Declare(std::string_view name, bool replicated = true) {
		const auto result = ecs::Schemas::Register(
			name,
			std::array{
				ecs::FieldSpec{"Energy", ecs::PropertyType::Double},
				ecs::FieldSpec{"Phase", ecs::PropertyType::Double},
			}
		);
		REQUIRE(result.Why == ecs::Schemas::Status::Ok);
		if (replicated)
			REQUIRE(ecs::Schemas::SetTags(result.Id, std::array<std::string_view, 1>{"replicated"}));
		return result.Id;
	}
	void Set(ecs::Store &store, ecs::Entity entity, ecs::ComponentId id, double energy, double phase = 0) {
		const auto &descriptor = ecs::Components::Describe(id);
		const auto *schema = ecs::Schemas::Of(id);
		std::vector<std::byte> value(descriptor.Size);
		descriptor.DefaultConstruct(value.data(), 1);
		REQUIRE(ecs::Schemas::WriteField(value.data(), *schema->Find("Energy"), &energy));
		REQUIRE(ecs::Schemas::WriteField(value.data(), *schema->Find("Phase"), &phase));
		store.SetComponent(entity, id, value.data());
		descriptor.Destruct(value.data(), 1);
	}
	double Energy(const ecs::Store &store, ecs::Entity entity, ecs::ComponentId id) {
		const auto *value = store.GetComponent(entity, id);
		REQUIRE(value != nullptr);
		const auto *field = ecs::Schemas::Of(id)->Find("Energy");
		alignas(double) std::array<std::byte, sizeof(double)> scratch{};
		return *static_cast<const double *>(ecs::Schemas::ReadField(value, *field, scratch.data()));
	}
}

TEST_CASE("late opt-in schemas join update and retry refused descriptions", "[replication][schema]") {
	Link link;
	const auto entity = link.Server.Create();
	link.Server.Set(entity, Anchor{4});
	link.Steps();
	REQUIRE(link.Replica.Joined());
	const auto state = Declare("replication_dynamic.LateState", false);
	Set(link.Server, entity, state, 11);
	link.Steps();
	CHECK_FALSE(link.Client.HasComponent(entity, state));
	REQUIRE(ecs::Schemas::SetTags(state, std::array<std::string_view, 1>{"replicated"}));
	link.Step(true);
	CHECK_FALSE(link.Client.HasComponent(entity, state));
	link.Steps();
	CHECK(Energy(link.Client, entity, state) == 11);
	Set(link.Server, entity, state, 37, 2);
	link.Steps(3);
	CHECK(Energy(link.Client, entity, state) == 37);
	CHECK(link.Client.Get<Anchor>(entity)->Value == 4);
	REQUIRE(ecs::Schemas::SetTags(state, {}));
	Set(link.Server, entity, state, 90);
	link.Steps();
	CHECK_FALSE(link.Authority.Replicated(core::Name("replication_dynamic.LateState")));
	CHECK_FALSE(link.Client.HasComponent(entity, state));
}

TEST_CASE("interest excludes private schema names until their rows become visible", "[replication][schema]") {
	Link link;
	const auto publicEntity = link.Server.Create();
	link.Server.Set(publicEntity, Anchor{1});
	const auto secretEntity = link.Server.Create();
	const auto state = Declare("replication_dynamic.PrivateState");
	Set(link.Server, secretEntity, state, 25);
	bool visible = false;
	link.Authority.SetInterest([&](replication::ClientId, ecs::Entity entity, const ecs::Store &) {
		return entity != secretEntity || visible;
	});
	link.Steps();
	REQUIRE(link.Replica.Joined());
	CHECK_FALSE(link.Client.Alive(secretEntity));
	for (const auto &message : link.Delivered) {
		core::ByteReader reader(message);
		replication::Message parsed;
		REQUIRE(replication::ReadMessage(reader, parsed));
		if (parsed.Kind == replication::MessageKind::Schemas)
			CHECK(parsed.Schema.Component != core::Name("replication_dynamic.PrivateState"));
	}
	visible = true;
	link.Steps();
	CHECK(Energy(link.Client, secretEntity, state) == 25);
}

TEST_CASE("late explicitly replicated schema resources receive their definition", "[replication][schema]") {
	Link link;
	const auto entity = link.Server.Create();
	link.Server.Set(entity, Anchor{1});
	link.Steps();
	REQUIRE(link.Replica.Joined());
	const auto state = Declare("replication_dynamic.WorldResource", false);
	Set(link.Server, entity, state, 43);
	link.Server.SetResourceById(state, link.Server.GetComponent(entity, state));
	link.Server.RemoveComponent(entity, state);
	link.Authority.Replicate(
		core::Name("replication_dynamic.WorldResource"), replication::ChangeDetection::Observed, true
	);
	link.Steps();
	const void *value = link.Client.ResourceById(state);
	REQUIRE(value != nullptr);
	const auto *field = ecs::Schemas::Of(state)->Find("Energy");
	alignas(double) std::array<std::byte, sizeof(double)> scratch{};
	CHECK(*static_cast<const double *>(ecs::Schemas::ReadField(value, *field, scratch.data())) == 43);
}

TEST_CASE(
	"viewer-local roots descendants and predictions never enter authoritative replication",
	"[replication][schema]"
) {
	Link link;
	const auto folder =
		ecs::Classes::Register("ReplicationDynamicFolder", ecs::Classes::RegisterInstanceRoot(), {});
	const auto ordinary = link.Server.CreateInstance(folder, "ordinary");
	const auto localRoot = link.Server.CreateInstance(folder, "viewer-root");
	const auto localChild = link.Server.CreateInstance(folder, "viewer-child");
	const auto predicted = link.Server.CreatePredicted();
	REQUIRE(link.Server.SetParent(localChild, localRoot));
	link.Server.Set(localRoot, ecs::ClientLocal{});
	for (const auto entity : {ordinary, localRoot, localChild, predicted})
		link.Server.Set(entity, Anchor{3});
	link.Steps();
	REQUIRE(link.Replica.Joined());
	CHECK(link.Client.Alive(ordinary));
	CHECK_FALSE(link.Client.Alive(localRoot));
	CHECK_FALSE(link.Client.Alive(localChild));
	CHECK_FALSE(link.Client.Alive(predicted));
	link.Server.Set(localChild, Anchor{8});
	link.Server.Set(predicted, Anchor{8});
	link.Steps(3);
	CHECK_FALSE(link.Client.Alive(localChild));
	CHECK_FALSE(link.Client.Alive(predicted));
	const auto newLocal = link.Server.CreateInstance(folder, "new-viewer-child");
	REQUIRE(link.Server.SetParent(newLocal, localRoot));
	link.Server.Set(newLocal, Anchor{5});
	link.Steps(3);
	CHECK_FALSE(link.Client.Alive(newLocal));
}

TEST_CASE(
	"schema chunks reject corrupt overlap and invalid definitions before registration",
	"[replication][schema]"
) {
	ecs::Store store{"schema-refusal"};
	replication::Replica replica;
	core::ByteWriter definition;
	definition.WriteUInt32(1);
	definition.WriteString("Energy");
	definition.WriteString("NotAType");
	definition.WriteString("native");
	definition.WriteString("");
	replication::SchemaChunk chunk;
	chunk.Component = core::Name("replication_dynamic.InvalidInbound");
	chunk.TotalBytes = static_cast<uint32_t>(definition.Size());
	chunk.Bytes.assign(definition.Bytes().begin(), definition.Bytes().begin() + 3);
	core::ByteWriter first;
	replication::WriteMessage(first, chunk);
	REQUIRE(replica.Receive(store, first.Bytes()) == replication::ApplyStatus::Ok);
	chunk.Bytes[0] ^= std::byte{1};
	core::ByteWriter corrupt;
	replication::WriteMessage(corrupt, chunk);
	CHECK(replica.Receive(store, corrupt.Bytes()) == replication::ApplyStatus::Malformed);
	chunk.Offset = 3;
	chunk.Bytes.assign(definition.Bytes().begin() + 3, definition.Bytes().end());
	core::ByteWriter remaining;
	replication::WriteMessage(remaining, chunk);
	const size_t count = ecs::Components::Count();
	CHECK(replica.Receive(store, remaining.Bytes()) == replication::ApplyStatus::Malformed);
	CHECK(ecs::Components::Count() == count);
	CHECK_FALSE(ecs::Components::Find(chunk.Component).IsValid());
	CHECK_FALSE(store.Alive(ecs::Entity{1}));
	CHECK(replication::ChannelFor(replication::MessageKind::Schemas) == net::ChannelKind::Reliable);
	CHECK(
		replication::QuicRouteFor(replication::MessageKind::Schemas).Channel ==
		replication::QuicRouteFor(replication::MessageKind::SnapshotChunk).Channel
	);
}

TEST_CASE(
	"default attributes honor visibility and local ownership in snapshots and deltas", "[replication][schema]"
) {
	Link link;
	ecs::RegisterAttributeComponents();
	const auto folder =
		ecs::Classes::Register("ReplicationAttributeFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto visible = link.Server.CreateInstance(folder, "visible");
	const auto hidden = link.Server.CreateInstance(folder, "private");
	const auto local = link.Server.CreateInstance(folder, "viewer");
	const auto localChild = link.Server.CreateInstance(folder, "viewer-child");
	const auto predicted = link.Server.CreatePredicted();
	link.Server.Set(local, ecs::ClientLocal{});
	REQUIRE(link.Server.SetParent(localChild, local));
	link.Authority.SetInterest([&](replication::ClientId, ecs::Entity entity, const ecs::Store &) {
		return entity != hidden;
	});
	const auto key = core::Name("Value");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::Double;
	value.Double = 7;
	for (const auto entity : {visible, hidden, local, localChild, predicted}) {
		link.Server.Set(entity, Anchor{1});
		REQUIRE(ecs::SetAttribute(link.Server, entity, key, value));
	}
	ecs::AttributeValue name;
	name.Type = ecs::PropertyType::String;
	name.String = "OrbitCamera";
	REQUIRE(ecs::SetAttribute(link.Server, visible, core::Name("Name"), name));
	ecs::AttributeValue frame;
	frame.Type = ecs::PropertyType::CFrame;
	frame.CFrame.Position = {4, 8, 12};
	REQUIRE(ecs::SetAttribute(link.Server, visible, core::Name("CFrame"), frame));
	link.Steps();
	REQUIRE(link.Replica.Joined());
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.Double == 7);
	REQUIRE(ecs::GetAttribute(link.Client, visible, core::Name("Name"), received));
	CHECK(received.String == "OrbitCamera");
	REQUIRE(ecs::GetAttribute(link.Client, visible, core::Name("CFrame"), received));
	CHECK(received.CFrame.Position == frame.CFrame.Position);
	CHECK(link.Client.Resource<ecs::AttributeTable>()->Entities.size() == 1);
	const auto clientLocal = link.Client.CreatePredicted();
	value.Double = 71;
	REQUIRE(ecs::SetAttribute(link.Client, clientLocal, key, value));
	value.Double = 29;
	for (const auto entity : {visible, hidden, local, localChild, predicted})
		REQUIRE(ecs::SetAttribute(link.Server, entity, key, value));
	link.Steps(8);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.Double == 29);
	CHECK(link.Client.Resource<ecs::AttributeTable>()->Entities.size() == 2);
	REQUIRE(ecs::GetAttribute(link.Client, clientLocal, key, received));
	CHECK(received.Double == 71);
}

TEST_CASE("post-join entities remain auditable with an observed resource", "[replication][schema]") {
	replication::AuthoritySettings settings;
	settings.Audit.Enabled = true;
	settings.Audit.EveryTicks = 1;
	Link link(settings);
	const auto folder =
		ecs::Classes::Register("ReplicationResourceAuditFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto initial = link.Server.CreateInstance(folder, "initial");
	link.Server.Set(initial, Anchor{1});
	const auto key = core::Name("Value");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::Double;
	value.Double = 7;
	REQUIRE(ecs::SetAttribute(link.Server, initial, key, value));
	link.Steps();
	REQUIRE(link.Replica.Joined());
	const auto audits = link.Replica.Stats().Audits;
	link.Server.Destroy(initial);
	const auto arriving = link.Server.CreateInstance(folder, "arriving");
	link.Server.Set(arriving, Anchor{2});
	value.Double = 29;
	REQUIRE(ecs::SetAttribute(link.Server, arriving, key, value));
	link.Delivered.clear();
	link.Steps();
	REQUIRE(link.Client.Alive(arriving));
	CHECK_FALSE(link.Client.Alive(initial));
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, arriving, key, received));
	CHECK(received.Double == 29);
	CHECK(link.Replica.Stats().Audits >= audits + 2);
	bool auditedArrival = false;
	for (const auto &message : link.Delivered) {
		core::ByteReader reader(message);
		replication::Message parsed;
		REQUIRE(replication::ReadMessage(reader, parsed));
		if (parsed.Kind != replication::MessageKind::GroupSignatures) continue;
		for (const auto &group : parsed.Signatures.Groups)
			for (const auto entity : group.Entities)
				auditedArrival |= entity == arriving;
	}
	CHECK(auditedArrival);
}

TEST_CASE(
	"oversized default attributes update reliably without repeating unchanged chunks", "[replication][schema]"
) {
	Link link;
	const auto folder =
		ecs::Classes::Register("ReplicationBulkAttributeFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto visible = link.Server.CreateInstance(folder, "visible");
	const auto hidden = link.Server.CreateInstance(folder, "private");
	link.Server.Set(visible, Anchor{1});
	link.Server.Set(hidden, Anchor{1});
	link.Authority.SetInterest([&](replication::ClientId, ecs::Entity entity, const ecs::Store &) {
		return entity != hidden;
	});
	const auto key = core::Name("Source");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String.assign(8192, 'a');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	value.String.assign(8192, 'h');
	REQUIRE(ecs::SetAttribute(link.Server, hidden, key, value));
	link.Steps(48);
	REQUIRE(link.Replica.Joined());
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == std::string(8192, 'a'));
	CHECK_FALSE(ecs::GetAttribute(link.Client, hidden, key, received));
	const auto local = link.Client.CreatePredicted();
	value.String = "local-input";
	REQUIRE(ecs::SetAttribute(link.Client, local, key, value));
	value.String.assign(8192, 'b');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps(48);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
	REQUIRE(ecs::GetAttribute(link.Client, local, key, received));
	CHECK(received.String == "local-input");
	link.Delivered.clear();
	link.Steps(8);
	for (const auto &message : link.Delivered)
		CHECK(replication::PeekMessageKind(message) != replication::MessageKind::SnapshotChunk);
	// A small update followed by the same large payload must leave the bulk
	// ledger eligible again, rather than treating stale bytes as delivered.
	value.String = "small";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps(8);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == "small");
	value.String.assign(8192, 'b');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps(48);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
}

TEST_CASE("resource overlays remember captured bytes when queued values change", "[replication][schema]") {
	Link link;
	const auto folder =
		ecs::Classes::Register("ReplicationQueuedAttributeFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto visible = link.Server.CreateInstance(folder, "visible");
	link.Server.Set(visible, Anchor{1});
	const auto key = core::Name("Source");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String = "initial";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps();
	REQUIRE(link.Replica.Joined());
	// Queue A without a chunk, then capture S on the very next tick.
	value.String.assign(8192, 'a');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Delivered.clear();
	link.Step();
	for (const auto &message : link.Delivered)
		CHECK(replication::PeekMessageKind(message) != replication::MessageKind::SnapshotChunk);
	value.String = "small";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Step();
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == "small");
	// Restoring A before any ordinary small delta must queue a fresh overlay.
	value.String.assign(8192, 'a');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps(48);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
	link.Delivered.clear();
	link.Steps(8);
	for (const auto &message : link.Delivered)
		CHECK(replication::PeekMessageKind(message) != replication::MessageKind::SnapshotChunk);
}

TEST_CASE("resource freshness survives reordered snapshot and delta channels", "[replication][schema]") {
	Link link;
	ecs::Components::Register<BulkValue>(
		"replication_dynamic.BulkValue",
		[](core::ByteWriter &writer, const void *values, size_t count) {
			const auto *rows = static_cast<const BulkValue *>(values);
			for (size_t index = 0; index < count; ++index)
				writer.WriteString(rows[index].Text);
		},
		[](core::ByteReader &reader, void *values, size_t count) {
			auto *rows = static_cast<BulkValue *>(values);
			for (size_t index = 0; index < count; ++index)
				rows[index].Text = reader.ReadString();
		},
		16384
	);
	link.Authority.Replicate(
		core::Name("replication_dynamic.BulkValue"), replication::ChangeDetection::Observed
	);
	const auto folder =
		ecs::Classes::Register("ReplicationOrderedAttributeFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto visible = link.Server.CreateInstance(folder, "visible");
	link.Server.Set(visible, Anchor{1});
	link.Server.Set(visible, BulkValue{"initial"});
	const auto key = core::Name("Source");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String = "initial";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps();
	REQUIRE(link.Replica.Joined());
	value.String.assign(8192, 'a');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Server.Set(visible, BulkValue{std::string(8192, 'e')});
	link.Step();
	std::vector<std::vector<std::byte>> delayedChunks;
	const auto publish = [&](bool holdChunks, bool holdDeltas, std::vector<std::vector<std::byte>> &held) {
		link.Authority.Publish(link.Server, ++link.Tick);
		for (const auto &message : link.Authority.Outgoing(link.Peer)) {
			const auto kind = replication::PeekMessageKind(message);
			if ((holdChunks && kind == replication::MessageKind::SnapshotChunk) ||
				(holdDeltas && kind == replication::MessageKind::Delta))
				held.push_back(message);
			else
				REQUIRE(link.Replica.Receive(link.Client, message) == replication::ApplyStatus::Ok);
		}
		link.Server.ClearChanges();
		const auto acknowledgement = link.Replica.Acknowledge();
		if (!acknowledgement.empty()) REQUIRE(link.Authority.Receive(link.Peer, acknowledgement));
	};
	for (size_t ticks = 0; ticks < 48; ++ticks) {
		publish(true, false, delayedChunks);
		if (!link.Authority.StatusOf(link.Peer).Streaming) break;
	}
	REQUIRE_FALSE(delayedChunks.empty());
	REQUIRE_FALSE(link.Authority.StatusOf(link.Peer).Streaming);
	value.String = "newer-small";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Step();
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
	for (const auto &message : delayedChunks)
		REQUIRE(link.Replica.Receive(link.Client, message) == replication::ApplyStatus::Ok);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == "newer-small");
	REQUIRE(link.Client.Get<BulkValue>(visible) != nullptr);
	CHECK(link.Client.Get<BulkValue>(visible)->Text == std::string(8192, 'e'));
	// Reverse the channels: withhold an old small resource delta, accept the
	// later reliable resource overlay, then deliver that stale resource row.
	std::vector<std::vector<std::byte>> delayedDeltas;
	value.String = "older-small";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	publish(false, true, delayedDeltas);
	REQUIRE_FALSE(delayedDeltas.empty());
	value.String.assign(8192, 'c');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	publish(false, true, delayedDeltas);
	for (size_t ticks = 0; ticks < 48; ++ticks) {
		publish(false, true, delayedDeltas);
		if (!link.Authority.StatusOf(link.Peer).Streaming) break;
	}
	REQUIRE_FALSE(link.Authority.StatusOf(link.Peer).Streaming);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
	for (const auto &message : delayedDeltas) {
		const auto status = link.Replica.Receive(link.Client, message);
		CHECK((status == replication::ApplyStatus::Ok || status == replication::ApplyStatus::Stale));
	}
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
}

TEST_CASE(
	"a completed resource write retains freshness when a later delta row is malformed",
	"[replication][schema]"
) {
	Link link;
	const auto folder =
		ecs::Classes::Register("ReplicationPartialAttributeFolder", ecs::Classes::RegisterInstanceRoot(), {});
	for (const auto &component : replication::DefaultReplicatedComponents())
		link.Authority.Replicate(core::Name(component.Name), component.Detection, component.Resource);
	const auto visible = link.Server.CreateInstance(folder, "visible");
	link.Server.Set(visible, Anchor{1});
	const auto key = core::Name("Source");
	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String = "initial";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Steps();
	REQUIRE(link.Replica.Joined());
	value.String.assign(8192, 'o');
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	link.Step();
	std::vector<std::vector<std::byte>> delayedChunks;
	for (size_t ticks = 0; ticks < 48; ++ticks) {
		link.Authority.Publish(link.Server, ++link.Tick);
		for (const auto &message : link.Authority.Outgoing(link.Peer)) {
			if (replication::PeekMessageKind(message) == replication::MessageKind::SnapshotChunk)
				delayedChunks.push_back(message);
			else
				REQUIRE(link.Replica.Receive(link.Client, message) == replication::ApplyStatus::Ok);
		}
		link.Server.ClearChanges();
		if (!link.Authority.StatusOf(link.Peer).Streaming) break;
	}
	REQUIRE_FALSE(delayedChunks.empty());
	REQUIRE_FALSE(link.Authority.StatusOf(link.Peer).Streaming);
	value.String = "accepted-before-malformed-row";
	REQUIRE(ecs::SetAttribute(link.Server, visible, key, value));
	const auto selected = ecs::SelectAttributes(link.Server, std::array{visible});
	core::ByteWriter resource;
	ecs::Components::Describe(ecs::Components::Assigned<ecs::AttributeTable>()).Write(resource, &selected, 1);
	replication::Delta malformed;
	malformed.Tick = link.Tick + 1;
	malformed.Final = true;
	malformed.Components.push_back(
		{core::Name("ecs.AttributeTable"), {ecs::NULL_ENTITY}, resource.TakeBytes()}
	);
	malformed.Components.push_back({core::Name("replication_dynamic.Anchor"), {visible}, {std::byte{0}}});
	core::ByteWriter message;
	replication::WriteMessage(message, malformed);
	REQUIRE(link.Replica.Receive(link.Client, message.Bytes()) == replication::ApplyStatus::Malformed);
	ecs::AttributeValue received;
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
	CHECK(link.Client.Get<Anchor>(visible)->Value == 1);
	for (const auto &chunk : delayedChunks)
		REQUIRE(link.Replica.Receive(link.Client, chunk) == replication::ApplyStatus::Ok);
	REQUIRE(ecs::GetAttribute(link.Client, visible, key, received));
	CHECK(received.String == value.String);
}

#if defined(__linux__)
TEST_CASE(
	"a cold process receives an unknown schema before its authoritative rows",
	"[replication][schema][process]"
) {
	constexpr const char *COMPONENT = "replication_dynamic.ColdState";
	if (const char *input = std::getenv("ATOMIC_SCHEMA_COLD_INPUT")) {
		REQUIRE_FALSE(ecs::Components::Find(core::Name(COMPONENT)).IsValid());
		std::ifstream file(input, std::ios::binary);
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
		REQUIRE_FALSE(bytes.empty());
		core::ByteReader reader({reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
		const ecs::Entity entity{reader.ReadUInt64()};
		const uint32_t count = reader.ReadUInt32();
		ecs::Store world{"cold-replica"};
		replication::Replica replica;
		for (uint32_t index = 0; index < count; ++index) {
			const uint32_t size = reader.ReadUInt32();
			std::vector<std::byte> message(size);
			reader.ReadRaw(message.data(), message.size());
			REQUIRE_FALSE(reader.Failed());
			REQUIRE(replica.Receive(world, message) == replication::ApplyStatus::Ok);
		}
		REQUIRE(replica.Joined());
		const auto id = ecs::Components::Find(core::Name(COMPONENT));
		REQUIRE(id.IsValid());
		CHECK(Energy(world, entity, id) == 72);
		return;
	}
	Link link;
	const auto state = Declare(COMPONENT);
	const auto entity = link.Server.Create();
	Set(link.Server, entity, state, 72);
	link.Steps();
	REQUIRE(link.Replica.Joined());
	core::ByteWriter recording;
	recording.WriteUInt64(entity.Id);
	recording.WriteUInt32(static_cast<uint32_t>(link.Delivered.size()));
	for (const auto &message : link.Delivered) {
		recording.WriteUInt32(static_cast<uint32_t>(message.size()));
		recording.WriteRaw(message.data(), message.size());
	}
	const auto path =
		std::filesystem::temp_directory_path() / ("atomic-schema-cold-" + std::to_string(getpid()) + ".bin");
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::filesystem::remove(Path);
		}
	} cleanup{path};
	{
		std::ofstream output(path, std::ios::binary);
		output.write(
			reinterpret_cast<const char *>(recording.Bytes().data()),
			static_cast<std::streamsize>(recording.Size())
		);
		REQUIRE(output.good());
	}
	std::vector<std::string> environment;
	for (char **entry = environ; *entry != nullptr; ++entry)
		environment.emplace_back(*entry);
	environment.push_back("ATOMIC_SCHEMA_COLD_INPUT=" + path.string());
	std::vector<char *> environmentPointers;
	for (auto &entry : environment)
		environmentPointers.push_back(entry.data());
	environmentPointers.push_back(nullptr);
	std::array<std::string, 4> arguments{
		std::filesystem::read_symlink("/proc/self/exe").string(),
		"a cold process receives an unknown schema before its authoritative rows",
		"--reporter",
		"compact"
	};
	std::vector<char *> argumentPointers;
	for (auto &argument : arguments)
		argumentPointers.push_back(argument.data());
	argumentPointers.push_back(nullptr);
	pid_t child = 0;
	REQUIRE(
		posix_spawn(
			&child,
			arguments[0].c_str(),
			nullptr,
			nullptr,
			argumentPointers.data(),
			environmentPointers.data()
		) == 0
	);
	int status = 0;
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 0);
}
#endif
