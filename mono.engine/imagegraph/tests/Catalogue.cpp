#include "../src/SourceChoice.hpp"
#include "../src/ValueText.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <set>
#include <string>

TEST_SUITE_ID("engine.imagegraph.catalogue")

using namespace engine::imagegraph;

TEST_CASE("Catalogue covers reviewed selected pinned source declarations", "[imagegraph]") {
	const auto catalogue = Catalogue();
	// The selected inventory includes reviewed documentation, project fixtures, and source evidence.
	CHECK(catalogue.size() == 886);
	REQUIRE(FindCatalogueSource("Node_String_Insert"));
	REQUIRE(FindCatalogueSource("Node_Spout_Receive"));
	REQUIRE(FindCatalogueSource("Node_Path_Redistribute"));
	CHECK(FindCatalogueSource("Node_Path_Redistribute")->Family == "undocumented");
	const std::pair<std::string_view, std::pair<std::string_view, std::string_view>> reviewed[] = {
		{"Node_Armature", {"Armature Create", "scripts/node_armature/node_armature.gml"}},
		{"Node_Armature_Bone", {"Armature Bone", "scripts/node_armature_bone/node_armature_bone.gml"}},
		{"Node_Armature_IK", {"Armature IK", "scripts/node_armature_ik/node_armature_ik.gml"}},
		{"Node_Armature_Mirror",
		 {"Armature Mirror", "scripts/node_armature_mirror/node_armature_mirror.gml"}},
		{"Node_Armature_Subdivide",
		 {"Armature Subdivide", "scripts/node_armature_subdivide/node_armature_subdivide.gml"}},
		{"Node_Array_Cumulative",
		 {"Array Cumulative", "scripts/node_array_cumulative/node_array_cumulative.gml"}},
	};
	for (const auto &[source, expected] : reviewed) {
		const CatalogueEntry *entry = FindCatalogueSource(source);
		INFO(source);
		REQUIRE(entry);
		CHECK(entry->Title == expected.first);
		CHECK(entry->SourceFile == expected.second);
	}
	const CatalogueEntry *armature = FindCatalogueSource("Node_Armature");
	REQUIRE(armature);
	// Zero ordinary ports does not describe the unverified bone editor state.
	CHECK(armature->Inputs.empty());
	REQUIRE(armature->Outputs.size() == 1);
	CHECK(armature->Outputs.front().Type == ValueType::Armature);
	for (const CatalogueEntry &entry : catalogue) {
		CHECK(entry.Title != "Channel Swizzle");
	}
	std::set<std::string_view> types;
	for (const CatalogueEntry &entry : catalogue) {
		INFO(entry.Type);
		CHECK(entry.Type.starts_with("pc."));
		CHECK(types.insert(entry.Type).second);
		CHECK(FindSchema(entry.Type) == &entry.Schema);
		CHECK(FindCatalogueSource(entry.SourceNode) == &entry);
		std::set<std::string_view> inputs, outputs;
		for (const CatalogueInput &input : entry.Inputs) {
			CHECK(inputs.insert(input.Id).second);
			if (input.Default.empty()) continue;
			INFO(input.Id);
			const auto value = CatalogueDefault(input);
			REQUIRE(value);
			CHECK(
				(IsAuthoredValueType(input.Type) ||
				 (entry.Type == "pc.group_input" && input.Id == "parent_value" &&
				  input.Type == ValueType::Any) ||
				 (entry.Type == "pc.argument" && input.Id == "default_value" && input.SourceIndex == 2 &&
				  input.SourceKind == "Text" && input.Type == ValueType::Any))
			);
		}
		for (const CatalogueOutput &output : entry.Outputs) {
			CHECK(outputs.insert(output.Id).second);
			if (!output.ConstructorDefault.empty()) {
				Value value;
				INFO(output.Id);
				CHECK(detail::ReadValueText(output.ConstructorDefault, value));
			}
		}
	}
}

TEST_CASE("Catalogue retains cold constructor values and unresolved expressions", "[imagegraph]") {
	const auto output = [](std::string_view source) -> const CatalogueOutput & {
		const CatalogueEntry *entry = FindCatalogueSource(source);
		REQUIRE(entry);
		REQUIRE(!entry->Outputs.empty());
		return entry->Outputs.front();
	};
	const CatalogueOutput &cache = output("Node_Cache");
	CHECK(cache.Type == ValueType::Image);
	CHECK(cache.ConstructorExpression == "noone");
	Value initial;
	REQUIRE(detail::ReadValueText(cache.ConstructorDefault, initial));
	CHECK(initial == Value{int64_t{-4}});
	const CatalogueOutput &array = output("Node_Cache_Array");
	CHECK(array.ConstructorExpression == "[]");
	REQUIRE(detail::ReadValueText(array.ConstructorDefault, initial));
	const auto *empty = std::get_if<ArrayValue>(&initial);
	REQUIRE(empty);
	CHECK(empty->ElementType == ValueType::Any);
	CHECK(empty->Items.empty());
	CHECK(empty->Elements.empty());
	CHECK(output("Node_Number").ConstructorDefault == "d 0");
	CHECK(output("Node_Vector2").ConstructorDefault == "v 0 0");
	CHECK(output("Node_String").ConstructorDefault == "s \"\"");
	const CatalogueOutput &path = output("Node_Path_Join");
	CHECK(path.ConstructorExpression == "self");
	CHECK(path.ConstructorDefault.empty());
	const CatalogueOutput &matrix = output("Node_Matrix");
	CHECK(matrix.ConstructorExpression == "new Matrix(3)");
	CHECK(matrix.ConstructorDefault == "m 3 3 0 0 0 0 0 0 0 0 0");
}

TEST_CASE("Catalogue keeps source input indices, mask modifiers and map inputs", "[imagegraph]") {
	const CatalogueEntry *bw = FindCatalogueSource("Node_BW");
	REQUIRE(bw);
	CHECK(bw->Type == "pc.bw");
	CHECK(bw->Family == "filter");
	const std::pair<std::string_view, int32_t> expected[] = {
		{"surface_in", 0},
		{"brightness", 1},
		{"contrast", 2},
		{"mask", 3},
		{"mix", 4},
		{"active", 5},
		{"channel", 6},
		{"invert_mask", 7},
		{"mask_feather", 8},
		{"brightness_map", 9},
		{"contrast_map", 10},
	};
	for (const auto &[id, index] : expected) {
		INFO(id);
		const CatalogueInput *input = FindCatalogueInputIndex(*bw, index);
		REQUIRE(input);
		CHECK(input->Id == id);
	}
	const CatalogueInput *contrast = FindCatalogueInput(*bw, "contrast");
	REQUIRE(contrast);
	CHECK(CatalogueDefault(*contrast) == Value{1.0});
	const CatalogueInput *range = FindCatalogueInput(*bw, "contrast_map_range");
	REQUIRE(range);
	CHECK(range->SourceIndex == -1);
	CHECK(CatalogueDefault(*range) == Value{Vector2{0.0, 1.0}});
	const CatalogueInput *channel = FindCatalogueInput(*bw, "channel");
	REQUIRE(channel);
	CHECK(CatalogueDefault(*channel) == Value{int64_t{15}});
}

TEST_CASE("Catalogue enum inputs carry source labels", "[imagegraph]") {
	const CatalogueEntry *blend = FindCatalogueSource("Node_Blend");
	REQUIRE(blend);
	const CatalogueInput *mode = FindCatalogueInput(*blend, "blend_mode");
	REQUIRE(mode);
	CHECK(mode->Type == ValueType::Enum);
	CHECK(CatalogueChoiceCount(*mode) == 25);
	CHECK(mode->Choices.starts_with("Normal;Replace;Multiply"));
}

TEST_CASE("Catalogue nodes round trip, validate enums and name missing executors", "[imagegraph]") {
	// Any filter whose native executor is still pending shows the refusal.
	const CatalogueEntry *pending = nullptr;
	for (const CatalogueEntry &entry : Catalogue()) {
		const CatalogueInput *surface = FindCatalogueInputIndex(entry, 0);
		if (!HasNativeExecutor(entry.Type) && surface && surface->Id == "surface_in" &&
			!entry.Outputs.empty() && entry.Outputs.front().Id == "surface_out") {
			pending = &entry;
			break;
		}
	}
	REQUIRE(pending);
	const std::string pendingType(pending->Type);
	Document document;
	document.FormatVersion = 6;
	document.Nodes.push_back(
		{"fill",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}}
	);
	document.Nodes.push_back({"pending", pendingType, "", {}, {}});
	document.Links.push_back({"fill", "image", "pending", "surface_in"});
	document.Outputs.push_back({"final", "pending", "surface_out"});

	Document read;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), read, diagnostic) == Status::Ok);
	CHECK(read == document);

	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image image;
	CHECK(Evaluate(document, plan, "final", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "pending");
	CHECK(diagnostic.Message == "node has no native executor");

	document.Nodes.push_back({"blend", "pc.blend", "", {}, {{"blend_mode", EnumValue{25}}}});
	CHECK(Compile(document, plan, diagnostic) == Status::Ok);
	const auto *mode = FindCatalogueInput(*FindCatalogueEntry("pc.blend"), "blend_mode");
	REQUIRE(mode);
	CHECK(detail::NormalizeSourceChoice(*mode, 25, false) == 25);
	document.Nodes.back().Values.front().Data = EnumValue{999};
	CHECK(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(detail::NormalizeSourceChoice(*mode, 999, false) == 30);
	Document unclamped;
	REQUIRE(Read(Write(document), unclamped, diagnostic) == Status::Ok);
	CHECK(std::get<EnumValue>(unclamped.Nodes.back().Values.front().Data).Value == 999);
	CHECK(detail::NormalizeSourceChoice(*mode, -4, false) == 0);
}

TEST_CASE("Runtime-only value types name themselves but hold no authored value", "[imagegraph]") {
	CHECK(ValueTypeName(ValueType::Particle) == "particle");
	CHECK(ParseValueTypeName("fluid_domain") == ValueType::FluidDomain);
	CHECK(ParseValueTypeName("dynamic_surface") == ValueType::DynamicSurface);
	CHECK_FALSE(ParseValueTypeName("missing"));
	CHECK(IsAuthoredValueType(ValueType::Path2D));
	CHECK_FALSE(IsAuthoredValueType(ValueType::Image));
	CHECK_FALSE(IsAuthoredValueType(ValueType::Particle));
	CHECK_FALSE(IsAuthoredValueType(ValueType::AudioBit));
}

TEST_CASE("Dynamic input groups repeat a source template after the fixed inputs", "[imagegraph]") {
	const CatalogueEntry *composite = FindCatalogueSource("Node_Composite");
	REQUIRE(composite);
	CHECK(composite->DynamicFixedLength == 4);
	CHECK(composite->DynamicGroupLength == 8);
	CHECK(composite->Schema.DynamicInputs);
	CHECK(
		std::count_if(
			composite->DynamicTemplate.begin(), composite->DynamicTemplate.end(), [](const auto &input) {
				return input.SourceIndex >= 0;
			}
		) == 8
	);
	size_t unitGroup = 0;
	const auto *unit = FindDynamicTemplate(*composite, "position_unit_2", unitGroup);
	REQUIRE(unit);
	CHECK(unitGroup == 2);
	CHECK(unit->SourceIndex == -1);
	CHECK(unit->Type == ValueType::Enum);
	size_t group = 0;
	const CatalogueInput *blend = FindDynamicTemplate(*composite, "blend_2", group);
	REQUIRE(blend);
	CHECK(group == 2);
	CHECK(blend->SourceIndex == 4);
	CHECK(blend->Type == ValueType::Enum);
	// Source index of group 2's Blend: 4 fixed inputs + 2 groups of 8 + offset 4.
	CHECK(
		composite->DynamicFixedLength + static_cast<int32_t>(group) * composite->DynamicGroupLength +
			blend->SourceIndex ==
		24
	);
	CHECK_FALSE(FindDynamicTemplate(*composite, "blend_", group));
	CHECK_FALSE(FindDynamicTemplate(*composite, "missing_0", group));

	Document document;
	document.FormatVersion = 6;
	document.Nodes.push_back(
		{"fill",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{1, 2, 3, 255}}}}
	);
	document.Nodes.push_back(
		{"stack", "pc.composite", "", {}, {}, {{"surface_0", ValueType::Image, std::nullopt}}}
	);
	document.Links.push_back({"fill", "image", "stack", "surface_0"});
	document.Outputs.push_back({"final", "stack", "surface_out"});
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(document, plan, diagnostic) == Status::Ok);
}

TEST_CASE("Pinned Pixel Composer metadata preserves hidden source constructors", "[imagegraph]") {
	const auto *instancer = FindCatalogueEntry("pc.3_d_instancer");
	REQUIRE(instancer);
	const auto *selector = FindCatalogueInput(*instancer, "colors_per_index_select");
	REQUIRE(selector);
	CHECK(selector->SourceIndex == -1);
	CHECK(selector->Type == ValueType::Enum);
	CHECK(selector->Default == "e 0");
	CHECK(selector->Choices == "Index Loop;Index Ping-pong;Random");

	const auto *exportNode = FindCatalogueEntry("pc.export");
	REQUIRE(exportNode);
	const auto *framerateUnit = FindCatalogueInput(*exportNode, "framerate_unit");
	REQUIRE(framerateUnit);
	CHECK(framerateUnit->Default == "e 1");
	CHECK(framerateUnit->SourceKind == "ExportFramerateUnit");
	CHECK(framerateUnit->Choices == "FPS;Relative to Preview");

	const auto *path = FindCatalogueEntry("pc.path_3_d");
	REQUIRE(path);
	const auto output = [](const CatalogueEntry &entry, std::string_view id) -> const CatalogueOutput * {
		const auto found =
			std::find_if(entry.Outputs.begin(), entry.Outputs.end(), [&](const auto &candidate) {
				return candidate.Id == id;
			});
		return found == entry.Outputs.end() ? nullptr : &*found;
	};
	const auto *position = output(*path, "position_out");
	const auto *pathData = output(*path, "path_data");
	REQUIRE(position);
	REQUIRE(pathData);
	CHECK(position->Type == ValueType::Vector3);
	CHECK(pathData->Type == ValueType::Path3D);
	const auto *camera = FindCatalogueEntry("pc.path_3_d_camera");
	const auto *transform = FindCatalogueEntry("pc.path_3_d_transform");
	REQUIRE(camera);
	REQUIRE(transform);
	REQUIRE(output(*camera, "rendered"));
	REQUIRE(output(*transform, "path"));
	CHECK(output(*camera, "rendered")->Type == ValueType::Path3D);
	CHECK(output(*transform, "path")->Type == ValueType::Path3D);

	const auto *draw = FindCatalogueEntry("pc.pb_draw_curve");
	REQUIRE(draw);
	CHECK(draw->DynamicFixedLength == 13);
	CHECK(draw->DynamicGroupLength == 26);
	CHECK(draw->DynamicGroupLimit == 64);
	CHECK(MaximumDynamicInputsForType(draw->Type) == 1792);
	CHECK(MaximumDynamicInputsForType("pc.blur") == 64);
	REQUIRE(draw->DynamicTemplate.size() == 28);
	size_t group = 0;
	REQUIRE(FindDynamicTemplate(*draw, "pattern_scale_0", group));
	CHECK(group == 0);
	REQUIRE(FindDynamicTemplate(*draw, "pattern_scale_unit_0", group));
	const auto *shines = FindDynamicTemplate(*draw, "shines_0", group);
	REQUIRE(shines);
	CHECK(shines->Type == ValueType::Array);
}

TEST_CASE("Version 7 project settings round trip and are validated", "[imagegraph]") {
	Document document;
	document.FormatVersion = 7;
	document.Nodes.push_back(
		{"fill", "image.solid", "", {}, {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}}
	);
	document.Outputs.push_back({"final", "fill", "image"});
	document.Project = ProjectSettings{64, 48, 1, 0, {{1, 2, 3, 4}}};
	Document read;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), read, diagnostic) == Status::Ok);
	CHECK(read == document);
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::Ok);
	document.Project->Oversample = 13;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	document.Project->Oversample = 3;
	document.FormatVersion = 6;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	// Version 6 text has no project record.
	CHECK(Write(document).find("project") == std::string::npos);
}

#include "../src/NodeExecutors.hpp"

TEST_CASE("Every native executor belongs to a catalogue entry", "[imagegraph]") {
	for (const std::string_view type : detail::ExecutorTypes()) {
		INFO(type);
		CHECK(FindCatalogueEntry(type));
	}
}

TEST_CASE(
	"Source processor depths distinguish complete palettes numeric rows and vector leaves",
	"[imagegraph][node_processor]"
) {
	const CatalogueEntry *sample = FindCatalogueEntry("pc.gradient_sample");
	REQUIRE(sample);
	const auto *ratio = FindCatalogueInput(*sample, "ratio");
	REQUIRE(ratio);
	CHECK(ratio->ArrayDepthKnown);
	CHECK(ratio->ArrayDepth == 1);
	const auto *mode = FindCatalogueInput(*sample, "attribute_array_process");
	REQUIRE(mode);
	CHECK(mode->Choices == "Loop;Hold;Expand;Expand inverse");
	CHECK(CatalogueDefault(*mode) == std::optional<Value>{EnumValue{0}});
	const CatalogueEntry *magnitude = FindCatalogueEntry("pc.vector_magnitude");
	REQUIRE(magnitude);
	const auto *vector = FindCatalogueInput(*magnitude, "vector");
	REQUIRE(vector);
	CHECK(vector->ArrayDepthKnown);
	CHECK(vector->ArrayDepth == 1);
	const CatalogueEntry *cross = FindCatalogueEntry("pc.vector_cross_2_d");
	REQUIRE(cross);
	const auto *point = FindCatalogueInput(*cross, "point_1");
	REQUIRE(point);
	CHECK(point->ArrayDepthKnown);
	CHECK(point->ArrayDepth == 1);
}

TEST_CASE(
	"Source choices preserve separator indices and verified real enum behavior", "[imagegraph][source_choice]"
) {
	const auto *blend = FindCatalogueEntry("pc.blend");
	REQUIRE(blend);
	const auto *mode = FindCatalogueInput(*blend, "blend_mode");
	REQUIRE(mode);
	REQUIRE(mode->SourceBehavior);
	CHECK(mode->SourceBehavior->StrictSuggestion == false);
	CHECK(mode->SourceBehavior->ActualConnectability == SourceActualConnectability::General);
	CHECK(mode->SourceBehavior->FractionalInterpolation == true);
	CHECK(mode->SourceBehavior->ChoiceClamp == SourceChoiceClamp::Default);
	REQUIRE(mode->SourceChoices);
	CHECK(mode->SourceChoices->RawCount == 31);
	REQUIRE(mode->SourceChoices->Entries.size() == 31);
	CHECK(mode->SourceChoices->Entries[2].SourceIndex == 2);
	CHECK(mode->SourceChoices->Entries[2].Separator);
	CHECK(mode->SourceChoices->Entries[2].Label.empty());
	CHECK(mode->SourceChoices->Entries[3].SourceIndex == 3);
	CHECK(mode->SourceChoices->Entries[3].Label == "Multiply");
	CHECK(CatalogueSourceEnumValue(*mode, EnumValue{0}));
	CHECK(CatalogueSourceEnumValue(*mode, int64_t{3}));
	CHECK(CatalogueSourceEnumValue(*mode, .5));
	CHECK_FALSE(CatalogueSourceEnumValue(*mode, std::string("Normal")));
	const auto *process = FindCatalogueInput(*blend, "attribute_array_process");
	REQUIRE(process);
	CHECK_FALSE(process->SourceBehavior);
	CHECK_FALSE(CatalogueSourceEnumValue(*process, .5));
	for (const auto &entry : Catalogue()) {
		for (const auto &input : entry.Inputs) {
			if (!input.SourceChoices || input.SourceChoices->Status != SourceChoicesStatus::Resolved)
				continue;
			INFO(entry.Type);
			INFO(input.Id);
			REQUIRE(input.SourceChoices->RawCount);
			CHECK(input.SourceChoices->Entries.size() == *input.SourceChoices->RawCount);
			for (size_t index = 0; index < input.SourceChoices->Entries.size(); ++index)
				CHECK(input.SourceChoices->Entries[index].SourceIndex == static_cast<int32_t>(index));
		}
	}
}

TEST_CASE(
	"Source choice policy requires known scalar ranges but disabled and array getters bypass clamps",
	"[imagegraph][source_choice]"
) {
	const auto *entry = FindCatalogueEntry("pc.gradient_palette");
	REQUIRE(entry);
	CatalogueInput input = *FindCatalogueInput(*entry, "interpolation");
	CHECK(detail::NormalizeSourceChoice(input, -4, false) == 0);
	CHECK(detail::NormalizeSourceChoice(input, 8, false) == 4);
	CHECK(detail::NormalizeSourceChoice(input, .5, false) == .5);
	CHECK(detail::NormalizeSourceChoice(input, -4, true) == -4);
	CHECK(detail::NormalizeSourceChoice(input, 8, true) == 8);
	input.SourceChoices->RawCount.reset();
	CHECK_FALSE(detail::NormalizeSourceChoice(input, .5, false));
	input.SourceBehavior->ChoiceClamp = SourceChoiceClamp::Disabled;
	CHECK(detail::NormalizeSourceChoice(input, 8, false) == 8);
	input.SourceBehavior->ChoiceClamp = SourceChoiceClamp::Unknown;
	CHECK_FALSE(detail::NormalizeSourceChoice(input, .5, false));
	input.SourceBehavior->ChoiceClamp = SourceChoiceClamp::Always;
	input.SourceChoices->RawCount = 0;
	CHECK_FALSE(detail::NormalizeSourceChoice(input, .5, false));
	CHECK_FALSE(detail::SourceChoiceNumber(Value{std::string("wrong")}));
}

TEST_CASE(
	"Manual source matrix array inputs retain exact types and known depth bounds",
	"[imagegraph][catalogue][matrix]"
) {
	// Pinned node_matrix_get/set loop position pairs; get/set_vector loop integer positions.
	for (const auto type : {"pc.matrix_get", "pc.matrix_set"}) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		CHECK_FALSE(FindCatalogueInput(*entry, "attribute_process"));
		const auto *position = FindCatalogueInput(*entry, "position");
		REQUIRE(position);
		CHECK(CatalogueAuthoredArray(
			*entry, *position, ArrayValue{ValueType::Vector2, {Vector2{0, 0}, Vector2{1, 1}}}
		));
		CHECK(CatalogueAuthoredArray(*entry, *position, ArrayValue{ValueType::Scalar, {0.0, 1.0}}));
		CHECK(CatalogueAuthoredArray(
			*entry, *position, ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}
		));
		CHECK_FALSE(
			CatalogueAuthoredArray(*entry, *position, ArrayValue{ValueType::Text, {std::string("pair")}})
		);
		CHECK_FALSE(
			CatalogueAuthoredArray(*entry, *position, ArrayValue{ValueType::Scalar, {0.0, 1.0, 2.0}})
		);
		CatalogueInput unknown = *position;
		unknown.ArrayDepthKnown = false;
		CHECK_FALSE(CatalogueAuthoredArray(*entry, unknown, ArrayValue{ValueType::Vector2, {Vector2{0, 0}}}));
		ArrayValue nested{ValueType::Vector2, {}};
		nested.Nested = {{Vector2{0, 0}}};
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *position, nested));
	}
	const auto *set = FindCatalogueEntry("pc.matrix_set");
	const auto *value = FindCatalogueInput(*set, "value");
	REQUIRE(value);
	CHECK(CatalogueAuthoredArray(*set, *value, ArrayValue{ValueType::Scalar, {3.0, 4.0}}));
	CHECK(CatalogueAuthoredArray(*set, *value, ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}));
	CHECK_FALSE(CatalogueAuthoredArray(*set, *value, ArrayValue{ValueType::Vector2, {Vector2{3, 4}}}));
	for (const auto type : {"pc.matrix_get_vector", "pc.matrix_set_vector"}) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const auto *position = FindCatalogueInput(*entry, "position");
		REQUIRE(position);
		CHECK(CatalogueAuthoredArray(
			*entry, *position, ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}
		));
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *position, ArrayValue{ValueType::Scalar, {0.0, 1.0}}));
		CHECK_FALSE(
			CatalogueAuthoredArray(*entry, *position, ArrayValue{ValueType::Text, {std::string("row")}})
		);
	}
	ArrayValue large{ValueType::Scalar, {}};
	large.Elements.resize(Limits::MaximumArrayElements + 1, 0.0);
	CHECK_FALSE(CatalogueAuthoredArray(*set, *value, large));
	const auto *getVector = FindCatalogueEntry("pc.matrix_get_vector");
	const auto *direction = FindCatalogueInput(*getVector, "direction");
	REQUIRE(direction);
	CHECK_FALSE(CatalogueAuthoredArray(
		*getVector, *direction, ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}
	));
}

TEST_CASE(
	"Manual numeric value families admit bounded flat numeric arrays",
	"[imagegraph][catalogue][manual_values]"
) {
	const std::pair<std::string_view, std::string_view> routes[] = {
		{"pc.number_simple", "value"},
		{"pc.compare", "a"},
		{"pc.compare", "b"},
		{"pc.math", "a"},
		{"pc.math", "b"},
		{"pc.math", "amount"},
		{"pc.vector_math", "a"},
		{"pc.vector_math", "b"}
	};
	for (const auto &[type, id] : routes) {
		INFO(type);
		INFO(id);
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		CHECK_FALSE(FindCatalogueInput(*entry, "attribute_process"));
		const auto *input = FindCatalogueInput(*entry, id);
		REQUIRE(input);
		CHECK(input->Type == ValueType::Scalar);
		CHECK(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Scalar, {1.0, 2.0}}));
		CHECK(
			CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}}})
		);
		CHECK(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Scalar, {}}));
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Text, {std::string("1")}}));
		// Authored Float arrays keep numeric leaves; linked Boolean arithmetic uses runtime casts.
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Boolean, {false, true}}));
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, ArrayValue{ValueType::Vector2, {Vector2{1, 2}}}));
		ArrayValue nested{ValueType::Scalar, {}};
		nested.Nested = {{1.0, 2.0}};
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, nested));
		CatalogueInput unknown = *input;
		unknown.ArrayDepthKnown = false;
		CHECK_FALSE(CatalogueAuthoredArray(*entry, unknown, ArrayValue{ValueType::Scalar, {1.0}}));
		ArrayValue invalid{ValueType::Scalar, {std::numeric_limits<double>::infinity()}};
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, invalid));
		ArrayValue large{ValueType::Scalar, {}};
		large.Elements.resize(Limits::MaximumArrayElements + 1, 1.0);
		CHECK_FALSE(CatalogueAuthoredArray(*entry, *input, large));
	}
	const auto *math = FindCatalogueEntry("pc.math");
	REQUIRE(math);
	for (const auto id : {"type", "angle", "to_integer", "output_vector", "from", "to"}) {
		const auto *input = FindCatalogueInput(*math, id);
		REQUIRE(input);
		CHECK_FALSE(CatalogueAuthoredArray(*math, *input, ArrayValue{ValueType::Scalar, {1.0, 2.0}}));
	}
}

TEST_CASE(
	"Vector Math authored numeric arrays require exact source input metadata",
	"[imagegraph][catalogue][manual_values]"
) {
	const auto *entry = FindCatalogueEntry("pc.vector_math");
	REQUIRE(entry);
	const ArrayValue values{ValueType::Scalar, {1.0, 2.0}};
	for (const auto id : {"a", "b"}) {
		const auto *input = FindCatalogueInput(*entry, id);
		REQUIRE(input);
		CHECK(input->SourceIndex == (std::string_view(id) == "a" ? 1 : 2));
		CHECK(input->SourceKind == "Float");
		CHECK(input->ArrayDepthKnown);
		CHECK(input->ArrayDepth == 0);
		CHECK(CatalogueAuthoredArray(*entry, *input, values));
		CatalogueInput altered = *input;
		altered.SourceIndex = 5;
		CHECK_FALSE(CatalogueAuthoredArray(*entry, altered, values));
		altered = *input;
		altered.SourceKind = "Int";
		CHECK_FALSE(CatalogueAuthoredArray(*entry, altered, values));
		altered = *input;
		altered.ArrayDepth = 1;
		CHECK_FALSE(CatalogueAuthoredArray(*entry, altered, values));
	}
	for (const auto &input : entry->Inputs)
		if (input.Id != "a" && input.Id != "b") CHECK_FALSE(CatalogueAuthoredArray(*entry, input, values));
}

TEST_CASE(
	"Catalogue preserves source declaration array classification independently of native shape",
	"[imagegraph][source_array_classification]"
) {
	auto input = [](std::string_view node, std::string_view port) -> const CatalogueInput & {
		const auto *entry = FindCatalogueEntry(node);
		REQUIRE(entry);
		const auto *value = FindCatalogueInput(*entry, port);
		REQUIRE(value);
		return *value;
	};
	// The source Array constructor caches type_array=1 but typeArray reads declaration display/type.
	CHECK(input("pc.matrix_multiply_vector", "vector").SourceKind == "Vector");
	CHECK(input("pc.matrix_multiply_vector", "vector").SourceArrayClassification == false);
	CHECK(input("pc.scatter", "array_indices").SourceKind == "IArray");
	CHECK(input("pc.scatter", "array_indices").SourceArrayClassification == false);
	CHECK(input("pc.quarternion_from_euler", "euler_rotation").SourceKind == "Vec3");
	CHECK(input("pc.quarternion_from_euler", "euler_rotation").SourceArrayClassification == true);
	CHECK(input("pc.3_d_affector", "rotation").Type == ValueType::Quaternion);
	CHECK(input("pc.3_d_affector", "rotation").SourceArrayClassification == true);
	CHECK(input("pc.number_simple", "value").SourceArrayClassification == false);
	// Source postprocessing switches these declarations between Integer and Float, both classification0.
	CHECK(input("pc.vector3", "x").SourceArrayClassification == false);
	CHECK(input("pc.vector3", "y").SourceArrayClassification == false);
	// Literal source setDisplay overrides the constructor's classification.
	CHECK(input("pc.pb_fx_highlight", "width").SourceKind == "Int");
	CHECK(input("pc.pb_fx_highlight", "width").SourceArrayClassification == true);
	CHECK(input("pc.random_shape", "amount").SourceKind == "Float");
	CHECK(input("pc.random_shape", "amount").SourceArrayClassification == true);
	// The source also puts setDisplay inside the value argument to newInput.
	CHECK(input("pc.widget_test", "path_array_box").SourceArrayClassification == true);
	CHECK(input("pc.widget_test", "path_load").SourceArrayClassification == false);
	// Host projection controls and dynamic source type declarations are not inferred from payload type.
	CHECK_FALSE(input("pc.random_shape", "dimension_unit").SourceArrayClassification.has_value());
	const auto *array = FindCatalogueEntry("pc.array");
	REQUIRE(array);
	REQUIRE_FALSE(array->DynamicTemplate.empty());
	CHECK_FALSE(array->DynamicTemplate.front().SourceArrayClassification.has_value());
	CatalogueInput unresolved;
	CHECK_FALSE(unresolved.SourceArrayClassification.has_value());
}
