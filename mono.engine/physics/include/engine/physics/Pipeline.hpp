#pragma once

// How the physics steps get into a world.
//
// Three entry points, in the order a host calls them: register the types at
// startup, prepare each world as it is built, register the systems on its
// scheduler.
//
// **The steps are registered as one system per phase, not one per step.**
// `ecs::Scheduler` is explicit that two systems in the same phase have no
// ordering guarantee, and `SyncBroadphase` reading the transforms
// `IntegrateMotion` just wrote is not a preference - it is a hard dependency. So
// the order is expressed by composition, which the scheduler's contract
// supports, rather than by registration order, which it does not.
// Each step still opens its own profiler span, so the overlay separates them
// anyway.
//
// @tier L8 · shared

namespace engine::ecs {
	class Scheduler;
	class Store;
}

namespace engine::physics {

	// Registers this module's resource under an explicit name.
	//
	// Called once during single-threaded startup, before any world is built and
	// before `ecs::Components::Seal`. A resource is keyed by a component id too,
	// so one that is never registered here would be minted by the first
	// `Store::SetResource` - under the compiler's spelling of the type, and
	// aborting outright once the table is sealed.
	//
	// Idempotent.
	void RegisterPhysicsComponents();

	// Gives a world its `PhysicsWorld` resource and starts the change tracking
	// the broad phase needs.
	//
	// **Call it when the world is built, not when it first ticks.**
	// `Store::Observe` moves every row already carrying the component into an
	// archetype with somewhere to put the bits, which is a structural change
	// nobody asked for at a moment nobody expected. It is also the reason this
	// is a separate call rather than something `SyncBroadphase` does lazily.
	//
	// Observing is what makes "only rebuild the static index when static
	// geometry changed" a fact rather than a hope: without it `Store::Changed`
	// is always false, and the static index would silently never be rebuilt.
	//
	// @param store    The world to prepare.
	// @param cellSize Grid cell edge in metres, or the `spatial::HashGrid`
	//                 default. About twice the median collider extent; the
	//                 measurement behind the default is in that header.
	void PreparePhysicsWorld(ecs::Store &store, float cellSize = 0.0f);

	// Adds the physics steps to a world's scheduler.
	//
	// Two systems: `physics.simulation` in `Phase::Simulation`, which
	// integrates and then indexes, and `physics.contacts` in
	// `Phase::PostSimulation`, which pairs, intersects, solves and publishes.
	// Six steps in two systems, in pipeline order.
	//
	// **How many times the six run is the world's `PhysicsClock`'s business.**
	// A world that never set a rate runs them once per tick, which is what they
	// did before the clock existed. A slower world skips whole ticks; a faster
	// one finishes its extra steps inside `physics.contacts`. See `Clock.hpp`.
	//
	// @param scheduler The scheduler to add to.
	void RegisterPhysicsSystems(ecs::Scheduler &scheduler);
}
