#include <engine/assets/ContentForm.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/assets/ProceduralDescription.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/IntakeBudget.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/BakedProceduralScene.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <client/ProceduralContent.hpp>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace client {
	namespace procedural_content_detail {
		constexpr size_t MAXIMUM_MESH_BYTES = 40 * 1024 * 1024;
		constexpr uint64_t MAXIMUM_CARRIER_BYTES = 64ull * 1024 * 1024;
		constexpr size_t MAXIMUM_MESH_REQUESTS = 4;

		bool SameScope(engine::procedural::RegionScope a, engine::procedural::RegionScope b) {
			return a.X == b.X && a.Y == b.Y && a.CellsPerSide == b.CellsPerSide;
		}

		bool Admitted(
			const engine::assets::Manifest &catalogue,
			std::string_view name,
			engine::assets::AssetKind kind,
			engine::assets::ContentForm form,
			uint64_t maximumBytes
		) {
			const auto *entry = catalogue.Find(name);
			const auto *bundle = entry ? catalogue.BundleFor(entry->Root) : nullptr;
			return entry && entry->Kind == kind && engine::assets::FormOfName(name) == form &&
				   engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle).Allows(form) &&
				   entry->TotalBytes <= maximumBytes && bundle && bundle->TotalBytes <= MAXIMUM_CARRIER_BYTES;
		}

		struct PendingMeshAsset {
			std::string Name;
			engine::delivery::RequestId Ticket{};
			std::vector<std::byte> Bytes{};
			bool Ready = false;
		};
		enum class Phase { Idle, Description, Meshes, Installed, Refused };
		struct Region {
			uint64_t StoreIdentity = 0;
			engine::ecs::Entity Mesh{};
			engine::script::BakedProceduralReference Reference{};
			engine::script::ProceduralSceneTarget Target{};
			engine::delivery::RequestId DescriptionTicket{};
			std::vector<std::byte> DescriptionBytes{};
			std::vector<PendingMeshAsset> Meshes{};
			Phase Stage = Phase::Idle;
			bool Seen = false;
		};

		void Cancel(engine::delivery::AssetClient &delivery, Region &region) {
			if (region.DescriptionTicket.IsValid()) delivery.Cancel(region.DescriptionTicket);
			for (auto &mesh : region.Meshes)
				if (mesh.Ticket.IsValid()) delivery.Cancel(mesh.Ticket);
			region.DescriptionTicket = {};
			region.DescriptionBytes.clear();
			region.Meshes.clear();
		}

		bool Same(const Region &region, const engine::script::BakedProceduralReference &reference) {
			return region.Reference.Description == reference.Description &&
				   region.Reference.Revision == reference.Revision &&
				   region.Reference.Part == reference.Part &&
				   SameScope(region.Reference.Scope, reference.Scope);
		}
	}

	struct ProceduralContent::State {
		std::vector<procedural_content_detail::Region> Regions;
		ProceduralContentCounters Tally{};

		void Refuse(engine::delivery::AssetClient &delivery, procedural_content_detail::Region &region) {
			procedural_content_detail::Cancel(delivery, region);
			region.Stage = procedural_content_detail::Phase::Refused;
			++Tally.Refused;
		}

		bool Description(
			engine::delivery::AssetClient &delivery,
			engine::delivery::IntakeBudget &budget,
			procedural_content_detail::Region &region
		) {
			using namespace procedural_content_detail;
			const auto *catalogue = delivery.Catalogue();
			if (!catalogue || !Admitted(
								  *catalogue,
								  region.Reference.Description.Text(),
								  engine::assets::AssetKind::Data,
								  engine::assets::ContentForm::AProcedural,
								  engine::assets::MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES
							  )) {
				Refuse(delivery, region);
				return false;
			}
			if (!region.DescriptionTicket.IsValid())
				region.DescriptionTicket = delivery.Request(region.Reference.Description.Text());
			const auto state = delivery.StateOf(region.DescriptionTicket);
			if (state == engine::delivery::RequestState::Pending) return false;
			if (state != engine::delivery::RequestState::Ready) {
				Refuse(delivery, region);
				return false;
			}
			if (!budget.Admits()) {
				budget.Defer();
				return false;
			}
			auto asset = delivery.Take(region.DescriptionTicket);
			region.DescriptionTicket = {};
			if (!asset || asset->Name != region.Reference.Description.Text() ||
				asset->Kind != engine::assets::AssetKind::Data ||
				asset->Root != catalogue->Find(asset->Name)->Root) {
				Refuse(delivery, region);
				return false;
			}
			budget.Spend(asset->Bytes.size());
			std::vector<engine::assets::ProceduralPlacement> placements;
			std::string failure;
			if (!engine::assets::DecodeProceduralDescription(asset->Bytes, placements, failure)) {
				Refuse(delivery, region);
				return false;
			}
			std::vector<std::string> names;
			names.reserve(placements.size());
			for (const auto &placement : placements)
				names.push_back(placement.MeshName);
			std::sort(names.begin(), names.end(), engine::procedural::StableNameLess);
			names.erase(std::unique(names.begin(), names.end()), names.end());
			size_t total = 0;
			for (const auto &name : names) {
				const auto *entry = catalogue->Find(name);
				if (!Admitted(
						*catalogue,
						name,
						engine::assets::AssetKind::Mesh,
						engine::assets::ContentForm::AMesh,
						MAXIMUM_MESH_BYTES
					) ||
					entry->TotalBytes > MAXIMUM_MESH_BYTES - total) {
					Refuse(delivery, region);
					return false;
				}
				total += static_cast<size_t>(entry->TotalBytes);
			}
			region.DescriptionBytes = std::move(asset->Bytes);
			region.Meshes.reserve(names.size());
			for (auto &name : names) {
				PendingMeshAsset pending{};
				pending.Name = std::move(name);
				region.Meshes.push_back(std::move(pending));
			}
			region.Stage = Phase::Meshes;
			return true;
		}

		void Meshes(
			engine::ecs::Store &store,
			engine::delivery::AssetClient &delivery,
			engine::delivery::IntakeBudget &budget,
			procedural_content_detail::Region &region
		) {
			using namespace procedural_content_detail;
			const auto *catalogue = delivery.Catalogue();
			if (!catalogue) {
				Refuse(delivery, region);
				return;
			}
			size_t outstanding = 0;
			for (auto &mesh : region.Meshes) {
				if (!mesh.Ticket.IsValid()) continue;
				const auto state = delivery.StateOf(mesh.Ticket);
				if (state == engine::delivery::RequestState::Pending) {
					++outstanding;
					continue;
				}
				if (state != engine::delivery::RequestState::Ready) {
					Refuse(delivery, region);
					return;
				}
				if (!budget.Admits()) {
					budget.Defer();
					++outstanding;
					continue;
				}
				auto asset = delivery.Take(mesh.Ticket);
				mesh.Ticket = {};
				if (!asset || asset->Name != mesh.Name || asset->Kind != engine::assets::AssetKind::Mesh ||
					asset->Root != catalogue->Find(mesh.Name)->Root) {
					Refuse(delivery, region);
					return;
				}
				const std::string expected =
					"procedural/" + engine::assets::Hasher::Of(asset->Bytes).ToHex() + ".amesh";
				if (expected != mesh.Name) {
					Refuse(delivery, region);
					return;
				}
				budget.Spend(asset->Bytes.size());
				mesh.Bytes = std::move(asset->Bytes);
				mesh.Ready = true;
			}
			for (auto &mesh : region.Meshes) {
				if (outstanding >= MAXIMUM_MESH_REQUESTS) break;
				if (mesh.Ready || mesh.Ticket.IsValid()) continue;
				mesh.Ticket = delivery.Request(mesh.Name);
				++outstanding;
			}
			if (std::any_of(region.Meshes.begin(), region.Meshes.end(), [](const auto &mesh) {
					return !mesh.Ready;
				}))
				return;
			const auto *reference = store.Get<engine::script::BakedProceduralReference>(region.Mesh);
			if (!reference || store.Identity() != region.StoreIdentity || !Same(region, *reference)) {
				Refuse(delivery, region);
				return;
			}
			engine::procedural::Diagnostic diagnostic;
			std::vector<engine::script::BakedProceduralMesh> views;
			views.reserve(region.Meshes.size());
			for (const auto &mesh : region.Meshes)
				views.push_back({mesh.Name, mesh.Bytes});
			const auto hash = engine::assets::Hasher::Of(region.DescriptionBytes);
			if (!engine::script::InstallBakedProceduralScene(
					store, region.Target, hash, region.DescriptionBytes, views, diagnostic
				)) {
				Refuse(delivery, region);
				return;
			}
			Cancel(delivery, region);
			region.Stage = Phase::Installed;
			++Tally.Installed;
		}
	};

	ProceduralContent::ProceduralContent() : Held(std::make_unique<State>()) {}
	ProceduralContent::~ProceduralContent() = default;

	void ProceduralContent::BeginFrame() {
		for (auto &region : Held->Regions)
			region.Seen = false;
	}

	void ProceduralContent::PumpWorld(
		engine::ecs::Store &store,
		engine::delivery::AssetClient &delivery,
		engine::delivery::IntakeBudget &budget
	) {
		using namespace procedural_content_detail;
		if (!store.IsOnOwningThread() || store.AdoptOnly() || !delivery.Ready()) return;
		try {
			store.Each<const engine::script::BakedProceduralReference>(
				[&](engine::ecs::Entity mesh, const engine::script::BakedProceduralReference &reference) {
					auto found =
						std::find_if(Held->Regions.begin(), Held->Regions.end(), [&](const Region &region) {
							return region.StoreIdentity == store.Identity() && region.Mesh == mesh;
						});
					if (found == Held->Regions.end()) {
						if (Held->Regions.size() >= engine::script::MAXIMUM_BAKED_PROCEDURAL_REFERENCES)
							return;
						Region added{};
						added.StoreIdentity = store.Identity();
						added.Mesh = mesh;
						added.Reference = reference;
						Held->Regions.push_back(std::move(added));
						found = std::prev(Held->Regions.end());
					} else if (!Same(*found, reference)) {
						Cancel(delivery, *found);
						found->Reference = reference;
						found->Target = {};
						found->Stage = Phase::Idle;
					}
					found->Seen = true;
				}
			);
			const auto active =
				std::find_if(Held->Regions.begin(), Held->Regions.end(), [](const Region &region) {
					return region.Stage == Phase::Description || region.Stage == Phase::Meshes;
				});
			Region *current = nullptr;
			if (active != Held->Regions.end()) {
				if (active->StoreIdentity == store.Identity() && active->Seen) current = &*active;
			} else {
				auto idle =
					std::find_if(Held->Regions.begin(), Held->Regions.end(), [&](const Region &region) {
						return region.StoreIdentity == store.Identity() && region.Seen &&
							   region.Stage == Phase::Idle;
					});
				if (idle != Held->Regions.end()) {
					engine::procedural::Diagnostic diagnostic;
					if (engine::script::CaptureProceduralSceneTarget(
							store,
							idle->Reference.Scope,
							idle->Mesh,
							idle->Reference.Part,
							idle->Target,
							diagnostic
						)) {
						idle->Stage = Phase::Description;
						current = &*idle;
					} else {
						Held->Refuse(delivery, *idle);
					}
				}
			}
			if (!current) return;
			if (current->Stage == Phase::Description && !Held->Description(delivery, budget, *current))
				return;
			if (current->Stage == Phase::Meshes) Held->Meshes(store, delivery, budget, *current);
		} catch (const std::bad_alloc &) {
			for (auto &region : Held->Regions)
				if (region.StoreIdentity == store.Identity() &&
					(region.Stage == Phase::Description || region.Stage == Phase::Meshes)) {
					Held->Refuse(delivery, region);
					break;
				}
		} catch (const std::length_error &) {
			for (auto &region : Held->Regions)
				if (region.StoreIdentity == store.Identity() &&
					(region.Stage == Phase::Description || region.Stage == Phase::Meshes)) {
					Held->Refuse(delivery, region);
					break;
				}
		}
	}

	void ProceduralContent::EndFrame(engine::delivery::AssetClient &delivery) {
		std::erase_if(Held->Regions, [&](auto &region) {
			if (region.Seen) return false;
			procedural_content_detail::Cancel(delivery, region);
			return true;
		});
	}

	ProceduralContentCounters ProceduralContent::Counters() const {
		ProceduralContentCounters result = Held->Tally;
		for (const auto &region : Held->Regions)
			if (region.Stage == procedural_content_detail::Phase::Description ||
				region.Stage == procedural_content_detail::Phase::Meshes)
				++result.Pending;
		return result;
	}
}
