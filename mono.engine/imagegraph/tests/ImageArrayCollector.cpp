#include "../src/ImageArrayCollector.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.image_array_collector")

using namespace engine::imagegraph;

namespace {
	Image Pixel(uint8_t red) {
		return Image{1, 1, {red, 0, 0, 255}, red};
	}
	ImageArray NestedInput() {
		ImageArray input;
		input.Images = {Pixel(10), Pixel(20)};
		input.Items = {ImageArrayItem{std::vector<ImageArrayItem>{ImageArrayItem{size_t{0}}}}};
		return input;
	}
}

TEST_CASE(
	"Image collector admits complete clones while prior results stay live",
	"[imagegraph][image_collector][evaluation_budget]"
) {
	const Image scalar = Pixel(30);
	const ImageArray nested = NestedInput();
	const auto originalItems = nested.Items;
	const detail::ImageArrayInput inputs[]{&scalar, &nested, &nested};
	for (const bool spread : {false, true}) {
		detail::ImageArrayFootprint footprint;
		REQUIRE(detail::MeasureImageArray(inputs, spread, footprint) == Status::Ok);
		CHECK(footprint.Images == 5);
		CHECK(footprint.Pixels == 20);
		CHECK(footprint.Roots == 3);
		CHECK(footprint.Items == (spread ? 5 : 7));
		for (const uint64_t limit : {64 + footprint.Bytes() - 1, 64 + footprint.Bytes()}) {
			detail::EvaluationBudget budget(limit);
			auto priorCharge = budget.Reserve(64);
			REQUIRE(priorCharge);
			{
				detail::AllocationReservation outputCharge;
				ImageArray output;
				const Status status = detail::CollectImageArray(inputs, spread, budget, output, outputCharge);
				if (limit < 64 + footprint.Bytes()) {
					CHECK(status == Status::LimitExceeded);
					CHECK(output.Images.empty());
					CHECK(output.Items.empty());
					CHECK(outputCharge.Bytes() == 0);
					CHECK(budget.Used() == 64);
				} else {
					REQUIRE(status == Status::Ok);
					REQUIRE(output.Images.size() == 5);
					CHECK(output.Images[2].Pixels == nested.Images[1].Pixels);
					CHECK(output.Images[4].Hash == nested.Images[1].Hash);
					CHECK(std::get<size_t>(output.Items[0].Data) == 0);
					const auto &second = std::get<std::vector<ImageArrayItem>>(output.Items[1].Data);
					const auto &third = std::get<std::vector<ImageArrayItem>>(output.Items[2].Data);
					const auto &secondLeaf =
						spread ? second[0] : std::get<std::vector<ImageArrayItem>>(second[0].Data)[0];
					const auto &thirdLeaf =
						spread ? third[0] : std::get<std::vector<ImageArrayItem>>(third[0].Data)[0];
					CHECK(std::get<size_t>(secondLeaf.Data) == 1);
					CHECK(std::get<size_t>(thirdLeaf.Data) == 3);
					CHECK(outputCharge.Bytes() == footprint.Bytes());
					CHECK(budget.Used() == limit);
				}
				CHECK(nested.Items == originalItems);
				CHECK(nested.Images[0].Pixels == Pixel(10).Pixels);
			}
			CHECK(budget.Used() == 64);
		}
	}
}

TEST_CASE(
	"Image collector replacement keeps old output on overlap refusal",
	"[imagegraph][image_collector][evaluation_budget]"
) {
	const Image first = Pixel(10), second = Pixel(20);
	const detail::ImageArrayInput initial[]{&first};
	const detail::ImageArrayInput replacement[]{&first, &second};
	detail::ImageArrayFootprint a, b;
	REQUIRE(detail::MeasureImageArray(initial, false, a) == Status::Ok);
	REQUIRE(detail::MeasureImageArray(replacement, false, b) == Status::Ok);
	for (const uint64_t limit : {a.Bytes() + b.Bytes() - 1, a.Bytes() + b.Bytes()}) {
		detail::EvaluationBudget budget(limit);
		detail::AllocationReservation charge;
		ImageArray output;
		REQUIRE(detail::CollectImageArray(initial, false, budget, output, charge) == Status::Ok);
		const auto oldItems = output.Items;
		const Status status = detail::CollectImageArray(replacement, false, budget, output, charge);
		if (limit < a.Bytes() + b.Bytes()) {
			CHECK(status == Status::LimitExceeded);
			CHECK(output.Items == oldItems);
			REQUIRE(output.Images.size() == 1);
			CHECK(output.Images[0].Pixels == first.Pixels);
			CHECK(charge.Bytes() == a.Bytes());
		} else {
			REQUIRE(status == Status::Ok);
			CHECK(output.Images.size() == 2);
			CHECK(charge.Bytes() == b.Bytes());
			CHECK(budget.Peak() == a.Bytes() + b.Bytes());
		}
	}
}

TEST_CASE(
	"Image collector measures empty rows and full depth before copying",
	"[imagegraph][image_collector][evaluation_budget]"
) {
	ImageArray empty;
	const detail::ImageArrayInput emptyInput[]{&empty};
	detail::ImageArrayFootprint footprint;
	REQUIRE(detail::MeasureImageArray(emptyInput, false, footprint) == Status::Ok);
	CHECK(footprint.Roots == 1);
	CHECK(footprint.Items == 1);
	REQUIRE(detail::MeasureImageArray(emptyInput, true, footprint) == Status::Ok);
	CHECK(footprint.Items == 0);
	ImageArray nested;
	nested.Images = {Pixel(1)};
	ImageArrayItem root{size_t{0}};
	for (int depth = 1; depth < 16; ++depth)
		root = ImageArrayItem{std::vector<ImageArrayItem>{std::move(root)}};
	nested.Items.push_back(std::move(root));
	const detail::ImageArrayInput input[]{&nested};
	REQUIRE(detail::MeasureImageArray(input, true, footprint) == Status::Ok);
	CHECK(footprint.Items == 16);
	CHECK(detail::MeasureImageArray(input, false, footprint) == Status::LimitExceeded);
	detail::EvaluationBudget budget(0);
	detail::AllocationReservation charge;
	ImageArray output;
	CHECK(detail::CollectImageArray(input, false, budget, output, charge) == Status::LimitExceeded);
	CHECK(budget.Peak() == 0);
}

TEST_CASE(
	"Image collector applies the full nested item count limit",
	"[imagegraph][image_collector][evaluation_budget]"
) {
	ImageArray input;
	input.Images = {Pixel(1)};
	input.Items.resize(Limits::MaximumArrayElements, ImageArrayItem{size_t{0}});
	const detail::ImageArrayInput inputs[]{&input};
	detail::ImageArrayFootprint footprint;
	REQUIRE(detail::MeasureImageArray(inputs, true, footprint) == Status::Ok);
	CHECK(footprint.Items == Limits::MaximumArrayElements);
	CHECK(detail::MeasureImageArray(inputs, false, footprint) == Status::LimitExceeded);
	input.Items.push_back(ImageArrayItem{size_t{0}});
	CHECK(detail::MeasureImageArray(inputs, true, footprint) == Status::LimitExceeded);
}

TEST_CASE(
	"Compiled image collector keeps nested inputs and mixed image offsets", "[imagegraph][image_collector]"
) {
	Document document;
	document.FormatVersion = 2;
	document.Nodes = {
		{"red",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{10, 0, 0, 255}}}},
		{"blue",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{20, 0, 0, 255}}}},
		{"inner", "value.array", "", {}, {}, {{"red", ValueType::Image, std::nullopt}}},
		{"outer",
		 "value.array",
		 "",
		 {},
		 {},
		 {{"blue", ValueType::Image, std::nullopt}, {"inner", ValueType::Array, std::nullopt}}},
	};
	document.Links = {
		{"red", "image", "inner", "red"},
		{"blue", "image", "outer", "blue"},
		{"inner", "array", "outer", "inner"}
	};
	document.Outputs = {{"all", "outer", "array"}};
	for (const bool spread : {false, true}) {
		document.Nodes.back().Values = {{"spread", spread}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		ImageArray output;
		INFO(diagnostic.Message);
		REQUIRE(EvaluateArray(document, plan, "all", {}, output, diagnostic) == Status::Ok);
		REQUIRE(output.Images.size() == 2);
		CHECK(output.Images[0].Pixels == Pixel(20).Pixels);
		CHECK(output.Images[1].Pixels == Pixel(10).Pixels);
		REQUIRE(output.Items.size() == 2);
		CHECK(std::get<size_t>(output.Items[0].Data) == 0);
		if (spread)
			CHECK(std::get<size_t>(output.Items[1].Data) == 1);
		else {
			const auto &children = std::get<std::vector<ImageArrayItem>>(output.Items[1].Data);
			REQUIRE(children.size() == 1);
			CHECK(std::get<size_t>(children[0].Data) == 1);
		}
	}
}
