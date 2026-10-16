#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/delivery/Source.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Region.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralJobs.hpp>
#include <engine/script/ProceduralScene.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <assetc/Procedural.hpp>
#include <cdn/Publisher.hpp>
#include <client/ProceduralContent.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

TEST_SUITE_ID("unified.proceduralcontent")
TEST_DEPENDS("engine.script.bakedproceduralscene")
TEST_DEPENDS("cdn.publisher")

using namespace engine;

namespace {
	constexpr procedural::RegionScope SCOPE{0, 0, 16};
	constexpr int64_t CELL = procedural::SUBCELLS_PER_CELL;
	constexpr std::string_view DESCRIPTION = "town/region.aproc";

	struct Fixture {
		std::filesystem::path Root;
		ecs::Store World{"procedural-content"};
		procedural::Diagnostic Diagnostic{};
		assets::SigningKey Key;
		Fixture()
			: Key([] {
				  auto key = assets::SigningKey::FromSeed(std::array<std::byte, 32>{});
				  return std::move(*key);
			  }()) {
			static int serial = 0;
			Root = std::filesystem::temp_directory_path() /
				   ("atomic-procedural-content-" + std::to_string(++serial));
			std::filesystem::remove_all(Root);
			scene::RegisterSceneComponents();
			script::RegisterProceduralComponents();
		}
		~Fixture() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
		script::ProceduralSceneTarget Target(bool bind = true) {
			const auto mesh = World.CreateInstance(scene::EditableMeshClass(), "Region");
			scene::PartDesc description;
			description.Class = ecs::Classes::Find(core::Name("MeshPart"));
			description.Mesh = scene::EditableMeshContentName(World, mesh);
			const auto part = scene::MakePart(World, description);
			REQUIRE(part != ecs::NULL_ENTITY);
			auto *collider = World.GetMutable<scene::Collider>(part);
			collider->CanQuery = false;
			if (bind)
				collider->Mask = spatial::LayerMask::None();
			else
				collider->Trigger = true;
			script::ProceduralSceneTarget target;
			if (bind)
				REQUIRE(script::BindProceduralSceneTarget(World, SCOPE, mesh, part, target, Diagnostic));
			else
				REQUIRE(script::CaptureProceduralSceneTarget(World, SCOPE, mesh, part, target, Diagnostic));
			return target;
		}
		void PublishFiles(assets::Manifest &manifest, assets::SignatureBytes &signature) {
			cdn::PublishSettings settings;
			settings.TrainDictionary = false;
			REQUIRE(cdn::Publish(Root / "content", Root / "store", Key, settings).has_value());
			auto store = assets::ChunkStore::Open(Root / "store", false);
			REQUIRE(store.has_value());
			auto signedManifest = store->ReadManifest(signature);
			REQUIRE(signedManifest.has_value());
			manifest = std::move(*signedManifest);
			REQUIRE(assets::VerifyManifestRoot(manifest.Root(), signature, Key.Public()));
		}
		std::unique_ptr<delivery::AssetClient> Client() {
			delivery::DeliverySettings settings;
			settings.Publisher = Key.Public();
			settings.Sources.push_back(
				{.Name = "signed-store",
				 .Kind = delivery::SourceKind::Directory,
				 .Location = (Root / "store").string(),
				 .Enabled = true,
				 .Role = delivery::SourceRole::Read}
			);
			auto client = delivery::MakeAssetClient(settings);
			REQUIRE(client != nullptr);
			return client;
		}
	};

	void Drive(Fixture &fixture, delivery::AssetClient &delivery, client::ProceduralContent &content) {
		delivery::IntakeBudget budget;
		for (size_t turn = 0; turn < 2000; ++turn) {
			delivery.Pump();
			budget.Begin();
			content.BeginFrame();
			content.PumpWorld(fixture.World, delivery, budget);
			content.EndFrame(delivery);
			if (content.Counters().Installed || content.Counters().Refused) return;
		}
	}
}

TEST_CASE(
	"signed procedural files reach the local scene through the production content pump",
	"[procedural][content][e2e]"
) {
	Fixture fixture;
	const std::array<script::ProceduralSource, 2> sources{
		{{procedural::SourceKind::Village, "oakstead", {8 * CELL, 8 * CELL}, ""},
		 {procedural::SourceKind::Entrance, "west-gate", {12 * CELL, 8 * CELL}, "oakstead"}}
	};
	REQUIRE(
		script::ConfigureProceduralWorld(
			fixture.World,
			"demo",
			procedural::ForestVillageDefaults("forest-village", 42),
			std::span(&SCOPE, 1),
			sources,
			fixture.Diagnostic
		)
	);
	const auto original = fixture.Target();
	script::ProceduralJobs jobs(fixture.World);
	const uint64_t ticket = jobs.Submit(fixture.World, SCOPE, parallel::JobContext::Serial);
	REQUIRE(ticket != 0);
	jobs.Poll();
	REQUIRE(jobs.PublishSceneBatch(
		fixture.World, std::span(&ticket, 1), std::span(&original, 1), fixture.World.Time().Tick
	));
	const auto baseline = *fixture.World.Get<scene::EditableMesh>(original.Mesh);
	const auto baselineRegion = *fixture.World.Get<script::ProceduralSceneRegion>(original.Mesh);
	const auto &published = fixture.World.Resource<script::ProceduralPublished>()->Regions;
	REQUIRE(published.size() == 1);
	bake::ProceduralBake baked;
	std::string failure;
	REQUIRE(assetc::CookProceduralArtifact(published[0].ArtifactBytes, SCOPE, baked, failure));
	REQUIRE(assetc::PublishProceduralBake(fixture.Root / "content", DESCRIPTION, baked, failure));
	assets::Manifest manifest;
	assets::SignatureBytes signature;
	fixture.PublishFiles(manifest, signature);
	const auto *description = manifest.Find(DESCRIPTION);
	REQUIRE(description);
	CHECK(description->Kind == assets::AssetKind::Data);
	CHECK(baked.DescriptionBytes.size() == description->TotalBytes);
	const assets::ContentHash rawDescription = assets::Hasher::Of(baked.DescriptionBytes);
	CHECK(rawDescription.Digest != description->Root.Digest);

	fixture.World.RemoveResource<script::ProceduralAuthority>();
	fixture.World.RemoveResource<script::ProceduralPublished>();
	const auto target = fixture.Target(false);
	CHECK_FALSE(fixture.World.Has<script::ProceduralSceneRegion>(target.Mesh));
	CHECK(fixture.World.Get<scene::Visual>(target.Part)->Fitted != target.Content);
	CHECK(fixture.World.Get<scene::Collider>(target.Part)->Mask == spatial::LayerMask::All());
	REQUIRE(
		script::SetBakedProceduralReference(
			fixture.World, SCOPE, target.Mesh, target.Part, DESCRIPTION, fixture.Diagnostic
		)
	);
	CHECK_FALSE(fixture.World.Has<script::ProceduralSceneRegion>(target.Mesh));
	CHECK(fixture.World.Get<scene::Visual>(target.Part)->Fitted != target.Content);
	auto delivery = fixture.Client();
	client::ProceduralContent content;
	Drive(fixture, *delivery, content);
	REQUIRE(content.Counters().Installed == 1);
	CHECK(content.Counters().Refused == 0);
	const auto *mesh = fixture.World.Get<scene::EditableMesh>(target.Mesh);
	const auto *region = fixture.World.Get<script::ProceduralSceneRegion>(target.Mesh);
	REQUIRE(mesh);
	REQUIRE(region);
	CHECK(fixture.World.Get<scene::Visual>(target.Part)->Fitted == target.Content);
	CHECK(fixture.World.Get<scene::Collider>(target.Part)->Mask == spatial::LayerMask::None());
	CHECK(mesh->Positions == baseline.Positions);
	CHECK(mesh->Normals == baseline.Normals);
	CHECK(mesh->UVs == baseline.UVs);
	CHECK(mesh->Colours == baseline.Colours);
	CHECK(mesh->Alphas == baseline.Alphas);
	CHECK(mesh->Indices == baseline.Indices);
	REQUIRE(region->Objects.size() == baselineRegion.Objects.size());
	for (size_t i = 0; i < region->Objects.size(); ++i) {
		CHECK(region->Objects[i].Id == baselineRegion.Objects[i].Id);
		CHECK(region->Objects[i].FirstIndex == baselineRegion.Objects[i].FirstIndex);
		CHECK(region->Objects[i].IndexCount == baselineRegion.Objects[i].IndexCount);
	}
	CHECK(region->ArtifactDigest.Bytes == rawDescription.Digest);

	// A target edit after demand admission must not be hidden by a later content download.
	auto staleDelivery = fixture.Client();
	for (size_t turn = 0; turn < 2000 && !staleDelivery->Ready(); ++turn)
		staleDelivery->Pump();
	REQUIRE(staleDelivery->Ready());
	client::ProceduralContent staleContent;
	delivery::IntakeBudget staleBudget;
	staleBudget.Begin();
	staleContent.BeginFrame();
	staleContent.PumpWorld(fixture.World, *staleDelivery, staleBudget);
	staleContent.EndFrame(*staleDelivery);
	REQUIRE(staleContent.Counters().Pending == 1);
	const auto acceptedPositions = mesh->Positions;
	const auto editedRevision = ++fixture.World.GetMutable<scene::EditableMesh>(target.Mesh)->Revision;
	Drive(fixture, *staleDelivery, staleContent);
	CHECK(staleContent.Counters().Installed == 0);
	CHECK(staleContent.Counters().Refused == 1);
	CHECK(fixture.World.Get<scene::EditableMesh>(target.Mesh)->Revision == editedRevision);
	CHECK(fixture.World.Get<scene::EditableMesh>(target.Mesh)->Positions == acceptedPositions);
	REQUIRE(
		script::SetBakedProceduralReference(
			fixture.World, SCOPE, target.Mesh, target.Part, DESCRIPTION, fixture.Diagnostic
		)
	);
	CHECK(fixture.World.Get<script::BakedProceduralReference>(target.Mesh)->Revision == 2);
	staleBudget.Begin();
	staleContent.BeginFrame();
	staleContent.PumpWorld(fixture.World, *staleDelivery, staleBudget);
	staleContent.EndFrame(*staleDelivery);
	REQUIRE(staleContent.Counters().Pending == 1);
	staleDelivery->Pump(); // A completion for revision 2 may now be ready.
	REQUIRE(
		script::SetBakedProceduralReference(
			fixture.World, SCOPE, target.Mesh, target.Part, DESCRIPTION, fixture.Diagnostic
		)
	);
	CHECK(fixture.World.Get<script::BakedProceduralReference>(target.Mesh)->Revision == 3);
	staleBudget.Begin();
	staleContent.BeginFrame();
	staleContent.PumpWorld(fixture.World, *staleDelivery, staleBudget);
	staleContent.EndFrame(*staleDelivery);
	CHECK(staleContent.Counters().Installed == 0);
	for (size_t turn = 0; turn < 2000 && staleContent.Counters().Installed == 0; ++turn) {
		staleDelivery->Pump();
		staleBudget.Begin();
		staleContent.BeginFrame();
		staleContent.PumpWorld(fixture.World, *staleDelivery, staleBudget);
		staleContent.EndFrame(*staleDelivery);
	}
	CHECK(staleContent.Counters().Installed == 1);
	CHECK(staleContent.Counters().Refused == 1);
	CHECK(fixture.World.Get<scene::EditableMesh>(target.Mesh)->Positions == acceptedPositions);

	REQUIRE(!baked.Meshes.empty());
	const auto damagedPath = fixture.Root / "content" / baked.Meshes.front().Name;
	{
		std::ofstream damaged(damagedPath, std::ios::binary | std::ios::trunc);
		REQUIRE(damaged.good());
		damaged << "signed but misnamed ordinary mesh";
	}
	fixture.PublishFiles(manifest, signature);
	const auto oldRevision = mesh->Revision;
	const auto oldPositions = mesh->Positions;
	auto damagedDelivery = fixture.Client();
	client::ProceduralContent damagedContent;
	Drive(fixture, *damagedDelivery, damagedContent);
	CHECK(damagedContent.Counters().Installed == 0);
	CHECK(damagedContent.Counters().Refused == 1);
	CHECK(fixture.World.Get<scene::EditableMesh>(target.Mesh)->Revision == oldRevision);
	CHECK(fixture.World.Get<scene::EditableMesh>(target.Mesh)->Positions == oldPositions);
}
