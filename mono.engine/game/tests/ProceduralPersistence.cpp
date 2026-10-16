#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/procedural/Region.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralInputs.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.game.proceduralpersistence")
TEST_DEPENDS("engine.game.roundtrip")
TEST_DEPENDS("engine.script.proceduralinputs")
TEST_DEPENDS("engine.script.bakedproceduralreference")

namespace {
	using namespace engine;

	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::array<procedural::RegionScope, 2> SCOPES{{{0, 0, 16}, {20, 0, 16}}};
	constexpr procedural::RegionScope BAKED_SCOPE{3, -2, 16};

	struct BakedTargets {
		ecs::Entity Mesh{};
		ecs::Entity Part{};
	};

	bool AuthorBakedReference(ecs::Store &store, BakedTargets &out) {
		out.Mesh = store.CreateInstance(scene::EditableMeshClass(), "BakedRegion");
		if (out.Mesh == ecs::NULL_ENTITY) return false;
		scene::PartDesc description;
		description.Class = ecs::Classes::Find(core::Name("MeshPart"));
		description.Mesh = scene::EditableMeshContentName(store, out.Mesh);
		out.Part = scene::MakePart(store, description);
		if (out.Part == ecs::NULL_ENTITY || !store.SetInstanceName(out.Part, "BakedPart")) return false;
		auto *collider = store.GetMutable<scene::Collider>(out.Part);
		if (collider == nullptr) return false;
		collider->Mask = spatial::LayerMask::None();
		collider->CanQuery = false;
		procedural::Diagnostic diagnostic;
		return script::SetBakedProceduralReference(
			store, BAKED_SCOPE, out.Mesh, out.Part, "town/region.aproc", diagnostic
		);
	}

	struct ScratchDirectory {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("engine-game-procedural-persistence-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ScratchDirectory() {
			std::filesystem::create_directories(Path);
		}
		~ScratchDirectory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};

	world::WorldId AddWorld(world::Universe &universe, core::Name name) {
		world::WorldSettings settings;
		settings.Name = name;
		return universe.Create(settings);
	}

	bool Configure(ecs::Store &store, procedural::Diagnostic &diagnostic) {
		const std::array<script::ProceduralSource, 2> sources{{
			{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
			{procedural::SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead"},
		}};
		if (!script::ConfigureProceduralWorld(
				store,
				"persistence",
				procedural::ForestVillageDefaults("forest-village", 42),
				SCOPES,
				sources,
				diagnostic
			))
			return false;
		if (!script::SetProceduralSpline(
				store, {"west-road", "west-gate", {10 * CELL, 8 * CELL}}, diagnostic
			))
			return false;
		if (!script::SetProceduralOverride(
				store,
				{"tree/1/1", procedural::OverrideAction::Replace, {328 * CELL, 2 * CELL}, 1, 0, 0, ""},
				diagnostic
			))
			return false;
		store.CreateInstance(scene::PartClass(), "KeepMe");
		return true;
	}

	struct AuthoritySnapshot {
		std::vector<uint8_t> Bytes;
		std::array<std::vector<uint8_t>, SCOPES.size()> RegionKeys;
		std::array<uint64_t, SCOPES.size()> Revisions{};
		uint64_t Incarnation = 0;
	};

	bool Capture(ecs::Store &store, AuthoritySnapshot &out, procedural::Diagnostic &diagnostic) {
		if (!script::EncodeProceduralAuthority(store, out.Bytes, diagnostic)) return false;
		const auto *authority = store.Resource<script::ProceduralAuthority>();
		if (authority == nullptr || authority->Regions.size() != SCOPES.size()) return false;
		out.Incarnation = authority->Incarnation;
		for (size_t index = 0; index < SCOPES.size(); ++index) {
			out.Revisions[index] = authority->Regions[index].SourceRevision;
			procedural::CompiledRecipe recipe;
			procedural::RegionRequest request;
			if (!script::CaptureProceduralRegion(store, SCOPES[index], recipe, request, diagnostic))
				return false;
			if (procedural::BuildRegionKey(recipe, request, out.RegionKeys[index], diagnostic) !=
				procedural::Status::Ok)
				return false;
		}
		return true;
	}

	bool CaptureWorld(world::Universe &universe, world::WorldId worldId, AuthoritySnapshot &out) {
		procedural::Diagnostic diagnostic;
		bool captured = false;
		const auto status =
			universe.Enter(worldId, [&](ecs::Store &store) { captured = Capture(store, out, diagnostic); });
		return status == world::WorldStatus::Ok && captured;
	}

	bool PublishOne(ecs::Store &store, procedural::RegionScope scope) {
		script::ProceduralJobs jobs(store);
		const uint64_t ticket = jobs.Submit(store, scope, parallel::JobContext::Serial);
		if (ticket == 0) return false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (jobs.Completions().empty() && std::chrono::steady_clock::now() < deadline) {
			jobs.Poll();
			if (jobs.Completions().empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (jobs.Completions().size() != 1 ||
			jobs.Completions()[0].Status != script::ProceduralJobStatus::Ready)
			return false;
		return jobs.PublishBatch(store, std::array{ticket}, store.Time().Tick);
	}

	std::string AddOptionalBlock(
		std::string document,
		std::string_view name,
		std::string_view payload,
		std::string_view encoding = "hex"
	) {
		const auto close = document.rfind("</World>");
		if (close == std::string::npos) return {};
		document.insert(
			close,
			"<" + std::string(name) + " encoding=\"" + std::string(encoding) + "\"><![CDATA[" +
				std::string(payload) + "]]></" + std::string(name) + ">"
		);
		return document;
	}

	std::string AddAuthorityBlock(std::string document, std::string_view payload) {
		return AddOptionalBlock(std::move(document), "ProceduralAuthority", payload);
	}

	bool HasAuthority(world::Universe &universe, world::WorldId worldId) {
		bool found = false;
		if (universe.Enter(worldId, [&](ecs::Store &store) {
				found = store.Resource<script::ProceduralAuthority>() != nullptr;
			}) != world::WorldStatus::Ok)
			return false;
		return found;
	}

	bool HasPublication(world::Universe &universe, world::WorldId worldId) {
		bool found = false;
		if (universe.Enter(worldId, [&](ecs::Store &store) {
				const auto *published = store.Resource<script::ProceduralPublished>();
				found = published != nullptr && !published->Regions.empty();
			}) != world::WorldStatus::Ok)
			return false;
		return found;
	}

	bool ReadFileText(const std::filesystem::path &path, std::string &out) {
		std::ifstream file(path, std::ios::binary);
		if (!file) return false;
		out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		return file.good() || file.eof();
	}
}

TEST_CASE(
	"procedural authored authority survives game save and load without accepted output", "[game][procedural]"
) {
	game::RegisterGameClasses();
	world::Universe source;
	const auto original = AddWorld(source, core::Name("Procedural"));
	REQUIRE(original.IsValid());
	bool configured = false;
	procedural::Diagnostic diagnostic;
	REQUIRE(source.Enter(original, [&](ecs::Store &store) {
		configured = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	AuthoritySnapshot expected;
	REQUIRE(CaptureWorld(source, original, expected));
	REQUIRE(expected.Incarnation != 0);
	CHECK(expected.Revisions[0] > 1);
	CHECK(expected.Revisions[1] > 1);
	REQUIRE(source.Enter(original, [&](ecs::Store &store) {
		configured = PublishOne(store, SCOPES[0]);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	CHECK(HasPublication(source, original));

	ScratchDirectory scratch;
	const auto path = scratch.Path / "procedural.agame";
	std::string error;
	REQUIRE(game::SaveGame(source, core::Name("ProceduralGame"), path, error));
	CHECK(error.empty());
	std::ifstream input(path, std::ios::binary);
	REQUIRE(input);
	const std::string document{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	CHECK(document.find("<ProceduralAuthority encoding=\"hex\">") != std::string::npos);
	CHECK(document.find("<![CDATA[") != std::string::npos);

	world::Universe loaded;
	game::GameInfo info;
	REQUIRE(game::LoadGame(loaded, path, info, error));
	REQUIRE(info.Worlds.size() == 1);
	const auto restored = loaded.Find(core::Name("Procedural"));
	REQUIRE(restored.IsValid());
	AuthoritySnapshot actual;
	REQUIRE(CaptureWorld(loaded, restored, actual));
	CHECK(actual.Bytes == expected.Bytes);
	CHECK(actual.RegionKeys == expected.RegionKeys);
	CHECK((actual.Revisions == std::array<uint64_t, 2>{1, 1}));
	CHECK(actual.Incarnation != expected.Incarnation);
	CHECK_FALSE(HasPublication(loaded, restored));
	loaded.Enter(restored, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
	});
}

TEST_CASE("procedural authority survives world document stop and restore", "[game][procedural]") {
	game::RegisterGameClasses();
	world::Universe universe;
	const auto original = AddWorld(universe, core::Name("Procedural"));
	const auto neighbor = AddWorld(universe, core::Name("Neighbor"));
	REQUIRE(original.IsValid());
	REQUIRE(neighbor.IsValid());
	bool configured = false;
	procedural::Diagnostic diagnostic;
	REQUIRE(universe.Enter(original, [&](ecs::Store &store) {
		configured = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	universe.Enter(neighbor, [](ecs::Store &store) {
		store.CreateInstance(scene::PartClass(), "NeighborKeep");
	});
	AuthoritySnapshot expected;
	REQUIRE(CaptureWorld(universe, original, expected));
	std::string error;
	const std::string document = game::WriteWorldDocument(universe, original, error);
	REQUIRE_FALSE(document.empty());
	CHECK(document.find("<ProceduralAuthority encoding=\"hex\">") != std::string::npos);
	REQUIRE(universe.Enter(original, [&](ecs::Store &store) {
		configured =
			script::SetProceduralSpline(store, {"west-road", "west-gate", {11 * CELL, 8 * CELL}}, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	AuthoritySnapshot changed;
	REQUIRE(CaptureWorld(universe, original, changed));
	CHECK(changed.Bytes != expected.Bytes);

	REQUIRE(universe.Destroy(original) == world::WorldStatus::Ok);
	const auto restored = game::ReadWorldDocument(universe, document, core::Name{}, error);
	REQUIRE(restored.IsValid());
	CHECK(universe.Count() == 2);
	CHECK(universe.Find(core::Name("Neighbor")).IsValid());
	AuthoritySnapshot actual;
	REQUIRE(CaptureWorld(universe, restored, actual));
	CHECK(actual.Bytes == expected.Bytes);
	CHECK(actual.RegionKeys == expected.RegionKeys);
	CHECK((actual.Revisions == std::array<uint64_t, 2>{1, 1}));
	CHECK(actual.Incarnation != expected.Incarnation);
	CHECK_FALSE(HasPublication(universe, restored));
	universe.Enter(neighbor, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("NeighborKeep") != ecs::NULL_ENTITY);
	});
}

TEST_CASE("missing and malformed procedural authority remains recoverable", "[game][procedural]") {
	game::RegisterGameClasses();
	world::Universe source;
	const auto plain = AddWorld(source, core::Name("Plain"));
	REQUIRE(plain.IsValid());
	source.Enter(plain, [](ecs::Store &store) { store.CreateInstance(scene::PartClass(), "KeepMe"); });
	std::string error;
	const std::string oldDocument = game::WriteWorldDocument(source, plain, error);
	REQUIRE_FALSE(oldDocument.empty());
	CHECK(oldDocument.find("ProceduralAuthority") == std::string::npos);

	world::Universe restored;
	const auto oldWorld = game::ReadWorldDocument(restored, oldDocument, core::Name("Old"), error);
	REQUIRE(oldWorld.IsValid());
	CHECK_FALSE(HasAuthority(restored, oldWorld));
	restored.Enter(oldWorld, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
	});

	const std::array<std::pair<std::string, std::string>, 4> invalidPayloads{{
		{"BadHex", "0g"},
		{"OddHex", "0"},
		{"Oversize", std::string(524290, '0')},
		{"InvalidPwh2", "00000000"},
	}};
	for (const auto &[name, payload] : invalidPayloads) {
		const std::string invalid = AddAuthorityBlock(oldDocument, payload);
		REQUIRE_FALSE(invalid.empty());
		const auto imported = game::ReadWorldDocument(restored, invalid, core::Name(name), error);
		INFO(name << ": " << error);
		REQUIRE(imported.IsValid());
		CHECK_FALSE(HasAuthority(restored, imported));
		restored.Enter(imported, [](ecs::Store &store) {
			CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
			CHECK(store.Resource<script::ProceduralAuthority>() == nullptr);
		});
	}

	world::Universe configured;
	const auto authored = AddWorld(configured, core::Name("Authored"));
	REQUIRE(authored.IsValid());
	bool setup = false;
	procedural::Diagnostic diagnostic;
	REQUIRE(configured.Enter(authored, [&](ecs::Store &store) {
		setup = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(setup);
	AuthoritySnapshot expected;
	REQUIRE(CaptureWorld(configured, authored, expected));
	const std::string validDocument = game::WriteWorldDocument(configured, authored, error);
	REQUIRE_FALSE(validDocument.empty());
	const auto partiallyValid = AddAuthorityBlock(validDocument, "00000000");
	const auto partial = game::ReadWorldDocument(restored, partiallyValid, core::Name("Partial"), error);
	REQUIRE(partial.IsValid());
	AuthoritySnapshot retained;
	REQUIRE(CaptureWorld(restored, partial, retained));
	CHECK(retained.Bytes == expected.Bytes);
	CHECK(retained.RegionKeys == expected.RegionKeys);
	restored.Enter(partial, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
	});
}

TEST_CASE(
	"invalid procedural authority cannot replace a saved game or produce a world document",
	"[game][procedural]"
) {
	game::RegisterGameClasses();
	world::Universe source;
	const auto worldId = AddWorld(source, core::Name("Procedural"));
	REQUIRE(worldId.IsValid());
	bool configured = false;
	procedural::Diagnostic diagnostic;
	REQUIRE(source.Enter(worldId, [&](ecs::Store &store) {
		configured = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	ScratchDirectory scratch;
	const auto path = scratch.Path / "atomic.agame";
	std::string error;
	REQUIRE(game::SaveGame(source, core::Name("ProceduralGame"), path, error));
	std::ifstream beforeFile(path, std::ios::binary);
	REQUIRE(beforeFile);
	const std::string before{std::istreambuf_iterator<char>(beforeFile), std::istreambuf_iterator<char>()};

	REQUIRE(source.Enter(worldId, [](ecs::Store &store) {
		store.ResourceMutable<script::ProceduralAuthority>()->RecipeBytes.clear();
	}) == world::WorldStatus::Ok);
	CHECK_FALSE(game::SaveGame(source, core::Name("ProceduralGame"), path, error));
	CHECK_FALSE(error.empty());
	std::ifstream afterFile(path, std::ios::binary);
	REQUIRE(afterFile);
	const std::string after{std::istreambuf_iterator<char>(afterFile), std::istreambuf_iterator<char>()};
	CHECK(after == before);
	CHECK(game::WriteWorldDocument(source, worldId, error).empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE(
	"invalid later authority cannot partially replace a saved universe", "[game][procedural][manifest]"
) {
	game::RegisterGameClasses();
	world::Universe source;
	const auto earlier = AddWorld(source, core::Name("AFirst"));
	const auto later = AddWorld(source, core::Name("ZLater"));
	REQUIRE(earlier.IsValid());
	REQUIRE(later.IsValid());
	procedural::Diagnostic diagnostic;
	bool configured = false;
	REQUIRE(source.Enter(earlier, [&](ecs::Store &store) {
		configured = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);
	REQUIRE(source.Enter(later, [&](ecs::Store &store) {
		configured = Configure(store, diagnostic);
	}) == world::WorldStatus::Ok);
	REQUIRE(configured);

	ScratchDirectory scratch;
	const auto manifest = scratch.Path / "atomic.auniverse";
	const auto worldDirectory = scratch.Path / "atomic.worlds";
	const auto earlierFile = worldDirectory / "1-AFirst.aworld";
	const auto laterFile = worldDirectory / "2-ZLater.aworld";
	std::string error;
	REQUIRE(game::SaveUniverse(source, core::Name("Atomic"), {}, manifest, {}, error));
	std::string manifestBefore;
	std::string earlierBefore;
	std::string laterBefore;
	REQUIRE(ReadFileText(manifest, manifestBefore));
	REQUIRE(ReadFileText(earlierFile, earlierBefore));
	REQUIRE(ReadFileText(laterFile, laterBefore));

	REQUIRE(source.Enter(earlier, [](ecs::Store &store) {
		store.CreateInstance(scene::PartClass(), "ChangedAfterSave");
	}) == world::WorldStatus::Ok);
	REQUIRE(source.Enter(later, [](ecs::Store &store) {
		store.ResourceMutable<script::ProceduralAuthority>()->RecipeBytes.clear();
	}) == world::WorldStatus::Ok);
	CHECK_FALSE(game::SaveUniverse(source, core::Name("Atomic"), {}, manifest, {}, error));
	CHECK_FALSE(error.empty());
	std::string manifestAfter;
	std::string earlierAfter;
	std::string laterAfter;
	REQUIRE(ReadFileText(manifest, manifestAfter));
	REQUIRE(ReadFileText(earlierFile, earlierAfter));
	REQUIRE(ReadFileText(laterFile, laterAfter));
	CHECK(manifestAfter == manifestBefore);
	CHECK(earlierAfter == earlierBefore);
	CHECK(laterAfter == laterBefore);
}

TEST_CASE("baked procedural references survive game file roundtrips", "[game][procedural][content]") {
	game::RegisterGameClasses();
	world::Universe source;
	const auto original = AddWorld(source, core::Name("Baked"));
	REQUIRE(original.IsValid());

	const procedural::RegionScope scope{3, -2, 16};
	ecs::Entity originalMesh = ecs::NULL_ENTITY;
	ecs::Entity originalPart = ecs::NULL_ENTITY;
	uint64_t sourceIdentity = 0;
	bool authored = false;
	procedural::Diagnostic diagnostic;
	REQUIRE(source.Enter(original, [&](ecs::Store &store) {
		const auto transient = store.CreateInstance(scene::PartClass(), "SessionOnly");
		if (transient == ecs::NULL_ENTITY) return;
		store.Set(transient, scene::TransientComponent{});

		originalMesh = store.CreateInstance(scene::EditableMeshClass(), "BakedRegion");
		if (originalMesh == ecs::NULL_ENTITY) return;

		scene::PartDesc description;
		description.Class = ecs::Classes::Find(core::Name("MeshPart"));
		description.Mesh = scene::EditableMeshContentName(store, originalMesh);
		originalPart = scene::MakePart(store, description);
		if (originalPart == ecs::NULL_ENTITY || !store.SetInstanceName(originalPart, "BakedPart")) return;

		auto *collider = store.GetMutable<scene::Collider>(originalPart);
		if (collider == nullptr) return;
		collider->Mask = spatial::LayerMask::None();
		collider->CanQuery = false;
		if (!script::SetBakedProceduralReference(
				store, scope, originalMesh, originalPart, "town/region.aproc", diagnostic
			) ||
			!script::SetBakedProceduralReference(
				store, scope, originalMesh, originalPart, "town/region.aproc", diagnostic
			))
			return;

		sourceIdentity = store.Identity();
		authored = store.Resource<script::ProceduralAuthority>() == nullptr &&
				   store.Resource<script::ProceduralPublished>() == nullptr &&
				   store.Resource<script::SourceCache>() == nullptr;
	}) == world::WorldStatus::Ok);
	REQUIRE(authored);
	REQUIRE(originalMesh != ecs::NULL_ENTITY);
	REQUIRE(originalPart != ecs::NULL_ENTITY);

	const auto checkRestored = [&](world::Universe &destination, world::WorldId restored) {
		REQUIRE(restored.IsValid());
		bool checked = false;
		const auto status = destination.Enter(restored, [&](ecs::Store &store) {
			const auto mesh = store.FindFirstRoot("BakedRegion");
			const auto part = store.FindFirstRoot("BakedPart");
			CHECK(store.FindFirstRoot("SessionOnly") == ecs::NULL_ENTITY);
			CHECK(store.Identity() != sourceIdentity);
			CHECK(mesh != ecs::NULL_ENTITY);
			CHECK(part != ecs::NULL_ENTITY);
			if (mesh == ecs::NULL_ENTITY || part == ecs::NULL_ENTITY) return;

			CHECK(mesh != originalMesh);
			CHECK(part != originalPart);
			const auto *collider = store.Get<scene::Collider>(part);
			REQUIRE(collider);
			CHECK(collider->Mask == spatial::LayerMask::None());
			CHECK_FALSE(collider->Trigger);
			CHECK_FALSE(collider->CanQuery);
			const auto *reference = store.Get<script::BakedProceduralReference>(mesh);
			REQUIRE(reference);
			CHECK(reference->Description.Text() == "town/region.aproc");
			CHECK(reference->Scope.X == scope.X);
			CHECK(reference->Scope.Y == scope.Y);
			CHECK(reference->Scope.CellsPerSide == scope.CellsPerSide);
			CHECK(reference->Part == part);
			CHECK(reference->Revision != 0);

			script::ProceduralSceneTarget target;
			procedural::Diagnostic restoredDiagnostic;
			REQUIRE(
				script::CaptureProceduralSceneTarget(store, scope, mesh, part, target, restoredDiagnostic)
			);
			CHECK(target.Content == scene::EditableMeshContentName(store, mesh));
			CHECK(target.StoreIdentity == store.Identity());
			CHECK(store.Resource<script::ProceduralAuthority>() == nullptr);
			CHECK(store.Resource<script::ProceduralPublished>() == nullptr);
			CHECK(store.Resource<script::SourceCache>() == nullptr);
			checked = true;
		});
		REQUIRE(status == world::WorldStatus::Ok);
		REQUIRE(checked);
	};

	SECTION("world document") {
		std::string error;
		const std::string document = game::WriteWorldDocument(source, original, error);
		REQUIRE_FALSE(document.empty());
		world::Universe destination;
		const auto restored = game::ReadWorldDocument(destination, document, core::Name{}, error);
		checkRestored(destination, restored);
	}

	SECTION("world file") {
		ScratchDirectory scratch;
		const auto path = scratch.Path / "baked.aworld";
		std::string error;
		REQUIRE(game::ExportWorld(source, original, path, error));
		world::Universe destination;
		const auto restored = game::ImportWorld(destination, path, core::Name{}, error);
		checkRestored(destination, restored);
	}

	SECTION("game file") {
		ScratchDirectory scratch;
		const auto path = scratch.Path / "baked.agame";
		std::string error;
		REQUIRE(game::SaveGame(source, core::Name("BakedGame"), path, error));
		world::Universe destination;
		game::GameInfo info;
		REQUIRE(game::LoadGame(destination, path, info, error));
		REQUIRE(info.Worlds.size() == 1);
		checkRestored(destination, destination.Find(core::Name("Baked")));
	}
}

TEST_CASE("transient baked subtrees and inert rows do not author demands", "[game][procedural][content]") {
	game::RegisterGameClasses();
	world::Universe source;
	const auto original = AddWorld(source, core::Name("Transient"));
	REQUIRE(original.IsValid());
	bool authored = false;
	const auto status = source.Enter(original, [&](ecs::Store &store) {
		BakedTargets targets;
		if (!AuthorBakedReference(store, targets)) return;
		const auto parent = store.CreateInstance(scene::PartClass(), "SessionOnly");
		store.Set(parent, scene::TransientComponent{});
		if (!store.SetParent(targets.Mesh, parent) || !store.SetParent(targets.Part, parent)) return;
		// An excluded subtree cannot block a save, even with an incomplete demand.
		store.GetMutable<script::BakedProceduralReference>(targets.Mesh)->Part = ecs::NULL_ENTITY;
		const auto inert = store.CreateInstance(scene::EditableMeshClass(), "InertRegion");
		store.Set(inert, script::BakedProceduralReference{});
		store.CreateInstance(scene::PartClass(), "KeepMe");
		authored = true;
	});
	REQUIRE(status == world::WorldStatus::Ok);
	REQUIRE(authored);
	std::string error;
	const auto document = game::WriteWorldDocument(source, original, error);
	REQUIRE_FALSE(document.empty());
	CHECK(document.find("BakedProceduralReferences") == std::string::npos);
	CHECK(document.find("BakedRegion") == std::string::npos);
	CHECK(document.find("BakedPart") == std::string::npos);
	CHECK(document.find("SessionOnly") == std::string::npos);
	ScratchDirectory scratch;
	const auto path = scratch.Path / "transient.agame";
	REQUIRE(game::SaveGame(source, core::Name("TransientGame"), path, error));
	world::Universe loaded;
	game::GameInfo info;
	REQUIRE(game::LoadGame(loaded, path, info, error));
	const auto restored = loaded.Find(core::Name("Transient"));
	REQUIRE(restored.IsValid());
	const auto loadedStatus = loaded.Enter(restored, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
		const auto inert = store.FindFirstRoot("InertRegion");
		REQUIRE(inert != ecs::NULL_ENTITY);
		CHECK_FALSE(store.Has<script::BakedProceduralReference>(inert));
		CHECK(store.FindFirstRoot("SessionOnly") == ecs::NULL_ENTITY);
	});
	REQUIRE(loadedStatus == world::WorldStatus::Ok);
}

TEST_CASE(
	"malformed optional baked references preserve authored items and earlier bindings",
	"[game][procedural][content]"
) {
	game::RegisterGameClasses();
	world::Universe source;
	const auto original = AddWorld(source, core::Name("Baked"));
	REQUIRE(original.IsValid());
	bool authored = false;
	const auto status = source.Enter(original, [&](ecs::Store &store) {
		BakedTargets targets;
		authored = AuthorBakedReference(store, targets);
		store.CreateInstance(scene::PartClass(), "KeepMe");
	});
	REQUIRE(status == world::WorldStatus::Ok);
	REQUIRE(authored);
	std::string error;
	const std::string document = game::WriteWorldDocument(source, original, error);
	REQUIRE_FALSE(document.empty());
	REQUIRE(document.find("<BakedProceduralReferences encoding=\"hex\">") != std::string::npos);
	const std::array<std::pair<std::string, std::string>, 5> invalidPayloads{{
		{"BadHex", "0g"},
		{"OddHex", "0"},
		{"Oversize", std::string(script::MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES * 2 + 2, '0')},
		{"UnknownVersion", "00000000"},
		{"Truncated", "0100000001000000"},
	}};
	for (const auto &[name, payload] : invalidPayloads) {
		INFO(name);
		world::Universe loaded;
		const auto invalid = AddOptionalBlock(document, "BakedProceduralReferences", payload);
		const auto restored = game::ReadWorldDocument(loaded, invalid, core::Name(name), error);
		REQUIRE(restored.IsValid());
		const auto loadedStatus = loaded.Enter(restored, [](ecs::Store &store) {
			CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
			const auto mesh = store.FindFirstRoot("BakedRegion");
			const auto part = store.FindFirstRoot("BakedPart");
			const auto *reference = store.Get<script::BakedProceduralReference>(mesh);
			REQUIRE(reference);
			CHECK(reference->Description.Text() == "town/region.aproc");
			CHECK(reference->Part == part);
			script::ProceduralSceneTarget target;
			procedural::Diagnostic diagnostic;
			CHECK(script::CaptureProceduralSceneTarget(store, BAKED_SCOPE, mesh, part, target, diagnostic));
		});
		REQUIRE(loadedStatus == world::WorldStatus::Ok);
	}
	world::Universe plain;
	const auto oldWorld = AddWorld(plain, core::Name("Old"));
	REQUIRE(oldWorld.IsValid());
	plain.Enter(oldWorld, [](ecs::Store &store) { store.CreateInstance(scene::PartClass(), "KeepMe"); });
	const auto oldDocument = game::WriteWorldDocument(plain, oldWorld, error);
	REQUIRE_FALSE(oldDocument.empty());
	world::Universe loaded;
	const auto invalid = AddOptionalBlock(oldDocument, "BakedProceduralReferences", "00", "unknown");
	const auto restored = game::ReadWorldDocument(loaded, invalid, core::Name{}, error);
	REQUIRE(restored.IsValid());
	const auto loadedStatus = loaded.Enter(restored, [](ecs::Store &store) {
		CHECK(store.FindFirstRoot("KeepMe") != ecs::NULL_ENTITY);
	});
	REQUIRE(loadedStatus == world::WorldStatus::Ok);
}

TEST_CASE("dangling baked demands cannot replace saved game content", "[game][procedural][content]") {
	game::RegisterGameClasses();
	world::Universe source;
	const auto original = AddWorld(source, core::Name("Baked"));
	REQUIRE(original.IsValid());
	BakedTargets targets;
	bool authored = false;
	const auto status =
		source.Enter(original, [&](ecs::Store &store) { authored = AuthorBakedReference(store, targets); });
	REQUIRE(status == world::WorldStatus::Ok);
	REQUIRE(authored);
	ScratchDirectory scratch;
	const auto path = scratch.Path / "baked.agame";
	std::string error;
	REQUIRE(game::SaveGame(source, core::Name("BakedGame"), path, error));
	std::string before;
	REQUIRE(ReadFileText(path, before));
	SECTION("destroyed target") {
		source.Enter(original, [&](ecs::Store &store) { store.DestroyInstance(targets.Part); });
	}
	SECTION("excluded target") {
		source.Enter(original, [&](ecs::Store &store) {
			store.Set(targets.Part, scene::TransientComponent{});
		});
	}
	CHECK_FALSE(game::SaveGame(source, core::Name("BakedGame"), path, error));
	CHECK_FALSE(error.empty());
	std::string after;
	REQUIRE(ReadFileText(path, after));
	CHECK(after == before);
	CHECK(game::WriteWorldDocument(source, original, error).empty());
	CHECK_FALSE(error.empty());
}

TEST_CASE(
	"invalid later baked demand cannot partially replace a saved universe", "[game][procedural][manifest]"
) {
	game::RegisterGameClasses();
	world::Universe source;
	const auto earlier = AddWorld(source, core::Name("AFirst"));
	const auto later = AddWorld(source, core::Name("ZLater"));
	REQUIRE(earlier.IsValid());
	REQUIRE(later.IsValid());
	BakedTargets targets;
	bool authored = false;
	const auto status =
		source.Enter(later, [&](ecs::Store &store) { authored = AuthorBakedReference(store, targets); });
	REQUIRE(status == world::WorldStatus::Ok);
	REQUIRE(authored);
	ScratchDirectory scratch;
	const auto manifest = scratch.Path / "atomic.auniverse";
	const auto worldDirectory = scratch.Path / "atomic.worlds";
	const auto earlierFile = worldDirectory / "1-AFirst.aworld";
	const auto laterFile = worldDirectory / "2-ZLater.aworld";
	std::string error;
	REQUIRE(game::SaveUniverse(source, core::Name("Atomic"), {}, manifest, {}, error));
	std::string manifestBefore;
	std::string earlierBefore;
	std::string laterBefore;
	REQUIRE(ReadFileText(manifest, manifestBefore));
	REQUIRE(ReadFileText(earlierFile, earlierBefore));
	REQUIRE(ReadFileText(laterFile, laterBefore));
	source.Enter(earlier, [](ecs::Store &store) {
		store.CreateInstance(scene::PartClass(), "ChangedAfterSave");
	});
	source.Enter(later, [&](ecs::Store &store) { store.DestroyInstance(targets.Part); });
	CHECK_FALSE(game::SaveUniverse(source, core::Name("Atomic"), {}, manifest, {}, error));
	CHECK_FALSE(error.empty());
	std::string manifestAfter;
	std::string earlierAfter;
	std::string laterAfter;
	REQUIRE(ReadFileText(manifest, manifestAfter));
	REQUIRE(ReadFileText(earlierFile, earlierAfter));
	REQUIRE(ReadFileText(laterFile, laterAfter));
	CHECK(manifestAfter == manifestBefore);
	CHECK(earlierAfter == earlierBefore);
	CHECK(laterAfter == laterBefore);
}
