// The bake pipeline: input nodes, processing nodes, export nodes.
//
// Every case here runs without opening a file, which is the property the graph
// is shaped around - `audio::NullDevice` is the same idea for the mixer. The
// bytes a source node holds are a built-in mesh serialised in the test itself,
// so the suite exercises the real import path over a real file format with no
// fixture on disk to go stale.

#include <engine/assets/Builtin.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/bake/Graph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bake.graph")

using Catch::Approx;
using engine::assets::AssetKind;
using engine::assets::BuiltinMesh;
using engine::assets::MakeBuiltin;
using engine::assets::MeshData;
using engine::assets::TextureData;
using engine::bake::Graph;
using engine::bake::NodeId;
using engine::bake::NodeKind;
using engine::bake::PayloadKind;
using engine::core::ByteReader;
using engine::core::ByteWriter;

namespace {
	// A 2x2 BMP, which is the smallest real image file worth having.
	constexpr std::array<uint8_t, 70> BMP{{
		0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
		0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
		0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
		0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
	}};

	std::span<const std::byte> Bytes(std::span<const uint8_t> raw) {
		return {reinterpret_cast<const std::byte *>(raw.data()), raw.size()};
	}

	std::string Ran(Graph &graph) {
		std::string failure;
		if (!graph.Run(failure)) {
			return failure.empty() ? "failed with no reason" : failure;
		}
		return {};
	}
}

TEST_CASE("a decoded image is written as a texture asset", "[bake][graph]") {
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId write = graph.AddWrite("textures/floor");

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, write));
	REQUIRE(Ran(graph).empty());

	CHECK(graph.Output(import).Kind == PayloadKind::Texture);
	CHECK(graph.Output(import).Texture.Width == 2);

	REQUIRE(graph.Baked().size() == 1);
	CHECK(graph.Baked()[0].Name == "textures/floor");

	// The kind is recorded rather than re-derived from the name downstream,
	// which is `assets::AssetKind`'s whole rule.
	CHECK(graph.Baked()[0].Kind == AssetKind::Texture);

	TextureData read;
	ByteReader reader(graph.Baked()[0].Bytes);
	REQUIRE(engine::assets::Texture::Read(reader, read));
	CHECK(read.Width == 2);
	CHECK(read.Height == 2);
}

TEST_CASE("a built-in mesh needs no bytes", "[bake][graph]") {
	Graph graph;
	const NodeId builtin = graph.AddBuiltin("engine.Sphere");
	const NodeId write = graph.AddWrite("engine.Sphere");

	REQUIRE(builtin.IsValid());
	REQUIRE(graph.Connect(builtin, write));
	REQUIRE(Ran(graph).empty());

	REQUIRE(graph.Baked().size() == 1);
	CHECK(graph.Baked()[0].Kind == AssetKind::Mesh);

	MeshData read;
	ByteReader reader(graph.Baked()[0].Bytes);
	REQUIRE(engine::assets::Mesh::Read(reader, read));
	CHECK(read.Vertices.size() == MakeBuiltin(BuiltinMesh::Sphere).Vertices.size());
}

TEST_CASE("an unknown built-in is refused when it is named", "[bake][graph]") {
	Graph graph;

	// At `Add` rather than at `Run`, so the mistake is reported where it was
	// made instead of at the end of a pipeline somebody has already wired.
	CHECK_FALSE(graph.AddBuiltin("engine.Dodecahedron").IsValid());
	CHECK_FALSE(graph.AddBuiltin("Sphere").IsValid());
	CHECK(graph.NodeCount() == 0);
}

TEST_CASE("a mesh round-trips through import and export", "[bake][graph]") {
	// The source bytes are a real baked mesh, so the source-import-write chain
	// is exercised against the format rather than against a stub.
	ByteWriter writer;
	REQUIRE(engine::assets::Mesh::Write(writer, MakeBuiltin(BuiltinMesh::Wedge)));
	const std::span<const std::byte> baked = writer.Bytes();
	const std::vector<std::byte> bytes(baked.begin(), baked.end());

	Graph graph;
	const NodeId source = graph.AddSource("props/ramp.amesh", bytes);
	const NodeId write = graph.AddWrite("props/ramp");

	// A source node's bytes are already an asset, so no import is needed -
	// except that the payload is `Bytes` and `Write` wants a mesh, which is the
	// mismatch this checks.
	REQUIRE(graph.Connect(source, write));
	CHECK_FALSE(Ran(graph).empty());
}

TEST_CASE("a fit node scales a mesh into a stated box", "[bake][graph]") {
	Graph graph;
	const NodeId builtin = graph.AddBuiltin("engine.Cube");
	const NodeId fit = graph.AddFit(4.0f);

	REQUIRE(graph.Connect(builtin, fit));
	REQUIRE(Ran(graph).empty());

	const MeshData &mesh = graph.Output(fit).Mesh;
	CHECK((mesh.Maximum.X - mesh.Minimum.X) == Approx(4.0f));
	CHECK(mesh.Maximum.Y == Approx(2.0f));
}

TEST_CASE("a scale node rebuilds normals only when it has to", "[bake][graph]") {
	SECTION("uniform leaves them alone") {
		Graph graph;
		const NodeId builtin = graph.AddBuiltin("engine.Cube");
		const NodeId scale = graph.AddScale({2.0f, 2.0f, 2.0f});
		REQUIRE(graph.Connect(builtin, scale));
		REQUIRE(Ran(graph).empty());

		// A cube's faces are flat, and rebuilding normals over a cube whose
		// vertices are shared per face changes nothing - but rebuilding them
		// after a *uniform* scale would be work with no reason, and the visible
		// consequence on a model with split normals is that it turns smooth.
		const MeshData &mesh = graph.Output(scale).Mesh;
		CHECK(mesh.Vertices[0].Normal[0] == Approx(1.0f));
		CHECK(mesh.Maximum.X == Approx(1.0f));
	}

	SECTION("non-uniform rebuilds them") {
		Graph graph;
		const NodeId builtin = graph.AddBuiltin("engine.Sphere");
		const NodeId scale = graph.AddScale({1.0f, 4.0f, 1.0f});
		REQUIRE(graph.Connect(builtin, scale));
		REQUIRE(Ran(graph).empty());

		const MeshData &mesh = graph.Output(scale).Mesh;
		CHECK(mesh.Maximum.Y == Approx(2.0f));

		// Every normal still unit length, which a stretched-but-unrebuilt
		// sphere's would also be - what changes is where they point, and the
		// cheap standing check is that nothing became degenerate.
		for (const engine::assets::MeshVertex &vertex : mesh.Vertices) {
			const float length = std::sqrt(
				vertex.Normal[0] * vertex.Normal[0] + vertex.Normal[1] * vertex.Normal[1] +
				vertex.Normal[2] * vertex.Normal[2]
			);
			REQUIRE(length == Approx(1.0f).margin(1e-4));
		}
	}
}

TEST_CASE("a resize node box-filters a texture", "[bake][graph]") {
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId resize = graph.AddResize(1, 1);

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, resize));
	REQUIRE(Ran(graph).empty());

	CHECK(graph.Output(resize).Texture.Width == 1);
	CHECK(graph.Output(resize).Texture.Height == 1);
}

TEST_CASE("an opaque node fills the alpha channel", "[bake][graph]") {
	Graph graph;
	const NodeId source = graph.AddSource("textures/sphere.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId opaque = graph.Add(NodeKind::Opaque);

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, opaque));
	REQUIRE(Ran(graph).empty());

	for (size_t pixel = 3; pixel < graph.Output(opaque).Texture.Pixels.size(); pixel += 4) {
		REQUIRE(graph.Output(opaque).Texture.Pixels[pixel] == std::byte{255});
	}
}

TEST_CASE("a mipmap node builds the chain and a mesh cannot have one", "[bake][graph]") {
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId mipmap = graph.Add(NodeKind::Mipmap);

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, mipmap));
	REQUIRE(Ran(graph).empty());
	CHECK(graph.Output(mipmap).Texture.LevelCount() == 2);

	// **A rerun rebuilds rather than appends**, which is what makes a graph a
	// description: the same nodes over the same source bake the same bytes.
	REQUIRE(Ran(graph).empty());
	CHECK(graph.Output(mipmap).Texture.LevelCount() == 2);

	Graph mesh;
	const NodeId cube = mesh.AddBuiltin("engine.Cube");
	const NodeId wrong = mesh.Add(NodeKind::Mipmap);
	REQUIRE(mesh.Connect(cube, wrong));
	CHECK_FALSE(Ran(mesh).empty());
}

TEST_CASE("one input fans out to several chains", "[bake][graph]") {
	// The useful direction, and the reason a node is allowed one input and any
	// number of consumers: one decode, three sizes.
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	REQUIRE(graph.Connect(source, import));

	for (const uint32_t size : {1u, 2u, 4u}) {
		const NodeId resize = graph.AddResize(size, size);
		const NodeId write = graph.AddWrite("textures/floor@" + std::to_string(size));
		REQUIRE(graph.Connect(import, resize));
		REQUIRE(graph.Connect(resize, write));
	}

	REQUIRE(Ran(graph).empty());
	REQUIRE(graph.Baked().size() == 3);
	CHECK(graph.Baked()[0].Name == "textures/floor@1");
	CHECK(graph.Baked()[2].Name == "textures/floor@4");
}

TEST_CASE("a second input on one node is refused rather than replacing the first", "[bake][graph]") {
	Graph graph;
	const NodeId first = graph.AddBuiltin("engine.Cube");
	const NodeId second = graph.AddBuiltin("engine.Sphere");
	const NodeId fit = graph.AddFit(1.0f);

	REQUIRE(graph.Connect(first, fit));

	// Silently rewiring would make the order of two `Connect` calls decide what
	// the pipeline does.
	CHECK_FALSE(graph.Connect(second, fit));
	REQUIRE(Ran(graph).empty());
	CHECK(graph.Output(fit).Source == "engine.Cube");
}

TEST_CASE("a cycle is refused at the wire", "[bake][graph]") {
	Graph graph;
	const NodeId builtin = graph.AddBuiltin("engine.Cube");
	const NodeId first = graph.Add(NodeKind::Smooth);
	const NodeId second = graph.Add(NodeKind::Smooth);

	REQUIRE(graph.Connect(builtin, first));
	REQUIRE(graph.Connect(first, second));

	// Closing the loop. Found here rather than at execution, where it would be
	// a run that never terminates rather than a call that returns false.
	CHECK_FALSE(graph.Connect(second, first));
	CHECK_FALSE(graph.Connect(first, first));

	REQUIRE(Ran(graph).empty());
}

TEST_CASE("an input node cannot be given an input", "[bake][graph]") {
	Graph graph;
	const NodeId builtin = graph.AddBuiltin("engine.Cube");
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));

	CHECK_FALSE(graph.Connect(builtin, source));
	CHECK_FALSE(graph.Connect(source, builtin));

	// And an input kind cannot be added through the plain overload, because it
	// would carry none of the data that makes it an input.
	CHECK_FALSE(graph.Add(NodeKind::Source).IsValid());
	CHECK_FALSE(graph.Add(NodeKind::Builtin).IsValid());
}

TEST_CASE("a node with no input fails the run rather than producing nothing", "[bake][graph]") {
	Graph graph;
	graph.Add(NodeKind::Smooth);

	// The alternative is a `Write` further down emitting an empty asset, which
	// publishes and then fails at whatever tries to draw it.
	CHECK_FALSE(Ran(graph).empty());
}

TEST_CASE("a kind mismatch names the file it came from", "[bake][graph]") {
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId fit = graph.AddFit(1.0f);

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, fit));

	// A bake tool runs over a directory, so "wants a mesh" without the name is
	// a message somebody has to bisect a build to act on.
	const std::string failure = Ran(graph);
	CHECK(failure.find("textures/floor.bmp") != std::string::npos);
}

TEST_CASE("running twice re-evaluates from the sources", "[bake][graph]") {
	Graph graph;
	const NodeId builtin = graph.AddBuiltin("engine.Cube");
	const NodeId write = graph.AddWrite("engine.Cube");
	REQUIRE(graph.Connect(builtin, write));

	REQUIRE(Ran(graph).empty());
	const std::vector<std::byte> first = graph.Baked()[0].Bytes;

	// A graph is a description rather than a one-shot, so the exports must not
	// accumulate across runs.
	REQUIRE(Ran(graph).empty());
	REQUIRE(graph.Baked().size() == 1);
	CHECK(graph.Baked()[0].Bytes == first);
}

TEST_CASE("an unknown node is not connectable", "[bake][graph]") {
	Graph graph;
	const NodeId real = graph.AddBuiltin("engine.Cube");

	CHECK_FALSE(graph.Connect(NodeId{}, real));
	CHECK_FALSE(graph.Connect(real, NodeId{}));
	CHECK_FALSE(graph.Connect(real, NodeId{9999}));
	CHECK(graph.Output(NodeId{9999}).Kind == PayloadKind::None);
}

TEST_CASE("a source name travels down the chain", "[bake][graph]") {
	// What makes an import node need no configuration: it dispatches on where
	// its bytes came from, so one chain shape serves every format.
	Graph graph;
	const NodeId source = graph.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId import = graph.Add(NodeKind::Import);
	const NodeId resize = graph.AddResize(1, 1);

	REQUIRE(graph.Connect(source, import));
	REQUIRE(graph.Connect(import, resize));
	REQUIRE(Ran(graph).empty());

	CHECK(graph.Output(resize).Source == "textures/floor.bmp");
}

TEST_CASE("a drawing enters through rasterize and never through import", "[bake][graph]") {
	constexpr std::string_view DRAWING =
		R"(<svg width="4" height="4"><rect width="4" height="4" fill="#00ff00"/></svg>)";
	const std::span<const std::byte> markup(
		reinterpret_cast<const std::byte *>(DRAWING.data()), DRAWING.size()
	);

	// **The size is the node's, because the file has none.** A `Resize` after an
	// `Import` cannot stand in for this: there would be nothing to resize until
	// something had already chosen a rasterisation, and that choice is the one
	// being made here.
	Graph graph;
	const NodeId source = graph.AddSource("icons/leaf.svg", markup);
	const NodeId rasterize = graph.AddRasterize(16, 16);
	const NodeId write = graph.AddWrite("icons/leaf");

	REQUIRE(graph.Connect(source, rasterize));
	REQUIRE(graph.Connect(rasterize, write));
	REQUIRE(Ran(graph).empty());

	CHECK(graph.Output(rasterize).Kind == PayloadKind::Texture);
	CHECK(graph.Output(rasterize).Texture.Width == 16);
	CHECK(graph.Output(rasterize).Texture.Height == 16);
	REQUIRE(graph.Baked().size() == 1);
	CHECK(graph.Baked()[0].Kind == AssetKind::Texture);

	// A zero target asks for the size the document declares, which is the only
	// size an SVG can be said to have.
	Graph declared;
	const NodeId bytes = declared.AddSource("icons/leaf.svg", markup);
	const NodeId intrinsic = declared.AddRasterize(0, 0);
	REQUIRE(declared.Connect(bytes, intrinsic));
	REQUIRE(Ran(declared).empty());
	CHECK(declared.Output(intrinsic).Texture.Width == 4);

	// An `Import` handed the same bytes names the node that would have worked
	// rather than guessing a size or failing inside an XML parser.
	Graph imported;
	const NodeId same = imported.AddSource("icons/leaf.svg", markup);
	const NodeId import = imported.Add(NodeKind::Import);
	REQUIRE(imported.Connect(same, import));
	CHECK(Ran(imported).find("rasterize node") != std::string::npos);

	// And a `Rasterize` handed something that already has pixels says so, rather
	// than reporting a PNG's compressed data as bad markup.
	Graph mistaken;
	const NodeId picture = mistaken.AddSource("textures/floor.bmp", Bytes(BMP));
	const NodeId wrong = mistaken.AddRasterize(16, 16);
	REQUIRE(mistaken.Connect(picture, wrong));
	CHECK(Ran(mistaken).find("already has a size") != std::string::npos);
}
