#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.script.bakedproceduralreference")
TEST_DEPENDS("engine.script.proceduralscene")

using namespace engine;

namespace {
	struct BakedTarget {
		ecs::Entity Mesh{};
		ecs::Entity Part{};
	};
	BakedTarget MakeBakedTarget(ecs::Store &store) {
		const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Region");
		scene::PartDesc description;
		description.Class = ecs::Classes::Find(core::Name("MeshPart"));
		description.Mesh = scene::EditableMeshContentName(store, mesh);
		const auto part = scene::MakePart(store, description);
		REQUIRE(part != ecs::NULL_ENTITY);
		auto *collider = store.GetMutable<scene::Collider>(part);
		collider->Mask = spatial::LayerMask::None();
		collider->CanQuery = false;
		return {mesh, part};
	}
	using DocumentRecord = std::pair<uint32_t, script::BakedProceduralReference>;
	std::vector<uint8_t> BakedPacket(std::span<const DocumentRecord> records, uint32_t version = 1) {
		const auto &type =
			ecs::Components::Describe(ecs::Components::Assigned<script::BakedProceduralReference>());
		core::ByteWriter writer;
		writer.WriteUInt32(version);
		writer.WriteUInt32(static_cast<uint32_t>(records.size()));
		for (const auto &[mesh, reference] : records) {
			writer.WriteUInt32(mesh);
			type.Write(writer, &reference, 1);
		}
		std::vector<uint8_t> bytes;
		for (const auto byte : writer.Bytes())
			bytes.push_back(std::to_integer<uint8_t>(byte));
		return bytes;
	}
}

TEST_CASE("default baked reference survives a snapshot without becoming a demand", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store source("default-baked-reference");
	const auto mesh = source.CreateInstance(scene::EditableMeshClass(), "Region");
	source.Set(mesh, script::BakedProceduralReference{});

	core::ByteWriter snapshot;
	REQUIRE(source.Save(snapshot));
	core::ByteReader reader(snapshot.Bytes());
	ecs::Store restored("restored-default-reference");
	REQUIRE(restored.Load(reader));
	const auto *reference = restored.Get<script::BakedProceduralReference>(mesh);
	REQUIRE(reference);
	CHECK_FALSE(reference->Description.IsValid());
	CHECK(reference->Scope.X == 0);
	CHECK(reference->Scope.Y == 0);
	CHECK(reference->Scope.CellsPerSide == procedural::RegionScope{}.CellsPerSide);
	CHECK(reference->Part == ecs::NULL_ENTITY);
	CHECK(reference->Revision == 0);
}

TEST_CASE(
	"baked reference reader rejects partially configured rows without changing the destination",
	"[procedural][content]"
) {
	script::RegisterProceduralComponents();
	const auto &type =
		ecs::Components::Describe(ecs::Components::Assigned<script::BakedProceduralReference>());
	const script::BakedProceduralReference good{core::Name("town/region.aproc"), {}, ecs::Entity{1}, 1};
	const std::array partial{
		script::BakedProceduralReference{{}, {}, {}, 1},
		script::BakedProceduralReference{core::Name("town/region.aproc"), {}, {}, 1},
		script::BakedProceduralReference{core::Name("town/region.aproc"), {}, ecs::Entity{1}, 0}
	};
	for (const auto &value : partial) {
		core::ByteWriter bytes;
		type.Write(bytes, &value, 1);
		core::ByteReader reader(bytes.Bytes());
		auto destination = good;
		type.Read(reader, &destination, 1);
		CHECK(reader.Failed());
		CHECK(destination.Description == good.Description);
		CHECK(destination.Part == good.Part);
		CHECK(destination.Revision == good.Revision);
	}
}

TEST_CASE("baked reference saves its content name, region and same-world part", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store source("baked-reference");
	const auto mesh = source.CreateInstance(scene::EditableMeshClass(), "Region");
	scene::PartDesc description;
	description.Class = ecs::Classes::Find(core::Name("MeshPart"));
	description.Mesh = scene::EditableMeshContentName(source, mesh);
	const auto part = scene::MakePart(source, description);
	REQUIRE(part != ecs::NULL_ENTITY);
	auto *collider = source.GetMutable<scene::Collider>(part);
	collider->Mask = spatial::LayerMask::None();
	collider->CanQuery = false;
	const procedural::RegionScope scope{3, -2, 16};
	procedural::Diagnostic diagnostic;
	REQUIRE(script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic));
	const auto *reference = source.Get<script::BakedProceduralReference>(mesh);
	REQUIRE(reference);
	CHECK(reference->Description.Text() == "town/region.aproc");
	CHECK(reference->Part == part);
	CHECK(reference->Scope.X == 3);
	CHECK(reference->Scope.Y == -2);
	CHECK(reference->Revision == 1);
	CHECK_FALSE(
		script::SetBakedProceduralReference(source, scope, mesh, part, "../region.aproc", diagnostic)
	);
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Description.Text() == "town/region.aproc");
	REQUIRE(script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic));
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Revision == 2);

	core::ByteWriter bytes;
	REQUIRE(source.Save(bytes));
	core::ByteReader reader(bytes.Bytes());
	ecs::Store restored("restored-reference");
	REQUIRE(restored.Load(reader));
	const auto *saved = restored.Get<script::BakedProceduralReference>(mesh);
	REQUIRE(saved);
	CHECK(saved->Description.Text() == "town/region.aproc");
	CHECK(saved->Part == part);
	CHECK(saved->Scope.X == scope.X);
	CHECK(saved->Scope.Y == scope.Y);
	CHECK(saved->Scope.CellsPerSide == scope.CellsPerSide);
	CHECK(saved->Revision == 2);
	auto terminal = *source.Get<script::BakedProceduralReference>(mesh);
	terminal.Revision = std::numeric_limits<uint64_t>::max();
	source.Set(mesh, terminal);
	CHECK_FALSE(
		script::SetBakedProceduralReference(source, scope, mesh, part, "town/region.aproc", diagnostic)
	);
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
	CHECK(source.Get<script::BakedProceduralReference>(mesh)->Revision == terminal.Revision);
}

TEST_CASE("baked reference admission caps one world's region count", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store store("bounded-baked-references");
	const procedural::RegionScope scope{0, 0, 16};
	for (size_t index = 0; index < script::MAXIMUM_BAKED_PROCEDURAL_REFERENCES; ++index) {
		const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Region");
		store.Set(mesh, script::BakedProceduralReference{core::Name("town/region.aproc"), scope, mesh, 1});
	}
	const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Overflow");
	scene::PartDesc description;
	description.Class = ecs::Classes::Find(core::Name("MeshPart"));
	description.Mesh = scene::EditableMeshContentName(store, mesh);
	const auto part = scene::MakePart(store, description);
	REQUIRE(part != ecs::NULL_ENTITY);
	procedural::Diagnostic diagnostic;
	CHECK_FALSE(
		script::SetBakedProceduralReference(store, scope, mesh, part, "town/region.aproc", diagnostic)
	);
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
	CHECK_FALSE(store.Has<script::BakedProceduralReference>(mesh));
}

TEST_CASE("baked document remaps IDs and restores fresh local bindings", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store source("baked-document-source");
	const auto first = MakeBakedTarget(source);
	const auto second = MakeBakedTarget(source);
	procedural::Diagnostic diagnostic;
	REQUIRE(
		script::SetBakedProceduralReference(
			source, {3, -2, 16}, first.Mesh, first.Part, "town/first.aproc", diagnostic
		)
	);
	REQUIRE(
		script::SetBakedProceduralReference(
			source, {7, 4, 8}, second.Mesh, second.Part, "town/second.aproc", diagnostic
		)
	);
	REQUIRE(
		script::SetBakedProceduralReference(
			source, {3, -2, 16}, first.Mesh, first.Part, "town/first.aproc", diagnostic
		)
	);
	const auto numbering = [&](ecs::Entity entity) -> uint32_t {
		if (entity == first.Mesh) return 31;
		if (entity == first.Part) return 11;
		if (entity == second.Mesh) return 7;
		if (entity == second.Part) return 12;
		return 0;
	};
	std::vector<uint8_t> bytes;
	REQUIRE(script::EncodeBakedProceduralReferences(source, numbering, bytes, diagnostic));
	REQUIRE(bytes.size() <= script::MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES);
	core::ByteReader reader(std::as_bytes(std::span(bytes)));
	CHECK(reader.ReadUInt32() == 1);
	CHECK(reader.ReadUInt32() == 2);
	const auto &type =
		ecs::Components::Describe(ecs::Components::Assigned<script::BakedProceduralReference>());
	CHECK(reader.ReadUInt32() == 7);
	script::BakedProceduralReference decoded;
	type.Read(reader, &decoded, 1);
	CHECK(decoded.Description.Text() == "town/second.aproc");
	CHECK(decoded.Part.Id == 12);
	CHECK(reader.ReadUInt32() == 31);
	type.Read(reader, &decoded, 1);
	CHECK(decoded.Part.Id == 11);
	CHECK(decoded.Revision == 2);
	REQUIRE(reader.AtEnd());

	ecs::Store restored("baked-document-restored");
	restored.CreateInstance(scene::EditableMeshClass(), "Unrelated");
	const auto restoredSecond = MakeBakedTarget(restored);
	const auto restoredFirst = MakeBakedTarget(restored);
	const core::Name stale("editable-mesh://999999");
	restored.GetMutable<scene::Visual>(restoredFirst.Part)->Mesh = stale;
	restored.GetMutable<scene::Visual>(restoredSecond.Part)->Mesh = stale;
	restored.GetMutable<scene::Collider>(restoredFirst.Part)->Mask = spatial::LayerMask::All();
	restored.GetMutable<scene::Collider>(restoredFirst.Part)->Trigger = false;
	restored.GetMutable<scene::Collider>(restoredSecond.Part)->Mask = spatial::LayerMask::All();
	restored.GetMutable<scene::Collider>(restoredSecond.Part)->Trigger = true;
	const auto instances = [&](uint32_t id) {
		if (id == 31) return restoredFirst.Mesh;
		if (id == 11) return restoredFirst.Part;
		if (id == 7) return restoredSecond.Mesh;
		if (id == 12) return restoredSecond.Part;
		return ecs::NULL_ENTITY;
	};
	script::ProceduralSceneTarget rejectedLiveTarget;
	restored.GetMutable<scene::Visual>(restoredFirst.Part)->Mesh =
		scene::EditableMeshContentName(restored, restoredFirst.Mesh);
	CHECK_FALSE(
		script::CaptureProceduralSceneTarget(
			restored, {}, restoredFirst.Mesh, restoredFirst.Part, rejectedLiveTarget, diagnostic
		)
	);
	restored.GetMutable<scene::Visual>(restoredFirst.Part)->Mesh = stale;
	REQUIRE(script::DecodeBakedProceduralReferences(restored, bytes, instances, diagnostic));
	const auto *reference = restored.Get<script::BakedProceduralReference>(restoredFirst.Mesh);
	REQUIRE(reference);
	CHECK(reference->Description.Text() == "town/first.aproc");
	CHECK(reference->Scope.X == 3);
	CHECK(reference->Scope.Y == -2);
	CHECK(reference->Scope.CellsPerSide == 16);
	CHECK(reference->Part == restoredFirst.Part);
	CHECK(reference->Revision == 1);
	for (const auto target : {restoredFirst, restoredSecond}) {
		const auto content = scene::EditableMeshContentName(restored, target.Mesh);
		CHECK(restored.Get<scene::Visual>(target.Part)->Mesh == content);
		CHECK(restored.Get<scene::Visual>(target.Part)->Fitted == content);
		CHECK(restored.Get<scene::Collider>(target.Part)->Mask == spatial::LayerMask::None());
		CHECK_FALSE(restored.Get<scene::Collider>(target.Part)->CanQuery);
	}
	CHECK_FALSE(restored.Get<scene::Collider>(restoredFirst.Part)->Trigger);
	CHECK(restored.Get<scene::Collider>(restoredSecond.Part)->Trigger);
}

TEST_CASE(
	"baked document omits inert and unmapped rows but refuses authored partial demands",
	"[procedural][content]"
) {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store store("baked-document-selection");
	const auto target = MakeBakedTarget(store);
	store.Set(target.Mesh, script::BakedProceduralReference{});
	procedural::Diagnostic diagnostic;
	std::vector<uint8_t> bytes{9};
	const auto numbered = [&](ecs::Entity entity) -> uint32_t { return entity == target.Mesh ? 1 : 2; };
	REQUIRE(script::EncodeBakedProceduralReferences(store, numbered, bytes, diagnostic));
	CHECK(bytes.empty());
	store.Set(target.Mesh, script::BakedProceduralReference{{}, {}, {}, 1});
	REQUIRE(
		script::EncodeBakedProceduralReferences(
			store, [](ecs::Entity) { return uint32_t{0}; }, bytes, diagnostic
		)
	);
	CHECK(bytes.empty());
	CHECK_FALSE(script::EncodeBakedProceduralReferences(store, numbered, bytes, diagnostic));
	REQUIRE(
		script::SetBakedProceduralReference(
			store, {}, target.Mesh, target.Part, "town/region.aproc", diagnostic
		)
	);
	CHECK_FALSE(
		script::EncodeBakedProceduralReferences(
			store,
			[&](ecs::Entity entity) -> uint32_t { return entity == target.Mesh ? 1 : 0; },
			bytes,
			diagnostic
		)
	);
	store.GetMutable<scene::Visual>(target.Part)->Transparency = 0.5f;
	CHECK_FALSE(script::EncodeBakedProceduralReferences(store, numbered, bytes, diagnostic));
}

TEST_CASE(
	"baked document refuses malformed packets without changing active demands or bindings",
	"[procedural][content]"
) {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store store("baked-document-rollback");
	const auto first = MakeBakedTarget(store);
	const auto second = MakeBakedTarget(store);
	procedural::Diagnostic diagnostic;
	REQUIRE(
		script::SetBakedProceduralReference(
			store, {1, 2, 16}, first.Mesh, first.Part, "town/old.aproc", diagnostic
		)
	);
	const auto prior = *store.Get<script::BakedProceduralReference>(first.Mesh);
	const core::Name stale("editable-mesh://old-store");
	store.GetMutable<scene::Visual>(first.Part)->Mesh = stale;
	store.GetMutable<scene::Visual>(first.Part)->Fitted = stale;
	store.GetMutable<scene::Visual>(second.Part)->Mesh = stale;
	store.GetMutable<scene::Collider>(first.Part)->Mask = spatial::LayerMask::All();
	store.GetMutable<scene::Collider>(first.Part)->Trigger = false;
	store.GetMutable<scene::Collider>(second.Part)->Mask = spatial::LayerMask::All();
	store.GetMutable<scene::Collider>(second.Part)->Trigger = true;
	const script::BakedProceduralReference reference{
		core::Name("town/new.aproc"), {4, 5, 8}, ecs::Entity{2}, 91
	};
	const std::array goodRecords{
		DocumentRecord{1, reference}, DocumentRecord{3, {reference.Description, {7, 8, 8}, ecs::Entity{4}, 5}}
	};
	const auto valid = BakedPacket(goodRecords);
	const auto instances = [&](uint32_t id) {
		if (id == 1) return first.Mesh;
		if (id == 2) return first.Part;
		if (id == 3) return second.Mesh;
		if (id == 4) return second.Part;
		return ecs::NULL_ENTITY;
	};
	std::vector<std::vector<uint8_t>> malformed;
	malformed.push_back(valid);
	malformed.back().pop_back();
	malformed.push_back(valid);
	malformed.back().push_back(0);
	malformed.push_back(BakedPacket(goodRecords, 2));
	for (const auto partId : {uint64_t{0}, uint64_t{999}, uint64_t{1} << 32}) {
		auto records = goodRecords;
		records[1].second.Part = ecs::Entity{partId};
		malformed.push_back(BakedPacket(records));
	}
	for (const auto meshId : {uint32_t{0}, uint32_t{1}, uint32_t{999}}) {
		auto records = goodRecords;
		records[1].first = meshId;
		malformed.push_back(BakedPacket(records));
	}
	{
		auto records = goodRecords;
		records[1].second.Part = ecs::Entity{2};
		malformed.push_back(BakedPacket(records));
		records[1].second = {};
		malformed.push_back(BakedPacket(records));
	}
	malformed.emplace_back(script::MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES + 1, 0);
	for (const auto &packet : malformed) {
		CHECK_FALSE(script::DecodeBakedProceduralReferences(store, packet, instances, diagnostic));
		const auto *retained = store.Get<script::BakedProceduralReference>(first.Mesh);
		REQUIRE(retained);
		CHECK(retained->Description == prior.Description);
		CHECK(retained->Part == prior.Part);
		CHECK(retained->Revision == prior.Revision);
		CHECK(retained->Scope.X == prior.Scope.X);
		CHECK(store.Get<scene::Visual>(first.Part)->Mesh == stale);
		CHECK(store.Get<scene::Visual>(first.Part)->Fitted == stale);
		CHECK_FALSE(store.Has<script::BakedProceduralReference>(second.Mesh));
		CHECK(store.Get<scene::Visual>(second.Part)->Mesh == stale);
		CHECK(store.Get<scene::Collider>(first.Part)->Mask == spatial::LayerMask::All());
		CHECK(store.Get<scene::Collider>(second.Part)->Mask == spatial::LayerMask::All());
		CHECK_FALSE(store.Get<scene::Collider>(first.Part)->Trigger);
		CHECK(store.Get<scene::Collider>(second.Part)->Trigger);
	}
	store.GetMutable<scene::Visual>(second.Part)->Tint = {0.5f, 1, 1};
	CHECK_FALSE(script::DecodeBakedProceduralReferences(store, valid, instances, diagnostic));
	CHECK(store.Get<scene::Visual>(first.Part)->Mesh == stale);
	CHECK(store.Get<script::BakedProceduralReference>(first.Mesh)->Description == prior.Description);
	CHECK(store.Get<scene::Collider>(first.Part)->Mask == spatial::LayerMask::All());
	CHECK(store.Get<scene::Collider>(second.Part)->Mask == spatial::LayerMask::All());
	store.GetMutable<scene::Visual>(second.Part)->Tint = {1, 1, 1};
	store.GetMutable<scene::Collider>(second.Part)->CanQuery = true;
	CHECK_FALSE(script::DecodeBakedProceduralReferences(store, valid, instances, diagnostic));
	CHECK(store.Get<scene::Visual>(first.Part)->Mesh == stale);
	CHECK(store.Get<scene::Collider>(first.Part)->Mask == spatial::LayerMask::All());
	CHECK(store.Get<scene::Collider>(second.Part)->Mask == spatial::LayerMask::All());
	CHECK(store.Get<scene::Collider>(second.Part)->CanQuery);
	const auto empty = BakedPacket({});
	REQUIRE(script::DecodeBakedProceduralReferences(store, empty, instances, diagnostic));
	CHECK(store.Get<scene::Visual>(first.Part)->Mesh == stale);
}

TEST_CASE("baked document bounds imported and retained demands", "[procedural][content]") {
	scene::RegisterSceneComponents();
	script::RegisterProceduralComponents();
	ecs::Store store("baked-document-capacity");
	const auto target = MakeBakedTarget(store);
	const script::BakedProceduralReference reference{core::Name("town/region.aproc"), {}, ecs::Entity{2}, 1};
	const std::array records{DocumentRecord{1, reference}};
	const auto packet = BakedPacket(records);
	const auto instances = [&](uint32_t id) { return id == 1 ? target.Mesh : target.Part; };
	for (size_t index = 0; index < script::MAXIMUM_BAKED_PROCEDURAL_REFERENCES; ++index) {
		const auto mesh = store.CreateInstance(scene::EditableMeshClass(), "Retained");
		store.Set(mesh, reference);
	}
	procedural::Diagnostic diagnostic;
	CHECK_FALSE(script::DecodeBakedProceduralReferences(store, packet, instances, diagnostic));
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
	CHECK_FALSE(store.Has<script::BakedProceduralReference>(target.Mesh));
	store.Set(target.Mesh, reference);
	CHECK_FALSE(script::DecodeBakedProceduralReferences(store, packet, instances, diagnostic));
	ecs::Entity removed{};
	store.Each<const script::BakedProceduralReference>([&](ecs::Entity mesh,
														   const script::BakedProceduralReference &) {
		if (mesh != target.Mesh) removed = mesh;
	});
	store.Remove<script::BakedProceduralReference>(removed);
	REQUIRE(script::DecodeBakedProceduralReferences(store, packet, instances, diagnostic));
	CHECK(store.Get<script::BakedProceduralReference>(target.Mesh)->Part == target.Part);
	std::vector<DocumentRecord> tooMany(script::MAXIMUM_BAKED_PROCEDURAL_REFERENCES + 1, records.front());
	CHECK_FALSE(script::DecodeBakedProceduralReferences(store, BakedPacket(tooMany), instances, diagnostic));
	CHECK(diagnostic.Code == procedural::Status::LimitExceeded);
}
