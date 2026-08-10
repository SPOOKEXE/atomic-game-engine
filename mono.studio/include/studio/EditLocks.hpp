#pragma once

// Who is holding what, so two editors do not fight over one model.
//
// The answer to the question `EditStream.hpp` used to say it had no answer for.
// Last write wins is fine for two people working in different corners and is
// exactly wrong for two people working on the same thing: the second write is
// applied everywhere, the first person's is gone, and neither of them is told.
//
// ## A lease on first touch, not a claim they have to ask for
//
// **Nobody asks for a lock and nobody waits for one.** The obvious design — a
// guest requests, the host grants, the guest then edits — costs a round trip
// *before the first edit of every interaction*, so clicking a part and dragging
// it would fail until the answer came back. That is a worse experience than the
// problem it solves.
//
// So the host grants implicitly: the first editor to touch a subtree holds it,
// every further edit renews the hold, and everybody else is refused until it
// lapses. The check happens where the ordering already is — one process decides
// who was first, which is the same reason the relay goes through the host.
//
// An explicit `Claim` exists on top of that for the case a person genuinely
// wants to reserve something before working on it, and it is the same table.
//
// ## Why a lease and not a lock
//
// An editor that crashes must not hold a model for ever, and there is nobody to
// notice that it has. So a hold expires on its own — `HoldSeconds` after the
// last edit that touched it — and an editor that is still working renews it
// simply by working. Leaving releases everything at once, which is the tidy
// path rather than the one correctness depends on.
//
// ## Why subtrees
//
// A lock over `Workspace.Model` that let somebody else edit
// `Workspace.Model.Part` would protect nothing: moving a model moves its
// children, and two people doing that at once is the case this exists for. So a
// hold covers a path and everything under it, and two holds conflict when
// either contains the other.
//
// ## What it is not
//
// **Not a permission system.** A hold says somebody is working there, not that
// they are allowed to and you are not. Everybody in a session already has the
// key that let them in; this stops collisions, not people.
//
// **Not enforced on the person who holds it.** The editor greys out what
// somebody else holds, and that is a courtesy; what is actually enforced is at
// the host, which refuses a waypoint touching somebody else's hold. A guest
// running a modified build cannot edit through it.
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
	// far as every hold is concerned, and the two edit through each other's
	// locks.
	//
	// @since v0.13
	using EditorId = uint32_t;

	// The host's own id. Never issued to a guest.
	inline constexpr EditorId HOST_EDITOR = 0;

	// One editor holding one subtree.
	//
	// @since v0.13
	struct Lease {
		// The subtree's root. Everything under it is held too.
		InstancePath Subject;

		// Who holds it.
		EditorId Holder = HOST_EDITOR;

		// When it lapses, on the host's clock.
		double ExpiresAtSeconds = 0.0;

		// Whether somebody asked for this rather than earning it by editing.
		//
		// **Only so a panel can say which.** An explicit hold and one taken by
		// touching a part behave identically — both expire, both block, both
		// renew — and a table that treated them differently would be two
		// mechanisms wearing one name.
		bool Claimed = false;
	};

	// How long a hold lasts and how many there may be.
	//
	// @since v0.13
	struct LockSettings {
		// How long after the last edit a hold lapses.
		//
		// **Long enough to cover thinking, short enough to survive a crash.**
		// Somebody dragging a model renews it constantly; somebody who alt-tabs
		// mid-thought should not have to re-take it; somebody whose editor died
		// should not block the model for the rest of the session. Ten seconds
		// is chosen rather than measured, and saying so is better than implying
		// otherwise.
		double HoldSeconds = 10.0;

		// The most holds one table keeps.
		//
		// Bounded like every other table fed from a wire: a guest that claimed
		// in a loop would otherwise be a guest spending the host's memory.
		// Past the cap a *new* hold is refused and the existing ones stand,
		// which is the same way round `network::Directory` does it and for the
		// same reason.
		size_t MaximumHolds = 256;
	};

	// Why an edit was refused.
	//
	// @since v0.13
	struct Blocked {
		// The path the holder holds — which may be an ancestor of what was
		// edited, and that is the useful thing to show a person: "Ana is
		// editing Workspace.Model" explains a refusal on
		// `Workspace.Model.Part` in a way the part's own name does not.
		InstancePath Subject;

		// Who holds it.
		EditorId Holder = HOST_EDITOR;
	};

	// Who is holding what.
	//
	// One per session, on the host. A guest keeps one too, filled from what the
	// host broadcasts, purely so the editor can grey out what somebody else has
	// — a guest's copy is never consulted to decide anything, because a decision
	// two processes could reach differently is a decision that will be.
	//
	// **Time is passed in, never read**, like everything else that crosses this
	// layer: an expiry is something a suite states rather than waits for.
	//
	// @since v0.13
	class LockTable {
	  public:
		// @param settings How long a hold lasts, and how many.
		explicit LockTable(const LockSettings &settings = {});

		// Whether `holder` may edit `path` right now.
		//
		// @param path       What is being edited.
		// @param holder     Who wants to.
		// @param nowSeconds The current time.
		// @return Nothing when the edit may go ahead — either nobody holds an
		//         overlapping subtree, or `holder` does. Otherwise who is in
		//         the way.
		std::optional<Blocked> Blocking(const InstancePath &path, EditorId holder, double nowSeconds) const;

		// Takes or renews a hold.
		//
		// **Renewing is the ordinary case and is why this is one call.** Every
		// edit an editor makes passes through here, so a person working on a
		// model holds it for as long as they keep working and for
		// `HoldSeconds` after they stop.
		//
		// @param path       The subtree.
		// @param holder     Who is holding it.
		// @param nowSeconds The current time.
		// @param claimed    Whether this was asked for rather than earned.
		// @return `false` when somebody else holds an overlapping subtree, or
		//         the table is full.
		bool Hold(const InstancePath &path, EditorId holder, double nowSeconds, bool claimed = false);

		// Gives up one hold.
		//
		// @param path   The subtree.
		// @param holder Who is giving it up. Somebody else's hold is left
		//        alone — a release that could take another editor's lock would
		//        be a lock anybody can pick.
		// @return Whether anything was released.
		bool Release(const InstancePath &path, EditorId holder);

		// Gives up everything one editor holds.
		//
		// What a departure calls. Tidy rather than load-bearing: the expiry is
		// what makes a crash survivable, and this is what makes a clean exit
		// immediate.
		//
		// @param holder Who left.
		// @return How many holds went.
		size_t ReleaseAll(EditorId holder);

		// Drops every hold that has lapsed.
		//
		// @param nowSeconds The current time.
		// @return How many went.
		size_t Expire(double nowSeconds);

		// Replaces the whole table.
		//
		// **What a guest does with what the host sent.** A snapshot rather than
		// a difference, because the table is small, it changes rarely, and a
		// guest that missed one difference would show the wrong person's name
		// on a model until something else happened to correct it.
		//
		// @param leases What the host holds.
		void Adopt(std::span<const Lease> leases);

		// Everything held, in no particular order.
		//
		// @return The leases, valid until the next call that changes the table.
		std::span<const Lease> Held() const {
			return Leases;
		}

		// Who holds the subtree a path sits in, whoever they are.
		//
		// For a panel: what to grey out, and whose name to put on it.
		//
		// @param path The path.
		// @return The lease, or null when nobody holds it.
		const Lease *HolderOf(const InstancePath &path) const;

		// Empties the table.
		void Clear();

	  private:
		LockSettings Limits;
		std::vector<Lease> Leases;
	};
}
