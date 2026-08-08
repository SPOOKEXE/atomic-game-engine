// A bake pipeline drawn as instances, checked without a screen.
//
// **The layout is the part worth testing.** A render pipeline is a chain and
// its columns are counting; a bake pipeline branches, so depth has to be
// derived from wires — and a node drawn left of something feeding it is a
// canvas that lies about which way the data goes.

#include <engine/bake/GraphDocument.hpp>
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
using engine::nodeview::AssetLayout;
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
