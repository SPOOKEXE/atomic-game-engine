#include "../src/nodes/SourceShape3DRaster.hpp"

#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_raster")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

namespace {
	struct RasterHarness {
		Node Authored{"shape", "pc.shape_3_d", "", {}, {}};
		EvaluationRequest Request;
		const CatalogueEntry *Entry = FindCatalogueEntry("pc.shape_3_d");
		NodeContext Context{Authored, *Entry, Request};
		std::array<ElementValue, 1> Palette{Colour{255, 0, 0, 128}};
		SourceShape3DRecipe Recipe;
		SourceShape3DRasterResult Result;
		RasterHarness() {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			Recipe.Width = Recipe.Height = 2;
			Recipe.Colours = Palette;
			Recipe.Interpolation = 1;
			Recipe.Oversample = 4;
		}
		SourceShape3DRasterTriangle Triangle(double depth = .5) {
			SourceShape3DRasterTriangle triangle;
			triangle.Vertices = {
				{{{0, 0}, {0, 0}, .125, depth, {0, 0, 1}},
				 {{2, 0}, {1, 0}, .125, depth, {0, 0, 1}},
				 {{0, 2}, {0, 1}, .125, depth, {0, 0, 1}}}
			};
			return triangle;
		}
		bool Draw(std::span<const SourceShape3DRasterTriangle> triangles, const Image *background = nullptr) {
			return RasterSourceShape3D(
				Context, Recipe, triangles, {}, background, SurfaceFormat::RGBA32Float, Result
			);
		}
	};
}

TEST_CASE("Shape 3D projected raster writes independent shader attachments", "[imagegraph][shape3d]") {
	RasterHarness harness;
	const std::array triangles{harness.Triangle()};
	REQUIRE(harness.Draw(triangles));
	const auto colour = ReadPixel(harness.Result.Images[0], 0, 0);
	const auto depth = ReadPixel(harness.Result.Images[1], 0, 0);
	const auto rim = ReadPixel(harness.Result.Images[2], 0, 0);
	CHECK(colour[0] == 1);
	CHECK(colour[1] == 0);
	CHECK(colour[3] < .51);
	CHECK(depth[0] == .5);
	CHECK(depth[3] == 1);
	CHECK(rim[0] == 1);
	CHECK(rim[3] == 1);
	CHECK(ReadPixel(harness.Result.Images[0], 1, 1)[3] == 0);
	CHECK(harness.Context.OutputImages.empty());
	CHECK(harness.Result.Charge.Bytes() == 3 * 2 * 2 * 16);
}

TEST_CASE("Shape 3D projected raster culls winding and keeps nearest depth", "[imagegraph][shape3d]") {
	RasterHarness harness;
	auto near = harness.Triangle(.2), far = harness.Triangle(.8);
	for (auto &vertex : far.Vertices)
		vertex.Tint = {0, 255, 0, 255};
	const std::array triangles{near, far};
	REQUIRE(harness.Draw(triangles));
	CHECK(ReadPixel(harness.Result.Images[0], 0, 0)[0] == 1);
	std::swap(near.Vertices[1], near.Vertices[2]);
	const std::array reversed{near};
	REQUIRE(harness.Draw(reversed));
	CHECK(ReadPixel(harness.Result.Images[0], 0, 0)[3] == 0);
}

TEST_CASE("Shape 3D projected raster refuses invalid normals atomically", "[imagegraph][shape3d]") {
	RasterHarness harness;
	const std::array valid{harness.Triangle()};
	REQUIRE(harness.Draw(valid));
	const auto previous = harness.Result.Images;
	auto invalid = harness.Triangle();
	invalid.Vertices[0].ViewNormal = {2, 0, 0};
	const std::array triangles{invalid};
	CHECK_FALSE(harness.Draw(triangles));
	CHECK(harness.Context.FailureCode == Status::InvalidValue);
	CHECK(harness.Result.Images == previous);
}

TEST_CASE(
	"Shape 3D projected raster charges whole processor work before allocation", "[imagegraph][shape3d]"
) {
	RasterHarness harness;
	harness.Context.ProcessorCount = 64000000;
	const std::array triangles{harness.Triangle()};
	const auto before = harness.Context.AllocationBudget().Used();
	CHECK_FALSE(harness.Draw(triangles));
	CHECK(harness.Context.FailureCode == Status::LimitExceeded);
	CHECK(harness.Context.AllocationBudget().Used() == before);
	CHECK(harness.Result.Images[0].Pixels.empty());
}

TEST_CASE("Shape 3D background affects only surface attachment", "[imagegraph][shape3d]") {
	RasterHarness harness;
	Image background;
	background.Width = background.Height = 1;
	background.Pixels = {0, 0, 255, 255};
	const std::array triangles{harness.Triangle()};
	REQUIRE(harness.Draw(triangles, &background));
	const auto surface = ReadPixel(harness.Result.Images[0], 0, 0);
	CHECK(surface[0] > .49);
	CHECK(surface[2] > .49);
	CHECK(surface[3] == 1);
	CHECK(ReadPixel(harness.Result.Images[0], 1, 1)[2] == 1);
	CHECK(ReadPixel(harness.Result.Images[1], 1, 1)[3] == 0);
	CHECK(ReadPixel(harness.Result.Images[2], 1, 1)[3] == 0);
}

TEST_CASE("Shape 3D zero texture alpha still writes hardware depth", "[imagegraph][shape3d]") {
	RasterHarness harness;
	Image transparent, opaque;
	transparent.Width = transparent.Height = opaque.Width = opaque.Height = 1;
	transparent.Pixels = {255, 255, 255, 0};
	opaque.Pixels = {255, 255, 255, 255};
	auto near = harness.Triangle(.1), far = harness.Triangle(.9);
	far.Submesh = 1;
	const std::array triangles{near, far};
	const std::array<const Image *, 2> textures{&transparent, &opaque};
	REQUIRE(RasterSourceShape3D(
		harness.Context,
		harness.Recipe,
		triangles,
		textures,
		nullptr,
		SurfaceFormat::RGBA32Float,
		harness.Result
	));
	CHECK(ReadPixel(harness.Result.Images[0], 0, 0)[3] == 0);
	CHECK(ReadPixel(harness.Result.Images[1], 0, 0)[3] == 0);
}

TEST_CASE(
	"Shape 3D interpolates vertex-normalized normals without fragment normalization", "[imagegraph][shape3d]"
) {
	RasterHarness harness;
	auto triangle = harness.Triangle();
	triangle.Vertices[1].ViewNormal = {1, 0, 0};
	triangle.Vertices[2].ViewNormal = {0, 1, 0};
	const std::array triangles{triangle};
	REQUIRE(harness.Draw(triangles));
	CHECK(ReadPixel(harness.Result.Images[2], 0, 0)[0] == .5);
}

TEST_CASE("Shape 3D admits repeated texture validation before scanning samples", "[imagegraph][shape3d]") {
	RasterHarness harness;
	const std::array triangles{harness.Triangle()};
	REQUIRE(harness.Draw(triangles));
	const auto previous = harness.Result.Images;
	const auto previousCharge = harness.Result.Charge.Bytes();
	const auto previousUsed = harness.Context.AllocationBudget().Used();
	Image texture;
	texture.Width = texture.Height = 1024;
	texture.Format = SurfaceFormat::RGBA32Float;
	texture.Pixels.resize(size_t(texture.Width) * texture.Height * 16);
	// Nonfinite sample distinguishes admission-first refusal from a premature scan.
	texture.Pixels[2] = 0xc0;
	texture.Pixels[3] = 0x7f;
	std::array<const Image *, 17> textures;
	textures.fill(&texture);
	CHECK_FALSE(RasterSourceShape3D(
		harness.Context,
		harness.Recipe,
		triangles,
		textures,
		nullptr,
		SurfaceFormat::RGBA32Float,
		harness.Result
	));
	CHECK(harness.Context.FailureCode == Status::LimitExceeded);
	CHECK(harness.Result.Images == previous);
	CHECK(harness.Result.Charge.Bytes() == previousCharge);
	CHECK(harness.Context.AllocationBudget().Used() == previousUsed);
}

TEST_CASE("Shape 3D admits background validation for the whole processor batch", "[imagegraph][shape3d]") {
	RasterHarness harness;
	harness.Context.ProcessorCount = 1000000;
	Image background;
	background.Width = background.Height = 8;
	background.Format = SurfaceFormat::RGBA32Float;
	background.Pixels.resize(8 * 8 * 16);
	background.Pixels[2] = 0xc0;
	background.Pixels[3] = 0x7f;
	const std::array triangles{harness.Triangle()};
	CHECK_FALSE(harness.Draw(triangles, &background));
	CHECK(harness.Context.FailureCode == Status::LimitExceeded);
	CHECK(harness.Result.Images[0].Pixels.empty());
	CHECK(harness.Result.Charge.Bytes() == 0);
}
