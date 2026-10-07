#include <engine/imagegraph/NoiseField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.noise_modes")
using namespace engine::imagegraph;

namespace {
	Value Position(uint8_t dimension) {
		if (dimension == 1) return .3125;
		if (dimension == 2) return Vector2{.3125, -.625};
		return Vector3{.3125, -.625, 1.125};
	}

	Document NoiseDocument(int64_t mode, int64_t dimension, int64_t outputType) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"noise",
			 "value.noise_field",
			 "",
			 {},
			 {{"mode", EnumValue{mode}},
			  {"dimension", EnumValue{dimension}},
			  {"output_type", EnumValue{outputType}},
			  {"seed", int64_t{37}},
			  {"frequency", 1.75},
			  {"octaves", int64_t{4}},
			  {"gain", .6}}}
		};
		if (mode == 1) document.Nodes.front().Values.push_back({"position", Position(uint8_t(dimension))});
		return document;
	}

	Status EvaluateOutput(
		const Document &document, std::string_view outputId, EvaluatedValue &value, Diagnostic &diagnostic
	) {
		Plan plan;
		const Status compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		return EvaluateValue(document, plan, std::string(outputId), {}, value, diagnostic);
	}

	void AddOutput(Document &document, std::string name, std::string node, std::string port) {
		document.Outputs.push_back({std::move(name), std::move(node), std::move(port)});
	}

	Document SampleDocument(int64_t dimension, int64_t components) {
		Document document = NoiseDocument(0, dimension, components);
		document.Nodes.push_back(
			{"sample",
			 "value.sample_noise",
			 "",
			 {},
			 {{"output_type", EnumValue{components}}, {"position", Position(uint8_t(dimension))}}}
		);
		document.Links = {{"noise", "field", "sample", "field"}};
		AddOutput(document, "sampled", "sample", "value");
		return document;
	}
}

TEST_CASE("Native noise selectors define all nine dimensional field and result shapes", "[noise_modes]") {
	constexpr std::array fieldTypes{
		std::array{ValueType::Noise1D, ValueType::Noise2D, ValueType::Noise3D},
		std::array{ValueType::Noise1DVector2, ValueType::Noise2DVector2, ValueType::Noise3DVector2},
		std::array{ValueType::Noise1DVector3, ValueType::Noise2DVector3, ValueType::Noise3DVector3}
	};
	for (int64_t components = 1; components <= 3; ++components) {
		for (int64_t dimension = 1; dimension <= 3; ++dimension) {
			Document document = NoiseDocument(0, dimension, components);
			AddOutput(document, "field", "noise", "field");
			EvaluatedValue output;
			Diagnostic diagnostic;
			REQUIRE(EvaluateOutput(document, "field", output, diagnostic) == Status::Ok);
			const auto &field = std::get<NoiseFieldValue>(output.Data);
			REQUIRE(ValidNoiseField(field));
			CHECK(NoiseFieldType(field) == fieldTypes[size_t(components - 1)][size_t(dimension - 1)]);
			CHECK(field.Data->Dimensions == dimension);
			CHECK(field.Data->Components == components);

			Document computed = NoiseDocument(1, dimension, components);
			AddOutput(computed, "value", "noise", "value");
			EvaluatedValue value;
			REQUIRE(EvaluateOutput(computed, "value", value, diagnostic) == Status::Ok);
			if (components == 1) CHECK(std::holds_alternative<double>(value.Data));
			if (components == 2) CHECK(std::holds_alternative<Vector2>(value.Data));
			if (components == 3) CHECK(std::holds_alternative<Vector3>(value.Data));
		}
	}
}

TEST_CASE("Generator sampling and computed noise agree for every field shape", "[noise_modes]") {
	for (int64_t dimension = 1; dimension <= 3; ++dimension) {
		for (int64_t components = 1; components <= 3; ++components) {
			Document sampled = SampleDocument(dimension, components);
			EvaluatedValue fromField;
			Diagnostic diagnostic;
			REQUIRE(EvaluateOutput(sampled, "sampled", fromField, diagnostic) == Status::Ok);
			if (components == 1) CHECK(std::holds_alternative<double>(fromField.Data));
			if (components == 2) CHECK(std::holds_alternative<Vector2>(fromField.Data));
			if (components == 3) CHECK(std::holds_alternative<Vector3>(fromField.Data));

			Document computed = NoiseDocument(1, dimension, components);
			AddOutput(computed, "computed", "noise", "value");
			EvaluatedValue direct;
			REQUIRE(EvaluateOutput(computed, "computed", direct, diagnostic) == Status::Ok);
			CHECK(fromField.Data == direct.Data);
		}
	}
}

TEST_CASE(
	"Vector results preserve the old scalar channel and add deterministic seeded channels", "[noise_modes]"
) {
	for (int64_t dimension = 1; dimension <= 3; ++dimension) {
		Document scalar = NoiseDocument(1, dimension, 1);
		AddOutput(scalar, "scalar", "noise", "value");
		Document vector2 = NoiseDocument(1, dimension, 2);
		AddOutput(vector2, "vector", "noise", "value");
		Document vector3 = NoiseDocument(1, dimension, 3);
		AddOutput(vector3, "vector", "noise", "value");
		EvaluatedValue oldValue, two, three, repeat;
		Diagnostic diagnostic;
		REQUIRE(EvaluateOutput(scalar, "scalar", oldValue, diagnostic) == Status::Ok);
		REQUIRE(EvaluateOutput(vector2, "vector", two, diagnostic) == Status::Ok);
		REQUIRE(EvaluateOutput(vector3, "vector", three, diagnostic) == Status::Ok);
		REQUIRE(EvaluateOutput(vector3, "vector", repeat, diagnostic) == Status::Ok);
		CHECK(std::get<Vector2>(two.Data).X == std::get<double>(oldValue.Data));
		CHECK(std::get<Vector3>(three.Data).X == std::get<double>(oldValue.Data));
		CHECK(std::get<Vector2>(two.Data).Y != std::get<Vector2>(two.Data).X);
		CHECK(std::get<Vector3>(three.Data).Y != std::get<Vector3>(three.Data).X);
		CHECK(std::get<Vector3>(three.Data).Z != std::get<Vector3>(three.Data).X);
		CHECK(three.Data == repeat.Data);
		vector3.Nodes.front().Values[3] = {"seed", int64_t{38}};
		EvaluatedValue changed;
		REQUIRE(EvaluateOutput(vector3, "vector", changed, diagnostic) == Status::Ok);
		CHECK(changed.Data != three.Data);
	}
}

TEST_CASE("Selector defaults and format nine documents survive native text roundtrip", "[noise_modes]") {
	Document oldStyle;
	oldStyle.FormatVersion = 9;
	oldStyle.Nodes = {{"noise", "value.noise_field", "", {}, {}}};
	AddOutput(oldStyle, "field", "noise", "field");
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(oldStyle), restored, diagnostic) == Status::Ok);
	CHECK(restored.FormatVersion == 9);
	EvaluatedValue output;
	REQUIRE(EvaluateOutput(restored, "field", output, diagnostic) == Status::Ok);
	const auto &field = std::get<NoiseFieldValue>(output.Data);
	REQUIRE(field.Data);
	CHECK(field.Data->Dimensions == 2);
	CHECK(field.Data->Components == 1);
	CHECK(field.Data->Seed == 0);
	CHECK(field.Data->Frequency == 1);
	CHECK(field.Data->Octaves == 1);
	CHECK(field.Data->Gain == .5);

	Document selected = NoiseDocument(1, 3, 3);
	AddOutput(selected, "value", "noise", "value");
	Document selectedRoundtrip;
	REQUIRE(Read(Write(selected), selectedRoundtrip, diagnostic) == Status::Ok);
	CHECK(selectedRoundtrip.Nodes.front().Values == selected.Nodes.front().Values);
	EvaluatedValue selectedValue;
	CHECK(EvaluateOutput(selectedRoundtrip, "value", selectedValue, diagnostic) == Status::Ok);
}

TEST_CASE("Invalid selectors, static selector links and animated selectors are rejected", "[noise_modes]") {
	for (const auto &[port, value] : std::array<std::pair<const char *, int64_t>, 6>{
			 {std::pair{"mode", int64_t{-1}},
			  {"mode", int64_t{2}},
			  {"dimension", int64_t{0}},
			  {"dimension", int64_t{4}},
			  {"output_type", int64_t{0}},
			  {"output_type", int64_t{4}}}
		 }) {
		Document document = NoiseDocument(0, 2, 1);
		auto property = std::find_if(
			document.Nodes.front().Values.begin(),
			document.Nodes.front().Values.end(),
			[&](const AuthoredValue &candidate) { return candidate.Port == port; }
		);
		REQUIRE(property != document.Nodes.front().Values.end());
		property->Data = EnumValue{value};
		AddOutput(document, "field", "noise", "field");
		Plan plan;
		Diagnostic diagnostic;
		CHECK(Compile(document, plan, diagnostic) != Status::Ok);
	}

	Plan plan;
	Diagnostic diagnostic;
	for (std::string_view selector : {"mode", "dimension", "output_type"}) {
		Document linked = NoiseDocument(0, 2, 1);
		linked.Nodes.push_back({"number", "pc.number", "", {}, {{"value", 1.0}}});
		linked.Links = {{"number", "number", "noise", std::string(selector)}};
		AddOutput(linked, "field", "noise", "field");
		CHECK(Compile(linked, plan, diagnostic) != Status::Ok);

		Document animated = NoiseDocument(0, 2, 1);
		animated.Nodes.front().SourceAnimatedInputs = {std::string(selector)};
		AddOutput(animated, "field", "noise", "field");
		CHECK(Compile(animated, plan, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.NodeId == "noise");
		CHECK(diagnostic.Port == selector);

		Document keyed = NoiseDocument(0, 2, 1);
		keyed.Keyframes = {{"noise", std::string(selector), 0, EnumValue{2}}};
		AddOutput(keyed, "field", "noise", "field");
		CHECK(Compile(keyed, plan, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.NodeId == "noise");
		CHECK(diagnostic.Port == selector);

		Document tracked = NoiseDocument(0, 2, 1);
		tracked.Tracks = {{"noise", std::string(selector)}};
		AddOutput(tracked, "field", "noise", "field");
		CHECK(Compile(tracked, plan, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.NodeId == "noise");
		CHECK(diagnostic.Port == selector);
	}

	Document invalidSample = SampleDocument(2, 1);
	invalidSample.Nodes.back().Values.front().Data = EnumValue{4};
	CHECK(Compile(invalidSample, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Port == "output_type");

	Document linkedSample = SampleDocument(2, 1);
	linkedSample.Nodes.push_back({"number", "pc.number", "", {}, {{"value", 1.0}}});
	linkedSample.Links.push_back({"number", "number", "sample", "output_type"});
	CHECK(Compile(linkedSample, plan, diagnostic) == Status::UnknownPort);

	const auto rejectsSampleAnimation = [&](const auto &makeAnimated) {
		Document sample = SampleDocument(2, 1);
		makeAnimated(sample);
		CHECK(Compile(sample, plan, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.NodeId == "sample");
		CHECK(diagnostic.Port == "output_type");
	};
	rejectsSampleAnimation([](Document &document) {
		document.Nodes.back().SourceAnimatedInputs = {"output_type"};
	});
	rejectsSampleAnimation([](Document &document) {
		document.Keyframes = {{"sample", "output_type", 0, EnumValue{2}}};
	});
	rejectsSampleAnimation([](Document &document) { document.Tracks = {{"sample", "output_type"}}; });
}

TEST_CASE("Wrong field and coordinate shapes fail graph compilation", "[noise_modes]") {
	Document wrongField = SampleDocument(2, 2);
	wrongField.Nodes.back().Values[0].Data = EnumValue{3};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(wrongField, plan, diagnostic) == Status::TypeMismatch);

	Document wrongCoordinates = NoiseDocument(1, 1, 1);
	wrongCoordinates.Nodes.front().Values.back().Data = Vector2{.25, .5};
	AddOutput(wrongCoordinates, "value", "noise", "value");
	CHECK(Compile(wrongCoordinates, plan, diagnostic) == Status::TypeMismatch);

	Document wrongComputedVector = NoiseDocument(1, 3, 1);
	wrongComputedVector.Nodes.front().Values.erase(
		std::remove_if(
			wrongComputedVector.Nodes.front().Values.begin(),
			wrongComputedVector.Nodes.front().Values.end(),
			[](const AuthoredValue &value) { return value.Port == "position"; }
		),
		wrongComputedVector.Nodes.front().Values.end()
	);
	wrongComputedVector.Nodes.push_back({"coordinate", "pc.vector2", "", {}, {{"x", .25}, {"y", .5}}});
	wrongComputedVector.Links = {{"coordinate", "vector", "noise", "position"}};
	AddOutput(wrongComputedVector, "value", "noise", "value");
	CHECK(Compile(wrongComputedVector, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "noise");
	CHECK(diagnostic.Port == "position");
}

TEST_CASE("Noise recipes enforce coordinate and control bounds", "[noise_modes]") {
	Document coordinate = NoiseDocument(1, 1, 1);
	coordinate.Nodes.front().Values.back().Data = 0x1p31;
	AddOutput(coordinate, "value", "noise", "value");
	EvaluatedValue output;
	Diagnostic diagnostic;
	CHECK(EvaluateOutput(coordinate, "value", output, diagnostic) == Status::InvalidValue);

	for (const auto &[port, value] :
		 std::array<std::pair<const char *, double>, 2>{{std::pair{"frequency", 8193.0}, {"gain", 1.01}}}) {
		Document invalid = NoiseDocument(0, 2, 1);
		auto property = std::find_if(
			invalid.Nodes.front().Values.begin(),
			invalid.Nodes.front().Values.end(),
			[&](const AuthoredValue &candidate) { return candidate.Port == port; }
		);
		REQUIRE(property != invalid.Nodes.front().Values.end());
		property->Data = value;
		AddOutput(invalid, "field", "noise", "field");
		CHECK(EvaluateOutput(invalid, "field", output, diagnostic) == Status::InvalidValue);
	}
	Document tooManyOctaves = NoiseDocument(0, 2, 1);
	tooManyOctaves.Nodes.front().Values[5].Data = int64_t{17};
	AddOutput(tooManyOctaves, "field", "noise", "field");
	CHECK(EvaluateOutput(tooManyOctaves, "field", output, diagnostic) == Status::InvalidValue);
}
