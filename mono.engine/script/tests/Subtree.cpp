// The subtree walk both VMs destroy and enumerate through.
//
// Tested here rather than through a VM because it is pure C++ over a `Store` -
// the same reason `Signals.cpp` tests the connection table directly. What a
// script sees on top of this is covered in `Scripting.cpp`, once per language.

#include "../src/Subtree.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.subtree")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::script::CallbackRef;
using engine::script::ChangeQueue;
using engine::script::EachDescendant;
using engine::script::ForgetSubtree;
using engine::script::SignalKind;
using engine::script::SignalTable;

namespace {
	// One instance, parented, with a name to read back in an assertion.
	Entity Make(Store &store, std::string_view name, Entity parent) {
		const Entity instance = store.CreateInstance(engine::scene::PartClass(), name);
		REQUIRE(instance != engine::ecs::NULL_ENTITY);
		if (parent != engine::ecs::NULL_ENTITY) {
			REQUIRE(store.SetParent(instance, parent));
		}
		return instance;
	}

	std::vector<std::string> NamesUnder(const Store &store, Entity instance) {
		std::vector<std::string> names;
		EachDescendant(store, instance, [&](Entity descendant) {
			names.emplace_back(store.InstanceNameOf(descendant).Text());
		});
		return names;
	}
}

TEST_CASE("EachDescendant walks depth first, in the recursive walk's order", "[script][subtree]") {
	engine::scene::EnsureClassTree();
	Store store("subtree_test");

	// root
	// ├─ a ── a1 ── a1x
	// ├─ b
	// └─ c ── c1
	const Entity root = Make(store, "root", engine::ecs::NULL_ENTITY);
	const Entity a = Make(store, "a", root);
	const Entity a1 = Make(store, "a1", a);
	Make(store, "a1x", a1);
	Make(store, "b", root);
	const Entity c = Make(store, "c", root);
	Make(store, "c1", c);

	// A child, then everything under that child, then the next child. Not
	// breadth first: `a1` comes before `b`, which is what a script writing the
	// walk by hand would produce and what Roblox returns.
	CHECK(NamesUnder(store, root) == std::vector<std::string>{"a", "a1", "a1x", "b", "c", "c1"});
}

TEST_CASE("EachDescendant visits nothing under a leaf", "[script][subtree]") {
	engine::scene::EnsureClassTree();
	Store store("subtree_test");

	const Entity leaf = Make(store, "leaf", engine::ecs::NULL_ENTITY);

	// The empty case, and the one that says the instance itself is not visited.
	CHECK(NamesUnder(store, leaf).empty());
}

TEST_CASE("ForgetSubtree drops a grandchild's connections, not only a child's", "[script][subtree]") {
	// **The regression this file exists for.** `Store::DestroyInstance` takes
	// the whole subtree, and both bindings used to forget only the direct
	// children - so a connection on a grandchild outlived the row it watched,
	// holding its VM's callable alive for the rest of the world's life.
	engine::scene::EnsureClassTree();
	Store store("subtree_test");

	const Entity model = Make(store, "model", engine::ecs::NULL_ENTITY);
	const Entity child = Make(store, "child", model);
	const Entity grandchild = Make(store, "grandchild", child);

	SignalTable signals;
	ChangeQueue changes;

	signals.Connect(SignalKind::Changed, model, 100);
	signals.Connect(SignalKind::Changed, child, 200);
	signals.Connect(SignalKind::Changed, grandchild, 300);
	changes.Watch(store, grandchild);

	std::vector<CallbackRef> released;
	ForgetSubtree(store, signals, changes, model, [&](CallbackRef reference) {
		released.push_back(reference);
	});

	// Every level, including the one two deep.
	CHECK(signals.Count(SignalKind::Changed, model) == 0);
	CHECK(signals.Count(SignalKind::Changed, child) == 0);
	CHECK(signals.Count(SignalKind::Changed, grandchild) == 0);

	// And the callables came back so the VM can release them. Without this the
	// leak is silent: nothing fires, nothing errors, and the refs are never
	// given up.
	CHECK(released == std::vector<CallbackRef>{100, 200, 300});
}

TEST_CASE("ForgetSubtree leaves a sibling subtree alone", "[script][subtree]") {
	engine::scene::EnsureClassTree();
	Store store("subtree_test");

	const Entity keep = Make(store, "keep", engine::ecs::NULL_ENTITY);
	const Entity kept = Make(store, "kept", keep);
	const Entity drop = Make(store, "drop", engine::ecs::NULL_ENTITY);
	Make(store, "dropped", drop);

	SignalTable signals;
	ChangeQueue changes;

	signals.Connect(SignalKind::Changed, kept, 1);
	const Entity dropped = store.FindFirstChild(drop, "dropped");
	signals.Connect(SignalKind::Changed, dropped, 2);

	std::vector<CallbackRef> released;
	ForgetSubtree(store, signals, changes, drop, [&](CallbackRef reference) {
		released.push_back(reference);
	});

	CHECK(signals.Count(SignalKind::Changed, kept) == 1);
	CHECK(released == std::vector<CallbackRef>{2});
}
