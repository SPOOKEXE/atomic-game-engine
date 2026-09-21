#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>
TEST_SUITE_ID("engine.render.portalcapturetree")
using namespace engine::render;
namespace {
	PortalCaptureTree Tree(size_t count = 2) {
		PortalCaptureTree tree;
		for (size_t i = 0; i < count; ++i) {
			PortalCaptureTreeNode n;
			n.Producer = {"room" + std::to_string(i), "capture", 7, 9};
			auto &r = n.Layers.Opaque;
			r.Key = {i + 1, "door", 3, 4};
			r.Status = PortalImageStatus::Ok;
			r.Scope = PortalImageScope::OpaqueLighting;
			r.CaptureTick = 5;
			r.CaptureLighting.emplace();
			r.Width = r.Height = 1;
			r.RowStride = 8;
			r.Pixels.resize(8);
			r.Depth.resize(4);
			r.PixelHash = engine::assets::Hasher::Of(r.Pixels);
			r.DepthHash = engine::assets::Hasher::Of(r.Depth);
			tree.Nodes.push_back(std::move(n));
			if (i) {
				PortalCaptureTreeEdge e;
				e.Parent = i - 1;
				e.Child = i;
				e.PortalKey = "door";
				PortalGeometry geometry;
				geometry.Rows.emplace_back();
				std::string error;
				REQUIRE(EncodePortalGeometry(geometry, e.Geometry, error));
				tree.Edges.push_back(std::move(e));
			}
		}
		return tree;
	}
}
TEST_CASE(
	"capture trees retain independent nodes and aperture geometry transactionally", "[render][portal-tree]"
) {
	auto tree = Tree();
	for (auto &node : tree.Nodes) {
		node.Layers.Transparent.assign(2, node.Layers.Opaque);
		node.Layers.SpatialOverlay = node.Layers.Opaque;
		node.Layers.SpatialOverlay->Depth.clear();
		node.Layers.SpatialOverlay->DepthHash = {};
	}
	tree.Nodes[1].Camera.Position = {3, 4, 5};
	tree.Edges[0].Position = {1, 2, 3};
	tree.Edges[0].Scale = 2;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	PortalCaptureTreeMeasure measure;
	REQUIRE(MeasurePortalCaptureTree(wire, measure, error));
	CHECK(measure.Nodes == 2);
	CHECK(measure.Images == 8);
	CHECK(measure.Pixels == 8);
	CHECK(measure.DepthBytes == 24);
	CHECK(measure.MetadataBytes > tree.Edges[0].Geometry.size());
	PortalCaptureTree copy;
	REQUIRE(DecodePortalCaptureTree(wire, copy, error));
	CHECK(copy == tree);
	for (size_t n = 0; n < wire.size(); ++n) {
		PortalCaptureTreeMeasure untouched;
		untouched.Nodes = 99;
		CHECK_FALSE(MeasurePortalCaptureTree(std::span(wire).first(n), untouched, error));
		CHECK(untouched.Nodes == 99);
	}
	wire.pop_back();
	CHECK_FALSE(DecodePortalCaptureTree(wire, copy, error));
	CHECK(copy == tree);
}
TEST_CASE("capture trees reject malformed topology camera and cumulative budgets", "[render][portal-tree]") {
	auto tree = Tree();
	std::vector<std::byte> wire{std::byte{42}};
	std::string error;
	SECTION("cycle") {
		tree.Edges[0].Parent = 1;
	}
	SECTION("duplicate aperture key") {
		tree = Tree(3);
		tree.Edges[1].Parent = 0;
	}
	SECTION("missing parent") {
		tree = Tree(3);
		tree.Edges[1].Child = 1;
	}
	SECTION("depth") {
		tree = Tree(6);
	}
	SECTION("node bound") {
		tree = Tree(17);
	}
	SECTION("missing lighting") {
		tree.Nodes[1].Layers.Opaque.CaptureLighting.reset();
	}
	SECTION("endpoint") {
		tree.Nodes[1].Producer.Generation = 0;
	}
	SECTION("quaternion") {
		tree.Nodes[1].Camera.Orientation = {0, 0, 0, 2};
	}
	SECTION("nonfinite") {
		tree.Edges[0].Scale = std::numeric_limits<float>::infinity();
	}
	SECTION("aperture") {
		tree.Edges[0].Second = tree.Edges[0].First;
	}
	SECTION("pixels") {
		for (auto &n : tree.Nodes) {
			auto &r = n.Layers.Opaque;
			r.Width = r.Height = 512;
			r.RowStride = 4096;
			r.Pixels.resize(512 * 512 * 8);
			r.Depth.resize(512 * 512 * 4);
			r.PixelHash = engine::assets::Hasher::Of(r.Pixels);
			r.DepthHash = engine::assets::Hasher::Of(r.Depth);
		}
	}
	SECTION("geometry rows") {
		PortalGeometry g;
		g.Rows.resize(129);
		for (auto &e : (tree = Tree(3)).Edges)
			REQUIRE(EncodePortalGeometry(g, e.Geometry, error));
	}
	CHECK_FALSE(EncodePortalCaptureTree(tree, wire, error));
	CHECK(wire == std::vector<std::byte>{std::byte{42}});
}
TEST_CASE("capture tree depth boundary and single root remain usable", "[render][portal-tree]") {
	std::string error;
	std::vector<std::byte> wire;
	for (size_t nodes : {size_t(1), size_t(5)}) {
		auto tree = Tree(nodes);
		REQUIRE(EncodePortalCaptureTree(tree, wire, error));
		PortalCaptureTree copy;
		REQUIRE(DecodePortalCaptureTree(wire, copy, error));
		CHECK(copy == tree);
	}
}

TEST_CASE(
	"capture tree program identity is shared while copied code bytes remain charged", "[render][portal-tree]"
) {
	auto tree = Tree(16);
	for (auto &edge : tree.Edges) {
		edge.Parent = 0;
		edge.PortalKey += std::to_string(edge.Child);
	}
	PortalCaptureLensProgram program;
	program.SpirV = {0x07230203, 0x00010000, 0, 1, 0};
	program.Hash = engine::assets::Hasher::Of(std::as_bytes(std::span(program.SpirV)));
	for (auto &node : tree.Nodes) {
		node.Layers.Lenses.Programs.push_back(program);
		PortalCaptureLens lens;
		lens.Shader = "effect";
		lens.ProgramHash = program.Hash;
		node.Layers.Lenses.Entries.push_back(lens);
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(ValidPortalCaptureTree(tree));
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	PortalCaptureTree decoded;
	REQUIRE(DecodePortalCaptureTree(wire, decoded, error));
	CHECK(decoded == tree);
	auto &extra = tree.Nodes.back().Layers.Lenses;
	for (uint32_t i = 1; i <= 15; ++i) {
		auto other = program;
		other.SpirV[2] = i;
		other.Hash = engine::assets::Hasher::Of(std::as_bytes(std::span(other.SpirV)));
		extra.Programs.push_back(other);
		auto lens = extra.Entries.front();
		lens.ProgramHash = other.Hash;
		extra.Entries.push_back(lens);
	}
	auto distinct = program;
	distinct.SpirV[2] = 16;
	distinct.Hash = engine::assets::Hasher::Of(std::as_bytes(std::span(distinct.SpirV)));
	tree.Nodes.front().Layers.Lenses.Programs.front() = distinct;
	tree.Nodes.front().Layers.Lenses.Entries.front().ProgramHash = distinct.Hash;
	CHECK_FALSE(ValidPortalCaptureTree(tree));
	for (auto &node : tree.Nodes) {
		auto &p = node.Layers.Lenses.Programs.front();
		p.SpirV.resize(32768);
		p.Hash = engine::assets::Hasher::Of(std::as_bytes(std::span(p.SpirV)));
		node.Layers.Lenses.Programs.resize(1);
		node.Layers.Lenses.Entries.resize(1);
		node.Layers.Lenses.Entries.front().ProgramHash = p.Hash;
	}
	CHECK_FALSE(ValidPortalCaptureTree(tree));
}

TEST_CASE("capture tree preflight charges expanded ambient planes", "[render][portal-tree]") {
	const bool directional = GENERATE(false, true);
	auto tree = Tree();
	for (auto &node : tree.Nodes) {
		auto &reply = node.Layers.Opaque;
		reply.Width = reply.Height = 32;
		reply.RowStride = reply.Width * 8;
		reply.Pixels.assign(32 * 32 * 8, std::byte{});
		reply.Depth.assign(32 * 32 * 4, std::byte{});
		reply.Normal.assign(32 * 32 * 4, std::byte{255});
		reply.AmbientResponse.assign(32 * 32 * 16, std::byte{});
		reply.PixelHash = engine::assets::Hasher::Of(reply.Pixels);
		reply.DepthHash = engine::assets::Hasher::Of(reply.Depth);
		reply.NormalHash = engine::assets::Hasher::Of(reply.Normal);
		reply.AmbientResponseHash = engine::assets::Hasher::Of(reply.AmbientResponse);
		reply.LightingBaseline = reply.AmbientResponse;
		reply.LightingBaselineHash = engine::assets::Hasher::Of(reply.LightingBaseline);
		if (directional) {
			reply.DirectionalResponse = reply.AmbientResponse;
			reply.DirectionalResponseHash = engine::assets::Hasher::Of(reply.DirectionalResponse);
		}
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	PortalCaptureTreeMeasure measure;
	REQUIRE(MeasurePortalCaptureTree(wire, measure, error));
	CHECK(measure.AmbientBytes == 2 * 32 * 32 * (directional ? 52 : 36));
	CHECK(measure.DepthBytes == 2 * 32 * 32 * 4);
	CHECK(measure.Pixels == 2 * 32 * 32);
	CHECK(wire.size() < measure.AmbientBytes);
	PortalCaptureTree decoded;
	REQUIRE(DecodePortalCaptureTree(wire, decoded, error));
	CHECK(decoded == tree);
	wire.pop_back();
	CHECK_FALSE(MeasurePortalCaptureTree(wire, measure, error));
	CHECK(measure.AmbientBytes == 2 * 32 * 32 * (directional ? 52 : 36));
	CHECK_FALSE(DecodePortalCaptureTree(wire, decoded, error));
	CHECK(decoded == tree);
}

TEST_CASE(
	"capture trees bind retained body exclusion across every node", "[render][portal-tree][retained-body]"
) {
	auto tree = Tree();
	for (auto &node : tree.Nodes)
		node.RetainedBodyPlayer = "91";
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalCaptureTree(tree, wire, error));
	PortalCaptureTree decoded;
	REQUIRE(DecodePortalCaptureTree(wire, decoded, error));
	CHECK(decoded == tree);
	const auto &root = tree.Nodes.front();
	PortalCaptureTreeAdmission admission{
		root.Layers.Opaque.Key,
		1,
		1,
		root.Camera,
		{root.Producer.World, root.Producer.Channel, root.Producer.Session, root.Producer.Generation},
		4,
		MAX_PORTAL_IMAGE_PIXELS,
		"91"
	};
	PortalCaptureTreeMeasure measured;
	REQUIRE(MatchPortalCaptureTree(wire, admission, [](auto) { return true; }, measured, error));
	admission.RetainedBodyPlayer = "92";
	CHECK_FALSE(MatchPortalCaptureTree(wire, admission, [](auto) { return true; }, measured, error));
	tree.Nodes.back().RetainedBodyPlayer = "92";
	CHECK_FALSE(EncodePortalCaptureTree(tree, wire, error));
	tree.Nodes.back().RetainedBodyPlayer = "091";
	CHECK_FALSE(ValidPortalCaptureTree(tree));
}
