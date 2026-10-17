#include <engine/testing/Suite.hpp>
#include <engine/weather/Assemblies.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>

TEST_SUITE_ID("engine.weather.assemblies")
TEST_DEPENDS("engine.weather.weather")

namespace {
	using namespace engine::weather;

	std::array<EncodedCell, ASSEMBLY_CHILDREN> Children() {
		std::array<EncodedCell, ASSEMBLY_CHILDREN> children{};
		for (size_t index = 0; index < children.size(); ++index) {
			CellValues sample;
			sample.Condensate = static_cast<uint8_t>(index * 3);
			sample.TemperatureAnomaly = static_cast<int16_t>(static_cast<int32_t>(index) - 5);
			sample.Vapour = static_cast<uint8_t>(200 + index);
			sample.PressureAnomaly = static_cast<int16_t>(-127 + static_cast<int32_t>(index));
			sample.Ice = static_cast<uint8_t>(index % 2);
			sample.Wind = {
				static_cast<int16_t>(index), static_cast<int16_t>(-static_cast<int32_t>(index)), 127
			};
			children[index] = *Encode(sample);
		}
		return children;
	}

	AssemblySummary Summary(const std::array<EncodedCell, ASSEMBLY_CHILDREN> &children, uint64_t tick = 100) {
		WeatherTime time;
		time.Tick = tick;
		time.Revision = 2;
		return *Summarize({{-4, 2, 0}, 1, 2}, time, children);
	}

	AssemblySummary Uniform() {
		std::array<EncodedCell, ASSEMBLY_CHILDREN> children{};
		CellValues sample;
		sample.Condensate = 40;
		sample.Vapour = 80;
		children.fill(*Encode(sample));
		return Summary(children);
	}
}

TEST_CASE("assembly summaries retain exact signed totals and conservative extrema", "[weather][assemblies]") {
	const auto summary = Summary(Children());
	CHECK(summary.Scalars[0].Sum == 84);
	CHECK(summary.Scalars[0].Minimum == 0);
	CHECK(summary.Scalars[0].Maximum == 21);
	CHECK(summary.Scalars[0].VarianceNumerator == 3024);
	CHECK(summary.Scalars[1].Sum == -12);
	CHECK(summary.Mean.TemperatureAnomaly == -2);
	CHECK(summary.Scalars[3].Sum == -988);
	CHECK(summary.ChildWind[7][1] == -7);
	CHECK(summary.Tick == 100);
	CHECK(summary.Revision == 2);
	const AssemblyStorage storage;
	CHECK(storage.DenseBytes == 64);
	CHECK(storage.RetainedWindBytes == 24);
	CHECK(storage.SummaryBytes >= storage.RetainedWindBytes);
	CHECK(storage.DenseToSummaryRatio == Catch::Approx(64.0f / static_cast<float>(sizeof(AssemblySummary))));
}

TEST_CASE(
	"balanced lossy splitting conserves every encoded total without clamping", "[weather][assemblies]"
) {
	const auto summary = Summary(Children());
	for (uint32_t seed = 0; seed < 128; ++seed) {
		const auto children = SplitBalanced(summary, seed, 7, uint64_t{1} << 45, 255);
		REQUIRE(children);
		const auto split = Summary(*children);
		for (size_t purpose = 0; purpose < ASSEMBLY_SCALARS; ++purpose) {
			CHECK(split.Scalars[purpose].Sum == summary.Scalars[purpose].Sum);
			CHECK(split.Scalars[purpose].Minimum >= summary.Scalars[purpose].Minimum);
			CHECK(split.Scalars[purpose].Maximum <= summary.Scalars[purpose].Maximum);
		}
		CHECK(split.ChildWind == summary.ChildWind);
		CHECK(SplitBalanced(summary, seed, 7, uint64_t{1} << 45, 255) == children);
	}
	CHECK(SplitBalanced(Uniform(), 4, 8, 0) == SplitBalanced(Uniform(), 4, 8, 0, 0));
	auto corrupt = summary;
	corrupt.Scalars[0].Sum = 2041;
	CHECK_FALSE(SplitBalanced(corrupt, 1, 1, 1));
	corrupt = summary;
	corrupt.ChildWind[0][0] = -128;
	CHECK_FALSE(SplitBalanced(corrupt, 1, 1, 1));
}

TEST_CASE(
	"aligned octree children reject disconnected regions and coordinate overflow", "[weather][assemblies]"
) {
	const auto coordinates = ChildCoordinates({{-4, 2, 0}, 1, 0});
	REQUIRE(coordinates);
	CHECK((*coordinates)[0] == CellCoordinate{-4, 2, 0});
	CHECK((*coordinates)[7] == CellCoordinate{-3, 3, 1});
	CHECK_FALSE(ChildCoordinates({{1, 0, 0}, 1, 0}));
	CHECK_FALSE(ChildCoordinates({{}, 0, 0}));
	CHECK_FALSE(ChildCoordinates({{}, 3, 0}));
	CHECK_FALSE(ChildCoordinates({{}, std::numeric_limits<uint32_t>::max(), 0}));
	auto malformed = Children();
	malformed[3].Codes[1] = 255;
	CHECK_FALSE(Summarize({{}, 1, 0}, {}, malformed));
}

TEST_CASE("canonical splitting is independent of query and prior merge order", "[weather][assemblies]") {
	WeatherDefinition definition;
	definition.Seed = 473;
	definition.BaseCell = -10;
	definition.ThicknessCells = 100;
	const auto time = ResolveTime(InitialAnchor(definition), uint64_t{1} << 40);
	REQUIRE(time);
	const AssemblyRegion region{{-16, 8, 24}, 4, 2};
	const auto children = EvaluateChildren(definition, *time, region);
	const auto coordinates = ChildCoordinates(region);
	REQUIRE(children);
	REQUIRE(coordinates);
	for (size_t reverse = ASSEMBLY_CHILDREN; reverse > 0; --reverse)
		CHECK((*children)[reverse - 1] == Evaluate(definition, *time, (*coordinates)[reverse - 1]));
	const auto summary = Summarize(region, *time, *children);
	REQUIRE(summary);
	REQUIRE(SplitBalanced(*summary, 2, 3, 4));
	CHECK(EvaluateChildren(definition, *time, region) == children);
	auto wrongRevision = *time;
	++wrongRevision.Revision;
	CHECK_FALSE(EvaluateChildren(definition, wrongRevision, region));
}

TEST_CASE("merge thresholds preserve fronts and divergent or directionless wind", "[weather][assemblies]") {
	const EncodingProfile profile;
	CHECK(CanMerge(Uniform(), profile));
	auto front = Uniform();
	front.Scalars[3].Minimum = -20;
	front.Scalars[3].Maximum = 20;
	CHECK_FALSE(CanMerge(front, profile));
	front = Uniform();
	front.Scalars[0].VarianceNumerator = 257;
	CHECK_FALSE(CanMerge(front, profile));
	auto divergent = Uniform();
	divergent.ChildWind[0][0] = 8;
	CHECK_FALSE(CanMerge(divergent, profile));
	for (auto &wind : divergent.ChildWind)
		wind = {8, 0, 0};
	CHECK(CanMerge(divergent, profile));
	divergent.ChildWind[7] = {-8, 0, 0};
	AssemblyThresholds windThresholds;
	windThresholds.MaximumVelocityDifferenceMetresPerSecond = 100;
	windThresholds.MaximumSpeedDifferenceMetresPerSecond = 100;
	CHECK_FALSE(CanMerge(divergent, profile, windThresholds));
	divergent.ChildWind[7] = {0, 0, 0};
	CHECK_FALSE(CanMerge(divergent, profile, windThresholds));
	windThresholds.MaximumDirectionRadians = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(CanMerge(Uniform(), profile, windThresholds));
}

TEST_CASE("assembly split hysteresis waits for residence and the larger threshold", "[weather][assemblies]") {
	auto summary = Uniform();
	AssemblyThresholds thresholds;
	summary.Scalars[0].Maximum = 50;
	CHECK_FALSE(CanMerge(summary, {}));
	CHECK_FALSE(NeedsSplit(summary, {}, thresholds, 0));
	summary.Scalars[0].Maximum = 60;
	CHECK_FALSE(NeedsSplit(summary, {}, thresholds, 90));
	CHECK(NeedsSplit(summary, {}, thresholds, 20));
	CHECK_FALSE(NeedsSplit(summary, {}, thresholds, 200));
}

TEST_CASE("physical vapour means use dry-air mass and condensate uses volume", "[weather][assemblies]") {
	std::array<PhysicalSample, ASSEMBLY_CHILDREN> samples{};
	std::array<WaterWeights, ASSEMBLY_CHILDREN> weights{};
	for (size_t child = 0; child < ASSEMBLY_CHILDREN; ++child) {
		samples[child].VapourKilogramsPerKilogram = child == 0 ? 0.5f : 0.0f;
		samples[child].CondensateKilogramsPerCubicMetre = child == 0 ? 0.25f : 0.0f;
		weights[child] = {child == 0 ? 2.0 : 1.0, child == 0 ? 3.0 : 1.0};
	}
	const auto water = WaterOf(samples, weights);
	REQUIRE(water);
	CHECK(water->VapourKilograms == Catch::Approx(3.0));
	CHECK(water->CondensateKilograms == Catch::Approx(0.5));
	CHECK(water->MeanVapourKilogramsPerKilogram == Catch::Approx(3.0 / 13.0));
	CHECK(water->MeanCondensateKilogramsPerCubicMetre == Catch::Approx(0.5 / 9.0));
	weights[0].VolumeCubicMetres = -1;
	CHECK_FALSE(WaterOf(samples, weights));
}
