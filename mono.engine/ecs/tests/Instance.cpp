#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

			Classes::Property<&Transform::X>(built.PVInstance, "X");
			Classes::Property<&Transform::Y>(built.PVInstance, "Y");
			Classes::Property<&Bounds::HalfExtent>(built.BasePart, "HalfExtent");
			Classes::Property<&Visual::Visible>(built.Part, "Visible");
			Classes::Property<&Visual::Mesh>(built.Part, "Mesh");

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

TEST_CASE("a field property generates a conversion that reads and writes it", "[ecs]") {
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

TEST_CASE("a tree churned by raw destroys keeps every live instance reachable", "[ecs][fuzz]") {
	// **The invariant the other two fuzz tests cannot state.** They ask whether
	// anything alive names something dead, which the truncation bug passes:
	// after a middle child was freed without unlinking, every *surviving* row
	// still named a live parent. What was wrong was the other direction — the
	// parent's list stopped at the hole, so the rows behind it were alive, held
	// a correct parent, and were reachable from no root at all. They would have
	// been written into the save file and drawn by nothing.
	//
	// So this walks down from the roots and requires the set it reaches to be
	// exactly the set that is alive. And it churns through `Store::Destroy` —
	// the raw one, which `DestroyInstance` and the two tests above never use.
	const Tree &tree = Classes_();
	Store store("raw-churn");

	std::vector<Entity> nodes;
	for (int index = 0; index < 80; index++) {
		const Entity node = store.CreateInstance(tree.Model, "N" + std::to_string(index));
		if (index > 0) {
			store.SetParent(node, nodes[Random::Bits(static_cast<uint32_t>(index), 701) % nodes.size()]);
		}
		nodes.push_back(node);
	}

	for (uint32_t step = 0; step < 400; step++) {
		const Entity target = nodes[Random::Bits(step, 702) % nodes.size()];
		if (!store.Alive(target)) {
			continue;
		}

		switch (Random::Bits(step, 703) % 5) {
		case 0:
			store.Destroy(target);
			break;
		case 1:
			store.DestroyInstance(target);
			break;
		case 2:
			store.SetParent(target, NULL_ENTITY);
			break;
		default: {
			const Entity parent = nodes[Random::Bits(step, 704) % nodes.size()];
			if (store.Alive(parent)) {
				store.SetParent(target, parent);
			}
			break;
		}
		}
	}

	const auto byId = [](Entity left, Entity right) { return left.Id < right.Id; };

	std::vector<Entity> live;
	for (const Entity node : nodes) {
		if (store.Alive(node)) {
			live.push_back(node);
		}
	}
	std::sort(live.begin(), live.end(), byId);

	// Down from every root, which is the direction a save file, the renderer
	// and the explorer all read the world in.
	std::vector<Entity> reached;
	std::vector<Entity> frontier;
	store.EachRoot([&](Entity root) { frontier.push_back(root); });

	while (!frontier.empty()) {
		const Entity at = frontier.back();
		frontier.pop_back();
		reached.push_back(at);

		// Bounded, so a list that somehow looped is a failure rather than a
		// test that never returns.
		REQUIRE(reached.size() <= nodes.size());
		store.EachChild(at, [&](Entity child) { frontier.push_back(child); });
	}
	std::sort(reached.begin(), reached.end(), byId);

	REQUIRE(reached == live);
}

// --- the tree's edges ------------------------------------------------------

TEST_CASE("reparenting to the parent it already has changes nothing", "[ecs][instance]") {
	// Roblox's `thing.Parent = thing.Parent` is a no-op, and this used to be an
	// unlink followed by an append — so a write that changed nothing moved the
	// instance to the back of its own siblings, and `GetChildren()` came back
	// in a different order on the machine that happened to run that line.
	//
	// The shape that finds it is ordinary: a script assigning a parent that may
	// or may not have changed, or an editor applying a drag onto the row the
	// instance was already under.
	const Tree &tree = Classes_();
	Store store("test");

	const Entity model = store.CreateInstance(tree.Model, "Model");
	std::vector<Entity> made;
	for (int index = 0; index < 4; index++) {
		const Entity part = store.CreateInstance(tree.Part, "Part" + std::to_string(index));
		REQUIRE(store.SetParent(part, model));
		made.push_back(part);
	}

	const std::vector<Name> before = ChildNames(store, model);
	REQUIRE(before.size() == 4);

	// The first child, which is the one an append would move furthest.
	REQUIRE(store.SetParent(made.front(), model));
	CHECK(ChildNames(store, model) == before);
	CHECK(store.ParentOf(made.front()) == model);

	// And a root told that it is a root.
	const Entity loose = store.CreateInstance(tree.Part, "Loose");
	REQUIRE(store.SetParent(loose, NULL_ENTITY));
	CHECK(store.ParentOf(loose) == NULL_ENTITY);
	CHECK(ChildNames(store, model) == before);
}

TEST_CASE("a raw destroy takes the row out of the tree first", "[ecs][instance]") {
	// **`Destroy`, not `DestroyInstance`.** Freeing a row does not touch the
	// links that point at it, and `EachChild` stops at the first dead link
	// rather than stepping over it — because the links *out of* a freed row
	// went with the row, so there is no way to reach what followed it.
	//
	// Destroying the middle of three children therefore truncated the list to
	// one and lost the other two: still alive, still in the save file, and
	// reachable from nothing. The unlink now happens before the free.
	const Tree &tree = Classes_();
	Store store("raw-destroy");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity first = store.CreateInstance(tree.Part, "A");
	const Entity middle = store.CreateInstance(tree.Part, "B");
	const Entity last = store.CreateInstance(tree.Part, "C");
	store.SetParent(first, parent);
	store.SetParent(middle, parent);
	store.SetParent(last, parent);

	SECTION("the middle child") {
		store.Destroy(middle);

		std::vector<Entity> children;
		store.EachChild(parent, [&](Entity child) { children.push_back(child); });
		REQUIRE(children == std::vector<Entity>{first, last});
	}

	SECTION("the first child") {
		store.Destroy(first);

		std::vector<Entity> children;
		store.EachChild(parent, [&](Entity child) { children.push_back(child); });
		REQUIRE(children == std::vector<Entity>{middle, last});
	}

	SECTION("the last child") {
		store.Destroy(last);

		std::vector<Entity> children;
		store.EachChild(parent, [&](Entity child) { children.push_back(child); });
		REQUIRE(children == std::vector<Entity>{first, middle});

		// The tail is what an append writes through, so it has to be the row
		// that survived rather than the one that went.
		const Entity added = store.CreateInstance(tree.Part, "D");
		REQUIRE(store.SetParent(added, parent));

		children.clear();
		store.EachChild(parent, [&](Entity child) { children.push_back(child); });
		REQUIRE(children == std::vector<Entity>{first, middle, added});
	}
}

TEST_CASE("a raw destroy inside a loop unlinks now and frees afterwards", "[ecs][instance]") {
	// **The two halves of a destroy do not defer together, and they must not.**
	// `Store::Destroy` inside `Each` queues the free until the loop ends, so
	// that removing a row cannot move the rows the loop is still walking. The
	// unlink cannot wait with it: once the row is vacated its own links are
	// gone, and there is no "afterwards" left to unlink it by.
	//
	// So the tree is right immediately and the directory catches up at the end
	// of the loop. Both halves are asserted here because a change to either one
	// on its own is a bug the other one hides.
	const Tree &tree = Classes_();
	Store store("destroy-in-loop");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity first = store.CreateInstance(tree.Part, "A");
	const Entity middle = store.CreateInstance(tree.Part, "B");
	const Entity last = store.CreateInstance(tree.Part, "C");
	store.SetParent(first, parent);
	store.SetParent(middle, parent);
	store.SetParent(last, parent);

	bool aliveInside = true;
	store.Each<const Hierarchy>([&](Entity entity, const Hierarchy &) {
		if (entity == middle) {
			store.Destroy(middle);
			aliveInside = store.Alive(middle);
		}
	});

	// Deferred, so it outlived the loop it was destroyed in.
	CHECK(aliveInside);
	CHECK_FALSE(store.Alive(middle));

	// And unlinked, so the two either side of it found each other.
	std::vector<Entity> children;
	store.EachChild(parent, [&](Entity child) { children.push_back(child); });
	CHECK(children == std::vector<Entity>{first, last});
}

TEST_CASE("a raw destroy re-roots the children it leaves behind", "[ecs][instance]") {
	// A raw destroy asks for one row to go, so the subtree stays — but a child
	// still pointing at a freed parent is worse than either outcome. It is not
	// a root, because `EachRoot` asks for `Parent == NULL_ENTITY` and this one
	// names something merely *dead*; so it is in the world, in the save file,
	// and reachable from nothing.
	const Tree &tree = Classes_();
	Store store("re-root");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity kept = store.CreateInstance(tree.Part, "Kept");
	const Entity nested = store.CreateInstance(tree.Part, "Nested");
	store.SetParent(kept, parent);
	store.SetParent(nested, kept);

	store.Destroy(parent);

	REQUIRE(store.Alive(kept));
	REQUIRE(store.ParentOf(kept) == NULL_ENTITY);

	// One level only. Taking the whole subtree would make this
	// `DestroyInstance`, which is a delete the caller did not ask for.
	REQUIRE(store.Alive(nested));
	REQUIRE(store.ParentOf(nested) == kept);

	size_t found = 0;
	store.EachRoot([&](Entity root) {
		if (root == kept) {
			found++;
		}
	});
	REQUIRE(found == 1);
}

TEST_CASE("EachChild never hands over a link that resolves to nothing", "[ecs][instance]") {
	// The walk has always stopped at a link it cannot resolve. What it used to
	// do first was call the body with it — so the one value a caller cannot
	// survive, a handle that is not a child, was the one value it was given.
	// `script`'s `GetChildren()` pushed it straight to Luau.
	//
	// Built by hand, because `Store::Destroy` no longer leaves this state: the
	// parent is pointed at a live entity that carries no `Hierarchy` at all,
	// which is what a link out of the tree looks like from in here.
	const Tree &tree = Classes_();
	Store store("dangling-link");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity real = store.CreateInstance(tree.Part, "Real");
	store.SetParent(real, parent);

	const Entity stranger = store.Create("");
	REQUIRE(store.Alive(stranger));
	REQUIRE(store.Get<Hierarchy>(stranger) == nullptr);

	Hierarchy links = *store.Get<Hierarchy>(parent);
	links.FirstChild = stranger;
	store.Set<Hierarchy>(parent, links);

	std::vector<Entity> seen;
	store.EachChild(parent, [&](Entity child) { seen.push_back(child); });
	CHECK(seen.empty());
}

// --- roots -----------------------------------------------------------------

TEST_CASE("roots are every instance with no parent", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("roots");

	const Entity first = store.CreateInstance(tree.Model, "First");
	const Entity second = store.CreateInstance(tree.Model, "Second");
	const Entity child = store.CreateInstance(tree.Part, "Child");

	std::vector<Entity> roots;
	const auto collect = [&] {
		roots.clear();
		store.EachRoot([&](Entity root) { roots.push_back(root); });
	};

	collect();
	CHECK(roots == std::vector<Entity>{first, second, child});

	store.SetParent(child, first);
	collect();
	CHECK(roots == std::vector<Entity>{first, second});

	// Back out again, and it is a root once more.
	store.SetParent(child, NULL_ENTITY);
	collect();
	CHECK(roots == std::vector<Entity>{first, second, child});

	// An entity that is not an instance carries no `Hierarchy`, so it is not a
	// root of the tree — it is not in the tree at all.
	store.Create("plain");
	collect();
	CHECK(roots == std::vector<Entity>{first, second, child});
}

TEST_CASE("roots come back in creation order, not insertion order", "[ecs][instance]") {
	// **The contract `Store::EachRoot` documents, and the one thing about it a
	// reader is most likely to assume wrongly.** A child list is threaded in
	// insertion order, so reparenting moves an instance to the end of its new
	// siblings; a root that was detached and reattached keeps its original
	// place here. Deterministic either way, which is what a recording needs —
	// an archetype walk would not have been, because a row moves when its
	// archetype does.
	const Tree &tree = Classes_();
	Store store("root-order");

	const Entity first = store.CreateInstance(tree.Model, "First");
	const Entity second = store.CreateInstance(tree.Model, "Second");
	const Entity third = store.CreateInstance(tree.Model, "Third");

	// Out of the tree and back in, which is what would move it to the end if
	// roots were ordered by anything the tree did rather than by their ids.
	store.SetParent(first, second);
	store.SetParent(first, NULL_ENTITY);

	std::vector<Entity> roots;
	store.EachRoot([&](Entity root) { roots.push_back(root); });
	CHECK(roots == std::vector<Entity>{first, second, third});

	// Adding a component moves the row to another archetype. The order must not
	// notice: that is the difference between this and a walk over the tables.
	store.Set<Motion>(third, Motion{1.0f});
	store.Set<Motion>(first, Motion{2.0f});

	roots.clear();
	store.EachRoot([&](Entity root) { roots.push_back(root); });
	CHECK(roots == std::vector<Entity>{first, second, third});
}

TEST_CASE("FindFirstRoot takes the first root with the name", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("find-root");

	const Entity first = store.CreateInstance(tree.Model, "Same");
	const Entity second = store.CreateInstance(tree.Model, "Same");
	const Entity buried = store.CreateInstance(tree.Part, "Buried");
	store.SetParent(buried, first);

	CHECK(store.FindFirstRoot("Same") == first);
	CHECK(second != NULL_ENTITY);

	// Not a root, so not found — this searches the world's own children and
	// not its whole tree.
	CHECK(store.FindFirstRoot("Buried") == NULL_ENTITY);
	CHECK(store.FindFirstRoot("Nothing") == NULL_ENTITY);
	CHECK(store.FindFirstRoot("") == NULL_ENTITY);
}

// --- the O(1) questions ----------------------------------------------------

TEST_CASE("HasChildren answers what EachChild would find", "[ecs][instance]") {
	// The probe a tree view asks per row. It is `FirstChild != NULL_ENTITY` and
	// never a walk, so what it has to be tested for is that it stays in step
	// with the list through every edit that empties one.
	const Tree &tree = Classes_();
	Store store("has-children");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity only = store.CreateInstance(tree.Part, "Only");

	CHECK_FALSE(store.HasChildren(parent));

	store.SetParent(only, parent);
	CHECK(store.HasChildren(parent));
	CHECK_FALSE(store.HasChildren(only));

	SECTION("emptied by reparenting away") {
		store.SetParent(only, NULL_ENTITY);
		CHECK_FALSE(store.HasChildren(parent));
	}

	SECTION("emptied by destroying the child") {
		store.DestroyInstance(only);
		CHECK_FALSE(store.HasChildren(parent));
	}

	SECTION("emptied by a raw destroy") {
		store.Destroy(only);
		CHECK_FALSE(store.HasChildren(parent));
	}

	SECTION("an entity that is not an instance has no children") {
		CHECK_FALSE(store.HasChildren(store.Create("plain")));
		CHECK_FALSE(store.HasChildren(NULL_ENTITY));
	}
}

TEST_CASE("descendancy includes the instance itself and stops at a root", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("descendancy");

	const Entity top = store.CreateInstance(tree.Model, "Top");
	const Entity middle = store.CreateInstance(tree.Model, "Middle");
	const Entity leaf = store.CreateInstance(tree.Part, "Leaf");
	const Entity elsewhere = store.CreateInstance(tree.Part, "Elsewhere");
	store.SetParent(middle, top);
	store.SetParent(leaf, middle);

	// Reflexive, which is what makes it the test `SetParent` uses to refuse a
	// cycle: an instance is inside its own subtree.
	CHECK(store.IsDescendantOf(top, top));

	CHECK(store.IsDescendantOf(leaf, middle));
	CHECK(store.IsDescendantOf(leaf, top));
	CHECK_FALSE(store.IsDescendantOf(top, leaf));
	CHECK_FALSE(store.IsDescendantOf(elsewhere, top));

	// **Not everything is a descendant of nothing.** The walk ends when it runs
	// out of parents, and the null handle is where it ends rather than
	// something it matches — so a caller asking "is this under NULL_ENTITY"
	// gets `false` and not "yes, everything is".
	CHECK_FALSE(store.IsDescendantOf(leaf, NULL_ENTITY));
	CHECK_FALSE(store.IsDescendantOf(NULL_ENTITY, top));
}

TEST_CASE("FindFirstChild searches one level and reports nothing honestly", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("find-child");

	const Entity parent = store.CreateInstance(tree.Model, "Parent");
	const Entity named = store.CreateInstance(tree.Part, "Wanted");
	const Entity unnamed = store.CreateInstance(tree.Part);
	const Entity deep = store.CreateInstance(tree.Part, "Deep");
	store.SetParent(unnamed, parent);
	store.SetParent(named, parent);
	store.SetParent(deep, named);

	CHECK(store.FindFirstChild(parent, "Wanted") == named);

	// One level, which is what makes it `FindFirstChild` rather than a search.
	CHECK(store.FindFirstChild(parent, "Deep") == NULL_ENTITY);

	CHECK(store.FindFirstChild(parent, "Absent") == NULL_ENTITY);
	CHECK(store.FindFirstChild(store.Create("plain"), "Wanted") == NULL_ENTITY);
	CHECK(store.FindFirstChild(NULL_ENTITY, "Wanted") == NULL_ENTITY);

	// **The empty name finds an unnamed instance, and this asserts it because
	// it is surprising rather than because it is obviously right.**
	// `CreateInstance` with an empty name leaves the instance with no
	// `core::Name` at all, `InstanceNameOf` answers with an invalid one, and an
	// invalid name is what `Name("")` compares equal to — so searching for ""
	// is asking for "whatever has no name", and it answers.
	//
	// Self-consistent, and not Roblox: there `FindFirstChild("")` finds only an
	// instance somebody named "". Worth a decision rather than a silent change,
	// so it is written down here as what the engine does today.
	CHECK(store.FindFirstChild(parent, "") == unnamed);
}

TEST_CASE("SetParent refuses ends that are not instances", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("bad-ends");

	const Entity instance = store.CreateInstance(tree.Part, "Part");
	const Entity plain = store.Create("plain");

	CHECK_FALSE(store.SetParent(plain, instance));
	CHECK_FALSE(store.SetParent(instance, plain));
	CHECK_FALSE(store.SetParent(NULL_ENTITY, instance));

	// Detaching something already detached is the one pairing that succeeds,
	// because it is asking for the state it is already in.
	CHECK(store.SetParent(instance, NULL_ENTITY));
	CHECK_FALSE(store.SetParent(plain, NULL_ENTITY));
}

TEST_CASE("parenting survives a child freed without unlinking", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("dangling");

	const Entity parent = store.CreateInstance(tree.Model);
	const Entity first = store.CreateInstance(tree.Part);
	const Entity doomed = store.CreateInstance(tree.Part);
	store.SetParent(first, parent);
	store.SetParent(doomed, parent);

	// **`Destroy`, not `DestroyInstance`, and that is the whole test.** The
	// raw destroy frees the row and leaves the parent's `LastChild` naming it,
	// which is the state the studio reached by releasing a viewport camera on
	// a world switch. The next parent into the same node then wrote through a
	// handle to a row that was gone.
	store.Destroy(doomed);

	const Entity added = store.CreateInstance(tree.Part);
	REQUIRE(store.SetParent(added, parent));

	std::vector<Entity> children;
	store.EachChild(parent, [&](Entity child) { children.push_back(child); });

	REQUIRE(children == std::vector<Entity>{first, added});
	REQUIRE(store.ParentOf(added) == parent);
}

TEST_CASE("parenting survives an only child freed without unlinking", "[ecs][instance]") {
	const Tree &tree = Classes_();
	Store store("dangling-only");

	const Entity parent = store.CreateInstance(tree.Model);
	const Entity doomed = store.CreateInstance(tree.Part);
	store.SetParent(doomed, parent);

	// The list is broken at the front as well as the back here, so there is no
	// surviving row to hang the repair off.
	store.Destroy(doomed);

	const Entity added = store.CreateInstance(tree.Part);
	REQUIRE(store.SetParent(added, parent));

	std::vector<Entity> children;
	store.EachChild(parent, [&](Entity child) { children.push_back(child); });

	REQUIRE(children == std::vector<Entity>{added});
	REQUIRE(store.ParentOf(added) == parent);
}
