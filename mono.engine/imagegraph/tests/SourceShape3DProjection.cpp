#include "../src/nodes/SourceShape3DProjection.hpp"

#include "../src/nodes/SourceShape3DGeometry.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_projection")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE(
	"Shape 3D native top-origin profile has an independent matrix-order coordinate oracle",
	"[imagegraph][shape3d]"
) {
	SourceShape3DRecipe r;
	r.Width = 100;
	r.Height = 80;
	r.WorldStack = {
		{{SourceShape3DTransformKind::Translate, {.25, 0, -.25}},
		 {SourceShape3DTransformKind::Translate, {}},
		 {SourceShape3DTransformKind::RotateX, {90, 0, 0}},
		 {SourceShape3DTransformKind::RotateY, {}},
		 {SourceShape3DTransformKind::RotateZ, {0, 0, -90}},
		 {SourceShape3DTransformKind::Translate, {}},
		 {SourceShape3DTransformKind::Scale, {.5, .5, .5}}}
	};
	MeshVertex3D v{{0, .1, .2}, {0, 1, 0}, {.3, .7}};
	SourceShape3DRasterVertex native, html;
	REQUIRE(ProjectSourceShape3DVertex(r, v, SourceShape3DFramebufferProfile::NativeTopOrigin, native));
	CHECK(native.Screen.X == Catch::Approx(30));
	CHECK(native.Screen.Y == Catch::Approx(20));
	CHECK(native.ClipDepth == Catch::Approx(.45));
	CHECK(native.TestDepth == Catch::Approx(.45));
	CHECK(native.ViewNormal.X == Catch::Approx(1));
	CHECK(native.ViewNormal.Y == Catch::Approx(0).margin(1e-14));
	CHECK(native.ViewNormal.Z == Catch::Approx(0).margin(1e-14));
	CHECK(native.UV == v.UV);
	REQUIRE(ProjectSourceShape3DVertex(r, v, SourceShape3DFramebufferProfile::HTML5FramebufferRows, html));
	CHECK(html.Screen.X == Catch::Approx(30));
	CHECK(html.Screen.Y == Catch::Approx(60));
	CHECK(html.ClipDepth == Catch::Approx(.45));
	CHECK(html.TestDepth == Catch::Approx(.725));
}

TEST_CASE(
	"Shape 3D projection preserves generated submesh order and bounds allocation", "[imagegraph][shape3d]"
) {
	Node n{"shape", "pc.shape_3_d", "", {}, {}};
	EvaluationRequest request;
	NodeContext c{n, *FindCatalogueEntry(n.Type), request};
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	SourceShape3DRecipe r;
	REQUIRE(BuildSourceShape3DRecipe(c, r));
	MeshValue3D mesh;
	AllocationReservation geometryCharge;
	REQUIRE(BuildSourceShape3DGeometry(c, r, mesh, geometryCharge));
	SourceShape3DProjectionResult output;
	REQUIRE(
		BuildSourceShape3DProjection(c, r, mesh, SourceShape3DFramebufferProfile::NativeTopOrigin, output)
	);
	REQUIRE(output.Triangles.size() == 12);
	for (size_t i = 0; i < output.Triangles.size(); ++i)
		CHECK(output.Triangles[i].Submesh == i / 2);
	CHECK(output.Charge.Bytes() == 12 * sizeof(SourceShape3DRasterTriangle));
	CHECK(c.OutputImages.empty());
	const auto previousScreen = output.Triangles[0].Vertices[0].Screen;
	const auto previousBytes = output.Charge.Bytes();
	EvaluationBudget tightBudget(previousBytes - 1);
	NodeContext limited{n, *FindCatalogueEntry(n.Type), request, tightBudget};
	limited.ByteBudget = Limits::MaximumEvaluationBytes;
	CHECK_FALSE(BuildSourceShape3DProjection(
		limited, r, mesh, SourceShape3DFramebufferProfile::NativeTopOrigin, output
	));
	CHECK(limited.FailureCode == Status::LimitExceeded);
	CHECK(tightBudget.Used() == 0);
	CHECK(output.Triangles[0].Vertices[0].Screen == previousScreen);
	CHECK(output.Charge.Bytes() == previousBytes);
}

TEST_CASE(
	"Shape 3D projection rejects malformed input atomically and defines zero normals", "[imagegraph][shape3d]"
) {
	SourceShape3DRecipe r;
	r.Width = r.Height = 32;
	for (auto &step : r.WorldStack)
		step = {SourceShape3DTransformKind::Translate, {}};
	MeshVertex3D v{{0, 0, 0}, {}, {}};
	SourceShape3DRasterVertex output;
	output.Screen = {7, 9};
	REQUIRE(ProjectSourceShape3DVertex(r, v, SourceShape3DFramebufferProfile::NativeTopOrigin, output));
	CHECK(output.ViewNormal == Vector3{});
	CHECK((output.Screen == Vector2{16, 16}));
	v.Position.X = std::numeric_limits<double>::infinity();
	CHECK_FALSE(ProjectSourceShape3DVertex(r, v, SourceShape3DFramebufferProfile::NativeTopOrigin, output));
	CHECK((output.Screen == Vector2{16, 16}));
	v.Position.X = 0;
	r.Near = 1;
	CHECK_FALSE(ProjectSourceShape3DVertex(r, v, SourceShape3DFramebufferProfile::NativeTopOrigin, output));
	CHECK((output.Screen == Vector2{16, 16}));
}

TEST_CASE(
	"Shape 3D forward normal transform differs from inverse transpose under nonuniform scale",
	"[imagegraph][shape3d]"
) {
	SourceShape3DRecipe r;
	r.Width = r.Height = 32;
	for (auto &step : r.WorldStack)
		step = {SourceShape3DTransformKind::Translate, {}};
	r.WorldStack.back() = {SourceShape3DTransformKind::Scale, {2, 1, .5}};
	SourceShape3DRasterVertex output;
	REQUIRE(ProjectSourceShape3DVertex(
		r, {{0, 0, 0}, {1, 1, 0}, {}}, SourceShape3DFramebufferProfile::NativeTopOrigin, output
	));
	CHECK(output.ViewNormal.X == Catch::Approx(-2 / std::sqrt(5.)));
	CHECK(output.ViewNormal.Y == 0);
	CHECK(output.ViewNormal.Z == Catch::Approx(-1 / std::sqrt(5.)));
}

TEST_CASE(
	"Shape 3D refuses a whole processor of modest spheres before projected allocation",
	"[imagegraph][shape3d]"
) {
	Node n{"shape", "pc.shape_3_d", "", {}, {}};
	EvaluationRequest request;
	NodeContext c{n, *FindCatalogueEntry(n.Type), request};
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	SourceShape3DRecipe r;
	r.Width = r.Height = 32;
	r.Shape = SourceShape3DKind::Sphere;
	MeshValue3D mesh;
	AllocationReservation geometryCharge;
	REQUIRE(BuildSourceShape3DGeometry(c, r, mesh, geometryCharge));
	SourceShape3DProjectionResult output;
	c.ProcessorCount = 4096;
	CHECK_FALSE(
		BuildSourceShape3DProjection(c, r, mesh, SourceShape3DFramebufferProfile::NativeTopOrigin, output)
	);
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(output.Triangles.empty());
	CHECK(output.Charge.Bytes() == 0);
}
