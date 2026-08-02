// The overlay, which is drawn on the CPU every frame the panels are open.
//
// **This is the one part of the renderer a benchmark can measure honestly.**
// Everything past the upload is the GPU's and depends on the driver, the
// display and what else is on the machine; the overlay is a software raster
// into a byte buffer, so its cost is this code's and nothing else's.
//
// It matters because it runs *per frame at the display's rate*, not per tick.
// At 300 fps a millisecond here is a third of the frame budget spent drawing
// the thing that tells you where the frame budget went.

#include <engine/render/DebugPanels.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/testing/Bench.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.bench.overlay")

using engine::render::DebugPanelData;
using engine::render::OverlayImage;
using engine::testing::Consume;

namespace overlay_bench {
	// Reused rather than constructed per sample: allocating a megabyte inside a
	// measured body measures the allocator.
	OverlayImage &ImageOf(int width, int height) {
		static OverlayImage image;
		image.Resize(width, height);
		return image;
	}

	// The panels as a busy frame actually has them: both open, a full system
	// list, a full counter list.
	DebugPanelData Busy() {
		static std::vector<engine::render::SystemTiming> systems;
		static std::vector<std::string> names;

		if (systems.empty()) {
			names.reserve(32);
			for (int index = 0; index < 32; index++) {
				names.push_back("system.number." + std::to_string(index));
			}
			for (int index = 0; index < 32; index++) {
				engine::render::SystemTiming timing;
				timing.Name = names[static_cast<size_t>(index)];
				timing.Milliseconds = 0.1f * static_cast<float>(index);
				systems.push_back(timing);
			}
		}

		DebugPanelData data;
		data.ShowStatistics = true;
		data.ShowFrameGraph = true;
		data.Entities = 100'000;
		data.TickRate = 60.0;
		data.TicksPerSecond = 59.8f;
		data.DrawCalls = 1;
		data.Triangles = 1'200'000;
		data.Backend = "vulkan";
		data.Scale = 2;
		data.Systems = systems;
		return data;
	}
}

using namespace overlay_bench;

// No benchmark for `Clear` on its own. It measured at zero, which is the
// correct answer — clearing marks a region rather than touching pixels — and a
// row of zeroes in a report teaches nothing while looking like a broken
// measurement. The cost that exists is in the drawing below.

BENCH("DrawDebugPanels · 1280x720, both open", 100) {
	OverlayImage &image = ImageOf(1280, 720);
	const DebugPanelData data = Busy();
	for (int pass = 0; pass < 100; pass++) {
		DrawDebugPanels(image, data);
	}
}

BENCH("DrawDebugPanels · 1920x1080, both open", 100) {
	OverlayImage &image = ImageOf(1920, 1080);
	const DebugPanelData data = Busy();
	for (int pass = 0; pass < 100; pass++) {
		DrawDebugPanels(image, data);
	}
}

BENCH("DrawDebugPanels · 3840x2160, both open", 50) {
	// A 4K display, where the overlay is four times the pixels and the same
	// text. If the cost scales with the area rather than with the glyphs, this
	// is where it shows.
	OverlayImage &image = ImageOf(3840, 2160);
	const DebugPanelData data = Busy();
	for (int pass = 0; pass < 50; pass++) {
		DrawDebugPanels(image, data);
	}
}

BENCH("DrawDebugPanels · nothing switched on", 1000) {
	// The path a normal frame takes. It has to be free, because it runs on
	// every frame of every run whether or not anybody pressed F3.
	OverlayImage &image = ImageOf(1920, 1080);
	DebugPanelData data;
	for (int pass = 0; pass < 1000; pass++) {
		DrawDebugPanels(image, data);
	}
}
