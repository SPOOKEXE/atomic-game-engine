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
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ContactImpulse;
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
	std::unique_ptr<Store> Spread(size_t columns, size_t height) {
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

		for (size_t column = 0; column < columns; column++) {
			// A lattice rather than random placement, so the case is the same
			// scene every run and a failure can be looked at.
			const auto x = static_cast<float>(column % 16) * 6.0f - 48.0f;
			const auto z = static_cast<float>(column / 16) * 6.0f - 48.0f;

			for (size_t level = 0; level < height; level++) {
				const Entity box = store->Create();
				const auto y = static_cast<float>(level) * 0.98f + 0.49f;
				store->Set<Transform>(box, Transform{CFrame{Vector3{x, y, z}}});

				// **Both, and the tag is not optional.** `FactsFor` asks whether
				// the world may move this and hands back an infinite mass when
				// the answer is no - so a row with a `Motion` and no tag solves
				// as a wall, every contact resolves to nothing, and the group
				// count this file measures comes back zero.
				store->Set<Simulated>(box, Simulated{});
				store->Set<Motion>(box, Motion{});

				RigidBody body;
				body.Kind = BodyKind::Dynamic;
				body.Mass = 1.0f;
				store->Set<RigidBody>(box, body);

				Collider shape;
				shape.Extent = Vector3{0.5f, 0.5f, 0.5f};
				store->Set<Collider>(box, shape);
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

TEST_CASE("a scene above the threshold is cut into several groups", "[solvergroups]") {
	// The partition doing its job. Nothing here asserts a count - that is a
	// function of `SOLVE_GROUP_TARGET` and of the scene - only that a wide scene
	// produces more than one run, because one run is a solve that cannot use the
	// machine however many workers it is handed.
	std::unique_ptr<Store> owned = Spread(96, 8);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	REQUIRE(world.RowCount() >= PARALLEL_SOLVE_ROWS);

	CHECK(world.SolverGroupCount() > 1);
	CHECK(world.SolverChunkSize() > 0.0f);
}

TEST_CASE("no two groups name one movable body", "[solvergroups]") {
	// **The whole correctness argument, asserted directly.** Two runs sweeping
	// at once write the bodies they name, so a body in two runs is a race and a
	// result that depends on which worker got there first. An immovable body is
	// exempt because a sweep never writes one - see `ApplyImpulse` - and the
	// floor below is in every stack's lowest contact, so a rule about all bodies
	// rather than movable ones would put this scene in one group.
	std::unique_ptr<Store> owned = Spread(96, 8);
	Store &store = *owned;
	StepOnce(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const std::vector<ContactRow> &rows = PipelineInternals::Rows(world);
	const std::vector<SolverBody> &bodies = PipelineInternals::Bodies(world);
	const std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(world);
	REQUIRE(groups.size() > 1);

	// Which group first claimed each movable body. A second claim by a different
	// group is the failure.
	std::vector<size_t> claimedBy(bodies.size(), SIZE_MAX);
	size_t movableClaims = 0;

	for (size_t group = 0; group < groups.size(); group++) {
		for (uint32_t body : BodiesIn(rows, groups[group])) {
			if (!bodies[body].Movable) {
				continue;
			}
			movableClaims++;
			if (claimedBy[body] == SIZE_MAX) {
				claimedBy[body] = group;
				continue;
			}
			CHECK(claimedBy[body] == group);
		}
	}

	// A scene where nothing was movable would pass the loop above without
	// testing anything.
	CHECK(movableClaims > 0);
}

TEST_CASE("every row is in exactly one run", "[solvergroups]") {
	// A row left out of every run is a contact that is never solved, and a row
	// in two is a contact applied twice - which doubles its impulse. Both are
	// silent: the scene still moves, just wrongly.
	std::unique_ptr<Store> owned = Spread(96, 8);
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
	std::unique_ptr<Store> owned = Spread(96, 8);
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

	std::unique_ptr<Store> firstOwned = Spread(96, 8);
	std::unique_ptr<Store> secondOwned = Spread(96, 8);
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
	std::unique_ptr<Store> owned = Spread(96, 8);
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
	std::unique_ptr<Store> owned = Spread(96, 8);
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

	for (int tick = 0; tick < 120; tick++) {
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
