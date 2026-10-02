#include "../src/PixelBuilderPayload.hpp"
#include "../src/SourceBuiltinRandomContext.hpp"

#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_builtin_random_images")
using namespace engine::imagegraph;
namespace {
	SourceBuiltinRandomCapture ImageCapture() {
		SourceBuiltinRandomCapture capture;
		capture.Authored = {"observed", "pc.blur_radial", "", {}, {}};
		capture.InputImages = {{"surface_in", Image{1, 1, {91, 32, 17, 255}}}};
		return capture;
	}
}
TEST_CASE(
	"Builtin RNG image recordings validate typed layout, duplicate ports and retained capacity",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture = ImageCapture();
	Diagnostic diagnostic;
	uint64_t bytes = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	const auto cost = bytes;
	CHECK(ValidateBuiltinRandomCaptures({&capture, 1}, cost - 1, bytes, diagnostic) == Status::LimitExceeded);
	capture.InputImages.front().Data.Pixels.reserve(2 * 1024 * 1024);
	CHECK(
		ValidateBuiltinRandomCaptures({&capture, 1}, 1024 * 1024, bytes, diagnostic) == Status::LimitExceeded
	);
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	CHECK(bytes >= 2 * 1024 * 1024);
	capture = ImageCapture();
	capture.InputImages.push_back(capture.InputImages.front());
	CHECK(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::DuplicateId
	);
	capture = ImageCapture();
	capture.InputImages.front().Data.Pixels.pop_back();
	CHECK(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::InvalidValue
	);
	capture = ImageCapture();
	capture.InputImages.front().Data.Format = SurfaceFormat::R32Float;
	capture.InputImages.front().Data.Pixels = {0, 0, 192, 127};
	CHECK(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::InvalidValue
	);
}
TEST_CASE(
	"Builtin RNG image binding checks source pixels and requires a complete closure",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture = ImageCapture();
	EvaluationRequest request;
	request.BuiltinRandomCaptures = {&capture, 1};
	const auto *entry = FindCatalogueEntry(capture.Authored.Type);
	REQUIRE(entry);
	Image image = capture.InputImages.front().Data;
	const auto match = [&](const Image &resolved, std::string_view port) {
		detail::NodeContext context(capture.Authored, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images.emplace_back(port, &resolved);
		const SourceBuiltinRandomCapture *record = nullptr;
		return std::pair{detail::FindSourceBuiltinRandomCapture(context, record), context.FailureCode};
	};
	CHECK(match(image, "surface_in").first);
	image.Hash = 123;
	CHECK(match(image, "surface_in").first);
	image.Pixels[0] ^= 1;
	CHECK(match(image, "surface_in").second == Status::InvalidValue);
	image = capture.InputImages.front().Data;
	image.Width = 2;
	CHECK(match(image, "surface_in").second == Status::InvalidValue);
	image = capture.InputImages.front().Data;
	image.Format = SurfaceFormat::RGBA4Unorm;
	CHECK(match(image, "surface_in").second == Status::InvalidValue);
	image = capture.InputImages.front().Data;
	CHECK(match(image, "mask").second == Status::InvalidValue);
	capture.InputImages.clear();
	CHECK(match(image, "surface_in").second == Status::InvalidValue);
}
TEST_CASE(
	"Builtin RNG preparation owns evaluated image bindings and preserves prior recordings on byte refusal",
	"[source_builtin_random]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{91, 32, 17, 255}}}},
		{"observed", "pc.blur_radial", "", {}, {{"center", Vector2{0, 0}}, {"center_unit", EnumValue{0}}}}
	};
	document.Links = {{"source", "surface_out", "observed", "surface_in"}};
	document.Outputs = {{"image", "observed", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceBuiltinRandomCapture capture;
	const auto status =
		PrepareSourceBuiltinRandomCapture(document, plan, "observed", {}, capture, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(capture.InputImages.size() == 1);
	CHECK(capture.InputImages.front().Port == "surface_in");
	CHECK(capture.InputImages.front().Data.Width == 2);
	CHECK(capture.InputImages.front().Data.Pixels == std::vector<uint8_t>{91, 32, 17, 255, 91, 32, 17, 255});
	const auto previous = capture;
	SourceBuiltinRandomCapture malformed = capture;
	malformed.InputImages.front().Data.Pixels.pop_back();
	REQUIRE(
		PrepareSourceBuiltinRandomCapture(document, plan, "observed", {}, malformed, diagnostic) == Status::Ok
	);
	CHECK(malformed == previous);
	CHECK(
		PrepareSourceBuiltinRandomCapture(document, plan, "observed", {}, capture, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(capture == previous);
	capture.InputImages.front().Data.Pixels.reserve(2 * 1024 * 1024);
	const auto retainedCapacity = capture.InputImages.front().Data.Pixels.capacity();
	CHECK(
		PrepareSourceBuiltinRandomCapture(document, plan, "observed", {}, capture, diagnostic, 1024 * 1024) ==
		Status::LimitExceeded
	);
	CHECK(capture == previous);
	CHECK(capture.InputImages.front().Data.Pixels.capacity() == retainedCapacity);
	document.Nodes.front().Values.back().Data = Colour{0, 0, 0, 255};
	CHECK(capture.InputImages.front().Data.Pixels == previous.InputImages.front().Data.Pixels);
}
TEST_CASE(
	"Builtin RNG image recordings share the unrelated output byte budget atomically",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture = ImageCapture();
	capture.InputImages.front().Data.Pixels.reserve(1024 * 1024);
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"solid",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{64, 64}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{1, 2, 3, 255}}}}
	};
	document.Outputs = {{"image", "solid", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	uint64_t bytes = 0;
	REQUIRE(
		ValidateBuiltinRandomCaptures({&capture, 1}, Limits::MaximumEvaluationBytes, bytes, diagnostic) ==
		Status::Ok
	);
	const auto cap = bytes + 128;
	Image output;
	REQUIRE(Evaluate(document, plan, "image", {}, output, diagnostic, cap) == Status::Ok);
	const Image previous = output;
	EvaluationRequest request;
	request.BuiltinRandomCaptures = {&capture, 1};
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic, cap) == Status::LimitExceeded);
	CHECK(output == previous);
	capture.InputImages.front().Data.Pixels.pop_back();
	CHECK(Evaluate(document, plan, "image", request, output, diagnostic) == Status::InvalidValue);
	CHECK(output == previous);
}
TEST_CASE(
	"Pixel Builder recipes deep-copy builtin RNG image closures and admit their bytes",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture = ImageCapture();
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"builder",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}}
	};
	document.Outputs = {{"recipe", "builder", "dynamic_builder"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.BuiltinRandomCaptures = {&capture, 1};
	EvaluatedValue result;
	const auto status = EvaluateValue(document, plan, "recipe", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &recipe = std::get<DynamicSurfaceValue>(result.Data);
	REQUIRE(recipe.Data);
	REQUIRE(recipe.Data->BuiltinRandomCaptures.size() == 1);
	const auto bytes = detail::PixelBuilderStorageBytes(recipe, true);
	CHECK(bytes >= capture.InputImages.front().Data.Pixels.size());
	capture.InputImages.front().Data.Pixels[0] = 0;
	CHECK(recipe.Data->BuiltinRandomCaptures.front().InputImages.front().Data.Pixels[0] == 91);
	DynamicSurfaceValue copy = recipe;
	copy.Data->BuiltinRandomCaptures.front().InputImages.front().Data.Pixels[0] = 5;
	CHECK(recipe.Data->BuiltinRandomCaptures.front().InputImages.front().Data.Pixels[0] == 91);
}

TEST_CASE(
	"Builtin RNG image binding accepts the exact selected processor row and refuses whole arrays",
	"[source_builtin_random]"
) {
	SourceBuiltinRandomCapture capture = ImageCapture();
	capture.ProcessorRow = 1;
	EvaluationRequest request;
	request.BuiltinRandomCaptures = {&capture, 1};
	const auto *entry = FindCatalogueEntry(capture.Authored.Type);
	REQUIRE(entry);
	ImageArray original;
	original.Images.push_back(capture.InputImages.front().Data);
	detail::NodeContext context(capture.Authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorRow = 1;
	context.ImageArrays.emplace_back("surface_in", &original);
	context.Images.emplace_back("surface_in", &original.Images.front());
	const SourceBuiltinRandomCapture *record = nullptr;
	REQUIRE(detail::FindSourceBuiltinRandomCapture(context, record));
	CHECK(record == &capture);
	context.Images.clear();
	CHECK_FALSE(detail::FindSourceBuiltinRandomCapture(context, record));
	CHECK(context.FailureCode == Status::InvalidValue);
}
