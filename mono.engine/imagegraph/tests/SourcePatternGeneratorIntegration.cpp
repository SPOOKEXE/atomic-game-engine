#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>
TEST_SUITE_ID("engine.imagegraph.source_pattern_generator_integration")
using namespace engine::imagegraph;
namespace {
	Document Generator(std::string type) {
		Document document;
		document.FormatVersion = 9;
		const bool hilbert = type == "pc.hilbert";
		std::vector<AuthoredValue> values{{"dimension", Vector2{8, 8}}, {"dimension_unit", EnumValue{0}}};
		if (hilbert) {
			values.insert(
				values.end(),
				{{"iteration", int64_t{1}},
				 {"orientation", EnumValue{1}},
				 {"thickness", 2.},
				 {"bg_color", Colour{7, 8, 9, 17}},
				 {"path_color", Gradient{0, {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}}}}}
			);
		} else {
			values.insert(
				values.end(),
				{{"attribute_color_depth", EnumValue{3}},
				 {"position", Vector2{}},
				 {"position_unit", EnumValue{0}},
				 {"scale", Vector2{2, 2}},
				 {"scale_unit", EnumValue{0}},
				 {"grouping", EnumValue{0}},
				 {"color_1", Colour{17, 31, 47, 255}},
				 {"color_2", Colour{223, 199, 173, 255}}}
			);
		}
		document.Nodes.push_back({"generator", std::move(type), "", {}, std::move(values)});
		document.Outputs.push_back({"out", "generator", "surface_out"});
		return document;
	}
	void Set(Document &document, std::string_view port, Value value) {
		for (auto &authored : document.Nodes.front().Values)
			if (authored.Port == port) {
				authored.Data = std::move(value);
				return;
			}
		document.Nodes.front().Values.push_back({std::string(port), std::move(value)});
	}
	Image EvaluatePersisted(Document document) {
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		const auto compile = Compile(restored, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(compile == Status::Ok);
		Image image;
		const auto evaluate = Evaluate(restored, plan, "out", {}, image, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(evaluate == Status::Ok);
		return image;
	}
}
TEST_CASE(
	"Persisted Hilbert has independently checked path and background samples", "[source_pattern_integration]"
) {
	const Image image = EvaluatePersisted(Generator("pc.hilbert"));
	REQUIRE(image.Width == 8);
	REQUIRE(image.Height == 8);
	CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
	// The first segment ends at point 1 of 4, so its gradient progress is .25.
	const std::array<uint8_t, 4> path{191, 0, 64, 255};
	const std::array<uint8_t, 4> background{7, 8, 9, 17};
	const size_t pathPixel = (4 * 8 + 2) * 4;
	const size_t backgroundPixel = 0;
	for (size_t channel = 0; channel < 4; ++channel) {
		CHECK(image.Pixels[pathPixel + channel] == path[channel]);
		CHECK(image.Pixels[backgroundPixel + channel] == background[channel]);
	}
}
TEST_CASE("Persisted Kisrhombille has independently checked cell colours", "[source_pattern_integration]") {
	auto document = Generator("pc.kisrhombille");
	Set(document, "angle", 90.);
	const Image image = EvaluatePersisted(std::move(document));
	REQUIRE(image.Width == 8);
	REQUIRE(image.Height == 8);
	const std::array<uint8_t, 4> color1{17, 31, 47, 255};
	const std::array<uint8_t, 4> color2{223, 199, 173, 255};
	for (const auto &[pixel, expected] :
		 std::array{std::pair{size_t{0}, color1}, std::pair{size_t{2 * 8 * 4}, color2}}) {
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(image.Pixels[pixel + channel] == expected[channel]);
	}
}
TEST_CASE("Persisted Kisrhombille emits every authored source colour depth", "[source_pattern_integration]") {
	const std::array<SurfaceFormat, 7> formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (int64_t choice = 2; choice <= 8; ++choice) {
		auto document = Generator("pc.kisrhombille");
		Set(document, "attribute_color_depth", EnumValue{choice});
		const Image image = EvaluatePersisted(std::move(document));
		const auto description = DescribeSurfaceFormat(formats[size_t(choice - 2)]);
		REQUIRE(description.has_value());
		CHECK(image.Format == formats[size_t(choice - 2)]);
		CHECK(image.Pixels.size() == size_t(8 * 8 * description->BytesPerPixel));
		CHECK(ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}
TEST_CASE(
	"Persisted generator processor rows match separate scalar documents", "[source_pattern_integration]"
) {
	for (const auto type : {"pc.hilbert", "pc.kisrhombille"}) {
		auto document = Generator(type);
		const std::string_view port = std::string_view(type) == "pc.hilbert" ? "iteration" : "grouping";
		ArrayValue rows;
		rows.ElementType = ValueType::Integer;
		rows.Elements = {int64_t{1}, int64_t{2}};
		Set(document, port, std::move(rows));
		Diagnostic diagnostic;
		Document restored;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(restored, plan, "out", {}, images, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		for (size_t row = 0; row < images.Images.size(); ++row) {
			auto scalar = Generator(type);
			Set(scalar, port, int64_t(row + 1));
			CHECK(images.Images[row] == EvaluatePersisted(std::move(scalar)));
		}
	}
}
TEST_CASE("Persisted Kisrhombille refusal preserves caller output", "[source_pattern_integration]") {
	auto document = Generator("pc.kisrhombille");
	Set(document, "dimension", Vector2{0, 8});
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image output{1, 1, {8, 9, 10, 255}};
	const Image before = output;
	CHECK(Evaluate(restored, plan, "out", {}, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == before);
}

TEST_CASE(
	"Hilbert exact GPU coverage requests preserve the caller image on refusal", "[source_pattern_integration]"
) {
	const auto document = Generator("pc.hilbert");
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image output = EvaluatePersisted(document);
	const Image before = output;
	EvaluationRequest request;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(Evaluate(restored, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "generator");
	CHECK(output == before);
	REQUIRE(Evaluate(restored, plan, "out", {}, output, diagnostic) == Status::Ok);
	CHECK(output == before);
}

TEST_CASE(
	"Generator row failures preserve a populated caller batch and permit recovery",
	"[source_pattern_integration]"
) {
	for (const auto type : {"pc.hilbert", "pc.kisrhombille"}) {
		DYNAMIC_SECTION(type) {
			const auto scalar = Generator(type);
			auto document = scalar;
			ArrayValue rows;
			const bool hilbert = std::string_view(type) == "pc.hilbert";
			if (hilbert) {
				rows.ElementType = ValueType::Integer;
				rows.Elements = {int64_t{1}, int64_t{11}};
			} else {
				rows.ElementType = ValueType::Vector2;
				rows.Elements = {Vector2{2, 2}, Vector2{0, 2}};
			}
			Set(document, hilbert ? "iteration" : "scale", std::move(rows));
			Document restored;
			Diagnostic diagnostic;
			REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
			Plan plan;
			REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
			ImageArray output;
			output.Images.push_back(EvaluatePersisted(scalar));
			const ImageArray before = output;
			CHECK(
				EvaluateArray(restored, plan, "out", {}, output, diagnostic) ==
				(hilbert ? Status::LimitExceeded : Status::InvalidValue)
			);
			CHECK(diagnostic.NodeId == "generator");
			CHECK(diagnostic.Port == (hilbert ? "iteration" : "scale"));
			CHECK(output.Images == before.Images);
			CHECK(output.Items == before.Items);
			REQUIRE(Compile(scalar, plan, diagnostic) == Status::Ok);
			Image recovered;
			REQUIRE(Evaluate(scalar, plan, "out", {}, recovered, diagnostic) == Status::Ok);
			CHECK(recovered == before.Images.front());
		}
	}
}
