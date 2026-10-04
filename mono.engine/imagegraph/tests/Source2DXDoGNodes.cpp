#include "ComplexGeneratorFixture.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <cmath>
#include <numbers>
TEST_SUITE_ID("engine.imagegraph.source_xdog")
using namespace complex_generator_test;
namespace {
	Document XDoG() {
		Document d;
		d.FormatVersion = 9;
		ArrayValue palette{ValueType::Colour, {}};
		MatrixValue matrix{8, 4, {}};
		for (size_t i = 0; i < 32; ++i) {
			palette.Elements.emplace_back(Colour{uint8_t(i * 7), uint8_t(255 - i * 7), uint8_t(i * 3), 255});
			matrix.Values.push_back(double(i));
		}
		d.Nodes = {
			{"generator",
			 "pc.xdo_g_threshold",
			 "",
			 {},
			 {{"radius", 2.}, {"radius_unit", EnumValue{0}}, {"oversample", EnumValue{3}}}},
			{"source",
			 "pc.interpret_matrix",
			 "",
			 {},
			 {{"dimension", Vector2{8, 4}},
			  {"dimension_unit", EnumValue{0}},
			  {"matrix", matrix},
			  {"palette", palette},
			  {"mode", EnumValue{1}},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		d.Links = {{"source", "surface_out", "generator", "surface_in"}};
		d.Outputs = {{"out", "generator", "surface_out"}};
		return d;
	}
	Image Original(Document d) {
		d.Outputs = {{"out", "source", "surface_out"}};
		return Draw(d);
	}
	Image DoG(Document d) {
		d.Outputs = {{"out", "generator", "do_g"}};
		return Draw(d);
	}
	void Same(const Image &a, const Image &b) {
		CHECK(a.Width == b.Width);
		CHECK(a.Height == b.Height);
		CHECK(a.Format == b.Format);
		const bool samePixels = a.Pixels == b.Pixels;
		if (!samePixels) {
			INFO("actual bytes " << a.Pixels.size() << ", expected bytes " << b.Pixels.size());
			const auto mismatch =
				std::mismatch(a.Pixels.begin(), a.Pixels.end(), b.Pixels.begin(), b.Pixels.end());
			if (mismatch.first != a.Pixels.end() && mismatch.second != b.Pixels.end()) {
				INFO(
					"first differing byte " << std::distance(a.Pixels.begin(), mismatch.first) << ": actual "
											<< unsigned(*mismatch.first) << ", expected "
											<< unsigned(*mismatch.second)
				);
				CHECK(samePixels);
			} else
				CHECK(samePixels);
		} else
			CHECK(samePixels);
		CHECK(a.Hash == SurfaceHash(a));
		CHECK(a == b);
	}
	uint8_t Byte(double x) {
		return uint8_t(std::floor(std::clamp(x, 0., 1.) * 255 + .5));
	}
	// Independent ordered source equations. Power-of-two fixture dimensions keep Clamp taps integral.
	Image BlurOracle(Image image, double size) {
		const size_t count = size_t(std::max(1., std::nearbyint(std::min(1024., size))));
		std::vector<double> kernel(count);
		double total = 0;
		const double spread = .3 * ((count - 1) * .5 - 1) + .8;
		for (size_t i = 0; i < count; ++i) {
			kernel[i] = (1. / std::sqrt(2 * std::numbers::pi * spread)) *
						std::exp(-(i * .5) * (i * .5) / (2 * spread * spread));
			total += kernel[i] * (i ? 2 : 1);
		}
		for (auto &w : kernel)
			w /= total;
		for (int axis = 0; axis < 2; ++axis) {
			Image out = image;
			for (uint32_t y = 0; y < image.Height; ++y)
				for (uint32_t x = 0; x < image.Width; ++x) {
					std::array<double, 3> sum{};
					double alpha = .00001, weights = .00001;
					const auto tap = [&](int offset, double index) {
						const int sx = std::clamp(int(x) + (axis ? 0 : offset), 0, int(image.Width) - 1),
								  sy = std::clamp(int(y) + (axis ? offset : 0), 0, int(image.Height) - 1);
						const auto weightAt = [&](size_t i) {
							return i < kernel.size() && i < 256 ? kernel[i] : 0.;
						};
						const auto whole = size_t(std::floor(index));
						const double fraction = index - std::floor(index);
						const double weight =
							weightAt(whole) + (weightAt(whole + 1) - weightAt(whole)) * fraction;
						const size_t p = (size_t(sy) * image.Width + sx) * 4;
						const double a = image.Pixels[p + 3] / 255.;
						alpha += weight * a;
						weights += weight;
						for (size_t c = 0; c < 3; ++c)
							sum[c] += (image.Pixels[p + c] / 255.) * (weight * a);
					};
					tap(0, 0);
					for (int i = 1; i < size; ++i) {
						const double index = i / size * size;
						tap(i, index);
						tap(-i, index);
					}
					const size_t p = (size_t(y) * image.Width + x) * 4;
					for (size_t c = 0; c < 3; ++c)
						out.Pixels[p + c] = Byte(sum[c] / alpha);
					out.Pixels[p + 3] = Byte(alpha / weights);
				}
			image = std::move(out);
		}
		image.Hash = SurfaceHash(image);
		return image;
	}
	Image DifferenceOracle(const Image &original, double radius, double k, double gamma, bool edge) {
		const auto first = BlurOracle(original, radius), second = BlurOracle(original, radius * k);
		Image out = original;
		for (size_t p = 0; p < out.Pixels.size(); p += 4) {
			for (size_t c = 0; c < 3; ++c) {
				double v = first.Pixels[p + c] / 255. - (second.Pixels[p + c] / 255.) * gamma;
				out.Pixels[p + c] = Byte(edge ? std::abs(v) : v);
			}
			out.Pixels[p + 3] = 255;
		}
		out.Hash = SurfaceHash(out);
		return out;
	}
	Image ThresholdOracle(Image dog, double epsilon, double smoothness) {
		for (size_t p = 0; p < dog.Pixels.size(); p += 4) {
			const double s = (dog.Pixels[p] / 255. + dog.Pixels[p + 1] / 255. + dog.Pixels[p + 2] / 255.) /
							 3. * (dog.Pixels[p + 3] / 255.),
						 lo = std::max(0., epsilon - smoothness), hi = std::min(1., epsilon + smoothness),
						 t = std::clamp((s - lo) / (hi - lo), 0., 1.);
			for (size_t c = 0; c < 3; ++c)
				dog.Pixels[p + c] = Byte(t * t * (3 - 2 * t));
			dog.Pixels[p + 3] = 255;
		}
		dog.Hash = SurfaceHash(dog);
		return dog;
	}
}
TEST_CASE("XDoG Gaussian difference and threshold match independent pinned equations", "[source_xdog]") {
	for (double radius : {0., 1., 2., 3.})
		for (double k : {.5, 1., 2., -1.})
			for (bool edge : {false, true}) {
				auto d = XDoG();
				Set(d, "radius", radius);
				Set(d, "k", k);
				Set(d, "gamma", .75);
				Set(d, "edge", edge);
				const auto dog = DifferenceOracle(Original(d), radius, k, .75, edge);
				Same(DoG(d), dog);
				Same(Draw(d), ThresholdOracle(dog, .1, .1));
			}
}
TEST_CASE("XDoG radius Reference units use source width rather than its maximum dimension", "[source_xdog]") {
	auto d = XDoG();
	for (auto &v : d.Nodes[1].Values) {
		if (v.Port == "dimension") v.Data = Vector2{4, 8};
		if (v.Port == "matrix") {
			auto &m = std::get<MatrixValue>(v.Data);
			m.Columns = 4;
			m.Rows = 8;
		}
	}
	Set(d, "radius", .25);
	Set(d, "radius_unit", EnumValue{1});
	Set(d, "k", 2.);
	Same(DoG(d), DifferenceOracle(Original(d), 1., 2., 1., false));
}
TEST_CASE("XDoG masks mix and channels affect only the threshold output", "[source_xdog]") {
	auto d = XDoG();
	const auto dog = DoG(d), original = Original(d);
	d.Nodes.push_back(Solid("mask", {1, 1}, {0, 0, 0, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	Same(Draw(d), original);
	Same(DoG(d), dog);
	Set(d, "mix", 0.);
	Set(d, "invert_mask", true);
	Same(Draw(d), original);
	Same(DoG(d), dog);
	Set(d, "mix", 1.);
	Set(d, "channel", int64_t{0});
	Same(Draw(d), original);
	Same(DoG(d), dog);
}
TEST_CASE("Inactive XDoG copies main image and identifies unavailable retained DoG", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "active", false);
	Same(Draw(d), Original(d));
	d.Outputs = {{"out", "generator", "do_g"}};
	Refuse(d, Status::UnsupportedExecution, "do_g", Limits::MaximumEvaluationBytes, "previous DoG");
}
TEST_CASE("XDoG undefined smoothstep refuses only main output while DoG remains readable", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "smoothness", 0.);
	const auto dog = DifferenceOracle(Original(d), 2., 8., 1., false);
	Same(DoG(d), dog);
	Refuse(d, Status::UnsupportedExecution, "surface_out", Limits::MaximumEvaluationBytes, "smoothstep");
}
TEST_CASE("XDoG physical mapped pairs and absent maps preserve source endpoints", "[source_xdog]") {
	for (const std::string port : {"gamma", "epsilon", "smoothness"}) {
		auto d = XDoG();
		Set(d, port + "_mapped", true);
		Set(d, port + "_map_range", Vector2{.25, .75});
		auto expected = d;
		Set(expected, port + "_mapped", false);
		Set(expected, port, .25);
		Same(Draw(d), Draw(expected));
		Same(DoG(d), DoG(expected));
		Set(d, port, ArrayValue{ValueType::Scalar, {.5, .9}});
		Set(expected, port, .5);
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE("XDoG mapped surfaces use mean RGB with inert alpha", "[source_xdog]") {
	for (const std::string port : {"gamma", "epsilon", "smoothness"}) {
		auto d = XDoG();
		Set(d, port + "_mapped", true);
		Set(d, port + "_map_range", Vector2{.25, .75});
		d.Nodes.push_back(Solid("map", {1, 1}, {255, 0, 0, 0}));
		d.Links.push_back({"map", "surface_out", "generator", port + "_map"});
		auto expected = XDoG();
		Set(expected, port, .25 + .5 / 3);
		Same(Draw(d), Draw(expected));
		Same(DoG(d), DoG(expected));
	}
}
TEST_CASE("XDoG original heterogeneous source rows retain first Reference width", "[source_xdog]") {
	auto d = XDoG();
	Node second = d.Nodes[1];
	second.Id = "small";
	for (auto &v : second.Values) {
		if (v.Port == "dimension") v.Data = Vector2{4, 8};
		if (v.Port == "matrix") {
			auto &m = std::get<MatrixValue>(v.Data);
			m.Columns = 4;
			m.Rows = 8;
		}
	}
	d.Nodes.push_back(second);
	Node list{"list", "value.array", "", {}, {}};
	list.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(list);
	d.Links = {
		{"source", "surface_out", "list", "a"},
		{"small", "surface_out", "list", "b"},
		{"list", "array", "generator", "surface_in"}
	};
	Set(d, "radius", .25);
	Set(d, "radius_unit", EnumValue{1});
	Set(d, "k", 2.);
	Set(d, "gamma", .75);
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto single = XDoG();
		single.Nodes[1] = i ? second : d.Nodes[1];
		single.Nodes[1].Id = "source";
		const auto original = Original(single), dog = DifferenceOracle(original, 2., 2., .75, false);
		Same(rows.Images[i], ThresholdOracle(dog, .1, .1));
		if (i)
			CHECK(
				rows.Images[i].Pixels !=
				ThresholdOracle(DifferenceOracle(original, 1., 2., .75, false), .1, .1).Pixels
			);
	}
}

TEST_CASE("XDoG persistence retains both output declarations and mapped source controls", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "gamma_mapped", true);
	Set(d, "gamma_map_range", Vector2{.25, .75});
	std::string text;
	Diagnostic diagnostic;
	text = Write(d);
	Document restored;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == d);
	Same(Draw(restored), Draw(d));
	Same(DoG(restored), DoG(d));
}
TEST_CASE("XDoG public byte refusal preserves prior output", "[source_xdog]") {
	Refuse(XDoG(), Status::LimitExceeded, "", 1);
}
TEST_CASE("XDoG later radius row is admitted before the first output", "[source_xdog]") {
	auto d = XDoG();
	ArrayValue radius{ValueType::Scalar, {1., 1000000000.}};
	Set(d, "radius", radius);
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("XDoG empty and clamped oversampling differ at source image edges", "[source_xdog]") {
	auto clamp = XDoG(), empty = clamp;
	Set(clamp, "k", 2.);
	Set(clamp, "gamma", .75);
	Set(empty, "k", 2.);
	Set(empty, "gamma", .75);
	Set(empty, "oversample", EnumValue{1});
	CHECK(DoG(clamp).Pixels != DoG(empty).Pixels);
	Same(DoG(clamp), DifferenceOracle(Original(clamp), 2., 2., .75, false));
}
TEST_CASE("XDoG seven declared depths publish both images in their resolved format", "[source_xdog]") {
	for (int depth = 2; depth <= 8; ++depth) {
		auto d = XDoG();
		Set(d, "attribute_color_depth", EnumValue{depth});
		Set(d, "gamma", .5);
		const auto main = Draw(d), dog = DoG(d);
		CHECK(main.Width == 8);
		CHECK(main.Height == 4);
		CHECK(main.Format == dog.Format);
		CHECK(main.Hash == SurfaceHash(main));
		CHECK(dog.Hash == SurfaceHash(dog));
		CHECK(ValidSurfaceLayout(main, Limits::MaximumDimension, Limits::MaximumOutputBytes));
		CHECK(ValidSurfaceLayout(dog, Limits::MaximumDimension, Limits::MaximumOutputBytes));
	}
}
TEST_CASE("XDoG inherited instance controls preserve active kernel identity", "[source_xdog]") {
	auto d = XDoG();
	const auto expected = Draw(d), dog = DoG(d);
	Node instance{"instance", "pc.xdo_g_threshold", "", {}, {}};
	instance.InstanceBase = "generator";
	d.Nodes.push_back(instance);
	d.Outputs = {{"out", "instance", "surface_out"}};
	Same(Draw(d), expected);
	d.Outputs = {{"out", "instance", "do_g"}};
	Same(Draw(d), dog);
}
TEST_CASE("XDoG later source dimensions participate in preflight before first allocation", "[source_xdog]") {
	auto d = XDoG();
	d.Nodes[1] = Solid("source", {1, 1}, {255, 255, 255, 255});
	d.Nodes.push_back(Solid("large", {256, 256}, {128, 64, 32, 255}));
	Node list{"list", "value.array", "", {}, {}};
	list.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(list);
	d.Links = {
		{"source", "surface_out", "list", "a"},
		{"large", "surface_out", "list", "b"},
		{"list", "array", "generator", "surface_in"}
	};
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("XDoG negative Gaussian radii retain source centre-only behavior", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "radius", -2.);
	Set(d, "gamma", .25);
	Same(DoG(d), DifferenceOracle(Original(d), -2., 8., .25, false));
}
TEST_CASE("XDoG linked Surface Radius returns dimension rows before Reference units", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "radius_unit", EnumValue{1});
	Set(d, "gamma", .75);
	Set(d, "k", 2.);
	d.Nodes.push_back(Solid("dimension_source", {2, 3}, {255, 255, 255, 255}));
	d.Links.push_back({"dimension_source", "surface_out", "generator", "radius"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	ImageArray rows;
	REQUIRE(EvaluateArray(d, plan, "out", {}, rows, diagnostic) == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	const auto source = Original(d);
	Same(rows.Images[0], ThresholdOracle(DifferenceOracle(source, 2., 2., .75, false), .1, .1));
	Same(rows.Images[1], ThresholdOracle(DifferenceOracle(source, 3., 2., .75, false), .1, .1));
}
TEST_CASE("XDoG original later feather work is admitted independently of row zero", "[source_xdog]") {
	auto d = XDoG();
	d.Nodes.push_back(Solid("mask", {1, 1}, {255, 255, 255, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	d.Nodes.push_back(Array("feathers", ValueType::Scalar, 0., 1e12));
	d.Links.push_back({"feathers", "array", "generator", "mask_feather"});
	Refuse(d, Status::LimitExceeded, "radius", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("XDoG absent mask leaves feather inert", "[source_xdog]") {
	auto d = XDoG();
	const auto main = Draw(d), dog = DoG(d);
	Set(d, "mask_feather", 1e12);
	Same(Draw(d), main);
	Same(DoG(d), dog);
}
TEST_CASE("XDoG red input Gaussian safe draws bypass filtering but staging stays red-only", "[source_xdog]") {
	for (int depth : {6, 7, 8}) {
		auto d = XDoG();
		for (auto &v : d.Nodes[1].Values)
			if (v.Port == "attribute_color_depth") v.Data = EnumValue{depth};
		Set(d, "attribute_color_depth", EnumValue{3});
		Set(d, "radius", 2.);
		Set(d, "gamma", .25);
		const auto source = Original(d);
		Image expected{
			source.Width, source.Height, std::vector<uint8_t>(size_t(source.Width) * source.Height * 4), 0
		};
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				SurfacePixel pixel;
				REQUIRE(LoadSurfacePixel(source, x, y, pixel));
				const auto q = Byte(pixel[0]);
				const auto offset = (size_t(y) * source.Width + x) * 4;
				expected.Pixels[offset] = Byte(q / 255. * .75);
				expected.Pixels[offset + 3] = 255;
			}
		expected.Hash = SurfaceHash(expected);
		Same(DoG(d), expected);
		Same(Draw(d), ThresholdOracle(expected, .1, .1));
	}
}
TEST_CASE("XDoG disabled first row still admits original later work", "[source_xdog]") {
	const auto *entry = FindCatalogueEntry("pc.xdo_g_threshold");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.xdo_g_threshold");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"generator", "pc.xdo_g_threshold", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 2;
	const auto first = imagegraph_test::MakeImage(1, 1, {1, 2, 3, 255});
	ImageArray originals;
	originals.Images = {
		first, imagegraph_test::MakeImage(256, 256, std::vector<uint8_t>(256 * 256 * 4, 255))
	};
	context.Images = {{"surface_in", &first}};
	context.ImageArrays = {{"surface_in", &originals}};
	context.Values = {{"active", false}, {"radius", 2.}, {"radius_unit", EnumValue{0}}};
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("whole-array work") != std::string::npos);
	CHECK(context.OutputImages.empty());
}

TEST_CASE("XDoG RGBA8 pixels match independent pinned literal equation goldens", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "k", 2.);
	Set(d, "gamma", .75);
	const std::vector<uint8_t> dogPixels{0,	 76, 0,	 255, 0,  74, 0,  255, 0,  70, 0,  255, 0,	68, 0,	255,
										 0,	 67, 0,	 255, 0,  65, 0,  255, 2,  62, 1,  255, 4,	60, 1,	255,
										 4,	 60, 2,	 255, 6,  58, 2,  255, 9,  55, 4,  255, 11, 53, 5,	255,
										 13, 51, 5,	 255, 14, 49, 6,  255, 18, 46, 8,  255, 20, 44, 8,	255,
										 35, 29, 15, 255, 37, 27, 16, 255, 40, 24, 17, 255, 42, 22, 18, 255,
										 43, 20, 19, 255, 45, 19, 20, 255, 48, 15, 21, 255, 50, 13, 22, 255,
										 50, 14, 22, 255, 52, 11, 22, 255, 56, 8,  24, 255, 57, 6,	24, 255,
										 59, 5,	 25, 255, 61, 3,  26, 255, 64, 0,  28, 255, 66, 0,	28, 255};
	Image dog{8, 4, dogPixels, 0};
	dog.Hash = SurfaceHash(dog);
	Same(DoG(d), dog);
	const std::vector<uint8_t> thresholdPixels{
		126, 126, 126, 255, 121, 121, 121, 255, 111, 111, 111, 255, 106, 106, 106, 255, 104, 104, 104,
		255, 99,  99,  99,	255, 99,  99,  99,	255, 99,  99,  99,	255, 101, 101, 101, 255, 101, 101,
		101, 255, 106, 106, 106, 255, 109, 109, 109, 255, 109, 109, 109, 255, 109, 109, 109, 255, 116,
		116, 116, 255, 116, 116, 116, 255, 134, 134, 134, 255, 136, 136, 136, 255, 139, 139, 139, 255,
		141, 141, 141, 255, 141, 141, 141, 255, 146, 146, 146, 255, 146, 146, 146, 255, 149, 149, 149,
		255, 151, 151, 151, 255, 149, 149, 149, 255, 156, 156, 156, 255, 154, 154, 154, 255, 158, 158,
		158, 255, 161, 161, 161, 255, 166, 166, 166, 255, 170, 170, 170, 255
	};
	Image threshold{8, 4, thresholdPixels, 0};
	threshold.Hash = SurfaceHash(threshold);
	Same(Draw(d), threshold);
}
TEST_CASE("XDoG Gaussian stages retain source alpha-weighted filtering", "[source_xdog]") {
	auto d = XDoG();
	Set(d, "k", 2.);
	Set(d, "gamma", .75);
	for (auto &v : d.Nodes[1].Values)
		if (v.Port == "palette") {
			auto &palette = std::get<ArrayValue>(v.Data);
			for (size_t i = 0; i < palette.Elements.size(); ++i)
				std::get<Colour>(palette.Elements[i]).Alpha = uint8_t((i % 4) * 85);
		}
	const auto source = Original(d), dog = DifferenceOracle(source, 2., 2., .75, false);
	Same(DoG(d), dog);
	Same(Draw(d), ThresholdOracle(dog, .1, .1));
}
TEST_CASE("XDoG RGBA4 and floating source formats retain native pass conversion", "[source_xdog]") {
	for (int depth : {2, 4, 5}) {
		auto d = XDoG();
		for (auto &v : d.Nodes[1].Values)
			if (v.Port == "attribute_color_depth") v.Data = EnumValue{depth};
		Set(d, "attribute_color_depth", EnumValue{3});
		Set(d, "radius", 0.);
		Set(d, "gamma", 0.);
		const auto source = Original(d);
		Image expected{
			source.Width, source.Height, std::vector<uint8_t>(size_t(source.Width) * source.Height * 4), 0
		};
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				SurfacePixel pixel;
				REQUIRE(LoadSurfacePixel(source, x, y, pixel));
				const auto offset = (size_t(y) * source.Width + x) * 4;
				for (size_t c = 0; c < 3; ++c)
					expected.Pixels[offset + c] = Byte(pixel[c]);
				expected.Pixels[offset + 3] = 255;
			}
		expected.Hash = SurfaceHash(expected);
		Same(DoG(d), expected);
		Same(Draw(d), ThresholdOracle(expected, .1, .1));
	}
}

TEST_CASE("XDoG interpolates source Gaussian tap indices after native division", "[source_xdog]") {
	const double size = 22.;
	const double index = 15. / size * size;
	REQUIRE(index < 15.);
	REQUIRE(std::floor(index) == 14.);
	auto d = XDoG();
	Set(d, "radius", size);
	Set(d, "k", .5);
	Set(d, "gamma", .75);
	const auto dog = DifferenceOracle(Original(d), size, .5, .75, false);
	Same(DoG(d), dog);
	Same(Draw(d), ThresholdOracle(dog, .1, .1));
}
TEST_CASE("XDoG projected Surface domain bypasses units without a borrowed Radius image", "[source_xdog]") {
	const auto source = Original(XDoG());
	const auto *entry = FindCatalogueEntry("pc.xdo_g_threshold");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.xdo_g_threshold");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"generator", "pc.xdo_g_threshold", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"surface_in", &source}};
	context.InputDomains = {{"radius", {ValueType::Image, std::nullopt, SourceSocketKind::Surface}}};
	context.Values = {
		{"radius", 2.}, {"radius_unit", EnumValue{1}}, {"k", 2.}, {"gamma", .75}, {"oversample", EnumValue{3}}
	};
	REQUIRE(context.Input("radius") == nullptr);
	REQUIRE(executor(context));
	const auto expected = DifferenceOracle(source, 2., 2., .75, false);
	bool found = false;
	for (auto &[port, image] : context.OutputImages) {
		if (port != "do_g") continue;
		image.Hash = SurfaceHash(image);
		Same(image, expected);
		found = true;
	}
	REQUIRE(found);
}
