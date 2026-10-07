#include "../src/SourceChoice.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_choice_aliases")
using namespace engine::imagegraph;

namespace {
	struct ChoiceCase {
		std::string_view SourceNode;
		int32_t SourceIndex;
		std::string_view Name;
		std::vector<std::string_view> Labels;
	};

	const std::vector<ChoiceCase> Cases = {
		{"Node_2D_light",
		 0,
		 "Light shape",
		 {"Point", "Ellipse", "Line", "Line asymmetric", "Saber", "Spot", "Flame"}},
		{"Node_2D_light", 10, "Attenuation", {"Quadratic", "Invert quadratic", "Linear", "Custom"}},
		{"Node_Bend", 2, "Type", {"Arc", "Wave"}},
		{"Node_Image_Grid", 0, "Main Axis", {"Horizontal", "Vertical"}},
		{"Node_Liquefy", 0, "Type", {"Push", "Twirl", "Pinch", "Bloat"}},
		{"Node_MK_Dialog", 0, "Track Position", {"Start", "End", "Start/End", "Always"}},
		{"Node_MK_Dialog", 1, "Apply Group", {"Letter", "Words", "All"}},
		{"Node_MK_Grass", 7, "Shape", {"Dense Bush", "V", "Hash", "Line", "W", "Surface"}},
		{"Node_MK_Tile", 5, "Edge Type", {"Uniform", "Individual"}},
		{"Node_MK_Tile", 12, "Edge Sprite", {"Single", "Left + Center + Right"}},
		{"Node_MK_Tile", 13, "Edge Transform", {"Flip", "Rotate"}},
		{"Node_Normal_Light", 0, "Type", {"Point", "Sun", "Line", "Spot"}},
		{"Node_Normal_Light", 9, "Attenuation", {"Quadratic", "Invert quadratic", "Linear", "Custom"}},
		{"Node_Repeat", 1, "Select Mode", {"Index", "Area", "Linear", "Surface"}},
		{"Node_Shadow_Cast", 0, "Type", {"Point", "Sun"}},
		{"Node_Shadow_Cast", 5, "Attenuation", {"Quadratic", "Invert quadratic", "Linear", "Custom"}},
	};

	const CatalogueInput *
	FindSelector(const CatalogueEntry &entry, int32_t sourceIndex, std::string_view name) {
		for (const CatalogueInput &input : entry.Inputs)
			if (input.SourceIndex == sourceIndex && input.Name == name) return &input;
		for (const CatalogueInput &input : entry.DynamicTemplate)
			if (input.SourceIndex == sourceIndex && input.Name == name) return &input;
		return nullptr;
	}
}

TEST_CASE("Refreshed source selectors retain raw slots and clamp rules", "[imagegraph][source_choice]") {
	for (const ChoiceCase &choiceCase : Cases) {
		INFO(choiceCase.SourceNode);
		INFO(choiceCase.SourceIndex);
		const CatalogueEntry *entry = FindCatalogueSource(choiceCase.SourceNode);
		REQUIRE(entry);
		const CatalogueInput *input = FindSelector(*entry, choiceCase.SourceIndex, choiceCase.Name);
		REQUIRE(input);
		CHECK(input->Name == choiceCase.Name);
		CHECK(input->Type == ValueType::Enum);
		std::string labels;
		for (const auto label : choiceCase.Labels) {
			if (!labels.empty()) labels += ";";
			labels += label;
		}
		CHECK(input->Choices == labels);
		REQUIRE(input->SourceBehavior);
		CHECK(input->SourceBehavior->ChoiceClamp == SourceChoiceClamp::Default);
		CHECK(input->SourceBehavior->FractionalInterpolation == true);
		CHECK(input->SourceBehavior->StrictSuggestion == false);
		REQUIRE(input->SourceChoices);
		CHECK(input->SourceChoices->Status == SourceChoicesStatus::Resolved);
		REQUIRE(input->SourceChoices->RawCount);
		CHECK(*input->SourceChoices->RawCount == choiceCase.Labels.size());
		REQUIRE(input->SourceChoices->Entries.size() == choiceCase.Labels.size());
		for (size_t index = 0; index < choiceCase.Labels.size(); ++index) {
			const CatalogueSourceChoice &actual = input->SourceChoices->Entries[index];
			CHECK(actual.SourceIndex == static_cast<int32_t>(index));
			CHECK(actual.Label == choiceCase.Labels[index]);
			CHECK_FALSE(actual.Separator);
		}
		CHECK(detail::NormalizeSourceChoice(*input, -4, false) == 0);
		CHECK(
			detail::NormalizeSourceChoice(*input, choiceCase.Labels.size() + 3.0, false) ==
			choiceCase.Labels.size() - 1.0
		);
		CHECK(detail::NormalizeSourceChoice(*input, .5, false) == .5);
		CHECK(detail::NormalizeSourceChoice(*input, -4, true) == -4);
		CHECK(
			detail::NormalizeSourceChoice(*input, choiceCase.Labels.size() + 3.0, true) ==
			choiceCase.Labels.size() + 3.0
		);
	}
}

TEST_CASE("Refreshed selector enum keys survive document round trip", "[imagegraph][source_choice]") {
	const CatalogueEntry *bend = FindCatalogueSource("Node_Bend");
	const CatalogueEntry *normalLight = FindCatalogueSource("Node_Normal_Light");
	REQUIRE(bend);
	REQUIRE(normalLight);
	const CatalogueInput *bendType = FindCatalogueInputIndex(*bend, 2);
	const CatalogueInput *lightType = FindSelector(*normalLight, 0, "Type");
	const CatalogueInput *lightAttenuation = FindSelector(*normalLight, 9, "Attenuation");
	REQUIRE(bendType);
	REQUIRE(lightType);
	REQUIRE(lightAttenuation);

	Document document;
	document.FormatVersion = 9;
	document.Groups.push_back({"group", "Group", {}});
	document.Nodes.push_back(
		{"bend", std::string(bend->Type), "group", {}, {{std::string(bendType->Id), EnumValue{1}}}}
	);
	document.Nodes.push_back({"light", std::string(normalLight->Type), "group", {}, {}});
	for (const CatalogueInput &input : normalLight->DynamicTemplate)
		document.Nodes.back().DynamicInputs.push_back(
			{std::string(input.Id) + "_0", input.Type, CatalogueDefault(input)}
		);
	for (auto &input : document.Nodes.back().DynamicInputs) {
		if (input.Id == std::string(lightType->Id) + "_0") input.Default = EnumValue{3};
		if (input.Id == std::string(lightAttenuation->Id) + "_0") input.Default = EnumValue{2};
	}
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"Helper alias labels keep separator slots out of the inspector list", "[imagegraph][source_choice]"
) {
	const CatalogueEntry *entry = FindCatalogueSource("Node_Path_Shape_3D");
	REQUIRE(entry);
	const CatalogueInput *shape = FindSelector(*entry, 2, "Shape");
	REQUIRE(shape);
	CHECK(shape->Choices == "Rectangle;Ellipse;Regular Polygon;Star;Spring;Spring Sphere;Spiral");
	REQUIRE(shape->SourceChoices);
	CHECK(shape->SourceChoices->RawCount == 9);
	REQUIRE(shape->SourceChoices->Entries.size() == 9);
	CHECK(shape->SourceChoices->Entries[3].Separator);
	CHECK(shape->SourceChoices->Entries[5].Separator);
	CHECK(shape->SourceChoices->Entries[4].SourceIndex == 4);
	CHECK(shape->SourceChoices->Entries[4].Label == "Star");
	CHECK(shape->SourceChoices->Entries[8].SourceIndex == 8);
	CHECK(shape->SourceChoices->Entries[8].Label == "Spiral");
	CHECK(detail::NormalizeSourceChoice(*shape, 12, false) == 8);
}
