// A bake pipeline drawn as instances, checked without a screen.
//
// **The layout is the part worth testing.** A render pipeline is a chain and
// its columns are counting; a bake pipeline branches, so depth has to be
// derived from wires — and a node drawn left of something feeding it is a
// canvas that lies about which way the data goes.

#include <engine/bake/GraphDocument.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/nodeview/Assets.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.nodeview.assets")
TEST_DEPENDS("engine.bake.graphdocument")

using engine::bake::Document;
using engine::bake::Operation;
using engine::bake::OperationKind;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::nodeview::AssetLayout;
using engine::nodeview::BuildAssets;
using engine::nodeview::Canvas;
using engine::nodeview::LayoutAssets;

namespace {
	Operation Builtin(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddBuiltin;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Fit(float size) {
		Operation operation;
		operation.Kind = OperationKind::AddFit;
		operation.Number = size;
		return operation;
	}

	Operation WriteNode(std::string name) {
		Operation operation;
		operation.Kind = OperationKind::AddWrite;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Wire(uint32_t from, uint32_t to) {
		Operation operation;
		operation.Kind = OperationKind::Connect;
		operation.From = from;
		operation.To = to;
		return operation;
	}

	// builtin -> fit -> write
	Document Chain() {
		Document document;
		document.Record(Builtin("engine.Cube"));
		document.Record(Fit(2.0f));
		document.Record(WriteNode("box.amesh"));
		document.Record(Wire(1, 2));
		document.Record(Wire(2, 3));
		return document;
	}

	Entity HostIn(Store &store) {
		engine::gui::RegisterGuiClasses();
		return store.CreateInstance(engine::gui::GuiClass("Frame"), "Panel");
	}
}

TEST_CASE("a chain lays out left to right", "[nodeview]") {
	const AssetLayout layout = LayoutAssets(Chain());

	REQUIRE(layout.Nodes.size() == 3);
	CHECK(layout.Nodes[0].Column == 0);
	CHECK(layout.Nodes[1].Column == 1);
	CHECK(layout.Nodes[2].Column == 2);
	CHECK(layout.Columns == 3);
	CHECK(layout.Rows == 1);

	// Wires are not nodes and keep their own numbering.
	CHECK(layout.Wires.size() == 2);
	CHECK(layout.Nodes[0].Position == 1);
}

TEST_CASE("a node sits one past the deepest thing feeding it", "[nodeview]") {
	// **The case a chain cannot show.** Two sources feed one write; the write
	// belongs right of both, not right of whichever was wired first.
	Document document;
	document.Record(Builtin("engine.Cube"));   // 1
	document.Record(Fit(2.0f));				   // 2
	document.Record(Builtin("engine.Sphere")); // 3
	document.Record(WriteNode("out.amesh"));   // 4
	document.Record(Wire(1, 2));
	document.Record(Wire(2, 4));
	document.Record(Wire(3, 4));

	const AssetLayout layout = LayoutAssets(document);

	CHECK(layout.Nodes[1].Column == 1); // fit, after the cube
	CHECK(layout.Nodes[2].Column == 0); // the second source is a root
	CHECK(layout.Nodes[3].Column == 2); // the write, past the fit and not past the sphere only
}

TEST_CASE("nodes sharing a depth stack instead of overlapping", "[nodeview]") {
	// "Many node trees in one editor" — two chains that never meet.
	Document document;
	document.Record(Builtin("engine.Cube"));   // 1
	document.Record(Builtin("engine.Sphere")); // 2
	document.Record(WriteNode("a.amesh"));	   // 3
	document.Record(WriteNode("b.amesh"));	   // 4
	document.Record(Wire(1, 3));
	document.Record(Wire(2, 4));

	const AssetLayout layout = LayoutAssets(document);

	CHECK(layout.Nodes[0].Column == 0);
	CHECK(layout.Nodes[1].Column == 0);
	CHECK(layout.Nodes[0].Row != layout.Nodes[1].Row);
	CHECK(layout.Rows == 2);
}

TEST_CASE("a wire naming a node the document does not hold is ignored", "[nodeview]") {
	// A canvas that would not draw a broken document could never show somebody
	// what was wrong with it. `bake::Build` is what refuses one.
	Document document;
	document.Record(Builtin("engine.Cube"));
	document.Record(Wire(1, 9));

	const AssetLayout layout = LayoutAssets(document);
	CHECK(layout.Nodes.size() == 1);
	CHECK(layout.Nodes[0].Column == 0);
}

TEST_CASE("the canvas draws one box per node", "[nodeview]") {
	Store store("nodeview.assets.build");
	const Entity host = HostIn(store);

	const Canvas canvas = BuildAssets(store, host, LayoutAssets(Chain()));

	REQUIRE(store.Alive(canvas.Root));
	REQUIRE(canvas.Nodes.size() == 3);
	for (const auto &node : canvas.Nodes) {
		CHECK(store.Alive(node.Box));
	}

	// **No bands.** A bake chain has no shared/per-view split to draw.
	CHECK(canvas.Bands.empty());
}

TEST_CASE("a box says what kind it is and what it names", "[nodeview]") {
	Store store("nodeview.assets.labels");
	const Entity host = HostIn(store);

	const Canvas canvas = BuildAssets(store, host, LayoutAssets(Chain()));

	const auto *first = store.Get<engine::gui::Label>(canvas.Nodes[0].Label);
	REQUIRE(first != nullptr);
	CHECK(first->Text.find("builtin") != std::string::npos);
	CHECK(first->Text.find("engine.Cube") != std::string::npos);

	// A `fit` carries no name, so it says only what it is.
	const auto *second = store.Get<engine::gui::Label>(canvas.Nodes[1].Label);
	REQUIRE(second != nullptr);
	CHECK(second->Text == "fit");
}

TEST_CASE("boxes are named by position so a panel can find one", "[nodeview]") {
	// A chain may hold four `import` nodes; the position is what tells them
	// apart, and it is the numbering `Operation::From` already uses.
	Store store("nodeview.assets.names");
	const Entity host = HostIn(store);

	const Canvas canvas = BuildAssets(store, host, LayoutAssets(Chain()));
	CHECK(canvas.Nodes[0].Name == Name("1"));
	CHECK(canvas.Nodes[2].Name == Name("3"));
}

TEST_CASE("rebuilding replaces the canvas", "[nodeview]") {
	Store store("nodeview.assets.rebuild");
	const Entity host = HostIn(store);

	BuildAssets(store, host, LayoutAssets(Chain()));
	const Canvas second = BuildAssets(store, host, LayoutAssets(Chain()));

	size_t children = 0;
	Entity only;
	store.EachChild(host, [&](Entity child) {
		children++;
		only = child;
	});
	CHECK(children == 1);
	CHECK(only == second.Root);
}

TEST_CASE("an empty document builds a canvas and no boxes", "[nodeview]") {
	Store store("nodeview.assets.empty");
	const Entity host = HostIn(store);

	const Canvas canvas = BuildAssets(store, host, LayoutAssets(Document{}));
	CHECK(store.Alive(canvas.Root));
	CHECK(canvas.Nodes.empty());
}
