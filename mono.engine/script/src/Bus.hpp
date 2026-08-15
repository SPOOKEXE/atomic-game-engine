#pragma once

// What the bus services share, with no VM in it.
//
// **Not `<engine/world/Bus.hpp>`, which is one layer down and is the bus.** That
// header says what a topic, a ticket and a status *are*; this one holds the two
// things a *script binding* over them needs and neither language decides - who
// is listening to a topic, and the word a script sees for a refusal.
//
// ## The subscription table
//
// **Shared machinery beside `SignalTable` and `DebrisQueue`, and for their
// reason.** A topic is a string and a listener is a callable neither language
// may interpret, so what is left - which callables a topic has and the order a
// delivery reaches them in - names no VM and must not be two implementations. It
// was two: Luau kept a table per topic in the registry under
// `engine.messaging.subscriptions` and JavaScript kept a `std::unordered_map` on
// its context, so `MessagingService` could not be described once even though
// neither half did anything language-shaped.
//
// **Order within a topic is insertion order, and that is the whole contract.**
// Two scripts subscribing to one topic are delivered to in the order they
// subscribed, on every run of the recording - which is `SignalTable`'s rule and
// `DebrisQueue`'s, said for a third list.
//
// **No unsubscribe, because the bus has none.** `Postbox::Subscribe` registers a
// world's interest and nothing withdraws it, so a `Disconnect` here would stop a
// callback firing while the world went on paying for the delivery. Roblox's
// `SubscribeAsync` returns a connection; this returns nothing, which is the
// honest surface until the bus grows the other half.
//
// @tier L9 · shared
// @since v0.16

#include "Signals.hpp"

#include <engine/world/Bus.hpp>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::script {

	// Every callable listening on every topic, in one VM.
	//
	// One per VM, beside `SignalTable`, `TaskQueue` and `DebrisQueue`. Holds no
	// VM types: what it produces is a list of `CallbackRef`, and the binding
	// calls them.
	class TopicSubscriptions {
	  public:
		// Adds a listener to a topic.
		//
		// @param topic    What was subscribed to.
		// @param callback The VM's name for the callable. The table takes it and
		//                 the caller must not release it.
		void Add(std::string_view topic, CallbackRef callback);

		// Who is listening to one topic, in subscription order.
		//
		// @param topic The delivery's key.
		// @return The listeners, empty for a topic nothing subscribed to. Valid
		//         until the next `Add`.
		std::span<const CallbackRef> Listeners(std::string_view topic) const;

		// Whether anything is listening to anything, so a pump with no
		// subscribers costs a bool rather than a hash of every delivery's key.
		bool Empty() const {
			return Topics.empty();
		}

		// **There is no `Clear` and no release walk**, unlike `SignalTable` and
		// `TaskQueue`. A subscription lasts as long as the VM does - there is no
		// unsubscribe for the same reason - so the only moment every reference
		// would be handed back is the one where the whole VM is being freed and
		// the registry goes with it.

	  private:
		// Keyed by topic, because that is what a delivery carries: a hash per
		// arrival rather than a scan of every subscription.
		std::unordered_map<std::string, std::vector<CallbackRef>> Topics;
	};

	// A stable, machine-readable name for a bus refusal.
	//
	// **Not `world::Describe(BusStatus)`, and the difference is who reads it.**
	// That one is prose for a log - "version conflict" - and this is a value a
	// script *compares against*, so it is the member's own spelling and changing
	// one would break a game rather than a sentence. Both languages hand the same
	// word back beside a reply, which is why it is here rather than once per pump:
	// it was written twice, and a status a script branched on differing between
	// two VMs is the drift this module keeps closing.
	//
	// `docs/retired/SCRIPT_CONCURRENCY.md` §5: named, not swallowed. Each of
	// these is something a script author has to be able to see and handle, so
	// each arrives beside the value rather than as a nil that could mean three
	// things.
	//
	// @param status What the bus answered.
	// @return The member's name, valid for the life of the program.
	const char *DescribeStatus(world::BusStatus status);
}
