// The class table on its own, apart from the tree built on top of it.
//
// Split out of `Instance.cpp` at v0.19, which was 1,689 lines covering two
// public headers. A suite is one file and one file is what the runner re-runs,
// so a change to `Classes.hpp` used to re-run every hierarchy, clone and churn
// case as well. `control/tests/Marshalling.cpp` had already declared
// `TEST_DEPENDS("engine.ecs.classes")` against a suite that did not exist.

#include "ClassTree.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.ecs.classes")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::PropertyType;
using engine::ecs::Store;

using namespace ecs_test;

TEST_CASE("a class is a name, a parent and a component set", "[ecs]") {
	const Tree &tree = ClassTree();

	REQUIRE(Classes::Describe(tree.Part).Name == Name("test.Part"));
	REQUIRE(Classes::Describe(tree.Part).Parent == tree.BasePart);
	REQUIRE(Classes::Find(Name("test.Part")) == tree.Part);
}

TEST_CASE("a derived class's set contains its base's", "[ecs]") {
	// Inheritance is set inclusion, which is what makes :IsA an ancestor test
	// and lets a query for the base match every derived instance.
	const Tree &tree = ClassTree();

	const auto &base = *Classes::Describe(tree.BasePart).Set;
	const auto &derived = *Classes::Describe(tree.Part).Set;

	REQUIRE(derived.ContainsAll(base.Ids()));
	REQUIRE(derived.Size() > base.Size());
}

TEST_CASE("IsA walks the ancestry and stops there", "[ecs]") {
	const Tree &tree = ClassTree();

	REQUIRE(Classes::IsA(tree.Part, tree.Part));
	REQUIRE(Classes::IsA(tree.Part, tree.BasePart));
	REQUIRE(Classes::IsA(tree.Part, tree.PVInstance));
	REQUIRE(Classes::IsA(tree.Part, tree.Instance));

	// Not the other way, and not across a sibling branch.
	REQUIRE_FALSE(Classes::IsA(tree.BasePart, tree.Part));
	REQUIRE_FALSE(Classes::IsA(tree.Part, tree.Model));
	REQUIRE_FALSE(Classes::IsA(tree.Model, tree.BasePart));

	REQUIRE_FALSE(Classes::IsA(ClassId{}, tree.Part));
	REQUIRE_FALSE(Classes::IsA(tree.Part, ClassId{}));
}

TEST_CASE("creatability does not change virtual class ancestry", "[ecs]") {
	const Tree &tree = ClassTree();
	const ClassId virtualBase = Classes::Register("test.VirtualBase", tree.Instance, {});
	const ClassId concrete = Classes::Register("test.VirtualLeaf", virtualBase, {});

	Classes::SetCreatable(virtualBase, false);

	CHECK_FALSE(Classes::Describe(virtualBase).Creatable);
	CHECK(Classes::Describe(concrete).Creatable);
	CHECK(Classes::IsA(concrete, virtualBase));
	CHECK(Classes::IsA(concrete, tree.Instance));
}

TEST_CASE("registering the same class name twice returns the same id", "[ecs]") {
	const Tree &tree = ClassTree();
	REQUIRE(Classes::Register("test.Part", {}) == tree.Part);
}

TEST_CASE("a field property generates a conversion that reads and writes it", "[ecs]") {
	const Tree &tree = ClassTree();
	const auto properties = Classes::Describe(tree.Part).Properties;

	const auto find = [&properties](const char *name) {
		for (const auto &property : properties) {
			if (property.Name == Name(name)) {
				return property;
			}
		}
		FAIL("no property named " << name);
		return properties.front();
	};

	const auto x = find("X");
	REQUIRE(x.Type == PropertyType::Float);
	REQUIRE(x.Kind == engine::ecs::PropertyKind::Field);
	REQUIRE(x.Size == sizeof(float));

	// The components it touches are declared rather than inferred. v0.6's
	// per-instance `.Changed` needs this to fan one component write out to
	// every property name observing it.
	REQUIRE(x.Reads->Contains(Components::Of<Transform>()));
	REQUIRE(x.Writes->Contains(Components::Of<Transform>()));

	// This is the assertion that used to be `Offset == 0`, and it is a stronger
	// one: a generated conversion pointed at the wrong field passes an offset
	// check and fails this.
	Store store("test");
	const Entity part = store.CreateInstance(tree.Part);

	float read = -1.0f;
	REQUIRE(x.Get(store, part, &read));
	REQUIRE(read == 0.0f);

	const float written = 4.5f;
	REQUIRE(x.Set(store, part, &written));
	REQUIRE(store.Get<Transform>(part)->X == 4.5f);
	REQUIRE(store.Get<Transform>(part)->Y == 0.0f);

	// The other field of the same component, so a conversion that ignored its
	// member pointer and wrote the front of the struct would fail here.
	const auto y = find("Y");
	const float second = -2.25f;
	REQUIRE(y.Set(store, part, &second));
	REQUIRE(store.Get<Transform>(part)->Y == -2.25f);
	REQUIRE(store.Get<Transform>(part)->X == 4.5f);

	REQUIRE(find("Visible").Type == PropertyType::Bool);
	REQUIRE(find("Mesh").Type == PropertyType::Name);
}

TEST_CASE("a derived class inherits its base's properties", "[ecs]") {
	const Tree &tree = ClassTree();

	// Part declares two properties and gets four more from above it.
	REQUIRE(Classes::Describe(tree.Part).Properties.size() == 6);
	REQUIRE(Classes::Describe(tree.PVInstance).Properties.size() == 2);
	REQUIRE(Classes::Describe(tree.Instance).Properties.empty());
}

TEST_CASE("an unregistered class describes as empty rather than crashing", "[ecs]") {
	REQUIRE(Classes::Describe(ClassId{}).Set == nullptr);
	REQUIRE(Classes::Describe(ClassId{0xFFFF'FFF0u}).Set == nullptr);
	REQUIRE_FALSE(Classes::Find(Name("test.never.registered")).IsValid());
}
