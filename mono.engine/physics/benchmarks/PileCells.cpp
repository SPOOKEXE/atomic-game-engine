// The dense 4,000-body scene with three broadphase cell sizes.
//
// The pile is intentionally pathological: every one-metre box is inside the
// same 4m cell in the reference fixture. Smaller cells may reduce false
// candidate pairs, but only if the ordered contact output and simulation state
// remain identical. The preflight checks those outputs before any timings run.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.pile-cells")

using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::CandidatePair;
using engine::physics::ContactEvent;
using engine::physics::ContactManifold;
using engine::physics::IntegrateMotion;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Publish;
using engine::physics::Solve;
using engine::physics::SweepFastBodies;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::Simulated;
using engine::scene::Transform;

namespace pile_cells_bench {
	constexpr size_t BODY_COUNT = 4000;
	constexpr int SETTLE_TICKS = 10;

	struct CellSpec {
		float Width;
		const char *Label;
	};

	constexpr std::array<CellSpec, 3> CELLS{{{4.0f, "4m"}, {2.0f, "2m"}, {1.0f, "1m"}}};

	struct Fixture {
		std::unique_ptr<Store> World;
		std::vector<Entity> Bodies;
	};

	std::array<std::unique_ptr<Fixture>, CELLS.size()> &Fixtures() {
		static std::array<std::unique_ptr<Fixture>, CELLS.size()> fixtures;
		return fixtures;
	}

	void Tick(Fixture &fixture) {
		Store &store = *fixture.World;
		store.AdvanceTick(1.0f / 60.0f);
		IntegrateMotion(store);
		SweepFastBodies(store);
		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		Solve(store);
		Publish(store);
	}

	Fixture &World(size_t cellIndex) {
		auto &fixture = Fixtures().at(cellIndex);
		if (fixture != nullptr) {
			return *fixture;
		}

		fixture = std::make_unique<Fixture>();
		fixture->World = std::make_unique<Store>("physics.bench.pile-cells");
		Store &store = *fixture->World;
		PreparePhysicsWorld(store, CELLS[cellIndex].Width);

		const Entity floor = store.Create();
		store.Set<Transform>(floor, Transform{CFrame{Vector3{0.0f, -1.0f, 0.0f}}});
		Collider ground;
		ground.Extent = Vector3{4096.0f, 1.0f, 4096.0f};
		store.Set<Collider>(floor, ground);

		fixture->Bodies.reserve(BODY_COUNT);
		for (size_t index = 0; index < BODY_COUNT; index++) {
			const auto seed = static_cast<uint32_t>(index);
			const Entity entity = store.Create();
			store.Set<Transform>(
				entity,
				Transform{CFrame{Vector3{
					Random::Range(seed, 3, -1.5f, 1.5f),
					0.5f + Random::Range(seed, 5, 0.0f, 3.0f),
					Random::Range(seed, 7, -1.5f, 1.5f),
				}}}
			);

			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			store.Set<Collider>(entity, collider);
			store.Set<Motion>(entity, Motion{});
			store.Set<RigidBody>(entity, RigidBody{});
			store.Set<Simulated>(entity, Simulated{});
			fixture->Bodies.push_back(entity);
		}

		for (int tick = 0; tick < SETTLE_TICKS; tick++) {
			Tick(*fixture);
		}
		return *fixture;
	}

	bool SameVector(const Vector3 &left, const Vector3 &right) {
		return left == right;
	}

	bool SameManifold(const ContactManifold &left, const ContactManifold &right) {
		if (left.A != right.A || left.B != right.B || !SameVector(left.Normal, right.Normal) ||
			left.PointCount != right.PointCount || left.Trigger != right.Trigger) {
			return false;
		}
		for (size_t point = 0; point < left.PointCount; point++) {
			const auto &first = left.Points[point];
			const auto &second = right.Points[point];
			if (!SameVector(first.Position, second.Position) || first.Penetration != second.Penetration ||
				first.Feature != second.Feature || first.Separation != second.Separation) {
				return false;
			}
		}
		return true;
	}

	bool SameManifolds(const Fixture &left, const Fixture &right) {
		const auto first = left.World->Resource<PhysicsWorld>()->Manifolds();
		const auto second = right.World->Resource<PhysicsWorld>()->Manifolds();
		if (first.size() != second.size()) {
			return false;
		}
		for (size_t index = 0; index < first.size(); index++) {
			if (!SameManifold(first[index], second[index])) {
				return false;
			}
		}
		return true;
	}

	bool SameEvents(const Fixture &left, const Fixture &right) {
		const auto first = left.World->Resource<PhysicsWorld>()->Events();
		const auto second = right.World->Resource<PhysicsWorld>()->Events();
		if (first.size() != second.size()) {
			return false;
		}
		for (size_t index = 0; index < first.size(); index++) {
			const ContactEvent &a = first[index];
			const ContactEvent &b = second[index];
			if (a.A != b.A || a.B != b.B || a.Phase != b.Phase) {
				return false;
			}
		}
		return true;
	}

	bool SameBodies(const Fixture &left, const Fixture &right) {
		if (left.Bodies.size() != right.Bodies.size()) {
			return false;
		}
		const Store &first = *left.World;
		const Store &second = *right.World;
		for (size_t index = 0; index < left.Bodies.size(); index++) {
			const Transform *a = first.Get<Transform>(left.Bodies[index]);
			const Transform *b = second.Get<Transform>(right.Bodies[index]);
			if (a == nullptr || b == nullptr || a->Frame.Position != b->Frame.Position ||
				a->Frame.QuaternionX != b->Frame.QuaternionX ||
				a->Frame.QuaternionY != b->Frame.QuaternionY ||
				a->Frame.QuaternionZ != b->Frame.QuaternionZ ||
				a->Frame.QuaternionW != b->Frame.QuaternionW) {
				return false;
			}

			const Motion *motionA = first.Get<Motion>(left.Bodies[index]);
			const Motion *motionB = second.Get<Motion>(right.Bodies[index]);
			if ((motionA == nullptr) != (motionB == nullptr)) {
				return false;
			}
			if (motionA != nullptr && (!SameVector(motionA->Linear, motionB->Linear) ||
									   !SameVector(motionA->Angular, motionB->Angular))) {
				return false;
			}
		}
		return true;
	}

	bool OrderedPairs(const Fixture &fixture) {
		const auto pairs = fixture.World->Resource<PhysicsWorld>()->Pairs();
		for (size_t index = 0; index < pairs.size(); index++) {
			if (pairs[index].A.Id >= pairs[index].B.Id || (index > 0 && pairs[index] < pairs[index - 1])) {
				return false;
			}
		}
		return true;
	}

	bool FinePairsAreWithinCoarse(const Fixture &coarse, const Fixture &fine) {
		const auto broad = coarse.World->Resource<PhysicsWorld>()->Pairs();
		const auto narrow = fine.World->Resource<PhysicsWorld>()->Pairs();
		return std::includes(broad.begin(), broad.end(), narrow.begin(), narrow.end());
	}

	void EnsureParity() {
		static bool checked = false;
		if (checked) {
			return;
		}

		Fixture &fourMetres = World(0);
		Fixture &twoMetres = World(1);
		Fixture &oneMetre = World(2);
		if (!OrderedPairs(fourMetres) || !OrderedPairs(twoMetres) || !OrderedPairs(oneMetre)) {
			throw std::runtime_error("pile cell study produced a non-canonical pair order");
		}
		if (!FinePairsAreWithinCoarse(fourMetres, twoMetres) ||
			!FinePairsAreWithinCoarse(fourMetres, oneMetre)) {
			throw std::runtime_error("finer pile cells produced a candidate absent from the 4m cell list");
		}
		if (!SameManifolds(fourMetres, twoMetres) || !SameManifolds(fourMetres, oneMetre)) {
			throw std::runtime_error("pile cell sizes changed ordered contact manifolds");
		}
		if (!SameEvents(fourMetres, twoMetres) || !SameEvents(fourMetres, oneMetre)) {
			throw std::runtime_error("pile cell sizes changed ordered contact events");
		}
		if (!SameBodies(fourMetres, twoMetres) || !SameBodies(fourMetres, oneMetre)) {
			throw std::runtime_error("pile cell sizes changed body state after the same 10 ticks");
		}

		for (size_t index = 0; index < CELLS.size(); index++) {
			const auto *world = World(index).World->Resource<PhysicsWorld>();
			std::fprintf(
				stderr,
				"pile-cell-study width=%s pairs=%zu manifolds=%zu events=%zu\n",
				CELLS[index].Label,
				world->Pairs().size(),
				world->Manifolds().size(),
				world->Events().size()
			);
		}
		checked = true;
	}

	void ConsumeWorld(const Fixture &fixture) {
		const PhysicsWorld *world = fixture.World->Resource<PhysicsWorld>();
		engine::testing::Consume(world->Pairs().size());
		engine::testing::Consume(world->Manifolds().size());
		engine::testing::Consume(world->Events().size());
	}
}

using namespace pile_cells_bench;

BENCH("Pile · 4m grid · full tick", 1) {
	EnsureParity();
	Fixture &fixture = World(0);
	Tick(fixture);
	ConsumeWorld(fixture);
}

BENCH("Pile · 2m grid · full tick", 1) {
	EnsureParity();
	Fixture &fixture = World(1);
	Tick(fixture);
	ConsumeWorld(fixture);
}

BENCH("Pile · 1m grid · full tick", 1) {
	EnsureParity();
	Fixture &fixture = World(2);
	Tick(fixture);
	ConsumeWorld(fixture);
}

BENCH("Pile · 4m grid · SyncBroadphase", 1) {
	EnsureParity();
	Fixture &fixture = World(0);
	SyncBroadphase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 2m grid · SyncBroadphase", 1) {
	EnsureParity();
	Fixture &fixture = World(1);
	SyncBroadphase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 1m grid · SyncBroadphase", 1) {
	EnsureParity();
	Fixture &fixture = World(2);
	SyncBroadphase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 4m grid · BroadPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(0);
	BroadPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 2m grid · BroadPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(1);
	BroadPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 1m grid · BroadPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(2);
	BroadPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 4m grid · NarrowPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(0);
	NarrowPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 2m grid · NarrowPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(1);
	NarrowPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 1m grid · NarrowPhase", 1) {
	EnsureParity();
	Fixture &fixture = World(2);
	NarrowPhase(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 4m grid · Solve", 1) {
	EnsureParity();
	Fixture &fixture = World(0);
	Solve(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 2m grid · Solve", 1) {
	EnsureParity();
	Fixture &fixture = World(1);
	Solve(*fixture.World);
	ConsumeWorld(fixture);
}

BENCH("Pile · 1m grid · Solve", 1) {
	EnsureParity();
	Fixture &fixture = World(2);
	Solve(*fixture.World);
	ConsumeWorld(fixture);
}
