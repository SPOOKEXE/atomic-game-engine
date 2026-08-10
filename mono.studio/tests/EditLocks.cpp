// Who is holding what, and every way a hold is meant to fail.
//
// No sockets and no world: this is arithmetic over paths and a clock that is
// passed in, which is what makes an expiry something a case *states* rather
// than waits for. What it crosses is covered by `studio.editstream`.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <studio/EditLocks.hpp>
#include <vector>

TEST_SUITE_ID("studio.editlocks")

using studio::EditorId;
using studio::HOST_EDITOR;
using studio::InstancePath;
using studio::Lease;
using studio::LockSettings;
using studio::LockTable;
using studio::Turn;
using studio::Waiting;

namespace {
	constexpr EditorId ANA = 1;
	constexpr EditorId BEN = 2;

	const InstancePath MODEL{"Workspace", "Model"};
	const InstancePath PART{"Workspace", "Model", "Part"};
	const InstancePath OTHER{"Workspace", "Elsewhere"};
}

TEST_CASE("a path contains itself and everything under it", "[studio][editlocks]") {
	CHECK(studio::Contains(MODEL, MODEL));
	CHECK(studio::Contains(MODEL, PART));
	CHECK_FALSE(studio::Contains(PART, MODEL));
	CHECK_FALSE(studio::Contains(MODEL, OTHER));

	// **An empty path contains nothing.** A lock over nothing must not read as
	// a lock over everything, which is what a plain prefix test would make it.
	CHECK_FALSE(studio::Contains({}, PART));
	CHECK_FALSE(studio::Contains({}, {}));

	// Overlap is containment in either direction, which is what a lock is
	// asking about: moving a model moves its children.
	CHECK(studio::Overlaps(MODEL, PART));
	CHECK(studio::Overlaps(PART, MODEL));
	CHECK_FALSE(studio::Overlaps(PART, OTHER));

	CHECK(studio::Describe(PART) == "Workspace.Model.Part");
	// Spelled out, because `Describe` is also overloaded on `Turn` and a bare
	// `{}` picks whichever the compiler likes.
	CHECK(studio::Describe(InstancePath{}) == "nothing");
}

TEST_CASE("an empty table grants everybody", "[studio][editlocks]") {
	LockTable locks;
	CHECK(locks.Request(PART, ANA, 0.0) == Turn::Granted);
	CHECK(locks.Held().size() == 1);
	CHECK(locks.Queue().empty());
	CHECK(locks.HolderOf(PART, 0.0) != nullptr);
}

TEST_CASE("the second editor queues rather than being refused", "[studio][editlocks]") {
	LockTable locks;

	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);

	// **Nobody's work is thrown away, and that is the point of the queue.**
	// Somebody asking for a subtree in use is not doing anything wrong; they
	// are second, and second still gets a turn.
	CHECK(locks.Request(PART, BEN, 0.0) == Turn::Queued);
	REQUIRE(locks.Queue().size() == 1);
	CHECK(locks.Queue().front().Holder == BEN);

	// Asking again does not move them or duplicate them.
	CHECK(locks.Request(PART, BEN, 0.1) == Turn::Queued);
	CHECK(locks.Queue().size() == 1);

	// And when the first gives it back, the second goes.
	const std::vector<Waiting> woken = locks.Release(MODEL, ANA, 1.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == BEN);
	CHECK(locks.Queue().empty());
	REQUIRE(locks.HolderOf(PART, 1.0) != nullptr);
	CHECK(locks.HolderOf(PART, 1.0)->Holder == BEN);
}

TEST_CASE("turns come round in the order they were asked for", "[studio][editlocks]") {
	LockTable locks;
	constexpr EditorId CAT = 3;

	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	REQUIRE(locks.Request(MODEL, BEN, 0.1) == Turn::Queued);
	REQUIRE(locks.Request(MODEL, CAT, 0.2) == Turn::Queued);

	// **Whoever was there first goes first**, which is the promise the whole
	// queue exists to keep: the next edit lands on top of theirs, in order.
	std::vector<Waiting> woken = locks.Release(MODEL, ANA, 1.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == BEN);

	woken = locks.Release(MODEL, BEN, 2.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == CAT);

	woken = locks.Release(MODEL, CAT, 3.0);
	CHECK(woken.empty());
	CHECK(locks.Held().empty());
}

TEST_CASE("a release wakes everybody it unblocks", "[studio][editlocks]") {
	LockTable locks;
	constexpr EditorId CAT = 3;

	// One editor holding a model blocks two others waiting on separate parts
	// inside it. Both become free at once, and both are handed a turn.
	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	REQUIRE(locks.Request({"Workspace", "Model", "First"}, BEN, 0.1) == Turn::Queued);
	REQUIRE(locks.Request({"Workspace", "Model", "Second"}, CAT, 0.2) == Turn::Queued);

	const std::vector<Waiting> woken = locks.Release(MODEL, ANA, 1.0);
	REQUIRE(woken.size() == 2);
	CHECK(woken[0].Holder == BEN);
	CHECK(woken[1].Holder == CAT);
	CHECK(locks.Held().size() == 2);
}

TEST_CASE("a turn covers the subtree in both directions", "[studio][editlocks]") {
	LockTable locks;

	// Somebody holding a part stops somebody else moving the model it is in.
	// Without this the model could be deleted out from under the turn, which is
	// the collision the ordering exists to prevent wearing a different hat.
	REQUIRE(locks.Request(PART, ANA, 0.0) == Turn::Granted);
	CHECK(locks.Request(MODEL, BEN, 0.1) == Turn::Queued);

	// And a subtree nobody is on is free.
	CHECK(locks.Request(OTHER, BEN, 0.1) == Turn::Granted);
}

TEST_CASE("asking twice for what you already hold renews it", "[studio][editlocks]") {
	LockSettings settings;
	settings.GrantSeconds = 2.0;
	LockTable locks(settings);

	// **An editor publishing twice in a row asks twice**, and the second ask
	// has to be a grant rather than a place in a queue behind itself.
	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	CHECK(locks.Request(MODEL, ANA, 1.0) == Turn::Granted);
	CHECK(locks.Request(PART, ANA, 1.5) == Turn::Granted);
	CHECK(locks.Held().size() == 1);

	// Renewed each time, so the guard is measured from the last ask.
	CHECK(locks.Expire(3.0).empty());
	CHECK(locks.Held().size() == 1);
	CHECK(locks.Expire(3.6).empty());
	CHECK(locks.Held().empty());
}

TEST_CASE("a guard hands the turn on when an editor dies", "[studio][editlocks]") {
	LockSettings settings;
	settings.GrantSeconds = 2.0;
	LockTable locks(settings);

	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	REQUIRE(locks.Request(MODEL, BEN, 0.1) == Turn::Queued);

	// **Not a lease on editing — a bound on one protocol step.** An editor
	// granted a subtree that then dies would hold it for ever, and there is
	// nobody to notice; the guard costs the next person a pause rather than the
	// session.
	CHECK(locks.Expire(1.9).empty());

	const std::vector<Waiting> woken = locks.Expire(2.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == BEN);
	REQUIRE(locks.HolderOf(MODEL, 2.0) != nullptr);
	CHECK(locks.HolderOf(MODEL, 2.0)->Holder == BEN);
}

TEST_CASE("a turn cannot be ended by somebody else", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);

	// A release that could end another editor's turn would be a queue anybody
	// can jump.
	CHECK(locks.Release(MODEL, BEN, 1.0).empty());
	REQUIRE(locks.HolderOf(MODEL, 1.0) != nullptr);
	CHECK(locks.HolderOf(MODEL, 1.0)->Holder == ANA);

	CHECK(locks.Release(MODEL, ANA, 1.0).empty());
	CHECK(locks.Held().empty());
}

TEST_CASE("leaving gives up the turn and the place in the queue", "[studio][editlocks]") {
	LockTable locks;
	constexpr EditorId CAT = 3;

	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	REQUIRE(locks.Request(MODEL, BEN, 0.1) == Turn::Queued);
	REQUIRE(locks.Request(MODEL, CAT, 0.2) == Turn::Queued);

	// Somebody in the queue leaving takes their place with them rather than
	// leaving a turn nobody will ever claim.
	CHECK(locks.ReleaseAll(BEN, 1.0).empty());
	CHECK(locks.Queue().size() == 1);

	const std::vector<Waiting> woken = locks.ReleaseAll(ANA, 2.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == CAT);
}

TEST_CASE("an edit that names nothing takes no turn", "[studio][editlocks]") {
	LockTable locks;

	// Whatever it is, the queue is not the thing that should order it.
	CHECK(locks.Request({}, ANA, 0.0) == Turn::Granted);
	CHECK(locks.Held().empty());
}

TEST_CASE("the queue is bounded and a flood cannot take somebody's place", "[studio][editlocks]") {
	LockSettings settings;
	settings.MaximumWaiting = 2;
	LockTable locks(settings);

	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	REQUIRE(locks.Request(MODEL, BEN, 0.1) == Turn::Queued);
	REQUIRE(locks.Request(MODEL, 3, 0.2) == Turn::Queued);

	// Past the cap a new request is refused and the queue stands — a bound that
	// lets a flood push out somebody's place is not a bound. **Refused rather
	// than dropped silently**, because a request that is neither granted nor
	// queued is an editor waiting for a message that will never come.
	for (EditorId flood = 4; flood < 40; ++flood) {
		CHECK(locks.Request(MODEL, flood, 0.3) == Turn::Refused);
	}
	CHECK(locks.Queue().size() == 2);
	CHECK(locks.Queue().front().Holder == BEN);
}

TEST_CASE("a guest's copy is a picture, not a clock", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);

	// A snapshot rather than a difference: the table is small, it changes
	// rarely, and a guest that missed one difference would show the wrong
	// person's name on a model until something else corrected it.
	const std::vector<Lease> theirs{Lease{OTHER, BEN, 0.0}};
	locks.Adopt(theirs);

	REQUIRE(locks.Held().size() == 1);
	CHECK(locks.Held().front().Holder == BEN);

	// **The guard belongs to the host's clock and means nothing here.** Adopted
	// rows do not lapse at a moment the host never chose.
	CHECK(locks.HolderOf(OTHER, 1'000'000.0) != nullptr);
	CHECK(locks.HolderOf(MODEL, 0.0) == nullptr);

	locks.Adopt({});
	CHECK(locks.Held().empty());
}

TEST_CASE("the host takes its turn like anybody else", "[studio][editlocks]") {
	LockTable locks;

	// A host that could edit through somebody else's turn would make the whole
	// thing advisory.
	REQUIRE(locks.Request(MODEL, ANA, 0.0) == Turn::Granted);
	CHECK(locks.Request(PART, HOST_EDITOR, 0.1) == Turn::Queued);

	const std::vector<Waiting> woken = locks.Release(MODEL, ANA, 1.0);
	REQUIRE(woken.size() == 1);
	CHECK(woken.front().Holder == HOST_EDITOR);
}

TEST_CASE("every turn names itself", "[studio][editlocks]") {
	CHECK(std::string_view(studio::Describe(Turn::Granted)) == "granted");
	CHECK(std::string_view(studio::Describe(Turn::Queued)) == "queued");
	CHECK(std::string_view(studio::Describe(Turn::Refused)) == "refused");
}
