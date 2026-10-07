#include <engine/imagegraph/NoiseField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.raster_noise_fields")
using namespace engine::imagegraph;

TEST_CASE("Raster field shapes sample red RG and RGB without changing coordinates", "[noise_field]") {
	for (uint8_t components = 1; components <= 3; ++components) {
		NoiseFieldValue field;
		auto &data = field.Data.emplace();
		data.Components = components;
		data.Raster = Image{2, 1, {32, 64, 96, 255, 128, 160, 192, 255}, 0};
		REQUIRE(ValidNoiseField(field));
		CHECK(NoiseFieldType(field) == *NoiseFieldType(2, components));
		std::array<double, 3> values{};
		const std::array<double, 2> left{-1, 0}, right{2, 1};
		REQUIRE(SampleNoiseField(field, left, std::span(values).first(components)));
		for (size_t channel = 0; channel < components; ++channel)
			CHECK(values[channel] == Catch::Approx(double(32 + 32 * channel) / 255));
		REQUIRE(SampleNoiseField(field, right, std::span(values).first(components)));
		for (size_t channel = 0; channel < components; ++channel)
			CHECK(values[channel] == Catch::Approx(double(128 + 32 * channel) / 255));
		CHECK_FALSE(SampleNoiseField(field, std::array<double, 1>{0}, std::span(values).first(components)));
		CHECK_FALSE(SampleNoiseField(field, left, std::span<double>{}));
		double scalar = 0;
		CHECK(SampleNoiseField(field, left, scalar) == (components == 1));
		data.Dimensions = 3;
		CHECK_FALSE(ValidNoiseField(field));
	}
}

TEST_CASE(
	"Image noise instance field sockets preserve scalar defaults and exact vector shapes", "[noise_field]"
) {
	for (const auto type : {"image.noise_simplex", "pc.noise_simplex", "pc.voronoi_extra"}) {
		Node node;
		node.Type = type;
		REQUIRE(IsNoiseImageGenerator(type));
		CHECK(NoiseGeneratorOutputType(node) == ValueType::Noise2D);
		for (int64_t components = 1; components <= 3; ++components) {
			node.Values = {{"output_type", EnumValue{components}}};
			NoiseNodeChoices choices;
			std::string_view failed;
			REQUIRE(ResolveNoiseNodeChoices(node, choices, failed));
			CHECK(choices.Dimensions == 2);
			CHECK(choices.Components == components);
			const auto port = NoiseNodePort(node, "field", PortDirection::Output);
			REQUIRE(port);
			CHECK(port->Type == *NoiseFieldType(2, uint8_t(components)));
			CHECK_FALSE(NoiseNodePort(node, "position", PortDirection::Input));
		}
		for (const Value invalid : {Value{EnumValue{0}}, Value{EnumValue{4}}, Value{2.0}}) {
			node.Values = {{"output_type", invalid}};
			NoiseNodeChoices choices;
			std::string_view failed;
			CHECK_FALSE(ResolveNoiseNodeChoices(node, choices, failed));
			CHECK(failed == "output_type");
			CHECK_FALSE(NoiseGeneratorOutputType(node));
		}
	}
}

TEST_CASE("Selected raster shapes survive saves and sample exactly the image channels", "[noise_field]") {
	for (const auto type : {"image.noise_simplex", "pc.noise_simplex"})
		for (int64_t components = 1; components <= 3; ++components) {
			Document document;
			document.FormatVersion = 9;
			Node source;
			source.Id = "source";
			source.Type = type;
			source.Values = {{"output_type", EnumValue{components}}, {"seed", 13.0}};
			const bool native = source.Type == "image.noise_simplex";
			if (native) {
				source.Values.push_back({"width", int64_t{2}});
				source.Values.push_back({"height", int64_t{2}});

			} else {
				source.Values.push_back({"dimension", Vector2{2, 2}});
				source.Values.push_back({"dimension_unit", EnumValue{0}});
				source.Values.push_back({"color_mode", EnumValue{1}});
			}
			document.Nodes = {
				source,
				{"sample",
				 "value.sample_noise",
				 "",
				 {},
				 {{"position", Vector2{.25, .75}}, {"output_type", EnumValue{components}}}}
			};
			document.Links = {{"source", "field", "sample", "field"}};
			document.Outputs = {
				{"image", "source", native ? "image" : "surface_out"},
				{"field", "source", "field"},
				{"sample", "sample", "value"}
			};
			Document restored;
			Diagnostic diagnostic;
			INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
			REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
			Plan plan;
			REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
			Image image;
			REQUIRE(Evaluate(restored, plan, "image", image, diagnostic) == Status::Ok);
			EvaluatedValue fieldValue, sample;
			REQUIRE(EvaluateValue(restored, plan, "field", {}, fieldValue, diagnostic) == Status::Ok);
			REQUIRE(EvaluateValue(restored, plan, "sample", {}, sample, diagnostic) == Status::Ok);
			const auto &field = std::get<NoiseFieldValue>(fieldValue.Data);
			CHECK(field.Data->Components == components);
			REQUIRE(fieldValue.Domain);
			CHECK(fieldValue.Domain->Type == *NoiseFieldType(2, uint8_t(components)));
			CHECK(field.Data->Dimensions == 2);
			SurfacePixel pixel;
			REQUIRE(LoadSurfacePixel(image, 0, 1, pixel));
			if (components == 1) CHECK(std::get<double>(sample.Data) == pixel[0]);
			if (components == 2) {
				const auto result = std::get<Vector2>(sample.Data);
				CHECK(result.X == pixel[0]);
				CHECK(result.Y == pixel[1]);
			}
			if (components == 3) {
				const auto result = std::get<Vector3>(sample.Data);
				CHECK(result.X == pixel[0]);
				CHECK(result.Y == pixel[1]);
				CHECK(result.Z == pixel[2]);
			}
			restored.Nodes[1].Values.back().Data = EnumValue{components == 3 ? 1 : components + 1};
			CHECK(Compile(restored, plan, diagnostic) == Status::TypeMismatch);
			CHECK(restored.Links.size() == 1);
		}
}

TEST_CASE("Raster result shape inherits from the base until explicitly overridden", "[noise_field]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"base",
		 "pc.noise_simplex",
		 "",
		 {},
		 {{"output_type", EnumValue{3}},
		  {"dimension", Vector2{2, 2}},
		  {"dimension_unit", EnumValue{0}},
		  {"seed", 13.0}}},
		{"instance", "pc.noise_simplex", "", {}, {{"output_type", EnumValue{1}}}}
	};
	document.Nodes[1].InstanceBase = "base";
	document.Outputs = {{"field", "instance", "field"}};
	Diagnostic diagnostic;
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "field", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<NoiseFieldValue>(result.Data).Data->Components == 3);
	CHECK(
		NoiseNodePort(document.Nodes[1], "field", PortDirection::Output, &document)->Type ==
		ValueType::Noise2DVector3
	);
	document.Nodes[1].InstanceOverrides = {"output_type"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(document, plan, "field", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<NoiseFieldValue>(result.Data).Data->Components == 1);
	document.Nodes[1].SourceAnimatedInputs = {"output_type"};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "output_type");
}

TEST_CASE("Processor rows publish arrays with the exact selected raster signature", "[noise_field]") {
	for (int64_t components : {2, 3}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"noise",
			 "pc.noise_simplex",
			 "",
			 {},
			 {{"dimension", Vector2{2, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"output_type", EnumValue{components}},
			  {"seed", 13.0},
			  {"iteration", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}}}}}}
		};
		document.Outputs = {{"fields", "noise", "field"}};
		Plan plan;
		Diagnostic diagnostic;
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue fields;
		REQUIRE(EvaluateValue(document, plan, "fields", {}, fields, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(fields.Data);
		REQUIRE(array.Elements.size() == 2);
		CHECK(array.ElementType == *NoiseFieldType(2, uint8_t(components)));
		REQUIRE(fields.Domain);
		CHECK(fields.Domain->Type == array.ElementType);
		for (const auto &element : array.Elements) {
			const auto &field = std::get<NoiseFieldValue>(element);
			CHECK(field.Data->Components == components);
			REQUIRE(field.Data->Raster);
		}
	}
}
