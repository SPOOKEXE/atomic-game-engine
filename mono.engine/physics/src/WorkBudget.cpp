#include "WorkBudgetInternal.hpp"
#include "WorldResource.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/characters/Controls.hpp>
#include <engine/scene/editable/EditableMesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace engine::physics {
	namespace {
		// Version 2 adds `RegionMetres`, the window turn and the region quota;
		// version 1 reads defaults.
		constexpr uint8_t SERIALIZER_VERSION = 2;
		const char *StageName(PhysicsWorkStage stage) {
			switch (stage) {
			case PhysicsWorkStage::None:
				return "none";
			case PhysicsWorkStage::Admission:
				return "admission";
			case PhysicsWorkStage::EditableMesh:
				return "editable mesh";
			case PhysicsWorkStage::Integration:
				return "integration";
			case PhysicsWorkStage::BroadphaseSync:
				return "broadphase sync";
			case PhysicsWorkStage::Broadphase:
				return "broadphase";
			case PhysicsWorkStage::Continuous:
				return "continuous collision";
			case PhysicsWorkStage::Narrowphase:
				return "narrowphase";
			case PhysicsWorkStage::Solver:
				return "solver";
			case PhysicsWorkStage::Welds:
				return "welds";
			case PhysicsWorkStage::CopiedContacts:
				return "copied contacts";
			}
			return "unknown";
		}
		core::Name BudgetName() {
			static const core::Name name{PHYSICS_WORK_BUDGET_COMPONENT};
			return name;
		}
		PhysicsWorkBudget *EnsureWorkBudget(ecs::Store &store) {
			if (auto *budget = PreparedWorkBudgetMutable(store)) return budget;
			// Older complete checkpoints have indexes and a clock but no work
			// resource. Explicit configuration and scheduled execution seed it.
			if (PreparedWorld(store) == nullptr) return nullptr;
			RegisterPhysicsWorkBudget();
			store.SetResource(PhysicsWorkBudget{});
			return PreparedWorkBudgetMutable(store);
		}
		auto LimitFields(PhysicsWorkLimits &limits) {
			return std::array{
				&limits.WorkPerTick,
				&limits.MovingBodies,
				&limits.Colliders,
				&limits.Joints,
				&limits.CopiedBodies,
				&limits.EditableMeshes,
				&limits.GeometryElements,
				&limits.GridEntries,
				&limits.RegionMetres
			};
		}
		void WriteBudgets(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *budgets = static_cast<const PhysicsWorkBudget *>(source);
			for (size_t index = 0; index < count; ++index) {
				auto limits = budgets[index].Limits;
				writer.WriteUInt8(SERIALIZER_VERSION);
				for (const auto *field : LimitFields(limits))
					writer.WriteUInt64(*field);
				writer.WriteUInt64(budgets[index].Tick);
				writer.WriteUInt64(budgets[index].Used);
				writer.WriteUInt64(budgets[index].GridEntriesUsed);
				writer.WriteBool(budgets[index].Blocked);
				writer.WriteBool(budgets[index].InProgress);
				writer.WriteUInt8(static_cast<uint8_t>(budgets[index].ExhaustedStage));
				writer.WriteUInt64(budgets[index].WindowTurn);
				writer.WriteUInt64(budgets[index].RegionQuota);
			}
		}
		void ReadBudgets(core::ByteReader &reader, void *destination, size_t count) {
			std::vector<PhysicsWorkBudget> decoded;
			constexpr size_t ENCODED_BYTES = 1 + 8 * 8 + 8 * 3 + 3;
			if (count > reader.Remaining() / ENCODED_BYTES) {
				reader.Fail();
				return;
			}
			decoded.reserve(count);
			for (size_t index = 0; index < count; ++index) {
				PhysicsWorkBudget budget;
				const uint8_t version = reader.ReadUInt8();
				if (version != 1 && version != SERIALIZER_VERSION) {
					reader.Fail();
					return;
				}
				// Version 1 predates `RegionMetres`, the last limit field.
				const auto fields = LimitFields(budget.Limits);
				for (size_t field = 0; field < fields.size() - (version == 1 ? 1 : 0); ++field)
					*fields[field] = reader.ReadUInt64();
				budget.Tick = reader.ReadUInt64();
				budget.Used = reader.ReadUInt64();
				budget.GridEntriesUsed = reader.ReadUInt64();
				const auto blocked = reader.ReadUInt8();
				const auto inProgress = reader.ReadUInt8();
				if (blocked > 1 || inProgress > 1) {
					reader.Fail();
					return;
				}
				budget.Blocked = blocked != 0;
				budget.InProgress = inProgress != 0;
				const auto stage = reader.ReadUInt8();
				if (reader.Failed() || stage > static_cast<uint8_t>(PhysicsWorkStage::CopiedContacts) ||
					(!budget.Blocked && stage != 0)) {
					reader.Fail();
					return;
				}
				budget.ExhaustedStage = static_cast<PhysicsWorkStage>(stage);
				if (version >= 2) {
					budget.WindowTurn = reader.ReadUInt64();
					budget.RegionQuota = reader.ReadUInt64();
				}
				if (reader.Failed()) return;
				if (budget.InProgress) {
					budget.Blocked = true;
					budget.InProgress = false;
					if (budget.ExhaustedStage == PhysicsWorkStage::None)
						budget.ExhaustedStage = PhysicsWorkStage::Admission;
				}
				decoded.push_back(budget);
			}
			std::copy(decoded.begin(), decoded.end(), static_cast<PhysicsWorkBudget *>(destination));
		}
	}

	void RegisterPhysicsWorkBudget() {
		ecs::Components::Register<PhysicsWorkBudget>(
			PHYSICS_WORK_BUDGET_COMPONENT, WriteBudgets, ReadBudgets
		);
	}
	const PhysicsWorkBudget *PhysicsWorkBudgetOf(const ecs::Store &store) {
		if (!ecs::Components::Find(BudgetName()).IsValid()) return nullptr;
		return store.Resource<PhysicsWorkBudget>();
	}
	PhysicsWorkBudget *PreparedWorkBudgetMutable(ecs::Store &store) {
		if (!ecs::Components::Find(BudgetName()).IsValid()) return nullptr;
		return store.ResourceMutable<PhysicsWorkBudget>();
	}
	void SetPhysicsWorkLimits(ecs::Store &store, const PhysicsWorkLimits &limits) {
		if (auto *budget = EnsureWorkBudget(store)) budget->Limits = limits;
	}
	bool PhysicsWorkActive(const ecs::Store &store) {
		const auto *budget = PhysicsWorkBudgetOf(store);
		return budget != nullptr && budget->InProgress;
	}
	[[noreturn]] void FailPhysicsWork(ecs::Store &store, PhysicsWorkStage stage, std::string_view limit) {
		auto *budget = PreparedWorkBudgetMutable(store);
		if (budget != nullptr) {
			if (!budget->Blocked) budget->ExhaustedStage = stage;
			budget->Blocked = true;
			budget->InProgress = false;
		}
		std::string failure =
			"physics work budget exhausted; recreate store or restore an earlier complete snapshot";
		if (budget != nullptr) {
			failure += "; stage=";
			failure += StageName(budget->ExhaustedStage);
			failure +=
				"; work=" + std::to_string(budget->Used) + "/" + std::to_string(budget->Limits.WorkPerTick);
			failure += "; grid entries=" + std::to_string(budget->GridEntriesUsed) + "/" +
					   std::to_string(budget->Limits.GridEntries);
			if (!limit.empty()) {
				failure += "; ";
				failure += limit;
			}
		} else {
			failure += "; stage=";
			failure += StageName(stage);
			failure += "; counters unavailable";
		}
		throw std::runtime_error(failure);
	}

	void ChargePhysicsWork(ecs::Store &store, PhysicsWorkStage stage, uint64_t units) {
		auto *budget = PreparedWorkBudgetMutable(store);
		if (budget == nullptr) return;
		if (budget->Blocked) FailPhysicsWork(store, budget->ExhaustedStage);
		if (!budget->InProgress) return;
		if (budget->Used > budget->Limits.WorkPerTick || units > budget->Limits.WorkPerTick - budget->Used)
			FailPhysicsWork(store, stage);
		budget->Used += units;
	}
	void AdmitPhysicsCount(
		ecs::Store &store, PhysicsWorkStage stage, uint64_t count, uint64_t maximum, std::string_view what
	) {
		ChargePhysicsWork(store, stage, 0);
		if (!PhysicsWorkActive(store)) return;
		if (count > maximum)
			FailPhysicsWork(
				store, stage, std::string(what) + "=" + std::to_string(count) + "/" + std::to_string(maximum)
			);
		ChargePhysicsWork(store, stage, count);
	}
	namespace {
		// Chooses this tick's active moving bodies and returns how many. Each
		// region keeps at most its quota (the smaller of `MovingBodies` and
		// `RegionQuota`), a run in id order starting at `WindowTurn * quota`
		// modulo the region's count. Humanoid root bodies are always active.
		uint64_t AdmitMovingWindow(ecs::Store &store, PhysicsWorkBudget &budget) {
			budget.Windowed = false;
			budget.Active.clear();
			if (budget.RegionQuota == 0) budget.RegionQuota = INITIAL_REGION_QUOTA;
			const uint64_t perRegion = std::min(budget.Limits.MovingBodies, budget.RegionQuota);
			if (perRegion == 0)
				FailPhysicsWork(store, PhysicsWorkStage::Admission, "moving bodies per region=0");

			static thread_local std::vector<uint64_t> pinned;
			pinned.clear();
			store.Each<const scene::Humanoid>([&](ecs::Entity row, const scene::Humanoid &humanoid) {
				const ecs::Entity body = humanoid.RootPart == ecs::NULL_ENTITY ? row : humanoid.RootPart;
				if (store.Has<scene::Motion>(body)) pinned.push_back(body.Id);
			});
			std::sort(pinned.begin(), pinned.end());

			struct Placed {
				std::array<int64_t, 3> Region;
				uint64_t Id;
				bool operator<(const Placed &other) const {
					return Region != other.Region ? Region < other.Region : Id < other.Id;
				}
			};
			static thread_local std::vector<Placed> placed;
			placed.clear();
			const float edge = static_cast<float>(std::max<uint64_t>(budget.Limits.RegionMetres, 1));
			store.Each<const scene::Motion, const scene::Transform>(
				[&](ecs::Entity entity, const scene::Motion &, const scene::Transform &transform) {
					const core::Vector3 &at = transform.Frame.Position;
					const auto cell = [edge](float value) {
						return std::isfinite(value) ? static_cast<int64_t>(std::floor(value / edge)) : 0;
					};
					placed.push_back({{cell(at.X), cell(at.Y), cell(at.Z)}, entity.Id});
				}
			);
			std::sort(placed.begin(), placed.end());

			for (size_t begin = 0; begin < placed.size();) {
				size_t end = begin + 1;
				while (end < placed.size() && placed[end].Region == placed[begin].Region)
					end++;
				const size_t count = end - begin;
				if (count <= perRegion) {
					for (size_t at = begin; at < end; at++)
						budget.Active.push_back(placed[at].Id);
				} else {
					budget.Windowed = true;
					const size_t offset = static_cast<size_t>((budget.WindowTurn * perRegion) % count);
					for (size_t step = 0; step < perRegion; step++)
						budget.Active.push_back(placed[begin + (offset + step) % count].Id);
				}
				begin = end;
			}
			if (!budget.Windowed) {
				budget.Active.clear();
				return placed.size();
			}
			budget.Active.insert(budget.Active.end(), pinned.begin(), pinned.end());
			std::sort(budget.Active.begin(), budget.Active.end());
			budget.Active.erase(std::unique(budget.Active.begin(), budget.Active.end()), budget.Active.end());
			return budget.Active.size();
		}
	}

	void BeginPhysicsWorkTick(ecs::Store &store) {
		auto *budget = EnsureWorkBudget(store);
		if (budget == nullptr) return;
		if (budget->Blocked) FailPhysicsWork(store, budget->ExhaustedStage);
		if (budget->InProgress) {
			if (budget->Tick != store.Time().Tick) FailPhysicsWork(store, PhysicsWorkStage::Admission);
			return;
		}
		if (budget->Tick != store.Time().Tick) {
			budget->Tick = store.Time().Tick;
			budget->Used = 0;
			budget->GridEntriesUsed = 0;
		}
		budget->InProgress = true;
		const auto &limits = budget->Limits;
		ChargePhysicsWork(store, PhysicsWorkStage::Admission, AdmitMovingWindow(store, *budget));
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::Collider>().Count(),
			limits.Colliders,
			"colliders"
		);
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::JointInstance>().Count() +
				store.Query<const scene::WeldConstraint>().Count(),
			limits.Joints,
			"joints"
		);
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::EditableMesh>().Count(),
			limits.EditableMeshes,
			"editable meshes"
		);
		// Resident geometry is bounded by `GeometryElements`. Admitting it reads
		// one size per array, so that is the work charged: charging every
		// resident element against the per-tick work budget made a streamed
		// terrain of a few dozen chunks exhaust it on admission alone, every tick.
		uint64_t geometry = 0;
		auto admitGeometry = [&](size_t elements) {
			if (elements > limits.GeometryElements - geometry)
				FailPhysicsWork(store, PhysicsWorkStage::Admission);
			geometry += elements;
			ChargePhysicsWork(store, PhysicsWorkStage::Admission, 1);
		};
		const auto *shapes = scene::CollisionShapesOf(store);
		if (shapes != nullptr) {
			admitGeometry(shapes->Hulls.size());
			admitGeometry(shapes->Meshes.size());
			for (const auto &hull : shapes->Hulls) {
				admitGeometry(hull.Shape.Points.size());
				admitGeometry(hull.Shape.Faces.size());
				admitGeometry(hull.Shape.Loops.size());
			}
			for (const auto &mesh : shapes->Meshes) {
				admitGeometry(mesh.Shape.Vertices.size());
				admitGeometry(mesh.Shape.Indices.size());
				admitGeometry(mesh.Shape.TriangleBounds.size());
				admitGeometry(mesh.Shape.Hierarchy.size());
				admitGeometry(mesh.Shape.HierarchyTriangles.size());
			}
		}
		if (const auto *ledger = store.Resource<scene::EditableMeshCollision>())
			admitGeometry(ledger->Rows.size());
		// Only meshes a collider names, and only what the collision bake reads:
		// a mesh that is only drawn, and the normals, UVs, colours and alphas
		// of any mesh, are never collided, so they are not physics work.
		std::unordered_set<core::Name> collided;
		store.Each<const scene::Collider>([&](ecs::Entity, const scene::Collider &collider) {
			ChargePhysicsWork(store, PhysicsWorkStage::Admission, 1);
			if (collider.Geometry.IsValid() &&
				(collider.Shape == scene::ShapeKind::Mesh || collider.Shape == scene::ShapeKind::Hull))
				collided.insert(collider.Geometry);
		});
		store.Each<const scene::EditableMesh>([&](ecs::Entity owner, const scene::EditableMesh &mesh) {
			if (!collided.contains(scene::EditableMeshContentName(store, owner))) return;
			// A split mesh collides through its cells, which are counted as theirs.
			if (scene::EditableMeshSplitsCollision(store, owner, mesh)) return;
			admitGeometry(mesh.Positions.size());
			admitGeometry(mesh.Indices.size());
		});
	}
	// Charges, before the refresh runs, for the editable-mesh collision shapes
	// it is about to build, from the same demand the refresh reads. One demand
	// pass and one mesh walk with binary lookups, so the reservation stays
	// linear in a world that streams hundreds of meshes beside many colliders.
	// Returns the sorted instance ids whose bake waits for a later tick.
	std::vector<uint64_t> ReserveEditableMeshWork(ecs::Store &store) {
		std::vector<uint64_t> deferred;
		if (!PhysicsWorkActive(store)) return deferred;
		// The same demand the refresh will act on, so nothing is reserved for
		// a shape it will not build.
		std::vector<scene::EditableMeshCollisionDemand> demand;
		scene::CollectEditableMeshCollisionDemand(store, demand);
		ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, demand.size() + 1);
		if (demand.empty()) return deferred;

		const auto *ledger = store.Resource<scene::EditableMeshCollision>();
		const bool ledgerSorted =
			ledger != nullptr &&
			std::is_sorted(ledger->Rows.begin(), ledger->Rows.end(), [](const auto &left, const auto &right) {
				return left.Instance < right.Instance;
			});
		const auto *shapes = scene::CollisionShapesOf(store);
		store.Each<const scene::EditableMesh>([&](ecs::Entity owner, const scene::EditableMesh &mesh) {
			ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, 1);
			const auto wanted = std::lower_bound(
				demand.begin(),
				demand.end(),
				owner.Id,
				[](const scene::EditableMeshCollisionDemand &row, uint64_t id) { return row.Instance < id; }
			);
			if (wanted == demand.end() || wanted->Instance != owner.Id || mesh.Positions.empty()) return;
			const bool wantsHull = wanted->Hull;

			bool current = false;
			if (ledger != nullptr) {
				if (ledgerSorted) {
					const auto row = std::lower_bound(
						ledger->Rows.begin(),
						ledger->Rows.end(),
						owner.Id,
						[](const scene::EditableMeshCollision::Baked &baked, uint64_t id) {
							return baked.Instance < id;
						}
					);
					current = row != ledger->Rows.end() && row->Instance == owner.Id &&
							  row->Revision == mesh.Revision;
				} else {
					// The refresh canonicalizes the ledger; an unsorted one is a
					// restored or tool-authored ledger seen once.
					for (const auto &row : ledger->Rows) {
						ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, 1);
						if (row.Instance == owner.Id && row.Revision == mesh.Revision) {
							current = true;
							break;
						}
					}
				}
			}
			if (current && shapes != nullptr) {
				const core::Name name = scene::EditableMeshContentName(store, owner);
				if (shapes->FindMesh(name) && (!wantsHull || shapes->FindHull(name))) return;
			}
			// A split mesh bakes empty; its cells are charged as meshes of their own.
			if (scene::EditableMeshSplitsCollision(store, owner, mesh) && !wantsHull) {
				ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, 1);
				return;
			}
			const uint64_t points = mesh.Positions.size();
			const auto *budget = PhysicsWorkBudgetOf(store);
			// Bakes may take half a tick's work; the rest is the solver's.
			const uint64_t share = budget->Limits.WorkPerTick / 2;
			// Quickhull repeatedly scans the point set: quadratic. Mesh-only
			// terrain avoids this cost.
			if (wantsHull && points > share / points) FailPhysicsWork(store, PhysicsWorkStage::EditableMesh);
			const uint64_t triangles = mesh.Indices.size() / 3;
			// Balanced median partitions visit each triangle once per tree level.
			const uint64_t cost =
				(wantsHull ? points * points : 0) + points + triangles * (std::bit_width(triangles) + 8);
			// One mesh that can never fit is a refusal; one that does not fit
			// beside this tick's other bakes waits for a later tick.
			if (cost > share) FailPhysicsWork(store, PhysicsWorkStage::EditableMesh);
			if (budget->Used > share || cost > share - budget->Used) {
				deferred.push_back(owner.Id);
				return;
			}
			ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, cost);
		});
		std::sort(deferred.begin(), deferred.end());
		return deferred;
	}

	void FinishPhysicsWorkTick(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Admission, 0);
		if (auto *budget = PreparedWorkBudgetMutable(store)) {
			// Each crowded region moves on to its next run of bodies.
			if (budget->InProgress && budget->Windowed) budget->WindowTurn++;
			if (budget->InProgress) {
				const uint64_t limit = budget->Limits.WorkPerTick;
				uint64_t &quota = budget->RegionQuota;
				if (quota == 0) quota = INITIAL_REGION_QUOTA;
				if (budget->Used > limit / 4 * 3)
					quota = std::max<uint64_t>(1, quota * 3 / 4);
				else if (budget->Used < limit / 2)
					quota = std::min(budget->Limits.MovingBodies, quota + std::max<uint64_t>(1, quota / 4));
			}
			budget->InProgress = false;
		}
	}
	size_t RemainingPhysicsGridEntries(ecs::Store &store) {
		const auto *budget = PhysicsWorkBudgetOf(store);
		if (budget->GridEntriesUsed > budget->Limits.GridEntries)
			FailPhysicsWork(store, PhysicsWorkStage::BroadphaseSync);
		return static_cast<size_t>(budget->Limits.GridEntries - budget->GridEntriesUsed);
	}

	spatial::TraversalBudget PhysicsTraversalBudget(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Broadphase, 0);
		const auto *budget = PhysicsWorkBudgetOf(store);
		return spatial::TraversalBudget{
			PhysicsWorkActive(store) ? static_cast<size_t>(budget->Limits.WorkPerTick - budget->Used)
									 : std::numeric_limits<size_t>::max()
		};
	}
	void FinishPhysicsTraversal(
		ecs::Store &store, PhysicsWorkStage stage, const spatial::TraversalBudget &budget, size_t initial
	) {
		ChargePhysicsWork(store, stage, initial - budget.Remaining);
		if (budget.Exhausted) FailPhysicsWork(store, stage);
		if (auto *status = PreparedWorkBudgetMutable(store); status && status->InProgress)
			status->GridEntriesUsed += budget.GeneratedEntries;
	}
	void ChargePhysicsPairWork(
		ecs::Store &store, PhysicsWorkStage stage, const ShapeInstance &first, const ShapeInstance &second
	) {
		const ShapeInstance *mesh = first.Mesh ? &first : second.Mesh ? &second : nullptr;
		if (mesh == nullptr) {
			ChargePhysicsWork(store, stage, PhysicsShapeWork(first));
			ChargePhysicsWork(store, stage, PhysicsShapeWork(second));
			return;
		}
		const ShapeInstance &convex = mesh == &first ? second : first;
		ChargePhysicsWork(store, stage, mesh->Mesh->Hierarchy.size());
		ChargePhysicsWork(
			store, stage, std::max(mesh->Mesh->TriangleCount(), mesh->Mesh->HierarchyTriangles.size())
		);
		// Each retained triangle opens its own weighted convex kernel. Separate
		// checked charges avoid overflowing a geometry-size product.
		const size_t kernels = std::min<size_t>(mesh->Mesh->TriangleCount(), 64);
		for (size_t kernel = 0; kernel < kernels; ++kernel) {
			ChargePhysicsWork(store, stage, PhysicsShapeWork(convex));
			ChargePhysicsWork(store, stage, 4); // One kernel and its triangle's three points.
		}
	}

	uint64_t PhysicsPairWork(const ShapeInstance &first, const ShapeInstance &second) {
		constexpr uint64_t MAXIMUM = std::numeric_limits<uint64_t>::max();
		const auto add = [](uint64_t total, uint64_t units) {
			return units > MAXIMUM - total ? MAXIMUM : total + units;
		};
		const ShapeInstance *mesh = first.Mesh ? &first : second.Mesh ? &second : nullptr;
		if (mesh == nullptr) return add(PhysicsShapeWork(first), PhysicsShapeWork(second));
		const ShapeInstance &convex = mesh == &first ? second : first;
		uint64_t total =
			add(mesh->Mesh->Hierarchy.size(),
				std::max(mesh->Mesh->TriangleCount(), mesh->Mesh->HierarchyTriangles.size()));
		const size_t kernels = std::min<size_t>(mesh->Mesh->TriangleCount(), 64);
		for (size_t kernel = 0; kernel < kernels; ++kernel)
			total = add(add(total, PhysicsShapeWork(convex)), 4);
		return total;
	}

	uint64_t PhysicsShapeWork(const ShapeInstance &shape) {
		// One unit is a node/triangle visit or a bounded analytic/convex kernel,
		// not an individual floating-point operation inside that kernel.
		uint64_t work = 1;
		if (shape.Mesh)
			work += shape.Mesh->Hierarchy.size() +
					std::max(shape.Mesh->TriangleCount(), shape.Mesh->HierarchyTriangles.size()) +
					std::min<size_t>(shape.Mesh->TriangleCount(), 64);
		if (shape.Hull)
			work += shape.Hull->Points.size() + shape.Hull->Faces.size() + shape.Hull->Loops.size();
		return work;
	}
}
