#pragma once

// The turn-taking that keeps two editors off one model, and loses neither edit.
//
// ## Ask, hold, edit, give back
//
// A guest asks the host before it publishes; the host hands out the subtree if
// nobody has it and **queues the request behind whoever does**; the guest sends
// its waypoint; the host applies it, relays it, gives the subtree back and
// grants the next in line.
//
// **Nobody's work is thrown away, and that is the whole point of the queue.**
// The version this replaced refused the second editor and rolled their change
// back at their own machine — which is correct, survivable, and still means
// somebody watched their edit disappear. Here the second edit lands *on top of*
// the first, in the order the host granted, and both people keep what they did.
//
// ## What the round trip costs, and why it is affordable
//
// The guest waits for a grant before its edit *replicates*. It does not wait to
// *see* it: the edit is applied locally the moment it is made, so the person
// feels no latency at all. What waits is the message, by one round trip — a
// millisecond on a subnet, tens across the internet — and what it buys is that
// the host, and only the host, decides who was first.
//
// ## Why there is still a timeout
//
// A guest that is granted a subtree and then dies would hold it for ever, and
// there is nobody to notice. So a grant has a guard — not a lease on editing,
// which is what the previous design got wrong, but a bound on one protocol
// step: the time between "you may" and "here it is". It is short, because
// nothing legitimate takes long, and it exists so a crash costs the next person
// a pause rather than the session.
//
// ## Why subtrees
//
// A hold over `Workspace.Model` that let somebody else edit
// `Workspace.Model.Part` would order nothing: moving a model moves its
// children, and two people doing that at once is the case this exists for. So a
// hold covers a path and everything under it, and two overlap when either
// contains the other.
//
// ## What it is not
//
// **Not a permission system.** A hold says whose turn it is, not that somebody
// is allowed and somebody else is not. Everybody in a session already has the
// key that let them in.
//
// **Not visible as waiting.** A queued editor is not blocked at their own
// machine — they carry on editing, and their messages queue behind each other
// in the order the host grants them.
//
// @tier L12 · client

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <studio/InstancePath.hpp>
#include <vector>

namespace studio {

	// Which editor a hold belongs to.
	//
	// **The host is zero and a guest is one past its client slot.** A
	// `ClientId` is the host's own numbering and means nothing to a guest, so
	// what crosses is this — the same number every editor in the session sees
	// for the same person, because the host is the one that issues it.
	//
	// The offset is load-bearing rather than cosmetic: client slots are dense
	// and start at zero, so without it the first guest to join is the host as
	// far as every hold is concerned, and the two take turns with themselves.
	//
	// @since v0.13
	using EditorId = uint32_t;

	// The host's own id. Never issued to a guest.
	inline constexpr EditorId HOST_EDITOR = 0;

	// One editor holding one subtree, for the length of one edit.
	//
	// @since v0.13
	struct Lease {
		// The subtree's root. Everything under it is held too.
		InstancePath Subject;

		// Whose turn it is.
		EditorId Holder = HOST_EDITOR;

		// When the grant stops being honoured, on the host's clock.
		//
		// **A guard on one protocol step, not a lease on editing.** It bounds
		// the gap between "you may" and "here it is", so an editor that died
		// holding a turn costs the next person a pause rather than the session.
		double ExpiresAtSeconds = 0.0;
	};

	// Somebody waiting for a turn.
	//
	// @since v0.13
	struct Waiting {
		// What they asked for.
		InstancePath Subject;

		// Who asked.
		EditorId Holder = HOST_EDITOR;
	};

	// What asking produced.
	//
	// @since v0.13
	enum class Turn : uint8_t {
		// Nobody had it. Go ahead.
		Granted,

		// Somebody has it. The request is remembered and will be granted when
		// they give it back.
		Queued,

		// The queue is full. **Refused rather than dropped silently**, because
		// a request that is neither granted nor queued is an editor waiting for
		// a message that will never come.
		Refused,
	};

	// Returns a stable, human-readable name for a turn.
	//
	// @param turn The turn to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Turn turn);

	// How long a grant is honoured and how many may wait.
	//
	// @since v0.13
	struct LockSettings {
		// How long after a grant the hold lapses if the edit never arrives.
		//
		// **Short, because nothing legitimate takes long.** The gap it bounds
		// is one message in each direction: the grant going out and the
		// waypoint coming back. Two seconds is many times a bad connection's
		// round trip and far less than a person would wait staring at a model
		// they cannot move. Chosen rather than measured, and saying so is
		// better than implying otherwise.
		double GrantSeconds = 2.0;

		// The most turns held at once.
		size_t MaximumHolds = 256;

		// The most requests waiting at once.
		//
		// Bounded like every other table fed from a wire: a guest that asked in
		// a loop would otherwise be a guest spending the host's memory. Past
		// the cap a new request is refused and the queue stands, which is the
		// way round `network::Directory` bounds its table and for the same
		// reason — a bound that lets a flood push out somebody's place in the
		// queue is not a bound.
		size_t MaximumWaiting = 256;
	};

	// Whose turn it is, and who is next.
	//
	// **One per session, on the host, and only the host's is consulted.** A
	// guest keeps a copy of what the host broadcasts purely so the editor can
	// show who is where; a decision two processes could reach differently is a
	// decision that will be.
	//
	// **Time is passed in, never read**, like everything else at this layer: a
	// guard is something a suite states rather than waits for.
	//
	// @since v0.13
	class LockTable {
	  public:
		// @param settings How long a grant lasts, and how much may queue.
		explicit LockTable(const LockSettings &settings = {});

		// Asks for a turn on a subtree.
		//
		// **Idempotent for an editor that already holds it.** An editor
		// publishing twice in a row asks twice, and the second ask has to be a
		// grant rather than a place in a queue behind itself.
		//
		// @param path       The subtree.
		// @param holder     Who is asking.
		// @param nowSeconds The current time.
		// @return Whether they may go ahead, are waiting, or were refused.
		Turn Request(const InstancePath &path, EditorId holder, double nowSeconds);

		// Gives a turn back, and says who gets it next.
		//
		// @param path       The subtree.
		// @param holder     Who is giving it up. Somebody else's turn is left
		//        alone — a release that could end another editor's turn would
		//        be a queue anybody can jump.
		// @param nowSeconds The current time.
		// @return Everybody who was waiting and may now go, in the order they
		//         asked. **In order, because that is the promise**: whoever was
		//         there first goes first, and the next edit lands on top.
		std::vector<Waiting> Release(const InstancePath &path, EditorId holder, double nowSeconds);

		// Gives up everything one editor holds or is waiting for.
		//
		// What a departure calls, and what makes a clean exit immediate — the
		// guard is what makes a crash survivable.
		//
		// @param holder     Who left.
		// @param nowSeconds The current time.
		// @return Everybody who may now go.
		std::vector<Waiting> ReleaseAll(EditorId holder, double nowSeconds);

		// Drops every grant whose guard has fired, and says who may go.
		//
		// @param nowSeconds The current time.
		// @return Everybody who may now go.
		std::vector<Waiting> Expire(double nowSeconds);

		// Who holds the subtree a path sits in, if anybody.
		//
		// @param path       The path.
		// @param nowSeconds The current time.
		// @return The lease, or null when the turn is free.
		const Lease *HolderOf(const InstancePath &path, double nowSeconds) const;

		// Replaces the whole table.
		//
		// **What a guest does with what the host sent.** A snapshot rather than
		// a difference, because the table is small, it changes rarely, and a
		// guest that missed one difference would show the wrong person's name
		// on a model until something else happened to correct it.
		//
		// @param leases What the host holds.
		void Adopt(std::span<const Lease> leases);

		// Every turn in progress.
		//
		// @return The leases, valid until the next call that changes the table.
		std::span<const Lease> Held() const {
			return Leases;
		}

		// Everybody waiting, in the order they asked.
		//
		// @return The queue.
		std::span<const Waiting> Queue() const {
			return Waiters;
		}

		// Empties the table.
		void Clear();

	  private:
		// Everybody in the queue who may now go, removed from it as they are.
		std::vector<Waiting> Wake(double nowSeconds);

		// Whether anything overlapping is held by somebody else.
		bool Busy(const InstancePath &path, EditorId holder, double nowSeconds) const;

		LockSettings Limits;
		std::vector<Lease> Leases;
		std::vector<Waiting> Waiters;
	};
}
