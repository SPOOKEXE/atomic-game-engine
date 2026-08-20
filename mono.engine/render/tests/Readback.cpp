#include <engine/render/Readback.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.readback")

using namespace engine::render;

TEST_CASE("BGRA pixels are reduced into channel histograms", "[render][readback]") {
	const std::array<uint32_t, 4> pixels = {
		0xFF000000u,
		0xFF102040u,
		0xFF80A0C0u,
		0xFFFFFFFFu,
	};
	const ImageHistogram histogram = Histogram(pixels);
	CHECK(histogram.Red.Counted == 4);
	CHECK(histogram.Red.Minimum == 0);
	CHECK(histogram.Red.Maximum == 255);
	CHECK(histogram.Green.Minimum == 0);
	CHECK(histogram.Green.Maximum == 255);
	CHECK(histogram.Blue.Minimum == 0);
	CHECK(histogram.Blue.Maximum == 255);
	CHECK(histogram.Alpha.Minimum == 255);
	CHECK(histogram.Alpha.Maximum == 255);
	CHECK(histogram.Alpha.Constant());
	CHECK_FALSE(histogram.Alpha.Blank());
	CHECK_FALSE(histogram.Uniform());
	CHECK(histogram.Red.Buckets[0] == 1);
	CHECK(histogram.Red.Buckets[1] == 1);
	CHECK(histogram.Red.Buckets[8] == 1);
	CHECK(histogram.Red.Buckets[15] == 1);
}

TEST_CASE("empty and uniform images remain distinguishable", "[render][readback]") {
	const ImageHistogram empty = Histogram({});
	CHECK_FALSE(empty.Uniform());
	CHECK_FALSE(empty.Red.Constant());
	CHECK(empty.Red.Counted == 0);

	const std::array<uint32_t, 3> black = {0u, 0u, 0u};
	const ImageHistogram uniform = Histogram(black);
	CHECK(uniform.Uniform());
	CHECK(uniform.Red.Blank());
	CHECK(uniform.Green.Blank());
	CHECK(uniform.Blue.Blank());
	CHECK(uniform.Alpha.Blank());
}

TEST_CASE("RGBA graph targets retain their channel order", "[render][readback]") {
	const std::array<uint32_t, 1> pixels = {0x40302010u};
	const ImageHistogram histogram = HistogramRgba(pixels);
	CHECK(histogram.Red.Minimum == 0x10);
	CHECK(histogram.Green.Minimum == 0x20);
	CHECK(histogram.Blue.Minimum == 0x30);
	CHECK(histogram.Alpha.Minimum == 0x40);
}

TEST_CASE("readback requests never overlap and retain their source frame", "[render][readback]") {
	PendingReadback pending;
	CHECK(pending.CanRequest());
	CHECK_FALSE(pending.HasImage());
	CHECK(pending.Age(10) == 0);

	pending.Submitted(10);
	CHECK_FALSE(pending.CanRequest());
	CHECK_FALSE(pending.Poll(false));
	CHECK_FALSE(pending.HasImage());
	CHECK(pending.Poll(true));
	CHECK(pending.CanRequest());
	CHECK(pending.HasImage());
	CHECK(pending.ImageFrame() == 10);
	CHECK(pending.Age(14) == 4);
	CHECK(pending.Age(9) == 0);
	CHECK_FALSE(pending.Poll(true));

	pending.Clear();
	CHECK(pending.CanRequest());
	CHECK_FALSE(pending.HasImage());
	CHECK(pending.ImageFrame() == 0);
}

TEST_CASE("renderer inspection state is usable before a device exists", "[render][readback]") {
	Renderer renderer;
	const engine::core::Name colour("colour");
	renderer.Inspect(colour);
	CHECK(renderer.Inspecting() == colour);
	CHECK_FALSE(renderer.Readback().IsValid());
	CHECK(renderer.PassTimings().empty());
	CHECK(renderer.PassWallTimes().empty());
	CHECK_FALSE(renderer.Timed());
	renderer.Inspect({});
	CHECK_FALSE(renderer.Inspecting().IsValid());
}
