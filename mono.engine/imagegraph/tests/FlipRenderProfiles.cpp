#include "SourceFlipLines.hpp"
#include "SourceFlipSprite.hpp"
#include "SourceFlipSpriteFrames.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_render_profiles")
using namespace engine::imagegraph;
TEST_CASE(
	"Source FLIP line history bridges zero slots and retains mixed-coordinate velocity mapping",
	"[imagegraph]"
) {
	Node node{"render", "pc.flip_render", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	request.Tick = 3;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	FluidDomainData data;
	data.Settings.MaximumParticles = 4;
	data.Settings.Spacing = 2;
	data.SourceParticleCount = int64_t{1};
	data.ReadbackPositions = {12, 8};
	data.ReadbackLife = {0};
	data.History = {{1, {4, 8}}, {2, {0, 0}}, {3, {8, 8}}};
	const Gradient gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}};
	std::vector<std::pair<Vector2, Vector2>> lines;
	REQUIRE(
		detail::VisitSourceFlipLines(
			context,
			data,
			&gradient,
			{0, 0},
			{0, 4},
			0,
			3,
			2,
			[&](Vector2 from, Vector2 to, double thickness, Colour color) {
				CHECK(thickness == 2);
				CHECK(color == Colour{209, 0, 46, 255});
				lines.emplace_back(from, to);
				return true;
			}
		)
	);
	REQUIRE(lines.size() == 2);
	CHECK(lines[0] == std::pair{Vector2{10, 6}, Vector2{6, 6}});
	CHECK(lines[1] == std::pair{Vector2{6, 6}, Vector2{2, 6}});
	data.ReadbackLife = {3};
	data.History[1].Positions = {6, 8};
	lines.clear();
	REQUIRE(
		detail::VisitSourceFlipLines(
			context, data, nullptr, {1, 1}, {0, 4}, 0, 3, 1, [&](Vector2 from, Vector2 to, double, Colour) {
				lines.emplace_back(from, to);
				return true;
			}
		)
	);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == std::pair{Vector2{4, 6}, Vector2{2, 6}});
	request.Subframe = .5;
	lines.clear();
	CHECK_FALSE(
		detail::VisitSourceFlipLines(
			context, data, nullptr, {0, 0}, {0, 4}, 0, 3, 1, [&](Vector2, Vector2, double, Colour) {
				lines.emplace_back();
				return true;
			}
		)
	);
	CHECK(lines.empty());
}
TEST_CASE(
	"Source FLIP sprite frames retain first-frame origins, logical ordering, and packed vertex alpha",
	"[imagegraph]"
) {
	Node node{"render", "pc.flip_render", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	const Image blue{4, 1, {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255}};
	const Image red{2, 2, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}};
	const ImageArray array{{blue, red}, {{size_t{1}}, {size_t{0}}}};
	context.ImageArrays = {{"fluid_particle", &array}};
	detail::SourceFlipSpriteFrames frames;
	REQUIRE(detail::ResolveSourceFlipSpriteFrames(context, frames));
	CHECK(frames.At(0) == &array.Images[1]);
	CHECK(frames.At(1) == &array.Images[0]);
	CHECK(frames.At(2) == &array.Images[1]);
	Image output{8, 8, std::vector<uint8_t>(8 * 8 * 4)};
	uint64_t work = 0;
	REQUIRE(
		detail::DrawSourceFlipSprite(
			context,
			output,
			*frames.At(1),
			{2, 2},
			{4, 4},
			1,
			{255, 255, 255, 255},
			.5,
			false,
			work,
			[&](uint32_t x, uint32_t y, detail::Rgba pixel) { return StoreSurfacePixel(output, x, y, pixel); }
		)
	);
	for (uint32_t x = 3; x < 7; ++x) {
		CHECK(output.Pixels[(3 * 8 + x) * 4 + 2] == 255);
		CHECK(output.Pixels[(3 * 8 + x) * 4 + 3] == 127);
	}
	CHECK(output.Pixels[(3 * 8 + 2) * 4 + 2] == 0);
	CHECK(output.Pixels[(4 * 8 + 3) * 4 + 2] == 0);
	CHECK(detail::SourceFlipSpriteVertexAlpha(.5) == 127 / 255.);
	CHECK(detail::SourceFlipSpriteVertexAlpha(2) == 1);
	CHECK(detail::SourceFlipSpriteVertexAlpha(-1) == 0);
	CHECK(detail::SourceFlipSpriteVertexAlpha(-2147483648. / 255) == 1);
	CHECK(detail::SourceFlipSpriteVertexAlpha(2147483647. / 255) == 1);
	context.Images = {{"fluid_particle", &red}};
	REQUIRE(detail::ResolveSourceFlipSpriteFrames(context, frames));
	CHECK(frames.Single == &red);
	CHECK(frames.Count == 1);
}
