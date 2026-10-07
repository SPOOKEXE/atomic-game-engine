#include "../tests/fixtures/SourcePixelKernels.hpp"

#include "SourceWorkloadProfile.hpp"

TEST_SUITE_ID("engine.imagegraph.bench.pixel-kernels")
using Fixture = engine::imagegraph::testing::SourcePixelKernelFixture;

BENCH("CPU Atlas Draw 128x128 16 ordered sprites compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Atlas16);
	p.Measure();
}
BENCH("CPU Atlas Draw 128x128 64 ordered sprites compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Atlas64);
	p.Measure();
}
BENCH("CPU Palette Sort 512 colours RGB packed key compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::PaletteRGB);
	p.Measure();
}
BENCH("CPU Palette Sort 512 colours reverse custom key compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::PaletteReverse);
	p.Measure();
}
BENCH("CPU Edge Detect 128x128 Sobel 9 neighbors compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::EdgeSobel);
	p.Measure();
}
BENCH("CPU Edge Detect 128x128 Laplacian 9 neighbors compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::EdgeLaplacian);
	p.Measure();
}
BENCH("CPU Bokeh 128x128 strength8 taps8 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Bokeh8);
	p.Measure();
}
BENCH("CPU Bokeh 128x128 strength8 taps32 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::Bokeh32);
	p.Measure();
}

BENCH("CPU Fold Greyscale 64x64 iteration3 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::FoldGreyscale);
	p.Measure();
}

BENCH("CPU Fold Map 64x64 iteration3 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::FoldMap);
	p.Measure();
}

BENCH("CPU Gaussian random 64x64 seed17.25 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::GaussianRandom);
	p.Measure();
}

BENCH("CPU Gaussian conversion 64x64 two red samplers compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::GaussianConversion);
	p.Measure();
}

BENCH("CPU Aniso Blend 64x64 two seeds compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::AnisoBlend);
	p.Measure();
}

BENCH("CPU Aniso Waterfall 64x64 three mapped controls tile compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::AnisoMapped);
	p.Measure();
}

BENCH("CPU Fold RGB field 32x32 Sample Vector3 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::RasterRGB);
	p.Measure();
}

BENCH("CPU Mirror Polar Clean Edge 96x96 unique coordinates trim24 compiled-plan", 1) {
	static Profile<Fixture> p(Fixture::Kind::MirrorPolarCleanEdge);
	p.Measure();
}
