#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d_executor")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

namespace source_shape3d_executor_test {
	bool HasCoverage(const Image &image) {
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x) {
				SurfacePixel pixel;
				if (LoadSurfacePixel(image, x, y, pixel) && pixel[3] > 0) return true;
			}
		return false;
	}
	void SetValue(detail::NodeContext &context, std::string_view port, Value value) {
		for (auto &[id, current] : context.Values)
			if (id == port) {
				current = std::move(value);
				return;
			}
		context.Values.emplace_back(port, std::move(value));
	}
	Image Solid(Colour colour) {
		return {1, 1, {colour.Red, colour.Green, colour.Blue, colour.Alpha}, 0};
	}
}
using namespace source_shape3d_executor_test;

TEST_CASE("Shape 3D compiled Cube publishes three finite covered outputs", "[imagegraph][shape3d]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"shape",
		 "pc.shape_3_d",
		 "",
		 {},
		 {{"dimension", Vector2{16, 12}},
		  {"dimension_unit", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{5}}}}
	};
	document.Outputs = {
		{"surface", "shape", "surface_out"}, {"depth", "shape", "depth"}, {"rim", "shape", "rim_normal"}
	};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	std::array<Image, 3> outputs;
	const std::array<std::string, 3> names{"surface", "depth", "rim"};
	for (size_t index = 0; index < outputs.size(); ++index) {
		const auto status = Evaluate(document, plan, names[index], {}, outputs[index], diagnostic);
		INFO(names[index] << ": " << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(outputs[index].Width == 16);
		CHECK(outputs[index].Height == 12);
		CHECK(FiniteSurfaceSamples(outputs[index]));
		CHECK(HasCoverage(outputs[index]));
	}
	Image repeated;
	REQUIRE(Evaluate(document, plan, "surface", {}, repeated, diagnostic) == Status::Ok);
	CHECK(repeated == outputs[0]);
}

TEST_CASE("Shape 3D flat texture list selects batch rows or submesh bindings", "[imagegraph][shape3d]") {
	const auto *entry = FindCatalogueEntry("pc.shape_3_d");
	const auto executor = detail::FindExecutor("pc.shape_3_d");
	REQUIRE(entry);
	REQUIRE(executor);
	ImageArray textures;
	textures.Images = {Solid({255, 0, 0, 255}), Solid({0, 0, 255, 255})};
	textures.Items = {{size_t{0}}, {size_t{1}}};
	for (bool arrayTexture : {false, true}) {
		Node node{"shape", "pc.shape_3_d", "", {}, {}};
		EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &input : entry->Inputs)
			if (const auto value = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *value);
		SetValue(context, "dimension", Vector2{12, 12});
		SetValue(context, "dimension_unit", EnumValue{0});
		SetValue(context, "attribute_color_depth", EnumValue{5});
		SetValue(context, "array_texture", arrayTexture);
		context.ImageArrays.emplace_back("texture", &textures);
		context.InputProvenanceResolved = true;
		for (const auto &[port, value] : context.Values)
			context.ValueViews.emplace_back(port, &value);
		const bool ran = detail::RunProcessorBatch(context, executor);
		INFO(arrayTexture << ": " << context.FailurePort << ": " << context.FailureMessage);
		REQUIRE(ran);
		if (arrayTexture) {
			REQUIRE(context.OutputImages.size() == 3);
			CHECK(context.OutputImageArrays.empty());
			for (const auto &[port, image] : context.OutputImages)
				CHECK(HasCoverage(image));
		} else {
			REQUIRE(context.OutputImageArrays.size() == 3);
			for (const auto &[port, images] : context.OutputImageArrays) {
				REQUIRE(images.Images.size() == 2);
				CHECK(HasCoverage(images.Images[0]));
				CHECK(HasCoverage(images.Images[1]));
				if (port == "surface_out") {
					CHECK(images.Images[0].Pixels != images.Images[1].Pixels);
					for (size_t row = 0; row < images.Images.size(); ++row) {
						const auto direct = RunNode(
							"pc.shape_3_d",
							{{"texture", &textures.Images[row]}},
							{{"dimension", Vector2{12, 12}},
							 {"dimension_unit", EnumValue{0}},
							 {"attribute_color_depth", EnumValue{5}},
							 {"array_texture", false}}
						);
						INFO(row << ": " << direct.Port << ": " << direct.Message);
						REQUIRE(direct.Ok);
						CHECK(images.Images[row].Pixels == direct.Output().Pixels);
					}
				}
			}
		}
	}
}

TEST_CASE("Shape 3D refuses nonfinite textures and renders zero-normal Octahedron", "[imagegraph][shape3d]") {
	Image malformed;
	malformed.Width = malformed.Height = 1;
	malformed.Format = SurfaceFormat::RGBA32Float;
	malformed.Pixels.resize(16);
	malformed.Pixels[2] = 0xc0;
	malformed.Pixels[3] = 0x7f;
	const auto invalid = RunNode(
		"pc.shape_3_d",
		{{"texture", &malformed}},
		{{"dimension", Vector2{8, 8}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(invalid.Port << ": " << invalid.Message);
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Images.empty());
	const auto octahedron = RunNode(
		"pc.shape_3_d",
		{},
		{{"shape", std::string("Octahedron")}, {"dimension", Vector2{8, 8}}, {"dimension_unit", EnumValue{0}}}
	);
	INFO(octahedron.Port << ": " << octahedron.Message);
	REQUIRE(octahedron.Ok);
	REQUIRE(octahedron.Images.size() == 3);
	for (const auto &[port, image] : octahedron.Images) {
		CHECK(FiniteSurfaceSamples(image));
		CHECK(HasCoverage(image));
	}
	const auto &rim = octahedron.Output("rim_normal");
	for (uint32_t y = 0; y < rim.Height; ++y)
		for (uint32_t x = 0; x < rim.Width; ++x) {
			SurfacePixel pixel;
			REQUIRE(LoadSurfacePixel(rim, x, y, pixel));
			CHECK(pixel[0] == 0);
			CHECK(pixel[1] == 0);
			CHECK(pixel[2] == 0);
		}
}

TEST_CASE("Shape 3D insufficient storage preserves selected output", "[imagegraph][shape3d]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"shape", "pc.shape_3_d", "", {}, {{"dimension", Vector2{16, 16}}, {"dimension_unit", EnumValue{0}}}}
	};
	document.Outputs = {{"out", "shape", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output = Solid({12, 34, 56, 255});
	const auto before = output;
	const auto status = Evaluate(document, plan, "out", {}, output, diagnostic, 1);
	INFO(diagnostic.Port << ": " << diagnostic.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(output == before);
}

namespace source_shape3d_executor_test {
	struct TextureListBatch {
		Node Authored{"shape", "pc.shape_3_d", "", {}, {}};
		EvaluationRequest Request;
		const CatalogueEntry *Entry = FindCatalogueEntry("pc.shape_3_d");
		detail::NodeContext Context{Authored, *Entry, Request};
		TextureListBatch(const ImageArray &textures) {
			Context.ByteBudget = Limits::MaximumEvaluationBytes;
			for (const auto &input : Entry->Inputs)
				if (const auto value = CatalogueDefault(input)) Context.Values.emplace_back(input.Id, *value);
			SetValue(Context, "dimension", Vector2{12, 12});
			SetValue(Context, "dimension_unit", EnumValue{0});
			SetValue(Context, "attribute_color_depth", EnumValue{5});
			SetValue(Context, "array_texture", true);
			Context.ImageArrays.emplace_back("texture", &textures);
			Context.InputProvenanceResolved = true;
			for (const auto &[port, value] : Context.Values)
				Context.ValueViews.emplace_back(port, &value);
		}
		bool Run() {
			return detail::RunProcessorBatch(Context, detail::FindExecutor("pc.shape_3_d"));
		}
	};
}

TEST_CASE("Shape 3D nested texture lists batch complete submesh rows", "[imagegraph][shape3d]") {
	ImageArray textures;
	textures.Images = {Solid({255, 0, 0, 255}), Solid({0, 0, 255, 255})};
	textures.Items = {
		{std::vector<ImageArrayItem>{{size_t{0}}, {size_t{1}}}},
		{std::vector<ImageArrayItem>{{size_t{1}}, {size_t{0}}}}
	};
	TextureListBatch batch(textures);
	const bool ran = batch.Run();
	INFO(batch.Context.FailurePort << ": " << batch.Context.FailureMessage);
	REQUIRE(ran);
	REQUIRE(batch.Context.OutputImageArrays.size() == 3);
	for (size_t row = 0; row < 2; ++row) {
		ImageArray flat;
		flat.Images = textures.Images;
		flat.Items = std::get<std::vector<ImageArrayItem>>(textures.Items[row].Data);
		TextureListBatch direct(flat);
		const bool directRan = direct.Run();
		INFO(row << ": " << direct.Context.FailurePort << ": " << direct.Context.FailureMessage);
		REQUIRE(directRan);
		REQUIRE(direct.Context.OutputImages.size() == 3);
		for (const auto &[port, images] : batch.Context.OutputImageArrays) {
			REQUIRE(images.Images.size() == 2);
			const auto expected = std::find_if(
				direct.Context.OutputImages.begin(),
				direct.Context.OutputImages.end(),
				[&](const auto &item) { return item.first == port; }
			);
			REQUIRE(expected != direct.Context.OutputImages.end());
			CHECK(images.Images[row].Pixels == expected->second.Pixels);
		}
	}
}

TEST_CASE("Shape 3D mixed and deeper texture lists refuse without partial outputs", "[imagegraph][shape3d]") {
	const std::array<std::vector<ImageArrayItem>, 2> malformed{
		std::vector<ImageArrayItem>{{size_t{0}}, {std::vector<ImageArrayItem>{{size_t{0}}}}},
		std::vector<ImageArrayItem>{{std::vector<ImageArrayItem>{{std::vector<ImageArrayItem>{{size_t{0}}}}}}}
	};
	for (const auto &items : malformed) {
		ImageArray textures;
		textures.Images = {Solid({255, 255, 255, 255})};
		textures.Items = items;
		TextureListBatch batch(textures);
		CHECK_FALSE(batch.Run());
		INFO(batch.Context.FailurePort << ": " << batch.Context.FailureMessage);
		CHECK(batch.Context.FailureCode == Status::InvalidValue);
		CHECK(batch.Context.OutputImages.empty());
		CHECK(batch.Context.OutputImageArrays.empty());
	}
}
