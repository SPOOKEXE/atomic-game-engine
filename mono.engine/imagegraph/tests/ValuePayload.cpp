#include "ValuePayload.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.value_payload")

using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE(
	"runtime array leaves retain rich value types and owned byte counts", "[imagegraph][value_payload]"
) {
	const std::vector<Value> values{
		true,
		int64_t{42},
		.25,
		std::string{"leaf"},
		Colour{},
		Vector2{},
		Gradient{0, {{2, {}}}},
		Area{},
		Curve{{}, {{}, {}}},
		Vector4{},
		Path2D{false, {{}, {}}, {{}}},
		Vector3{},
		Quaternion{},
		EnumValue{2},
		AudioBit{{1, 2}, 10},
		MatrixValue{2, 1, {1, 2}}
	};
	for (const auto &value : values) {
		REQUIRE(ValidRuntimeValue(value));
		const auto leaf = ArrayElement(value);
		REQUIRE(leaf);
		ArrayValue array{PayloadType(value), {*leaf}, {}};
		CHECK(ValidRuntimeValue(array));
		CHECK(
			ValuePayloadBytes(array) ==
			sizeof(Value) + sizeof(ElementValue) + ValuePayloadBytes(value) - sizeof(Value)
		);
		array.Nested = {array.Elements, {}};
		array.Elements.clear();
		CHECK(ValidRuntimeValue(array));
		CHECK_FALSE(ValidValuePayload(array, false));
		CHECK(
			ValuePayloadBytes(array) == sizeof(Value) + 2 * sizeof(std::vector<ElementValue>) +
											sizeof(ElementValue) + ValuePayloadBytes(value) - sizeof(Value)
		);
	}
	CHECK_FALSE(ArrayElement(Value{ArrayValue{}}));
}

TEST_CASE(
	"runtime payload guards shape counts numeric fields and aggregate bytes", "[imagegraph][value_payload]"
) {
	ArrayValue array{ValueType::Scalar, {}, {{1.0}, {}}};
	REQUIRE(ValidRuntimeValue(array));
	array.Elements.emplace_back(2.0);
	CHECK_FALSE(ValidRuntimeValue(array));
	array.Elements.clear();
	array.Nested.front().front() = true;
	CHECK_FALSE(ValidRuntimeValue(array));
	array.Nested = std::vector<std::vector<ElementValue>>(Limits::MaximumArrayElements);
	CHECK(ValidRuntimeValue(array));
	array.Nested.emplace_back();
	CHECK_FALSE(ValidRuntimeValue(array));
	array.Nested = {{}};
	array.Nested.front().resize(Limits::MaximumArrayElements, 1.0);
	CHECK(ValidRuntimeValue(array));
	array.Nested.push_back({1.0});
	CHECK_FALSE(ValidRuntimeValue(array));
	array = {ValueType::Text, {}, {}};
	array.Elements.resize(4, std::string(Limits::MaximumTextBytes, 'a'));
	CHECK_FALSE(ValidRuntimeValue(array));
	array.Elements.resize(3);
	CHECK(ValidRuntimeValue(array));
	const double nan = std::numeric_limits<double>::quiet_NaN();
	for (const Value &invalid : std::vector<Value>{
			 nan,
			 Vector2{nan, 0},
			 Vector3{0, nan, 0},
			 Vector4{0, 0, nan, 0},
			 Quaternion{0, 0, 0, nan},
			 Gradient{0, {{nan, {}}}},
			 Area{nan},
			 Curve{{nan}, {{}, {}}},
			 Path2D{false, {{{nan}, 0}}, {}},
			 MatrixValue{1, 1, {nan}},
			 AudioBit{{nan}, 10}
		 }) {
		CHECK_FALSE(ValidRuntimeValue(invalid));
		const auto leaf = ArrayElement(invalid);
		REQUIRE(leaf);
		CHECK_FALSE(ValidRuntimeValue(ArrayValue{PayloadType(invalid), {*leaf}, {}}));
	}
	CHECK(ValidRuntimeValue(Gradient{}));
	CHECK_FALSE(ValidValuePayload(Gradient{}, false));
	CHECK(ValidRuntimeValue(Gradient{0, {{2, {}}, {-1, {}}}}));
	CHECK_FALSE(ValidValuePayload(Gradient{0, {{2, {}}, {-1, {}}}}, false));
	CHECK_FALSE(ValidRuntimeValue(Gradient{7, {}}));
	CHECK_FALSE(ValidRuntimeValue(MatrixValue{2, 1, {1}}));
	CHECK_FALSE(ValidRuntimeValue(AudioBit{{1}, 10, {{1}}}));
}

TEST_CASE(
	"runtime audio clips and capture frames keep separate aggregate limits", "[imagegraph][value_payload]"
) {
	AudioBit clip{
		{},
		48000,
		{std::vector<double>(Limits::MaximumAudioClipSamples / 2),
		 std::vector<double>(Limits::MaximumAudioClipSamples / 2)}
	};
	CHECK(ValidRuntimeValue(clip));
	CHECK_FALSE(ValidAudioPlanes(clip.Samples, clip.Channels));
	CHECK(
		ValuePayloadBytes(clip) ==
		sizeof(Value) + Limits::MaximumAudioClipSamples * sizeof(double) + 2 * sizeof(std::vector<double>)
	);
	clip.Channels.front().push_back(0);
	clip.Channels.back().push_back(0);
	CHECK_FALSE(ValidRuntimeValue(clip));
	AudioBit frame{std::vector<double>(Limits::MaximumAudioSamplesPerFrame), 48000};
	CHECK(ValidAudioPlanes(frame.Samples, frame.Channels));
	frame.Samples.push_back(0);
	CHECK_FALSE(ValidAudioPlanes(frame.Samples, frame.Channels));
	CHECK(ValidRuntimeValue(frame));
}

TEST_CASE(
	"empty runtime arrays support explicit general shape and representable leaf types",
	"[imagegraph][value_payload]"
) {
	CHECK(ValidRuntimeValue(ArrayValue{ValueType::Any, {}, {}}));
	CHECK(ValidRuntimeValue(ArrayValue{ValueType::Any, {}, {{}}}));
	for (ValueType type :
		 {ValueType::Gradient,
		  ValueType::Path2D,
		  ValueType::Matrix,
		  ValueType::AudioBit,
		  ValueType::Image,
		  ValueType::Mesh,
		  ValueType::Buffer,
		  ValueType::NodeRef,
		  ValueType::Struct}) {
		CHECK(ValidRuntimeValue(ArrayValue{type, {}, {}}));
		CHECK(ValidRuntimeValue(ArrayValue{type, {}, {{}}}));
	}
	for (ValueType type : {ValueType::Object, ValueType::Array, static_cast<ValueType>(255)}) {
		CHECK_FALSE(ValidRuntimeValue(ArrayValue{type, {}, {}}));
		CHECK_FALSE(ValidRuntimeValue(ArrayValue{type, {}, {{}}}));
	}
}

TEST_CASE("preflighted rich array leaves move their owned payload", "[imagegraph][value_payload]") {
	Value value = AudioBit{std::vector<double>(8192, .25), 48000};
	REQUIRE(ValidRuntimeValue(value));
	const double *original = std::get<AudioBit>(value).Samples.data();
	const auto leaf = ArrayElement(std::move(value));
	REQUIRE(leaf);
	CHECK(std::get<AudioBit>(*leaf).Samples.data() == original);
	CHECK(std::get<AudioBit>(*leaf).Samples.size() == 8192);
	CHECK_FALSE(ArrayElement(Value{ArrayValue{}}));
}

TEST_CASE(
	"Weighted selector owns bounded runtime-only state and deep copies its contents",
	"[imagegraph][value_payload]"
) {
	ArraySelectorValue selector;
	selector.Data.emplace();
	selector.Data->Values = ArrayValue{ValueType::Scalar, {10.0, 20.0, 30.0}};
	selector.Data->CumulativeWeights = {0, 1, 2};
	selector.Data->TotalWeight = 3;
	CHECK(ValidRuntimeValue(selector));
	CHECK_FALSE(detail::ValidPayload(selector, false));
	auto copy = selector;
	copy.Data->Values.Elements[0] = 40.0;
	CHECK(std::get<double>(selector.Data->Values.Elements[0]) == 10);
	CHECK(
		detail::RetainedPayloadBytes(selector) >=
		sizeof(ArraySelectorData) + 3 * sizeof(double) + 3 * sizeof(ElementValue)
	);
	selector.Data->CumulativeWeights.pop_back();
	CHECK_FALSE(ValidRuntimeValue(selector));
}

TEST_CASE("general array image leaves require finite float samples", "[imagegraph][value_payload]") {
	Image image;
	image.Width = image.Height = 1;
	image.Format = SurfaceFormat::RGBA32Float;
	image.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(image, 0, 0, {.25, .5, 1, 1}));
	ArrayValue array;
	array.ElementType = ValueType::Any;
	array.Items.push_back(SourceArrayItem{image});
	CHECK(ValidRuntimeValue(array));
	for (double sample :
		 {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
		const uint32_t word = std::bit_cast<uint32_t>(static_cast<float>(sample));
		for (size_t byte = 0; byte < 4; ++byte)
			image.Pixels[byte] = static_cast<uint8_t>(word >> (byte * 8));
		array.Items[0] = SourceArrayItem{image};
		CHECK_FALSE(ValidRuntimeValue(array));
	}
}
