#include "PackingComparison.hpp"
#include "WeatherSample.hpp"

#include <engine/testing/Suite.hpp>
#include <engine/weather/Weather.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.weather.weather")

using namespace engine::weather;

TEST_CASE("weather codec round trips every legal byte without platform word order", "[weather]") {
	for (uint32_t code = 0; code < 256; code++) {
		CellValues values;
		values.Condensate = static_cast<uint8_t>(code);
		values.Vapour = static_cast<uint8_t>(255u - code);
		values.Ice = static_cast<uint8_t>(code);
		const auto encoded = Encode(values);
		REQUIRE(encoded);
		CHECK(Decode(*encoded) == values);
		if (code < 255) {
			values.TemperatureAnomaly = static_cast<int16_t>(static_cast<int32_t>(code) - 127);
			values.PressureAnomaly = values.TemperatureAnomaly;
			values.Wind.fill(values.TemperatureAnomaly);
			REQUIRE(Encode(values));
			CHECK(Decode(*Encode(values)) == values);
		}
	}
	CellValues invalid;
	invalid.Wind[1] = 128;
	CHECK_FALSE(Encode(invalid));
	invalid = {};
	invalid.TemperatureAnomaly = -128;
	CHECK_FALSE(Encode(invalid));
	invalid = {};
	invalid.PressureAnomaly = 128;
	CHECK_FALSE(Encode(invalid));
	for (uint32_t index : {1u, 3u, 4u, 5u, 6u}) {
		auto cell = *Encode(CellValues{});
		cell.Codes[index] = 255;
		CHECK_FALSE(Decode(cell));
	}
}

TEST_CASE("weather compilation rejects invalid semantics rather than changing their meaning", "[weather]") {
	engine::scene::WeatherCompute controls;
	const auto compiled = Compile(controls, 47);
	REQUIRE(compiled);
	CHECK(compiled->Definition.LayerKey == 47);
	CHECK(compiled->Definition.FeatureShift == 5);
	CHECK(compiled->Definition.WindQ20[0] == 4915);
	CHECK(compiled->Definition.Coverage == 128);
	controls.CellSizeMetres = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(Compile(controls));
	controls = {};
	controls.FeatureCells = 31;
	CHECK_FALSE(Compile(controls));
	controls = {};
	controls.VapourMixingRatio = controls.VapourScale + 0.01f;
	CHECK_FALSE(Compile(controls));
	controls = {};
	controls.AlgorithmVersion = 2;
	CHECK_FALSE(Compile(controls));
	controls = {};
	controls.Mode = engine::scene::WeatherMode::Stateful;
	CHECK_FALSE(Compile(controls));
	controls = {};
	controls.CellSizeMetres = 1;
	controls.BaseHeightMetres = -16384;
	controls.ThicknessMetres = 65536;
	controls.FeatureCells = 1;
	controls.WindMetresPerSecond = {-64, -16, 64};
	controls.TimeScale = 16;
	controls.TickRate = 1;
	const auto boundary = Compile(controls);
	REQUIRE(boundary);
	CHECK(boundary->Definition.BaseCell == -16384);
	CHECK(boundary->Definition.ThicknessCells == 65536);
	CHECK(boundary->Definition.WindQ20[0] == -MAXIMUM_WIND_Q20);
	CHECK(Decode(Evaluate(boundary->Definition, {}, {-1, 100, 0})));
}

TEST_CASE("weather named layers use fixed text keys independent of prior interning", "[weather]") {
	using engine::core::Name;
	const Name unrelatedBefore("weather.core.unrelated.before");
	const Name beta("weather.core.layer.beta");
	const Name unrelatedBetween("weather.core.unrelated.between");
	const Name alpha("weather.core.layer.alpha");
	CHECK(unrelatedBefore != unrelatedBetween);
	CHECK(WeatherLayerKey("default") == 3477690857u);
	CHECK(WeatherLayerKey(alpha.Text()) == 1004092005u);
	CHECK(WeatherLayerKey(beta.Text()) == 4279316115u);
	engine::scene::WeatherCompute controls;
	controls.LayerName = alpha;
	const auto compiledAlpha = Compile(controls);
	REQUIRE(compiledAlpha);
	CHECK(compiledAlpha->Definition.LayerKey == 1004092005u);
	controls.LayerName = beta;
	const auto compiledBeta = Compile(controls);
	REQUIRE(compiledBeta);
	CHECK(compiledBeta->Definition.LayerKey == 4279316115u);
	bool distinctSamples = false;
	for (int32_t index = 0; index < 16; index++) {
		const CellCoordinate coordinate{index * 31, 20, -index * 17};
		distinctSamples |= Evaluate(compiledAlpha->Definition, {}, coordinate) !=
						   Evaluate(compiledBeta->Definition, {}, coordinate);
	}
	CHECK(distinctSamples);
	const Name unrelatedAfter("weather.core.unrelated.after");
	CHECK(unrelatedAfter.IsValid());
	controls.LayerName = Name(alpha.Text());
	const auto recreated = Compile(controls);
	REQUIRE(recreated);
	CHECK(recreated->Definition.LayerKey == compiledAlpha->Definition.LayerKey);
	const auto explicitZero = Compile(controls, 0u);
	REQUIRE(explicitZero);
	CHECK(explicitZero->Definition.LayerKey == 0);
	controls.LayerName = {};
	CHECK_FALSE(Compile(controls));
}

TEST_CASE("weather reference hash and huge tick cells have fixed portable codes", "[weather]") {
	CHECK(Hash(0) == 0);
	CHECK(Hash(1) == 1753845952u);
	CHECK(Hash(UINT32_MAX) == 1734902346u);
	WeatherDefinition definition;
	definition.BaseCell = -3;
	definition.ThicknessCells = 16;
	definition.WindQ20 = {-17 * 4096, 3 * 4096, 29 * 4096};
	const auto anchor = InitialAnchor(definition);
	const auto time = ResolveTime(anchor, UINT64_MAX);
	REQUIRE(time);
	CHECK(
		Evaluate(definition, *time, {INT32_MAX, 5, INT32_MIN}) ==
		EncodedCell{{16, 136, 137, 147, 142, 124, 131, 0}}
	);
	const auto boundary = ResolveTime(anchor, UINT32_MAX);
	REQUIRE(boundary);
	CHECK(Evaluate(definition, *boundary, {-33, 3, 17}) == EncodedCell{{0, 136, 99, 103, 143, 127, 135, 0}});
	CHECK(Evaluate(definition, {}, {0, 4, 0}) == EncodedCell{{16, 137, 137, 147, 142, 124, 131, 0}});
}

TEST_CASE("weather direct seeking equals many rebased seeks even at maximum ticks", "[weather]") {
	WeatherDefinition definition;
	definition.WindQ20 = {-32768 * 4096 + 31, 7 * 4096 + 13, 92 * 4096 - 5};
	definition.TimeScaleQ16 = 98305;
	auto anchor = InitialAnchor(definition);
	const auto direct = ResolveTime(anchor, UINT64_MAX);
	REQUIRE(direct);
	for (uint64_t tick : {uint64_t{1}, uint64_t{65535}, uint64_t{1} << 32u, uint64_t{1} << 63u}) {
		const auto replacement = Reanchor(anchor, definition, tick);
		REQUIRE(replacement);
		anchor = *replacement;
	}
	const auto segmented = ResolveTime(anchor, UINT64_MAX);
	REQUIRE(segmented);
	CHECK(segmented->DisplacementQ8 == direct->DisplacementQ8);
	CHECK(segmented->DisplacementRemainderQ12 == direct->DisplacementRemainderQ12);
	CHECK(segmented->PhaseTick == direct->PhaseTick);
	CHECK(segmented->PhaseFractionQ16 == direct->PhaseFractionQ16);
	CHECK_FALSE(ResolveTime(anchor, anchor.Tick - 1));
	anchor.TimeScaleQ16 = 16u * 65536u + 1u;
	CHECK_FALSE(ResolveTime(anchor, anchor.Tick));
}

TEST_CASE("weather wind edits and pause preserve accepted cumulative motion", "[weather]") {
	WeatherDefinition first;
	first.WindQ20 = {256 * 4096, -128 * 4096, 64 * 4096};
	const auto initial = InitialAnchor(first, 100);
	const auto before = ResolveTime(initial, 200);
	REQUIRE(before);
	WeatherDefinition next = first;
	next.WindQ20 = {-512 * 4096, 0, 256 * 4096};
	next.Revision = 2;
	const auto changed = Reanchor(initial, next, 200);
	REQUIRE(changed);
	const auto sameTick = ResolveTime(*changed, 200);
	REQUIRE(sameTick);
	CHECK(sameTick->DisplacementQ8 == before->DisplacementQ8);
	CHECK(sameTick->PhaseTick == before->PhaseTick);
	const auto later = ResolveTime(*changed, 300);
	REQUIRE(later);
	CHECK(later->DisplacementQ8[0] == static_cast<uint32_t>(-25600));
	next.Paused = true;
	const auto paused = Reanchor(*changed, next, 300);
	REQUIRE(paused);
	const auto frozen = ResolveTime(*paused, UINT64_MAX);
	REQUIRE(frozen);
	CHECK(frozen->DisplacementQ8 == later->DisplacementQ8);
	CHECK(frozen->DisplacementRemainderQ12 == later->DisplacementRemainderQ12);
	CHECK(frozen->PhaseTick == later->PhaseTick);
	next.Paused = false;
	const auto resumed = Reanchor(*paused, next, 400);
	REQUIRE(resumed);
	CHECK(resumed->DisplacementQ8 == frozen->DisplacementQ8);
	CHECK_FALSE(Reanchor(*changed, next, 199));
}

TEST_CASE("weather fractional wind accumulates with bounded position error", "[weather]") {
	engine::scene::WeatherCompute controls;
	const auto compiled = Compile(controls);
	REQUIRE(compiled);
	const auto anchor = InitialAnchor(compiled->Definition);
	for (uint64_t tick : {uint64_t{1000}, uint64_t{4097}, uint64_t{200000}}) {
		const auto time = ResolveTime(anchor, tick);
		REQUIRE(time);
		const uint64_t displacement =
			(static_cast<uint64_t>(time->DisplacementQ8[0]) << 12u) | time->DisplacementRemainderQ12[0];
		const double actualMetres = static_cast<double>(displacement) * controls.CellSizeMetres / 1048576.0;
		const double expectedMetres =
			static_cast<double>(tick) * controls.WindMetresPerSecond.X / controls.TickRate;
		const double errorBoundMetres = static_cast<double>(tick) * controls.CellSizeMetres / 2097152.0;
		CHECK(std::abs(actualMetres - expectedMetres) <= errorBoundMetres);
		CHECK(time->DisplacementRemainderQ12[0] < 4096);
	}
	const auto before = ResolveTime(anchor, 1001);
	REQUIRE(before);
	CHECK(before->DisplacementRemainderQ12[0] != 0);
	controls.TickRate = 40;
	controls.WindMetresPerSecond.X = -3.0f;
	const auto changed = Compile(controls);
	REQUIRE(changed);
	const auto replacement = Reanchor(anchor, changed->Definition, 1001);
	REQUIRE(replacement);
	CHECK(replacement->DisplacementQ8 == before->DisplacementQ8);
	CHECK(replacement->DisplacementRemainderQ12 == before->DisplacementRemainderQ12);
	WeatherDefinition pausedDefinition = changed->Definition;
	pausedDefinition.Paused = true;
	const auto paused = Reanchor(*replacement, pausedDefinition, 1001);
	REQUIRE(paused);
	const auto frozen = ResolveTime(*paused, UINT64_MAX);
	REQUIRE(frozen);
	CHECK(frozen->DisplacementQ8 == before->DisplacementQ8);
	CHECK(frozen->DisplacementRemainderQ12 == before->DisplacementRemainderQ12);
	const auto resumed = Reanchor(*paused, changed->Definition, 2000);
	REQUIRE(resumed);
	CHECK(resumed->DisplacementRemainderQ12 == before->DisplacementRemainderQ12);
	const auto later = ResolveTime(*replacement, 4097);
	REQUIRE(later);
	const uint64_t beforePosition =
		(static_cast<uint64_t>(before->DisplacementQ8[0]) << 12u) | before->DisplacementRemainderQ12[0];
	const uint64_t laterPosition =
		(static_cast<uint64_t>(later->DisplacementQ8[0]) << 12u) | later->DisplacementRemainderQ12[0];
	constexpr uint64_t mask = (uint64_t{1} << 44u) - 1u;
	CHECK(
		laterPosition ==
		((beforePosition +
		  uint64_t{3096} * static_cast<uint64_t>(static_cast<int64_t>(changed->Definition.WindQ20[0]))) &
		 mask)
	);
	controls.CellSizeMetres = 4096;
	controls.TickRate = 120;
	controls.WindMetresPerSecond.X = 6.0f;
	const auto coarse = Compile(controls);
	REQUIRE(coarse);
	CHECK(coarse->Definition.WindQ20[0] > 0);
	const auto coarseTime = ResolveTime(InitialAnchor(coarse->Definition), 1000);
	REQUIRE(coarseTime);
	CHECK(coarseTime->DisplacementQ8[0] > 0);
	WeatherTimelineAnchor invalid;
	invalid.DisplacementRemainderQ12[1] = 4096;
	CHECK_FALSE(ResolveTime(invalid, 0));
	invalid = {};
	invalid.WindQ20[0] = MAXIMUM_WIND_Q20 + 1;
	CHECK_FALSE(ResolveTime(invalid, 0));
	invalid.WindQ20[0] = -MAXIMUM_WIND_Q20 - 1;
	CHECK_FALSE(ResolveTime(invalid, 0));
}

TEST_CASE("weather noise is continuous across signed advection and declared periods", "[weather]") {
	WeatherDefinition definition;
	definition.BaseCell = -8;
	definition.ThicknessCells = 16;
	WeatherTime moved;
	moved.DisplacementQ8 = {static_cast<uint32_t>(-256), 0, 0};
	CHECK(Evaluate(definition, moved, {-1, 1, 0}) == Evaluate(definition, {}, {0, 1, 0}));
	CHECK(Evaluate(definition, {}, {-1, 1, 0}) == Evaluate(definition, {}, {16777215, 1, 0}));
	CHECK(Evaluate(definition, {}, {INT32_MIN, 1, INT32_MAX}) == Evaluate(definition, {}, {0, 1, -1}));
	moved.DisplacementQ8[0] = UINT32_MAX;
	const auto atWrap = Evaluate(definition, moved, {0, 1, 0});
	const auto afterWrap = Evaluate(definition, {}, {0, 1, 0});
	for (size_t index = 0; index < ENCODED_CELL_BYTES; index++) {
		CHECK(std::abs(static_cast<int>(atWrap.Codes[index]) - afterWrap.Codes[index]) <= 1);
	}
	WeatherTime before;
	before.PhaseTick = 255;
	WeatherTime after;
	after.PhaseTick = 256;
	CHECK(Evaluate(definition, before, {0, 1, 0}) == Evaluate(definition, after, {0, 1, 0}));
}

TEST_CASE("weather batches retain scalar values under generation order changes", "[weather]") {
	WeatherDefinition definition;
	WeatherTime time;
	time.PhaseTick = UINT64_MAX;
	std::vector<CellCoordinate> coordinates;
	for (int32_t index = -128; index < 128; index++) {
		coordinates.push_back({index * 31, index % 25, -index * 17});
	}
	std::vector<EncodedCell> output(coordinates.size());
	REQUIRE(EvaluateBatch(definition, time, coordinates, output));
	for (size_t index = 0; index < coordinates.size(); index++) {
		CHECK(output[index] == Evaluate(definition, time, coordinates[index]));
		CHECK(Decode(output[index]));
	}
	std::reverse(coordinates.begin(), coordinates.end());
	std::vector<EncodedCell> reversed(coordinates.size());
	REQUIRE(EvaluateBatch(definition, time, coordinates, reversed));
	std::reverse(reversed.begin(), reversed.end());
	CHECK(reversed == output);
	const auto preserved = output;
	CHECK_FALSE(EvaluateBatch(definition, time, std::span(coordinates).first(2), output));
	CHECK(output == preserved);
	CHECK(EvaluateBatch(definition, time, {}, {}));
}

TEST_CASE("weather packed lanes and scalar tails agree at spatial and phase boundaries", "[weather]") {
	for (uint32_t featureShift : {0u, 2u, 5u, 10u}) {
		WeatherDefinition definition;
		definition.FeatureShift = featureShift;
		definition.BaseCell = -8;
		definition.ThicknessCells = 32;
		definition.Seed = Hash(featureShift + 23);
		definition.LayerKey = Hash(featureShift + 173);
		for (uint64_t phase : {uint64_t{0}, uint64_t{255}, uint64_t{256}, uint64_t{1} << 60u, UINT64_MAX}) {
			WeatherTime time;
			time.PhaseTick = phase;
			time.DisplacementQ8 = {UINT32_MAX, 256, 0x80000000u};
			for (size_t count = 0; count < 20; count++) {
				std::vector<CellCoordinate> coordinates(count);
				std::vector<EncodedCell> packed(count);
				for (size_t index = 0; index < count; index++) {
					coordinates[index] = {
						index % 2 == 0 ? INT32_MIN + static_cast<int32_t>(index)
									   : INT32_MAX - static_cast<int32_t>(index),
						static_cast<int32_t>(index) - 8,
						-static_cast<int32_t>(index * 16777215)
					};
				}
				REQUIRE(EvaluateBatch(definition, time, coordinates, packed));
				for (size_t index = 0; index < count; index++) {
					CHECK(packed[index] == Evaluate(definition, time, coordinates[index]));
				}
			}
		}
	}
}

TEST_CASE(
	"weather purposes preserve unrelated fields while moisture and temperature affect clouds", "[weather]"
) {
	WeatherDefinition definition;
	definition.Coverage = 255;
	definition.Vapour = 200;
	definition.SaturationBase = 40;
	WeatherDefinition changed = definition;
	changed.WindCodes = {-127, 127, 0};
	bool moved = false;
	bool seedChanged = false;
	bool cloudPresent = false;
	for (int32_t index = 1; index < 24; index++) {
		const CellCoordinate coordinate{index * 31, index, -index * 17};
		const auto initial = Evaluate(definition, {}, coordinate);
		const auto windEdited = Evaluate(changed, {}, coordinate);
		for (uint32_t field : {0u, 1u, 2u, 3u, 7u}) {
			CHECK(initial.Codes[field] == windEdited.Codes[field]);
		}
		WeatherTime later;
		later.PhaseTick = 8192;
		moved |= initial != Evaluate(definition, later, coordinate);
		WeatherDefinition otherSeed = definition;
		otherSeed.Seed++;
		seedChanged |= initial != Evaluate(otherSeed, {}, coordinate);
		cloudPresent |= initial.Codes[0] > 0;
		WeatherDefinition hot = definition;
		hot.SaturationBase = 255;
		hot.TemperatureVariation = 0;
		CHECK(Evaluate(hot, {}, coordinate).Codes[0] == 0);
		WeatherDefinition dry = definition;
		dry.Vapour = 0;
		dry.SaturationBase = 255;
		CHECK(Evaluate(dry, {}, coordinate).Codes[0] == 0);
	}
	CHECK(moved);
	CHECK(seedChanged);
	CHECK(cloudPresent);
	CHECK(Evaluate(definition, {}, {0, -1, 0}).Codes[0] == 0);
	CHECK(Evaluate(definition, {}, {0, 25, 0}).Codes[0] == 0);
	WeatherDefinition thin = definition;
	thin.ThicknessCells = 1;
	thin.SaturationBase = 0;
	thin.Vapour = 255;
	CHECK(Evaluate(thin, {}, {0, 0, 0}).Codes[0] > 0);
}

TEST_CASE("weather optics and water use declared kilograms rather than relative humidity", "[weather]") {
	CellValues values;
	values.Condensate = 255;
	values.Vapour = 255;
	values.TemperatureAnomaly = 127;
	values.PressureAnomaly = -127;
	values.Wind = {-127, 127, 0};
	values.Ice = 255;
	const auto physical = ToPhysical(*Encode(values), {}, 1000);
	REQUIRE(physical);
	CHECK(physical->CondensateKilogramsPerCubicMetre == Catch::Approx(0.003f));
	CHECK(physical->VapourKilogramsPerKilogram == Catch::Approx(0.03f));
	CHECK(physical->TemperatureCelsius == Catch::Approx(40.5f));
	CHECK(physical->WindMetresPerSecond.X == -64.0f);
	CHECK(physical->WindMetresPerSecond.Y == 16.0f);
	CHECK(physical->IceFraction == 1.0f);
	CHECK(ExtinctionPerMetre(physical->CondensateKilogramsPerCubicMetre) == Catch::Approx(0.45f));
	CHECK(WaterKilograms(*physical, 10.0f, 1.0f) == Catch::Approx(0.33f));
	CHECK(ExtinctionPerMetre(-1.0f) == 0.0f);
	CHECK(WaterKilograms(*physical, -1.0f) == 0.0f);
	CHECK_FALSE(ToPhysical(*Encode(values), {}, std::numeric_limits<float>::infinity()));
	EncodingProfile invalidProfile;
	invalidProfile.BaseTemperatureCelsius = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(ToPhysical(*Encode(values), invalidProfile, 0.0f));
	CHECK(ExtinctionPerMetre(std::numeric_limits<float>::max(), 2.0f) == 0.0f);
	CHECK(ExtinctionPerMetre(1.0f, -1.0f) == 0.0f);
	CHECK(WaterKilograms(*physical, 1.0f, -1.0f) == 0.0f);
	PhysicalSample invalidSample = *physical;
	invalidSample.VapourKilogramsPerKilogram = -1.0f;
	CHECK(WaterKilograms(invalidSample, 1.0f) == 0.0f);
}

TEST_CASE("weather CPU packing comparisons declare exact and lossy output separately", "[weather]") {
	using namespace engine::weather::comparison;
	std::array<EncodedCell, 256> input{};
	for (uint32_t code = 0; code < input.size(); code++) {
		for (size_t field = 0; field < 8; field++)
			input[code].Codes[field] = static_cast<uint8_t>(SignedField(field) ? std::min(code, 254u) : code);
	}
	std::array<CellWords, 256> words{};
	std::array<uint8_t, 256 * 8> planes{};
	std::array<Fp8Cell, 256> fp8{};
	std::array<EncodedCell, 256> decoded{};
	REQUIRE(PackWords(input, words));
	REQUIRE(UnpackWords(words, decoded));
	CHECK(decoded == input);
	REQUIRE(PackPlanes(input, planes));
	REQUIRE(UnpackPlanes(planes, decoded));
	CHECK(decoded == input);
	REQUIRE(PackFp8(input, fp8));
	REQUIRE(UnpackFp8(fp8, decoded));
	uint32_t unsignedError = 0;
	uint32_t signedError = 0;
	for (size_t cell = 0; cell < input.size(); cell++) {
		REQUIRE(Decode(decoded[cell]));
		for (size_t field = 0; field < 8; field++) {
			const uint32_t error = static_cast<uint32_t>(
				std::abs(static_cast<int32_t>(input[cell].Codes[field]) - decoded[cell].Codes[field])
			);
			if (SignedField(field))
				signedError = std::max(signedError, error);
			else
				unsignedError = std::max(unsignedError, error);
		}
	}
	CHECK(unsignedError <= 8);
	CHECK(signedError <= 4);
	CHECK(unsignedError > 0);
	CHECK(signedError > 0);
	CHECK(sizeof(CellWords) == ENCODED_CELL_BYTES);
	CHECK(sizeof(Fp8Cell) == ENCODED_CELL_BYTES);
	CHECK_FALSE(PackWords(std::span(input).first(1), words));
	CHECK_FALSE(PackPlanes(input, std::span(planes).first(8)));
	input[0].Codes[1] = 255;
	CHECK_FALSE(PackFp8(input, fp8));
	fp8[0][0] = 0xff;
	CHECK_FALSE(UnpackFp8(fp8, decoded));
}

TEST_CASE("weather density upper bounds certify whole intervals over every future phase", "[weather]") {
	WeatherDefinition definition;
	definition.Coverage = 0;
	CHECK(DensityUpperBound(definition, INT32_MIN, INT32_MAX) == 0);
	definition.Coverage = 255;
	definition.Vapour = 255;
	definition.SaturationBase = 0;
	definition.SaturationLapseQ8 = 0;
	definition.TemperatureSaturationQ8 = 0;
	definition.ThicknessCells = 1;
	CHECK(DensityUpperBound(definition, 0, 0) == 254);
	CHECK(DensityUpperBound(definition, -1, -1) == 0);
	CHECK(DensityUpperBound(definition, 1, INT32_MAX) == 0);
	CHECK(DensityUpperBound(definition, -1, 1) == 254);
	definition.ThicknessCells = 2;
	CHECK(DensityUpperBound(definition, 0, 0) == 126);
	CHECK(DensityUpperBound(definition, 1, 1) == 128);
	CHECK(DensityUpperBound(definition, 0, 1) == 128);
	definition.ThicknessCells = 65536;
	CHECK(DensityUpperBound(definition, INT32_MIN, INT32_MAX) == 254);
	CHECK(DensityUpperBound(definition, 0, 0) == 0);
	definition.BaseCell = INT32_MIN;
	CHECK(DensityUpperBound(definition, INT32_MIN, INT32_MAX) == 254);
	definition.BaseCell = INT32_MAX;
	CHECK(DensityUpperBound(definition, INT32_MIN, INT32_MAX) == 0);
	definition = {};
	definition.SaturationBase = 255;
	definition.SaturationLapseQ8 = 0;
	definition.TemperatureVariation = 0;
	CHECK(DensityUpperBound(definition, -1, 26) == 0);
	CHECK_FALSE(DensityUpperBound(definition, 1, 0));
	definition.Version = ALGORITHM_VERSION + 1;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
	definition = {};
	definition.Coverage = 256;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
	definition = {};
	definition.SaturationLapseQ8 = INT32_MIN;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
	definition = {};
	definition.ThicknessCells = 0;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
	definition = {};
	definition.WindCodes[0] = 128;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
	definition = {};
	definition.WindQ20[0] = MAXIMUM_WIND_Q20 + 1;
	CHECK_FALSE(DensityUpperBound(definition, 0, 0));
}

TEST_CASE(
	"weather density interval proof bounds one million canonical samples across five seeds", "[weather]"
) {
	constexpr std::array seeds{1u, 7u, 19u, 101u, 1729u};
	uint64_t sampleCount = 0;
	for (uint32_t seed : seeds) {
		uint32_t random = seed;
		auto next = [&] {
			random = Hash(random + 0x9e3779b9u);
			return random;
		};
		for (uint32_t region = 0; region < 400; region++) {
			WeatherDefinition definition;
			definition.Seed = next();
			definition.LayerKey = next();
			definition.BaseCell = static_cast<int32_t>(next() % 65536) - 32768;
			definition.ThicknessCells = next() % 256 + 1;
			definition.FeatureShift = next() % 11;
			definition.Coverage = next() % 256;
			definition.Vapour = next() % 256;
			definition.FrontContrast = next() % 256;
			definition.Growth = next() % 256;
			definition.TemperatureVariation = next() % 128;
			definition.SaturationBase = static_cast<int32_t>(next() % 256);
			definition.SaturationLapseQ8 = static_cast<int32_t>(next() % 131073) - 65536;
			definition.TemperatureSaturationQ8 = next() % 65537;
			if (region % 8 == 0) definition.Coverage = 0;
			if (region % 8 == 1) {
				definition.Vapour = 0;
				definition.TemperatureVariation = 0;
				definition.SaturationBase = 255;
				definition.SaturationLapseQ8 = 0;
			}
			if (region % 8 == 2) {
				definition.SaturationBase = 0;
				definition.SaturationLapseQ8 = 0;
				definition.TemperatureSaturationQ8 = 0;
			}
			if (region % 8 == 3) definition.SaturationLapseQ8 = -65536;
			if (region % 8 == 4) definition.SaturationLapseQ8 = 65536;
			if (region % 8 == 5) definition.BaseCell = INT32_MIN;
			if (region % 8 == 6) definition.BaseCell = INT32_MAX - 256;
			const int32_t minimumY = static_cast<int32_t>(
				std::max(int64_t{INT32_MIN}, static_cast<int64_t>(definition.BaseCell) - next() % 16)
			);
			const int32_t maximumY = static_cast<int32_t>(std::min(
				int64_t{INT32_MAX},
				static_cast<int64_t>(definition.BaseCell) + definition.ThicknessCells + next() % 16
			));
			const auto bound = DensityUpperBound(definition, minimumY, maximumY);
			REQUIRE(bound);
			// Independent worst-case noise cube exercises the algebra before hashing imposes correlations.
			const std::array<uint32_t, 8> worstNoise{255, 255, 255, 255, 0, 128, 128, 128};
			const int32_t middleY = static_cast<int32_t>(std::clamp(
				static_cast<int64_t>(definition.BaseCell) + definition.ThicknessCells / 2,
				static_cast<int64_t>(minimumY),
				static_cast<int64_t>(maximumY)
			));
			for (int32_t y : {minimumY, middleY, maximumY}) {
				const auto model =
					engine::weather::detail::FinishWeatherSample(definition, {0, y, 0}, worstNoise);
				CAPTURE(seed, region, y, model.Codes[0], *bound);
				CHECK(model.Codes[0] <= *bound);
			}
			WeatherTime time;
			uint32_t sampledMaximum = 0;
			const uint64_t intervalSize =
				static_cast<uint64_t>(static_cast<int64_t>(maximumY) - minimumY + 1);
			for (uint32_t sample = 0; sample < 500; sample++) {
				time.PhaseTick = (uint64_t{next()} << 32u) | next();
				for (auto &displacement : time.DisplacementQ8)
					displacement = next();
				const int32_t y =
					sample == 0 ? minimumY
					: sample == 1
						? maximumY
						: static_cast<int32_t>(static_cast<int64_t>(minimumY) + next() % intervalSize);
				const CellCoordinate coordinate{
					std::bit_cast<int32_t>(next()), y, std::bit_cast<int32_t>(next())
				};
				const auto cell = Evaluate(definition, time, coordinate);
				sampledMaximum = std::max(sampledMaximum, uint32_t{cell.Codes[0]});
				if (cell.Codes[0] > *bound) {
					CAPTURE(
						seed,
						region,
						sample,
						minimumY,
						maximumY,
						coordinate.X,
						coordinate.Y,
						coordinate.Z,
						time.PhaseTick,
						cell.Codes[0],
						*bound
					);
					REQUIRE(cell.Codes[0] <= *bound);
				}
				++sampleCount;
			}
			CHECK(sampledMaximum <= *bound);
		}
	}
	CHECK(sampleCount == 1000000);
}
