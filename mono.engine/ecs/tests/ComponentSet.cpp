#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.ecs.componentset")

using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::ComponentSet;

namespace {
	struct Alpha {
		int Value = 0;
	};
	struct Beta {
		int Value = 0;
	};
	struct Gamma {
		int Value = 0;
	};
	struct Delta {
		int Value = 0;
	};

	ComponentId A() {
		return Components::Of<Alpha>();
	}
	ComponentId B() {
		return Components::Of<Beta>();
	}
	ComponentId C() {
		return Components::Of<Gamma>();
	}
	ComponentId D() {
		return Components::Of<Delta>();
	}

	bool Ascending(const ComponentSet &set) {
		return std::is_sorted(set.Ids().begin(), set.Ids().end());
	}
}

TEST_CASE("the same components intern to the same set", "[ecs]") {
	const ComponentSet &first = ComponentSet::Intern({A(), B()});
	const ComponentSet &second = ComponentSet::Intern({A(), B()});

	// Identity, not equality: an archetype holds a reference to this.
	REQUIRE(&first == &second);
	REQUIRE(first == second);
	REQUIRE(first.Id() == second.Id());
}

TEST_CASE("order does not change the set", "[ecs]") {
	// The canonical form is what makes an archetype lookup a hash of one
	// number. Two systems naming the same components in different orders must
	// not build two tables.
	const ComponentSet &forward = ComponentSet::Intern({A(), B(), C()});
	const ComponentSet &backward = ComponentSet::Intern({C(), B(), A()});
	const ComponentSet &shuffled = ComponentSet::Intern({B(), A(), C()});

	REQUIRE(&forward == &backward);
	REQUIRE(&forward == &shuffled);
	REQUIRE(Ascending(forward));
}

TEST_CASE("duplicates collapse", "[ecs]") {
	const ComponentSet &once = ComponentSet::Intern({A()});
	const ComponentSet &twice = ComponentSet::Intern({A(), A(), A()});

	REQUIRE(&once == &twice);
	REQUIRE(once.Size() == 1);
}

TEST_CASE("invalid ids are dropped rather than stored", "[ecs]") {
	// The corrupt-snapshot path: a file naming a component this build does not
	// have resolves to an invalid id, and the set has to remain usable.
	const ComponentSet &set = ComponentSet::Intern({A(), ComponentId{}, B()});

	REQUIRE(set.Size() == 2);
	REQUIRE(set.Contains(A()));
	REQUIRE(set.Contains(B()));
	REQUIRE_FALSE(set.Contains(ComponentId{}));
}

TEST_CASE("the empty set exists and is interned", "[ecs]") {
	// An entity with no components is not a special case; it lives in the
	// archetype the empty set names.
	const ComponentSet &empty = ComponentSet::Empty();
	REQUIRE(empty.IsEmpty());
	REQUIRE(empty.Size() == 0);
	REQUIRE(&ComponentSet::Intern({}) == &empty);
	REQUIRE(&ComponentSet::Intern({ComponentId{}}) == &empty);
}

TEST_CASE("membership is exact", "[ecs]") {
	const ComponentSet &set = ComponentSet::Intern({A(), C()});

	REQUIRE(set.Contains(A()));
	REQUIRE(set.Contains(C()));
	REQUIRE_FALSE(set.Contains(B()));
	REQUIRE_FALSE(set.Contains(D()));
}

TEST_CASE("subset testing is what a query match is", "[ecs]") {
	const ComponentSet &wide = ComponentSet::Intern({A(), B(), C(), D()});

	// A query for <A, C> matches an archetype holding A, B, C and D.
	const ComponentId wanted[] = {A(), C()};
	REQUIRE(wide.ContainsAll(wanted));

	// And an archetype missing one term does not match.
	const ComponentSet &narrow = ComponentSet::Intern({A(), B()});
	REQUIRE_FALSE(narrow.ContainsAll(wanted));

	// An empty requirement matches everything, which is what an unconstrained
	// iteration over every entity means.
	REQUIRE(narrow.ContainsAll({}));
	REQUIRE(ComponentSet::Empty().ContainsAll({}));

	// An invalid term matches nothing, rather than being skipped. A query
	// naming a component this build does not have must return no rows, not
	// every row.
	const ComponentId missing[] = {A(), ComponentId{}};
	REQUIRE_FALSE(wide.ContainsAll(missing));
}

TEST_CASE("With and Without are the archetype graph's edges", "[ecs]") {
	const ComponentSet &base = ComponentSet::Intern({A()});

	const ComponentSet &grown = base.With(B());
	REQUIRE(grown.Size() == 2);
	REQUIRE(grown.Contains(A()));
	REQUIRE(grown.Contains(B()));
	REQUIRE(Ascending(grown));

	const ComponentSet &shrunk = grown.Without(B());
	REQUIRE(&shrunk == &base);
}

TEST_CASE("With and Without are no-ops when they would change nothing", "[ecs]") {
	const ComponentSet &set = ComponentSet::Intern({A(), B()});

	// Adding what is already there returns the same set rather than interning
	// a second copy — which is what keeps `Set` on an existing component from
	// moving the entity to a new archetype.
	REQUIRE(&set.With(A()) == &set);
	REQUIRE(&set.With(ComponentId{}) == &set);

	// Removing what was never there likewise.
	REQUIRE(&set.Without(D()) == &set);
	REQUIRE(&set.Without(ComponentId{}) == &set);
}

TEST_CASE("Without down to nothing reaches the empty set", "[ecs]") {
	const ComponentSet &single = ComponentSet::Intern({A()});
	const ComponentSet &none = single.Without(A());

	REQUIRE(none.IsEmpty());
	REQUIRE(&none == &ComponentSet::Empty());
}

TEST_CASE("a set reference stays valid as more are interned", "[ecs]") {
	// An archetype holds one of these for its whole life, while other worlds
	// keep interning new ones. A vector-backed table would move the storage and
	// leave every archetype pointing at freed memory.
	const ComponentSet &held = ComponentSet::Intern({A(), B(), C(), D()});
	const uint32_t id = held.Id();
	const size_t size = held.Size();

	struct Filler1 {
		int Value = 0;
	};
	struct Filler2 {
		int Value = 0;
	};
	struct Filler3 {
		int Value = 0;
	};

	const ComponentId fillers[] = {
		Components::Of<Filler1>(), Components::Of<Filler2>(), Components::Of<Filler3>()
	};
	for (const ComponentId filler : fillers) {
		ComponentSet::Intern({filler});
		ComponentSet::Intern({filler, A()});
		ComponentSet::Intern({filler, A(), B()});
	}

	REQUIRE(held.Id() == id);
	REQUIRE(held.Size() == size);
	REQUIRE(held.Contains(D()));
	REQUIRE(Ascending(held));
}

TEST_CASE("interning the same set from many threads yields one set", "[ecs]") {
	struct Contended1 {
		int Value = 0;
	};
	struct Contended2 {
		int Value = 0;
	};

	const ComponentId first = Components::Of<Contended1>();
	const ComponentId second = Components::Of<Contended2>();

	constexpr int THREADS = 8;
	std::vector<const ComponentSet *> seen(THREADS, nullptr);
	std::vector<std::thread> workers;

	for (int index = 0; index < THREADS; index++) {
		workers.emplace_back([&seen, index, first, second] {
			seen[index] = &ComponentSet::Intern({first, second});
		});
	}
	for (auto &worker : workers) {
		worker.join();
	}

	for (const ComponentSet *set : seen) {
		REQUIRE(set == seen.front());
	}
}

TEST_CASE("distinct sets get distinct ids", "[ecs]") {
	const ComponentSet &left = ComponentSet::Intern({A(), B()});
	const ComponentSet &right = ComponentSet::Intern({A(), C()});

	REQUIRE(left != right);
	REQUIRE(left.Id() != right.Id());
	REQUIRE(ComponentSet::Count() >= 2);
}
