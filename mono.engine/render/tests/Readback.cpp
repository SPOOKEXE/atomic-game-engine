// The arithmetic behind stage 8, exercised without a GPU.
//
// **What this is for**: three of the eleven faults in §1.5 of
// `docs/PIPELINE_NODES.md` need pixels rather than the authored graph — "is
// this alpha blank" (3), "is this target a blank image" (4), and overdraw (9).
// The first two are a reduction over a downloaded image, and a reduction is
// arithmetic; it belongs where a suite can reach it.
//
// **And the download policy is checked here too**, which matters more than it
// looks. The rule is *never stall, and say how stale the picture is* — the
// opposite of what `--capture` does — and a policy with no test is a policy that
// quietly becomes "stall sometimes" the first time somebody debugs a blank
// panel.

#include <engine/render/Readback.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.render.readback")

using engine::render::ChannelHistogram;
using engine::render::Histogram;
using engine::render::HISTOGRAM_BUCKETS;
using engine::render::ImageHistogram;
using engine::render::PendingReadback;

namespace {
	// One pixel, in the byte order a downloaded scene target has.
	uint32_t Pixel(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
		return static_cast<uint32_t>(blue) | (static_cast<uint32_t>(green) << 8) |
			   (static_cast<uint32_t>(red) << 16) | (static_cast<uint32_t>(alpha) << 24);
	}

	uint32_t Total(const ChannelHistogram &channel) {
		uint32_t sum = 0;
		for (const uint32_t bucket : channel.Buckets) {
			sum += bucket;
		}
		return sum;
	}
}

TEST_CASE("a histogram separates the channels it was given", "[render]") {
	// **The case that would fail silently if the byte order were wrong.** Four
	// distinct values, one per channel: a red histogram built from the blue
	// bytes still has the right *shape*, so only distinct values catch it.
	const std::vector<uint32_t> pixels{Pixel(10, 70, 130, 200)};
	const ImageHistogram image = Histogram(pixels);

	CHECK(image.Red.Minimum == 10);
	CHECK(image.Green.Minimum == 70);
	CHECK(image.Blue.Minimum == 130);
	CHECK(image.Alpha.Minimum == 200);

	CHECK(image.Red.Counted == 1);
	CHECK(Total(image.Red) == 1);
}

TEST_CASE("a blank channel is the fault the editor could not see", "[render]") {
	// Fault 3: the captured frame's albedo target had a blank alpha channel and
	// a human found it by reading a capture for half an hour.
	std::vector<uint32_t> pixels;
	for (uint8_t value = 0; value < 64; value++) {
		pixels.push_back(Pixel(value, static_cast<uint8_t>(255 - value), 128, 0));
	}

	const ImageHistogram image = Histogram(pixels);

	CHECK(image.Alpha.Blank());
	CHECK(image.Alpha.Constant());

	// **Constant is not the same as blank**, and both are worth telling apart: a
	// mask that is all ones is doing its job, and a channel that is all zero is
	// either unwritten or wasted.
	CHECK(image.Blue.Constant());
	CHECK_FALSE(image.Blue.Blank());

	// The channels that vary are not accused of anything.
	CHECK_FALSE(image.Red.Constant());
	CHECK_FALSE(image.Green.Constant());

	// And the image as a whole is not uniform, because two channels move.
	CHECK_FALSE(image.Uniform());
}

TEST_CASE("a uniform image is the whole-target version of the same fault", "[render]") {
	// Fault 4: "a completely blank image", which is fault 3 over every channel.
	const std::vector<uint32_t> pixels(32, Pixel(0, 0, 0, 255));
	const ImageHistogram image = Histogram(pixels);

	CHECK(image.Uniform());
	CHECK(image.Red.Blank());
	CHECK(image.Alpha.Constant());
	CHECK_FALSE(image.Alpha.Blank());
}

TEST_CASE("an image nobody downloaded is not an image that is blank", "[render]") {
	// **The case that decides what the panel draws on a node with no readback.**
	// Reporting "constant" for a target nothing sampled would put a warning
	// triangle on a node whose only crime is not having been looked at.
	const ImageHistogram image = Histogram({});

	CHECK(image.Red.Counted == 0);
	CHECK_FALSE(image.Red.Constant());
	CHECK_FALSE(image.Red.Blank());
	CHECK_FALSE(image.Uniform());
}

TEST_CASE("every pixel lands in exactly one bucket", "[render]") {
	// The extremes are what a bucketing mistake breaks: 255 belongs in the last
	// bucket and not one past the end, which is a write off the end of the array
	// rather than a wrong bar.
	std::vector<uint32_t> pixels;
	for (size_t value = 0; value < 256; value++) {
		pixels.push_back(Pixel(static_cast<uint8_t>(value), 0, 0, 0));
	}

	const ImageHistogram image = Histogram(pixels);

	REQUIRE(Total(image.Red) == 256);
	CHECK(image.Red.Minimum == 0);
	CHECK(image.Red.Maximum == 255);

	// Evenly spread, because the input is.
	for (const uint32_t bucket : image.Red.Buckets) {
		CHECK(bucket == 256 / HISTOGRAM_BUCKETS);
	}
}

TEST_CASE("a readback holds one download at a time", "[render]") {
	PendingReadback pending;

	CHECK(pending.CanRequest());
	CHECK_FALSE(pending.HasImage());

	pending.Submitted(10);
	CHECK_FALSE(pending.CanRequest());

	// **A fence that has not signalled is not an answer.** Polling an unfinished
	// download must not hand over a transfer buffer the GPU is still writing.
	CHECK_FALSE(pending.Poll(false));
	CHECK_FALSE(pending.HasImage());
	CHECK_FALSE(pending.CanRequest());

	CHECK(pending.Poll(true));
	CHECK(pending.HasImage());
	CHECK(pending.CanRequest());

	// **True once, on the call that made the pixels readable.** A caller maps
	// the transfer buffer on that edge, and a `Poll` that kept saying true would
	// have it map and reduce the same image every frame.
	CHECK_FALSE(pending.Poll(true));
}

TEST_CASE("a readback says how old its picture is", "[render]") {
	// The whole reason this is not `--capture`: the panel shows a frame-old
	// image and has to say so out loud.
	PendingReadback pending;

	pending.Submitted(10);
	CHECK(pending.Age(12) == 0);

	// **Signalled on frame 12, but the picture is frame 10's.** Ageing it from
	// the fence rather than the request would report one frame when the answer
	// is two — a number that looks measured and is wrong.
	REQUIRE(pending.Poll(true));
	CHECK(pending.ImageFrame() == 10);
	CHECK(pending.Age(12) == 2);
	CHECK(pending.Age(10) == 0);

	// A frame counter that went backwards saturates rather than wrapping to
	// something enormous.
	CHECK(pending.Age(3) == 0);
}

TEST_CASE("a readback forgets everything for a device that went away", "[render]") {
	PendingReadback pending;
	pending.Submitted(4);
	REQUIRE(pending.Poll(true));
	REQUIRE(pending.HasImage());

	pending.Clear();

	CHECK(pending.CanRequest());
	CHECK_FALSE(pending.HasImage());
	CHECK(pending.Age(100) == 0);
}
