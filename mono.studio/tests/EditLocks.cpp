// Who is holding what, and every way a hold is meant to fail.
//
// No sockets and no world: this is arithmetic over paths and a clock that is
// passed in, which is what makes an expiry something a case *states* rather
// than waits for. What it crosses is covered by `studio.editstream`.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <studio/EditLocks.hpp>
#include <vector>

TEST_SUITE_ID("studio.editlocks")

using studio::Blocked;
using studio::EditorId;
using studio::HOST_EDITOR;
using studio::InstancePath;
using studio::Lease;
using studio::LockSettings;
using studio::LockTable;

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
	CHECK(studio::Describe({}) == "nothing");
}

TEST_CASE("nobody is blocked by an empty table", "[studio][editlocks]") {
	LockTable locks;
	CHECK_FALSE(locks.Blocking(PART, ANA, 0.0).has_value());
	CHECK(locks.Held().empty());
	CHECK(locks.HolderOf(PART) == nullptr);
}

TEST_CASE("the first editor to touch a subtree holds it", "[studio][editlocks]") {
	LockTable locks;

	REQUIRE(locks.Hold(MODEL, ANA, 0.0));
	CHECK(locks.Held().size() == 1);

	// The holder is not blocked by their own hold, which is the whole point of
	// taking it.
	CHECK_FALSE(locks.Blocking(MODEL, ANA, 1.0).has_value());
	CHECK_FALSE(locks.Blocking(PART, ANA, 1.0).has_value());

	// Somebody else is, and is told what is in the way rather than merely that
	// something is — "Ana is editing Workspace.Model" explains a refusal on the
	// part in a way the part's own name does not.
	const std::optional<Blocked> blocked = locks.Blocking(PART, BEN, 1.0);
	REQUIRE(blocked.has_value());
	CHECK(blocked->Holder == ANA);
	CHECK(blocked->Subject == MODEL);

	// And a subtree nobody holds is free.
	CHECK_FALSE(locks.Blocking(OTHER, BEN, 1.0).has_value());
}

TEST_CASE("a hold covers upwards as well as downwards", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(PART, ANA, 0.0));

	// Somebody holding a part stops somebody else moving the model it is in.
	// Without this the model could be deleted out from under the hold, which
	// is the collision the lock exists to prevent wearing a different hat.
	const std::optional<Blocked> blocked = locks.Blocking(MODEL, BEN, 1.0);
	REQUIRE(blocked.has_value());
	CHECK(blocked->Subject == PART);
}

TEST_CASE("editing renews rather than accumulating", "[studio][editlocks]") {
	LockSettings settings;
	settings.HoldSeconds = 10.0;
	LockTable locks(settings);

	REQUIRE(locks.Hold(MODEL, ANA, 0.0));

	// Somebody working on a model and then dragging a part inside it is still
	// working on the model. A table that took a second hold for the child would
	// fill with one lease per part they touched.
	for (double now = 1.0; now < 40.0; now += 1.0) {
		REQUIRE(locks.Hold(PART, ANA, now));
		CHECK(locks.Held().size() == 1);
		CHECK(locks.Expire(now) == 0);
	}

	// And it is still held, forty seconds past a ten-second lease, because it
	// was renewed the whole way.
	CHECK(locks.Blocking(MODEL, BEN, 39.0).has_value());
}

TEST_CASE("a hold lapses on its own", "[studio][editlocks]") {
	LockSettings settings;
	settings.HoldSeconds = 10.0;
	LockTable locks(settings);

	REQUIRE(locks.Hold(MODEL, ANA, 0.0));
	CHECK(locks.Blocking(PART, BEN, 9.0).has_value());

	// **An editor that crashed must not hold a model for ever**, and there is
	// nobody to notice that it has. So the hold stops blocking at its expiry
	// whether or not anybody has swept it.
	CHECK_FALSE(locks.Blocking(PART, BEN, 10.0).has_value());

	CHECK(locks.Expire(9.0) == 0);
	CHECK(locks.Expire(10.0) == 1);
	CHECK(locks.Held().empty());

	// And now somebody else can take it.
	CHECK(locks.Hold(MODEL, BEN, 10.0));
}

TEST_CASE("a hold cannot be taken from its holder", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(MODEL, ANA, 0.0));

	CHECK_FALSE(locks.Hold(PART, BEN, 1.0));
	CHECK_FALSE(locks.Hold(MODEL, BEN, 1.0));
	CHECK(locks.Held().size() == 1);
	CHECK(locks.Held().front().Holder == ANA);

	// A release that could take another editor's lock would be a lock anybody
	// can pick.
	CHECK_FALSE(locks.Release(MODEL, BEN));
	CHECK(locks.Held().size() == 1);

	CHECK(locks.Release(MODEL, ANA));
	CHECK(locks.Held().empty());
}

TEST_CASE("releasing a parent releases what it covers", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(PART, ANA, 0.0));
	REQUIRE(locks.Hold(OTHER, ANA, 0.0));
	REQUIRE(locks.Held().size() == 2);

	// Letting go of the model lets go of the part inside it. Leaving the child
	// behind would be a lock somebody thought they had released.
	CHECK(locks.Release(MODEL, ANA));
	REQUIRE(locks.Held().size() == 1);
	CHECK(locks.Held().front().Subject == OTHER);
}

TEST_CASE("moving up replaces the hold rather than adding one", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(PART, ANA, 0.0));

	// Editing a model after editing one of its parts is one interaction and
	// should be one lease.
	REQUIRE(locks.Hold(MODEL, ANA, 1.0));
	REQUIRE(locks.Held().size() == 1);
	CHECK(locks.Held().front().Subject == MODEL);
}

TEST_CASE("leaving gives up everything at once", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(MODEL, ANA, 0.0));
	REQUIRE(locks.Hold(OTHER, BEN, 0.0));

	CHECK(locks.ReleaseAll(ANA) == 1);
	CHECK(locks.Held().size() == 1);
	CHECK(locks.Held().front().Holder == BEN);

	// Tidy rather than load-bearing: the expiry is what makes a crash
	// survivable, and this is what makes a clean exit immediate.
	CHECK(locks.ReleaseAll(ANA) == 0);
}

TEST_CASE("an empty path holds nothing and blocks nobody", "[studio][editlocks]") {
	LockTable locks;

	// An edit that names nothing is not an edit anybody can hold against.
	// Whatever it is, a lock is not the thing that should stop it.
	CHECK_FALSE(locks.Hold({}, ANA, 0.0));
	CHECK(locks.Held().empty());

	REQUIRE(locks.Hold(MODEL, ANA, 0.0));
	CHECK_FALSE(locks.Blocking({}, BEN, 1.0).has_value());
}

TEST_CASE("the table is bounded and a flood cannot evict a hold", "[studio][editlocks]") {
	LockSettings settings;
	settings.MaximumHolds = 4;
	LockTable locks(settings);

	std::vector<InstancePath> taken;
	for (int index = 0; index < 4; ++index) {
		InstancePath path{"Workspace", "Model" + std::to_string(index)};
		REQUIRE(locks.Hold(path, ANA, 0.0));
		taken.push_back(std::move(path));
	}

	// Past the cap a new hold is refused and the existing ones stand — the way
	// round `network::Directory` bounds its table, and for the same reason: a
	// bound that lets a flood push out what somebody is working on is not a
	// bound.
	for (int index = 4; index < 100; ++index) {
		CHECK_FALSE(locks.Hold({"Workspace", "Flood" + std::to_string(index)}, BEN, 0.0));
	}
	CHECK(locks.Held().size() == 4);
	for (const InstancePath &path : taken) {
		CHECK(locks.Blocking(path, BEN, 0.0).has_value());
	}

	// And a renewal of something already held still works when the table is
	// full, which is the other half of that: somebody working must not lose
	// their hold because somebody else started flooding.
	CHECK(locks.Hold(taken.front(), ANA, 1.0));
}

TEST_CASE("a claimed hold behaves exactly like an earned one", "[studio][editlocks]") {
	LockTable locks;

	REQUIRE(locks.Hold(MODEL, ANA, 0.0, true));
	CHECK(locks.Held().front().Claimed);

	// Both expire, both block, both renew. A table that treated them
	// differently would be two mechanisms wearing one name; the flag is only
	// so a panel can say which.
	CHECK(locks.Blocking(PART, BEN, 1.0).has_value());
	CHECK(locks.Expire(11.0) == 1);
}

TEST_CASE("a guest's copy is replaced whole", "[studio][editlocks]") {
	LockTable locks;
	REQUIRE(locks.Hold(MODEL, ANA, 0.0));

	// A snapshot rather than a difference: the table is small, it changes
	// rarely, and a guest that missed one difference would show the wrong
	// person's name on a model until something else happened to correct it.
	const std::vector<Lease> theirs{Lease{OTHER, BEN, 0.0, false}};
	locks.Adopt(theirs);

	REQUIRE(locks.Held().size() == 1);
	CHECK(locks.Held().front().Holder == BEN);
	CHECK(locks.HolderOf(OTHER) != nullptr);
	CHECK(locks.HolderOf(MODEL) == nullptr);

	locks.Adopt({});
	CHECK(locks.Held().empty());
}

TEST_CASE("the host is an editor like any other", "[studio][editlocks]") {
	LockTable locks;

	// A host that could edit through somebody else's hold would make the whole
	// thing advisory.
	REQUIRE(locks.Hold(MODEL, ANA, 0.0));
	CHECK(locks.Blocking(PART, HOST_EDITOR, 1.0).has_value());

	REQUIRE(locks.ReleaseAll(ANA) == 1);
	REQUIRE(locks.Hold(MODEL, HOST_EDITOR, 1.0));
	CHECK(locks.Blocking(PART, ANA, 2.0).has_value());
}
