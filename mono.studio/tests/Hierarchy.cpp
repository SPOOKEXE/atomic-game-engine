// What the Explorer decides to draw, without an imgui frame to draw it into.
//
// **The second part of this program a headless test can reach**, and it is the
// half that has an algorithm in it. `Widgets.cpp` says the panels themselves
// need a window and a device; compiling a tree does not, which is the whole
// reason the compile is a class rather than a function inside `DrawExplorer`.

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <studio/Hierarchy.hpp>
#include <vector>

TEST_SUITE_ID("studio.hierarchy")

using engine::core::Name;
using engine::ecs::ClassId;
using engine::ecs::Classes;
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
	// one `Store::EachRoot` yields — creation order, not insertion order — and
	// a panel that sorted them its own way would be a second answer to it.
	std::vector<Entity> roots;
	scene.World.EachRoot([&](Entity root) { roots.push_back(root); });

	REQUIRE(view.Rows().size() == roots.size());
	for (size_t index = 0; index < roots.size(); index++) {
		CHECK(view.Rows()[index].Instance == roots[index]);
		CHECK(view.Rows()[index].Depth == 0);
	}

	// Closed, so nothing below a root is a row — but the arrow is drawn.
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
		Names(view) ==
		std::vector<std::string>{"Workspace", "Model", "Handle", "Grip", "Terrain", "Lighting"}
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
	// it anyway and unfolds the path to it — a match with no parents above it
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
	CHECK(Names(view) == std::vector<std::string>{"Workspace", "Model", "Handle", "Grip", "Terrain", "Lighting"});

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
		// stamp that missed it would be the panel showing the old name — wrong
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
		// Same parent, same names, same count — only the order changed, and
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
	// same names, so their *contents* hash identically — correct arithmetic and
	// the wrong answer. Which world it is has to be part of the signature, or a
	// view handed the other one keeps showing the first.
	CHECK(view.Rebuild(second.World, Closed()));
	CHECK_FALSE(view.Rebuild(second.World, Closed()));
	CHECK(view.Rebuild(first.World, Closed()));
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
