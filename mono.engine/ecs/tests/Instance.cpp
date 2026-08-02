#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.instance")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::core::Random;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Hierarchy;
using engine::ecs::InstanceClass;
using engine::ecs::NULL_ENTITY;
using engine::ecs::PropertyType;
using engine::ecs::Store;

namespace instance_test {
	struct Transform {
		float X = 0.0f;
		float Y = 0.0f;
	};
	struct Bounds {
		float HalfExtent = 0.5f;
	};
	struct Visual {
		Name Mesh;
		bool Visible = true;
	};
	struct Motion {
		float Speed = 0.0f;
	};

	// One class tree, registered once for the whole suite. Classes are
	// process-wide and never unregister, exactly like components.
	struct Tree {
		ClassId Instance;
		ClassId PVInstance;
		ClassId BasePart;
		ClassId Part;
		ClassId Model;
	};

	const Tree &Classes_() {
		static const Tree tree = [] {
			Tree built;
			built.Instance = Classes::Register("test.Instance", {});

			const ComponentId transform = Components::Register<Transform>("test.Transform");
			const ComponentId bounds = Components::Register<Bounds>("test.Bounds");
			const ComponentId visual = Components::Register<Visual>("test.Visual");
			const ComponentId motion = Components::Register<Motion>("test.Motion");

			const ComponentId pv[] = {transform};
			built.PVInstance = Classes::Register("test.PVInstance", built.Instance, pv);

			const ComponentId base[] = {bounds};
			built.BasePart = Classes::Register("test.BasePart", built.PVInstance, base);

			const ComponentId part[] = {visual};
			built.Part = Classes::Register("test.Part", built.BasePart, part);

			const ComponentId model[] = {motion};
			built.Model = Classes::Register("test.Model", built.PVInstance, model);

			// The prototype rows. A default declared on a base applies to
			// everything registered under it afterwards.
			Classes::Default<Bounds>(built.BasePart, Bounds{2.5f});
			Classes::Default<Visual>(built.Part, Visual{Name("test.mesh.cube"), true});

			Classes::Property<Transform>(built.PVInstance, "X", &Transform::X);
			Classes::Property<Transform>(built.PVInstance, "Y", &Transform::Y);
			Classes::Property<Bounds>(built.BasePart, "HalfExtent", &Bounds::HalfExtent);
			Classes::Property<Visual>(built.Part, "Visible", &Visual::Visible);
			Classes::Property<Visual>(built.Part, "Mesh", &Visual::Mesh);

			return built;
		}();
		return tree;
	}

	std::vector<Name> ChildNames(const Store &store, Entity parent) {
		std::vector<Name> names;
		store.EachChild(parent, [&](Entity child) { names.push_back(store.InstanceNameOf(child)); });
		return names;
	}
}

using namespace instance_test;

// --- the class table ------------------------------------------------------

TEST_CASE("a class is a name, a parent and a component set", "[ecs]") {
	const Tree &tree = Classes_();

	REQUIRE(Classes::Describe(tree.Part).Name == Name("test.Part"));
	REQUIRE(Classes::Describe(tree.Part).Parent == tree.BasePart);
	REQUIRE(Classes::Find(Name("test.Part")) == tree.Part);
}

TEST_CASE("a derived class's set contains its base's", "[ecs]") {
	// Inheritance is set inclusion, which is what makes :IsA an ancestor test
	// and lets a query for the base match every derived instance.
	const Tree &tree = Classes_();

	const auto &base = *Classes::Describe(tree.BasePart).Set;
	const auto &derived = *Classes::Describe(tree.Part).Set;

	REQUIRE(derived.ContainsAll(base.Ids()));
	REQUIRE(derived.Size() > base.Size());
}

TEST_CASE("IsA walks the ancestry and stops there", "[ecs]") {
	const Tree &tree = Classes_();

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

TEST_CASE("registering the same class name twice returns the same id", "[ecs]") {
	const Tree &tree = Classes_();
	REQUIRE(Classes::Register("test.Part", {}) == tree.Part);
}

TEST_CASE("properties resolve to a component and an offset", "[ecs]") {
	const Tree &tree = Classes_();
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

	// Measured from a real object rather than declared, so a reordered field
	// cannot point a binding at the wrong bytes.
	const auto x = find("X");
	const auto y = find("Y");
	REQUIRE(x.Component == Components::Of<Transform>());
	REQUIRE(x.Offset == 0);
	REQUIRE(y.Offset == sizeof(float));
	REQUIRE(x.Type == PropertyType::Float);

	REQUIRE(find("Visible").Type == PropertyType::Bool);
	REQUIRE(find("Mesh").Type == PropertyType::Name);
}

TEST_CASE("a derived class inherits its base's properties", "[ecs]") {
	const Tree &tree = Classes_();

	// Part declares one property and gets four more from above it.
	REQUIRE(Classes::Describe(tree.Part).Properties.size() == 5);
	REQUIRE(Classes::Describe(tree.PVInstance).Properties.size() == 2);
	REQUIRE(Classes::Describe(tree.Instance).Properties.empty());
}

TEST_CASE("an unregistered class describes as empty rather than crashing", "[ecs]") {
	REQUIRE(Classes::Describe(ClassId{}).Set == nullptr);
	REQUIRE(Classes::Describe(ClassId{0xFFFF'FFF0u}).Set == nullptr);
	REQUIRE_FALSE(Classes::Find(Name("test.never.registered")).IsValid());
}

// --- creating instances ---------------------------------------------------

TEST_CASE("Instance.new lands in the class's archetype", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity part = store.CreateInstance(tree.Part, "Baseplate");

	REQUIRE(store.Alive(part));
	REQUIRE(store.ClassOf(part) == tree.Part);
	REQUIRE(store.InstanceNameOf(part) == Name("Baseplate"));

	// Everything the class names, and nothing it does not.
	REQUIRE(store.Has<Transform>(part));
	REQUIRE(store.Has<Bounds>(part));
	REQUIRE(store.Has<Visual>(part));
	REQUIRE(store.Has<Hierarchy>(part));
	REQUIRE(store.Has<InstanceClass>(part));
	REQUIRE_FALSE(store.Has<Motion>(part));
}

TEST_CASE("a new instance starts from the prototype row", "[ecs]") {
	// The defaults are values copied from a hidden row, not a constructor. That
	// is what lets a snapshot carry them and the manifest describe them.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity part = store.CreateInstance(tree.Part);

	REQUIRE(store.Get<Bounds>(part)->HalfExtent == 2.5f);			  // from BasePart
	REQUIRE(store.Get<Visual>(part)->Mesh == Name("test.mesh.cube")); // from Part
	REQUIRE(store.Get<Visual>(part)->Visible);

	// And a component with no declared default is value-initialised, not
	// whatever the allocation held.
	REQUIRE(store.Get<Transform>(part)->X == 0.0f);
}

TEST_CASE("a base class default reaches a derived class", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	// Bounds was defaulted on BasePart; Part inherits both the component and
	// the value.
	REQUIRE(store.Get<Bounds>(store.CreateInstance(tree.BasePart))->HalfExtent == 2.5f);
	REQUIRE(store.Get<Bounds>(store.CreateInstance(tree.Part))->HalfExtent == 2.5f);
}

TEST_CASE("creating one instance builds one table, not a chain of them", "[ecs]") {
	// Adding the components one at a time would walk the entity through every
	// intermediate archetype and create each of them, which is a table per
	// prefix of the class's component list.
	const Tree &tree = Classes_();
	Store store("test");

	store.CreateInstance(tree.Part);
	REQUIRE(store.TableCount() == 1);

	store.CreateInstance(tree.Part);
	REQUIRE(store.TableCount() == 1);
}

TEST_CASE("an invalid class creates nothing", "[ecs]") {
	Store store("test");
	REQUIRE(store.CreateInstance(ClassId{}) == NULL_ENTITY);
}

TEST_CASE("a query for a base component matches every derived instance", "[ecs]") {
	// The payoff of inheritance being set inclusion: a system over Transform
	// sees parts and models without knowing either exists.
	const Tree &tree = Classes_();
	Store store("test");

	store.CreateInstance(tree.Part);
	store.CreateInstance(tree.Part);
	store.CreateInstance(tree.Model);

	REQUIRE(store.CountMatching<Transform>() == 3);
	REQUIRE(store.CountMatching<Visual>() == 2);
	REQUIRE(store.CountMatching<Motion>() == 1);
}

TEST_CASE("IsA works through an instance", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity part = store.CreateInstance(tree.Part);
	REQUIRE(store.IsA(part, tree.BasePart));
	REQUIRE(store.IsA(part, tree.Instance));
	REQUIRE_FALSE(store.IsA(part, tree.Model));

	// A plain entity is not an instance of anything.
	REQUIRE_FALSE(store.IsA(store.Create(), tree.Instance));
	REQUIRE_FALSE(store.ClassOf(store.Create()).IsValid());
}

// --- the hierarchy --------------------------------------------------------

TEST_CASE("children come back in insertion order", "[ecs]") {
	// Not an incidental property: replication and replay both compare child
	// lists, and an order that varied between runs would make them disagree
	// about something neither changed.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model, "Model");
	for (int index = 0; index < 8; index++) {
		const Entity part = store.CreateInstance(tree.Part, "Part" + std::to_string(index));
		REQUIRE(store.SetParent(part, model));
	}

	const std::vector<Name> names = ChildNames(store, model);
	REQUIRE(names.size() == 8);
	for (int index = 0; index < 8; index++) {
		REQUIRE(names[static_cast<size_t>(index)] == Name("Part" + std::to_string(index)));
	}
}

TEST_CASE("reparenting unlinks from the old parent", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity first = store.CreateInstance(tree.Model, "First");
	const Entity second = store.CreateInstance(tree.Model, "Second");
	const Entity part = store.CreateInstance(tree.Part, "Part");

	store.SetParent(part, first);
	REQUIRE(ChildNames(store, first).size() == 1);

	store.SetParent(part, second);
	REQUIRE(ChildNames(store, first).empty());
	REQUIRE(ChildNames(store, second).size() == 1);
	REQUIRE(store.ParentOf(part) == second);
}

TEST_CASE("unparenting to nothing leaves a root", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model);
	const Entity part = store.CreateInstance(tree.Part);

	store.SetParent(part, model);
	store.SetParent(part, NULL_ENTITY);

	REQUIRE(store.ParentOf(part) == NULL_ENTITY);
	REQUIRE(ChildNames(store, model).empty());
	REQUIRE(store.Alive(part));
}

TEST_CASE("removing the first, middle and last child all relink", "[ecs]") {
	// Three different paths through the doubly-linked sibling list, and the
	// one most likely to leave a dangling link.
	const Tree &tree = Classes_();

	for (int target = 0; target < 3; target++) {
		Store store("test");
		const Entity model = store.CreateInstance(tree.Model);

		std::vector<Entity> parts;
		for (int index = 0; index < 3; index++) {
			const Entity part = store.CreateInstance(tree.Part, "P" + std::to_string(index));
			store.SetParent(part, model);
			parts.push_back(part);
		}

		store.SetParent(parts[static_cast<size_t>(target)], NULL_ENTITY);

		const std::vector<Name> names = ChildNames(store, model);
		REQUIRE(names.size() == 2);
		for (int index = 0; index < 3; index++) {
			if (index == target) {
				continue;
			}
			const Name expected("P" + std::to_string(index));
			REQUIRE(std::find(names.begin(), names.end(), expected) != names.end());
		}
	}
}

TEST_CASE("a cycle is refused rather than hung on", "[ecs]") {
	// A cycle is not a wrong answer; it is an infinite loop in every walk of
	// the tree, including the one that would destroy it.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity grandparent = store.CreateInstance(tree.Model);
	const Entity parent = store.CreateInstance(tree.Model);
	const Entity child = store.CreateInstance(tree.Model);

	store.SetParent(parent, grandparent);
	store.SetParent(child, parent);

	REQUIRE_FALSE(store.SetParent(grandparent, child));
	REQUIRE_FALSE(store.SetParent(grandparent, grandparent));
	REQUIRE(store.ParentOf(grandparent) == NULL_ENTITY);
}

TEST_CASE("descendancy walks upwards", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity root = store.CreateInstance(tree.Model);
	const Entity middle = store.CreateInstance(tree.Model);
	const Entity leaf = store.CreateInstance(tree.Part);

	store.SetParent(middle, root);
	store.SetParent(leaf, middle);

	REQUIRE(store.IsDescendantOf(leaf, root));
	REQUIRE(store.IsDescendantOf(leaf, leaf));
	REQUIRE_FALSE(store.IsDescendantOf(root, leaf));
}

TEST_CASE("FindFirstChild takes the first match in insertion order", "[ecs]") {
	// Siblings may share a name, exactly as they may in Roblox.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model);
	const Entity first = store.CreateInstance(tree.Part, "Same");
	const Entity second = store.CreateInstance(tree.Part, "Same");
	store.SetParent(first, model);
	store.SetParent(second, model);

	REQUIRE(store.FindFirstChild(model, "Same") == first);
	REQUIRE(store.FindFirstChild(model, "Missing") == NULL_ENTITY);
}

TEST_CASE("EachChild survives the body reparenting what it was handed", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity from = store.CreateInstance(tree.Model);
	const Entity to = store.CreateInstance(tree.Model);

	for (int index = 0; index < 6; index++) {
		store.SetParent(store.CreateInstance(tree.Part, "P" + std::to_string(index)), from);
	}

	size_t moved = 0;
	store.EachChild(from, [&](Entity child) {
		store.SetParent(child, to);
		moved++;
	});

	REQUIRE(moved == 6);
	REQUIRE(ChildNames(store, from).empty());
	REQUIRE(ChildNames(store, to).size() == 6);
}

// --- destroying -----------------------------------------------------------

TEST_CASE("destroying an instance takes its whole subtree", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity root = store.CreateInstance(tree.Model);
	std::vector<Entity> all{root};

	for (int index = 0; index < 4; index++) {
		const Entity branch = store.CreateInstance(tree.Model);
		store.SetParent(branch, root);
		all.push_back(branch);

		for (int leaf = 0; leaf < 3; leaf++) {
			const Entity part = store.CreateInstance(tree.Part);
			store.SetParent(part, branch);
			all.push_back(part);
		}
	}

	const Entity survivor = store.CreateInstance(tree.Part);

	store.DestroyInstance(root);

	for (const Entity entity : all) {
		REQUIRE_FALSE(store.Alive(entity));
	}
	REQUIRE(store.Alive(survivor));
}

TEST_CASE("destroying a child leaves the parent's list intact", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model);
	std::vector<Entity> parts;
	for (int index = 0; index < 5; index++) {
		const Entity part = store.CreateInstance(tree.Part, "P" + std::to_string(index));
		store.SetParent(part, model);
		parts.push_back(part);
	}

	store.DestroyInstance(parts[2]);

	const std::vector<Name> names = ChildNames(store, model);
	REQUIRE(names.size() == 4);
	REQUIRE(std::find(names.begin(), names.end(), Name("P2")) == names.end());
}

TEST_CASE("a handle to a destroyed instance is a tombstone", "[ecs]") {
	// Roblox keeps a destroyed instance readable while a script holds it. Here
	// the row is freed and the generation check makes the stale handle safe.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity part = store.CreateInstance(tree.Part);
	store.DestroyInstance(part);

	REQUIRE_FALSE(store.Alive(part));
	REQUIRE(store.Get<Transform>(part) == nullptr);
	REQUIRE_FALSE(store.ClassOf(part).IsValid());
	REQUIRE(store.ParentOf(part) == NULL_ENTITY);
}

// --- cloning --------------------------------------------------------------

TEST_CASE("a clone copies values and belongs to no tree", "[ecs]") {
	// :Clone() leaves the copy parented nowhere. A clone that appeared in the
	// world at the moment it was made would run before the caller had finished
	// configuring it.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model, "Model");
	const Entity part = store.CreateInstance(tree.Part, "Original");
	store.SetParent(part, model);
	store.GetMutable<Transform>(part)->X = 7.0f;

	const Entity copy = store.CloneInstance(part);

	REQUIRE(store.Alive(copy));
	REQUIRE(copy != part);
	REQUIRE(store.ClassOf(copy) == tree.Part);
	REQUIRE(store.InstanceNameOf(copy) == Name("Original"));
	REQUIRE(store.Get<Transform>(copy)->X == 7.0f);

	REQUIRE(store.ParentOf(copy) == NULL_ENTITY);
	REQUIRE(ChildNames(store, model).size() == 1);
}

TEST_CASE("a clone is independent of its source", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity part = store.CreateInstance(tree.Part);
	store.GetMutable<Transform>(part)->X = 1.0f;

	const Entity copy = store.CloneInstance(part);
	store.GetMutable<Transform>(copy)->X = 2.0f;

	REQUIRE(store.Get<Transform>(part)->X == 1.0f);
	REQUIRE(store.Get<Transform>(copy)->X == 2.0f);
}

TEST_CASE("cloning takes the whole subtree", "[ecs]") {
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model, "Model");
	for (int index = 0; index < 3; index++) {
		const Entity part = store.CreateInstance(tree.Part, "P" + std::to_string(index));
		store.SetParent(part, model);

		const Entity nested = store.CreateInstance(tree.Part, "Nested");
		store.SetParent(nested, part);
	}

	const Entity copy = store.CloneInstance(model);

	const std::vector<Name> names = ChildNames(store, copy);
	REQUIRE(names.size() == 3);
	REQUIRE(names[0] == Name("P0"));
	REQUIRE(names[2] == Name("P2"));

	// And one level deeper, so the recursion is not one-deep.
	const Entity firstCopy = store.FindFirstChild(copy, "P0");
	REQUIRE(ChildNames(store, firstCopy).size() == 1);
	REQUIRE(store.FindFirstChild(firstCopy, "Nested") != NULL_ENTITY);

	// The original is untouched.
	REQUIRE(ChildNames(store, model).size() == 3);
}

TEST_CASE("cloning something that is not an instance yields nothing", "[ecs]") {
	Store store("test");
	REQUIRE(store.CloneInstance(store.Create()) == NULL_ENTITY);
	REQUIRE(store.CloneInstance(NULL_ENTITY) == NULL_ENTITY);
}

// --- snapshots ------------------------------------------------------------

TEST_CASE("a tree survives a snapshot", "[ecs]") {
	// The hierarchy is four entity handles in a component, so this is the case
	// that fails if a snapshot re-allocates the directory rather than
	// reproducing it.
	const Tree &tree = Classes_();
	Store source("source");

	const Entity model = source.CreateInstance(tree.Model, "Model");
	std::vector<Entity> parts;
	for (int index = 0; index < 5; index++) {
		const Entity part = source.CreateInstance(tree.Part, "P" + std::to_string(index));
		source.SetParent(part, model);
		source.GetMutable<Transform>(part)->X = static_cast<float>(index);
		parts.push_back(part);
	}

	ByteWriter writer;
	REQUIRE(source.Save(writer));

	Store restored("restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	REQUIRE(restored.ClassOf(model) == tree.Model);
	REQUIRE(restored.InstanceNameOf(model) == Name("Model"));

	const std::vector<Name> names = ChildNames(restored, model);
	REQUIRE(names.size() == 5);
	for (int index = 0; index < 5; index++) {
		REQUIRE(names[static_cast<size_t>(index)] == Name("P" + std::to_string(index)));
		REQUIRE(restored.ParentOf(parts[static_cast<size_t>(index)]) == model);
		REQUIRE(restored.Get<Transform>(parts[static_cast<size_t>(index)])->X == static_cast<float>(index));
	}
}

// --- churn ----------------------------------------------------------------

TEST_CASE("the tree stays consistent under random reparenting", "[ecs][fuzz]") {
	// The invariants that must hold whatever the operations were: every child
	// names its parent, every parent lists its children, and no walk loops.
	const Tree &tree = Classes_();
	Store store("churn");

	std::vector<Entity> nodes;
	for (int index = 0; index < 40; index++) {
		nodes.push_back(store.CreateInstance(tree.Model, "N" + std::to_string(index)));
	}

	for (uint32_t step = 0; step < 4'000; step++) {
		const Entity child = nodes[Random::Bits(step, 501) % nodes.size()];
		const bool detach = Random::Bits(step, 502) % 4 == 0;
		const Entity parent = detach ? NULL_ENTITY : nodes[Random::Bits(step, 503) % nodes.size()];

		store.SetParent(child, parent);
	}

	size_t broken = 0;
	for (const Entity node : nodes) {
		// Every child of this node names it as their parent.
		store.EachChild(node, [&](Entity child) {
			if (store.ParentOf(child) != node) {
				broken++;
			}
		});

		// And walking upwards terminates rather than looping.
		size_t depth = 0;
		for (Entity walk = node; walk != NULL_ENTITY; walk = store.ParentOf(walk)) {
			if (++depth > nodes.size() + 1) {
				broken++;
				break;
			}
		}

		// A node with a parent appears in that parent's child list exactly once.
		const Entity parent = store.ParentOf(node);
		if (parent != NULL_ENTITY) {
			size_t appearances = 0;
			store.EachChild(parent, [&](Entity child) {
				if (child == node) {
					appearances++;
				}
			});
			if (appearances != 1) {
				broken++;
			}
		}
	}

	REQUIRE(broken == 0);
}

TEST_CASE("destroying random subtrees leaves no orphans", "[ecs][fuzz]") {
	const Tree &tree = Classes_();
	Store store("churn");

	std::vector<Entity> nodes;
	for (int index = 0; index < 60; index++) {
		const Entity node = store.CreateInstance(tree.Model);
		if (index > 0) {
			store.SetParent(node, nodes[Random::Bits(static_cast<uint32_t>(index), 601) % nodes.size()]);
		}
		nodes.push_back(node);
	}

	for (uint32_t step = 0; step < 30; step++) {
		const Entity victim = nodes[Random::Bits(step, 602) % nodes.size()];
		if (store.Alive(victim)) {
			store.DestroyInstance(victim);
		}
	}

	// Nothing alive may name a dead parent, and no dead node may still appear
	// in a live parent's list.
	size_t orphans = 0;
	for (const Entity node : nodes) {
		if (!store.Alive(node)) {
			continue;
		}
		const Entity parent = store.ParentOf(node);
		if (parent != NULL_ENTITY && !store.Alive(parent)) {
			orphans++;
		}
		store.EachChild(node, [&](Entity child) {
			if (!store.Alive(child)) {
				orphans++;
			}
		});
	}

	REQUIRE(orphans == 0);
}
