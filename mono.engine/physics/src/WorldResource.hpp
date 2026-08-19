#pragma once

// Reaching a world's `PhysicsWorld` without registering the type to find out
// that it is not there.
//
// **`Store::Resource<T>()` registers `T` as a side effect of looking for it.**
// It has to: the resource table is keyed by `ComponentId`, so reaching one
// means calling `Components::Of<T>()`, and that mints an id under the
// *compiler's* spelling for anything not registered yet. That is the right
// convenience for a component a system invented. It is wrong here, and wrong in
// a way that surfaces a long way from the call:
//
// - A query against a world nobody prepared registers
//   `engine::physics::PhysicsWorld`, because asking whether the resource is
//   there is what registers it.
// - The next `RegisterPhysicsComponents` asks for the same type under
//   `physics.PhysicsWorld`. A type has one name, so `Components::Adopt` refuses
//   the second and aborts the process.
// - The abort is the registry doing its job, and it is the *lucky* outcome.
//   `mono.server/include/server/Simulation.hpp` names the unlucky one: a
//   component registered under the compiler's spelling produces a snapshot a
//   process that spells it differently cannot read, and nothing says so at the
//   time.
//
// It is not a test-only hazard. Any program that runs a physics query before
// `PreparePhysicsWorld` hits it, and it hides because it needs the query to
// come *first* - which in a suite is a matter of which order the cases were
// shuffled into.
//
// So every read of the resource in this module goes through here, and "is this
// world prepared" is answered with `Components::Find`, which takes a name and
// registers nothing.

#include <engine/physics/Clock.hpp>
#include <engine/physics/PhysicsWorld.hpp>

#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::physics {

	// The name `PhysicsWorld` is registered under.
	//
	// **One spelling, used by the registration and by the lookup that checks
	// for it.** Two copies of this string would disagree exactly once, and the
	// symptom would be a module that thinks no world is ever prepared.
	inline constexpr std::string_view PHYSICS_WORLD_COMPONENT = "physics.PhysicsWorld";

	// The name `PhysicsClock` is registered under, for the same reason and with
	// the same one-spelling rule.
	inline constexpr std::string_view PHYSICS_CLOCK_COMPONENT = "physics.PhysicsClock";

	// Whether this process has registered the resource type at all.
	//
	// A name lookup, so it registers nothing and is safe to ask of a process
	// that has never called `RegisterPhysicsComponents`.
	//
	// @return `true` once `RegisterPhysicsComponents` has run.
	// @threadsafe
	bool PhysicsWorldRegistered();

	// The world's physics resource, or `nullptr` with a line in the log.
	//
	// @param store The world to read.
	// @return The resource, or `nullptr` when the world was never prepared.
	// @threadsafe
	const PhysicsWorld *PreparedWorld(const ecs::Store &store);

	// The same, for a step that writes.
	//
	// @param store The world to read.
	// @return The resource, or `nullptr` when the world was never prepared.
	PhysicsWorld *PreparedWorldMutable(ecs::Store &store);

	// The world's physics clock, or `nullptr` when the world was never prepared.
	//
	// **Silent where `PreparedWorld` complains.** A step called directly rather
	// than through the pipeline is the ordinary case in this module's suites and
	// benchmarks, and such a world has no clock by design -
	// `PhysicsStepSeconds` falls back to the world's tick for exactly that. A
	// line per read would be a log full of a supported case.
	//
	// @param store The world to read.
	// @return The clock, or `nullptr`.
	// @threadsafe
	const PhysicsClock *PreparedClock(const ecs::Store &store);

	// The same, for the steps that charge time to it.
	//
	// @param store The world to read.
	// @return The clock, or `nullptr`.
	PhysicsClock *PreparedClockMutable(ecs::Store &store);

	// The one rule about what a physics rate may be.
	//
	// Private because both callers are in this module - `SetPhysicsTickRate`
	// and the snapshot reader - and because two copies of the rule would
	// disagree exactly once, on the input that was crafted to find it.
	//
	// @param stepsPerSecond The rate as it arrived.
	// @return Zero for anything not above zero, and `PhysicsClock::
	//         MAXIMUM_RATE` for anything above that.
	double SanePhysicsRate(double stepsPerSecond);
}
