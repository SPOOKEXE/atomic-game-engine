#pragma once

// The client half of a Play run, in the editor's own process.
//
// **`RunMode::Play` has claimed since v0.7 to be "both halves in one process,
// the arrangement `HostRole::OfBoth` describes" — and until this it was one
// half wearing both hats.** The world ticked with `IsServer()` and `IsClient()`
// both true, which is what a single-player process looks like from a script; but
// there was no replica anywhere, so nothing a client would actually *see* was
// ever produced. A property that never crossed a wire looked identical to one
// that did, and the first place that shows is a game that works in the studio
// and not on a server.
//
// This is the other half: an `Authority` over the world being played and a
// `Replica` of it in a second world, with **nothing in between them**. No
// datagram, no header, no cipher, no window — the bytes the authority produced
// are the bytes the replica reads, in the order they were produced. That is
// `mono.unified_server_client`'s arrangement exactly, and it is deliberately the
// same one: a second way to stand the two halves up in one process would be a
// second thing to keep in step with what replication means.
//
// ## Why a second world rather than a second view of one
//
// A viewport per world already exists, so the cheap-looking answer is to point
// the second viewport at the same world and call one of them "the client". That
// is a picture of the same rows drawn twice. **The whole value of this panel is
// the difference between the two** — a part that moved on the server and has not
// arrived, a colour that never crossed because nothing marked it dirty, a
// creation the client has not been told about. Two views of one store cannot
// show any of that, because there is only one answer in the process.
//
// So the replica is a real `world::World` holding real rows applied from real
// messages, and what the second viewport draws is what a connected client would
// have drawn.
//
// ## What it is not
//
// **Not a network test.** There is no loss, no reordering and no latency here
// beyond the one tick the pipeline costs; `mono.unified_server_client` takes
// `--drop` for that and `mono.server`/`mono.client` over a socket is the real
// thing. What this catches is the class of bug that survives a perfect link:
// state that is never sent at all.
//
// **Not authored content.** The replica world is created when Play starts and
// destroyed when Stop does. It is never written to a game file, never renamed,
// never edited and never idle-closed — see `Editor::IsReplicaWorld`.
//
// @tier L12 · client

#include <engine/ecs/Entity.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/world/World.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::world {
	class Universe;
}

namespace studio {

	// What the last step moved, for the panel that shows the two side by side.
	//
	// @since v0.7
	struct LinkReport {
		// The server tick the last publish described.
		uint64_t Tick = 0;

		// The last tick the replica holds in full.
		//
		// Behind `Tick` by one step in the ordinary case, because a message
		// produced this step is applied this step and acknowledged the next.
		//
		// **Zero is not "never joined", and reading it that way is a mistake
		// this file made first.** A run publishes before its world has ticked,
		// so the join snapshot is stamped tick 0 and a world that has not
		// changed since leaves this at zero for ever — which is correct, and is
		// what a scene with no systems in it looks like. `ClientEntities` is the
		// field that answers whether anything arrived; this one answers when.
		uint64_t Applied = 0;

		// Messages handed over on the last step.
		size_t Messages = 0;

		// What those messages weighed, in bytes.
		size_t Bytes = 0;

		// The largest single message, which is the number that says whether a
		// tick would have crossed a real link. `net` adds a header and a tag to
		// each of these, so a figure near the message limit is already one that
		// would not fit.
		size_t LargestMessage = 0;

		// Messages since the run started.
		uint64_t TotalMessages = 0;

		// What all of them weighed, in bytes.
		uint64_t TotalBytes = 0;

		// Entities carrying a `scene::Transform` on each side.
		//
		// **The first place a divergence shows, and the cheapest.** Equal counts
		// with a blank client view is a drawing problem; unequal counts is a
		// replication one, and the two have nothing to do with each other.
		size_t ServerEntities = 0;

		// The replica's side of that comparison.
		size_t ClientEntities = 0;
	};

	// An authority over one world and a replica of it in another.
	//
	// Built by `Editor::BeginRun` for `RunMode::Play`, stepped once per tick
	// after the universe has ticked, and torn down by `Editor::EndRun`.
	//
	// @since v0.7
	class PlayLink {
	  public:
		PlayLink() = default;
		~PlayLink() = default;

		PlayLink(const PlayLink &) = delete;
		PlayLink &operator=(const PlayLink &) = delete;

		// Creates the replica world beside `authority` and admits a client.
		//
		// The new world takes the authority's name with a suffix, is marked as a
		// replica in both of the ways v0.6 settled — `world::Replica` for the
		// buses and `Store::SetAdoptOnly` for the storage — and gets the
		// client's presentation seam so that it has a draw list to publish.
		//
		// @param universe  The editor's universe. Gains one world.
		// @param authority The world being played.
		// @param tickRate  The authority's rate, which is what the replica's
		//        interpolation delay is measured against. **Not the frame rate**:
		//        a delay in ticks against a frame rate is a delay that changes
		//        when the display does.
		// @param error     Filled when this returns false.
		// @param label     What to call this client, appended to the authority's
		//        name. **A world's name is its identity to the whole universe**
		//        — rule 4 — so two clients that took one name would be two
		//        worlds nothing could tell apart, in a panel or in a recording.
		// @return `false` when the replica world could not be created.
		// @param adopt     A `Player` already in `authority` to take over rather
		//        than admitting a new one. **What makes a teleport followable**:
		//        the destination world has already rebuilt the player from the
		//        arriving payload, and a link that admitted a second one would
		//        put the same person in the world twice.
		bool Start(
			engine::world::Universe &universe,
			engine::world::WorldId authority,
			double tickRate,
			std::string &error,
			std::string_view label = "client",
			engine::ecs::Entity adopt = engine::ecs::Entity{}
		);

		// How many frames this client's player has been missing for.
		//
		// **A teleport is not instant and cannot be**, which is what this
		// counts. The departure destroys the player during a tick; the envelope
		// is routed at that tick's barrier; the destination admits the arrival
		// when *its* script heartbeat next pumps its inbox — so there is a
		// window, at least one tick wide, where the player is in neither world.
		// An editor that declared the client lost on the first frame of that
		// window would kill every teleport it followed, which is exactly what it
		// did before this existed.
		//
		// Reset by arriving. Read by `Editor::FollowTeleports`.
		int &Missing() {
			return Missing_;
		}

		// What this client's player is called.
		//
		// **A name and not a handle, because a teleport does not move one.**
		// Nothing crosses a world boundary but bytes, so the player in the
		// destination is a different entity that happens to be the same person —
		// and the only thing the two share is what they are called. `Editor::
		// FollowTeleports` is the one reader.
		const std::string &PlayerName() const {
			return PlayerName_;
		}

		// Publishes this tick, hands the messages over, and acknowledges.
		//
		// **Called after `Universe::Tick` and before the next one**, which is the
		// only window where the dirty bits describe the tick that just ran — a
		// world clears them at the *start* of a tick, so reading them later is
		// how a tick's worth of movement goes missing. `mono.server` says the
		// same thing at its own publish and for the same reason.
		//
		// Does nothing when the link was never started.
		//
		// @param universe The editor's universe.
		void Step(engine::world::Universe &universe);

		// Destroys the replica world and forgets the client.
		//
		// @param universe The editor's universe.
		void Stop(engine::world::Universe &universe);

		// The replica world, or an invalid handle when there is none.
		engine::world::WorldId ReplicaWorld() const {
			return Replica_;
		}

		// The world this client is a client *of*.
		//
		// **Not the same as the run's world once a teleport has been followed**,
		// and that difference is a bug this accessor exists to have prevented: a
		// link that moved to another scene still belongs to the run that started
		// it, so asking the run's world whether this client's player is alive
		// answers "no" for ever and follows the same teleport every frame.
		engine::world::WorldId AuthorityWorld() const {
			return Authority_;
		}

		// Whether this link is running.
		bool IsRunning() const {
			return Replica_.IsValid();
		}

		// The `Player` this client is, in the *authority's* world.
		//
		// **The authority's handle, and a replica's handle for the same thing.**
		// A replica applies structure with the entity ids it was sent, so the
		// two ends agree on numbering by construction — `game::JoinNotice` says
		// the same about a real wire, and it is what lets one handle be looked
		// up in either store.
		//
		// Null for a world with no `Players` service, which is every scene
		// nobody furnished.
		engine::ecs::Entity Player() const {
			return Player_;
		}

		// What the last step moved.
		const LinkReport &Report() const {
			return Last;
		}

	  private:
		engine::world::WorldId Authority_;
		engine::world::WorldId Replica_;

		engine::replication::Authority Server;
		engine::replication::Replica Client;
		engine::replication::ClientId Handle;

		// The player this client was given, in the authority's world.
		engine::ecs::Entity Player_;

		// What that player is called, which is all a teleport carries.
		std::string PlayerName_;

		// Frames since this client's player stopped being anywhere.
		int Missing_ = 0;

		LinkReport Last;
	};
}
