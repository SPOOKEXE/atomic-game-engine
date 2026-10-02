#include "SourceRigidFracture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.rigid_fracture_kernel")
using namespace engine::imagegraph;
TEST_CASE("Source fracture fixed region passes create owned separate convex pieces", "[rigid]") {
	Image base{10, 5, std::vector<uint8_t>(10 * 5 * 4)}, map = base;
	for (uint32_t y = 0; y < 5; ++y)
		for (uint32_t x = 0; x < 10; ++x) {
			REQUIRE(StoreSurfacePixel(base, x, y, {.25, .5, 1, 1}));
			REQUIRE(
				StoreSurfacePixel(map, x, y, x < 5 ? SurfacePixel{1, 0, 0, 1} : SurfacePixel{0, 1, 0, 1})
			);
		}
	std::vector<detail::RigidFracturePiece> pieces;
	Diagnostic diagnostic;
	const auto status =
		detail::SourceRigidFracture(base, map, {0, 0}, Limits::MaximumEvaluationBytes, pieces, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(pieces.size() == 2);
	CHECK(pieces[0].Origin == Vector2{0, 0});
	CHECK(pieces[1].Origin == Vector2{5, 0});
	for (const auto &piece : pieces) {
		CHECK(piece.Texture.Width == 5);
		CHECK(piece.Texture.Height == 5);
		REQUIRE(piece.Points.size() >= 3);
		CHECK(piece.Points.size() <= 8);
		CHECK(piece.Texture.Pixels.size() == 100);
	}
	CHECK(pieces[0].Texture.Pixels == pieces[1].Texture.Pixels);
	const auto retained = pieces;
	CHECK(detail::SourceRigidFracture(base, map, {0, 0}, 1, pieces, diagnostic) == Status::LimitExceeded);
	REQUIRE(pieces.size() == retained.size());
	CHECK(pieces[0].Texture == retained[0].Texture);
	auto blank = map;
	std::fill(blank.Pixels.begin(), blank.Pixels.end(), 0);
	REQUIRE(
		detail::SourceRigidFracture(
			base, blank, {0, 0}, Limits::MaximumEvaluationBytes, pieces, diagnostic
		) == Status::Ok
	);
	CHECK(pieces.empty());
}
