#include <engine/imagegraph/NoiseField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <tuple>

TEST_SUITE_ID("engine.imagegraph.noise_field")
using namespace engine::imagegraph;

namespace {
	NoiseFieldValue Recipe(uint8_t dimensions, int64_t seed = 17) {
		NoiseFieldValue field;
		auto &data = field.Data.emplace();
		data.Dimensions = dimensions;
		data.Seed = seed;
		data.Frequency = 1.75;
		data.Octaves = 4;
		data.Gain = .6;
		return field;
	}

	Status EvaluateValueOutput(
		const Document &document, std::string_view name, EvaluatedValue &value, Diagnostic &diagnostic
	) {
		Plan plan;
		const Status compiled = Compile(document, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		return EvaluateValue(document, plan, std::string(name), {}, value, diagnostic);
	}
}

TEST_CASE("Noise recipes sample deterministically in one, two and three dimensions", "[noise_field]") {
	const std::array<double, 1> point1{.125};
	const std::array<double, 2> point2{.125, -.75};
	const std::array<double, 3> point3{.125, -.75, 1.5};
	for (const auto &[field, point] : std::array{
			 std::pair{Recipe(1), std::span<const double>(point1)},
			 std::pair{Recipe(2), std::span<const double>(point2)},
			 std::pair{Recipe(3), std::span<const double>(point3)}
		 }) {
		double first = -1, repeat = -1;
		REQUIRE(ValidNoiseField(field));
		REQUIRE(SampleNoiseField(field, point, first));
		REQUIRE(SampleNoiseField(field, point, repeat));
		CHECK(first == repeat);
		CHECK(first >= 0);
		CHECK(first <= 1);
	}
	auto otherSeed = Recipe(2, 18);
	double original = 0, changed = 0;
	REQUIRE(SampleNoiseField(Recipe(2), point2, original));
	REQUIRE(SampleNoiseField(otherSeed, point2, changed));
	CHECK(original != changed);
}

TEST_CASE("Noise recipes reject invalid controls and malformed sample coordinates", "[noise_field]") {
	NoiseFieldValue empty;
	CHECK_FALSE(ValidNoiseField(empty));
	for (const uint8_t dimensions : {uint8_t{0}, uint8_t{4}})
		CHECK_FALSE(ValidNoiseField(Recipe(dimensions)));
	for (const auto &[octaves, frequency, gain] :
		 {std::tuple<int64_t, double, double>{0, 1, .5},
		  {17, 1, .5},
		  {1, 0, .5},
		  {1, 8193, .5},
		  {1, std::numeric_limits<double>::infinity(), .5},
		  {1, 1, -.01},
		  {1, 1, 1.01}}) {
		auto invalid = Recipe(2);
		invalid.Data->Octaves = octaves;
		invalid.Data->Frequency = frequency;
		invalid.Data->Gain = gain;
		CHECK_FALSE(ValidNoiseField(invalid));
	}
	const std::array<double, 1> wrongDimension{.2};
	const std::array<double, 2> nonFinite{.2, std::numeric_limits<double>::quiet_NaN()};
	const std::array<double, 2> tooLarge{.2, 0x1p31};
	double value = 123;
	CHECK_FALSE(SampleNoiseField(Recipe(2), wrongDimension, value));
	CHECK_FALSE(SampleNoiseField(Recipe(2), nonFinite, value));
	CHECK_FALSE(SampleNoiseField(Recipe(2), tooLarge, value));
}

TEST_CASE("Noise fields own deep copies of raster data", "[noise_field]") {
	NoiseFieldValue original = Recipe(2);
	auto &data = *original.Data;
	data.Raster = Image{2, 1, {0, 0, 0, 0, 255, 0, 0, 0}, 0};
	NoiseFieldValue copy = original;
	REQUIRE(copy.Data);
	REQUIRE(copy.Data->Raster);
	copy.Data->Raster->Pixels[0] = 255;
	CHECK(original.Data->Raster->Pixels[0] == 0);
	const std::array<double, 2> left{0, 0}, right{1, 0};
	double leftValue = 0, rightValue = 0;
	REQUIRE(SampleNoiseField(original, left, leftValue));
	REQUIRE(SampleNoiseField(original, right, rightValue));
	CHECK(leftValue == 0);
	CHECK(rightValue == 1);
	CHECK(NoiseFieldType(original) == ValueType::Noise2D);
}

TEST_CASE("Noise field value nodes generate and sample matching dimensional values", "[noise_field]") {
	for (const int64_t dimension : {1, 2, 3}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"field",
			 "value.noise_field",
			 "",
			 {},
			 {{"dimension", EnumValue{dimension}},
			  {"seed", int64_t{11}},
			  {"frequency", 2.0},
			  {"octaves", int64_t{3}},
			  {"gain", .4}}},
			{"sample", "value.sample_noise", "", {}, {}}
		};
		if (dimension == 1) document.Nodes[1].Values.push_back({"position", .25});
		if (dimension == 2) document.Nodes[1].Values.push_back({"position", Vector2{.25, -.5}});
		if (dimension == 3) document.Nodes[1].Values.push_back({"position", Vector3{.25, -.5, 1.0}});
		document.Links = {{"field", "field", "sample", "field"}};
		document.Outputs = {{"value", "sample", "value"}, {"noise", "field", "field"}};
		EvaluatedValue sample, generated;
		Diagnostic diagnostic;
		REQUIRE(EvaluateValueOutput(document, "value", sample, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValueOutput(document, "noise", generated, diagnostic) == Status::Ok);
		const auto &field = std::get<NoiseFieldValue>(generated.Data);
		REQUIRE(ValidNoiseField(field));
		CHECK(
			NoiseFieldType(field) == (dimension == 1   ? ValueType::Noise1D
									  : dimension == 2 ? ValueType::Noise2D
													   : ValueType::Noise3D)
		);
		std::array<double, 3> coordinates{.25, -.5, 1.0};
		double sampled = 0;
		REQUIRE(
			SampleNoiseField(field, std::span(coordinates).first(static_cast<size_t>(dimension)), sampled)
		);
		CHECK(std::get<double>(sample.Data) == sampled);
	}
}

TEST_CASE("Noise sampler reports coordinate dimension mismatch", "[noise_field]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"field", "value.noise_field", "", {}, {{"dimension", EnumValue{2}}}},
		{"sample", "value.sample_noise", "", {}, {{"position", .25}}}
	};
	document.Links = {{"field", "field", "sample", "field"}};
	document.Outputs = {{"value", "sample", "value"}};
	EvaluatedValue value;
	Diagnostic diagnostic;
	CHECK(EvaluateValueOutput(document, "value", value, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.NodeId == "sample");
	CHECK(diagnostic.Port == "position");
}

TEST_CASE("Noise generator accepts integral linked numeric controls", "[noise_field]") {
	Document linked;
	linked.FormatVersion = 9;
	linked.Nodes = {
		{"seed", "pc.number", "", {}, {{"value", 29.0}}},
		{"frequency", "pc.number", "", {}, {{"value", 3.0}}},
		{"field", "value.noise_field", "", {}, {{"dimension", EnumValue{1}}}}
	};
	linked.Links = {{"seed", "number", "field", "seed"}, {"frequency", "number", "field", "frequency"}};
	linked.Outputs = {{"field", "field", "field"}};
	Document literal = linked;
	literal.Nodes.resize(1);
	literal.Links.clear();
	literal.Nodes[0] = {
		"field",
		"value.noise_field",
		"",
		{},
		{{"dimension", EnumValue{1}}, {"seed", int64_t{29}}, {"frequency", 3.0}}
	};
	literal.Outputs = {{"field", "field", "field"}};
	EvaluatedValue linkedValue, literalValue;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValueOutput(linked, "field", linkedValue, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValueOutput(literal, "field", literalValue, diagnostic) == Status::Ok);
	const auto &fromLinks = std::get<NoiseFieldValue>(linkedValue.Data);
	const auto &fromLiterals = std::get<NoiseFieldValue>(literalValue.Data);
	CHECK(fromLinks == fromLiterals);
}

TEST_CASE("Simplex image field is emitted only when requested and retains image pixels", "[noise_field]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"noise",
		 "image.noise_simplex",
		 "",
		 {},
		 {{"width", int64_t{4}}, {"height", int64_t{3}}, {"seed", 13.0}}}
	};
	document.Outputs = {{"image", "noise", "image"}, {"field", "noise", "field"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(document, plan, "image", image, diagnostic) == Status::Ok);
	EvaluatedValue fieldOutput;
	REQUIRE(EvaluateValue(document, plan, "field", {}, fieldOutput, diagnostic) == Status::Ok);
	const auto &field = std::get<NoiseFieldValue>(fieldOutput.Data);
	REQUIRE(field.Data->Raster);
	CHECK(field.Data->Raster->Width == image.Width);
	CHECK(field.Data->Raster->Height == image.Height);
	CHECK(field.Data->Raster->Format == image.Format);
	CHECK(field.Data->Raster->Pixels == image.Pixels);
	CHECK(field.Data->Raster->Hash == image.Hash);
}

TEST_CASE("Noise generator rejects fractional integer controls without throwing", "[noise_field]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"seed", "pc.number", "", {}, {{"value", 29.8}}}, {"field", "value.noise_field", "", {}, {}}
	};
	document.Links = {{"seed", "number", "field", "seed"}};
	document.Outputs = {{"out", "field", "field"}};
	EvaluatedValue value;
	Diagnostic diagnostic;
	CHECK(EvaluateValueOutput(document, "out", value, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "seed");
}

TEST_CASE("Source noise fields sample the same raster as its selected image output", "[noise_field]") {
	for (const auto *type :
		 {"pc.noise_simplex", "pc.perlin", "pc.cellular", "pc.voronoi_extra", "pc.shard_noise"}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"source",
			 type,
			 "",
			 {},
			 {{"dimension", Vector2{3, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"seed", 123.0},
			  {"iteration", int64_t{2}}}},
			{"sample", "value.sample_noise", "", {}, {{"position", Vector2{.25, .75}}}}
		};
		if (std::string_view(type) == "pc.voronoi_extra" || std::string_view(type) == "pc.shard_noise")
			document.Nodes[0].Values.pop_back();
		if (std::string_view(type) == "pc.shard_noise") {
			auto &controls = document.Nodes[0].Values;
			controls.push_back({"scale_mapped", true});
			controls.push_back({"progress_mapped", true});
			controls.push_back({"sharpness_mapped", true});
			controls.push_back({"progress", 0.0});
			controls.push_back({"sharpness", 1.0});
		}
		document.Links = {{"source", "field", "sample", "field"}};
		document.Outputs = {{"image", "source", "surface_out"}, {"sample", "sample", "value"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		Image image;
		REQUIRE(Evaluate(document, plan, "image", image, diagnostic) == Status::Ok);
		EvaluatedValue sampled;
		REQUIRE(EvaluateValue(document, plan, "sample", {}, sampled, diagnostic) == Status::Ok);
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(image, 0, 1, pixel));
		CHECK(std::get<double>(sampled.Data) == pixel[0]);
	}
}

TEST_CASE("Source noise processor arrays retain fields and sampler diagnoses field arrays", "[noise_field]") {
	for (const auto *type :
		 {"pc.noise_simplex", "pc.perlin", "pc.cellular", "pc.voronoi_extra", "pc.shard_noise"}) {
		Document processor;
		processor.FormatVersion = 9;
		ArrayValue iterations;
		iterations.ElementType = ValueType::Integer;
		iterations.Elements = {int64_t{1}, int64_t{2}};
		processor.Nodes = {
			{"source",
			 type,
			 "",
			 {},
			 {{"dimension", Vector2{3, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"seed", 123.0},
			  {"iteration", iterations}}}
		};
		if (std::string_view(type) == "pc.voronoi_extra")
			processor.Nodes[0].Values.back() = {"progress", ArrayValue{ValueType::Scalar, {0.0, 1.0}}};
		if (std::string_view(type) == "pc.shard_noise")
			processor.Nodes[0].Values.back() = {"rotation", ArrayValue{ValueType::Scalar, {0.0, 1.0}}};
		if (std::string_view(type) == "pc.shard_noise") {
			auto &controls = processor.Nodes[0].Values;
			controls.insert(
				controls.begin(),
				{{"scale_mapped", true},
				 {"progress_mapped", true},
				 {"sharpness_mapped", true},
				 {"sharpness", 1.0}}
			);
		}
		processor.Outputs = {{"images", "source", "surface_out"}, {"fields", "source", "field"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(processor, plan, diagnostic) == Status::Ok);
		ImageArray images;
		REQUIRE(EvaluateArray(processor, plan, "images", {}, images, diagnostic) == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		for (size_t row = 0; row < images.Images.size(); ++row) {
			Document scalar = processor;
			scalar.Nodes[0].Values.back().Data =
				(std::string_view(type) == "pc.voronoi_extra" || std::string_view(type) == "pc.shard_noise")
					? Value{double(row)}
					: Value{int64_t(row + 1)};
			Plan scalarPlan;
			REQUIRE(Compile(scalar, scalarPlan, diagnostic) == Status::Ok);
			Image expected;
			REQUIRE(Evaluate(scalar, scalarPlan, "images", expected, diagnostic) == Status::Ok);
			CHECK(images.Images[row] == expected);
		}
		EvaluatedValue fields;
		REQUIRE(EvaluateValue(processor, plan, "fields", {}, fields, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(fields.Data);
		REQUIRE(array.Elements.size() == images.Images.size());
		CHECK(array.ElementType == ValueType::Noise2D);
		for (size_t row = 0; row < array.Elements.size(); ++row) {
			const auto &field = std::get<NoiseFieldValue>(array.Elements[row]);
			REQUIRE(field.Data->Raster);
			CHECK(field.Data->Raster->Pixels == images.Images[row].Pixels);
		}
		Document sampler = processor;
		sampler.Nodes.push_back({"sample", "value.sample_noise", "", {}, {{"position", Vector2{.25, .75}}}});
		sampler.Links = {{"source", "field", "sample", "field"}};
		sampler.Outputs = {{"value", "sample", "value"}};
		EvaluatedValue value;
		CHECK(EvaluateValueOutput(sampler, "value", value, diagnostic) == Status::InvalidValue);
		CHECK(diagnostic.NodeId == "sample");
		CHECK(diagnostic.Port == "field");
	}
}
