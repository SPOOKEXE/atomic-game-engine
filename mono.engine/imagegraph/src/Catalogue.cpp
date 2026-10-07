#include "NativeCatalogue.hpp"
#include "NodeExecutors.hpp"
#include "ValueText.hpp"

#include <engine/imagegraph/Catalogue.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::imagegraph {
	namespace {
		// A char array keeps the generated text out of constant evaluation.
		const char CATALOGUE_TEXT[] =
#include "SourceCatalogue.inc"
			;
		const std::string_view CATALOGUE_SOURCE(CATALOGUE_TEXT, sizeof(CATALOGUE_TEXT) - 1);

		struct CatalogueParsed {
			std::vector<CatalogueInput> Inputs;
			std::vector<CatalogueInput> Template;
			int32_t FixedLength = 0;
			int32_t GroupLength = 0;
			int32_t GroupLimit = 0;
			std::map<std::string_view, std::vector<CatalogueSourceChoice>> InputChoices, TemplateChoices;
			std::vector<CatalogueOutput> Outputs;
			std::vector<PortSchema> Ports;
			std::vector<PropertySchema> Properties;
		};

		struct CatalogueStorage {
			std::deque<std::string> ChoiceLabels;
			std::deque<std::string> ConstructorExpressions;
			std::vector<CatalogueParsed> Nodes;
			std::vector<CatalogueEntry> Entries;
			std::unordered_map<std::string_view, size_t> ByType;
			std::unordered_map<std::string_view, size_t> BySource;
		};

		std::vector<std::string_view> SplitRecord(std::string_view line) {
			std::vector<std::string_view> fields;
			size_t start = 0;
			for (;;) {
				const size_t tab = line.find('\t', start);
				fields.push_back(line.substr(start, tab == std::string_view::npos ? tab : tab - start));
				if (tab == std::string_view::npos) return fields;
				start = tab + 1;
			}
		}

		int32_t RecordIndex(std::string_view text) {
			int32_t value = -1;
			std::from_chars(text.data(), text.data() + text.size(), value);
			return value;
		}

		// The generated table is checked by its tests; a malformed record is a build defect, so parsing
		// skips it rather than carrying a runtime error path.
		CatalogueStorage BuildCatalogue() {
			CatalogueStorage storage;
			std::vector<CatalogueEntry> heads;
			size_t start = 0;
			while (start < CATALOGUE_SOURCE.size()) {
				size_t end = CATALOGUE_SOURCE.find('\n', start);
				if (end == std::string_view::npos) end = CATALOGUE_SOURCE.size();
				const std::string_view line = CATALOGUE_SOURCE.substr(start, end - start);
				start = end + 1;
				const std::vector<std::string_view> fields = SplitRecord(line);
				if (fields[0] == "N" && fields.size() == 6) {
					CatalogueEntry head;
					head.Type = fields[1];
					head.SourceNode = fields[2];
					head.Title = fields[3];
					head.Family = fields[4];
					head.SourceFile = fields[5];
					heads.push_back(head);
					storage.Nodes.emplace_back();
				} else if (fields[0] == "I" && fields.size() == 8 && !storage.Nodes.empty()) {
					const auto type = ParseValueTypeName(fields[5]);
					if (!type) continue;
					storage.Nodes.back().Inputs.push_back(
						{fields[1], fields[2], RecordIndex(fields[3]), fields[4], *type, fields[6], fields[7]}
					);
				} else if (fields[0] == "D" && (fields.size() == 3 || fields.size() == 4) &&
						   !storage.Nodes.empty()) {
					storage.Nodes.back().FixedLength = RecordIndex(fields[1]);
					storage.Nodes.back().GroupLength = RecordIndex(fields[2]);
					if (fields.size() == 4) storage.Nodes.back().GroupLimit = RecordIndex(fields[3]);
				} else if (fields[0] == "T" && fields.size() == 8 && !storage.Nodes.empty()) {
					const auto type = ParseValueTypeName(fields[5]);
					if (!type) continue;
					storage.Nodes.back().Template.push_back(
						{fields[1], fields[2], RecordIndex(fields[3]), fields[4], *type, fields[6], fields[7]}
					);
				} else if (fields[0] == "V" && fields.size() == 4 && !storage.Nodes.empty() &&
						   (fields[1] == "I" || fields[1] == "T")) {
					auto &inputs =
						fields[1] == "T" ? storage.Nodes.back().Template : storage.Nodes.back().Inputs;
					for (CatalogueInput &input : inputs)
						if (input.Id == fields[2]) input.SourceConstructorConstant = fields[3] == "1";
				} else if (fields[0] == "S" && fields.size() == 4 && !storage.Nodes.empty() &&
						   (fields[1] == "I" || fields[1] == "T")) {
					auto &inputs =
						fields[1] == "T" ? storage.Nodes.back().Template : storage.Nodes.back().Inputs;
					for (CatalogueInput &input : inputs) {
						if (input.Id != fields[2]) continue;
						if (fields[3] == "0")
							input.SourceArrayClassification = false;
						else if (fields[3] == "1")
							input.SourceArrayClassification = true;
						else
							input.SourceArrayClassification = std::nullopt;
					}
				} else if (fields[0] == "A" && fields.size() == 4 && !storage.Nodes.empty()) {
					auto &inputs =
						fields[1] == "T" ? storage.Nodes.back().Template : storage.Nodes.back().Inputs;
					for (CatalogueInput &input : inputs) {
						if (input.Id != fields[2]) continue;
						int32_t depth = -1;
						const auto [end, error] =
							std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), depth);
						input.ArrayDepthKnown =
							error == std::errc{} && end == fields[3].data() + fields[3].size();
						if (input.ArrayDepthKnown && depth >= 0 && depth <= 255)
							input.ArrayDepth = static_cast<uint8_t>(depth);
						else
							input.ArrayDepthKnown = false;
					}
				} else if ((fields[0] == "B" || fields[0] == "C" || fields[0] == "Q") && fields.size() >= 4 &&
						   !storage.Nodes.empty() && (fields[1] == "I" || fields[1] == "T")) {
					auto &node = storage.Nodes.back();
					auto &inputs = fields[1] == "T" ? node.Template : node.Inputs;
					auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
						return input.Id == fields[2];
					});
					if (found == inputs.end()) continue;
					if (fields[0] == "B" && fields.size() == 8) {
						auto flag = [](std::string_view text) -> std::optional<bool> {
							if (text == "0") return false;
							if (text == "1") return true;
							return std::nullopt;
						};
						CatalogueSourceBehavior behavior;
						behavior.StrictSuggestion = flag(fields[3]);
						behavior.ActualConnectability = fields[4] == "general"
															? SourceActualConnectability::General
															: SourceActualConnectability::Unknown;
						behavior.FractionalInterpolation = flag(fields[5]);
						behavior.ChoiceClamp = fields[6] == "always"	 ? SourceChoiceClamp::Always
											   : fields[6] == "default"	 ? SourceChoiceClamp::Default
											   : fields[6] == "disabled" ? SourceChoiceClamp::Disabled
																		 : SourceChoiceClamp::Unknown;
						found->SourceBehavior = behavior;
						if (!found->SourceChoices) found->SourceChoices.emplace();
						uint32_t count = 0;
						const auto [end, error] =
							std::from_chars(fields[7].data(), fields[7].data() + fields[7].size(), count);
						if (error == std::errc{} && end == fields[7].data() + fields[7].size())
							found->SourceChoices->RawCount = count;
					} else if (fields[0] == "C" && fields.size() == 4) {
						if (!found->SourceChoices) found->SourceChoices.emplace();
						found->SourceChoices->Status = fields[3] == "resolved" ? SourceChoicesStatus::Resolved
																			   : SourceChoicesStatus::Unknown;
					} else if (fields[0] == "Q" && fields.size() == 6 && found->SourceChoices &&
							   found->SourceChoices->Status == SourceChoicesStatus::Resolved) {
						int32_t index = -1;
						const auto [end, error] =
							std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), index);
						Value label;
						if (error != std::errc{} || end != fields[3].data() + fields[3].size() || index < 0 ||
							(fields[4] != "0" && fields[4] != "1") ||
							!detail::ReadValueText(fields[5], label) ||
							!std::holds_alternative<std::string>(label))
							continue;
						storage.ChoiceLabels.push_back(std::move(std::get<std::string>(label)));
						auto &choices = fields[1] == "T" ? node.TemplateChoices : node.InputChoices;
						choices[found->Id].push_back({index, storage.ChoiceLabels.back(), fields[4] == "1"});
					}
				} else if (fields[0] == "O" && fields.size() == 7 && !storage.Nodes.empty()) {
					const auto type = ParseValueTypeName(fields[4]);
					if (!type) continue;
					Value expression;
					if (!detail::ReadValueText(fields[6], expression) ||
						!std::holds_alternative<std::string>(expression))
						continue;
					storage.ConstructorExpressions.push_back(std::move(std::get<std::string>(expression)));
					storage.Nodes.back().Outputs.push_back(
						{fields[1],
						 fields[2],
						 RecordIndex(fields[3]),
						 *type,
						 fields[5],
						 storage.ConstructorExpressions.back()}
					);
				}
			}
			// Spans are taken only after every vector has reached its final size.
			for (size_t index = 0; index < storage.Nodes.size(); index++) {
				CatalogueParsed &node = storage.Nodes[index];
				for (auto &input : node.Inputs)
					if (input.SourceChoices) input.SourceChoices->Entries = node.InputChoices[input.Id];
				for (auto &input : node.Template)
					if (input.SourceChoices) input.SourceChoices->Entries = node.TemplateChoices[input.Id];
				// Audio Window stores this unit as an input attribute, rather than a separate newInput.
				if (heads[index].Type == "pc.audio_window")
					node.Inputs.push_back(
						{"location_unit",
						 "Location Unit",
						 -1,
						 "EScroll",
						 ValueType::Enum,
						 "e 0",
						 "Bit;Second;Progress"}
					);
				// A native projection of the endpoint pair stored in source Radius slot 2.
				if (heads[index].Type == "pc.kuwahara")
					node.Inputs.push_back(
						{"radius_map_range",
						 "Radius Map Range",
						 -1,
						 "MapRange",
						 ValueType::Vector2,
						 "v 0 2",
						 ""}
					);
				// Radius slot 2 owns the native endpoint projection.
				if (heads[index].Type == "pc.blobify")
					node.Inputs.push_back(
						{"radius_map_range",
						 "Radius Map Range",
						 -1,
						 "MapRange",
						 ValueType::Vector2,
						 "v 0 3",
						 ""}
					);
				// A native projection of the endpoint pair stored in source Width slot 1.
				if (heads[index].Type == "pc.erode")
					node.Inputs.push_back(
						{"width_map_range",
						 "Width Map Range",
						 -1,
						 "MapRange",
						 ValueType::Vector2,
						 "v 0 1",
						 ""}
					);
				if (heads[index].Type == "pc.solid") {
					for (CatalogueInput &input : node.Inputs)
						if (input.Id == "dimension_unit") input.Choices = "Pixel;Project";
				}
				if (heads[index].Type == "pc.group_input") {
					node.Inputs.push_back(
						{"parent_value", "Parent Value", -1, "Generic_any", ValueType::Any, "d -1", ""}
					);
				}
				if (heads[index].Type == "pc.group_output")
					node.Outputs.push_back({"value", "Value", 0, ValueType::Any});
				// Argument Type changes only source junction metadata; String mode retains raw values.
				if (heads[index].Type == "pc.argument") {
					for (CatalogueInput &input : node.Inputs)
						if (input.Id == "default_value") input.Type = ValueType::Any;
					for (CatalogueOutput &output : node.Outputs)
						if (output.Id == "value") output.Type = ValueType::Any;
				}
				// Source Text emits a complete per-letter Atlas array.
				if (heads[index].Type == "pc.text")
					for (CatalogueOutput &output : node.Outputs)
						if (output.Id == "draw_data") output.Type = ValueType::Array;
				// Only source Font getters accept the owned bitmap font carrier.
				if (heads[index].Type == "pc.font_bitmap")
					for (CatalogueOutput &output : node.Outputs)
						if (output.Id == "font") output.Type = ValueType::Font;
				// These source colour junctions emit palettes despite declaring VALUE_TYPE.color.
				if (heads[index].Type == "pc.gradient_extract" || heads[index].Type == "pc.gradient_sample")
					for (CatalogueOutput &output : node.Outputs)
						if (output.Id == "colors") output.Type = ValueType::Array;
				// Atlas Get writes one tuple or scalar per processor row before array aggregation.
				if (heads[index].Type == "pc.atlas_get")
					for (CatalogueOutput &output : node.Outputs) {
						if (output.Id == "position" || output.Id == "scale")
							output.Type = ValueType::Vector2;
						else if (output.Id == "rotation" || output.Id == "alpha")
							output.Type = ValueType::Scalar;
					}
				if (heads[index].Family == "generate" &&
					(heads[index].Type.find("noise") != std::string_view::npos ||
					 heads[index].Type == "pc.perlin" || heads[index].Type == "pc.perlin_extra" ||
					 heads[index].Type == "pc.cellular" || heads[index].Type == "pc.voronoi_extra") &&
					std::any_of(node.Outputs.begin(), node.Outputs.end(), [](const auto &output) {
						return output.Type == ValueType::Image;
					}))
					node.Outputs.push_back({"field", "2D Field", -1, ValueType::Noise2D});
				for (const CatalogueInput &input : node.Inputs) {
					node.Ports.push_back({input.Id, input.Type, PortDirection::Input});
					if (IsAuthoredValueType(input.Type) ||
						(heads[index].Type == "pc.group_input" && input.Id == "parent_value") ||
						(heads[index].Type == "pc.argument" && input.Id == "default_value"))
						node.Properties.push_back({input.Id, input.Type});
				}
				if (std::any_of(node.Outputs.begin(), node.Outputs.end(), [](const auto &output) {
						return output.Id == "field" && output.Type == ValueType::Noise2D;
					}))
					node.Properties.push_back({"output_type", ValueType::Enum});
				for (const CatalogueOutput &output : node.Outputs)
					node.Ports.push_back({output.Id, output.Type, PortDirection::Output});
			}
			for (size_t index = 0; index < storage.Nodes.size(); index++) {
				const CatalogueParsed &node = storage.Nodes[index];
				CatalogueEntry entry = heads[index];
				entry.Inputs = node.Inputs;
				entry.Outputs = node.Outputs;
				entry.DynamicFixedLength = node.FixedLength;
				entry.DynamicGroupLength = node.GroupLength;
				entry.DynamicGroupLimit = node.GroupLimit;
				entry.DynamicTemplate = node.Template;
				entry.Schema = NodeSchema{
					entry.Type,
					node.Ports,
					node.Properties,
					node.GroupLength > 0,
					entry.Type == "pc.array_split"
				};
				storage.ByType.emplace(entry.Type, storage.Entries.size());
				storage.BySource.emplace(entry.SourceNode, storage.Entries.size());
				storage.Entries.push_back(entry);
			}
			return storage;
		}

		const CatalogueStorage &CatalogueData() {
			static const CatalogueStorage storage = BuildCatalogue();
			return storage;
		}
	}

	std::span<const CatalogueEntry> Catalogue() {
		return CatalogueData().Entries;
	}

	const CatalogueEntry *FindCatalogueEntry(std::string_view type) {
		if (const auto *native = detail::FindNativeCatalogueEntry(type)) return native;
		if (!type.starts_with("pc.")) return nullptr;
		const CatalogueStorage &storage = CatalogueData();
		const auto found = storage.ByType.find(type);
		return found == storage.ByType.end() ? nullptr : &storage.Entries[found->second];
	}

	const CatalogueEntry *FindCatalogueSource(std::string_view sourceNode) {
		const CatalogueStorage &storage = CatalogueData();
		const auto found = storage.BySource.find(sourceNode);
		return found == storage.BySource.end() ? nullptr : &storage.Entries[found->second];
	}

	const CatalogueInput *FindCatalogueInput(const CatalogueEntry &entry, std::string_view id) {
		const auto found =
			std::find_if(entry.Inputs.begin(), entry.Inputs.end(), [&](const CatalogueInput &input) {
				return input.Id == id;
			});
		return found == entry.Inputs.end() ? nullptr : &*found;
	}

	const CatalogueInput *FindCatalogueInputIndex(const CatalogueEntry &entry, int32_t sourceIndex) {
		if (sourceIndex < 0) return nullptr;
		const auto found =
			std::find_if(entry.Inputs.begin(), entry.Inputs.end(), [&](const CatalogueInput &input) {
				return input.SourceIndex == sourceIndex;
			});
		return found == entry.Inputs.end() ? nullptr : &*found;
	}

	const CatalogueInput *
	FindDynamicTemplate(const CatalogueEntry &entry, std::string_view id, size_t &group) {
		const size_t separator = id.rfind('_');
		if (separator == std::string_view::npos || separator + 1 == id.size()) return nullptr;
		size_t parsed = 0;
		const std::string_view digits = id.substr(separator + 1);
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
		if (error != std::errc{} || end != digits.data() + digits.size()) return nullptr;
		const std::string_view base = id.substr(0, separator);
		for (const CatalogueInput &input : entry.DynamicTemplate) {
			if (input.Id == base) {
				group = parsed;
				return &input;
			}
		}
		return nullptr;
	}

	bool CatalogueSourceRawValue(const CatalogueInput &input, const Value &value) {
		const auto *raw = std::get_if<double>(&value);
		return raw && std::isfinite(*raw) && input.SourceIndex >= 0 &&
			   ((input.Type == ValueType::Integer &&
				 (input.SourceKind == "Int" || input.SourceKind == "ISlider")) ||
				(input.Type == ValueType::Boolean &&
				 (input.SourceKind == "Bool" || input.SourceKind == "Active")));
	}

	bool CatalogueSourceEnumValue(const CatalogueInput &input, const Value &value) {
		if (input.Type != ValueType::Enum || !input.SourceBehavior) return false;
		if (std::holds_alternative<EnumValue>(value) || std::holds_alternative<int64_t>(value)) return true;
		const auto *number = std::get_if<double>(&value);
		return number && std::isfinite(*number) && input.SourceBehavior->FractionalInterpolation == true;
	}

	bool CatalogueAuthoredArray(
		const CatalogueEntry &entry, const CatalogueInput &input, const ArrayValue &array
	) {
		if (entry.Type == "pc.argument" && input.Id == "default_value" && input.SourceIndex == 2 &&
			input.SourceKind == "Text" && input.Type == ValueType::Any)
			return detail::ValidPayload(array, false);
		// The source WAV sink consumes [channel][sample] itself and starts with [[]].
		if (entry.Type == "pc.wav_file_write" && input.Id == "audio_data" && input.SourceIndex == 1 &&
			input.SourceKind == "Float" && input.Type == ValueType::Scalar && input.ArrayDepthKnown &&
			input.ArrayDepth == 1)
			return (array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer) &&
				   array.Elements.empty() && array.Items.empty() && !array.Nested.empty() &&
				   detail::ValidPayload(array, true);
		// This Node-derived update consumes the full list of coordinate rows itself.
		if (entry.Type == "pc.points_remap" && input.Id == "points" && input.SourceIndex == 0 &&
			input.SourceKind == "Vec2" && input.Type == ValueType::Vector2 && input.ArrayDepthKnown &&
			input.ArrayDepth == 2)
			return (array.ElementType == ValueType::Vector2 || array.ElementType == ValueType::Scalar ||
					array.ElementType == ValueType::Integer || array.ElementType == ValueType::Any) &&
				   detail::ValidPayload(array, true);
		// The source fixed-length Vec3 getter pads or truncates these numeric component arrays.
		if (entry.Type == "pc.gradient_cube" && input.SourceKind == "Vec3" &&
			input.Type == ValueType::Vector3 && input.ArrayDepthKnown && input.ArrayDepth == 1 &&
			((input.Id == "rotation" && input.SourceIndex == 1) ||
			 (input.Id == "rotation_2" && input.SourceIndex == 8) ||
			 (input.Id == "scale_2" && input.SourceIndex == 9)))
			return (array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer) &&
				   array.Nested.empty() && array.Items.empty() && detail::ValidPayload(array, false);
		if ((entry.Type == "pc.perlin_cube" || entry.Type == "pc.cellular_cube" ||
			 entry.Type == "pc.simplex_cube") &&
			input.SourceKind == "Vec3" && input.Type == ValueType::Vector3 && input.ArrayDepthKnown &&
			input.ArrayDepth == 1 &&
			((input.Id == "rotation" && input.SourceIndex == 2) ||
			 (input.Id == "rotation_2" && input.SourceIndex == 8) ||
			 (input.Id == "scale_2" && input.SourceIndex == 9) ||
			 (input.Id == "position" && input.SourceIndex == 12)))
			return (array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer) &&
				   array.Nested.empty() && array.Items.empty() && detail::ValidPayload(array, false);
		// These compound controls are complete source values selected by the processor row.
		if (entry.Type == "pc.gradient" &&
			((input.Type == ValueType::Curve &&
			  (input.Id == "progress_remap" || input.Id == "inverse_curve" || input.Id == "curve")) ||
			 (input.Type == ValueType::Gradient && input.Id == "gradient")))
			return array.ElementType == input.Type && array.Nested.empty() && array.Items.empty() &&
				   detail::ValidPayload(array, false);
		// These fields project the mapped source numeric slot, whose endpoint depth is one.
		const bool mappedRange =
			input.SourceKind == "MapRange" && input.Type == ValueType::Vector2 &&
			((entry.Type == "pc.3_d_material" &&
			  (input.Id == "metalic_map_range" || input.Id == "roughness_map_range")) ||
			 (entry.Type == "pc.bevel" && input.Id == "height_map_range") ||
			 (entry.Type == "pc.erode" && input.Id == "width_map_range") ||
			 (entry.Type == "pc.gradient" &&
			  (input.Id == "angle_map_range" || input.Id == "radius_map_range" ||
			   input.Id == "shift_map_range" || input.Id == "scale_map_range")) ||
			 (entry.Type == "pc.noise_simplex" &&
			  (input.Id == "iteration_map_range" || input.Id == "scale_map_range"))) &&
			std::any_of(
				entry.Schema.Properties.begin(), entry.Schema.Properties.end(), [&](const auto &property) {
					return property.Id == input.Id && property.Type == ValueType::Vector2;
				}
			);
		if (mappedRange) {
			if (!array.Nested.empty() || !detail::ValidPayload(array, false)) return false;
			return array.ElementType == ValueType::Vector2 ||
				   ((array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer) &&
					array.Elements.size() == 2);
		}
		const bool directNumber =
			entry.Type == "pc.number" && input.Id == "value" && input.SourceIndex == 0 &&
			input.SourceKind == "Float" && input.Type == ValueType::Scalar &&
			std::any_of(
				entry.Schema.Properties.begin(), entry.Schema.Properties.end(), [](const auto &property) {
					return property.Id == "value" && property.Type == ValueType::Scalar;
				}
			);
		const bool manualNumeric =
			input.SourceKind == "Float" && input.Type == ValueType::Scalar && input.ArrayDepth == 0 &&
			((entry.Type == "pc.number_simple" && input.Id == "value" && input.SourceIndex == 0) ||
			 ((entry.Type == "pc.compare" || entry.Type == "pc.math" || entry.Type == "pc.vector_math") &&
			  ((input.Id == "a" && input.SourceIndex == 1) || (input.Id == "b" && input.SourceIndex == 2))) ||
			 (entry.Type == "pc.math" && input.Id == "amount" && input.SourceIndex == 5)) &&
			std::any_of(
				entry.Schema.Properties.begin(), entry.Schema.Properties.end(), [&](const auto &property) {
					return property.Id == input.Id && property.Type == ValueType::Scalar;
				}
			);
		// These Node-derived updates loop arrays directly rather than inheriting Node_Processor.
		const bool coordinates = (entry.Type == "pc.matrix_get" || entry.Type == "pc.matrix_set") &&
								 input.Id == "position" && input.SourceIndex == 1 &&
								 input.SourceKind == "IVec2" && input.Type == ValueType::Vector2 &&
								 input.ArrayDepth == 1;
		const bool matrixValues = entry.Type == "pc.matrix_set" && input.Id == "value" &&
								  input.SourceIndex == 2 && input.SourceKind == "Float" &&
								  input.Type == ValueType::Scalar && input.ArrayDepth == 0;
		const bool vectorPositions =
			(entry.Type == "pc.matrix_get_vector" || entry.Type == "pc.matrix_set_vector") &&
			input.Id == "position" && input.SourceIndex == 2 && input.SourceKind == "Int" &&
			input.Type == ValueType::Integer && input.ArrayDepth == 0;
		const bool directMatrix =
			(coordinates || matrixValues || vectorPositions) &&
			std::any_of(
				entry.Schema.Properties.begin(), entry.Schema.Properties.end(), [&](const auto &property) {
					return property.Id == input.Id && property.Type == input.Type;
				}
			);
		if (input.SourceIndex < 0 || !input.ArrayDepthKnown ||
			(!directNumber && !directMatrix && !manualNumeric &&
			 !FindCatalogueInput(entry, "attribute_process")) ||
			!array.Nested.empty() || array.ElementType > ValueType::Vector2 ||
			!detail::ValidPayload(array, false))
			return false;
		if (manualNumeric)
			return array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer;
		if (directMatrix) {
			if (coordinates)
				return array.ElementType == ValueType::Vector2 ||
					   ((array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer) &&
						array.Elements.size() == 2);
			if (matrixValues)
				return array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer;
			return array.ElementType == ValueType::Integer;
		}
		if (input.Type == ValueType::Enum && input.SourceBehavior &&
			input.SourceBehavior->FractionalInterpolation == true &&
			(array.ElementType == ValueType::Scalar || array.ElementType == ValueType::Integer))
			return true;
		if (array.ElementType == input.Type) return true;
		// Vec2 is a source array of two numeric components, before outer processor batching.
		return input.Type == ValueType::Vector2 && input.ArrayDepth == 1 &&
			   array.ElementType == ValueType::Scalar && array.Elements.size() == 2;
	}

	std::optional<Value> CatalogueDefault(const CatalogueInput &input) {
		if (input.Default.empty()) return std::nullopt;
		Value value;
		if (!detail::ReadValueText(input.Default, value)) return std::nullopt;
		return value;
	}

	size_t CatalogueChoiceCount(const CatalogueInput &input) {
		if (input.Choices.empty()) return 0;
		return static_cast<size_t>(std::count(input.Choices.begin(), input.Choices.end(), ';')) + 1;
	}

	bool HasNativeExecutor(std::string_view type) {
		return detail::FindExecutor(type) != nullptr;
	}
}
