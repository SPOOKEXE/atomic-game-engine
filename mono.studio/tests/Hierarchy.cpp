// What the Explorer decides to draw, without an imgui frame to draw it into.
//
// **The second part of this program a headless test can reach**, and it is the
// half that has an algorithm in it. `Widgets.cpp` says the panels themselves
// need a window and a device; compiling a tree does not, which is the whole
// reason the compile is a class rather than a function inside `DrawExplorer`.

#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <studio/Hierarchy.hpp>
#include <studio/Widgets.hpp>
#include <vector>

TEST_SUITE_ID("studio.hierarchy")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Entity;
using engine::ecs::InstanceName;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using studio::HierarchyRequest;
using studio::HierarchyRow;
using studio::HierarchyView;

namespace {
	// The class every instance below is made as. `Part` rather than `Folder`
	// because the scene module registers it and the view reads no property that
	// distinguishes them.
	ClassId PartClass() {
		engine::scene::RegisterSceneClasses();
		return Classes::Find(Name("Part"));
	}

	// The rows, as names, which is what a failure is readable as.
	std::vector<std::string> Names(const HierarchyView &view) {
		std::vector<std::string> names;
		for (const HierarchyRow &row : view.Rows()) {
			names.emplace_back(row.Text);
		}
		return names;
	}

	// Whether a name is among the rows.
	bool Shows(const HierarchyView &view, std::string_view name) {
		for (const HierarchyRow &row : view.Rows()) {
			if (std::string_view(row.Text) == name) {
				return true;
			}
		}
		return false;
	}

	// The row for a name, or null.
	const HierarchyRow *RowNamed(const HierarchyView &view, std::string_view name) {
		for (const HierarchyRow &row : view.Rows()) {
			if (std::string_view(row.Text) == name) {
				return &row;
			}
		}
		return nullptr;
	}

	// A world shaped like something an author would have open:
	//
	//     Workspace
	//       Model
	//         Handle
	//         Grip
	//       Terrain
	//     Lighting
	struct Scene {
		Store World{"hierarchy_test"};
		Entity Workspace;
		Entity Model;
		Entity Handle;
		Entity Grip;
		Entity Terrain;
		Entity Lighting;

		Scene() {
			const ClassId part = PartClass();

			Workspace = World.CreateInstance(part, "Workspace");
			Model = World.CreateInstance(part, "Model");
			Handle = World.CreateInstance(part, "Handle");
			Grip = World.CreateInstance(part, "Grip");
			Terrain = World.CreateInstance(part, "Terrain");
			Lighting = World.CreateInstance(part, "Lighting");

			World.SetParent(Model, Workspace);
			World.SetParent(Handle, Model);
			World.SetParent(Grip, Model);
			World.SetParent(Terrain, Workspace);
		}
	};

	// A request with nothing open and nothing revealed.
	HierarchyRequest Closed(std::string_view filter = {}) {
		HierarchyRequest request;
		request.Filter = filter;
		return request;
	}
}

TEST_CASE("a closed tree is its roots, in entity-id order", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed()));

	// `Workspace` and `Lighting` are the two with no parent. The order is the
	// one `Store::EachRoot` yields - creation order, not insertion order - and
	// a panel that sorted them its own way would be a second answer to it.
	std::vector<Entity> roots;
	scene.World.EachRoot([&](Entity root) { roots.push_back(root); });

	REQUIRE(view.Rows().size() == roots.size());
	for (size_t index = 0; index < roots.size(); index++) {
		CHECK(view.Rows()[index].Instance == roots[index]);
		CHECK(view.Rows()[index].Depth == 0);
	}

	// Closed, so nothing below a root is a row - but the arrow is drawn.
	CHECK(RowNamed(view, "Workspace")->HasChildren);
	CHECK_FALSE(RowNamed(view, "Workspace")->Open);
	CHECK_FALSE(RowNamed(view, "Lighting")->HasChildren);
	CHECK_FALSE(Shows(view, "Model"));
}

TEST_CASE("an open node's children follow it, in insertion order", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	const Entity open[] = {scene.Workspace, scene.Model};
	HierarchyRequest request = Closed();
	request.Open = open;

	REQUIRE(view.Rebuild(scene.World, request));

	// Depth-first and in the order the sibling list runs, which is the order
	// `GetChildren()` returns. `Handle` before `Grip` because that is the order
	// they were parented in, and `Terrain` after both because a subtree is
	// emitted before the next sibling.
	CHECK(
		Names(view) == std::vector<std::string>{"Workspace", "Model", "Handle", "Grip", "Terrain", "Lighting"}
	);

	CHECK(RowNamed(view, "Workspace")->Depth == 0);
	CHECK(RowNamed(view, "Model")->Depth == 1);
	CHECK(RowNamed(view, "Handle")->Depth == 2);
	CHECK(RowNamed(view, "Terrain")->Depth == 1);

	CHECK(view.RowOf(scene.Grip) == 3);
	CHECK(view.Holds(scene.Grip));
	CHECK(view.Count() == 6);
}

TEST_CASE("a filter matches at any depth and brings its parents with it", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	// Nothing is open, and the match is two levels down. Studio's search finds
	// it anyway and unfolds the path to it - a match with no parents above it
	// is a row with no way to tell where it lives.
	REQUIRE(view.Rebuild(scene.World, Closed("Handle")));

	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Model", "Handle"});
	CHECK(view.MatchCount() == 1);
	CHECK(view.Filtering());

	// The ancestors are opened whatever the author last clicked, and marked as
	// what they are: present because something under them matched.
	CHECK(RowNamed(view, "Workspace")->Open);
	CHECK(RowNamed(view, "Model")->Open);
	CHECK_FALSE(RowNamed(view, "Workspace")->Matched);
	CHECK(RowNamed(view, "Handle")->Matched);

	// The siblings that did not match are gone, at both levels.
	CHECK_FALSE(Shows(view, "Grip"));
	CHECK_FALSE(Shows(view, "Terrain"));
	CHECK_FALSE(Shows(view, "Lighting"));

	// **A hidden row is still a row this world holds.** "Is this handle live"
	// and "is it on screen" are different questions, and answering them with
	// one value is how a selection quietly disappears.
	CHECK(view.Holds(scene.Grip));
	CHECK(view.RowOf(scene.Grip) == HierarchyView::NO_ROW);
}

TEST_CASE("a match does not unfold its own subtree", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	// Searching for a model should not put a thousand parts on screen because
	// their parent happens to be called what was typed.
	REQUIRE(view.Rebuild(scene.World, Closed("Model")));

	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Model"});
	CHECK(RowNamed(view, "Model")->Matched);

	// And with nothing under it left to show, it offers no arrow.
	CHECK_FALSE(RowNamed(view, "Model")->HasChildren);
	CHECK_FALSE(RowNamed(view, "Model")->Open);
}

TEST_CASE("a reveal opens the path to an instance without widening a filter", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	const Entity reveal[] = {scene.Grip};
	HierarchyRequest request = Closed();
	request.Reveal = reveal;

	REQUIRE(view.Rebuild(scene.World, request));
	CHECK(
		Names(view) == std::vector<std::string>{"Workspace", "Model", "Handle", "Grip", "Terrain", "Lighting"}
	);

	// The same reveal against a filter that excludes it. The path is opened,
	// because that costs nothing and is what the author asked for; the row
	// itself stays hidden, because a filter that a selection could smuggle rows
	// past is not a filter.
	request.Filter = "Terrain";
	REQUIRE(view.Rebuild(scene.World, request));
	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Terrain"});
	CHECK_FALSE(Shows(view, "Grip"));
}

TEST_CASE("the signature holds the rows still while the world does", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed()));

	// The whole point: an editor is open all day and almost every frame asks
	// the same question about the same world.
	CHECK_FALSE(view.Rebuild(scene.World, Closed()));
	CHECK_FALSE(view.Rebuild(scene.World, Closed()));
}

TEST_CASE("the signature moves for every edit the rows depend on", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed()));
	REQUIRE_FALSE(view.Rebuild(scene.World, Closed()));

	SECTION("a rename") {
		// The name is drawn on the row and is what the filter matches, so a
		// stamp that missed it would be the panel showing the old name - wrong
		// for exactly as long as nothing else changed, which is the version of
		// this bug nobody reports.
		scene.World.Set<InstanceName>(scene.Grip, InstanceName{Name("Hilt")});
		CHECK(view.Rebuild(scene.World, Closed()));

		const Entity open[] = {scene.Workspace, scene.Model};
		HierarchyRequest request = Closed();
		request.Open = open;
		REQUIRE(view.Rebuild(scene.World, request));
		CHECK(Shows(view, "Hilt"));
		CHECK_FALSE(Shows(view, "Grip"));
	}

	SECTION("a reparent") {
		scene.World.SetParent(scene.Grip, scene.Terrain);
		CHECK(view.Rebuild(scene.World, Closed()));
	}

	SECTION("a reorder among siblings") {
		// Same parent, same names, same count - only the order changed, and
		// the order is what the rows are.
		scene.World.SetParent(scene.Handle, NULL_ENTITY);
		REQUIRE(view.Rebuild(scene.World, Closed()));
		scene.World.SetParent(scene.Handle, scene.Model);
		CHECK(view.Rebuild(scene.World, Closed()));

		const Entity open[] = {scene.Workspace, scene.Model};
		HierarchyRequest request = Closed();
		request.Open = open;
		REQUIRE(view.Rebuild(scene.World, request));
		CHECK(
			Names(view) ==
			std::vector<std::string>{"Workspace", "Model", "Grip", "Handle", "Terrain", "Lighting"}
		);
	}

	SECTION("a create") {
		const Entity added = scene.World.CreateInstance(PartClass(), "Effect");
		scene.World.SetParent(added, scene.Model);
		CHECK(view.Rebuild(scene.World, Closed()));
	}

	SECTION("a destroy") {
		scene.World.DestroyInstance(scene.Model);
		CHECK(view.Rebuild(scene.World, Closed()));
		CHECK_FALSE(view.Holds(scene.Handle));
	}

	SECTION("the filter") {
		CHECK(view.Rebuild(scene.World, Closed("Grip")));
	}

	SECTION("the open set") {
		const Entity open[] = {scene.Workspace};
		HierarchyRequest request = Closed();
		request.Open = open;
		CHECK(view.Rebuild(scene.World, request));
	}

	SECTION("the reveal set") {
		const Entity reveal[] = {scene.Handle};
		HierarchyRequest request = Closed();
		request.Reveal = reveal;
		CHECK(view.Rebuild(scene.World, request));
	}

	SECTION("the open set, but not the order it arrives in") {
		// Sorted before it is folded in, so a caller whose vector happens to
		// shuffle does not pay for a rebuild that produces the same rows.
		const Entity forwards[] = {scene.Workspace, scene.Model};
		const Entity backwards[] = {scene.Model, scene.Workspace};

		HierarchyRequest request = Closed();
		request.Open = forwards;
		REQUIRE(view.Rebuild(scene.World, request));

		request.Open = backwards;
		CHECK_FALSE(view.Rebuild(scene.World, request));
	}
}

TEST_CASE("a view pointed at another world re-compiles", "[studio][hierarchy]") {
	Scene first;
	Scene second;
	HierarchyView view;

	REQUIRE(view.Rebuild(first.World, Closed()));
	CHECK_FALSE(view.Rebuild(first.World, Closed()));

	// Two worlds built the same way allocate the same entity ids and hold the
	// same names, so their *contents* hash identically - correct arithmetic and
	// the wrong answer. Which world it is has to be part of the signature, or a
	// view handed the other one keeps showing the first.
	CHECK(view.Rebuild(second.World, Closed()));
	CHECK_FALSE(view.Rebuild(second.World, Closed()));
	CHECK(view.Rebuild(first.World, Closed()));
}

TEST_CASE("the signature ignores everything the rows do not read", "[studio][hierarchy]") {
	// **The other half of the contract, and the half a soundness test cannot
	// reach.** A signature that changed whenever anything in the world moved
	// would be correct and worthless: an editor open beside a running
	// simulation would re-compile its tree every frame, which is the cost this
	// whole design exists to avoid.
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed()));
	REQUIRE_FALSE(view.Rebuild(scene.World, Closed()));

	SECTION("a property on an instance") {
		// The case that happens sixty times a second. `Transform` is not one of
		// the three columns the flatten reads, so moving every part in the
		// scene must not cost a single rebuild.
		for (const engine::ecs::Entity moved :
			 {scene.Workspace, scene.Model, scene.Handle, scene.Grip, scene.Terrain}) {
			engine::scene::Transform shifted;
			shifted.Frame.Position = engine::core::Vector3{1.0f, 2.0f, 3.0f};
			scene.World.Set<engine::scene::Transform>(moved, shifted);
		}

		// The write landed, so the absence of a rebuild below is the signature
		// ignoring a real change rather than the test having done nothing.
		REQUIRE(scene.World.Get<engine::scene::Transform>(scene.Grip) != nullptr);
		REQUIRE(scene.World.Get<engine::scene::Transform>(scene.Grip)->Frame.Position.X == 1.0f);

		CHECK_FALSE(view.Rebuild(scene.World, Closed()));
	}

	SECTION("the clock") {
		scene.World.AdvanceTick(1.0f / 60.0f);
		scene.World.SetFrame(1.0f / 60.0f, 0.5f);
		CHECK_FALSE(view.Rebuild(scene.World, Closed()));
	}

	SECTION("entities that are not instances") {
		// A plain entity carries no `Hierarchy`, so it is not in the tree and
		// not in the query the scan runs.
		const engine::ecs::Entity plain = scene.World.Create("plain");
		CHECK_FALSE(view.Rebuild(scene.World, Closed()));

		scene.World.Destroy(plain);
		CHECK_FALSE(view.Rebuild(scene.World, Closed()));
	}
}

TEST_CASE("a skipped rebuild leaves the rows usable", "[studio][hierarchy]") {
	// The rows are the answer, not a cache in front of one - so everything that
	// reads them has to keep working on a frame that did not re-compile. This
	// is what makes `Rebuild`'s `false` safe to ignore at the call site.
	Scene scene;
	HierarchyView view;

	const engine::ecs::Entity open[] = {scene.Workspace, scene.Model};
	HierarchyRequest request = Closed();
	request.Open = open;

	REQUIRE(view.Rebuild(scene.World, request));
	const std::vector<std::string> before = Names(view);
	const size_t grip = view.RowOf(scene.Grip);
	const size_t count = view.Count();

	REQUIRE_FALSE(view.Rebuild(scene.World, request));

	CHECK(Names(view) == before);
	CHECK(view.RowOf(scene.Grip) == grip);
	CHECK(view.Count() == count);
	CHECK(view.Holds(scene.Grip));
	CHECK(view.MatchCount() == 0);
}

TEST_CASE("a deep chain flattens in order and reports its depths", "[studio][hierarchy]") {
	const ClassId part = PartClass();
	Store world{"hierarchy_deep"};

	std::vector<engine::ecs::Entity> chain;
	for (int level = 0; level < 24; level++) {
		const engine::ecs::Entity made = world.CreateInstance(part, "Level" + std::to_string(level));
		if (!chain.empty()) {
			world.SetParent(made, chain.back());
		}
		chain.push_back(made);
	}

	HierarchyView view;
	HierarchyRequest request = Closed();
	request.Open = chain;
	REQUIRE(view.Rebuild(world, request));

	REQUIRE(view.Rows().size() == chain.size());
	for (size_t level = 0; level < chain.size(); level++) {
		CHECK(view.Rows()[level].Instance == chain[level]);
		CHECK(view.Rows()[level].Depth == level);
		CHECK(view.RowOf(chain[level]) == level);
	}

	// The deepest row is the only leaf, and the only one with no expander.
	CHECK_FALSE(view.Rows().back().HasChildren);
	CHECK(view.Rows().front().HasChildren);

	// Filtering to the bottom of the chain keeps the whole chain, because every
	// level of it is an ancestor of the match.
	REQUIRE(view.Rebuild(world, Closed("Level23")));
	CHECK(view.Rows().size() == chain.size());
	CHECK(view.MatchCount() == 1);
	CHECK(view.Rows().back().Matched);
	CHECK_FALSE(view.Rows().front().Matched);
}

TEST_CASE("several roots each keep their own subtree", "[studio][hierarchy]") {
	// The interleaving a single-root scene cannot catch: a root's whole subtree
	// is emitted before the next root starts, rather than the roots coming out
	// first and their children after.
	const ClassId part = PartClass();
	Store world{"hierarchy_forest"};

	const engine::ecs::Entity left = world.CreateInstance(part, "Left");
	const engine::ecs::Entity right = world.CreateInstance(part, "Right");
	const engine::ecs::Entity leftChild = world.CreateInstance(part, "LeftChild");
	const engine::ecs::Entity rightChild = world.CreateInstance(part, "RightChild");
	world.SetParent(leftChild, left);
	world.SetParent(rightChild, right);

	const engine::ecs::Entity open[] = {left, right};
	HierarchyRequest request = Closed();
	request.Open = open;

	HierarchyView view;
	REQUIRE(view.Rebuild(world, request));
	CHECK(Names(view) == std::vector<std::string>{"Left", "LeftChild", "Right", "RightChild"});

	// A filter that hits both branches keeps both, and nothing else.
	REQUIRE(view.Rebuild(world, Closed("Child")));
	CHECK(Names(view) == std::vector<std::string>{"Left", "LeftChild", "Right", "RightChild"});
	CHECK(view.MatchCount() == 2);
}

TEST_CASE("a filter matching nothing shows nothing", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed("zzzz")));
	CHECK(view.Rows().empty());
	CHECK(view.MatchCount() == 0);
	CHECK(view.Filtering());

	// Still the world it was: hiding every row is not forgetting them.
	CHECK(view.Count() == 6);
	CHECK(view.Holds(scene.Workspace));
	CHECK(view.RowOf(scene.Workspace) == HierarchyView::NO_ROW);
}

TEST_CASE("handles that name nothing are ignored rather than fatal", "[studio][hierarchy]") {
	// Both sets are handles the panel collected on an earlier frame, and an
	// instance can be deleted between one frame and the next - by an undo, by a
	// script, by Stop restoring a snapshot. A stale handle has to be nothing
	// more than an entry nobody matches.
	Scene scene;
	HierarchyView view;

	const engine::ecs::Entity dead = scene.Grip;
	scene.World.DestroyInstance(dead);

	const engine::ecs::Entity open[] = {scene.Workspace, dead, engine::ecs::Entity{0xABCDEF}};
	const engine::ecs::Entity reveal[] = {dead};
	HierarchyRequest request = Closed();
	request.Open = open;
	request.Reveal = reveal;

	REQUIRE(view.Rebuild(scene.World, request));
	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Model", "Terrain", "Lighting"});
	CHECK_FALSE(view.Holds(dead));
	CHECK(view.RowOf(dead) == HierarchyView::NO_ROW);

	// `Model` is closed, because the only thing that asked for it open was a
	// handle to something that is gone.
	CHECK_FALSE(RowNamed(view, "Model")->Open);
	CHECK(RowNamed(view, "Model")->HasChildren);
}

TEST_CASE("an unnamed instance draws as unnamed and matches no filter", "[studio][hierarchy]") {
	const ClassId part = PartClass();
	Store world{"hierarchy_unnamed"};

	const engine::ecs::Entity anonymous = world.CreateInstance(part, "");
	REQUIRE(anonymous != NULL_ENTITY);

	HierarchyView view;
	REQUIRE(view.Rebuild(world, Closed()));

	REQUIRE(view.Rows().size() == 1);
	CHECK(std::string_view(view.Rows()[0].Text) == "(unnamed)");

	// The class is still drawn, because that is the half of the row that always
	// has something to say.
	CHECK(std::string_view(view.Rows()[0].ClassText) == "Part");
	CHECK_FALSE(view.Rows()[0].Name.IsValid());

	// **Not matched by the empty-ish filter that finds it in the store.**
	// `FindFirstChild("")` treats an invalid name as a name; a filter box is a
	// person typing, and a row with nothing to read cannot be what they meant.
	REQUIRE(view.Rebuild(world, Closed("unnamed")));
	CHECK(view.Rows().empty());
	CHECK(view.MatchCount() == 0);
}

TEST_CASE("opening a leaf changes nothing", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	// `Terrain` has no children, so asking for it open is a request the tree
	// has nothing to honour with - and must not draw an expander for.
	const engine::ecs::Entity open[] = {scene.Workspace, scene.Terrain};
	HierarchyRequest request = Closed();
	request.Open = open;

	REQUIRE(view.Rebuild(scene.World, request));
	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Model", "Terrain", "Lighting"});
	CHECK_FALSE(RowNamed(view, "Terrain")->Open);
	CHECK_FALSE(RowNamed(view, "Terrain")->HasChildren);
}

TEST_CASE("the compiled rows are what a plain walk of the tree would draw", "[studio][hierarchy][fuzz]") {
	// **A model, written from the store rather than from the compiled nodes.**
	// Every other test here names a case somebody thought of. This one builds a
	// random tree, opens a random part of it, and checks the flattened rows
	// against a straightforward recursive walk - the obvious implementation the
	// clever one has to agree with.
	//
	// It is the only test that can catch a wrong answer in the parts that do
	// not look like anything: the binary chop into the sorted nodes, the
	// reverse push onto the flatten's stack, and the ancestor propagation's
	// early exit.
	const ClassId part = PartClass();
	Store world{"hierarchy_model"};

	std::vector<Entity> nodes;
	for (int index = 0; index < 120; index++) {
		// A quarter of the names repeat, so the filter has something to match
		// in more than one branch and at more than one depth.
		const Entity made =
			world.CreateInstance(part, (index % 4 == 0 ? "Marked" : "Node") + std::to_string(index));
		if (index > 0 && Random::Bits(static_cast<uint32_t>(index), 811) % 5 != 0) {
			world.SetParent(made, nodes[Random::Bits(static_cast<uint32_t>(index), 812) % nodes.size()]);
		}
		nodes.push_back(made);
	}

	std::vector<Entity> open;
	for (size_t index = 0; index < nodes.size(); index++) {
		if (Random::Bits(static_cast<uint32_t>(index), 813) % 3 != 0) {
			open.push_back(nodes[index]);
		}
	}

	const auto isOpen = [&](Entity instance) {
		return std::find(open.begin(), open.end(), instance) != open.end();
	};

	HierarchyRequest request = Closed();
	request.Open = open;

	HierarchyView view;
	REQUIRE(view.Rebuild(world, request));

	SECTION("unfiltered") {
		std::vector<Entity> expected;
		std::vector<uint16_t> depths;

		// The obvious walk: a node, then its children when it is open.
		const auto walk = [&](Entity at, uint16_t depth, auto &&self) -> void {
			expected.push_back(at);
			depths.push_back(depth);
			if (!isOpen(at)) {
				return;
			}
			world.EachChild(at, [&](Entity child) { self(child, static_cast<uint16_t>(depth + 1), self); });
		};
		world.EachRoot([&](Entity root) { walk(root, 0, walk); });

		// **The model has to have produced a tree worth comparing.** A model
		// test that quietly degenerates to "both sides are empty" passes
		// forever and checks nothing.
		REQUIRE(expected.size() > 20);
		REQUIRE(*std::max_element(depths.begin(), depths.end()) >= 2);

		REQUIRE(view.Rows().size() == expected.size());
		for (size_t index = 0; index < expected.size(); index++) {
			CHECK(view.Rows()[index].Instance == expected[index]);
			CHECK(view.Rows()[index].Depth == depths[index]);
			CHECK(view.RowOf(expected[index]) == index);
		}
	}

	SECTION("filtered") {
		int score = 0;
		const auto matches = [&](Entity at) {
			const Name name = world.InstanceNameOf(at);
			return name.IsValid() && studio::FuzzyMatch("Marked", studio::Label(name), score);
		};

		// Shown when this node matched, or anything beneath it did.
		const auto survives = [&](Entity at, auto &&self) -> bool {
			bool any = matches(at);
			world.EachChild(at, [&](Entity child) {
				if (self(child, self)) {
					any = true;
				}
			});
			return any;
		};

		std::vector<Entity> expected;
		const auto walk = [&](Entity at, uint16_t depth, auto &&self) -> void {
			if (!survives(at, survives)) {
				return;
			}
			expected.push_back(at);

			// An ancestor of a match is opened whatever the author last
			// clicked; anything else keeps the open set's answer, and neither
			// opens onto a subtree the filter has emptied.
			bool below = false;
			world.EachChild(at, [&](Entity child) {
				if (survives(child, survives)) {
					below = true;
				}
			});
			if (!below || !(isOpen(at) || below)) {
				return;
			}
			world.EachChild(at, [&](Entity child) { self(child, static_cast<uint16_t>(depth + 1), self); });
		};

		request.Filter = "Marked";
		REQUIRE(view.Rebuild(world, request));
		world.EachRoot([&](Entity root) { walk(root, 0, walk); });

		// Narrowed, but not to nothing - see the note above.
		REQUIRE(expected.size() > 5);
		REQUIRE(expected.size() < view.Count());

		REQUIRE(view.Rows().size() == expected.size());
		for (size_t index = 0; index < expected.size(); index++) {
			CHECK(view.Rows()[index].Instance == expected[index]);
		}

		// Every row is either a match or an ancestor of one, and every match is
		// a row.
		size_t matched = 0;
		for (const HierarchyRow &row : view.Rows()) {
			CHECK(survives(row.Instance, survives));
			CHECK(row.Matched == matches(row.Instance));
			if (row.Matched) {
				matched++;
			}
		}
		CHECK(view.MatchCount() == matched);
	}
}

TEST_CASE("Forget re-compiles unconditionally", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;

	REQUIRE(view.Rebuild(scene.World, Closed()));
	REQUIRE_FALSE(view.Rebuild(scene.World, Closed()));

	// The escape hatch for what the address cannot distinguish: a world freed
	// and another allocated where it was.
	view.Forget();
	CHECK(view.Rebuild(scene.World, Closed()));
}

TEST_CASE("an empty world compiles to nothing", "[studio][hierarchy]") {
	PartClass();
	Store world{"hierarchy_empty"};
	HierarchyView view;

	REQUIRE(view.Rebuild(world, Closed()));
	CHECK(view.Rows().empty());
	CHECK(view.Count() == 0);
	CHECK(view.RowOf(NULL_ENTITY) == HierarchyView::NO_ROW);
	CHECK_FALSE(view.Holds(NULL_ENTITY));
}

// --- the panel logic that used to live inside Editor -------------------------

TEST_CASE("a shift-click range is what the eye sees between two rows", "[studio][hierarchy]") {
	// **The drawn order, not the tree.** A range in a tree view is the rows
	// between two rows on screen, which is the flattened order with the closed
	// subtrees left out - so `Terrain` is in a range from `Handle` to
	// `Lighting` and `Grip` is too, because both are drawn between them.
	Scene scene;
	HierarchyView view;

	const Entity open[] = {scene.Workspace, scene.Model};
	HierarchyRequest request = Closed();
	request.Open = open;
	REQUIRE(view.Rebuild(scene.World, request));

	// Workspace, Model, Handle, Grip, Terrain, Lighting
	const auto names = [](std::span<const HierarchyRow> rows) {
		std::vector<std::string> out;
		for (const HierarchyRow &row : rows) {
			out.emplace_back(row.Text);
		}
		return out;
	};

	CHECK(
		names(studio::RowsBetween(view, scene.Handle, scene.Lighting)) ==
		std::vector<std::string>{"Handle", "Grip", "Terrain", "Lighting"}
	);

	// **Symmetric.** Dragging a range upwards is the same range as dragging it
	// down, or shift-clicking above the anchor would select nothing.
	CHECK(
		names(studio::RowsBetween(view, scene.Lighting, scene.Handle)) ==
		std::vector<std::string>{"Handle", "Grip", "Terrain", "Lighting"}
	);

	// A range of one is the row itself rather than nothing.
	CHECK(names(studio::RowsBetween(view, scene.Grip, scene.Grip)) == std::vector<std::string>{"Grip"});

	SECTION("an end that is not drawn yields nothing") {
		// The caller falls back to a plain click on an empty range, which is
		// what makes a stale anchor harmless rather than a gesture that
		// silently selects the wrong run.
		HierarchyView closed;
		REQUIRE(closed.Rebuild(scene.World, Closed()));

		// `Grip` is inside a collapsed `Workspace`, so it is not a row.
		CHECK(studio::RowsBetween(closed, scene.Grip, scene.Lighting).empty());
		CHECK(studio::RowsBetween(closed, scene.Lighting, scene.Grip).empty());
		CHECK(studio::RowsBetween(closed, NULL_ENTITY, scene.Lighting).empty());
	}

	SECTION("a filter changes what is between") {
		REQUIRE(view.Rebuild(scene.World, Closed("a")));

		// Whatever survived the filter, a range over it never includes a row
		// the filter removed - because the rows are the range.
		for (const HierarchyRow &row :
			 studio::RowsBetween(view, view.Rows().front().Instance, view.Rows().back().Instance)) {
			CHECK(view.RowOf(row.Instance) != HierarchyView::NO_ROW);
		}
	}
}

TEST_CASE("a multi-drag moves the outermost members only", "[studio][hierarchy]") {
	// Dragging a model and one of its own parts together means "move the
	// model". Moving both would take the part out of the model on the way,
	// which is the one outcome nobody dragging them together wants.
	Scene scene;
	HierarchyView view;
	REQUIRE(view.Rebuild(scene.World, Closed()));

	std::vector<Entity> out;

	SECTION("a parent and its child") {
		const Entity moving[] = {scene.Model, scene.Handle};
		studio::TopMost(view, moving, out);
		CHECK(out == std::vector<Entity>{scene.Model});
	}

	SECTION("a grandparent and a grandchild") {
		const Entity moving[] = {scene.Workspace, scene.Grip};
		studio::TopMost(view, moving, out);
		CHECK(out == std::vector<Entity>{scene.Workspace});
	}

	SECTION("siblings are all outermost") {
		const Entity moving[] = {scene.Handle, scene.Grip};
		studio::TopMost(view, moving, out);
		CHECK(out == std::vector<Entity>{scene.Handle, scene.Grip});
	}

	SECTION("order is the caller's") {
		// The first entry is what the reveal follows afterwards, so it has to
		// still be first.
		const Entity moving[] = {scene.Terrain, scene.Lighting, scene.Model};
		studio::TopMost(view, moving, out);
		CHECK(out == std::vector<Entity>{scene.Terrain, scene.Lighting, scene.Model});
	}

	SECTION("nothing moving is nothing to move") {
		studio::TopMost(view, {}, out);
		CHECK(out.empty());
	}

	SECTION("rows a filter is hiding still count as ancestors") {
		// The drag can only start on a drawn row, but the *ancestor test* has
		// to see the whole world - a model hidden by the filter is still the
		// parent of the part being dragged.
		HierarchyView filtered;
		REQUIRE(filtered.Rebuild(scene.World, Closed("Grip")));
		CHECK(filtered.RowOf(scene.Handle) == HierarchyView::NO_ROW);

		const Entity moving[] = {scene.Model, scene.Handle};
		studio::TopMost(filtered, moving, out);
		CHECK(out == std::vector<Entity>{scene.Model});
	}
}

TEST_CASE("IsUnder is reflexive and stops at the root", "[studio][hierarchy]") {
	Scene scene;
	HierarchyView view;
	REQUIRE(view.Rebuild(scene.World, Closed()));

	CHECK(view.IsUnder(scene.Grip, scene.Model));
	CHECK(view.IsUnder(scene.Grip, scene.Workspace));
	CHECK(view.IsUnder(scene.Model, scene.Model));
	CHECK_FALSE(view.IsUnder(scene.Workspace, scene.Grip));
	CHECK_FALSE(view.IsUnder(scene.Grip, scene.Lighting));

	// Not everything is inside nothing, matching `Store::IsDescendantOf`.
	CHECK_FALSE(view.IsUnder(scene.Grip, NULL_ENTITY));
	CHECK_FALSE(view.IsUnder(NULL_ENTITY, scene.Workspace));

	// And it agrees with the store, which is the thing it is standing in for.
	CHECK(
		view.IsUnder(scene.Grip, scene.Workspace) == scene.World.IsDescendantOf(scene.Grip, scene.Workspace)
	);
	CHECK(
		view.IsUnder(scene.Lighting, scene.Workspace) ==
		scene.World.IsDescendantOf(scene.Lighting, scene.Workspace)
	);
}
