#include "RenderFixture.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.render.portalshadowdomain")

namespace {
	using namespace engine;
	using namespace engine::render;

	std::vector<float> ShadowSamples(Renderer &renderer) {
		const auto image =
			test::CaptureResource(renderer, core::Name("shadow"), 0, 2048, 2048, test::ImageFormat::R32Float);
		std::vector<float> samples;
		samples.reserve(size_t(image.Width) * image.Height);
		for (size_t row = 0; row < image.Height; ++row) {
			core::ByteReader reader(
				std::span(image.Bytes).subspan(row * image.RowStrideBytes, image.Width * 4)
			);
			while (!reader.AtEnd())
				samples.push_back(reader.ReadFloat());
		}
		return samples;
	}
}

TEST_CASE("split shadow casters share the complete native raster domain", "[render][gpu][shadow-domain][.]") {
	const float scale = GENERATE(.37f, 1.f, 3.1f);
	CAPTURE(scale);
	test::FixtureDevice fixture;
	fixture.Initialise();
	std::array<scene::DrawInstance, 2> rows;
	rows[0].Source = 101;
	rows[0].Frame.Position = core::Vector3{-.7f, .2f, -4} * scale;
	rows[0].HalfExtent = core::Vector3{.45f, .7f, .3f} * scale;
	rows[1].Source = 202;
	// The second caster sits outside the eye frustum and still changes the sun fit.
	rows[1].Frame.Position = core::Vector3{8, 1, -6} * scale;
	rows[1].HalfExtent = core::Vector3{.6f, .4f, .5f} * scale;
	for (auto &row : rows)
		row.CastShadow = true;
	SceneTarget target{65, 37};
	View view;
	view.Target = &target;
	view.World = 11;
	view.Instances = rows;
	view.OverrideLighting = true;
	view.Lighting.Direction = core::Vector3{-.8f, -.3f, -.6f}.Unit();
	OverlayImage overlay;
	const auto render = [&] {
		REQUIRE(
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("shadow"))
		);
		return ShadowSamples(fixture.Render);
	};
	const auto native = render();
	view.DirectionalShadowBounds = graph::BoundsOfAll(rows);
	view.Instances = std::span(rows).first(1);
	const auto source = render();
	view.Instances = std::span(rows).last(1);
	const auto body = render();
	REQUIRE(native.size() == source.size());
	REQUIRE(native.size() == body.size());
	size_t mismatches = 0, sourcePixels = 0, bodyPixels = 0, invalid = 0;
	for (size_t pixel = 0; pixel < native.size(); ++pixel) {
		invalid +=
			!std::isfinite(native[pixel]) || !std::isfinite(source[pixel]) || !std::isfinite(body[pixel]);
		sourcePixels += source[pixel] < 1.f;
		bodyPixels += body[pixel] < 1.f;
		mismatches += native[pixel] != std::min(source[pixel], body[pixel]);
	}
	CHECK(invalid == 0);
	CHECK(sourcePixels > 0);
	CHECK(bodyPixels > 0);
	CHECK(mismatches == 0);
	view.Instances = std::span(rows).first(1);
	view.DirectionalShadowBounds.reset();
	const auto separatelyFitted = render();
	CHECK(separatelyFitted != source);
	view.Instances = rows;
	CHECK(render() == native);
}

TEST_CASE("plain shadow casters batch across alternating materials", "[render][gpu][shadow-domain][.]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	const auto addMaterial = [&](const char *name, uint32_t rgba) {
		assets::TextureData texture;
		texture.Width = texture.Height = 1;
		for (size_t channel = 0; channel < 4; channel++) {
			texture.Pixels.push_back(std::byte((rgba >> (channel * 8)) & 255u));
		}
		REQUIRE(fixture.Render.AddTexture(core::Name(name), texture));
	};
	addMaterial("material.first", 0xFF0000FFu);
	addMaterial("material.second", 0xFF00FF00u);
	std::array<scene::DrawInstance, 3> rows;
	for (size_t index = 0; index < rows.size(); index++) {
		rows[index].Source = index + 1;
		rows[index].Frame.Position = {static_cast<float>(index) - 1.0f, 0.0f, -4.0f};
		rows[index].HalfExtent = {.35f, .5f, .3f};
		rows[index].CastShadow = true;
		rows[index].Texture = core::Name("material.first");
	}
	SceneTarget target{65, 37};
	View view;
	view.Target = &target;
	view.Instances = rows;
	view.OverrideLighting = true;
	view.Lighting.Direction = core::Vector3{-.8f, -.3f, -.6f}.Unit();
	OverlayImage overlay;
	const auto render = [&] {
		const FrameResult result = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(result.Ran(core::Name("shadow")));
		return std::pair{result.DrawCalls, ShadowSamples(fixture.Render)};
	};
	const auto uniform = render();
	rows[1].Texture = core::Name("material.second");
	const auto alternating = render();
	// The two opaque material runs remain, while the built-in shadow pass has
	// one draw for all three casters.
	CHECK(alternating.first == uniform.first + 2);
	CHECK(alternating.second == uniform.second);
}

TEST_CASE(
	"a plain caster resets shadow uniforms after a detailed caster", "[render][gpu][shadow-domain][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	std::array<scene::DrawInstance, 2> rows;
	rows[0].Source = 1;
	rows[0].Frame.Position = {-0.7f, 0.0f, -4.0f};
	rows[0].HalfExtent = {.45f, .6f, .3f};
	rows[0].CastShadow = true;
	rows[0].SeamNormal = {1.0f, 0.0f, 0.0f};
	rows[1].Source = 2;
	rows[1].Frame.Position = {0.9f, 0.0f, -4.0f};
	rows[1].HalfExtent = {.4f, .55f, .3f};
	rows[1].CastShadow = true;
	SceneTarget target{65, 37};
	View view;
	view.Target = &target;
	view.Instances = rows;
	view.OverrideLighting = true;
	view.Lighting.Direction = core::Vector3{-.8f, -.3f, -.6f}.Unit();
	OverlayImage overlay;
	const auto render = [&] {
		REQUIRE(
			fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("shadow"))
		);
		return ShadowSamples(fixture.Render);
	};
	const auto combined = render();
	view.DirectionalShadowBounds = graph::BoundsOfAll(rows);
	view.Instances = std::span(rows).first(1);
	const auto detailed = render();
	view.Instances = std::span(rows).last(1);
	const auto plain = render();
	REQUIRE(combined.size() == detailed.size());
	REQUIRE(combined.size() == plain.size());
	size_t mismatches = 0;
	for (size_t pixel = 0; pixel < combined.size(); pixel++) {
		mismatches += combined[pixel] != std::min(detailed[pixel], plain[pixel]);
	}
	CHECK(mismatches == 0);
}

TEST_CASE(
	"invalid shadow domains refuse rendering and leave the next view usable",
	"[render][gpu][shadow-domain][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	scene::DrawInstance row;
	row.Source = 1;
	row.Frame.Position = {0, 0, -4};
	row.HalfExtent = {.5f, .5f, .5f};
	SceneTarget target{33, 21};
	View view;
	view.Target = &target;
	view.Instances = std::span(&row, 1);
	OverlayImage overlay;
	const float infinity = std::numeric_limits<float>::infinity();
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float maximum = std::numeric_limits<float>::max();
	const std::array invalid{
		core::AABB{{nan, -1, -5}, {1, 1, -3}},
		core::AABB{{-1, -1, -5}, {infinity, 1, -3}},
		core::AABB{{1, -1, -5}, {-1, 1, -3}},
		core::AABB{{-maximum, -1, -5}, {maximum, 1, -3}},
		core::AABB{{-.25f, -.25f, -4.25f}, {.25f, .25f, -3.75f}}
	};
	for (size_t index = 0; index < invalid.size(); ++index) {
		CAPTURE(index);
		view.DirectionalShadowBounds = invalid[index];
		const auto result = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK_FALSE(result.Ran(core::Name("shadow")));
		CHECK_FALSE(result.Ran(core::Name("deferred-lighting")));
	}
	view.DirectionalShadowBounds = graph::BoundsOfAll(view.Instances);
	CHECK(fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("shadow")));
}
