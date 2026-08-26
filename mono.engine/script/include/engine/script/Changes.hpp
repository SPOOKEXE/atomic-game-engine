#pragma once

// One component write, every property name that observes it.
//
// **This is the half of `.Changed` the projection model creates, and it is the
// half nothing had written down.** `ecs::ChangeChannel` answers "did this
// entity's `Transform` change" - per *component*, which is what storage knows.
// A script asks a different question: "did this part's `Position` change". The
// two are not the same, because a property is a projection and one component
// write lands under several names:
//
//     Transform  ->  CFrame, Position, Orientation
//     Bounds     ->  Size
//     Visual     ->  Color, Visible, Mesh, Transparency
//
// A script told that `Transform` moved would have to learn the component layout
// to make sense of it, which is exactly the coupling `PropertyDescriptor` exists
// to prevent. `PropertyDescriptor::Reads` is the map the fan-out needs, and it
// was declared at v0.5 rather than left for a conversion to know privately for
// this reason. Nothing in this file names a property or a component.
//
// ## When it fires, and why not sooner
//
// Roblox fires `.Changed` at the moment of assignment. That is not available
// here and the reason is worth stating: a script write would re-enter the VM
// from inside `Store::SetProperty`, so a handler could destroy the instance
// whose property was half-written.
//
// So the chain is the one `ecs::Store` already built for exactly this:
//
//   1. a script writes a property during the `Simulation` phase,
//   2. `World::Tick` calls `FlushSignals` after the phases - the barrier -
//      and the listeners here fan the component out to property names and
//      **queue** them,
//   3. the next `Heartbeat` drains the queue and calls the script.
//
// **One tick of latency, and it buys determinism.** `docs/retired/SCRIPT_CONCURRENCY.md`
// §1 permits a resume from a tick boundary and nothing else that is not a
// barrier delivery; step 3 is that boundary. Firing at step 2 instead would put
// the handler's own writes on the far side of the next `ClearChanges` - which
// runs at the *start* of a tick - so a `.Changed` handler that moved something
// would signal nothing, once, silently.
//
// **The cost is bounded by what somebody connected to.** The roadmap's
// objection to `OnChanged<T>` was that filtering a whole-world signal per
// connection walks every change for every listener. It is subscribed once per
// component instead, and the entity filter is a hash lookup.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <functional>
#include <unordered_set>
#include <vector>

namespace engine::script {

	// What changed since the last barrier, as property names.
	//
	// One per VM. Holds no VM types: what it produces is an entity and a name,
	// and each binding turns those into a call.
	//
	// @since v0.6
	class ChangeQueue {
	  public:
		ChangeQueue() = default;
		~ChangeQueue() = default;

		ChangeQueue(const ChangeQueue &) = delete;
		ChangeQueue &operator=(const ChangeQueue &) = delete;
		ChangeQueue(ChangeQueue &&) = delete;
		ChangeQueue &operator=(ChangeQueue &&) = delete;

		// Starts reporting changes for one instance.
		//
		// Subscribes to every component any of its properties reads, once per
		// component for the whole world rather than once per instance -
		// `Store::OnChangedComponent` fires for every entity, and the filter is
		// what makes it per instance.
		//
		// Observing a component moves rows into an archetype with somewhere to
		// put the dirty bits, so this is a structural change. It happens at
		// connect time deliberately: a script setting up its listeners is a
		// better moment for that than the middle of a tick.
		//
		// @param store    The world.
		// @param instance The instance to watch.
		void Watch(ecs::Store &store, ecs::Entity instance);

		// Stops reporting changes for one instance.
		//
		// The component subscriptions stay. Dropping the last watcher of a
		// component would not un-observe it - `Store` has no such call, because
		// un-observing means another archetype move - so tearing the listener
		// down would buy a hash lookup per change and cost a second structural
		// pass if anything ever watched again.
		//
		// @param instance The instance to stop watching.
		void Unwatch(ecs::Entity instance);

		// Queues one notification directly, without a component write behind it.
		//
		// **For attributes, which are the one authored value that is not a
		// component.** Everything else here arrives through
		// `Store::OnChangedComponent` and is fanned out to the property names
		// reading that component; an attribute has no component and no property
		// descriptor, so there is nothing to fan from and the writer says what
		// changed itself.
		//
		// **It goes through the same queue rather than firing on the spot**, and
		// that is the whole reason this is a method here instead of a call to
		// `FireSignal` at the write. The queue dedups - a value written three
		// times in one tick signals once, with what it ended at - and it defers to
		// the barrier, so a listener cannot mutate the world in the middle of a
		// loop over it. An attribute that fired immediately would have neither
		// property and would be the one signal in the engine that behaves
		// differently from every other.
		//
		// Watching is not required: an attribute has no component to subscribe to,
		// so the filter `Watch` exists for does not apply and a recorded change is
		// always delivered.
		//
		// @param instance The instance the value is on.
		// @param name     What changed.
		void Record(ecs::Entity instance, core::Name name);

		// Hands over everything the last barrier recorded and empties the queue.
		//
		// @param body Called as `body(instance, propertyName)`, in queue order.
		void Drain(const std::function<void(ecs::Entity, core::Name)> &body);

		// Reports whether anything is queued.
		//
		// @return `true` when `Drain` would call `body` at least once.
		bool Empty() const {
			return Pending.empty();
		}

		// Drops every subscription. Called before the store outlives the VM.
		//
		// @param store The world the subscriptions are on.
		void Detach(ecs::Store &store);

	  private:
		// A queued notification.
		struct Change {
			ecs::Entity Instance;
			core::Name Property;
		};

		// One entity and one property name, packed to key the dedup set.
		//
		// A property written three times in one tick signals once, which is the
		// rule `Store::FlushSignals` already states for components - a script
		// seeing three calls with two values nobody will ever observe is worse
		// than one call with the value it ended at.
		static uint64_t KeyOf(ecs::Entity instance, core::Name property);

		// Fans one component write out to the property names reading it.
		void Fan(const ecs::Store &store, ecs::Entity instance, ecs::ComponentId component);

		std::unordered_set<uint64_t> Watched;
		std::unordered_set<uint64_t> Queued;
		std::vector<Change> Pending;

		// One per component, not one per watched instance.
		std::vector<ecs::Store::Connection> Subscriptions;
		std::unordered_set<uint32_t> Subscribed;
	};
}
