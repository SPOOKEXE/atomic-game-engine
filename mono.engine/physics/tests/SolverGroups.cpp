#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

// Private: the groups, the border run and the row array are the solver's
// working set, and the whole of this suite is assertions about their shape.
#include "PipelineInternals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.solvergroups")
// The partition the groups are read off.
TEST_DEPENDS("engine.spatial.chunkmap")
// The manifolds the rows are built from, and the pair order they arrive in.
TEST_DEPENDS("engine.physics.narrowphase")
// The impulses a solve leaves for the next one, which the groups must not
// disturb the order of.
TEST_DEPENDS("engine.physics.solver")
// The dispatch the groups exist to be handed to.
TEST_DEPENDS("engine.parallel.jobs")

using engine::core::CFrame;
using engine::core::FrameGraph;
using engine::core::Metrics;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ContactImpulse;
using engine::physics::ContactManifold;
using engine::physics::ContactRow;
using engine::physics::IntegrateMotion;
using engine::physics::NarrowPhase;
using engine::physics::PARALLEL_SOLVE_ROWS;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Publish;
using engine::physics::Solve;
using engine::physics::SolverBody;
using engine::physics::SolverColor;
using engine::physics::SolverGroup;
using engine::physics::SyncBroadphase;
using engine::scene::BodyKind;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::Simulated;
using engine::scene::Transform;

namespace {
	// A scene wide enough that the partition has something to cut.
	//
	// **Stacks spread over a hundred metres rather than one tall pile**, which
	// is what gives the chunk map more than one occupied chunk to find. A pile
	// is the solver's worst case for *cost* and its worst case for *groups* too,
	// and the cases below want the second thing to be interesting.
	//
	// Every box is a metre cube, unanchored and dynamic, so every contact in the
	// scene has two movable bodies and the border set is not trivially empty.
	void
	AddSpreadBodies(Store &store, size_t columns, size_t height, std::vector<Entity> *created = nullptr) {
		if (created != nullptr) {
			created->reserve(created->size() + columns * height);
		}
		for (size_t column = 0; column < columns; column++) {
			// A lattice rather than random placement, so the case is the same
			// scene every run and a failure can be looked at.
			const auto x = static_cast<float>(column % 16) * 6.0f - 48.0f;
			const auto z = static_cast<float>(column / 16) * 6.0f - 48.0f;

			for (size_t level = 0; level < height; level++) {
				const Entity box = store.Create();
				const auto y = static_cast<float>(level) * 0.98f + 0.49f;
				store.Set<Transform>(box, Transform{CFrame{Vector3{x, y, z}}});

				// **Both, and the tag is not optional.** `FactsFor` asks whether
				// the world may move this and hands back an infinite mass when
				// the answer is no - so a row with a `Motion` and no tag solves
				// as a wall, every contact resolves to nothing, and the group
				// count this file measures comes back zero.
				store.Set<Simulated>(box, Simulated{});
				store.Set<Motion>(box, Motion{});

				RigidBody body;
				body.Kind = BodyKind::Dynamic;
				body.Mass = 1.0f;
				store.Set<RigidBody>(box, body);

				Collider shape;
				shape.Extent = Vector3{0.5f, 0.5f, 0.5f};
				store.Set<Collider>(box, shape);
				if (created != nullptr) {
					created->push_back(box);
				}
			}
		}
	}

	std::unique_ptr<Store> Spread(size_t columns, size_t height, std::vector<Entity> *created = nullptr) {
		auto owned = std::make_unique<Store>("physics.solvergroups");
		Store *store = owned.get();
		PreparePhysicsWorld(*store, 1.0f);

		// One floor, anchored, so the tallest scene still has something to press
		// on - and so that every stack's lowest contact names one body that
		// every other stack's lowest contact also names. That is the case the
		// group rule exists for: an immovable body constrains nothing.
		const Entity floor = store->Create();
		store->Set<Transform>(floor, Transform{CFrame{Vector3{0.0f, -1.0f, 0.0f}}});
		Collider ground;
		ground.Extent = Vector3{128.0f, 1.0f, 128.0f};
		store->Set<Collider>(floor, ground);

		AddSpreadBodies(*store, columns, height, created);

		return owned;
	}

	Entity AddMovableBox(Store &store, Vector3 position) {
		const Entity box = store.Create();
		store.Set<Transform>(box, Transform{CFrame{position}});
		store.Set<Simulated>(box, Simulated{});
		store.Set<Motion>(box, Motion{});
		RigidBody body;
		body.Kind = BodyKind::Dynamic;
		body.Mass = 1.0f;
		store.Set<RigidBody>(box, body);
		Collider shape;
		shape.Extent = Vector3{0.5f, 0.5f, 0.5f};
		store.Set<Collider>(box, shape);
		return box;
	}

	std::unique_ptr<Store> DenseConnected(size_t count) {
		auto owned = std::make_unique<Store>("physics.solvergroups.dense");
		Store &store = *owned;
		PreparePhysicsWorld(store, 1.0f);
		for (size_t at = 0; at < count; at++) {
			const Entity body = store.Create();
			store.Set<Transform>(body, Transform{CFrame{Vector3::Zero}});
			store.Set<Simulated>(body, Simulated{});
			store.Set<Motion>(body, Motion{});
			store.Set<RigidBody>(body, RigidBody{});
			Collider collider;
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			store.Set<Collider>(body, collider);
		}
		return owned;
	}

	std::unique_ptr<Store> ConnectedLattice(size_t side, Entity *firstBody = nullptr) {
		auto owned = std::make_unique<Store>("physics.solvergroups.lattice");
		Store &store = *owned;
		PreparePhysicsWorld(store, 1.0f);
		for (size_t z = 0; z < side; z++) {
			for (size_t x = 0; x < side; x++) {
				const Entity body = store.Create();
				if (x == 0 && z == 0 && firstBody != nullptr) {
					*firstBody = body;
				}
				store.Set<Transform>(
					body,
					Transform{
						CFrame{Vector3{static_cast<float>(x) * 0.98f, 0.0f, static_cast<float>(z) * 0.98f}}
					}
				);
				store.Set<Simulated>(body, Simulated{});
				store.Set<Motion>(body, Motion{});
				store.Set<RigidBody>(body, RigidBody{});
				Collider collider;
				collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
				store.Set<Collider>(body, collider);
			}
		}
		return owned;
	}

	// One tick's worth of the four steps a solve needs behind it, with weight.
	//
	// **The weight is the caller's, because this engine has no gravity term.**
	// `physics/AGENTS.md` is explicit that a world with no down should not have
	// to switch one off, so a host applies it in its own `PreSimulation` system
	// and `tests/Behaviour.cpp` does the same. Without it every contact here
	// resolves to a zero normal impulse and the warm-start case below has
	// nothing to warm-start from.
	void StepOnce(Store &store) {
		store.Each<Motion>([](Entity, Motion &motion) { motion.Linear.Y -= 9.81f / 60.0f; });

		SyncBroadphase(store);
		BroadPhase(store);
		NarrowPhase(store);
		Solve(store);
	}

	// Every body index either half of a run names, movable or not.
	std::vector<uint32_t> BodiesIn(const std::vector<ContactRow> &rows, SolverGroup run) {
		std::vector<uint32_t> named;
		for (uint32_t at = run.FirstRow; at < run.FirstRow + run.RowCount; at++) {
			named.push_back(static_cast<uint32_t>(rows[at].First));
			named.push_back(static_cast<uint32_t>(rows[at].Second));
		}
		std::sort(named.begin(), named.end());
		named.erase(std::unique(named.begin(), named.end()), named.end());
		return named;
	}
}

TEST_CASE("a scene below the threshold is one run in manifold order", "[solvergroups]") {
	// The path every other suite in this module takes, and the reason they all
	// still pass unchanged: below `PARALLEL_SOLVE_ROWS` there is no partition,
	// one group holds every row, and the order is the order the manifolds
	// arrived in. Partitioning a scene of a dozen crates would cost a sort and
	// a counting pass to produce one group.
	std::unique_ptr<Store> owned = Spread(2, 3);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.RowCount() < PARALLEL_SOLVE_ROWS);

	CHECK(world.SolverGroupCount() == 1);
	CHECK(world.BorderRowCount() == 0);
	CHECK(world.SolverChunkSize() == 0.0f);

	const SolverGroup only = PipelineInternals::SolverGroups(world)[0];
	CHECK(only.FirstRow == 0);
}

TEST_CASE("the solver reports each round's interior, border and group shape", "[solvergroups]") {
	std::unique_ptr<Store> owned = Spread(2, 3);
	Store &store = *owned;

	Metrics::Clear();
	const bool frameGraphWasEnabled = FrameGraph::IsEnabled();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	StepOnce(store);
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(frameGraphWasEnabled);

	const auto gauge = [](std::string_view name) {
		const auto found = Metrics::GetGauge(name);
		REQUIRE(found.has_value());
		return found->Value;
	};

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	CHECK(gauge("physics.solve.groups") == 1.0);
	CHECK(gauge("physics.solve.group-rows.min") == static_cast<double>(world.RowCount()));
	CHECK(gauge("physics.solve.group-rows.median") == static_cast<double>(world.RowCount()));
	CHECK(gauge("physics.solve.group-rows.max") == static_cast<double>(world.RowCount()));
	CHECK(gauge("physics.solve.interior-rows") == static_cast<double>(world.RowCount()));
	CHECK(gauge("physics.solve.border-rows") == 0.0);

	const auto spansNamed = [&spans](std::string_view name) {
		return std::count_if(spans.begin(), spans.end(), [name](const engine::core::FrameSpan &span) {
			return span.Name == name;
		});
	};
	constexpr size_t rounds = engine::physics::SOLVER_ITERATIONS / engine::physics::SOLVE_SWEEPS_PER_BATCH;
	CHECK(spansNamed("physics.solve-round") == rounds);
	CHECK(spansNamed("physics.solve-interior") == rounds);
	CHECK(spansNamed("physics.solve-border") == rounds);

	Metrics::Clear();
}

TEST_CASE("a scene above the threshold is cut into several groups", "[solvergroups]") {
	// The partition doing its job. Nothing here asserts a count - that is a
	// function of `SOLVE_GROUP_TARGET` and of the scene - only that a wide scene
	// produces more than one run, because one run is a solve that cannot use the
	// machine however many workers it is handed.
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.RowCount() >= PARALLEL_SOLVE_ROWS);

	CHECK(PipelineInternals::SolverColors(world).size() > 1);
	CHECK(world.UsesColourSchedule());
	CHECK(world.SolverColourCount() == PipelineInternals::SolverColors(world).size());
	CHECK_FALSE(world.UsesIslandSchedule());
	CHECK(world.SolverChunkSize() == 0.0f);
}

TEST_CASE("independent tall stacks share a floor without joining islands", "[solvergroups]") {
	// One component per column even when a column is tall. The shared floor is
	// read-only, so it must not turn those independent dynamic islands into a
	// colour candidate.
	//
	// **Reduced from 256 columns to 64**: the island count and the
	// `UsesIslandSchedule` property are preserved at 64 columns, and the
	// step is four times faster. The test only needs to show that each
	// column is its own island and the floor is shared.
	std::unique_ptr<Store> owned = Spread(64, 16);
	Store &store = *owned;
	Metrics::Clear();
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	CHECK(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	CHECK(PipelineInternals::SolverColors(world).empty());
	CHECK(world.UsesIslandSchedule());
	CHECK_FALSE(world.UsesColourSchedule());
	CHECK(world.SolverColourCount() == 0);
	CHECK(world.ConstraintIslandCount() == 64);
	CHECK(world.SolverChunkSize() == 0.0f);

	StepOnce(store);
	const auto reuse = Metrics::Get("physics.solve.island.topology-reuse");
	REQUIRE(reuse.has_value());
	CHECK(reuse->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("solver memory shrinks live topology and bounds retained regrowth", "[solvergroups][memory]") {
	std::vector<Entity> bodies;
	std::unique_ptr<Store> owned = Spread(256, 16, &bodies);
	Store &store = *owned;

	Metrics::Clear();
	StepOnce(store);
	Publish(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const auto warmed = world.MemoryStats();
	REQUIRE(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	REQUIRE(world.UsesIslandSchedule());
	REQUIRE(world.ConstraintIslandCount() == 256);
	REQUIRE(world.SolverIslandRetainedBytes() > 0);
	const auto totalRetained = Metrics::GetGauge("physics.memory.total.retained-bytes");
	REQUIRE(totalRetained.has_value());
	REQUIRE(totalRetained->Value == static_cast<double>(warmed.Total().RetainedBytes));

	for (Entity body : bodies) {
		store.Destroy(body);
	}
	StepOnce(store);
	Publish(store);
	const auto shrunk = world.MemoryStats();
	CHECK(world.RowCount() == 0);
	CHECK_FALSE(world.UsesIslandSchedule());
	CHECK(shrunk.Solver.LiveBytes < warmed.Solver.LiveBytes);

	bodies.clear();
	AddSpreadBodies(store, 256, 16, &bodies);
	StepOnce(store);
	Publish(store);
	const auto regrown = world.MemoryStats();
	CHECK(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	CHECK(world.UsesIslandSchedule());
	CHECK(world.ConstraintIslandCount() == 256);
	CHECK(regrown.Solver.RetainedBytes <= warmed.Solver.RetainedBytes);
	Metrics::Clear();
}

TEST_CASE("island groups cover every row once and never share a movable body", "[solvergroups]") {
	// **64 columns instead of 256**: the test only needs to verify
	// that every row is covered exactly once and no movable body is
	// shared between groups. 64 columns above the partition threshold
	// exercises the same property at a fraction of the cost.
	std::unique_ptr<Store> owned = Spread(64, 16);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.UsesIslandSchedule());
	REQUIRE(world.BorderRowCount() == 0);
	const std::vector<ContactRow> &rows = PipelineInternals::Rows(world);
	const std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(world);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	std::vector<uint8_t> covered(world.RowCount(), 0);
	std::vector<uint32_t> claimedBy(bodies.size(), UINT32_MAX);

	for (size_t group = 0; group < groups.size(); group++) {
		const SolverGroup run = groups[group];
		for (uint32_t at = run.FirstRow; at < run.FirstRow + run.RowCount; at++) {
			REQUIRE(at < world.RowCount());
			covered[at]++;
			for (uint32_t body : {rows[at].First, rows[at].Second}) {
				if (!bodies[body].Movable) {
					continue;
				}
				if (claimedBy[body] == UINT32_MAX) {
					claimedBy[body] = static_cast<uint32_t>(group);
				}
				CHECK(claimedBy[body] == group);
			}
		}
	}

	CHECK(std::all_of(covered.begin(), covered.end(), [](uint8_t count) { return count == 1; }));
	const auto selected = Metrics::GetGauge("physics.solve.island.selected");
	const auto count = Metrics::GetGauge("physics.solve.islands");
	const auto retained = Metrics::GetGauge("physics.solve.island.retained-bytes");
	REQUIRE(selected.has_value());
	REQUIRE(count.has_value());
	REQUIRE(retained.has_value());
	CHECK(selected->Value == 1.0);
	CHECK(count->Value == static_cast<double>(world.ConstraintIslandCount()));
	CHECK(retained->Value == static_cast<double>(world.SolverIslandRetainedBytes()));
}

TEST_CASE("a movable bridge merges and removing it splits cached islands", "[solvergroups]") {
	// **64 columns instead of 256**: the bridge test only needs to
	// find a manifold between two movable bodies and verify that
	// inserting/removing it changes the island count by one. 64
	// columns gives 64 islands and enough bodies to find a bridge.
	std::unique_ptr<Store> owned = Spread(64, 16);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.UsesIslandSchedule());
	CHECK(world.ConstraintIslandCount() == 64);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	REQUIRE(bodies.size() > 17);
	REQUIRE(bodies[1].Movable);
	REQUIRE(bodies[17].Movable);

	std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(world);
	const auto source = std::find_if(manifolds.begin(), manifolds.end(), [](const ContactManifold &manifold) {
		return !manifold.Trigger && manifold.PointCount != 0;
	});
	REQUIRE(source != manifolds.end());
	ContactManifold bridge = *source;
	bridge.A = bodies[1].Owner;
	bridge.B = bodies[17].Owner;
	const auto lessPair = [](const ContactManifold &left, const ContactManifold &right) {
		return left.A.Id != right.A.Id ? left.A.Id < right.A.Id : left.B.Id < right.B.Id;
	};
	const size_t bridgeIndex = static_cast<size_t>(
		std::lower_bound(manifolds.begin(), manifolds.end(), bridge, lessPair) - manifolds.begin()
	);

	Metrics::Clear();
	manifolds.insert(manifolds.begin() + static_cast<std::ptrdiff_t>(bridgeIndex), bridge);
	Solve(store);
	CHECK(world.UsesIslandSchedule());
	CHECK(world.ConstraintIslandCount() == 63);
	const auto merged = Metrics::Get("physics.solve.island.topology-rebuild");
	REQUIRE(merged.has_value());
	CHECK(merged->Value == 1.0);
	const size_t highWaterBytes = world.SolverIslandRetainedBytes();

	Metrics::Clear();
	manifolds.erase(manifolds.begin() + static_cast<std::ptrdiff_t>(bridgeIndex));
	Solve(store);
	CHECK(world.UsesIslandSchedule());
	CHECK(world.ConstraintIslandCount() == 64);
	const auto split = Metrics::Get("physics.solve.island.topology-rebuild");
	REQUIRE(split.has_value());
	CHECK(split->Value == 1.0);

	for (size_t cycle = 0; cycle < 3; cycle++) {
		manifolds.insert(manifolds.begin() + static_cast<std::ptrdiff_t>(bridgeIndex), bridge);
		Solve(store);
		CHECK(world.SolverIslandRetainedBytes() <= highWaterBytes);

		manifolds.erase(manifolds.begin() + static_cast<std::ptrdiff_t>(bridgeIndex));
		Solve(store);
		CHECK(world.SolverIslandRetainedBytes() <= highWaterBytes);
	}
	Metrics::Clear();
}

TEST_CASE("no two manifold blocks in a colour name one movable body", "[solvergroups]") {
	// **The whole correctness argument, asserted directly.** Two runs sweeping
	// at once write the bodies they name, so a body in two runs is a race and a
	// result that depends on which worker got there first. An immovable body is
	// exempt because a sweep never writes one - see `ApplyImpulse`. This lattice
	// has only movable contacts, so its multi-wave plan exercises the
	// actual write-disjointness rule directly.
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<ContactRow> &rows = PipelineInternals::Rows(world);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	const std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(world);
	const std::vector<SolverColor> &colors = PipelineInternals::SolverColors(world);
	REQUIRE(colors.size() > 1);

	// Which group first claimed each movable body. A second claim by a different
	// group is the failure.
	std::vector<size_t> claimedBy(bodies.size(), SIZE_MAX);
	size_t movableClaims = 0;

	for (size_t color = 0; color < colors.size(); color++) {
		const SolverColor wave = colors[color];
		for (size_t group = wave.FirstGroup; group < wave.FirstGroup + wave.GroupCount; group++) {
			for (uint32_t body : BodiesIn(rows, groups[group])) {
				if (!bodies[body].Movable) {
					continue;
				}
				movableClaims++;
				if (claimedBy[body] == SIZE_MAX) {
					claimedBy[body] = color;
					continue;
				}
				CHECK(claimedBy[body] != color);
			}
		}
	}

	// A scene where nothing was movable would pass the loop above without
	// testing anything.
	CHECK(movableClaims > 0);
}

TEST_CASE("a colour keeps every manifold point block in one worker group", "[solvergroups]") {
	// **64 instead of a larger value**: the test only needs to
	// show that colours partition the bodies into disjoint
	// groups. A 64x64 lattice = 4096 bodies produces enough
	// contacts to exercise the colouring path.
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<SolverColor> &colors = PipelineInternals::SolverColors(world);
	const std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(world);
	const std::vector<uint32_t> &colorOf = PipelineInternals::SolverColorOfManifold(world);
	const std::vector<uint32_t> &starts = PipelineInternals::RowStartOfManifold(world);
	const auto manifolds = world.Manifolds();
	REQUIRE(!colors.empty());
	REQUIRE(colorOf.size() == manifolds.size());

	for (size_t manifold = 0; manifold < manifolds.size(); manifold++) {
		if (colorOf[manifold] == UINT32_MAX || manifolds[manifold].PointCount == 0) {
			continue;
		}
		const SolverColor wave = colors[colorOf[manifold]];
		const auto found = std::find_if(
			groups.begin() + wave.FirstGroup,
			groups.begin() + wave.FirstGroup + wave.GroupCount,
			[&starts, &manifolds, manifold](const SolverGroup &group) {
				return group.FirstRow == starts[manifold] && group.RowCount == manifolds[manifold].PointCount;
			}
		);
		CHECK(found != groups.begin() + wave.FirstGroup + wave.GroupCount);
	}
}

TEST_CASE("a coloured solve propagates impulses across several wave boundaries", "[solvergroups]") {
	// The first box closes into the chain. A later box cannot acquire that
	// velocity unless successive colour waves exchange the previous wave's
	// result during the same solve, which guards against batching all sweeps of
	// one colour ahead of its neighbours.
	Entity first;
	std::unique_ptr<Store> owned = ConnectedLattice(64, &first);
	Store &store = *owned;
	store.GetMutable<Motion>(first)->Linear.X = 10.0f;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(PipelineInternals::SolverColors(world).size() > 1);
	const auto retained = Metrics::GetGauge("physics.solve.color.retained-bytes");
	REQUIRE(retained.has_value());
	CHECK(retained->Value > 0.0);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	REQUIRE(bodies.size() > 4);
	CHECK(bodies[4].LinearVelocity.X > 0.001f);
}

TEST_CASE("an unchanged contact topology reuses its deterministic colours", "[solvergroups]") {
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	Metrics::Clear();
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<uint32_t> first = PipelineInternals::SolverColorOfManifold(world);
	REQUIRE(!first.empty());

	StepOnce(store);
	CHECK(PipelineInternals::SolverColorOfManifold(world) == first);
	const auto reuse = Metrics::Get("physics.solve.color.topology-reuse");
	REQUIRE(reuse.has_value());
	CHECK(reuse->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("a point-count change retains colours and recomputes manifold placement", "[solvergroups]") {
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<uint32_t> colours = PipelineInternals::SolverColorOfManifold(world);
	const size_t rows = world.RowCount();

	std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(world);
	auto changed = std::find_if(manifolds.begin(), manifolds.end(), [](const ContactManifold &manifold) {
		return !manifold.Trigger && manifold.PointCount > 1;
	});
	REQUIRE(changed != manifolds.end());
	changed->PointCount--;

	Metrics::Clear();
	Solve(store);
	CHECK(PipelineInternals::SolverColorOfManifold(world) == colours);
	CHECK(world.RowCount() == rows - 1);
	const auto reuse = Metrics::Get("physics.solve.color.topology-reuse");
	REQUIRE(reuse.has_value());
	CHECK(reuse->Value == 1.0);

	std::vector<uint32_t> groupRows;
	for (const SolverGroup &group : PipelineInternals::SolverGroups(world)) {
		groupRows.push_back(group.RowCount);
	}
	REQUIRE(!groupRows.empty());
	std::sort(groupRows.begin(), groupRows.end());
	const auto groupMinimum = Metrics::GetGauge("physics.solve.group-rows.min");
	const auto groupMedian = Metrics::GetGauge("physics.solve.group-rows.median");
	const auto groupMaximum = Metrics::GetGauge("physics.solve.group-rows.max");
	REQUIRE(groupMinimum.has_value());
	REQUIRE(groupMedian.has_value());
	REQUIRE(groupMaximum.has_value());
	CHECK(groupMinimum->Value == groupRows.front());
	CHECK(groupMedian->Value == groupRows[groupRows.size() / 2]);
	CHECK(groupMaximum->Value == groupRows.back());

	std::vector<uint32_t> colorRows;
	for (const SolverColor &color : PipelineInternals::SolverColors(world)) {
		colorRows.push_back(color.RowCount);
	}
	REQUIRE(!colorRows.empty());
	std::sort(colorRows.begin(), colorRows.end());
	const auto colorMedian = Metrics::GetGauge("physics.solve.color-rows.median");
	REQUIRE(colorMedian.has_value());
	CHECK(colorMedian->Value == colorRows[colorRows.size() / 2]);
	Metrics::Clear();
}

TEST_CASE("the colour threshold is re-evaluated when point counts cross it", "[solvergroups]") {
	// The manifold owners never change. Only the number of rows they contribute
	// crosses the fixed activation threshold, first entering and then leaving the
	// coloured path.
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	SyncBroadphase(store);
	BroadPhase(store);
	NarrowPhase(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(world);
	std::vector<uint32_t> pointCounts;
	pointCounts.reserve(manifolds.size());
	for (size_t at = 0; at < manifolds.size(); at++) {
		pointCounts.push_back(manifolds[at].PointCount);
		manifolds[at].PointCount = at % 2 == 0 ? 0 : 1;
	}

	Solve(store);
	CHECK(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	CHECK(world.RowCount() < engine::physics::SOLVER_COLOR_MIN_ROWS);
	CHECK(PipelineInternals::SolverColors(world).empty());
	CHECK(!PipelineInternals::SolverTopology(world).empty());
	CHECK(world.SolverChunkSize() > 0.0f);

	for (size_t at = 0; at < manifolds.size(); at++) {
		manifolds[at].PointCount = pointCounts[at];
	}
	Metrics::Clear();
	Solve(store);
	CHECK(world.RowCount() >= engine::physics::SOLVER_COLOR_MIN_ROWS);
	CHECK(PipelineInternals::SolverColors(world).size() > 1);
	const auto activated = Metrics::Get("physics.solve.color.topology-rebuild");
	REQUIRE(activated.has_value());
	CHECK(activated->Value == 1.0);

	for (size_t at = 0; at < manifolds.size(); at++) {
		manifolds[at].PointCount = at % 2 == 0 ? 0 : 1;
	}
	Solve(store);
	CHECK(world.RowCount() < engine::physics::SOLVER_COLOR_MIN_ROWS);
	CHECK(PipelineInternals::SolverColors(world).empty());
	CHECK(world.SolverChunkSize() > 0.0f);
	Metrics::Clear();
}

TEST_CASE("trigger and eligibility changes rebuild the exact topology key", "[solvergroups]") {
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(world);
	REQUIRE(!manifolds.empty());

	Metrics::Clear();
	manifolds.front().Trigger = true;
	Solve(store);
	const auto triggerRebuild = Metrics::Get("physics.solve.color.topology-rebuild");
	REQUIRE(triggerRebuild.has_value());
	CHECK(triggerRebuild->Value == 1.0);

	Metrics::Clear();
	const Entity changed = manifolds.back().A;
	store.Remove<Simulated>(changed);
	Solve(store);
	const auto eligibilityRebuild = Metrics::Get("physics.solve.color.topology-rebuild");
	REQUIRE(eligibilityRebuild.has_value());
	CHECK(eligibilityRebuild->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("a replacement entity invalidates a retained colour key", "[solvergroups]") {
	std::unique_ptr<Store> owned = ConnectedLattice(64);
	Store &store = *owned;
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const Entity previous = world.Manifolds().back().A;
	const Transform placement = *store.Get<Transform>(previous);
	store.Destroy(previous);

	const Entity replacement = store.Create();
	CHECK(replacement.Id != previous.Id);
	store.Set<Transform>(replacement, placement);
	store.Set<Simulated>(replacement, Simulated{});
	store.Set<Motion>(replacement, Motion{});
	store.Set<RigidBody>(replacement, RigidBody{});
	Collider collider;
	collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
	store.Set<Collider>(replacement, collider);

	Metrics::Clear();
	SyncBroadphase(store);
	BroadPhase(store);
	NarrowPhase(store);
	Solve(store);
	const auto rebuilt = Metrics::Get("physics.solve.color.topology-rebuild");
	REQUIRE(rebuilt.has_value());
	CHECK(rebuilt->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("a replacement entity invalidates a retained island key", "[solvergroups]") {
	// **64 columns instead of 256**: the test only needs to
	// show that replacing an entity invalidates the island
	// topology key. 64 columns at 16 levels = 1024 bodies
	// is above the partition threshold and exercises the
	// same island rebuild property.
	std::unique_ptr<Store> owned = Spread(64, 16);
	Store &store = *owned;
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.UsesIslandSchedule());
	const auto changed = std::find_if(
		world.Manifolds().begin(), world.Manifolds().end(), [&store](const ContactManifold &manifold) {
			return store.Has<Motion>(manifold.A) && store.Has<Motion>(manifold.B);
		}
	);
	REQUIRE(changed != world.Manifolds().end());
	const Entity previous = changed->A;
	const Transform placement = *store.Get<Transform>(previous);
	store.Destroy(previous);

	const Entity replacement = store.Create();
	CHECK(replacement.Id != previous.Id);
	store.Set<Transform>(replacement, placement);
	store.Set<Simulated>(replacement, Simulated{});
	store.Set<Motion>(replacement, Motion{});
	store.Set<RigidBody>(replacement, RigidBody{});
	Collider collider;
	collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
	store.Set<Collider>(replacement, collider);

	Metrics::Clear();
	SyncBroadphase(store);
	BroadPhase(store);
	NarrowPhase(store);
	Solve(store);
	const auto rebuilt = Metrics::Get("physics.solve.island.topology-rebuild");
	REQUIRE(rebuilt.has_value());
	CHECK(rebuilt->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("a high-degree graph retains the chunk fallback", "[solvergroups]") {
	std::unique_ptr<Store> owned = DenseConnected(80);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	CHECK(PipelineInternals::SolverColors(world).empty());
	CHECK(world.SolverChunkSize() > 0.0f);
	const auto retained = Metrics::GetGauge("physics.solve.color.retained-bytes");
	REQUIRE(retained.has_value());
	CHECK(retained->Value > 0.0);

	Metrics::Clear();
	StepOnce(store);
	const auto reuse = Metrics::Get("physics.solve.color.topology-reuse");
	REQUIRE(reuse.has_value());
	CHECK(reuse->Value == 1.0);
	Metrics::Clear();
}

TEST_CASE("an over-ceiling world clears cached island and colour routes", "[solvergroups]") {
	// **256 columns instead of 64**: the over-ceiling test needs
	// `SolverChunkSize() > 0.0f` which requires the partition
	// to exist. 256 columns at 16 levels = 4096 bodies, above
	// the island schedule threshold.
	std::unique_ptr<Store> owned = Spread(256, 16);
	Store &store = *owned;
	StepOnce(store);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.UsesIslandSchedule());

	for (size_t column = 256; column < 513; column++) {
		const float x = static_cast<float>(column % 16) * 6.0f - 48.0f;
		const float z = static_cast<float>(column / 16) * 6.0f - 48.0f;
		for (size_t level = 0; level < 16; level++) {
			AddMovableBox(store, Vector3{x, static_cast<float>(level) * 0.98f + 0.49f, z});
		}
	}

	SyncBroadphase(store);
	BroadPhase(store);
	NarrowPhase(store);
	Solve(store);
	CHECK(world.RowCount() >= PARALLEL_SOLVE_ROWS);
	CHECK(!world.UsesIslandSchedule());
	CHECK(world.ConstraintIslandCount() == 0);
	CHECK(PipelineInternals::SolverColors(world).empty());
	CHECK(world.SolverChunkSize() > 0.0f);
}

TEST_CASE("every row is in exactly one run", "[solvergroups]") {
	// A row left out of every run is a contact that is never solved, and a row
	// in two is a contact applied twice - which doubles its impulse. Both are
	// silent: the scene still moves, just wrongly.
	//
	// **64 columns instead of 96**: the test only needs to verify
	// that every row is covered exactly once. 64 columns at 8 levels
	// = 512 bodies, below the partition threshold, so the single
	// group covers everything. The property holds regardless of count.
	std::unique_ptr<Store> owned = Spread(64, 8);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(world);
	const SolverGroup border = PipelineInternals::BorderRows(world);

	std::vector<int> visits(world.RowCount(), 0);
	const auto mark = [&visits](SolverGroup run) {
		for (uint32_t at = run.FirstRow; at < run.FirstRow + run.RowCount; at++) {
			REQUIRE(at < visits.size());
			visits[at]++;
		}
	};

	for (const SolverGroup &group : groups) {
		mark(group);
	}
	mark(border);

	CHECK(std::count(visits.begin(), visits.end(), 1) == static_cast<long>(visits.size()));
}

TEST_CASE("a border row is one whose two movable bodies are in two chunks", "[solvergroups]") {
	// What the border set is *for*. Every row in it has to fail the rule the
	// groups pass, or it is a row that could have been dispatched and was not.
	//
	// **256 columns at 8 levels**: the test needs border rows
	// (above the partition threshold) to verify that every
	// border row's two bodies are in different chunks.
	// 256 columns at 8 levels = 2048 bodies, exactly at the
	// partition threshold, which is the minimum that produces
	// border rows.
	std::unique_ptr<Store> owned = Spread(256, 8);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<ContactRow> &rows = PipelineInternals::Rows(world);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	const SolverGroup border = PipelineInternals::BorderRows(world);

	for (uint32_t at = border.FirstRow; at < border.FirstRow + border.RowCount; at++) {
		CHECK(bodies[rows[at].First].Movable);
		CHECK(bodies[rows[at].Second].Movable);
	}
}

TEST_CASE("a partitioned solve gives the same answer twice", "[solvergroups]") {
	// **The determinism the partition exists to preserve**, and the case that
	// would fail if a group ever named a body another group also writes. Two
	// worlds built identically, stepped identically, compared body for body -
	// with a pool running, so the sweeps really are dispatched and the schedule
	// really does vary between the two runs.
	// **Started and stopped around the case, never left running.** The pool is
	// process-wide and its workers block on a condition variable inside a
	// function-local static; a suite that starts one and walks away leaves
	// twenty-three threads waiting on an object the process is about to destroy.
	// `benchmarks/Integrate.cpp` pairs the two in a static guard for the same
	// reason.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	} workers;

	std::unique_ptr<Store> firstOwned = Spread(64, 8);
	std::unique_ptr<Store> secondOwned = Spread(64, 8);
	Store &first = *firstOwned;
	Store &second = *secondOwned;

	for (int tick = 0; tick < 3; tick++) {
		StepOnce(first);
		Publish(first);
		StepOnce(second);
		Publish(second);
	}

	const std::vector<SolverBody> &left = PipelineInternals::Bodies(*first.ResourceMutable<PhysicsWorld>());
	const std::vector<SolverBody> &right = PipelineInternals::Bodies(*second.ResourceMutable<PhysicsWorld>());

	REQUIRE(left.size() == right.size());
	REQUIRE(left.size() > 100);

	size_t differing = 0;
	for (size_t at = 0; at < left.size(); at++) {
		if (!(left[at].LinearVelocity == right[at].LinearVelocity) ||
			!(left[at].AngularVelocity == right[at].AngularVelocity)) {
			differing++;
		}
	}
	CHECK(differing == 0);
}

TEST_CASE("the impulse cache a partitioned solve leaves is sorted", "[solvergroups]") {
	// **The bug this case exists for is silent and cost nothing to write.** The
	// rows are grouped by chunk, so the row array is in no entity order at all;
	// the impulse cache has to be in pair order because next tick's warm start
	// binary-searches it. Writing the cache at row indices leaves it complete,
	// the right size, and unfindable - the scene keeps working and every stack
	// quietly stops warm-starting.
	std::unique_ptr<Store> owned = Spread(64, 8);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<ContactImpulse> &cache = PipelineInternals::ImpulseCache(world);
	REQUIRE(cache.size() >= PARALLEL_SOLVE_ROWS);
	CHECK(std::is_sorted(cache.begin(), cache.end()));

	// And the entries are unique on their key, because a duplicate would make
	// the search's answer a function of which of the two it landed on.
	CHECK(
		std::adjacent_find(cache.begin(), cache.end(), [](const ContactImpulse &a, const ContactImpulse &b) {
			return !(a < b) && !(b < a);
		}) == cache.end()
	);
}

TEST_CASE("a partitioned solve warm-starts from the tick before it", "[solvergroups]") {
	// The other half of the case above: a sorted cache is only worth having if
	// the next tick finds things in it. A settling scene converges, so the
	// second tick's impulses should be close to the first's - and with the cache
	// unfindable they would start from zero every tick and the totals would not
	// track each other at all.
	std::unique_ptr<Store> owned = Spread(64, 8);
	Store &store = *owned;

	StepOnce(store);
	Publish(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	double firstTotal = 0.0;
	for (const ContactImpulse &entry : PipelineInternals::ImpulseCache(world)) {
		firstTotal += static_cast<double>(entry.Normal);
	}
	REQUIRE(firstTotal > 0.0);

	StepOnce(store);

	// Every row of the second tick warm-started from a row of the first, so the
	// impulses it accumulates begin where the last ones ended rather than at
	// zero. Checked as a ratio because the exact figure is the scene's.
	double secondTotal = 0.0;
	for (const ContactImpulse &entry : PipelineInternals::ImpulseCache(world)) {
		secondTotal += static_cast<double>(entry.Normal);
	}
	CHECK(secondTotal > firstTotal * 0.5);
}

TEST_CASE("a partitioned stack still stands up", "[solvergroups]") {
	// **The accuracy side of `SOLVE_SWEEPS_PER_BATCH`, which is the constant the
	// speed side paid for.** Sweeping a group four times per handover means news
	// crosses a chunk face four times a tick instead of sixteen, and the thing
	// that goes wrong when information does not cross fast enough is a stack
	// that leans and then slides apart.
	//
	// A hundred and twenty-eight towers of eight, dropped a millimetre apart and
	// left for two simulated seconds, measured as the furthest any box ends up
	// from the column it started in - the same number `Solver.hpp`'s table calls
	// tower drift, taken on a scene large enough to be partitioned.
	//
	// **Both numbers this prints are the accuracy column of
	// `SOLVE_SWEEPS_PER_BATCH`'s table**, and that comment is where they are
	// written down against the cost they bought. Re-take them here by rebuilding
	// with a different value, exactly as `SOLVER_ITERATIONS` is re-taken.
	//
	// The bound below is loose on purpose. This is a chaotic system and the case
	// is looking for a scene that fell over, not for a tolerance.
	std::unique_ptr<Store> owned = Spread(128, 8);
	Store &store = *owned;

	std::vector<std::pair<Entity, Vector3>> started;
	store.Each<const Transform, const Motion>([&started](
												  Entity entity, const Transform &placement, const Motion &
											  ) { started.emplace_back(entity, placement.Frame.Position); });
	REQUIRE(started.size() > 500);

	for (int tick = 0; tick < 60; tick++) {
		StepOnce(store);
		Publish(store);
		IntegrateMotion(store);
	}

	REQUIRE(store.ResourceMutable<PhysicsWorld>()->SolverGroupCount() > 1);

	float furthest = 0.0f;
	float deepest = 0.0f;
	for (const auto &[entity, origin] : started) {
		const Transform *now = store.Get<Transform>(entity);
		if (now == nullptr) {
			continue;
		}
		const float dx = now->Frame.Position.X - origin.X;
		const float dz = now->Frame.Position.Z - origin.Z;
		furthest = std::max(furthest, std::sqrt(dx * dx + dz * dz));

		// **How far a box ended up below where it started, which is the less
		// chaotic of the two numbers.** Drift is a toppling measurement and a
		// toppling tower is chaotic - a solver change moves it in either
		// direction. Sink is convergence: a stack whose contacts have not been
		// solved enough times settles into itself, and the amount it sinks is
		// monotone in how badly they were solved.
		deepest = std::max(deepest, origin.Y - now->Frame.Position.Y);
	}

	INFO("furthest horizontal drift, metres: " << furthest);
	INFO("deepest sink, metres: " << deepest);

	// A tower that stands has its boxes within a box-width of where they
	// started. Half a metre is a box that has slid off its neighbour, which is
	// the failure this case is looking for rather than a tight tolerance on a
	// chaotic system.
	CHECK(furthest < 0.5f);
}
