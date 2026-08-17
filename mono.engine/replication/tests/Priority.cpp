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

	CHECK(score(Client(), At(75)) == Approx(0.25f));
}

TEST_CASE("past the falloff everything scores nothing", "[replication][priority]") {
	const DistancePriority score = Ramp(10.0f);

	CHECK(score(Client(), At(10)) == Approx(0.0f));
	CHECK(score(Client(), At(1000)) == Approx(0.0f));

	CHECK(score(Client(), At(1000)) >= 0.0f);
}

TEST_CASE("a client with no viewpoint scores everything the same", "[replication][priority]") {
	DistancePriority score = Ramp();
	score.Viewpoint = [](ClientId, Vector3 &) { return false; };

	CHECK(score(Client(), At(0)) == Approx(0.0f));
	CHECK(score(Client(), At(1000)) == Approx(0.0f));
}

TEST_CASE("an entity with no position sinks to the bottom", "[replication][priority]") {
	DistancePriority score = Ramp();
	score.Position = [](Entity, Vector3 &) { return false; };

	CHECK(score(Client(), At(0)) == Approx(0.0f));
}

TEST_CASE("half a scorer scores everything the same", "[replication][priority]") {
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
		out = Vector3{3.0f, 5.0f, 4.0f};
		return true;
	};

	const float distance = std::sqrt(9.0f + 25.0f + 16.0f);
	CHECK(score(Client(), At(0)) == Approx(1.0f - distance / 10.0f));
}

TEST_CASE("two clients in different places order the world differently", "[replication][priority]") {
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

TEST_CASE("scoring never asks whether anything is in the way", "[replication][priority]") {
	// The split. `operator()` is asked about every entity for every client and
	// `Refine` is asked about the rows in contention, so an occlusion query
	// reached from the scorer would be the expensive half running everywhere -
	// which is the thing the two halves exist to stop.
	int asked = 0;

	DistancePriority score = Ramp(100.0f);
	score.Blocked = [&asked](ClientId, Entity) {
		asked++;
		return true;
	};

	CHECK(score(Client(), At(10)) == Approx(0.9f));
	CHECK(asked == 0);
}

TEST_CASE("something out of sight scores less but not nothing", "[replication][priority]") {
	DistancePriority score = Ramp(100.0f);
	score.Blocked = [](ClientId, Entity entity) { return entity.Id >= 50; };

	const float visible = score.Refine(Client(), At(10), score(Client(), At(10)));
	const float hidden = score.Refine(Client(), At(60), score(Client(), At(60)));

	CHECK(visible == Approx(0.9f));
	CHECK(hidden == Approx(0.4f * 0.25f));

	CHECK(hidden > score(Client(), At(95)));
	CHECK(hidden < visible);
}

TEST_CASE("refining only ever lowers a score", "[replication][priority]") {
	// What lets `Authority` treat an unrefined score as an upper bound and stop
	// asking about rows no tick could reach. A refinement that raised one would
	// let a row it never asked about be overtaken invisibly.
	DistancePriority score = Ramp(100.0f);
	score.Blocked = [](ClientId, Entity) { return true; };

	for (const uint64_t at : {5u, 25u, 50u, 75u, 99u}) {
		const float plain = score(Client(), At(at));
		CHECK(score.Refine(Client(), At(at), plain) <= plain);
	}
}

TEST_CASE("occlusion is not asked about below the floor", "[replication][priority]") {
	int asked = 0;

	DistancePriority score = Ramp(100.0f);
	score.OcclusionFloor = 0.5f;
	score.Blocked = [&asked](ClientId, Entity) {
		asked++;
		return true;
	};

	CHECK(score.Refine(Client(), At(10), 0.9f) == Approx(0.9f * 0.25f));
	CHECK(asked == 1);

	CHECK(score.Refine(Client(), At(70), 0.3f) == Approx(0.3f));
	CHECK(asked == 1);

	// Exactly at the floor is below it. `!(score > floor)` rather than
	// `score <= floor`, so a NaN score is refused rather than being carried
	// into a raycast.
	CHECK(score.Refine(Client(), At(50), 0.5f) == Approx(0.5f));
	CHECK(asked == 1);
}

TEST_CASE("no occlusion query is the behaviour from before there was one", "[replication][priority]") {
	const DistancePriority score = Ramp(100.0f);
	CHECK(score(Client(), At(10)) == Approx(0.9f));
	CHECK(score.Refine(Client(), At(10), 0.9f) == Approx(0.9f));
}

TEST_CASE("a nonsense hidden fraction cannot break the ordering", "[replication][priority]") {
	const float nan = std::numeric_limits<float>::quiet_NaN();

	DistancePriority score = Ramp(100.0f);
	score.Blocked = [](ClientId, Entity) { return true; };

	score.HiddenFraction = nan;
	CHECK(std::isfinite(score.Refine(Client(), At(10), 0.9f)));

	score.HiddenFraction = 5.0f;
	CHECK(score.Refine(Client(), At(10), 0.9f) == Approx(0.9f));

	score.HiddenFraction = -1.0f;
	CHECK(score.Refine(Client(), At(10), 0.9f) == Approx(0.0f));
}
