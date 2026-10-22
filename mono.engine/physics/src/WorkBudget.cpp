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
#include <engine/scene/EditableMesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::physics {
	namespace {
		constexpr uint8_t SERIALIZER_VERSION = 1;
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
				&limits.GridEntries
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
				if (reader.ReadUInt8() != SERIALIZER_VERSION) {
					reader.Fail();
					return;
				}
				for (auto *field : LimitFields(budget.Limits))
					*field = reader.ReadUInt64();
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
	[[noreturn]] void FailPhysicsWork(ecs::Store &store, PhysicsWorkStage stage) {
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
	void AdmitPhysicsCount(ecs::Store &store, PhysicsWorkStage stage, uint64_t count, uint64_t maximum) {
		ChargePhysicsWork(store, stage, 0);
		if (!PhysicsWorkActive(store)) return;
		if (count > maximum) FailPhysicsWork(store, stage);
		ChargePhysicsWork(store, stage, count);
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
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::Motion, const scene::Transform>().Count(),
			limits.MovingBodies
		);
		AdmitPhysicsCount(
			store, PhysicsWorkStage::Admission, store.Query<const scene::Collider>().Count(), limits.Colliders
		);
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::JointInstance>().Count() +
				store.Query<const scene::WeldConstraint>().Count(),
			limits.Joints
		);
		AdmitPhysicsCount(
			store,
			PhysicsWorkStage::Admission,
			store.Query<const scene::EditableMesh>().Count(),
			limits.EditableMeshes
		);
		uint64_t geometry = 0;
		auto admitGeometry = [&](size_t elements) {
			if (elements > limits.GeometryElements - geometry)
				FailPhysicsWork(store, PhysicsWorkStage::Admission);
			geometry += elements;
			ChargePhysicsWork(store, PhysicsWorkStage::Admission, elements);
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
		store.Each<const scene::EditableMesh>([&](ecs::Entity, const scene::EditableMesh &mesh) {
			admitGeometry(mesh.Positions.size());
			admitGeometry(mesh.Normals.size());
			admitGeometry(mesh.UVs.size());
			admitGeometry(mesh.Colours.size());
			admitGeometry(mesh.Alphas.size());
			admitGeometry(mesh.Indices.size());
		});
	}
	void ReserveEditableMeshWork(ecs::Store &store) {
		if (!PhysicsWorkActive(store)) return;
		const auto *ledger = store.Resource<scene::EditableMeshCollision>();
		store.Each<const scene::EditableMesh>([&](ecs::Entity owner, const scene::EditableMesh &mesh) {
			bool current = false;
			if (ledger != nullptr) {
				for (const auto &row : ledger->Rows) {
					ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, 1);
					if (row.Instance == owner.Id && row.Revision == mesh.Revision) {
						current = true;
						break;
					}
				}
			}

			const auto name = scene::EditableMeshContentName(store, owner);
			bool demanded = false;
			bool wantsHull = false;
			store.Each<const scene::Collider>([&](ecs::Entity, const scene::Collider &collider) {
				ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, 1);
				if (collider.Geometry == name &&
					(collider.Shape == scene::ShapeKind::Mesh || collider.Shape == scene::ShapeKind::Hull))
					demanded = true;
				if (collider.Geometry == name && collider.Shape == scene::ShapeKind::Hull) wantsHull = true;
			});
			if (!demanded || mesh.Positions.empty()) return;
			const auto *shapes = scene::CollisionShapesOf(store);
			if (current && shapes && shapes->FindMesh(name) && (!wantsHull || shapes->FindHull(name))) return;
			const uint64_t points = mesh.Positions.size();
			if (wantsHull) {
				// Quickhull repeatedly scans the point set. Refuse its quadratic
				// reservation before dispatch; mesh-only terrain avoids this cost.
				const auto *budget = PhysicsWorkBudgetOf(store);
				if (points > budget->Limits.WorkPerTick / points)
					FailPhysicsWork(store, PhysicsWorkStage::EditableMesh);
				ChargePhysicsWork(store, PhysicsWorkStage::EditableMesh, points * points);
			}
			const uint64_t triangles = mesh.Indices.size() / 3;
			// Balanced median partitions visit each triangle once per tree level.
			ChargePhysicsWork(
				store, PhysicsWorkStage::EditableMesh, points + triangles * (std::bit_width(triangles) + 8)
			);
		});
	}

	void FinishPhysicsWorkTick(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Admission, 0);
		if (auto *budget = PreparedWorkBudgetMutable(store)) budget->InProgress = false;
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
