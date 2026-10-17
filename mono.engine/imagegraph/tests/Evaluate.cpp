#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.imagegraph.evaluate")
TEST_DEPENDS("engine.imagegraph.document")

using namespace engine::imagegraph;

namespace {
	using Pixel = std::array<uint8_t, 4>;
	constexpr Pixel RED{255, 0, 0, 255}, GREEN{0, 255, 0, 255}, BLUE{0, 0, 255, 255},
		WHITE{255, 255, 255, 255}, CLEAR{};
	Image Pixels(uint32_t width, uint32_t height, std::initializer_list<Pixel> pixels) {
		Image image{width, height, {}};
		for (const auto &pixel : pixels)
			for (uint8_t channel : pixel)
				image.Pixels.push_back(std::byte{channel});
		REQUIRE(image.IsValid());
		return image;
	}
	Image Asymmetric() {
		return Pixels(2, 2, {RED, GREEN, BLUE, WHITE});
	}
	Image Run(const Operation &operation, Image source = Asymmetric()) {
		Document document{
			.Nodes = {{"source", Source{"source.png"}, {}, {}}, {"result", operation, {"source"}}},
			.Outputs = {{"main", "result"}}
		};
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic));
		Image result;
		REQUIRE(Evaluate(
			document,
			plan,
			{},
			[&](std::string_view path, Image &image, std::string &) {
				CHECK(path == "source.png");
				image = source;
				return true;
			},
			result,
			diagnostic
		));
		return result;
	}
	void Same(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Pixels == expected.Pixels);
	}
}

TEST_CASE("image validity requires exact bounded RGBA8 bytes", "[imagegraph][pixels]") {
	CHECK_FALSE(Image{}.IsValid());
	Image image = Asymmetric();
	image.Pixels.pop_back();
	CHECK_FALSE(image.IsValid());
	image.Width = Limits::MaximumDimension + 1;
	CHECK_FALSE(image.IsValid());
}

TEST_CASE("source selected by durable output returns unchanged asymmetric pixels", "[imagegraph][pixels]") {
	Document document{
		.Nodes = {{"source", Source{"source.png"}, {}, {}}, {"unused", Source{"missing.png"}, {}, {}}},
		.Outputs = {{"original", "source"}}
	};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic));
	Image image;
	size_t calls = 0;
	REQUIRE(Evaluate(
		document,
		plan,
		"original",
		[&](std::string_view path, Image &out, std::string &) {
			CHECK(path == "source.png");
			calls++;
			out = Asymmetric();
			return true;
		},
		image,
		diagnostic
	));
	CHECK(calls == 1);
	Same(image, Asymmetric());
}

TEST_CASE("flips crop and nearest resize have literal pixel layouts", "[imagegraph][pixels]") {
	Same(Run(Flip{true, false}), Pixels(2, 2, {GREEN, RED, WHITE, BLUE}));
	Same(Run(Flip{false, true}), Pixels(2, 2, {BLUE, WHITE, RED, GREEN}));
	Same(Run(Flip{true, true}), Pixels(2, 2, {WHITE, BLUE, GREEN, RED}));
	Same(Run(Crop{1, 0, 1, 2}), Pixels(1, 2, {GREEN, WHITE}));
	Same(Run(Crop{-1, -1, 2, 2}), Pixels(2, 2, {CLEAR, CLEAR, CLEAR, RED}));
	Same(Run(Resize{4, 2}), Pixels(4, 2, {RED, RED, GREEN, GREEN, BLUE, BLUE, WHITE, WHITE}));
	Same(Run(Resize{1, 1}), Pixels(1, 1, {WHITE}));
	Same(Run(Resize{1, 1, Sampling::Bilinear}), Pixels(1, 1, {{128, 128, 128, 255}}));
	Same(
		Run(Resize{1, 1, Sampling::Bilinear}, Pixels(2, 1, {{255, 0, 0, 0}, BLUE})),
		Pixels(1, 1, {{0, 0, 255, 128}})
	);
}

TEST_CASE(
	"affine image canvas uses clockwise rotation pixel centers and explicit pivot", "[imagegraph][pixels]"
) {
	Same(Run(Transform{2, 2}), Asymmetric());
	Same(Run(Transform{2, 2, 1, 0}), Pixels(2, 2, {CLEAR, RED, CLEAR, BLUE}));
	Same(Run(Transform{2, 2, 0, 0, 1, 1, 90, 1, 1}), Pixels(2, 2, {BLUE, RED, WHITE, GREEN}));
	Same(Run(Transform{2, 2, 0, 0, -1, 1, 0, 1, 1}), Pixels(2, 2, {GREEN, RED, WHITE, BLUE}));
	Same(Run(Transform{4, 2, 0, 0, 2, 1}), Pixels(4, 2, {RED, RED, GREEN, GREEN, BLUE, BLUE, WHITE, WHITE}));
	Same(Run(Transform{2, 2, 100, -100}), Pixels(2, 2, {CLEAR, CLEAR, CLEAR, CLEAR}));
	Same(Run(Transform{2, 2, 0, 0, 1, 1, 360, 1, 1}), Asymmetric());
	Same(Run(Transform{2, 2, 0, 0, 1, 1, 0, 1, 1, Sampling::Bilinear}), Asymmetric());
}

TEST_CASE(
	"normal blend returns straight RGBA8 alpha and removes transparent hidden colour", "[imagegraph][pixels]"
) {
	Document document{
		.Nodes =
			{{"back", Source{"back.png"}, {}, {}},
			 {"front", Source{"front.png"}, {}, {}},
			 {"blend", Blend{}, {"back", "front"}, {}}},
		.Outputs = {{"main", "blend"}}
	};
	const Image back = Pixels(2, 1, {{255, 0, 0, 128}, {255, 0, 0, 0}});
	const Image front = Pixels(2, 1, {{0, 0, 255, 128}, {0, 255, 0, 0}});
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic));
	const SourceResolver sources = [&](std::string_view path, Image &image, std::string &) {
		image = path == "back.png" ? back : front;
		return true;
	};
	Image result;
	REQUIRE(Evaluate(document, plan, {}, sources, result, diagnostic));
	Same(result, Pixels(2, 1, {{85, 0, 170, 192}, CLEAR}));
	std::get<Blend>(document.Nodes[2].Value).Opacity = 0;
	REQUIRE(Evaluate(document, plan, {}, sources, result, diagnostic));
	Same(result, Pixels(2, 1, {{255, 0, 0, 128}, CLEAR}));
}

TEST_CASE(
	"shared image source runs once and graph serialization preserves evaluated output", "[imagegraph][pixels]"
) {
	Document document{
		.Nodes =
			{{"source", Source{"source.png"}, {}, {}},
			 {"left", Flip{true, false}, {"source"}, {}},
			 {"right", Flip{false, true}, {"source"}, {}},
			 {"blend", Blend{}, {"left", "right"}, {}}},
		.Outputs = {{"main", "blend"}, {"raw", "source"}}
	};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic));
	size_t calls = 0;
	SourceResolver sources = [&](std::string_view, Image &image, std::string &) {
		calls++;
		image = Asymmetric();
		return true;
	};
	Image result;
	REQUIRE(Evaluate(document, plan, "main", sources, result, diagnostic));
	CHECK(calls == 1);
	Same(result, Pixels(2, 2, {BLUE, WHITE, RED, GREEN}));
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	Document decoded;
	REQUIRE(Read(encoded, decoded, diagnostic));
	Plan restored;
	REQUIRE(Compile(decoded, restored, diagnostic));
	Image reopened;
	REQUIRE(Evaluate(decoded, restored, "main", sources, reopened, diagnostic));
	Same(reopened, result);
}

TEST_CASE(
	"evaluation refuses invalid sources selection or plan while preserving accepted pixels",
	"[imagegraph][pixels]"
) {
	Document document{.Nodes = {{"source", Source{"source.png"}, {}, {}}}, .Outputs = {{"main", "source"}}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic));
	Image output = Asymmetric();
	const auto refused = [&](std::string_view selection, const SourceResolver &sources) {
		CHECK_FALSE(Evaluate(document, plan, selection, sources, output, diagnostic));
		Same(output, Asymmetric());
	};
	SECTION("missing resolver") {
		refused({}, {});
	}
	SECTION("missing source") {
		refused({}, [](std::string_view, Image &, std::string &failure) {
			failure = "missing source";
			return false;
		});
		CHECK(diagnostic.Node == "source");
	}
	SECTION("inconsistent source") {
		refused({}, [](std::string_view, Image &out, std::string &) {
			out = {2, 2, {std::byte{1}}};
			return true;
		});
	}
	SECTION("oversized source") {
		refused({}, [](std::string_view, Image &out, std::string &) {
			out.Width = Limits::MaximumDimension + 1;
			out.Height = 1;
			return true;
		});
	}
	SECTION("resolver throws") {
		refused({}, [](std::string_view, Image &, std::string &) -> bool {
			throw std::runtime_error("decode refused");
		});
		CHECK(diagnostic.Node == "source");
	}
	SECTION("missing output") {
		refused("missing", {});
	}
	SECTION("ambiguous output") {
		document.Outputs.push_back({"second", "source"});
		REQUIRE(Compile(document, plan, diagnostic));
		refused({}, {});
	}
	SECTION("invalid plan") {
		plan.Order[0] = 100;
		refused({}, {});
	}
}

TEST_CASE(
	"blend mismatch and declared memory or pixel work are refused before allocation", "[imagegraph][pixels]"
) {
	Diagnostic diagnostic;
	Plan plan;
	Image output = Asymmetric();
	Document document;
	SECTION("mismatched source canvases") {
		document = {
			.Nodes =
				{{"source", Source{"source.png"}, {}, {}},
				 {"solid", Solid{}, {}, {}},
				 {"blend", Blend{}, {"source", "solid"}, {}}},
			.Outputs = {{"main", "blend"}}
		};
		REQUIRE(Compile(document, plan, diagnostic));
		CHECK_FALSE(Evaluate(
			document,
			plan,
			{},
			[](std::string_view, Image &out, std::string &) {
				out = Asymmetric();
				return true;
			},
			output,
			diagnostic
		));
		CHECK(diagnostic.Node == "blend");
		CHECK(diagnostic.Message.find("matching") != std::string::npos);
	}
	SECTION("pixel work") {
		document = {
			.Nodes =
				{{"solid", Solid{}, {}, {}},
				 {"resize", Resize{4096, 4096, Sampling::Bilinear}, {"solid"}, {}}},
			.Outputs = {{"main", "resize"}}
		};
		REQUIRE(Compile(document, plan, diagnostic));
		CHECK_FALSE(Evaluate(document, plan, {}, {}, output, diagnostic));
		CHECK(diagnostic.Message.find("pixel work") != std::string::npos);
	}
	SECTION("retained bytes") {
		document = {.Nodes = {{"solid", Solid{4096, 4096}, {}, {}}}, .Outputs = {{"main", "flip4"}}};
		for (size_t index = 1; index <= 4; index++)
			document.Nodes.push_back(
				{"flip" + std::to_string(index),
				 Flip{},
				 {index == 1 ? "solid" : "flip" + std::to_string(index - 1)},
				 {}}
			);
		REQUIRE(Compile(document, plan, diagnostic));
		CHECK_FALSE(Evaluate(document, plan, {}, {}, output, diagnostic));
		CHECK(diagnostic.Message.find("retained") != std::string::npos);
	}
	Same(output, Asymmetric());
}
