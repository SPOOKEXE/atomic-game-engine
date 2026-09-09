#include "RenderFixture.hpp"
#include "RenderTypes.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/ResourceImage.hpp>
#include <engine/testing/Suite.hpp>

#include <array>

TEST_SUITE_ID("engine.render.portalshadowimage")

namespace {
	using namespace engine;
	using namespace engine::render;
	const core::Name PIPELINE("shadow-image-oracle"), SHADOW_CAPTURE("capture-shadow"),
		COLOUR_CAPTURE("capture-colour");

	void Install(Renderer &renderer) {
		using namespace graph;
		PipelineDocument document;
		const auto appendShadow = [&] {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = SHADOW_CAPTURE,
				 .NodeKind = core::Name("shadow-capture"),
				 .Scope = NodeScope::View}
			);
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name("shadow"), .Key = core::Name("shadow")}
			);
		};
		bool haveView = false, inserted = false;
		const auto base = DefaultPbrDocument();
		for (const auto &edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode) {
				if (edit.Scope == NodeScope::View) haveView = true;
				if (haveView && edit.Scope == NodeScope::Frame && !inserted) {
					appendShadow();
					inserted = true;
				}
			}
			document.Record(edit);
		}
		REQUIRE(inserted);
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = COLOUR_CAPTURE,
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		document.Record({.Kind = EditKind::Reads, .Target = core::Name("lit"), .Key = core::Name("source")});
		RenderGraph graph;
		core::Name offender;
		REQUIRE(Build(document, graph, offender) == PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(PIPELINE, graph));
	}

	ResourceImage Take(Renderer &renderer, uint64_t token) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (auto image = renderer.TakeResourceImage(token)) return std::move(*image);
			SDL_Delay(1);
		}
		FAIL("shadow image capture timed out");
		return {};
	}

	View MakeView(const SceneTarget &target, std::span<const scene::DrawInstance> rows, size_t slot = 0) {
		View view;
		view.Target = &target;
		view.Pipeline = PIPELINE;
		view.World = slot + 17;
		view.Slot = slot;
		view.Instances = rows;
		view.OverrideLighting = true;
		view.Lighting.Direction = core::Vector3{.4f, -1, -.2f}.Unit();
		return view;
	}

	void CheckMap(const ResourceImage &image) {
		REQUIRE(image.Status == ResourceImageStatus::Ok);
		CHECK(image.Kind == ResourceImageKind::DirectionalShadow);
		CHECK(image.Resource == core::Name("shadow"));
		CHECK(image.DepthResource == image.Resource);
		CHECK(image.Width == SHADOW_RESOLUTION);
		CHECK(image.Height == SHADOW_RESOLUTION);
		CHECK(image.RowStride == SHADOW_RESOLUTION * 4);
		CHECK(image.CaptureFrame != 0);
		REQUIRE(image.Depth.size() == size_t(SHADOW_RESOLUTION) * SHADOW_RESOLUTION * 4);
		CHECK(image.Pixels.empty());
		CHECK(image.Normal.empty());
		CHECK(image.AmbientResponse.empty());
		CHECK(image.LightingBaseline.empty());
		CHECK(image.DirectionalResponse.empty());
		REQUIRE(image.Shadow.has_value());
	}
	double ShadowClears() {
		for (const auto &counter : core::Metrics::Snapshot().Counters)
			if (counter.Name == core::Name("render.shadow.clears")) return counter.Value;
		return 0;
	}
}

TEST_CASE(
	"native shadow capture owns exact depth and its producing domain", "[render][gpu][shadow-image][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	SceneTarget target{33, 29};
	std::array<scene::DrawInstance, 1> rows;
	rows[0].Frame.Position = {0, 0, -4};
	rows[0].HalfExtent = {.6f, .9f, .4f};
	rows[0].CastShadow = true;
	auto view = MakeView(target, rows);
	view.DirectionalShadowBounds = core::AABB{{-3, -3, -8}, {4, 5, 2}};
	const std::array nodes{SHADOW_CAPTURE, COLOUR_CAPTURE};
	std::array<uint64_t, 2> tokens{};
	REQUIRE(renderer.QueueResourceImages(PIPELINE, nodes, 0, ResourceImageDelivery::CopiedPixels, tokens));
	OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("shadow")));
	const auto captured = Take(renderer, tokens[0]);
	const auto colour = Take(renderer, tokens[1]);
	CheckMap(captured);
	REQUIRE(colour.Status == ResourceImageStatus::Ok);
	CHECK(captured.CaptureFrame == colour.CaptureFrame);
	CHECK(colour.Kind == ResourceImageKind::Colour);
	CHECK_FALSE(colour.Shadow.has_value());
	const auto expected = test::CaptureResource(
		renderer, core::Name("shadow"), 0, SHADOW_RESOLUTION, SHADOW_RESOLUTION, test::ImageFormat::R32Float
	);
	CHECK(captured.Depth == expected.Bytes);
	const auto bounds = graph::BoundsOfAll(rows);
	CHECK(captured.Shadow->SourceBounds.Minimum == bounds.Minimum);
	CHECK(captured.Shadow->SourceBounds.Maximum == bounds.Maximum);
	CHECK(captured.Shadow->DomainBounds.Minimum == view.DirectionalShadowBounds->Minimum);
	CHECK(captured.Shadow->DomainBounds.Maximum == view.DirectionalShadowBounds->Maximum);
	const auto matrix = graph::FitDirectionalLight(*view.DirectionalShadowBounds, view.Lighting.Direction);
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			CHECK(captured.Shadow->LightViewProjection[column * 4 + row] == matrix[column][row]);
	core::ByteReader depths(captured.Depth);
	size_t written = 0, invalid = 0;
	while (!depths.AtEnd()) {
		const float sample = depths.ReadFloat();
		invalid += !std::isfinite(sample) || sample < 0 || sample > 1;
		written += sample < 1;
	}
	CHECK(invalid == 0);
	CHECK(written > 1000);
	CHECK(renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE, 0, ResourceImageDelivery::Resident) == 0);
	const auto cancelled = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(cancelled != 0);
	CHECK(renderer.CancelResourceImage(cancelled));
	CHECK_FALSE(renderer.TakeResourceImage(cancelled).has_value());
	const auto submitted = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(submitted != 0);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(renderer.CancelResourceImage(submitted));
	const auto following = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(following != 0);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckMap(Take(renderer, following));
	CHECK_FALSE(renderer.TakeResourceImage(submitted).has_value());
	CHECK(captured.Depth == expected.Bytes);
}

TEST_CASE("empty source shadow capture clears depth only when requested", "[render][gpu][shadow-image][.]") {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	SceneTarget target{33, 29};
	auto view = MakeView(target, {});
	OverlayImage overlay;
	const auto before = ShadowClears();
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(ShadowClears() == before);
	const auto token = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(token != 0);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(core::Name("shadow")));
	const auto captured = Take(renderer, token);
	CheckMap(captured);
	CHECK(ShadowClears() == before + 1);
	core::ByteReader depths(captured.Depth);
	size_t nonClear = 0;
	while (!depths.AtEnd())
		nonClear += depths.ReadFloat() != 1.f;
	CHECK(nonClear == 0);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CHECK(ShadowClears() == before + 1);
}

TEST_CASE(
	"shadow readback refuses a third full map before exceeding staging capacity",
	"[render][gpu][shadow-image][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	SceneTarget target{33, 29};
	auto view = MakeView(target, {});
	const std::array nodes{SHADOW_CAPTURE, SHADOW_CAPTURE, SHADOW_CAPTURE};
	std::array<uint64_t, 3> tokens{};
	REQUIRE(renderer.QueueResourceImages(PIPELINE, nodes, 0, ResourceImageDelivery::CopiedPixels, tokens));
	OverlayImage overlay;
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	size_t accepted = 0, refused = 0;
	for (const auto token : tokens) {
		const auto image = Take(renderer, token);
		if (image.Status == ResourceImageStatus::Ok) {
			CheckMap(image);
			++accepted;
		} else {
			CHECK(image.Status == ResourceImageStatus::Failed);
			CHECK(image.Depth.empty());
			++refused;
		}
	}
	CHECK(accepted == 2);
	CHECK(refused == 1);
	const auto retry = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(retry != 0);
	renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	CheckMap(Take(renderer, retry));
}

TEST_CASE(
	"batched worlds capture their own shadow bytes before the shared target changes",
	"[render][gpu][shadow-image][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	SceneTarget target{33, 29};
	std::array<scene::DrawInstance, 2> rows;
	rows[0].Frame.Position = {0, 0, -4};
	rows[0].HalfExtent = {.4f, .8f, .3f};
	rows[1].Frame.Position = {1, 0, -5};
	rows[1].HalfExtent = {.8f, .2f, .6f};
	std::array<View, 2> views{
		MakeView(target, std::span(rows).first(1)), MakeView(target, std::span(rows).last(1), 1)
	};
	views[1].Lighting.Direction = core::Vector3{-.7f, -.2f, -.3f}.Unit();
	OverlayImage overlay;
	std::array<std::vector<std::byte>, 2> expected;
	for (size_t index = 0; index < views.size(); ++index) {
		const auto token = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE, index);
		REQUIRE(token != 0);
		renderer.Render(std::span(&views[index], 1), overlay, nullptr, false);
		expected[index] = Take(renderer, token).Depth;
	}
	REQUIRE(expected[0] != expected[1]);
	std::array<uint64_t, 2> tokens;
	for (size_t index = 0; index < views.size(); ++index) {
		tokens[index] = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE, index);
		REQUIRE(tokens[index] != 0);
	}
	renderer.Render(views, overlay, nullptr, false);
	for (size_t index = 0; index < views.size(); ++index) {
		const auto image = Take(renderer, tokens[index]);
		CheckMap(image);
		CHECK(image.Depth == expected[index]);
		const auto bounds = graph::BoundsOfAll(views[index].Instances);
		CHECK(image.Shadow->SourceBounds.Minimum == bounds.Minimum);
		CHECK(image.Shadow->SourceBounds.Maximum == bounds.Maximum);
	}
}

TEST_CASE(
	"an explicit shadow domain cannot leak into the next ordinary view", "[render][gpu][shadow-image][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	Install(renderer);
	SceneTarget target{33, 29};
	scene::DrawInstance row;
	row.Frame.Position = {0, 0, -4};
	row.HalfExtent = {.4f, .8f, .3f};
	std::array<View, 2> views{MakeView(target, std::span(&row, 1)), MakeView(target, std::span(&row, 1), 1)};
	views[1].World = views[0].World;
	views[0].DirectionalShadowBounds = core::AABB{{-3, -3, -8}, {4, 5, 2}};
	OverlayImage overlay;
	renderer.Render(std::span(&views[1], 1), overlay, nullptr, false);
	const auto expected = test::CaptureResource(
		renderer, core::Name("shadow"), 1, SHADOW_RESOLUTION, SHADOW_RESOLUTION, test::ImageFormat::R32Float
	);
	const auto token = renderer.QueueResourceImage(PIPELINE, SHADOW_CAPTURE);
	REQUIRE(token != 0);
	const auto clears = ShadowClears();
	renderer.Render(views, overlay, nullptr, false);
	CHECK(ShadowClears() == clears + 2);
	const auto captured = Take(renderer, token);
	CheckMap(captured);
	CHECK(captured.Depth != expected.Bytes);
	const auto ordinary = test::CaptureResource(
		renderer, core::Name("shadow"), 1, SHADOW_RESOLUTION, SHADOW_RESOLUTION, test::ImageFormat::R32Float
	);
	CHECK(ordinary.Bytes == expected.Bytes);
}
