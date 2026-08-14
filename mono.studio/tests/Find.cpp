// The Find predicate, which is the half of the panel that can be silently
// wrong.
//
// **A filter that quietly matches nothing looks exactly like a scene that
// contains nothing**, and neither the panel nor the person reading it can tell
// the two apart. So `MatchesQuery` is a free function over a `Store` and this
// file exercises it directly; the widgets around it need a window and are
// covered by running the editor.
//
// The property this suite is really pinning: **the predicate names no
// property.** Every case below asks about `Transparency`, `Anchored` or
// `Material` without the matcher having heard of any of them, because they
// arrive through `PropertyDescriptor` like everything else. A change that made
// Find special-case a type would still pass the cases that use that type and
// fail the ones that do not.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.find")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using studio::FindQuery;
using studio::MatchesQuery;

namespace {
	struct Fixture {
		Store World{"find_test"};

		Fixture() {
			engine::scene::EnsureClassTree();
		}

		Entity Part(const char *name) {
			const Entity made = World.CreateInstance(engine::scene::PartClass(), name);
			REQUIRE(made != NULL_ENTITY);
			return made;
		}
	};

	bool Matches(const Store &store, Entity instance, const FindQuery &query) {
		std::string matched;
		return MatchesQuery(store, instance, query, matched);
	}
}

TEST_CASE("an empty query matches every instance", "[studio][find]") {
	// The identity case, and it decides the shape of the control: an empty
	// field does not filter. If an empty query matched nothing, "show me
	// everything" would need a mode of its own.
	Fixture fixture;
	const Entity part = fixture.Part("Anything");

	CHECK(Matches(fixture.World, part, FindQuery{}));
}

TEST_CASE("the class filter is IsA, not an exact name", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery exact;
	exact.Class = "Part";
	CHECK(Matches(fixture.World, part, exact));

	// **The point of using the class tree rather than a string compare.** A
	// `Part` is a `BasePart`, so searching for the base finds the leaf - which
	// is what somebody means by "every part" and what set inclusion already
	// says.
	FindQuery base;
	base.Class = "BasePart";
	CHECK(Matches(fixture.World, part, base));

	FindQuery unrelated;
	unrelated.Class = "Script";
	CHECK_FALSE(Matches(fixture.World, part, unrelated));
}

TEST_CASE("a class nobody registered matches nothing rather than everything", "[studio][find]") {
	// The direction to fail in. A filter that fell back to "match all" when it
	// did not recognise the class would answer a typo with the whole scene.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Class = "NotAClass";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("the name filter is a case-insensitive substring", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("RedDoorFrame");

	FindQuery middle;
	middle.Name = "door";
	CHECK(Matches(fixture.World, part, middle));

	FindQuery missing;
	missing.Name = "window";
	CHECK_FALSE(Matches(fixture.World, part, missing));
}

TEST_CASE("a property is found by name with no value asked for", "[studio][find]") {
	// **`Transparency` is never written down in the matcher.** It arrives
	// through `PropertiesOf`, which is why a property declared by any module
	// tomorrow is searchable with nothing here changing.
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "Transparency";

	std::string matched;
	REQUIRE(MatchesQuery(fixture.World, part, query, matched));

	// And the row says what matched, so a result list is readable without
	// clicking every entry.
	CHECK(matched.find("Transparency") != std::string::npos);
}

TEST_CASE("a property nothing declares matches nothing", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	FindQuery query;
	query.Property = "NoSuchProperty";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("a value is matched through its rendered text", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Booleans, numbers and enums all go through `FormatValue`, so one
	// predicate covers every type - and `Anchored` is a *structural* property
	// on top of that, presence of a tag spelled as a bool, which the matcher
	// also never learns.
	const bool anchored = true;
	REQUIRE(fixture.World.SetProperty(part, Name("Anchored"), &anchored, sizeof(anchored)));

	FindQuery query;
	query.Property = "Anchored";
	query.Value = "true";
	CHECK(Matches(fixture.World, part, query));

	FindQuery other;
	other.Property = "Anchored";
	other.Value = "false";
	CHECK_FALSE(Matches(fixture.World, part, other));
}

TEST_CASE("exact compares through the type rather than the text", "[studio][find]") {
	// **The case the `exact` checkbox exists for.** A substring match on the
	// rendered text says 0.5 is in "0.55", which is right for browsing and
	// wrong for "which parts are exactly half transparent".
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	const float transparency = 0.55f;
	fixture.World.SetProperty(part, Name("Transparency"), &transparency, sizeof(transparency));

	FindQuery loose;
	loose.Property = "Transparency";
	loose.Value = "0.5";
	CHECK(Matches(fixture.World, part, loose));

	FindQuery exact = loose;
	exact.Exact = true;
	CHECK_FALSE(Matches(fixture.World, part, exact));

	FindQuery right;
	right.Property = "Transparency";
	right.Value = "0.55";
	right.Exact = true;
	CHECK(Matches(fixture.World, part, right));
}

TEST_CASE("filters combine with and, not or", "[studio][find]") {
	Fixture fixture;
	const Entity part = fixture.Part("Block");

	// Right class, wrong name: not a match. An `or` would return it and make
	// every additional filter widen the search instead of narrowing it.
	FindQuery query;
	query.Class = "Part";
	query.Name = "NotThisOne";
	CHECK_FALSE(Matches(fixture.World, part, query));
}

TEST_CASE("an entity that is not an instance never matches", "[studio][find]") {
	// A module's own storage row has no class, and the explorer does not show
	// it. Find must agree, or the result list contains things nothing can
	// select.
	Fixture fixture;
	const Entity bare = fixture.World.Create();
	REQUIRE(bare != NULL_ENTITY);

	CHECK_FALSE(Matches(fixture.World, bare, FindQuery{}));
}
