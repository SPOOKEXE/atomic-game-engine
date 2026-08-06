// The distance score `Authority::SetPriority` has been waiting for since v0.4.
//
// **The thing worth testing is the separation, not the arithmetic.** The
// arithmetic is a subtraction and a clamp; what matters is that a scorer with a
// missing accessor, a client with no viewpoint, an entity with no position and
// a caller supplying a NaN all produce a *finite* score — because `std::sort`
// on a comparator that is not a weak ordering is undefined rather than merely
// surprising, and every one of those four comes from outside this module.

#include <engine/replication/Priority.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.replication.priority")

using Catch::Approx;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::replication::ClientId;
using engine::replication::DistancePriority;

namespace {
	ClientId Client(uint32_t index = 0) {
		ClientId id;
		id.Index = index;
		id.Generation = 1;
		return id;
	}

	Entity At(uint32_t id) {
		Entity entity;
		entity.Id = id;
		return entity;
	}

	// A scorer with a viewpoint at the origin and entities on the X axis at
	// their own id, in metres.
	DistancePriority Ramp(float falloff = 64.0f) {
		DistancePriority score;
		score.FalloffMetres = falloff;
		score.Viewpoint = [](ClientId, Vector3 &out) {
			out = Vector3{0.0f, 0.0f, 0.0f};
			return true;
		};
		score.Position = [](Entity entity, Vector3 &out) {
			out = Vector3{static_cast<float>(entity.Id), 0.0f, 0.0f};
			return true;
		};
		return score;
	}
}

TEST_CASE("nearer scores higher, and the ramp is linear", "[replication][priority]") {
	const DistancePriority score = Ramp(100.0f);

	CHECK(score(Client(), At(0)) == Approx(1.0f));
	CHECK(score(Client(), At(25)) == Approx(0.75f));
	CHECK(score(Client(), At(50)) == Approx(0.5f));

	// **Linear rather than inverse square, and the midpoint is the check.** An
	// inverse square collapses to almost nothing within a few metres, so
	// everything past that range sorts identically and the rotation decides
	// anyway — which is the behaviour the score exists to improve on.
	CHECK(score(Client(), At(75)) == Approx(0.25f));
}

TEST_CASE("past the falloff everything scores nothing", "[replication][priority]") {
	const DistancePriority score = Ramp(10.0f);

	CHECK(score(Client(), At(10)) == Approx(0.0f));
	CHECK(score(Client(), At(1000)) == Approx(0.0f));

	// Clamped rather than negative: a negative score would sort *below* an
	// entity with no position at all, which is not a distinction worth making.
	CHECK(score(Client(), At(1000)) >= 0.0f);
}

TEST_CASE("a client with no viewpoint scores everything the same", "[replication][priority]") {
	// The round robin the score replaces, and the right answer for a client
	// that has just joined or a spectator the host does not place — rather than
	// pretending it stands at the origin.
	DistancePriority score = Ramp();
	score.Viewpoint = [](ClientId, Vector3 &) { return false; };

	CHECK(score(Client(), At(0)) == Approx(0.0f));
	CHECK(score(Client(), At(1000)) == Approx(0.0f));
}

TEST_CASE("an entity with no position sinks to the bottom", "[replication][priority]") {
	DistancePriority score = Ramp();
	score.Position = [](Entity, Vector3 &) { return false; };

	// A thing with no place in the world has no claim on a budget ahead of one
	// that has.
	CHECK(score(Client(), At(0)) == Approx(0.0f));
}

TEST_CASE("half a scorer scores everything the same", "[replication][priority]") {
	// The safe thing for a host that wired one accessor and forgot the other:
	// the behaviour `Authority` has with no hook at all, rather than a crash.
	DistancePriority missingPosition;
	missingPosition.Viewpoint = [](ClientId, Vector3 &out) {
		out = Vector3{};
		return true;
	};
	CHECK(missingPosition(Client(), At(0)) == Approx(0.0f));

	DistancePriority empty;
	CHECK(empty(Client(), At(0)) == Approx(0.0f));
}

TEST_CASE("nothing a caller supplies can produce a non-finite score", "[replication][priority]") {
	// **The property that makes this safe to hand to `std::sort`.** A NaN in a
	// comparator is not a weak ordering; `Authority` guards this too, and a
	// scorer that could not be trusted on its own is one every caller has to
	// remember to wrap.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();

	SECTION("a NaN position") {
		DistancePriority score = Ramp();
		score.Position = [nan](Entity, Vector3 &out) {
			out = Vector3{nan, nan, nan};
			return true;
		};
		CHECK(std::isfinite(score(Client(), At(0))));
	}

	SECTION("an infinite viewpoint") {
		DistancePriority score = Ramp();
		score.Viewpoint = [infinity](ClientId, Vector3 &out) {
			out = Vector3{infinity, 0.0f, 0.0f};
			return true;
		};
		CHECK(std::isfinite(score(Client(), At(0))));
	}

	SECTION("a zero or negative falloff") {
		CHECK(std::isfinite(Ramp(0.0f)(Client(), At(1))));
		CHECK(std::isfinite(Ramp(-5.0f)(Client(), At(1))));
		CHECK(Ramp(0.0f)(Client(), At(1)) == Approx(0.0f));
	}

	SECTION("a non-finite falloff") {
		CHECK(std::isfinite(Ramp(nan)(Client(), At(1))));
		CHECK(std::isfinite(Ramp(infinity)(Client(), At(1))));
	}
}

TEST_CASE("distance is measured in three dimensions", "[replication][priority]") {
	DistancePriority score;
	score.FalloffMetres = 10.0f;
	score.Viewpoint = [](ClientId, Vector3 &out) {
		out = Vector3{0.0f, 0.0f, 0.0f};
		return true;
	};
	score.Position = [](Entity, Vector3 &out) {
		// A 3-4-5 triangle in the XZ plane, five metres up: 3, 4 and 5 give
		// a distance of exactly `sqrt(9 + 16 + 25)`.
		out = Vector3{3.0f, 5.0f, 4.0f};
		return true;
	};

	const float distance = std::sqrt(9.0f + 25.0f + 16.0f);
	CHECK(score(Client(), At(0)) == Approx(1.0f - distance / 10.0f));
}

TEST_CASE("two clients in different places order the world differently", "[replication][priority]") {
	// The whole point of the hook: the score is per client, so one player's
	// neighbour is not everybody's.
	DistancePriority score;
	score.FalloffMetres = 100.0f;
	score.Viewpoint = [](ClientId client, Vector3 &out) {
		out = Vector3{client.Index == 0 ? 0.0f : 100.0f, 0.0f, 0.0f};
		return true;
	};
	score.Position = [](Entity entity, Vector3 &out) {
		out = Vector3{static_cast<float>(entity.Id), 0.0f, 0.0f};
		return true;
	};

	CHECK(score(Client(0), At(10)) > score(Client(0), At(90)));
	CHECK(score(Client(1), At(90)) > score(Client(1), At(10)));
}

TEST_CASE("something out of sight scores less but not nothing", "[replication][priority]") {
	// **Not zero, and that is the whole design.** An entity scored at nothing
	// is one the rotation alone ever sends — so a player walking round a corner
	// finds everything behind it stale, and the first thing they see is a wall
	// of objects snapping into place. A hidden thing should update *less*.
	DistancePriority score = Ramp(100.0f);
	score.Blocked = [](ClientId, Entity entity) { return entity.Id >= 50; };

	const float visible = score(Client(), At(10));
	const float hidden = score(Client(), At(60));

	CHECK(visible == Approx(0.9f));
	CHECK(hidden == Approx(0.4f * 0.25f));

	// It still beats the far half of the world.
	CHECK(hidden > score(Client(), At(95)));
	CHECK(hidden < visible);
}

TEST_CASE("occlusion is not asked about below the floor", "[replication][priority]") {
	// **The cheap test gates the expensive one.** A raycast is orders of
	// magnitude dearer than a subtraction, and something already scoring near
	// nothing cannot be moved far enough by occlusion to change its place.
	int asked = 0;

	DistancePriority score = Ramp(100.0f);
	score.OcclusionFloor = 0.5f;
	score.Blocked = [&asked](ClientId, Entity) {
		asked++;
		return true;
	};

	// Well inside the floor: asked, and scaled.
	CHECK(score(Client(), At(10)) == Approx(0.9f * 0.25f));
	CHECK(asked == 1);

	// Below it: not asked at all, and the distance score stands.
	CHECK(score(Client(), At(70)) == Approx(0.3f));
	CHECK(asked == 1);
}

TEST_CASE("no occlusion query is the behaviour from before there was one", "[replication][priority]") {
	const DistancePriority score = Ramp(100.0f);
	CHECK(score(Client(), At(10)) == Approx(0.9f));
}

TEST_CASE("a nonsense hidden fraction cannot break the ordering", "[replication][priority]") {
	const float nan = std::numeric_limits<float>::quiet_NaN();

	DistancePriority score = Ramp(100.0f);
	score.Blocked = [](ClientId, Entity) { return true; };

	score.HiddenFraction = nan;
	CHECK(std::isfinite(score(Client(), At(10))));

	// Clamped rather than trusted: a fraction above one would score a hidden
	// thing *higher* than the same thing in plain sight.
	score.HiddenFraction = 5.0f;
	CHECK(score(Client(), At(10)) == Approx(0.9f));

	score.HiddenFraction = -1.0f;
	CHECK(score(Client(), At(10)) == Approx(0.0f));
}
