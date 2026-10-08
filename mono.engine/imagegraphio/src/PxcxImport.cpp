#include "CookedShaderAnnotation.hpp"
#include "GroupInstances.hpp"
#include "HlslSourceArguments.hpp"
#include "ImageCacheAnnotation.hpp"
#include "InlineCollections.hpp"
#include "OrdinarySourceGroups.hpp"
#include "PxcxKeyProvenance.hpp"
#include "PxcxNativePorts.hpp"
#include "SourceCommonAdmission.hpp"
#include "SourceNoiseFieldAnnotation.hpp"
#include "TileProperties.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/SourceFrameCacheLoading.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace engine::imagegraphio {
	std::optional<PxcxReferencePreview> PxcxImport::ReferencePreview() const {
		if (Source.ThumbnailRgba.size() != bake::PxcxLimits::ThumbnailRgbaBytes) return std::nullopt;
		uint64_t hash = 14695981039346656037ull;
		for (uint8_t byte : Source.ThumbnailRgba) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		return PxcxReferencePreview{256, 256, Source.ThumbnailRgba, hash};
	}

	namespace {
		using Json = nlohmann::json;
		using imagegraph::ArrayValue;
		using imagegraph::Colour;
		using imagegraph::ElementValue;
		using imagegraph::Node;
		using imagegraph::Vector2;
		constexpr uint32_t SUPPORTED_VERSION = 121092;
		constexpr std::array<int64_t, 7> BLEND_MODES = {0, 1, 3, 8, 9, 11, 22};

		bool Fail(std::string &failure, std::string reason) {
			failure = std::move(reason);
			return false;
		}

		std::string Opaque(std::string_view type) {
			return "pxcx.opaque/" + std::string(type);
		}
		std::string InputPort(uint32_t index) {
			return "input-" + std::to_string(index);
		}
		std::string OutputPort(uint32_t index) {
			return "output-" + std::to_string(index);
		}

		const Json *Input(const Json &node, size_t index) {
			const auto inputs = node.find("inputs");
			if (inputs == node.end() || !inputs->is_array() || index >= inputs->size()) return nullptr;
			const Json &input = (*inputs)[index];
			return input.is_object() ? &input : nullptr;
		}

		// The pinned source's valueAnimator serializes an unanimated value as {d: value}.
		const Json *Current(const Json &node, size_t index) {
			const Json *input = Input(node, index);
			if (!input || input->contains("from_node") || input->contains("anim") ||
				input->contains("global_key") || input->contains("global_use"))
				return nullptr;
			const auto record = input->find("r");
			if (record == input->end() || !record->is_object()) return nullptr;
			const auto value = record->find("d");
			return value == record->end() ? nullptr : &*value;
		}

		bool Bool(const Json &source, size_t index, std::string_view port, Node &node) {
			const Json *value = Current(source, index);
			if (!value || !value->is_boolean()) return false;
			node.Values.push_back({std::string(port), value->get<bool>()});
			return true;
		}
		bool Int(const Json &source, size_t index, std::string_view port, Node &node) {
			const Json *value = Current(source, index);
			if (!value || !value->is_number_integer() ||
				(value->is_number_unsigned() && value->get<uint64_t>() > INT64_MAX))
				return false;
			node.Values.push_back({std::string(port), value->get<int64_t>()});
			return true;
		}
		bool Real(const Json &source, size_t index, std::string_view port, Node &node) {
			const Json *value = Current(source, index);
			if (!value || !value->is_number()) return false;
			const double number = value->get<double>();
			if (!std::isfinite(number)) return false;
			node.Values.push_back({std::string(port), number});
			return true;
		}
		bool Vec(const Json &source, size_t index, std::string_view port, Node &node) {
			const Json *value = Current(source, index);
			if (!value || !value->is_array() || value->size() != 2 || !(*value)[0].is_number() ||
				!(*value)[1].is_number())
				return false;
			const Vector2 vector{(*value)[0].get<double>(), (*value)[1].get<double>()};
			if (!std::isfinite(vector.X) || !std::isfinite(vector.Y)) return false;
			node.Values.push_back({std::string(port), vector});
			return true;
		}
		bool MaskAlpha(const Json &source, size_t index, Node &node) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto attributes = input->find("attri");
			if (attributes == input->end() || !attributes->is_object()) return false;
			const auto flag = attributes->find("mask_alpha_only");
			if (flag != attributes->end() && !flag->is_boolean()) return false;
			node.Values.push_back({"mask_alpha_only", flag == attributes->end() ? false : flag->get<bool>()});
			return true;
		}
		bool DefaultMaskAlpha(const Json &source, size_t index) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto attributes = input->find("attri");
			if (attributes == input->end() || !attributes->is_object()) return false;
			const auto flag = attributes->find("mask_alpha_only");
			return flag != attributes->end() && flag->is_boolean() && !flag->get<bool>();
		}
		bool Unmapped(const Json &source, size_t index) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto attributes = input->find("attri");
			if (attributes == input->end()) return true;
			if (!attributes->is_object()) return false;
			const auto mapped = attributes->find("mapped");
			return mapped == attributes->end() || (mapped->is_boolean() && !mapped->get<bool>());
		}
		bool Mapped(const Json &source, size_t index) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto attributes = input->find("attri");
			if (attributes == input->end() || !attributes->is_object()) return false;
			const auto mapped = attributes->find("mapped");
			return mapped != attributes->end() && mapped->is_boolean() && mapped->get<bool>();
		}
		bool ReferenceUnit(const Json &source, size_t index) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto unit = input->find("unit");
			return unit != input->end() && unit->is_number_integer() && unit->get<int64_t>() == 1;
		}
		bool Active(const Json &source, size_t index) {
			const Json *value = Current(source, index);
			return value && value->is_boolean() && value->get<bool>();
		}
		bool DefaultNumber(const Json &source, size_t index, double expected) {
			const Json *value = Current(source, index);
			return value && value->is_number() && value->get<double>() == expected;
		}
		bool DefaultBool(const Json &source, size_t index, bool expected) {
			const Json *value = Current(source, index);
			return value && value->is_boolean() && value->get<bool>() == expected;
		}
		bool DefaultVector(const Json &source, size_t index, std::initializer_list<double> expected) {
			const Json *value = Current(source, index);
			if (!value || !value->is_array() || value->size() != expected.size()) return false;
			size_t at = 0;
			for (double number : expected) {
				if (!(*value)[at].is_number() || (*value)[at].get<double>() != number) return false;
				at++;
			}
			return true;
		}
		bool DefaultCurve(const Json &source, size_t index, bool flat) {
			return flat ? DefaultVector(
							  source,
							  index,
							  {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1.0 / 3.0, 0, -1.0 / 3.0, 0, 1, 0, 0, 0}
						  )
						: DefaultVector(
							  source,
							  index,
							  {0,
							   1,
							   0,
							   0,
							   1,
							   0,
							   0,
							   0,
							   0,
							   0,
							   1.0 / 3.0,
							   1.0 / 3.0,
							   -1.0 / 3.0,
							   -1.0 / 3.0,
							   1,
							   1,
							   0,
							   0}
						  );
		}
		bool DefaultThresholdCurve(const Json &source, size_t index) {
			return DefaultVector(
				source, index, {0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1.0 / 3.0, 0, -1.0 / 3.0, 0, 1, 1, 0, 0}
			);
		}
		bool PackedColour(const Json &value, Colour &color) {
			if (!value.is_number()) return false;
			const double packedDouble = value.get<double>();
			if (!std::isfinite(packedDouble) || packedDouble < 0 || packedDouble > UINT32_MAX ||
				std::trunc(packedDouble) != packedDouble)
				return false;
			const uint32_t packed = static_cast<uint32_t>(packedDouble);
			color = {
				static_cast<uint8_t>(packed),
				static_cast<uint8_t>(packed >> 8),
				static_cast<uint8_t>(packed >> 16),
				static_cast<uint8_t>(packed >> 24)
			};
			return true;
		}

		bool CurveValue(const Json &source, size_t index, std::string_view port, Node &node) {
			const Json *value = Current(source, index);
			if (!value || !value->is_array() || value->size() < 18 || (value->size() - 6) % 6 != 0 ||
				value->size() > 60)
				return false;
			imagegraph::Curve curve;
			for (size_t offset = 0; offset < value->size(); ++offset) {
				const Json &part = (*value)[offset];
				if (!part.is_number() || !std::isfinite(part.get<double>())) return false;
				if (offset < 6)
					curve.Header[offset] = part.get<double>();
				else {
					if ((offset - 6) % 6 == 0) curve.Anchors.emplace_back();
					curve.Anchors.back()[(offset - 6) % 6] = part.get<double>();
				}
			}
			if ((curve.Header[2] != 0 && curve.Header[2] != 1) || curve.Header[1] == 0) return false;
			for (size_t index = 1; index < curve.Anchors.size(); ++index)
				if (curve.Anchors[index][2] <= curve.Anchors[index - 1][2]) return false;
			node.Values.push_back({std::string(port), std::move(curve)});
			return true;
		}
		bool GradientMappedScalar(
			const Json &source,
			size_t valueIndex,
			size_t mapIndex,
			std::string_view minimumPort,
			std::string_view maximumPort,
			Node &node
		) {
			const Json *map = Input(source, mapIndex);
			if (!map || (!map->contains("from_node") && !DefaultNumber(source, mapIndex, -4))) return false;
			const Json *value = Current(source, valueIndex);
			if (!value) return false;
			if (Unmapped(source, valueIndex) && !map->contains("from_node"))
				return Real(source, valueIndex, minimumPort, node);
			if (!Mapped(source, valueIndex) || !value->is_array() || value->size() != 2 ||
				!(*value)[0].is_number() || !(*value)[1].is_number())
				return false;
			const double minimum = (*value)[0].get<double>();
			const double maximum = (*value)[1].get<double>();
			if (!std::isfinite(minimum) || !std::isfinite(maximum)) return false;
			node.Values.push_back({std::string(minimumPort), minimum});
			node.Values.push_back({std::string(maximumPort), maximum});
			return true;
		}

		bool Number(const Json &source, Node &node) {
			// The pinned Number update reads only Value and Integer for its output.
			if (Input(source, 20)) return false;
			for (size_t index = 0; index < 20; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index) || !Current(source, index)) return false;
			}
			const Json *value = Current(source, 0);
			const Json *integer = Current(source, 1);
			if (!value->is_number() || !integer->is_boolean()) return false;
			double number = value->get<double>();
			if (!std::isfinite(number)) return false;
			if (integer->get<bool>()) {
				// The public source does not define GameMaker's half-tie rounding here.
				if (std::abs(number - std::trunc(number)) == 0.5) return false;
				number = std::round(number);
			}
			node.Type = "value.number";
			node.Values.push_back({"value", number});
			return true;
		}

		bool MathScalar(const Json &source, Node &node) {
			const Json *operation = Current(source, 0);
			constexpr std::array<int64_t, 6> supported = {0, 1, 2, 3, 13, 18};
			if (Input(source, 9) || !operation || !operation->is_number_integer() ||
				std::find(supported.begin(), supported.end(), operation->get<int64_t>()) == supported.end() ||
				!DefaultNumber(source, 3, 1) || !DefaultBool(source, 4, false) ||
				!DefaultBool(source, 8, false))
				return false;
			const int64_t mode = operation->get<int64_t>();
			if (mode != 13 && !DefaultNumber(source, 5, 0)) return false;
			if (mode != 18 && (!DefaultVector(source, 6, {0, 1}) || !DefaultVector(source, 7, {0, 1})))
				return false;
			if (mode == 18) {
				const Json *from = Current(source, 6);
				if (!from || !from->is_array() || from->size() != 2 || !(*from)[0].is_number() ||
					!(*from)[1].is_number() || (*from)[0] == (*from)[1])
					return false;
			}
			for (size_t index = 0; index < 9; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index)) return false;
				if (index == 1 && input->contains("from_node")) continue;
				if (!Current(source, index)) return false;
			}
			node.Type = "value.math";
			if (!Input(source, 1)->contains("from_node") && !Real(source, 1, "a", node)) return false;
			node.Values.push_back({"degrees", true});
			if (!Real(source, 2, "b", node) || !Int(source, 0, "mode", node)) return false;
			if (mode == 13 && !Real(source, 5, "amount", node)) return false;
			if (mode == 18 && (!Vec(source, 6, "from", node) || !Vec(source, 7, "to", node))) return false;
			return true;
		}

		const Json *NodeById(const Json &root, std::string_view id) {
			const auto nodes = root.find("nodes");
			if (nodes == root.end() || !nodes->is_array()) return nullptr;
			for (const Json &node : *nodes) {
				if (node.is_object() && node.value("id", "") == id) return &node;
			}
			return nullptr;
		}

		bool LinkedNumber(const Json &root, const Json &math) {
			const Json *a = Input(math, 1);
			if (!a || !a->contains("from_node") || !a->at("from_node").is_string() ||
				a->value("from_index", -1) != 0)
				return false;
			const Json *number = NodeById(root, a->at("from_node").get_ref<const std::string &>());
			if (!number || number->value("type", "") != "Node_Number") return false;
			Node candidate;
			return Number(*number, candidate);
		}

		bool LinkedAddAngle(const Json &root, const Json &gradient) {
			const Json *angle = Input(gradient, 3);
			if (!angle || !angle->contains("from_node") || !angle->at("from_node").is_string() ||
				angle->value("from_index", -1) != 0 || !Unmapped(gradient, 3))
				return false;
			const Json *math = NodeById(root, angle->at("from_node").get_ref<const std::string &>());
			if (!math || math->value("type", "") != "Node_Math") return false;
			Node candidate;
			return DefaultNumber(*math, 0, 0) && MathScalar(*math, candidate) && LinkedNumber(root, *math);
		}

		bool Dimensions(const Json &root, const Json &source, Node &node, size_t index = 0) {
			const Json *input = Input(source, index);
			if (!input) return false;
			const auto attributes = input->find("attri");
			if (attributes == input->end() || !attributes->is_object()) return false;
			const auto mode = attributes->find("use_project_dimension");
			if (mode == attributes->end() || !mode->is_number_integer()) return false;
			const Json *dimension = nullptr;
			if (mode->get<int64_t>() == 1) {
				const auto project = root.find("attributes");
				if (project == root.end() || !project->is_object()) return false;
				const auto found = project->find("surface_dimension");
				if (found == project->end()) return false;
				dimension = &*found;
			} else if (mode->get<int64_t>() == 0) {
				dimension = Current(source, index);
			} else
				return false;
			if (!dimension || !dimension->is_array() || dimension->size() != 2) return false;
			std::array<int64_t, 2> sides{};
			for (size_t axis = 0; axis < 2; axis++) {
				const Json &value = (*dimension)[axis];
				if (!value.is_number()) return false;
				const double side = value.get<double>();
				if (!std::isfinite(side) || side < 1 || side > imagegraph::Limits::MaximumDimension ||
					std::trunc(side) != side)
					return false;
				sides[axis] = static_cast<int64_t>(side);
			}
			if (static_cast<uint64_t>(sides[0]) * static_cast<uint64_t>(sides[1]) * 4 >
				imagegraph::Limits::MaximumOutputBytes)
				return false;
			node.Values.push_back({"width", sides[0]});
			node.Values.push_back({"height", sides[1]});
			return true;
		}

		bool Solid(const Json &root, const Json &source, Node &node) {
			node.Type = "image.solid";
			if (!Dimensions(root, source, node)) return false;
			const Json *color = Current(source, 1);
			if (!color || !color->is_number_integer()) return false;
			const double packedDouble = color->get<double>();
			if (!std::isfinite(packedDouble) || packedDouble < 0 || packedDouble > UINT32_MAX ||
				std::trunc(packedDouble) != packedDouble)
				return false;
			const uint32_t packed = static_cast<uint32_t>(packedDouble);
			// Public color_function.gml stores R, G, B, A in ascending byte order.
			node.Values.push_back(
				{"colour",
				 Colour{
					 static_cast<uint8_t>(packed),
					 static_cast<uint8_t>(packed >> 8),
					 static_cast<uint8_t>(packed >> 16),
					 static_cast<uint8_t>(packed >> 24)
				 }}
			);
			return Bool(source, 2, "empty", node) && Bool(source, 4, "use_mask_dimension", node) &&
				   MaskAlpha(source, 3, node);
		}
		bool Gradient(const Json &root, const Json &source, Node &node) {
			const bool linkedAngle = Input(source, 3) && Input(source, 3)->contains("from_node");
			if (linkedAngle && !LinkedAddAngle(root, source)) return false;
			if (!DefaultNumber(source, 15, -4) || !DefaultVector(source, 16, {0, 0, 1, 0})) return false;
			const Json *uv = Input(source, 18);
			if (!uv || (!uv->contains("from_node") && !DefaultNumber(source, 18, -4))) return false;
			const Json *mask = Input(source, 8);
			if (!mask || (!mask->contains("from_node") && !DefaultNumber(source, 8, -4))) return false;
			node.Type = "image.gradient";
			if (!Dimensions(root, source, node)) return false;
			const Json *encoded = Current(source, 1);
			if (!encoded || !encoded->is_string() ||
				encoded->get_ref<const std::string &>().size() > imagegraph::Limits::MaximumTextBytes)
				return false;
			Json keys;
			try {
				keys = Json::parse(encoded->get_ref<const std::string &>());
			} catch (const Json::exception &) {
				return false;
			}
			if (!keys.is_object() || !keys.contains("type") || !keys["type"].is_number() ||
				!keys.contains("keys") || !keys["keys"].is_array() || keys["keys"].empty() ||
				keys["keys"].size() > imagegraph::Limits::MaximumGradientKeys)
				return false;
			const double mode = keys["type"].get<double>();
			if (!std::isfinite(mode) || mode < 0 || mode > 6 || std::trunc(mode) != mode) return false;
			imagegraph::Gradient gradient;
			gradient.Mode = static_cast<uint8_t>(mode);
			for (const Json &entry : keys["keys"]) {
				if (!entry.is_object() || !entry.contains("time") || !entry["time"].is_number() ||
					!entry.contains("value"))
					return false;
				const double time = entry["time"].get<double>();
				Colour color;
				if (!std::isfinite(time) || time < 0 || time > 1 ||
					(!gradient.Keys.empty() && time <= gradient.Keys.back().Time) ||
					!PackedColour(entry["value"], color))
					return false;
				gradient.Keys.push_back({time, color});
			}
			node.Values.push_back({"gradient", std::move(gradient)});
			if (!Int(source, 2, "type", node)) return false;
			if (linkedAngle) {
				if (!DefaultNumber(source, 10, -4)) return false;
				node.Values.push_back({"angle", 0.0});
			} else if (!GradientMappedScalar(source, 3, 10, "angle", "angle_max", node)) {
				return false;
			}
			if (!GradientMappedScalar(source, 4, 11, "radius", "radius_max", node) ||
				!Vec(source, 6, "center", node) || !Vec(source, 17, "shape", node) ||
				!Bool(source, 14, "uniform_ratio", node) ||
				!GradientMappedScalar(source, 5, 12, "shift", "shift_max", node) ||
				!GradientMappedScalar(source, 9, 13, "scale", "scale_max", node) ||
				!Int(source, 7, "loop", node) || !Real(source, 19, "uv_mix", node) ||
				!Real(source, 21, "inverse_axis", node) || !Vec(source, 23, "level_in", node) ||
				!Vec(source, 25, "level_out", node) || !CurveValue(source, 20, "progress_remap", node) ||
				!CurveValue(source, 22, "inverse_curve", node) || !CurveValue(source, 24, "curve", node))
				return false;
			return true;
		}
		bool Simplex(const Json &root, const Json &source, Node &node) {
			// The native evaluator currently implements unmasked scalar grayscale noise.
			if (!DefaultNumber(source, 4, 0) || !DefaultVector(source, 5, {0, 1}) ||
				!DefaultVector(source, 6, {0, 1}) || !DefaultVector(source, 7, {0, 1}) ||
				!DefaultNumber(source, 8, -4) || !DefaultNumber(source, 9, -4) ||
				!DefaultNumber(source, 13, -4) || !DefaultNumber(source, 15, -4) ||
				!DefaultNumber(source, 16, 1))
				return false;
			node.Type = "image.noise_simplex";
			if (!Dimensions(root, source, node)) return false;
			return Real(source, 14, "seed", node) && Int(source, 3, "iterations", node) &&
				   Bool(source, 17, "tile", node) && Vec(source, 1, "position", node) &&
				   Real(source, 10, "rotation", node) && Vec(source, 2, "scale", node) &&
				   Real(source, 11, "iteration_scaling", node) &&
				   Real(source, 12, "iteration_amplitude", node) && Vec(source, 18, "level_in", node) &&
				   Vec(source, 19, "level_out", node);
		}
		bool Checker(const Json &root, const Json &source, Node &node) {
			const auto inputs = source.find("inputs");
			const auto attributes = source.find("attri");
			if (inputs == source.end() || !inputs->is_array() || inputs->size() != 14 ||
				attributes == source.end() || !attributes->is_object())
				return false;
			const auto process = attributes->find("process");
			const auto depth = attributes->find("color_depth");
			if (process == attributes->end() || !process->is_boolean() || !process->get<bool>() ||
				depth == attributes->end() || !depth->is_number_integer() ||
				(depth->get<int64_t>() != 0 && depth->get<int64_t>() != 1))
				return false;
			for (size_t index = 0; index < 14; ++index) {
				const Json *input = Input(source, index);
				if (!input || input->contains("from_node") || !Current(source, index) ||
					!Unmapped(source, index))
					return false;
			}
			if (!ReferenceUnit(source, 1) || !ReferenceUnit(source, 3) || !DefaultNumber(source, 6, -4) ||
				!DefaultNumber(source, 7, -4) || !DefaultNumber(source, 8, 0) ||
				!DefaultNumber(source, 10, -4) || !DefaultNumber(source, 11, -4) ||
				!DefaultNumber(source, 12, 1))
				return false;
			const Json *first = Current(source, 4);
			const Json *second = Current(source, 5);
			Colour firstColour, secondColour;
			if (!first || !second || !PackedColour(*first, firstColour) ||
				!PackedColour(*second, secondColour))
				return false;
			node.Type = "image.checker";
			if (!Dimensions(root, source, node)) return false;
			node.Values.push_back({"color_1", firstColour});
			node.Values.push_back({"color_2", secondColour});
			return Real(source, 1, "size", node) && Real(source, 13, "aspect", node) &&
				   Real(source, 2, "angle", node) && Vec(source, 3, "position", node) &&
				   Bool(source, 9, "diagonal", node);
		}
		bool Offset(const Json &source, Node &node) {
			if (!Active(source, 3) || !DefaultNumber(source, 4, -4) || !DefaultNumber(source, 5, 1) ||
				!DefaultBool(source, 6, false) || !DefaultNumber(source, 7, 0))
				return false;
			node.Type = "image.offset";
			return Real(source, 1, "x_offset", node) && Real(source, 2, "y_offset", node) &&
				   Real(source, 8, "angle", node);
		}
		bool Polar(const Json &source, Node &node) {
			const auto polarBool = [&](size_t index, std::string_view port) {
				const Json *value = Current(source, index);
				if (!value) return false;
				if (value->is_boolean()) {
					node.Values.push_back({std::string(port), value->get<bool>()});
					return true;
				}
				if (!value->is_number_integer() ||
					(value->is_number_unsigned() && value->get<uint64_t>() > 1) ||
					(value->get<int64_t>() != 0 && value->get<int64_t>() != 1))
					return false;
				node.Values.push_back({std::string(port), value->get<int64_t>() == 1});
				return true;
			};
			const Json *image = Input(source, 0);
			const auto inputs = source.find("inputs");
			const auto attributes = source.find("attri");
			if (!image || !image->contains("from_node") || inputs == source.end() || !inputs->is_array() ||
				inputs->size() != 19 || attributes == source.end() || !attributes->is_object())
				return false;
			const auto interpolation = attributes->find("interpolate");
			const auto process = attributes->find("process");
			const auto oversample = attributes->find("oversample");
			const auto depth = attributes->find("color_depth");
			if (interpolation == attributes->end() || !interpolation->is_number_integer() ||
				(interpolation->get<int64_t>() != 1 && interpolation->get<int64_t>() != 2) ||
				process == attributes->end() || !process->is_boolean() || !process->get<bool>() ||
				oversample == attributes->end() || !oversample->is_number_integer() ||
				oversample->get<int64_t>() != 0 || depth == attributes->end() ||
				!depth->is_number_integer() || depth->get<int64_t>() != 0)
				return false;
			for (size_t index = 1; index < 19; ++index)
				if (!Current(source, index) || !Unmapped(source, index)) return false;
			if (!DefaultNumber(source, 1, -4) || !DefaultNumber(source, 2, 1) || !Active(source, 3) ||
				!DefaultNumber(source, 4, 15) || !DefaultBool(source, 7, false) ||
				!DefaultNumber(source, 8, 0) || !DefaultNumber(source, 11, -4) ||
				!DefaultNumber(source, 15, -4) || !DefaultNumber(source, 17, -4) ||
				!ReferenceUnit(source, 18))
				return false;
			const Json *mode = Current(source, 9);
			const Json *range = Current(source, 13);
			if (!mode || !mode->is_number_integer() ||
				(mode->is_number_unsigned() && mode->get<uint64_t>() > 2) || mode->get<int64_t>() < 0 ||
				mode->get<int64_t>() > 2 || !range || !range->is_array() || range->size() != 2 ||
				!(*range)[0].is_number() || !(*range)[1].is_number() ||
				(*range)[0].get<double>() == (*range)[1].get<double>())
				return false;
			node.Type = "image.polar";
			node.Values.push_back({"interpolation", interpolation->get<int64_t>()});
			return Vec(source, 12, "tile", node) && Vec(source, 18, "center", node) &&
				   Real(source, 16, "angle", node) && polarBool(5, "invert") && polarBool(10, "swap_axis") &&
				   Real(source, 6, "blend", node) && Int(source, 9, "radius_mode", node) &&
				   Vec(source, 13, "range", node) && Real(source, 14, "twist", node);
		}
		bool Threshold(const Json &source, Node &node) {
			// The native simple threshold has no curve or mapped-control evaluator.
			if (!Active(source, 6) || !DefaultNumber(source, 4, -4) || !DefaultNumber(source, 5, 1) ||
				!DefaultBool(source, 11, false) || !DefaultNumber(source, 12, 0) ||
				!DefaultNumber(source, 13, -4) || !DefaultNumber(source, 14, -4) ||
				!DefaultThresholdCurve(source, 21) || !DefaultThresholdCurve(source, 22) ||
				!DefaultNumber(source, 15, 0))
				return false;
			node.Type = "image.threshold";
			return Int(source, 10, "channel", node) && Bool(source, 1, "brightness", node) &&
				   Real(source, 2, "brightness_threshold", node) &&
				   Real(source, 3, "brightness_smoothness", node) &&
				   Int(source, 16, "adaptive_radius", node) && Bool(source, 17, "brightness_invert", node) &&
				   Bool(source, 20, "brightness_multiply", node) && Int(source, 19, "apply_to_alpha", node) &&
				   Bool(source, 7, "alpha", node) && Real(source, 8, "alpha_threshold", node) &&
				   Real(source, 9, "alpha_smoothness", node) && Bool(source, 18, "alpha_invert", node);
		}
		bool Invert(const Json &source, Node &node) {
			node.Type = "image.invert";
			return Active(source, 3) && Bool(source, 7, "include_alpha", node) &&
				   Int(source, 4, "channel", node) && Real(source, 2, "mix", node) &&
				   Bool(source, 5, "invert_mask", node) && Real(source, 6, "mask_feather", node);
		}
		bool Blend(const Json &source, Node &node) {
			node.Type = "image.blend";
			const Json *mode = Current(source, 2);
			if (!mode || !mode->is_number_integer() ||
				std::find(BLEND_MODES.begin(), BLEND_MODES.end(), mode->get<int64_t>()) == BLEND_MODES.end())
				return false;
			return Active(source, 8) && Bool(source, 15, "swap", node) &&
				   Bool(source, 12, "invert_mask", node) && Real(source, 13, "mask_feather", node) &&
				   Int(source, 6, "output_dimension", node) && Vec(source, 7, "constant_dimension", node) &&
				   Int(source, 2, "blend_mode", node) && Real(source, 3, "opacity", node) &&
				   Bool(source, 9, "preserve_alpha", node) && Int(source, 5, "fill_mode", node) &&
				   Vec(source, 14, "position", node) && Int(source, 10, "horizontal_align", node) &&
				   Int(source, 11, "vertical_align", node) && MaskAlpha(source, 4, node);
		}
		bool Tile(const Json &root, const Json &source, Node &node) {
			// The native evaluator has no UV-map path. Input 2 is the foreign dimension control.
			if (!DefaultNumber(source, 9, -4) || !DefaultNumber(source, 10, 1)) return false;
			node.Type = "image.tile";
			if (!Dimensions(root, source, node, 2)) return false;
			return Int(source, 1, "scaling_type", node) && Vec(source, 3, "amount", node) &&
				   Vec(source, 5, "spacing", node) && Vec(source, 4, "position", node) &&
				   Real(source, 8, "rotation", node) && Vec(source, 12, "scale", node) &&
				   Int(source, 6, "shift_axis", node) && Real(source, 7, "shift", node) &&
				   Int(source, 11, "pattern", node);
		}
		bool Blur(const Json &source, Node &node) {
			// Only the default integer Gaussian kernel is implemented natively.
			const Json *size = Current(source, 1);
			if (!size || !size->is_number() || !Unmapped(source, 1) || !std::isfinite(size->get<double>()) ||
				size->get<double>() < 0 || size->get<double>() > 32 ||
				std::trunc(size->get<double>()) != size->get<double>() || !DefaultNumber(source, 2, 0) ||
				!Active(source, 7) || !DefaultNumber(source, 12, 1) || !DefaultNumber(source, 13, 0) ||
				!DefaultNumber(source, 5, -4) || !DefaultNumber(source, 6, 1) ||
				!DefaultBool(source, 9, false) || !DefaultNumber(source, 10, 0) ||
				!DefaultNumber(source, 14, -4) || !DefaultNumber(source, 15, 1) ||
				!DefaultNumber(source, 16, -4) || !DefaultThresholdCurve(source, 17))
				return false;
			if (!DefaultMaskAlpha(source, 5)) return false;
			node.Type = "image.blur";
			const Json *color = Current(source, 4);
			Colour packed;
			if (!color || !PackedColour(*color, packed)) return false;
			node.Values.push_back({"color", packed});
			return Real(source, 1, "size", node) && Int(source, 2, "intensity", node) &&
				   Bool(source, 3, "override_color", node) && Bool(source, 11, "gamma", node) &&
				   Real(source, 12, "aspect", node) && Real(source, 13, "direction", node) &&
				   Int(source, 8, "channel", node);
		}
		bool ColorAdjust(const Json &source, Node &node) {
			if (!DefaultNumber(source, 12, 0) || !Active(source, 11) || !DefaultBool(source, 16, false) ||
				!DefaultNumber(source, 17, 1))
				return false;
			for (size_t index : std::array<size_t, 8>{1, 2, 3, 4, 5, 7, 9, 10})
				if (!Unmapped(source, index)) return false;
			for (size_t index = 18; index <= 25; ++index)
				if (!DefaultNumber(source, index, -4)) return false;
			const Json *mask = Input(source, 8);
			if (!mask || (!mask->contains("from_node") && !DefaultNumber(source, 8, -4))) return false;
			if (!DefaultMaskAlpha(source, 8)) return false;
			const Json *color = Current(source, 6);
			Colour packed;
			if (!color || !PackedColour(*color, packed)) return false;
			node.Type = "image.color_adjust";
			node.Values.push_back({"blend", packed});
			return Int(source, 15, "channel", node) && Real(source, 9, "alpha", node) &&
				   Real(source, 1, "brightness", node) && Real(source, 2, "contrast", node) &&
				   Real(source, 10, "exposure", node) && Real(source, 3, "hue", node) &&
				   Real(source, 4, "saturation", node) && Real(source, 5, "value", node) &&
				   Int(source, 14, "blend_mode", node) && Real(source, 7, "blend_amount", node);
		}
		bool Posterize(const Json &source, Node &node) {
			// This subset uses the RGB palette path without hue bias or a reference surface.
			if (!DefaultBool(source, 5, true) || !DefaultNumber(source, 7, -4) ||
				!DefaultNumber(source, 8, 0) || !DefaultNumber(source, 10, 0) ||
				!DefaultNumber(source, 11, -4))
				return false;
			if (!Unmapped(source, 1) || !Bool(source, 2, "use_palette", node) ||
				!std::get<bool>(node.Values.back().Data))
				return false;
			const Json *paletteValue = Current(source, 1);
			if (!paletteValue || !paletteValue->is_array() || paletteValue->empty() ||
				paletteValue->size() > imagegraph::Limits::MaximumPaletteEntries)
				return false;
			ArrayValue palette{imagegraph::ValueType::Colour, {}};
			palette.Elements.reserve(paletteValue->size());
			for (const Json &value : *paletteValue) {
				Colour color;
				if (!PackedColour(value, color)) return false;
				palette.Elements.emplace_back(ElementValue{color});
			}
			node.Type = "image.posterize";
			node.Values.push_back({"palette", std::move(palette)});
			if (!Bool(source, 6, "posterize_alpha", node) || !Int(source, 3, "steps", node) ||
				!Real(source, 4, "gamma", node) || !Bool(source, 9, "use_global_range", node) ||
				!MaskAlpha(source, 12, node) || !Real(source, 13, "mix", node) ||
				!Bool(source, 14, "invert_mask", node) || !Real(source, 15, "mask_feather", node))
				return false;
			const Json *mask = Input(source, 12);
			return mask && (mask->contains("from_node") || DefaultNumber(source, 12, -4)) &&
				   Unmapped(source, 12);
		}
		bool Shape(const Json &root, const Json &source, Node &node) {
			// These four shapes use the native distance path only with static reference units.
			// The pinned source applies reference dimensions before dividing them back out.
			for (size_t index = 0; index < 53; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index) || input->contains("anim") ||
					input->contains("global_key") || input->contains("global_use"))
					return false;
				if ((index == 46 || index == 50) && input->contains("from_node")) continue;
				if (!Current(source, index)) return false;
			}
			if (Input(source, 53) || !DefaultNumber(source, 1, 2) || !DefaultNumber(source, 15, 1) ||
				!DefaultNumber(source, 9, 0) || !DefaultBool(source, 18, false) ||
				!DefaultNumber(source, 36, 0) || !DefaultBool(source, 49, true) ||
				!DefaultVector(source, 48, {0, 0, 0, 0}) || !DefaultNumber(source, 41, 0) ||
				!DefaultVector(source, 42, {0, 0}) || !DefaultNumber(source, 44, -4) ||
				!DefaultNumber(source, 45, 1) || !DefaultNumber(source, 47, 0) ||
				!DefaultCurve(source, 29, false) || !DefaultMaskAlpha(source, 50))
				return false;
			for (size_t index : std::array<size_t, 6>{16, 17, 32, 33, 35, 40})
				if (!ReferenceUnit(source, index)) return false;
			const Json *background = Input(source, 46);
			const Json *mask = Input(source, 50);
			if (!background || (!background->contains("from_node") && !DefaultNumber(source, 46, -4)) ||
				!mask || (!mask->contains("from_node") && !DefaultNumber(source, 50, -4)))
				return false;
			const Json *kind = Current(source, 2);
			if (!kind || !kind->is_string()) return false;
			const std::string &name = kind->get_ref<const std::string &>();
			if (name != "Rectangle" && name != "Ellipse" && name != "Half" && name != "Triangle")
				return false;
			const Json *scale = Current(source, 28);
			if (!scale || !scale->is_number() || !std::isfinite(scale->get<double>()) ||
				scale->get<double>() <= 0)
				return false;
			const Json *level = Current(source, 20);
			if (!level || !level->is_array() || level->size() != 2 || !(*level)[0].is_number() ||
				!(*level)[1].is_number() || (*level)[0] == (*level)[1])
				return false;
			Colour color, backgroundColor;
			const Json *shapeColor = Current(source, 10);
			const Json *bgColor = Current(source, 11);
			if (!shapeColor || !PackedColour(*shapeColor, color) || !bgColor ||
				!PackedColour(*bgColor, backgroundColor))
				return false;
			node.Type = "image.shape";
			if (!Dimensions(root, source, node)) return false;
			node.Values.push_back({"shape", name});
			node.Values.push_back({"color", color});
			node.Values.push_back({"bg_color", backgroundColor});
			if (!Int(source, 15, "position_mode", node) || !Vec(source, 16, "center", node) ||
				!Vec(source, 17, "half_size", node) || !Real(source, 19, "shape_rotation", node) ||
				!Real(source, 28, "shape_scale", node) || !Vec(source, 20, "level", node) ||
				!Int(source, 1, "background", node) || !Int(source, 47, "bg_blend", node) ||
				!Bool(source, 6, "antialias", node) || !Bool(source, 12, "height_render", node) ||
				!Bool(source, 37, "opacity", node) || !Bool(source, 51, "multiply_alpha", node) ||
				!MaskAlpha(source, 50, node))
				return false;
			if (name == "Half") return Vec(source, 40, "point1", node);
			if (name == "Triangle")
				return Vec(source, 32, "point1", node) && Vec(source, 33, "point2", node) &&
					   Vec(source, 35, "point3", node);
			return true;
		}
		bool Vignette(const Json &source, Node &node) {
			// The native path evaluates the scalar controls without masks, parameter maps, or curves.
			if (Input(source, 17) || !Active(source, 1) || !ReferenceUnit(source, 15) ||
				!DefaultNumber(source, 7, -4) || !DefaultNumber(source, 8, -4) ||
				!DefaultNumber(source, 9, -4) || !DefaultCurve(source, 10, false) ||
				!DefaultNumber(source, 11, -4) || !DefaultNumber(source, 12, 1) ||
				!DefaultBool(source, 13, false) || !DefaultNumber(source, 14, 0))
				return false;
			for (size_t index = 0; index < 17; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index) || input->contains("anim") ||
					input->contains("global_key") || input->contains("global_use"))
					return false;
				if (index != 0 && !Current(source, index)) return false;
			}
			const Json *image = Input(source, 0);
			if (!image || !image->contains("from_node")) return false;
			const Json *color = Current(source, 16);
			Colour packed;
			if (!color || !PackedColour(*color, packed)) return false;
			node.Type = "image.vignette";
			node.Values.push_back({"color", packed});
			return Vec(source, 15, "center", node) && Real(source, 5, "roundness", node) &&
				   Real(source, 2, "exposure", node) && Real(source, 3, "strength", node) &&
				   Real(source, 4, "exponent", node) && Real(source, 6, "lighten", node);
		}
		bool CurveColor(const Json &source, Node &node) {
			if (Input(source, 12) || !Active(source, 7) || !DefaultMaskAlpha(source, 5)) return false;
			for (size_t index = 0; index < 12; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index) || input->contains("anim") ||
					input->contains("global_key") || input->contains("global_use"))
					return false;
				if (index != 0 && index != 5 && !Current(source, index)) return false;
			}
			if (!Input(source, 0)->contains("from_node")) return false;
			if (!Input(source, 5)->contains("from_node") && !DefaultNumber(source, 5, -4)) return false;
			node.Type = "image.curve";
			return CurveValue(source, 1, "brightness", node) && CurveValue(source, 2, "red", node) &&
				   CurveValue(source, 3, "green", node) && CurveValue(source, 4, "blue", node) &&
				   CurveValue(source, 11, "alpha", node) && Real(source, 6, "mix", node) &&
				   Int(source, 8, "channel", node) && Bool(source, 9, "invert_mask", node) &&
				   Real(source, 10, "mask_feather", node);
		}
		bool Colorize(const Json &source, Node &node) {
			if (Input(source, 16) || !Active(source, 5) || !DefaultMaskAlpha(source, 3) ||
				!DefaultNumber(source, 10, -4) || !DefaultNumber(source, 11, -4) ||
				!DefaultVector(source, 12, {0, 0, 1, 0}))
				return false;
			for (size_t index = 0; index < 16; ++index) {
				const Json *input = Input(source, index);
				if (!input || !Unmapped(source, index) || input->contains("anim") ||
					input->contains("global_key") || input->contains("global_use"))
					return false;
				if (index != 0 && index != 3 && !Current(source, index)) return false;
			}
			if (!Input(source, 0)->contains("from_node")) return false;
			if (!Input(source, 3)->contains("from_node") && !DefaultNumber(source, 3, -4)) return false;
			const Json *encoded = Current(source, 1);
			if (!encoded || !encoded->is_string() ||
				encoded->get_ref<const std::string &>().size() > imagegraph::Limits::MaximumTextBytes)
				return false;
			Json parsed;
			try {
				parsed = Json::parse(encoded->get_ref<const std::string &>());
			} catch (const Json::exception &) {
				return false;
			}
			if (!parsed.is_object() || !parsed.contains("type") || !parsed["type"].is_number() ||
				parsed["type"].get<double>() != 0 || !parsed.contains("keys") || !parsed["keys"].is_array() ||
				parsed["keys"].size() < 2 || parsed["keys"].size() > imagegraph::Limits::MaximumGradientKeys)
				return false;
			imagegraph::Gradient gradient;
			for (const Json &entry : parsed["keys"]) {
				if (!entry.is_object() || !entry.contains("time") || !entry["time"].is_number() ||
					!entry.contains("value"))
					return false;
				const double time = entry["time"].get<double>();
				Colour color;
				if (!std::isfinite(time) || time < 0 || time > 1 ||
					(!gradient.Keys.empty() && time <= gradient.Keys.back().Time) ||
					!PackedColour(entry["value"], color))
					return false;
				gradient.Keys.push_back({time, color});
			}
			node.Type = "image.colorize";
			node.Values.push_back({"gradient", std::move(gradient)});
			return Real(source, 2, "shift", node) && Vec(source, 14, "color_range", node) &&
				   Int(source, 15, "overflow", node) && Bool(source, 6, "multiply_alpha", node) &&
				   Bool(source, 13, "keep_alpha", node) && Int(source, 7, "channel", node) &&
				   Real(source, 4, "mix", node) && Bool(source, 8, "invert_mask", node) &&
				   Real(source, 9, "mask_feather", node);
		}

		std::string_view NativeInput(std::string_view type, uint32_t index) {
			if (type == "Node_Math") {
				if (index == 1) return "a";
			} else if (type == "Node_Solid") {
				if (index == 5) return "foreground";
				if (index == 3) return "mask";
			} else if (type == "Node_Invert") {
				if (index == 0) return "image";
				if (index == 1) return "mask";
			} else if (type == "Node_Blend") {
				if (index == 0) return "background";
				if (index == 1) return "foreground";
				if (index == 4) return "mask";
			} else if (type == "Node_Gradient") {
				if (index == 3) return "angle_value";
				if (index == 18) return "uv_map";
				if (index == 8) return "mask";
				if (index == 10) return "angle_map";
				if (index == 11) return "radius_map";
				if (index == 12) return "shift_map";
				if (index == 13) return "scale_map";
			} else if (type == "Node_Offset" || type == "Node_Threshold" || type == "Node_Polar") {
				if (index == 0) return "image";
			} else if (type == "Node_Tile" || type == "Node_Blur" || type == "Node_Color_adjust" ||
					   type == "Node_Posterize") {
				if (index == 0) return "image";
				if ((type == "Node_Color_adjust" && index == 8) || (type == "Node_Posterize" && index == 12))
					return "mask";
			} else if (type == "Node_Shape") {
				if (index == 46) return "bg_surface";
				if (index == 50) return "mask";
			} else if (type == "Node_Vignette") {
				if (index == 0) return "image";
			} else if (type == "Node_Curve" || type == "Node_Colorize") {
				if (index == 0) return "image";
				if (index == (type == "Node_Curve" ? 5u : 3u)) return "mask";
			}
			return {};
		}
		bool ReservedSourceOutput(const bake::PxcxLinkFact &link) {
			return link.FromTag == -2 || link.FromTag == -3 || link.FromTag == -4;
		}
		bool CommonSourceRoute(const bake::PxcxLinkFact &link) {
			return ReservedSourceOutput(link) || link.DestinationUpdateTrigger;
		}
		bool SourceSolidCommonLifecycle(
			const Json &source, const bake::PxcxArchive &archive, std::string_view nodeId
		) {
			const auto attributes = source.find("attri");
			if (attributes != source.end() && attributes->is_object()) {
				for (const auto key : {"show_update_trigger", "outp_meta", "update_graph"}) {
					const auto flag = attributes->find(key);
					if (flag != attributes->end() && flag->is_boolean() &&
						flag->get<bool>() == (std::string_view(key) != "update_graph"))
						return true;
				}
			}
			return std::any_of(archive.Links.begin(), archive.Links.end(), [&](const auto &link) {
				return CommonSourceRoute(link) && (link.FromNode == nodeId || link.ToNode == nodeId);
			});
		}
		std::string ReservedSourceOutputPort(const bake::PxcxLinkFact &link) {
			if (link.FromTag == -2) return "pxcx.update_in_trigger";
			if (link.FromTag == -3) return "pxcx.updated_out_trigger";
			return "pxcx.metadata." + std::to_string(link.FromIndex);
		}
		bool LinksRepresentable(
			const bake::PxcxArchive &archive, const bake::PxcxNodeFact &node, bool admitCommon
		) {
			for (const bake::PxcxLinkFact &link : archive.Links) {
				if (!admitCommon && CommonSourceRoute(link) &&
					(link.FromNode == node.Id || link.ToNode == node.Id))
					return false;
				if (link.FromNode == node.Id && !ReservedSourceOutput(link) &&
					detail::LegacyNativeOutputPort(node.Type, link.FromIndex).empty())
					return false;
				if (link.ToNode == node.Id && !link.DestinationUpdateTrigger &&
					NativeInput(node.Type, link.ToInputIndex).empty())
					return false;
			}
			return true;
		}
		void OpaqueDiagnostic(PxcxImport &result, const bake::PxcxNodeFact &node, std::string reason) {
			result.Diagnostics.push_back({imagegraph::Status::UnknownNode, node.Id, {}, std::move(reason)});
		}

		bool WholeNumber(const Json &value, int64_t &out) {
			if (!value.is_number()) return false;
			const double number = value.get<double>();
			if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > 9.0e15)
				return false;
			out = static_cast<int64_t>(number);
			return true;
		}

		bool OrdinarySourceOutputTag(const Json &record) {
			const auto tagged = record.find("from_tag");
			if (tagged == record.end()) return true;
			if (!tagged->is_number()) return false;
			const double tag = tagged->get<double>();
			if (!std::isfinite(tag) || std::trunc(tag) != tag) return false;
			// Pinned getOutputIndex falls through for every tag except trigger and metadata selectors.
			return tag != -2 && tag != -3 && tag != -4;
		}

		bool Numbers(const Json &value, size_t count, std::array<double, 4> &out) {
			if (!value.is_array() || value.size() != count) return false;
			for (size_t index = 0; index < count; index++) {
				if (!value[index].is_number()) return false;
				out[index] = value[index].get<double>();
				if (!std::isfinite(out[index])) return false;
			}
			return true;
		}

		template <class T>
		bool AdmitNativeSlots(std::vector<T> &items, size_t count, detail::ImportBudget *budget) {
			if (count <= items.capacity()) return true;
			if (count > UINT64_MAX / sizeof(T) || (budget && !budget->Hold(count * sizeof(T)))) return false;
			items.reserve(count);
			return true;
		}
		bool AdmitNativeTextSize(size_t size, detail::ImportBudget *budget) {
			return size <= imagegraph::Limits::MaximumTextBytes &&
				   (!budget || budget->Hold(std::max(size, std::string{}.capacity()) + 1));
		}
		bool AdmitNativeText(std::string_view text, detail::ImportBudget *budget) {
			return AdmitNativeTextSize(text.size(), budget);
		}

		bool NativePortName(
			std::string_view prefix,
			std::string_view separator,
			std::string_view suffix,
			detail::ImportBudget *budget,
			std::string &out
		) {
			if (prefix.size() > imagegraph::Limits::MaximumTextBytes ||
				separator.size() > imagegraph::Limits::MaximumTextBytes - prefix.size() ||
				suffix.size() > imagegraph::Limits::MaximumTextBytes - prefix.size() - separator.size())
				return false;
			const size_t size = prefix.size() + separator.size() + suffix.size();
			if (!AdmitNativeTextSize(size, budget)) return false;
			std::string result(size, '\0');
			auto tail = std::copy(prefix.begin(), prefix.end(), result.begin());
			tail = std::copy(separator.begin(), separator.end(), tail);
			std::copy(suffix.begin(), suffix.end(), tail);
			out = std::move(result);
			return true;
		}

		bool GradientText(
			const Json &value, imagegraph::Gradient &gradient, detail::ImportBudget *budget = nullptr
		) {
			if (!value.is_string() ||
				value.get_ref<const std::string &>().size() > imagegraph::Limits::MaximumTextBytes)
				return false;
			Json keys;
			try {
				keys = Json::parse(value.get_ref<const std::string &>());
			} catch (const Json::exception &) {
				return false;
			}
			if (!keys.is_object() || !keys.contains("type") || !keys.contains("keys") ||
				!keys["keys"].is_array() || keys["keys"].empty() ||
				keys["keys"].size() > imagegraph::Limits::MaximumGradientKeys)
				return false;
			int64_t mode = 0;
			if (!WholeNumber(keys["type"], mode) || mode < 0 || mode > 6) return false;
			gradient = {static_cast<uint8_t>(mode), {}};
			if (!AdmitNativeSlots(gradient.Keys, keys["keys"].size(), budget)) return false;
			for (const Json &entry : keys["keys"]) {
				if (!entry.is_object() || !entry.contains("time") || !entry["time"].is_number() ||
					!entry.contains("value"))
					return false;
				const double time = entry["time"].get<double>();
				Colour color;
				if (!std::isfinite(time) || !PackedColour(entry["value"], color)) return false;
				gradient.Keys.push_back({time, color});
			}
			return true;
		}

		bool SourceArrayItems(
			const Json &value,
			std::vector<imagegraph::SourceArrayItem> &items,
			size_t depth,
			size_t &count,
			uint64_t &bytes,
			detail::ImportBudget *budget,
			imagegraph::ValueType leafType = imagegraph::ValueType::Any
		) {
			using namespace imagegraph;
			if (!value.is_array() || depth >= Limits::MaximumArrayDepth ||
				value.size() > Limits::MaximumArrayElements - count)
				return false;
			count += value.size();
			const uint64_t storage = value.size() * sizeof(SourceArrayItem);
			if (storage > Limits::MaximumArrayBytes - bytes || !AdmitNativeSlots(items, value.size(), budget))
				return false;
			bytes += storage;
			for (const auto &child : value) {
				SourceArrayItem item;
				if (child.is_array()) {
					std::vector<SourceArrayItem> nested;
					if (!SourceArrayItems(child, nested, depth + 1, count, bytes, budget, leafType))
						return false;
					item.Data = std::move(nested);
				} else if (leafType == ValueType::Colour) {
					Colour colour;
					if (!PackedColour(child, colour)) return false;
					item.Data = ElementValue{colour};
				} else if (child.is_boolean())
					item.Data = ElementValue{child.get<bool>()};
				else if (child.is_number_unsigned()) {
					const auto number = child.get<uint64_t>();
					if (number > uint64_t(INT64_MAX)) return false;
					item.Data = ElementValue{int64_t(number)};
				} else if (child.is_number_integer())
					item.Data = ElementValue{child.get<int64_t>()};
				else if (child.is_number_float()) {
					const double number = child.get<double>();
					if (!std::isfinite(number)) return false;
					item.Data = ElementValue{number};
				} else if (child.is_string()) {
					const auto &text = child.get_ref<const std::string &>();
					if (text.size() > Limits::MaximumTextBytes ||
						text.size() > Limits::MaximumArrayBytes - bytes || !AdmitNativeText(text, budget))
						return false;
					bytes += text.size();
					item.Data = ElementValue{std::string(text)};
				} else
					return false;
				items.push_back(std::move(item));
			}
			return true;
		}
		// Converts one serialized source value to a catalogue input type. False means the value has no exact
		// native meaning, and the node stays opaque.
		bool CatalogueValue(
			const Json &value,
			imagegraph::ValueType type,
			size_t choices,
			imagegraph::Value &out,
			imagegraph::ValueType element = imagegraph::ValueType::Scalar,
			detail::ImportBudget *budget = nullptr
		) {
			using imagegraph::ValueType;
			std::array<double, 4> parts{};
			int64_t whole = 0;
			switch (type) {
			case ValueType::Any:
				if (value.is_boolean()) {
					out = value.get<bool>();
					return true;
				}
				if (value.is_number_integer()) {
					out = value.get<int64_t>();
					return true;
				}
				if (value.is_number_unsigned()) {
					if (value.get<uint64_t>() > uint64_t(INT64_MAX)) return false;
					out = static_cast<int64_t>(value.get<uint64_t>());
					return true;
				}
				if (value.is_number_float()) {
					if (!std::isfinite(value.get<double>())) return false;
					out = value.get<double>();
					return true;
				}
				if (value.is_string()) {
					if (value.get_ref<const std::string &>().size() > imagegraph::Limits::MaximumTextBytes)
						return false;
					if (!AdmitNativeText(value.get_ref<const std::string &>(), budget)) return false;
					out = value.get<std::string>();
					return true;
				}
				if (value.is_array()) {
					const bool numeric = std::all_of(value.begin(), value.end(), [](const Json &item) {
						return item.is_number() && std::isfinite(item.get<double>());
					});
					if (numeric)
						return CatalogueValue(value, ValueType::Array, 0, out, ValueType::Scalar, budget);
					ArrayValue array{ValueType::Any, {}};
					size_t count = 0;
					uint64_t bytes = 0;
					if (!SourceArrayItems(value, array.Items, 0, count, bytes, budget)) return false;
					out = std::move(array);
					return true;
				}
				return false;
			case ValueType::Boolean:
				if (value.is_boolean()) {
					out = value.get<bool>();
					return true;
				}
				if (!WholeNumber(value, whole) || (whole != 0 && whole != 1)) return false;
				out = whole == 1;
				return true;
			// GML booleans are the numbers 0 and 1.
			case ValueType::Integer:
				if (value.is_boolean())
					whole = value.get<bool>() ? 1 : 0;
				else if (!WholeNumber(value, whole))
					return false;
				out = whole;
				return true;
			case ValueType::Scalar:
				if (value.is_boolean()) {
					out = value.get<bool>() ? 1.0 : 0.0;
					return true;
				}
				if (!value.is_number() || !std::isfinite(value.get<double>())) return false;
				out = value.get<double>();
				return true;
			case ValueType::Enum:
				if (value.is_boolean())
					whole = value.get<bool>() ? 1 : 0;
				else if (!WholeNumber(value, whole))
					return false;
				if (whole < 0 || (choices > 0 && static_cast<size_t>(whole) >= choices)) return false;
				out = imagegraph::EnumValue{whole};
				return true;
			case ValueType::Colour: {
				Colour color;
				if (!PackedColour(value, color)) return false;
				out = color;
				return true;
			}
			case ValueType::Vector2:
				if (!Numbers(value, 2, parts)) return false;
				out = Vector2{parts[0], parts[1]};
				return true;
			case ValueType::Vector3:
				if (!Numbers(value, 3, parts)) return false;
				out = imagegraph::Vector3{parts[0], parts[1], parts[2]};
				return true;
			case ValueType::Vector4:
				if (!Numbers(value, 4, parts)) return false;
				out = imagegraph::Vector4{parts[0], parts[1], parts[2], parts[3]};
				return true;
			case ValueType::Quaternion:
				if (!Numbers(value, 4, parts)) return false;
				out = imagegraph::Quaternion{parts[0], parts[1], parts[2], parts[3]};
				return true;
			case ValueType::Text:
				if (!value.is_string() ||
					value.get_ref<const std::string &>().size() > imagegraph::Limits::MaximumTextBytes)
					return false;
				if (!AdmitNativeText(value.get_ref<const std::string &>(), budget)) return false;
				out = value.get<std::string>();
				return true;
			case ValueType::Gradient: {
				imagegraph::Gradient gradient;
				if (!GradientText(value, gradient, budget)) return false;
				out = std::move(gradient);
				return true;
			}
			case ValueType::Curve: {
				if (!value.is_array() || value.size() < 18 || (value.size() - 6) % 6 != 0 ||
					(value.size() - 6) / 6 > imagegraph::Limits::MaximumCurveAnchors)
					return false;
				imagegraph::Curve curve;
				if (!AdmitNativeSlots(curve.Anchors, (value.size() - 6) / 6, budget)) return false;
				for (size_t offset = 0; offset < value.size(); ++offset) {
					if (!value[offset].is_number() || !std::isfinite(value[offset].get<double>()))
						return false;
					if (offset < 6)
						curve.Header[offset] = value[offset].get<double>();
					else {
						if ((offset - 6) % 6 == 0) curve.Anchors.emplace_back();
						curve.Anchors.back()[(offset - 6) % 6] = value[offset].get<double>();
					}
				}
				out = std::move(curve);
				return true;
			}
			case ValueType::Matrix: {
				// Matrix.serialize is { size: [columns, rows], isize, raw }; a bare list is a square matrix.
				imagegraph::MatrixValue matrix;
				const Json *raw = &value;
				if (value.is_object()) {
					const auto size = value.find("size"), stored = value.find("raw");
					if (size == value.end() || stored == value.end() || !Numbers(*size, 2, parts) ||
						parts[0] < 1 || parts[1] < 1 || std::trunc(parts[0]) != parts[0] ||
						std::trunc(parts[1]) != parts[1] || parts[0] > UINT32_MAX || parts[1] > UINT32_MAX)
						return false;
					matrix.Columns = uint32_t(parts[0]);
					matrix.Rows = uint32_t(parts[1]);
					raw = &*stored;
				} else if (value.is_array()) {
					matrix.Columns = matrix.Rows = uint32_t(std::floor(std::sqrt(double(value.size()))));
				} else {
					return false;
				}
				if (!raw->is_array() ||
					uint64_t(matrix.Columns) * matrix.Rows > imagegraph::Limits::MaximumArrayElements ||
					matrix.Columns == 0)
					return false;
				if (!AdmitNativeSlots(matrix.Values, size_t(matrix.Columns) * matrix.Rows, budget))
					return false;
				matrix.Values.assign(size_t(matrix.Columns) * matrix.Rows, 0.0);
				for (size_t index = 0; index < std::min(raw->size(), matrix.Values.size()); index++) {
					if (!(*raw)[index].is_number()) return false;
					matrix.Values[index] = (*raw)[index].get<double>();
				}
				out = std::move(matrix);
				return true;
			}
			case ValueType::Area: {
				if (!value.is_array() || value.size() != 6) return false;
				std::array<double, 6> area{};
				for (size_t index = 0; index < 6; index++) {
					if (!value[index].is_number() || !std::isfinite(value[index].get<double>())) return false;
					area[index] = value[index].get<double>();
				}
				if (area[4] != 0 && area[4] != 1) return false;
				if (area[5] != 0 && area[5] != 1 && area[5] != 2) return false;
				out = imagegraph::Area{
					area[0],
					area[1],
					area[2],
					area[3],
					static_cast<uint8_t>(area[4]),
					static_cast<uint8_t>(area[5])
				};
				return true;
			}
			case ValueType::Array: {
				if (!value.is_array() || value.size() > imagegraph::Limits::MaximumArrayElements)
					return false;
				ArrayValue array{element, {}};
				if (!AdmitNativeSlots(array.Elements, value.size(), budget)) return false;
				for (const Json &item : value) {
					if (element == ValueType::Colour) {
						Colour color;
						if (!PackedColour(item, color)) return false;
						array.Elements.emplace_back(color);
					} else if (element == ValueType::Boolean) {
						if (!item.is_boolean()) return false;
						array.Elements.emplace_back(item.get<bool>());
					} else if (element == ValueType::Integer) {
						if (!WholeNumber(item, whole)) return false;
						array.Elements.emplace_back(whole);
					} else if (element == ValueType::Text) {
						if (!item.is_string() ||
							!AdmitNativeText(item.get_ref<const std::string &>(), budget))
							return false;
						array.Elements.emplace_back(item.get<std::string>());
					} else if (element == ValueType::Vector2) {
						if (!Numbers(item, 2, parts)) return false;
						array.Elements.emplace_back(Vector2{parts[0], parts[1]});
					} else {
						if (!item.is_number() || !std::isfinite(item.get<double>())) return false;
						array.Elements.emplace_back(item.get<double>());
					}
				}
				out = std::move(array);
				return true;
			}
			default:
				return false;
			}
		}

		// Node attribute lists that are not numbers: node ids become text, nested lists keep their JSON text.
		bool AttributeList(const Json &value, imagegraph::ValueType type, imagegraph::Value &out) {
			if (type != imagegraph::ValueType::Array || !value.is_array() ||
				value.size() > imagegraph::Limits::MaximumArrayElements)
				return false;
			// Lists of number pairs, such as Path weights, flatten to numbers.
			const bool numberRows = std::all_of(value.begin(), value.end(), [](const Json &item) {
				return item.is_array() && std::all_of(item.begin(), item.end(), [](const Json &part) {
						   return part.is_number() && std::isfinite(part.get<double>());
					   });
			});
			if (numberRows && !value.empty()) {
				ArrayValue numbers{imagegraph::ValueType::Scalar, {}};
				for (const Json &item : value)
					for (const Json &part : item)
						numbers.Elements.emplace_back(part.get<double>());
				if (numbers.Elements.size() > imagegraph::Limits::MaximumArrayElements) return false;
				out = std::move(numbers);
				return true;
			}
			ArrayValue array{imagegraph::ValueType::Text, {}};
			for (const Json &item : value) {
				std::string text = item.is_string() ? item.get<std::string>() : item.dump();
				if (text.size() > imagegraph::Limits::MaximumTextBytes) return false;
				array.Elements.emplace_back(std::move(text));
			}
			out = std::move(array);
			return true;
		}

		// Palettes hold colours, point lists hold vectors, and path/text lists retain strings.
		imagegraph::ValueType ArrayElement(const imagegraph::CatalogueInput &input) {
			if (input.SourceKind == "Palette") return imagegraph::ValueType::Colour;
			if (input.SourceKind == "Vec2" || input.SourceKind == "IVec2" || input.SourceKind == "Vector" ||
				input.SourceKind == "Vec2Arr")
				return imagegraph::ValueType::Vector2;
			if (input.SourceKind == "Text" || input.SourceKind == "FPath") return imagegraph::ValueType::Text;
			return imagegraph::ValueType::Scalar;
		}

		bool CatalogueLiteralArray(
			const Json &,
			const imagegraph::CatalogueEntry &,
			const imagegraph::CatalogueInput &,
			imagegraph::Value &,
			detail::ImportBudget *budget = nullptr
		);
		bool CatalogueDriver(
			const Json &record, imagegraph::Keyframe &key, detail::ImportBudget *budget = nullptr
		) {
			if (record.is_number() && record.get<double>() == 0) return true;
			if (!record.is_object()) return false;
			const auto type = record.find("typ");
			if (type == record.end() || !type->is_string()) return false;
			const auto number = [&](std::string_view name, double &value) {
				const auto item = record.find(name);
				if (item == record.end() || !item->is_number()) return false;
				value = item->get<double>();
				return std::isfinite(value);
			};
			const auto &name = type->get_ref<const std::string &>();
			if (name == "linear") {
				imagegraph::KeyframeLinearDriver control;
				if (!number("spd", control.Speed)) return false;
				key.SourceDriver = control;
			} else if (name == "sine") {
				imagegraph::KeyframeSineDriver control;
				if (!number("fre", control.Frequency) || !number("amp", control.Amplitude) ||
					!number("phs", control.Phase))
					return false;
				control.Smooth = 0;
				if (record.contains("smt") && !number("smt", control.Smooth)) return false;
				key.SourceDriver = control;
			} else if (name == "snap") {
				imagegraph::KeyframeSnapDriver control;
				if (!number("snp", control.Size)) return false;
				key.SourceDriver = control;
			} else if (name == "bounce" || name == "elastic") {
				int64_t amount;
				double spacing, curve = 2;
				const auto value = record.find("amo");
				if (value == record.end() || !WholeNumber(*value, amount) || amount > 1024 ||
					!number("amp", spacing))
					return false;
				if (record.contains("stp") && !number("stp", curve)) return false;
				if (name == "bounce")
					key.SourceDriver = imagegraph::KeyframeBounceDriver{amount, spacing, curve};
				else
					key.SourceDriver = imagegraph::KeyframeElasticDriver{amount, spacing, curve};
			} else if (name == "curve") {
				const auto raw = record.find("crv");
				imagegraph::Curve curve;
				if (raw == record.end() || !raw->is_array() || raw->size() < 6 || (raw->size() - 6) % 6 ||
					(raw->size() - 6) / 6 > imagegraph::Limits::MaximumCurveAnchors)
					return false;
				if (!AdmitNativeSlots(curve.Anchors, (raw->size() - 6) / 6, budget)) return false;
				curve.Anchors.resize((raw->size() - 6) / 6);
				for (size_t i = 0; i < raw->size(); i++) {
					if (!(*raw)[i].is_number() || !std::isfinite((*raw)[i].get<double>())) return false;
					if (i < 6)
						curve.Header[i] = (*raw)[i].get<double>();
					else
						curve.Anchors[(i - 6) / 6][(i - 6) % 6] = (*raw)[i].get<double>();
				}
				key.SourceDriver = imagegraph::KeyframeCurveDriver{std::move(curve)};
			} else
				return false;
			return imagegraph::ValidKeyframeSourceDriver(*key.SourceDriver);
		}

		bool CatalogueSourceChoice(
			const Json &value, const imagegraph::CatalogueInput *input, imagegraph::Value &out
		) {
			if (!input || !value.is_number() || !std::isfinite(value.get<double>())) return false;
			imagegraph::Value choice = value.get<double>();
			if (!imagegraph::CatalogueSourceEnumValue(*input, choice)) return false;
			out = std::move(choice);
			return true;
		}

		bool CatalogueHlslValue(
			const Json &value,
			const imagegraph::CatalogueEntry *entry,
			const imagegraph::CatalogueInput *input,
			imagegraph::Value &out,
			detail::ImportBudget *budget
		) {
			using imagegraph::ValueType;
			if (!entry || entry->Type != "pc.hlsl" || !input || !input->Id.starts_with("argument_value_"))
				return false;
			if (input->Type == ValueType::Scalar || input->Type == ValueType::Integer ||
				input->Type == ValueType::Image || input->Type == ValueType::Colour) {
				if (value.is_number_float() && std::isfinite(value.get<double>())) {
					out = value.get<double>();
					return true;
				}
				if (!value.is_number_integer() ||
					(value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(INT64_MAX)))
					return false;
				out = value.get<int64_t>();
				return true;
			}
			if (input->Type != ValueType::Array || !value.is_array()) return false;
			const auto integer = [](const Json &item) { return item.is_number_integer(); };
			const bool nested = !value.empty() && value.front().is_array();
			const bool integers = std::all_of(value.begin(), value.end(), [&](const Json &item) {
				return nested ? item.is_array() && std::all_of(item.begin(), item.end(), integer)
							  : integer(item);
			});
			imagegraph::ArrayValue array{integers ? ValueType::Integer : ValueType::Scalar, {}};
			const auto row = [&](const Json &source, auto &destination) {
				if (source.size() > imagegraph::Limits::MaximumArrayElements ||
					!AdmitNativeSlots(destination, source.size(), budget))
					return false;
				for (const auto &item : source) {
					if (item.is_number_integer()) {
						if (item.is_number_unsigned() && item.get<uint64_t>() > uint64_t(INT64_MAX))
							return false;
						destination.emplace_back(item.get<int64_t>());
					} else if (item.is_number_float() && std::isfinite(item.get<double>()))
						destination.emplace_back(item.get<double>());
					else
						return false;
				}
				return true;
			};
			if (nested) {
				if (value.size() > imagegraph::Limits::MaximumArrayElements ||
					!AdmitNativeSlots(array.Nested, value.size(), budget))
					return false;
				for (const auto &source : value) {
					if (!source.is_array()) return false;
					array.Nested.emplace_back();
					if (!row(source, array.Nested.back())) return false;
				}
			} else if (!row(value, array.Elements))
				return false;
			out = std::move(array);
			return true;
		}
		bool CatalogueRawValue(
			const Json &value,
			const imagegraph::CatalogueInput *input,
			imagegraph::Value &out,
			bool explicitReal = false
		) {
			// Explicit JSON reals retain raw source getter storage, including integral-valued reals.
			// Integer and Boolean tokens keep their established typed projection.
			if (!input || !value.is_number() || (explicitReal && !value.is_number_float())) return false;
			imagegraph::Value raw = value.get<double>();
			if (!imagegraph::CatalogueSourceRawValue(*input, raw)) return false;
			out = std::move(raw);
			return true;
		}

		bool CatalogueEmptyGroupVector(
			const Json &value,
			const imagegraph::CatalogueEntry *entry,
			const imagegraph::CatalogueInput *input,
			imagegraph::Value &out
		) {
			if (!entry || !input || (entry->Type != "pc.group_input" && entry->Type != "pc.group_output") ||
				input->Type != imagegraph::ValueType::Vector2 ||
				(input->SourceKind != "Range" && input->SourceKind != "Vec2") || !value.is_array() ||
				!value.empty())
				return false;
			out = imagegraph::ArrayValue{imagegraph::ValueType::Scalar, {}};
			return true;
		}

		// Maps source-eased keys and the verified deterministic source drivers. Unknown drivers stay opaque.
		bool CatalogueKeyframes(
			const Json &records,
			const std::string &nodeId,
			std::string_view port,
			imagegraph::ValueType type,
			size_t choices,
			imagegraph::ValueType element,
			std::vector<imagegraph::Keyframe> &out,
			const imagegraph::CatalogueEntry *entry = nullptr,
			const imagegraph::CatalogueInput *input = nullptr,
			detail::ImportBudget *budget = nullptr
		) {
			// A compressed animated record reloads as a normal frame-zero key with source defaults.
			if (records.is_object()) {
				const auto stored = records.find("d");
				if (stored == records.end() || out.size() >= imagegraph::Limits::MaximumKeyframes)
					return false;
				if (!AdmitNativeSlots(out, out.size() + 1, budget) || !AdmitNativeText(nodeId, budget) ||
					!AdmitNativeText(port, budget))
					return false;
				imagegraph::Value data;
				if (!CatalogueHlslValue(*stored, entry, input, data, budget) &&
					!CatalogueRawValue(*stored, input, data, true) &&
					!CatalogueValue(*stored, type, choices, data, element, budget) &&
					!CatalogueSourceChoice(*stored, input, data) &&
					!CatalogueRawValue(*stored, input, data) &&
					!CatalogueEmptyGroupVector(*stored, entry, input, data) &&
					!(entry && input && CatalogueLiteralArray(*stored, *entry, *input, data, budget)))
					return false;
				imagegraph::Keyframe keyframe;
				keyframe.NodeId = std::string(nodeId);
				keyframe.Port = std::string(port);
				keyframe.Data = std::move(data);
				keyframe.Interpolation = "source";
				keyframe.Ease = imagegraph::KeyframeEase{"linear", "linear", {0, 1}, {0, 0}};
				if (!AdmitNativeText(detail::CompactSourceKeyId, budget)) return false;
				keyframe.SourceKeyId = detail::CompactSourceKeyId;
				out.push_back(std::move(keyframe));
				return true;
			}
			if (!records.is_array() || records.empty() || out.size() > imagegraph::Limits::MaximumKeyframes ||
				records.size() > imagegraph::Limits::MaximumKeyframes - out.size())
				return false;
			if (!AdmitNativeSlots(out, out.size() + records.size(), budget)) return false;
			static constexpr std::array<std::string_view, 3> SIDES = {"linear", "bezier", "cut"};
			size_t occurrence = 0;
			for (const Json &record : records) {
				if (!record.is_array() || record.size() < 8) return false;
				const bool scalarTriggerTime =
					input && input->SourceKind == "Trigger" && record[0].is_number();
				if (!scalarTriggerTime && (!record[0].is_array() || record[0].size() < 2)) return false;
				int64_t kind = 0, inType = 0, outType = 0;
				imagegraph::FrameTime frame;
				const Json &time = scalarTriggerTime ? record[0] : record[0][1];
				if ((!scalarTriggerTime && (!WholeNumber(record[0][0], kind) || (kind != 0 && kind != 1))) ||
					!time.is_number() || !imagegraph::SplitFrameTime(time.get<double>(), frame))
					return false;
				if (!AdmitNativeText(nodeId, budget) || !AdmitNativeText(port, budget)) return false;
				imagegraph::Value data;
				if (!CatalogueHlslValue(record[1], entry, input, data, budget) &&
					!CatalogueRawValue(record[1], input, data, true) &&
					!CatalogueValue(record[1], type, choices, data, element, budget) &&
					!CatalogueSourceChoice(record[1], input, data) &&
					!CatalogueRawValue(record[1], input, data) &&
					!(records.size() == 1 && kind == 0 && record[7].is_number() &&
					  record[7].get<double>() == 0 &&
					  CatalogueEmptyGroupVector(record[1], entry, input, data)) &&
					!(entry && input && CatalogueLiteralArray(record[1], *entry, *input, data, budget)))
					return false;
				std::array<double, 4> ease{};
				if (!Numbers(record[2], 2, ease) || !WholeNumber(record[4], inType) ||
					!WholeNumber(record[5], outType) || inType < 0 || inType > 2 || outType < 0 ||
					outType > 2)
					return false;
				std::array<double, 4> easeOut{};
				if (!Numbers(record[3], 2, easeOut)) return false;
				imagegraph::Keyframe keyframe;
				keyframe.NodeId = std::string(nodeId);
				keyframe.Port = std::string(port);
				// Source kind1 is retained metadata; raw time-data tails stay in the archive.
				keyframe.Kind =
					kind == 0 ? imagegraph::KeyframeKind::Normal : imagegraph::KeyframeKind::Adder;
				if (!imagegraph::SetFrameTime(keyframe, frame)) return false;
				keyframe.Data = std::move(data);
				keyframe.Interpolation = "source";
				keyframe.Ease = imagegraph::KeyframeEase{
					std::string(SIDES[static_cast<size_t>(inType)]),
					std::string(SIDES[static_cast<size_t>(outType)]),
					{ease[0], ease[1]},
					{easeOut[0], easeOut[1]}
				};
				if (!CatalogueDriver(record[7], keyframe, budget)) return false;
				const auto identity = detail::SourceKeyId(frame, keyframe.Kind, occurrence++);
				if (!identity || !AdmitNativeText(identity->View(), budget)) return false;
				keyframe.SourceKeyId = identity->View();
				out.push_back(std::move(keyframe));
			}
			return true;
		}

		// KEYFRAME_END order is hold, loop, ping, wrap; both scalar axes share this property metadata.
		bool CatalogueTrack(
			const Json &record,
			const std::string &nodeId,
			std::string_view port,
			imagegraph::Document &animation,
			detail::ImportBudget *budget = nullptr,
			bool allowSeparatedVec2 = false
		) {
			if ((record.value("sep_axis", false) && !allowSeparatedVec2) ||
				animation.Tracks.size() >= imagegraph::Limits::MaximumTracks)
				return false;
			int64_t end = 0, range = -1;
			if (record.contains("on_end") && (!WholeNumber(record["on_end"], end) || end < 0 || end > 3))
				return false;
			if (record.contains("loop_range") && !WholeNumber(record["loop_range"], range)) return false;
			if (!AdmitNativeSlots(animation.Tracks, animation.Tracks.size() + 1, budget) ||
				!AdmitNativeText(nodeId, budget) || !AdmitNativeText(port, budget))
				return false;
			// Native source-eased keys always name their track, including the default hold end.
			static constexpr std::array<std::string_view, 4> ENDS = {"hold", "loop", "ping", "wrap"};
			animation.Tracks.push_back(
				{nodeId, std::string(port), std::string(ENDS[static_cast<size_t>(end)]), range}
			);
			return true;
		}

		const Json *Attribute(const Json &input, std::string_view key) {
			const auto attributes = input.find("attri");
			if (attributes == input.end() || !attributes->is_object()) return nullptr;
			const auto found = attributes->find(key);
			return found == attributes->end() ? nullptr : &*found;
		}

		// Projects one source record onto its catalogue input. Linked inputs keep their default; the link
		// itself is added with the graph links.
		bool CatalogueLiteralArray(
			const Json &value,
			const imagegraph::CatalogueEntry &entry,
			const imagegraph::CatalogueInput &input,
			imagegraph::Value &out,
			detail::ImportBudget *budget
		) {
			using imagegraph::ValueType;
			// Markov's processor consumes one complete palette per row. Preserve outer axes,
			// including empty rows, instead of flattening their packed colours into one palette.
			if (entry.Type == "pc.markov_gradient" && input.Id == "colors" && input.SourceIndex == 3 &&
				input.SourceKind == "Palette" && input.Type == ValueType::Array && input.ArrayDepthKnown &&
				input.ArrayDepth == 1 && imagegraph::FindCatalogueInput(entry, "attribute_process")) {
				ArrayValue array{ValueType::Any, {}};
				size_t count = 0;
				uint64_t bytes = 0;
				if (!SourceArrayItems(value, array.Items, 0, count, bytes, budget, ValueType::Colour))
					return false;
				out = std::move(array);
				return true;
			}
			const ValueType leafType = input.Type == ValueType::Enum && input.SourceBehavior &&
											   input.SourceBehavior->FractionalInterpolation == true
										   ? ValueType::Scalar
										   : input.Type;
			// These source updates broadcast numeric arrays themselves. The shared authored-array
			// predicate retains the exact verified input indices, depths, leaf kinds and payload caps.
			const bool manualNumeric =
				(entry.Type == "pc.number_simple" || entry.Type == "pc.math" || entry.Type == "pc.compare") &&
				imagegraph::CatalogueAuthoredArray(entry, input, ArrayValue{ValueType::Scalar, {}});
			if (!value.is_array() || value.size() > imagegraph::Limits::MaximumArrayElements ||
				leafType > ValueType::Vector2 || input.SourceIndex < 0 || !input.ArrayDepthKnown ||
				(!manualNumeric && !imagegraph::FindCatalogueInput(entry, "attribute_process") &&
				 !(entry.Type == "pc.number" && input.Id == "value" && input.SourceIndex == 0)))
				return false;
			uint64_t bytes = value.size() * sizeof(imagegraph::ElementValue);
			for (const Json &item : value) {
				if (input.Type == ValueType::Text) {
					if (!item.is_string()) return false;
					const size_t size = item.get_ref<const std::string &>().size();
					if (size > imagegraph::Limits::MaximumTextBytes ||
						size > imagegraph::Limits::MaximumArrayBytes - bytes)
						return false;
					bytes += size;
				}
			}
			if (bytes > imagegraph::Limits::MaximumArrayBytes) return false;
			ArrayValue array{leafType, {}};
			if (!AdmitNativeSlots(array.Elements, value.size(), budget)) return false;
			for (const Json &item : value) {
				imagegraph::Value leaf;
				if (!CatalogueValue(
						item,
						leafType,
						imagegraph::CatalogueChoiceCount(input),
						leaf,
						imagegraph::ValueType::Scalar,
						budget
					))
					return false;
				const bool represented = std::visit(
					[&](auto &&element) {
						using T = std::decay_t<decltype(element)>;
						if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int64_t> ||
									  std::is_same_v<T, double> || std::is_same_v<T, std::string> ||
									  std::is_same_v<T, Colour> || std::is_same_v<T, Vector2>) {
							array.Elements.emplace_back(std::move(element));
							return true;
						} else
							return false;
					},
					std::move(leaf)
				);
				if (!represented) return false;
			}
			if (!imagegraph::CatalogueAuthoredArray(entry, input, array)) return false;
			out = std::move(array);
			return true;
		}

		bool CatalogueVec2Axes(
			const Json &record,
			const imagegraph::CatalogueInput &input,
			const std::string &nodeId,
			Node &node,
			imagegraph::Document &document,
			std::string &reason,
			detail::ImportBudget *budget
		) {
			const auto fail = [&](std::string_view message) {
				reason = std::string(message);
				return false;
			};
			const auto encoded = record.find("animators");
			if (encoded != record.end() && !encoded->is_array())
				return fail("source Vec2 separated animator storage must be an array");
			size_t aggregate = document.Keyframes.size();
			for (const auto &other : document.Nodes)
				if (&other != &node && other.SourceSeparatedVec2Animators)
					for (const auto &stored : other.SourceSeparatedVec2Animators->Inputs)
						for (const auto &axis : stored.Axes)
							aggregate += axis.Keys.size();
			if (node.SourceSeparatedVec2Animators)
				for (const auto &stored : node.SourceSeparatedVec2Animators->Inputs)
					for (const auto &axis : stored.Axes)
						aggregate += axis.Keys.size();
			const auto dormant = record.find("r");
			if (dormant != record.end() &&
				(record.value("anim", false) || (dormant->is_array() && !dormant->empty())))
				aggregate += dormant->is_array() ? dormant->size() : 1;
			for (size_t axis = 0; axis < 2; ++axis) {
				const Json *saved =
					encoded != record.end() && axis < encoded->size() ? &(*encoded)[axis] : nullptr;
				const size_t count = saved && saved->is_array() ? std::max(size_t{1}, saved->size()) : 1;
				if (aggregate > imagegraph::Limits::MaximumKeyframes ||
					count > imagegraph::Limits::MaximumKeyframes - aggregate) {
					if (budget) budget->Reject();
					return fail("source Vec2 axes and dormant keys exceed aggregate key bounds");
				}
				aggregate += count;
			}

			Vector2 defaults;
			bool needsConstructor = false;
			for (size_t axis = 0; axis < 2; ++axis) {
				const Json *saved =
					encoded != record.end() && axis < encoded->size() ? &(*encoded)[axis] : nullptr;
				needsConstructor |= !saved || (saved->is_array() && saved->empty());
			}
			if (needsConstructor) {
				if (budget && !budget->Hold(256 + 3 * input.Default.size())) return false;
				const auto authoredDefault = imagegraph::CatalogueDefault(input);
				const imagegraph::SourceVec2Default *savedDefault = nullptr;
				if (node.SourceVec2Defaults)
					for (const auto &value : node.SourceVec2Defaults->Inputs)
						if (value.Port == input.Id) savedDefault = &value;
				if (savedDefault)
					defaults = savedDefault->Data;
				else if (node.Type == "pc.mirror_polar" && input.Id == "constant_dimension" &&
						 document.Project)
					defaults = {
						double(document.Project->SurfaceWidth), double(document.Project->SurfaceHeight)
					};
				else if (authoredDefault && std::holds_alternative<Vector2>(*authoredDefault))
					defaults = std::get<Vector2>(*authoredDefault);
				else
					return fail("source Vec2 axis constructor default is not represented");
			}
			if (!node.SourceSeparatedVec2Animators) {
				if (budget && !budget->Hold(sizeof(imagegraph::SourceSeparatedVec2Data))) return false;
				node.SourceSeparatedVec2Animators.emplace();
			}
			auto &inputs = node.SourceSeparatedVec2Animators->Inputs;
			if (!AdmitNativeSlots(inputs, inputs.size() + 1, budget) || !AdmitNativeText(input.Id, budget))
				return false;
			inputs.push_back({std::string(input.Id), {}});
			auto &native = inputs.back();
			native.Separated = record.value("sep_axis", false);
			const auto keyCount = [&]() {
				size_t count = document.Keyframes.size();
				for (const auto &other : document.Nodes)
					if (&other != &node && other.SourceSeparatedVec2Animators)
						for (const auto &stored : other.SourceSeparatedVec2Animators->Inputs)
							for (const auto &axis : stored.Axes)
								count += axis.Keys.size();
				for (const auto &stored : inputs)
					for (const auto &axis : stored.Axes)
						count += axis.Keys.size();
				return count;
			};
			for (size_t axis = 0; axis < 2; ++axis) {
				const Json *saved =
					encoded != record.end() && axis < encoded->size() ? &(*encoded)[axis] : nullptr;
				auto &keys = native.Axes[axis].Keys;
				const size_t count = saved && saved->is_array() ? saved->size() : 1;
				const size_t existing = keyCount();
				if (existing > imagegraph::Limits::MaximumKeyframes ||
					std::max(size_t{1}, count) > imagegraph::Limits::MaximumKeyframes - existing) {
					if (budget) budget->Reject();
					return fail("source Vec2 axes and dormant keys exceed aggregate key bounds");
				}
				if (!saved || (saved->is_array() && saved->empty())) {
					if (!AdmitNativeSlots(keys, 1, budget) || !AdmitNativeText(nodeId, budget) ||
						!AdmitNativeText(input.Id, budget))
						return false;
					keys.push_back(
						{nodeId,
						 std::string(input.Id),
						 0,
						 axis == 0 ? defaults.X : defaults.Y,
						 "source",
						 imagegraph::KeyframeEase{}}
					);
				} else {
					if (!CatalogueKeyframes(
							*saved,
							nodeId,
							input.Id,
							imagegraph::ValueType::Scalar,
							0,
							imagegraph::ValueType::Any,
							keys,
							nullptr,
							nullptr,
							budget
						))
						return fail("source Vec2 separated scalar keys have no exact native mapping");
					const std::string_view prefix = axis == 0 ? "animators/0/" : "animators/1/";
					for (auto &key : keys) {
						if (key.SourceKeyId.size() >
								imagegraph::Limits::MaximumSourceKeyIdBytes - prefix.size() ||
							!AdmitNativeTextSize(prefix.size() + key.SourceKeyId.size(), budget))
							return fail("source Vec2 physical axis key identity exceeds byte bounds");
						key.SourceKeyId.insert(0, prefix);
					}
				}
			}
			document.FormatVersion = 9;
			return true;
		}
		bool CatalogueInputValue(
			const Json &record,
			const imagegraph::CatalogueEntry &entry,
			const imagegraph::CatalogueInput &input,
			const std::string &nodeId,
			Node &node,
			imagegraph::Document &animation,
			std::string &reason,
			bool parseLinkedLocalAnimator = false,
			detail::ImportBudget *budget = nullptr
		) {
			if (!AdmitNativeText(input.Id, budget)) {
				reason = "source input storage exceeds native animator operation bounds";
				return false;
			}
			const std::string id(input.Id);
			if (input.SourceKind == "IVec2") {
				const auto unit = record.find("unit");
				int64_t mode = 0;
				if (unit != record.end() && (!WholeNumber(*unit, mode) || mode != 0)) {
					reason = "source IVec2 nonconstant unit conversion is not represented";
					return false;
				}
			}
			bool separatedVec2 = false;
			// A linked Vec2 path getter reads this consumer's local raw animator X.
			if (input.Type == imagegraph::ValueType::Vector2 && input.SourceIndex >= 0 &&
				(input.SourceKind == "Vec2" || input.SourceKind == "IVec2" ||
				 input.SourceKind == "Dimension" || input.SourceKind == "Range")) {
				parseLinkedLocalAnimator = true;
				const auto constructorDefault = record.find("def_val");
				if (constructorDefault != record.end() && constructorDefault->is_array() &&
					constructorDefault->size() == 2) {
					const auto &value = *constructorDefault;
					if (!value[0].is_number() || !value[1].is_number() ||
						!std::isfinite(value[0].get<double>()) || !std::isfinite(value[1].get<double>())) {
						reason = "source constructor default pair has no finite scalar representation";
						return false;
					}
					if (!node.SourceVec2Defaults) {
						if (budget && !budget->Hold(sizeof(imagegraph::SourceVec2DefaultsData))) {
							reason = "source constructor default storage exceeds native operation bounds";
							return false;
						}
						node.SourceVec2Defaults.emplace();
					}
					auto &defaults = node.SourceVec2Defaults->Inputs;
					if (defaults.size() >= imagegraph::Limits::MaximumArrayElements ||
						!AdmitNativeSlots(defaults, defaults.size() + 1, budget) ||
						!AdmitNativeText(id, budget)) {
						reason = "source constructor default pair exceeds native operation bounds";
						return false;
					}
					defaults.push_back({id, {value[0].get<double>(), value[1].get<double>()}});
					animation.FormatVersion = 9;
				}
				const auto separated = record.find("sep_axis");
				if (separated != record.end() && !separated->is_boolean()) {
					reason = "source Vec2 separated axis flag is malformed";
					return false;
				}
				const auto axes = record.find("animators");
				separatedVec2 = separated != record.end() && separated->get<bool>();
				if (separatedVec2 || (axes != record.end() && (!axes->is_array() || !axes->empty()))) {
					if (!CatalogueVec2Axes(record, input, nodeId, node, animation, reason, budget))
						return false;
				}
			}
			const auto appendValue = [&](std::string_view port, imagegraph::Value data) {
				if (!AdmitNativeSlots(node.Values, node.Values.size() + 1, budget) ||
					!AdmitNativeText(port, budget)) {
					reason = "source authored storage exceeds native animator operation bounds";
					return false;
				}
				node.Values.push_back({std::string(port), std::move(data)});
				return true;
			};
			const auto makePort = [&](std::string_view suffix, std::string &out) {
				if (NativePortName(id, "_", suffix, budget, out)) return true;
				reason = "source generated port storage exceeds native animator operation bounds";
				return false;
			};
			if (record.contains("global_key") || record.contains("global_use")) {
				if ((record.contains("global_key") && !record["global_key"].is_string()) ||
					(record.contains("global_use") && !record["global_use"].is_boolean())) {
					reason = "source expression metadata is malformed";
					return false;
				}
				std::string code = record.value("global_key", std::string{});
				if (node.SourceInputExpressions.size() >=
						imagegraph::Limits::MaximumSourceInputExpressionsPerNode ||
					!AdmitNativeText(code, budget) || !AdmitNativeText(id, budget) ||
					!AdmitNativeSlots(
						node.SourceInputExpressions, node.SourceInputExpressions.size() + 1, budget
					)) {
					reason = "source expression metadata exceeds native budget";
					return false;
				}
				node.SourceInputExpressions.push_back(
					{id, std::move(code), record.value("global_use", false)}
				);
				animation.FormatVersion = 9;
			}
			std::optional<int64_t> quaternionMode;
			if (input.Type == imagegraph::ValueType::Quaternion) {
				const auto attributes = record.find("attri");
				if (attributes != record.end() && !attributes->is_object()) {
					reason = "Quaternion attributes must be an object";
					return false;
				}
				int64_t mode = 1;
				if (const auto *angle = Attribute(record, "angle_display");
					angle && (!WholeNumber(*angle, mode) || mode < 0 || mode > 1)) {
					reason = "Quaternion angle_display must be raw or Euler";
					return false;
				}
				quaternionMode = mode;
			}
			if (entry.Type == "pc.text" && input.Id == "color_by_letter") {
				const auto attributes = record.find("attri");
				if (attributes != record.end() && !attributes->is_object()) {
					reason = "Text palette attributes must be an object";
					return false;
				}
				if (const Json *selection = Attribute(record, "array_select")) {
					int64_t mode = 0;
					const auto *selector = imagegraph::FindCatalogueInput(entry, "color_by_letter_select");
					if (!selector || selector->SourceKind != "SourceArraySelect" || input.SourceIndex != 31 ||
						input.SourceKind != "Palette" || !WholeNumber(*selection, mode) || mode < 0 ||
						mode > 2) {
						reason = "Text palette array_select must be Loop, Ping-pong or Random";
						return false;
					}
					if (!appendValue("color_by_letter_select", imagegraph::EnumValue{mode})) return false;
				}
			}
			if (entry.Type == "pc.export" && input.Id == "framerate") {
				int64_t mode = 1;
				if (const auto *unit = Attribute(record, "unit");
					unit && (!WholeNumber(*unit, mode) || mode < 0 || mode > 1)) {
					reason = "Export framerate unit must be FPS or Relative";
					return false;
				}
				if (!appendValue("framerate_unit", imagegraph::EnumValue{mode})) return false;
			}
			// Node_Audio_Window stores Bit/Second/Progress on Location.attributes.unit, serialized as attri.
			if (entry.Type == "pc.audio_window" && input.Id == "location") {
				const auto attributes = record.find("attri");
				if (attributes != record.end() && !attributes->is_object()) {
					reason = "Audio Window Location attributes must be an object";
					return false;
				}
				if (const Json *unit = Attribute(record, "unit")) {
					int64_t mode = 0;
					if (!WholeNumber(*unit, mode) || mode < 0 || mode > 2) {
						reason = "Audio Window Location unit must be Bit, Second or Progress";
						return false;
					}
					if (!appendValue("location_unit", imagegraph::EnumValue{mode})) return false;
				}
			}
			if (const Json *unit = record.find("unit") != record.end() ? &record["unit"] : nullptr) {
				int64_t mode = 0;
				std::string unitId;
				if (!makePort("unit", unitId)) return false;
				const imagegraph::CatalogueInput *toggle = imagegraph::FindCatalogueInput(entry, unitId);
				if (!(entry.Type == "pc.export" && input.Id == "framerate") && toggle &&
					toggle->SourceKind == "ValueUnit" && WholeNumber(*unit, mode) &&
					(mode == 0 || mode == 1)) {
					if (!appendValue(unitId, imagegraph::EnumValue{mode})) return false;
				}
			}
			if (const Json *project = Attribute(record, "use_project_dimension")) {
				int64_t mode = 0;
				if (!WholeNumber(*project, mode) || mode < 0 || mode > 2) {
					reason = "dimension unit is not recognised";
					return false;
				}
				std::string unitId;
				if (!makePort("unit", unitId)) return false;
				if (imagegraph::FindCatalogueInput(entry, unitId)) {
					if (!appendValue(unitId, imagegraph::EnumValue{mode})) return false;
				}
			}
			if (const Json *flag = Attribute(record, "override_instance")) {
				if (!flag->is_boolean()) {
					reason = "instance override must be a Boolean";
					return false;
				}
				if (flag->get<bool>()) {
					if (!AdmitNativeSlots(
							node.InstanceOverrides, node.InstanceOverrides.size() + 1, budget
						) ||
						!AdmitNativeText(id, budget))
						return false;
					node.InstanceOverrides.push_back(id);
				}
			}
			bool mapped = false;
			if (const Json *flag = Attribute(record, "mapped");
				flag && flag->is_boolean() && flag->get<bool>()) {
				std::string mappedId;
				if (!makePort("mapped", mappedId)) return false;
				if (!imagegraph::FindCatalogueInput(entry, mappedId)) {
					reason = "input is mapped but the catalogue has no map toggle";
					return false;
				}
				if (!appendValue(mappedId, true)) return false;
				mapped = true;
			}
			const auto attributes = record.find("attri");
			if (attributes != record.end() && attributes->is_object()) {
				for (const auto &[key, flag] : attributes->items()) {
					if (!flag.is_boolean() || key == "mapped" || key == "override_instance") continue;
					if (key == "mask_alpha_only") {
						if (imagegraph::FindCatalogueInput(entry, "mask_alpha_only")) {
							if (!appendValue("mask_alpha_only", flag.get<bool>())) return false;
						}
						continue;
					}
					std::string toggleId;
					if (!makePort(key, toggleId)) return false;
					const imagegraph::CatalogueInput *toggle =
						imagegraph::FindCatalogueInput(entry, toggleId);
					if (toggle && toggle->SourceKind == "CurveToggle") {
						if (!appendValue(toggleId, flag.get<bool>())) return false;
					}
				}
			}
			// A linked quaternion still processes the incoming tuple with its own display metadata.
			if (record.contains("from_node") && !parseLinkedLocalAnimator && quaternionMode) {
				if (animation.Tracks.size() >= imagegraph::Limits::MaximumTracks) {
					if (budget) budget->Reject();
					reason = "linked Quaternion metadata exceeds native track count bounds";
					return false;
				}
				if (!CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
					reason = "linked Quaternion getter metadata is not representable";
					return false;
				}
				animation.Tracks.back().QuaternionMode = quaternionMode;
				return true;
			}
			if ((record.contains("from_node") && !parseLinkedLocalAnimator) ||
				(input.Type == imagegraph::ValueType::Image &&
				 !(entry.Type == "pc.hlsl" && input.Id.starts_with("argument_value_"))) ||
				(!imagegraph::IsAuthoredValueType(input.Type) &&
				 !(entry.Type == "pc.hlsl" && input.Id.starts_with("argument_value_")) &&
				 !(entry.Type == "pc.group_input" && input.Id == "parent_value")))
				return true;
			const bool opaqueGradientRange =
				entry.Type == "pc.gradient" && input.Id == "gradient_map_range" && input.SourceIndex == 16 &&
				input.SourceKind == "Vec4" && input.Type == imagegraph::ValueType::Vector4;
			if (opaqueGradientRange && record.contains("anim") && !record.at("anim").is_boolean()) {
				reason = "opaque gradient range animator mode is malformed";
				return false;
			}
			const auto stored = record.find("r");
			if (stored == record.end()) return true;
			// Reloading an empty list leaves valueAnimator's constructor key intact.
			if (parseLinkedLocalAnimator && input.Type == imagegraph::ValueType::Vector2 &&
				entry.Type != "pc.group_input" && entry.Type != "pc.group_output" && stored->is_array() &&
				stored->empty()) {
				if (mapped || !CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
					reason = "empty source Vec2 constructor animator settings are not representable";
					return false;
				}
				return true;
			}
			// Empty source arrays leave the layer-name constructor's frame-zero key intact.
			if ((entry.Type == "pc.ase_layer" || entry.Type == "pc.ora_layer" ||
				 entry.Type == "pc.krita_layer") &&
				input.Id == "layer_name" && input.Type == imagegraph::ValueType::Text &&
				input.SourceKind == "Text" && input.Default == R"(s "")" && stored->is_array() &&
				stored->empty()) {
				if (mapped || !CatalogueTrack(record, nodeId, id, animation, budget)) {
					reason = "empty artwork layer-name constructor settings are not representable";
					return false;
				}
				return appendValue(id, std::string{});
			}
			// Empty scalar HLSL animators return numeric zero in valueAnimator.getValue.
			if (entry.Type == "pc.hlsl" && input.Id.starts_with("argument_value_") &&
				input.Type != imagegraph::ValueType::Array && stored->is_array() && stored->empty()) {
				if (mapped || !CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
					reason = "empty HLSL animator settings are not representable";
					return false;
				}
				return appendValue(id, imagegraph::Value{0.0});
			}
			const size_t choices = imagegraph::CatalogueChoiceCount(input);
			if (record.value("anim", false) || (stored->is_array() && !stored->empty())) {
				if ((entry.Type == "pc.group_input" || entry.Type == "pc.group_output") &&
					stored->is_array() && stored->empty()) {
					if (mapped || !CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
						reason = "empty group animator settings are not representable";
						return false;
					}
					if (quaternionMode) animation.Tracks.back().QuaternionMode = quaternionMode;
					// Source typeArray returns [] for Range and Vec2; scalar controls return zero.
					imagegraph::Value emptyValue = 0.0;
					if (input.SourceKind == "Range" || input.SourceKind == "Vec2")
						emptyValue = imagegraph::ArrayValue{imagegraph::ValueType::Scalar, {}};
					if (!appendValue(id, std::move(emptyValue))) return false;
					return true;
				}
				if (mapped ||
					!CatalogueKeyframes(
						*stored,
						nodeId,
						id,
						input.Type,
						choices,
						ArrayElement(input),
						animation.Keyframes,
						&entry,
						&input,
						budget
					) ||
					!CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
					reason = "animated input " + id + " has no exact native keyframe mapping";
					return false;
				}
				if (quaternionMode) animation.Tracks.back().QuaternionMode = quaternionMode;
				return true;
			}
			if ((entry.Type == "pc.group_input" || entry.Type == "pc.group_output") && stored->is_array() &&
				stored->empty()) {
				if (mapped || !CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
					reason = "static empty group animator settings are not representable";
					return false;
				}
				if (quaternionMode) animation.Tracks.back().QuaternionMode = quaternionMode;
				imagegraph::Value emptyValue;
				if (input.Type == imagegraph::ValueType::Vector2 &&
					(input.SourceKind == "Range" || input.SourceKind == "Vec2"))
					emptyValue = 0.0;
				else if (!CatalogueValue(
							 Json(0), input.Type, choices, emptyValue, ArrayElement(input), budget
						 ) &&
						 !CatalogueRawValue(Json(0), &input, emptyValue)) {
					reason = "static empty group animator zero has no exact native meaning";
					return false;
				}
				if (!appendValue(id, std::move(emptyValue))) return false;
				return true;
			}
			// Triggers and driven keys serialize an unanimated value as a one-key list.
			const bool singleKey = stored->is_array() && stored->size() == 1 && (*stored)[0].is_array() &&
								   (*stored)[0].size() >= 2;
			if (!singleKey && (!stored->is_object() || !stored->contains("d"))) {
				reason = "input value record is not a static value";
				return false;
			}
			const Json &value = singleKey ? (*stored)[0][1] : (*stored)["d"];
			if ((record.contains("on_end") || record.contains("loop_range")) &&
				!CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
				reason = "static compressed animator settings are not representable";
				return false;
			}
			// An unlinked path junction stores the empty surface marker -4; the path stays empty.
			if (input.Type == imagegraph::ValueType::Path2D && value.is_number() && value.get<double>() == -4)
				return true;
			imagegraph::Value converted;
			if (mapped) {
				std::string rangeId;
				if (!makePort("map_range", rangeId)) return false;
				const imagegraph::CatalogueInput *range = imagegraph::FindCatalogueInput(entry, rangeId);
				if (range &&
					CatalogueValue(value, range->Type, 0, converted, imagegraph::ValueType::Scalar, budget)) {
					if (!appendValue(rangeId, std::move(converted))) return false;
					return true;
				}
			}
			if (!CatalogueHlslValue(value, &entry, &input, converted, budget) &&
				!CatalogueRawValue(value, &input, converted, true) &&
				!CatalogueValue(value, input.Type, choices, converted, ArrayElement(input), budget) &&
				!CatalogueSourceChoice(value, &input, converted) &&
				!CatalogueRawValue(value, &input, converted) &&
				!CatalogueEmptyGroupVector(value, &entry, &input, converted) &&
				!CatalogueLiteralArray(value, entry, input, converted, budget)) {
				reason = "input " + id + " value has no exact native meaning";
				return false;
			}
			// A compact source record reloads as one normal frame-zero key even when its getter is static.
			// Keep the fixed projection for authored controls; the key retains the source animator storage.
			const bool gradientStorage = entry.Type == "pc.gradient" && input.Id == "gradient" &&
										 input.SourceIndex == 1 && input.SourceKind == "Gradient" &&
										 input.Type == imagegraph::ValueType::Gradient;
			const bool staticGradientRangeStorage =
				entry.Type == "pc.gradient" && input.Id == "gradient_map_range" && input.SourceIndex == 16 &&
				input.SourceKind == "Vec4" && input.Type == imagegraph::ValueType::Vector4 &&
				record.contains("anim") && record.at("anim").is_boolean() &&
				!record.at("anim").template get<bool>();
			if (!mapped && stored->is_object() && stored->contains("d") &&
				(imagegraph::HasNativeExecutor(entry.Type) || entry.Type == "pc.group_input" ||
				 entry.Type == "pc.group_output" || gradientStorage || staticGradientRangeStorage)) {
				if (animation.Keyframes.size() >= imagegraph::Limits::MaximumKeyframes) {
					if (budget) budget->Reject();
					reason = "static compact source keyframe count exceeds the native limit";
					return false;
				}
				const size_t before = animation.Keyframes.size();
				if (CatalogueKeyframes(
						*stored,
						nodeId,
						id,
						input.Type,
						choices,
						ArrayElement(input),
						animation.Keyframes,
						&entry,
						&input,
						budget
					)) {
					if (!record.contains("on_end") && !record.contains("loop_range") &&
						!CatalogueTrack(record, nodeId, id, animation, budget, separatedVec2)) {
						reason = "static compact animator settings are not representable";
						return false;
					}
					if (quaternionMode) animation.Tracks.back().QuaternionMode = quaternionMode;
				} else {
					animation.Keyframes.resize(before);
					if (budget && budget->Exceeded()) {
						reason = "static compact animator storage exceeds operation bounds";
						return false;
					}
				}
			}
			if (quaternionMode) {
				imagegraph::Quaternion result;
				const auto *tuple = std::get_if<imagegraph::Quaternion>(&converted);
				if (!tuple || !imagegraph::ConvertSourceQuaternion(*tuple, *quaternionMode, result)) {
					reason = "Quaternion source control cannot be converted";
					return false;
				}
				converted = result;
			}
			if (!appendValue(id, std::move(converted))) return false;
			return true;
		}

		bool ProjectGlobals(
			const Json &root, imagegraph::Document &graph, detail::ImportBudget &budget, std::string &failure
		) {
			auto saved = root.find("global");
			if (saved == root.end()) saved = root.find("global_node");
			if (saved == root.end()) return true;
			if (!saved->is_object() || !saved->contains("inputs") || !(*saved)["inputs"].is_array()) {
				failure = "PXC global inputs are malformed";
				return false;
			}
			const auto &inputs = (*saved)["inputs"];
			if (inputs.empty()) return true;
			if (inputs.size() > imagegraph::MaximumDynamicInputsForType("pc.global_scope")) {
				failure = "PXC global input count exceeds bounds";
				return false;
			}
			Node node;
			node.Id = "$project-global";
			node.Type = "pc.global_scope";
			while (std::any_of(graph.Nodes.begin(), graph.Nodes.end(), [&](const Node &existing) {
				return existing.Id == node.Id;
			}))
				node.Id += "_";
			if (!AdmitNativeText(node.Id, &budget) ||
				!AdmitNativeSlots(graph.Nodes, graph.Nodes.size() + 1, &budget)) {
				failure = "PXC global node exceeds import budget";
				return false;
			}
			imagegraph::CatalogueEntry entry{};
			entry.Type = "pc.global_scope";
			for (const auto &record : inputs) {
				if (!record.is_object() || !record.contains("global_name") ||
					!record["global_name"].is_string()) {
					failure = "PXC global name is missing";
					return false;
				}
				std::string name = record["global_name"].get<std::string>();
				int64_t type = 0, display = 0;
				if (name.empty() || !AdmitNativeText(name, &budget) ||
					std::any_of(
						node.DynamicInputs.begin(),
						node.DynamicInputs.end(),
						[&](const auto &v) { return v.Id == name; }
					) ||
					(record.contains("global_type") && !WholeNumber(record["global_type"], type)) ||
					(record.contains("global_disp") && !WholeNumber(record["global_disp"], display)) ||
					type < 0 || type > 7 || display < 0 || display > 12) {
					failure = "PXC global type, display or name is invalid";
					return false;
				}
				static constexpr imagegraph::ValueType types[]{
					imagegraph::ValueType::Integer,
					imagegraph::ValueType::Scalar,
					imagegraph::ValueType::Boolean,
					imagegraph::ValueType::Colour,
					imagegraph::ValueType::Gradient,
					imagegraph::ValueType::Text,
					imagegraph::ValueType::Curve,
					imagegraph::ValueType::Text
				};
				imagegraph::CatalogueInput input{};
				input.Id = name;
				input.Type = types[type];
				input.SourceKind = "Global";
				if (type <= 1) {
					if (display == 7) {
						input.Type = imagegraph::ValueType::Vector2;
						input.SourceKind = "Vec2";
					} else if (display == 8) {
						input.Type = imagegraph::ValueType::Vector3;
						input.SourceKind = "Vec3";
					} else if (display == 9) {
						input.Type = imagegraph::ValueType::Vector4;
						input.SourceKind = "Vec4";
					} else if (display == 1 || display == 3 || display == 5 || display == 10) {
						input.Type = imagegraph::ValueType::Vector2;
						input.SourceKind = "Range";
					} else if (display == 6 || display == 11) {
						input.Type = imagegraph::ValueType::Vector4;
						input.SourceKind = "Vec4";
					} else if (display == 12)
						input.Type = imagegraph::ValueType::Array;
				}
				if (type == 3 && display == 1) {
					input.Type = imagegraph::ValueType::Array;
					input.SourceKind = "Palette";
				}
				if (((type == 2 || type == 4 || type == 6 || type == 7) && display != 0) ||
					((type == 3 || type == 5) && display > 1)) {
					failure = "PXC global display index is invalid for its type";
					return false;
				}
				if (record.contains("anim") && !record["anim"].is_boolean()) {
					failure = "PXC global animator flag is malformed";
					return false;
				}
				auto &modes =
					record.value("anim", false) ? node.SourceAnimatedInputs : node.SourceStaticInputs;
				if (!AdmitNativeSlots(modes, modes.size() + 1, &budget) || !AdmitNativeText(name, &budget)) {
					failure = "PXC global animator modes exceed budget";
					return false;
				}
				modes.push_back(name);
				const size_t before = node.Values.size();
				if (!CatalogueInputValue(record, entry, input, node.Id, node, graph, failure, false, &budget))
					return false;
				imagegraph::DynamicInput dynamic{name, input.Type, std::nullopt};
				for (size_t v = before; v < node.Values.size(); ++v) {
					if (node.Values[v].Port != name) {
						failure = "PXC global attributes have no exact mapping";
						return false;
					}
					dynamic.Default = std::move(node.Values[v].Data);
				}
				node.Values.resize(before);
				if (!AdmitNativeSlots(node.DynamicInputs, node.DynamicInputs.size() + 1, &budget)) {
					failure = "PXC global inputs exceed budget";
					return false;
				}
				node.DynamicInputs.push_back(std::move(dynamic));
			}
			graph.ProjectGlobalNodeId = node.Id;
			graph.FormatVersion = 9;
			graph.Nodes.push_back(std::move(node));
			return true;
		}
		// Projects a node with a source catalogue entry, keeping every source input index and value.
		std::optional<int64_t> StaticHlslChoice(const Json &record) {
			if (record.contains("from_node") || record.value("anim", false) ||
				record.value("global_use", false))
				return std::nullopt;
			const auto stored = record.find("r");
			if (stored == record.end() || (stored->is_array() && stored->empty())) return int64_t{0};
			if (!stored->is_object() || !stored->contains("d")) return std::nullopt;
			int64_t choice = 0;
			return WholeNumber((*stored)["d"], choice) && detail::HlslArgumentType(choice)
					   ? std::optional{choice}
					   : std::nullopt;
		}

#include "SourceCommonProjection.inc"

		bool CatalogueNode(
			const Json &source,
			const imagegraph::CatalogueEntry &entry,
			const std::string &nodeId,
			Node &node,
			imagegraph::Document &animation,
			std::string &reason,
			detail::ImportBudget *budget = nullptr
		) {
			node.Type = std::string(entry.Type);
			node.Values.clear();
			const auto inputs = source.find("inputs");
			if (inputs == source.end() || !inputs->is_array() || inputs->size() > size_t(INT32_MAX)) {
				reason = "node has no input records";
				return false;
			}
			for (size_t index = 0; index < inputs->size(); index++) {
				const Json &record = (*inputs)[index];
				if (!record.is_object()) continue;
				const int32_t sourceIndex = static_cast<int32_t>(index);
				if (const imagegraph::CatalogueInput *input =
						imagegraph::FindCatalogueInputIndex(entry, sourceIndex)) {
					if (!CatalogueInputValue(
							record, entry, *input, nodeId, node, animation, reason, false, budget
						))
						return false;
					continue;
				}
				// Collection custom sockets are projected into child boundary junctions below.
				if (entry.SourceNode == "Node_Pixel_Builder" && sourceIndex >= 4) {
					const auto attributes = source.find("attri");
					if (attributes != source.end() && attributes->is_object() &&
						attributes->contains("custom_input_list")) {
						const auto &sockets = attributes->at("custom_input_list");
						if (sockets.is_array() && sockets.size() <= imagegraph::Limits::MaximumGroupPorts &&
							index - 4 < sockets.size())
							continue;
					}
				}
				if (entry.DynamicGroupLength <= 0 || sourceIndex < entry.DynamicFixedLength) {
					reason =
						"input index " + std::to_string(index) + " is not in the pinned source catalogue";
					return false;
				}
				const int32_t offset = (sourceIndex - entry.DynamicFixedLength) % entry.DynamicGroupLength;
				const int32_t group = (sourceIndex - entry.DynamicFixedLength) / entry.DynamicGroupLength;
				const auto templateInput = std::find_if(
					entry.DynamicTemplate.begin(), entry.DynamicTemplate.end(), [&](const auto &candidate) {
						return candidate.SourceIndex == offset;
					}
				);
				if (templateInput == entry.DynamicTemplate.end()) {
					reason =
						"dynamic input offset " + std::to_string(offset) + " is not in the source template";
					return false;
				}
				if (node.DynamicInputs.size() >= imagegraph::MaximumDynamicInputsForType(entry.Type)) {
					reason = "dynamic inputs exceed the native limit";
					return false;
				}
				const std::string suffix = std::to_string(group);
				const size_t idSize = templateInput->Id.size() + 1 + suffix.size();
				if (!AdmitNativeSlots(node.DynamicInputs, node.DynamicInputs.size() + 1, budget) ||
					!AdmitNativeTextSize(idSize, budget)) {
					reason = "dynamic input storage exceeds native animator operation bounds";
					return false;
				}
				std::string dynamicId(idSize, '\0');
				std::copy(templateInput->Id.begin(), templateInput->Id.end(), dynamicId.begin());
				dynamicId[templateInput->Id.size()] = '_';
				std::copy(suffix.begin(), suffix.end(), dynamicId.begin() + templateInput->Id.size() + 1);
				imagegraph::DynamicInput dynamic{std::move(dynamicId), templateInput->Type, std::nullopt};
				if (entry.Type == "pc.hlsl") {
					const std::string origin = "pxc:input:" + std::to_string(index);
					if (!AdmitNativeText(origin, budget)) {
						reason = "HLSL source input origin exceeds native operation bounds";
						return false;
					}
					dynamic.SourceInputId = origin;
					animation.FormatVersion = 9;
				}

				std::optional<int64_t> hlslMode;
				if (entry.Type == "pc.hlsl" && templateInput->Id == "argument_value") {
					const Json *selectorRecord = Input(source, index - 1);
					const auto choice = selectorRecord ? StaticHlslChoice(*selectorRecord) : std::nullopt;
					const auto type = choice ? detail::HlslArgumentType(*choice) : std::nullopt;
					if (!type) {
						reason = "HLSL argument typing requires a proven static literal selector";
						return false;
					}

					hlslMode = *choice;
					dynamic.Type = *type;
				}

				if ((entry.Type == "pc.lua_compute" || entry.Type == "pc.lua_surface") &&
					templateInput->Id == "argument_value") {
					int64_t selection = 0;
					const auto selector = std::find_if(
						node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &socket) {
							return socket.Id == "argument_type_" + suffix;
						}
					);
					if (selector != node.DynamicInputs.end() && selector->Default) {
						const auto *choice = std::get_if<imagegraph::EnumValue>(&*selector->Default);
						if (!choice || choice->Value < 0 || choice->Value > 3) {
							reason = "Lua argument type is invalid";
							return false;
						}
						selection = choice->Value;
					}
					static constexpr imagegraph::ValueType types[]{
						imagegraph::ValueType::Scalar,
						imagegraph::ValueType::Text,
						imagegraph::ValueType::Image,
						imagegraph::ValueType::Struct
					};
					dynamic.Type = types[selection];
				}
				if (hlslMode) {
					if (const auto count = detail::HlslArgumentLength(*hlslMode)) {
						imagegraph::ArrayValue value{imagegraph::ValueType::Integer, {}};
						if (!AdmitNativeSlots(value.Elements, count, budget)) {
							reason = "HLSL default exceeds import bounds";
							return false;
						}
						value.Elements.assign(count, int64_t{0});
						dynamic.Default = std::move(value);
					} else
						dynamic.Default = *hlslMode == 0
											  ? imagegraph::Value{0.0}
											  : imagegraph::Value{int64_t{*hlslMode == 7 ? -4 : 0}};
				} else if (imagegraph::IsAuthoredValueType(dynamic.Type)) {
					if (dynamic.Type == templateInput->Type)
						dynamic.Default = imagegraph::CatalogueDefault(*templateInput);
					else if (dynamic.Type == imagegraph::ValueType::Text)
						dynamic.Default = std::string{};
				}
				auto represented = *templateInput;
				represented.Id = dynamic.Id;
				represented.Type = dynamic.Type;
				if (entry.Type == "pc.mesh_warp" && templateInput->SourceKind == "Puppet")
					represented.Type = imagegraph::ValueType::Array;
				const size_t valuesBefore = node.Values.size();
				if (!CatalogueInputValue(
						record,
						entry,
						represented,
						nodeId,
						node,
						animation,
						reason,
						hlslMode.has_value(),
						budget
					))
					return false;
				for (size_t value = valuesBefore; value < node.Values.size(); ++value) {
					if (node.Values[value].Port != dynamic.Id) {
						reason = "dynamic input attributes have no exact native meaning";
						return false;
					}
					dynamic.Default = std::move(node.Values[value].Data);
				}
				// Source absent Path sockets are noone, not authored empty path objects.
				if (entry.Type == "pc.path_bridge" && templateInput->SourceKind == "Path" &&
					node.Values.size() == valuesBefore)
					dynamic.Default.reset();
				if (hlslMode &&
					((dynamic.Default && !detail::HlslArgumentValue(*hlslMode, *dynamic.Default)) ||
					 std::any_of(
						 animation.Keyframes.begin(), animation.Keyframes.end(), [&](const auto &key) {
							 return key.NodeId == nodeId && key.Port == dynamic.Id &&
									!detail::HlslArgumentValue(*hlslMode, key.Data);
						 }
					 ))) {
					reason = "HLSL argument value does not match its source selector shape";
					return false;
				}
				if (entry.Type == "pc.mesh_warp" && templateInput->SourceKind == "Puppet") {
					const auto valid = [](const imagegraph::Value &value) {
						const auto *array = std::get_if<imagegraph::ArrayValue>(&value);
						return array && array->ElementType == imagegraph::ValueType::Scalar &&
							   array->Elements.size() == 7 && array->Nested.empty() && array->Items.empty() &&
							   std::all_of(
								   array->Elements.begin(), array->Elements.end(), [](const auto &item) {
									   const auto *number = std::get_if<double>(&item);
									   return number && std::isfinite(*number);
								   }
							   );
					};
					if ((dynamic.Default && !valid(*dynamic.Default)) ||
						std::any_of(
							animation.Keyframes.begin(), animation.Keyframes.end(), [&](const auto &key) {
								return key.NodeId == nodeId && key.Port == dynamic.Id && !valid(key.Data);
							}
						)) {
						reason = "Puppet controls require seven finite scalar values";
						return false;
					}
				}
				const auto attributes = record.find("attri");
				if (attributes != record.end() && attributes->is_object() &&
					attributes->contains("layerName")) {
					const auto &name = (*attributes)["layerName"];
					if (!name.is_string() || !AdmitNativeText(name.get_ref<const std::string &>(), budget)) {
						reason = "source room layer binding exceeds native text bounds";
						return false;
					}
					dynamic.SourceLayerName = name.get<std::string>();
					if (!dynamic.SourceLayerName.empty()) animation.FormatVersion = 9;
				}
				node.Values.resize(valuesBefore);
				node.DynamicInputs.push_back(std::move(dynamic));
			}
			if (entry.Type == "pc.tile_tileset" || entry.Type == "pc.tile_rule" ||
				entry.Type == "pc.tile_convert") {
				for (std::string_view port :
					 {"animatedTiles", "autoterrain", "ruleTiles", "colorList", "colorMap"}) {
					const bool relevant =
						entry.Type == "pc.tile_tileset"
							? port == "animatedTiles" || port == "autoterrain" || port == "ruleTiles"
						: entry.Type == "pc.tile_rule" ? port == "ruleTiles"
													   : port == "colorList" || port == "colorMap";
					if (!relevant) continue;
					if (auto raw = Attribute(source, port)) {
						imagegraph::Value value;
						const bool decoded = port == "colorMap"
												 ? detail::DecodeTileColorMap(*raw, value, budget)
												 : detail::DecodeTileProperty(*raw, value, budget);
						if (!decoded ||
							!AdmitNativeSlots(
								node.SourceProperties, node.SourceProperties.size() + 1, budget
							) ||
							!AdmitNativeText(port, budget)) {
							reason = "tile source properties exceed their native mapping or budget";
							return false;
						}
						node.SourceProperties.push_back({std::string(port), std::move(value)});
						animation.FormatVersion = 9;
					}
				}
			}
			if (entry.Type == "pc.cache" || entry.Type == "pc.cache_array") {
				for (const std::string_view port : {"serialize", "cache_group", "cache"}) {
					std::optional<Json> raw;
					if (port == "cache") {
						if (source.contains("cache")) raw = source["cache"];
					} else if (const auto *attribute = Attribute(source, port))
						raw = *attribute;
					if (!raw) continue;
					imagegraph::Value value;
					bool decoded = false;
					if (port == "serialize" && raw->is_boolean()) {
						value = raw->get<bool>();
						decoded = true;
					} else if (port == "cache" && raw->is_string()) {
						const auto &text = raw->get_ref<const std::string &>();
						if (AdmitNativeText(text, budget)) {
							value = text;
							decoded = true;
						}
					} else if (port == "cache_group" && raw->is_array() &&
							   raw->size() <= imagegraph::Limits::MaximumNodes) {
						imagegraph::ArrayValue ids{imagegraph::ValueType::Text, {}};
						decoded = AdmitNativeSlots(ids.Elements, raw->size(), budget);
						for (const auto &id : *raw) {
							if (!decoded || !id.is_string() ||
								!AdmitNativeText(id.get_ref<const std::string &>(), budget)) {
								decoded = false;
								break;
							}
							ids.Elements.push_back(id.get<std::string>());
						}
						if (decoded) value = std::move(ids);
					}
					if (!decoded ||
						!AdmitNativeSlots(node.SourceProperties, node.SourceProperties.size() + 1, budget) ||
						!AdmitNativeText(port, budget)) {
						reason = "source frame-cache metadata exceeds its typed mapping or budget";
						return false;
					}
					node.SourceProperties.push_back({std::string(port), std::move(value)});
					animation.FormatVersion = 9;
				}
			}
			if (entry.Type == "pc.wav_file_read")
				if (auto raw = Attribute(source, "file_checker")) {
					if (!raw->is_boolean() ||
						!AdmitNativeSlots(node.SourceProperties, node.SourceProperties.size() + 1, budget) ||
						!AdmitNativeText("file_checker", budget)) {
						reason = "WAV File Watcher must be a bounded source boolean";
						return false;
					}
					node.SourceProperties.push_back({"file_checker", raw->get<bool>()});
					animation.FormatVersion = 9;
				}
			if (entry.Type == "pc.mesh_warp") {
				for (std::string_view port : {"pin", "mesh_bound"})
					if (auto raw = Attribute(source, port)) {
						imagegraph::Value value;
						if (!CatalogueValue(
								*raw,
								imagegraph::ValueType::Array,
								0,
								value,
								port == "pin" ? imagegraph::ValueType::Integer
											  : imagegraph::ValueType::Vector2,
								budget
							) ||
							!AdmitNativeSlots(
								node.SourceProperties, node.SourceProperties.size() + 1, budget
							) ||
							!AdmitNativeText(port, budget)) {
							reason = "Mesh Warp source properties exceed native mapping or budget";
							return false;
						}
						node.SourceProperties.push_back({std::string(port), std::move(value)});
						animation.FormatVersion = 9;
					}
			}
			const auto attributes = source.find("attri");
			if (entry.SourceNode == "Node_Array_Split" && attributes != source.end() &&
				attributes->is_object() && attributes->contains("output_amount")) {
				int64_t count = 0;
				if (!WholeNumber((*attributes)["output_amount"], count) || count < 0 ||
					count > int64_t(imagegraph::Limits::MaximumDynamicOutputsPerNode) + 1) {
					reason = "Array Split output_amount exceeds native output bounds";
					return false;
				}
				for (int64_t index = 1; index < count; ++index) {
					std::string id = "val_" + std::to_string(index);
					if (!AdmitNativeSlots(node.DynamicOutputs, node.DynamicOutputs.size() + 1, budget) ||
						!AdmitNativeTextSize(id.size(), budget)) {
						reason = "Array Split outputs exceed operation bounds";
						return false;
					}
					node.DynamicOutputs.push_back({std::move(id), imagegraph::ValueType::Any});
				}
			}
			if (attributes != source.end() && attributes->is_object()) {
				for (const std::string_view key : {"interpolate", "oversample"}) {
					const auto found = attributes->find(key);
					const imagegraph::CatalogueInput *input = imagegraph::FindCatalogueInput(entry, key);
					if (found == attributes->end() || !input) continue;
					imagegraph::Value converted;
					if (!CatalogueValue(
							*found,
							imagegraph::ValueType::Enum,
							imagegraph::CatalogueChoiceCount(*input),
							converted
						)) {
						reason = std::string(key) + " attribute is out of range";
						return false;
					}
					node.Values.push_back({std::string(key), std::move(converted)});
				}
				if (!attributes->contains("color_depth") &&
					imagegraph::FindCatalogueInput(entry, "attribute_color_depth"))
					node.Values.push_back({"attribute_color_depth", imagegraph::EnumValue{3}});
				if (entry.Type == "pc.ase_file_read" || entry.Type == "pc.camera") {
					if (const auto visible = attributes->find("layer_visible");
						visible != attributes->end()) {
						if (entry.Type == "pc.camera" &&
							(!visible->is_array() || visible->size() != node.DynamicInputs.size() / 6 ||
							 std::any_of(visible->begin(), visible->end(), [](const auto &item) {
								 return !item.is_boolean();
							 }))) {
							reason = "Camera visibility requires one boolean per source layer";
							return false;
						}
						imagegraph::Value value;
						if (!CatalogueValue(
								*visible,
								imagegraph::ValueType::Array,
								0,
								value,
								imagegraph::ValueType::Boolean,
								budget
							) ||
							!AdmitNativeSlots(
								node.SourceProperties, node.SourceProperties.size() + 1, budget
							) ||
							!AdmitNativeTextSize(13, budget)) {
							reason = "Layer visibility metadata exceeds native boolean array bounds";
							return false;
						}
						node.SourceProperties.push_back({"layer_visible", std::move(value)});
						animation.FormatVersion = 9;
					}
				}
				for (const auto &[key, stored] : attributes->items()) {
					if (entry.Type == "pc.mesh_warp" && (key == "pin" || key == "mesh_bound")) continue;
					const std::string id = "attribute_" + key;
					const imagegraph::CatalogueInput *input = imagegraph::FindCatalogueInput(entry, id);
					if (!input) continue;
					imagegraph::Value converted;
					if (!(entry.Type == "pc.ase_file_read" && key == "layer_loop"
							  ? CatalogueValue(
									stored,
									imagegraph::ValueType::Array,
									0,
									converted,
									imagegraph::ValueType::Boolean,
									budget
								)
							  : CatalogueValue(
									stored, input->Type, imagegraph::CatalogueChoiceCount(*input), converted
								)) &&
						!AttributeList(stored, input->Type, converted)) {
						reason = "node attribute " + key + " has no exact native meaning";
						return false;
					}
					node.Values.push_back({id, std::move(converted)});
				}
			}
			if (detail::SourceImageCacheType(node.Type)) {
				detail::ImageCacheAttributes cache;
				if (!detail::ReadImageCacheAnnotation(source, node.Type, cache, reason)) return false;
				const auto append = [&](std::string_view port,
										std::optional<std::string_view> text,
										std::optional<bool> boolean = std::nullopt) {
					if (node.SourceProperties.size() >= imagegraph::Limits::MaximumPropertiesPerNode ||
						!AdmitNativeSlots(node.SourceProperties, node.SourceProperties.size() + 1, budget) ||
						!AdmitNativeText(port, budget) || (text && !AdmitNativeText(*text, budget)))
						return false;
					node.SourceProperties.push_back(
						{std::string(port),
						 text ? imagegraph::Value{std::string(*text)} : imagegraph::Value{*boolean}}
					);
					return true;
				};
				if ((cache.Enabled && !append(detail::ImageCacheUse, std::nullopt, *cache.Enabled)) ||
					(cache.Data && !append(detail::ImageCacheData, *cache.Data)) ||
					(cache.Layout &&
					 !append(detail::ImageCacheLayout, bake::SpriteCacheLayoutName(*cache.Layout))) ||
					(cache.Hash && !append(detail::ImageCacheHash, *cache.Hash))) {
					reason = "image sprite cache properties exceed native import bounds";
					return false;
				}
				if (cache.Enabled || cache.Data || cache.Layout) animation.FormatVersion = 9;
			}
			return true;
		}

		// Constructor defaults and stripped saved attributes are different source states.
		std::optional<int64_t> SourceDepthChoice(const Json &source) {
			const auto attributes = source.find("attri");
			if (attributes != source.end()) {
				if (!attributes->is_object()) return std::nullopt;
				const auto depth = attributes->find("color_depth");
				int64_t choice = 3;
				if (depth != attributes->end() && !WholeNumber(*depth, choice)) return std::nullopt;
				return choice >= 0 && choice <= 8 ? std::optional{choice} : std::nullopt;
			}
			if (detail::IsOrdinarySourceGroup(source.at("type").get_ref<const std::string &>())) return 1;
			const auto *entry =
				imagegraph::FindCatalogueSource(source.at("type").get_ref<const std::string &>());
			const auto *depth =
				entry ? imagegraph::FindCatalogueInput(*entry, "attribute_color_depth") : nullptr;
			const auto value = depth ? imagegraph::CatalogueDefault(*depth) : std::nullopt;
			const auto *choice = value ? std::get_if<imagegraph::EnumValue>(&*value) : nullptr;
			return choice ? std::optional{choice->Value} : std::nullopt;
		}

		struct SourceDepthResolver {
			const Json &Root;
			std::unordered_map<std::string_view, const Json *> Sources;
			std::unordered_map<const Json *, std::optional<int64_t>> Cache;
			std::vector<const Json *> Path;

			explicit SourceDepthResolver(const Json &root) : Root(root) {
				const auto &nodes = Root.at("nodes");
				Sources.reserve(nodes.size());
				Cache.reserve(nodes.size());
				Path.reserve(nodes.size() * 3 + 1);
				for (const Json &source : nodes)
					Sources.emplace(source.at("id").get_ref<const std::string &>(), &source);
			}
			const Json *Find(const Json &id) const {
				if (!id.is_string()) return nullptr;
				const auto found = Sources.find(id.get_ref<const std::string &>());
				return found == Sources.end() ? nullptr : found->second;
			}
			std::optional<int64_t> Project() const {
				int64_t depth = 1;
				const auto attributes = Root.find("attributes");
				if (attributes != Root.end() && attributes->is_object() &&
					attributes->contains("color_depth") && !WholeNumber(attributes->at("color_depth"), depth))
					return std::nullopt;
				return depth >= 0 && depth <= 6 ? std::optional{depth + 2} : std::nullopt;
			}
			const Json *Parent(const Json &source) const {
				const auto parent = source.find("group");
				return parent == source.end() ? nullptr : Find(*parent);
			}
			bool HasParent(const Json &source) const {
				const auto parent = source.find("group");
				return parent != source.end() && !(*parent == -1) && !(*parent == "");
			}
			// Legacy RGBA8 executors also require the represented inherited allocation context.
			bool LegacyContextRgba8(const Json &source) const {
				const Json *current = &source;
				for (size_t steps = 0; steps <= Sources.size(); ++steps) {
					if (!HasParent(*current)) return Project() == 3;
					current = Parent(*current);
					if (!current ||
						!detail::IsOrdinarySourceGroup(current->at("type").get_ref<const std::string &>()) ||
						current->contains("instanceBase"))
						return false;
					const auto choice = SourceDepthChoice(*current);
					if (!choice || *choice == 0) return false;
					if (*choice != 1) return *choice == 3;
				}
				return false;
			}
			bool LegacyInputsRgba8(const Json &source, const imagegraph::CatalogueEntry &entry) {
				for (const auto &input : entry.Inputs) {
					if (input.Type != imagegraph::ValueType::Image || input.SourceIndex < 0) continue;
					const Json *record = Input(source, static_cast<size_t>(input.SourceIndex));
					if (!record || !record->contains("from_node")) continue;
					const Json *producer = Find(record->at("from_node"));
					if (!producer) return false;
					// Opaque producers cannot execute. Explicit subgraph cuts retain optional-input
					// replacement.
					if (!imagegraph::FindCatalogueSource(
							producer->at("type").get_ref<const std::string &>()
						) &&
						!detail::IsOrdinarySourceGroup(producer->at("type").get_ref<const std::string &>()))
						continue;

					int64_t index = 0;
					if (!producer ||
						(record->contains("from_index") && !WholeNumber(record->at("from_index"), index)) ||
						!OrdinarySourceOutputTag(*record) || index < 0 || index > UINT32_MAX ||
						Resolve(*producer, true, static_cast<uint32_t>(index)) != 3)
						return false;
				}
				return true;
			}
			std::optional<int64_t>
			Resolve(const Json &source, bool output = false, uint32_t outputIndex = 0) {
				Path.clear();
				const Json *current = &source;
				std::optional<int64_t> result;
				const auto followInput = [&](const Json &node, size_t index) {
					const Json *record = Input(node, index);
					if (!record || record->contains("anim") || record->contains("global_key") ||
						record->contains("global_use"))
						return false;
					const auto from = record->find("from_node");
					if (from == record->end()) {
						const Json *value = Current(node, index);
						while (value && value->is_array() && !value->empty())
							value = &(*value)[0];
						if (value && ((value->is_array() && value->empty()) || (*value == -4))) result = 3;
						return false;
					}
					int64_t indexValue = 0;
					if ((record->contains("from_index") &&
						 !WholeNumber(record->at("from_index"), indexValue)) ||
						!OrdinarySourceOutputTag(*record) || indexValue < 0 || indexValue > UINT32_MAX)
						return false;
					current = Find(*from);
					outputIndex = static_cast<uint32_t>(indexValue);
					output = true;
					return current != nullptr;
				};
				for (size_t steps = 0; current && steps <= Sources.size() * 3; ++steps) {
					if (current->contains("instanceBase")) {
						current = Find(current->at("instanceBase"));
						continue;
					}
					const auto &type = current->at("type").get_ref<const std::string &>();
					if (output && (detail::IsOrdinarySourceGroup(type) ||
								   (type == "Node_Pixel_Builder" && outputIndex >= 2))) {
						const auto attributes = current->find("attri");
						if (attributes == current->end() || !attributes->is_object() ||
							!attributes->contains("custom_output_list"))
							break;
						const auto &list = attributes->at("custom_output_list");
						if (type == "Node_Pixel_Builder") outputIndex -= 2;
						if (!list.is_array() || outputIndex >= list.size()) break;
						current = Find(list[outputIndex]);
						outputIndex = 0;
						continue;
					}
					if (output && type == "Node_Group_Output") {
						if (outputIndex != 0 || !followInput(*current, 0)) break;
						continue;
					}
					if (output && type == "Node_Group_Input") {
						const Json *parent = Parent(*current);
						if (!parent || outputIndex != 0) break;
						const auto attributes = parent->find("attri");
						if (attributes == parent->end() || !attributes->is_object() ||
							!attributes->contains("custom_input_list"))
							break;
						const auto &list = attributes->at("custom_input_list");
						if (!list.is_array()) break;
						const auto found = std::find(list.begin(), list.end(), current->at("id"));
						if (found == list.end() ||
							!followInput(
								*parent,
								static_cast<size_t>(found - list.begin()) +
									(parent->at("type") == "Node_Pixel_Builder" ? 4 : 0)
							))
							break;
						continue;
					}
					const auto *entry = imagegraph::FindCatalogueSource(type);
					const auto *depth =
						entry ? imagegraph::FindCatalogueInput(*entry, "attribute_color_depth") : nullptr;
					if (output) {
						if (!entry || !depth || outputIndex != 0 ||
							std::none_of(entry->Outputs.begin(), entry->Outputs.end(), [](const auto &port) {
								return port.SourceIndex == 0 && port.Type == imagegraph::ValueType::Image;
							}))
							break;
						output = false;
					}
					if (!depth && !detail::IsOrdinarySourceGroup(type)) break;
					if (const auto cached = Cache.find(current); cached != Cache.end()) {
						result = cached->second;
						break;
					}
					Path.push_back(current);
					const auto choice = SourceDepthChoice(*current);
					if (!choice) break;
					if (*choice >= 2) {
						result = choice;
						break;
					}
					if (*choice == 0) {
						if (!followInput(*current, 0)) break;
						continue;
					}
					if (!HasParent(*current)) {
						result = Project();
						break;
					}
					current = Parent(*current);
					if (!current ||
						!detail::IsOrdinarySourceGroup(current->at("type").get_ref<const std::string &>()))
						break;
				}
				for (const Json *node : Path)
					Cache.emplace(node, result);
				return result;
			}
		};

		// Instance children retain their original source socket order and class. A legacy image.*
		// shortcut is local to one saved node and cannot supply a different instance's interface.
		using GroupSourceSet =
			std::set<std::string_view, std::less<>, detail::ImportAllocator<std::string_view>>;
		bool CanonicalInstanceGroups(const Json &root, GroupSourceSet &groups, std::string &failure) {
			const auto &nodes = root.at("nodes");
			for (const auto &node : nodes) {
				if (node.at("type") != "Node_Group") continue;
				const auto instance = node.find("instanceBase");
				if (instance == node.end() ||
					(instance->is_string() && instance->get_ref<const std::string &>().empty()))
					continue;
				if (!instance->is_string())
					return Fail(failure, "source group instance base is not a durable name");
				groups.insert(node.at("id").get_ref<const std::string &>());
				groups.insert(instance->get_ref<const std::string &>());
			}
			for (size_t pass = 0; pass <= nodes.size(); ++pass) {
				const size_t count = groups.size();
				for (const auto &node : nodes) {
					if (node.at("type") != "Node_Group") continue;
					const auto &id = node.at("id").get_ref<const std::string &>();
					const auto parent = node.find("group");
					if (parent != node.end() && parent->is_string() &&
						groups.contains(parent->get_ref<const std::string &>()))
						groups.insert(id);
					if (!groups.contains(id)) continue;
					const auto instance = node.find("instanceBase");
					if (instance != node.end() && instance->is_string() &&
						!instance->get_ref<const std::string &>().empty())
						groups.insert(instance->get_ref<const std::string &>());
				}
				if (groups.size() == count) return true;
			}
			return Fail(failure, "source group instance closure exceeds its bounded membership count");
		}

		// Group sockets are ordered by saved child IDs, independently of their display ordering.
		bool ProjectOrdinaryGroups(
			const Json &root,
			PxcxImport &result,
			const GroupSourceSet &canonicalGroups,
			std::string &failure,
			uint64_t previousDocumentBytes,
			detail::ImportBudget &operationBudget,
			bool admitCommon
		) {
			using imagegraph::Group;
			using imagegraph::Junction;
			using imagegraph::PortDirection;
			using imagegraph::ValueType;
			struct Boundary {
				std::string GroupId;
				PortDirection Direction;
				size_t Index;
				std::string JunctionId;
			};
			const auto &sourceNodes = root.at("nodes");
			const auto holdText = [&](std::string_view text) {
				return operationBudget.Hold(std::max<size_t>(text.size(), 15) + 1);
			};
			using SourcePair = std::pair<const std::string_view, const Json *>;
			using GroupPair = std::pair<const std::string_view, size_t>;
			using BoundaryPair = std::pair<const std::string_view, Boundary>;
			// Keys borrow the immutable archive JSON. Actual tree nodes and Boundary names are charged.
			std::map<std::string_view, const Json *, std::less<>, detail::ImportAllocator<SourcePair>>
				sources(std::less<>{}, detail::ImportAllocator<SourcePair>{operationBudget});
			std::map<std::string_view, size_t, std::less<>, detail::ImportAllocator<GroupPair>> groups(
				std::less<>{}, detail::ImportAllocator<GroupPair>{operationBudget}
			);
			std::map<std::string_view, Boundary, std::less<>, detail::ImportAllocator<BoundaryPair>>
				boundaries(std::less<>{}, detail::ImportAllocator<BoundaryPair>{operationBudget});
			try {
				for (const Json &source : sourceNodes)
					sources.emplace(source.at("id").get_ref<const std::string &>(), &source);
				const auto parentOf = [&](const Json &source, std::string &parent) {
					const auto stored = source.find("group");
					if (stored == source.end() || (stored->is_number_integer() && *stored == -1)) return true;
					if (!stored->is_string()) return false;
					const auto &text = stored->get_ref<const std::string &>();
					if (text.size() > imagegraph::Limits::MaximumTextBytes) return false;
					if (!holdText(text)) return false;
					parent = text;
					return true;
				};
				const auto membership = [&](Node &node) {
					std::string parent;
					if (!parentOf(*sources.at(node.Id), parent) ||
						(!parent.empty() && !groups.contains(parent))) {
						if (!node.Type.starts_with("pxcx.opaque/"))
							return Fail(failure, "source child has an unknown or non-group parent");
						node.GroupId.clear();
						result.Diagnostics.push_back(
							{imagegraph::Status::UnsupportedExecution,
							 node.Id,
							 "group",
							 "opaque source group membership remains in the archive"}
						);
						return true;
					}
					node.GroupId = std::move(parent);
					return true;
				};
				for (const Json &source : sourceNodes) {
					const bool builder = source.at("type") == "Node_Pixel_Builder";
					const auto &sourceType = source.at("type").get_ref<const std::string &>();
					if (!detail::IsOrdinarySourceGroup(sourceType) && !builder) continue;
					if (builder &&
						std::none_of(
							result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [&](const Node &node) {
								return node.Id == source.at("id").get_ref<const std::string &>() &&
									   node.Type == "pc.pixel_builder";
							}
						))
						return Fail(failure, "source Pixel Builder controls are not exactly projected");
					if (result.Graph.Groups.size() == imagegraph::Limits::MaximumGroups)
						return Fail(failure, "source group count exceeds the native limit");

					if (source.contains("render") && !source.at("render").is_boolean())
						return Fail(failure, "source group render flag is not boolean");

					Group group;
					group.Id = source.at("id").get<std::string>();
					group.RenderActive = source.value("render", true);
					if (!group.RenderActive) result.Graph.FormatVersion = 10;
					if (builder) {
						group.OwnerNodeId = group.Id;
						result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
					}
					if (!builder && source.contains("instanceBase")) {
						if (!source.at("instanceBase").is_string() ||
							source.at("instanceBase").get_ref<const std::string &>().size() >
								imagegraph::Limits::MaximumTextBytes)
							return Fail(failure, "group instance base is not a bounded durable ID");
						group.InstanceBase = source.at("instanceBase").get<std::string>();
					}
					if (const auto name = source.find("name"); name != source.end()) {
						if (!name->is_string()) return Fail(failure, "source group name is not text");
						const auto &text = name->get_ref<const std::string &>();
						if (text.size() > imagegraph::Limits::MaximumTextBytes)
							return Fail(failure, "source group name exceeds the native limit");
						group.Name = text;
					} else
						group.Name = sourceType == "Node_Collection" ? "" : "Group";
					if (group.Name.size() > imagegraph::Limits::MaximumTextBytes ||
						!parentOf(source, group.ParentId))
						return Fail(failure, "source group identity exceeds the native limits");
					const auto attributes = source.find("attri");
					if (attributes != source.end()) {
						if (!attributes->is_object())
							return Fail(failure, "source group attributes are not an object");
						group.ColorDepth = 3;
						if (const auto depth = attributes->find("color_depth"); depth != attributes->end()) {
							if (!WholeNumber(*depth, group.ColorDepth) || group.ColorDepth < 0 ||
								group.ColorDepth > 8)
								return Fail(
									failure, "source group color_depth is outside its verified range"
								);
						}
					}
					if (attributes != source.end()) {
						const Json defaults = {
							{"lock_input", false},
							{"always_topo", false},
							{"node_width", 0},
							{"node_height", 0},
							{"node_param_width", 192},
							{"outp_meta", false},
							{"color", -1},
							{"update_graph", true},
							{"show_update_trigger", false},
							{"array_process", 0}
						};
						for (const auto &[key, stored] : attributes->items()) {
							if (key == "outp_meta" || key == "show_update_trigger" || key == "update_graph") {
								if (!stored.is_boolean())
									return Fail(
										failure, "source group common attribute " + key + " is not boolean"
									);
								// Local lifecycle flags are captured once in the ordered common owner
								// registry.
								continue;
							}
							if (key == "pure_function") {
								if (!stored.is_boolean())
									return Fail(failure, "source group pure_function is not boolean");
								group.PureFunction = stored.get<bool>();
								if (!group.PureFunction) result.Graph.FormatVersion = 10;
								continue;
							}
							// grug Collection path belongs to the file host. source retains it; importer
							// never opens it.
							if (key == "path") {
								if (!stored.is_string() || stored.get_ref<const std::string &>().size() >
															   bake::PxcxLimits::MaximumNodeTextBytes)
									return Fail(failure, "source group path is not bounded text");
								continue;
							}
							if (key == "color_depth" || key == "interpolate" || key == "oversample" ||
								key == "custom_input_list" || key == "custom_output_list")
								continue;
							if (key == "input_display_list" || key == "output_display_list") {
								if (!stored.is_array())
									return Fail(failure, "source group display list is not an array");
								result.Diagnostics.push_back(
									{imagegraph::Status::UnsupportedExecution,
									 group.Id,
									 key,
									 "source group display ordering remains in the archive"}
								);
								continue;
							}
							const auto expected = defaults.find(key);
							if (builder && key == "always_topo" && stored == true) continue;
							if (expected == defaults.end() || stored != *expected)
								return Fail(failure, "source group attribute " + key + " is not represented");
						}
					}
					if (source.contains("tool")) {
						int64_t tool;
						if (builder && source.at("tool").is_string()) {
							const auto &toolId = source.at("tool").get_ref<const std::string &>();
							const auto selected = sources.find(toolId);
							std::string parent;
							if (toolId.size() > imagegraph::Limits::MaximumTextBytes ||
								selected == sources.end() || !parentOf(*selected->second, parent) ||
								parent != group.Id)
								return Fail(
									failure, "Pixel Builder tool must identify a bounded local child"
								);
							result.Diagnostics.push_back(
								{imagegraph::Status::UnsupportedExecution,
								 group.Id,
								 "tool",
								 "source preview tool selection remains in the archive"}
							);
						} else if (!WholeNumber(source.at("tool"), tool) || tool != -4)
							return Fail(failure, "source group tool node is not represented");
					}
					for (std::string_view key : {"interpolate", "oversample"}) {
						// Source constructors inherit (0); saved stripped attributes restore Pixel/Empty (1).
						int64_t choice = attributes == source.end() ? 0 : 1;
						if (attributes != source.end() && attributes->contains(key)) {
							if (!WholeNumber(attributes->at(key), choice))
								return Fail(failure, "source group sampling attribute is not integral");
						}
						if (choice < 0 || choice > (key == "interpolate" ? 7 : 13))
							return Fail(
								failure, "source group sampling attribute is outside its source range"
							);
						if (key == "interpolate")
							group.Interpolation = choice;
						else
							group.Oversample = choice;
					}
					groups.emplace(
						sources.at(group.Id)->at("id").get_ref<const std::string &>(),
						result.Graph.Groups.size()
					);
					result.Graph.Groups.push_back(std::move(group));
				}
				if (groups.empty()) {
					for (Node &node : result.Graph.Nodes) {
						if (node.Id == result.Graph.ProjectGlobalNodeId && node.Type == "pc.global_scope")
							continue;
						if (!membership(node)) return false;
					}
					return true;
				}
				for (const Group &group : result.Graph.Groups) {
					std::string_view parent = group.ParentId;
					for (size_t depth = 0; !parent.empty(); ++depth) {
						const auto found = groups.find(std::string(parent));
						if (depth >= groups.size() || found == groups.end())
							return Fail(failure, "source group hierarchy has an unknown parent or cycle");
						parent = result.Graph.Groups[found->second].ParentId;
					}
				}
				for (Group &group : result.Graph.Groups) {
					const Json &source = *sources.at(group.Id);
					const auto attributes = source.find("attri");
					for (const auto &[key, direction, kind] :
						 {std::tuple{"custom_input_list", PortDirection::Input, "Node_Group_Input"},
						  std::tuple{"custom_output_list", PortDirection::Output, "Node_Group_Output"}}) {
						if (attributes == source.end() || !attributes->contains(key)) continue;
						const Json &list = attributes->at(key);
						if (!list.is_array() ||
							list.size() > imagegraph::Limits::MaximumGroupPorts - group.Ports.size())
							return Fail(failure, "source group socket list exceeds the native limit");
						for (size_t index = 0; index < list.size(); ++index) {
							if (!list[index].is_string())
								return Fail(failure, "source group socket ID is not text");
							const auto &storedId = list[index].get_ref<const std::string &>();
							if (storedId.empty() ||
								storedId.size() > imagegraph::Limits::MaximumTextBytes -
													  std::string_view("/parent-value").size())
								return Fail(failure, "source group socket ID exceeds the native limit");
							const std::string &id = storedId;
							const auto child = sources.find(id);
							std::string parent;
							if (!holdText(group.Id) || !operationBudget.Hold(id.size() + 13 + 16))
								return Fail(failure, "source boundary names exceed the operation budget");
							std::string junctionId;
							junctionId.reserve(id.size() + 13);
							junctionId.append(id);
							junctionId.append("/parent-value");
							if (child == sources.end() || child->second->at("type") != kind ||
								!parentOf(*child->second, parent) || parent != group.Id ||
								!boundaries
									 .emplace(id, Boundary{group.Id, direction, index, std::move(junctionId)})
									 .second)
								return Fail(
									failure,
									"source group socket has a missing, duplicated or unrelated child ID"
								);

							if (result.Graph.Junctions.size() == imagegraph::Limits::MaximumJunctions)
								return Fail(failure, "source group junction count exceeds the native limit");
							result.Graph.Junctions.push_back(
								Junction{
									boundaries.at(id).JunctionId,
									group.Id,
									ValueType::Any,
									direction == PortDirection::Input ? std::optional<imagegraph::Value>{-1.0}
																	  : std::nullopt
								}
							);
							if (boundaries.at(id).JunctionId.size() > imagegraph::Limits::MaximumTextBytes ||
								sources.contains(boundaries.at(id).JunctionId))
								return Fail(
									failure, "derived boundary junction ID conflicts or exceeds text bounds"
								);
							group.Ports.push_back({id, boundaries.at(id).JunctionId, direction, id});
							auto control = std::find_if(
								result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [&](const Node &n) {
									return n.Id == id;
								}
							);
							if (control == result.Graph.Nodes.end())
								return Fail(failure, "group boundary control node is absent");
							const auto *entry = imagegraph::FindCatalogueSource(kind);
							if (!entry) return Fail(failure, "group boundary has no source-backed schema");
							if (control->Type != entry->Type) {

								const auto replacementBytes = imagegraph::NodeClonePayloadBytes(*control);
								if (!replacementBytes || !operationBudget.Hold(*replacementBytes))
									return Fail(
										failure,
										"boundary control replacement exceeds native projection operation "
										"bounds"
									);
								imagegraph::Document animation;
								Node replacement = *control;
								replacement.Type = entry->Type;
								replacement.Values.clear();
								std::string reason;
								if (!CatalogueNode(
										*child->second,
										*entry,
										id,
										replacement,
										animation,
										reason,
										&operationBudget
									))
									return Fail(failure, "group boundary controls: " + reason);
								*control = std::move(replacement);
								if (!AdmitNativeSlots(
										result.Graph.Keyframes,
										result.Graph.Keyframes.size() + animation.Keyframes.size(),
										&operationBudget
									) ||
									!AdmitNativeSlots(
										result.Graph.Tracks,
										result.Graph.Tracks.size() + animation.Tracks.size(),
										&operationBudget
									))
									return Fail(
										failure, "native animator publication exceeds operation bounds"
									);
								result.Graph.Keyframes.insert(
									result.Graph.Keyframes.end(),
									std::make_move_iterator(animation.Keyframes.begin()),
									std::make_move_iterator(animation.Keyframes.end())
								);
								result.Graph.Tracks.insert(
									result.Graph.Tracks.end(),
									std::make_move_iterator(animation.Tracks.begin()),
									std::make_move_iterator(animation.Tracks.end())
								);
							}
							control->GroupId = group.Id;
							if (child->second->contains("instanceBase"))
								control->InstanceBase = child->second->at("instanceBase").get<std::string>();
							if (direction == PortDirection::Input) {
								const Json *record =
									Input(source, index + (group.OwnerNodeId.empty() ? 0 : 4));
								if (record) {
									if (record->contains("anim") && !record->at("anim").is_boolean())
										return Fail(failure, "group parent animator mode is malformed");
									const bool animated = record->value("anim", false);
									auto &modes = animated ? control->SourceAnimatedInputs
														   : control->SourceStaticInputs;
									const size_t count = modes.size() + 1;
									if (count > imagegraph::Limits::MaximumArrayElements ||
										!holdText("parent_value") ||
										(count > modes.capacity() &&
										 !operationBudget.Hold(count * sizeof(std::string))))
										return Fail(
											failure, "group parent animator mode exceeds the operation budget"
										);
									modes.reserve(count);
									modes.emplace_back("parent_value");
								}
								if (record) {
									imagegraph::Document animation;
									const auto *parent =
										imagegraph::FindCatalogueInput(*entry, "parent_value");
									std::erase_if(control->Values, [](const auto &v) {
										return v.Port == "parent_value";
									});
									std::string reason;
									if (!parent || !CatalogueInputValue(
													   *record,
													   *entry,
													   *parent,
													   id,
													   *control,
													   animation,
													   reason,
													   true,
													   &operationBudget
												   ))
										return Fail(failure, "group parent value: " + reason);
									if (const auto authored = std::find_if(
											control->Values.begin(),
											control->Values.end(),
											[](const auto &v) { return v.Port == "parent_value"; }
										);
										authored != control->Values.end()) {
										const auto bytes = imagegraph::ValueClonePayloadBytes(authored->Data);
										if (!bytes || !operationBudget.Hold(*bytes))
											return Fail(
												failure, "parent junction value copy exceeds operation bounds"
											);
										result.Graph.Junctions.back().Default =
											imagegraph::Value(authored->Data);
									}
									if (!AdmitNativeSlots(
											result.Graph.Keyframes,
											result.Graph.Keyframes.size() + animation.Keyframes.size(),
											&operationBudget
										) ||
										!AdmitNativeSlots(
											result.Graph.Tracks,
											result.Graph.Tracks.size() + animation.Tracks.size(),
											&operationBudget
										))
										return Fail(
											failure, "native animator publication exceeds operation bounds"
										);
									result.Graph.Keyframes.insert(
										result.Graph.Keyframes.end(),
										std::make_move_iterator(animation.Keyframes.begin()),
										std::make_move_iterator(animation.Keyframes.end())
									);
									result.Graph.Tracks.insert(
										result.Graph.Tracks.end(),
										std::make_move_iterator(animation.Tracks.begin()),
										std::make_move_iterator(animation.Tracks.end())
									);
								}
							}
						}
					}
				}
				for (const Json &source : sourceNodes) {
					const std::string type = source.at("type").get<std::string>();
					if ((type == "Node_Group_Input" || type == "Node_Group_Output") &&
						!boundaries.contains(source.at("id").get<std::string>()))
						return Fail(failure, "source group IO is absent from its saved socket list");
				}
				const auto route = [&](std::string &nodeId, std::string &port, uint32_t index, bool output) {
					const auto group = groups.find(nodeId);
					if (group != groups.end()) {
						const auto direction = output ? PortDirection::Output : PortDirection::Input;
						if (!result.Graph.Groups[group->second].OwnerNodeId.empty()) {
							const uint32_t fixed = output ? 2 : 4;
							if (index < fixed) return true;
							index -= fixed;
						}
						for (const auto &[id, boundary] : boundaries)
							if (boundary.GroupId == nodeId && boundary.Direction == direction &&
								boundary.Index == index) {
								nodeId = boundary.JunctionId;
								port = "value";
								return true;
							}
						return false;
					}
					return true;
				};
				for (size_t index = 0; index < result.Source.Links.size(); ++index) {
					const auto &stored = result.Source.Links[index];
					auto &link = result.Graph.Links[index];
					const Json *record = Input(*sources.at(stored.ToNode), stored.ToInputIndex);
					const bool boundaryLink =
						groups.contains(stored.FromNode) || groups.contains(stored.ToNode) ||
						boundaries.contains(stored.FromNode) || boundaries.contains(stored.ToNode);
					if (boundaryLink &&
						((!admitCommon && CommonSourceRoute(stored)) ||
						 (!ReservedSourceOutput(stored) && record && !OrdinarySourceOutputTag(*record))))
						return Fail(failure, "source group boundary tag interpretation is not represented");
					if (const auto boundary = boundaries.find(stored.FromNode);
						!ReservedSourceOutput(stored) && boundary != boundaries.end())
						link.FromPort = "value";
					if (const auto boundary = boundaries.find(stored.ToNode);
						!stored.DestinationUpdateTrigger && boundary != boundaries.end()) {
						const auto *entry = imagegraph::FindCatalogueSource(
							boundary->second.Direction == PortDirection::Input ? "Node_Group_Input"
																			   : "Node_Group_Output"
						);
						const auto *input = entry ? imagegraph::FindCatalogueInputIndex(
														*entry, static_cast<int32_t>(stored.ToInputIndex)
													)
												  : nullptr;
						if (!input)
							return Fail(failure, "group control input index has no declared source mapping");
						link.ToPort = input->Id;
					}
					if ((!ReservedSourceOutput(stored) &&
						 !route(link.FromNode, link.FromPort, stored.FromIndex, true)) ||
						(!stored.DestinationUpdateTrigger &&
						 !route(link.ToNode, link.ToPort, stored.ToInputIndex, false)))
						return Fail(failure, "source group socket connection index is not in its saved list");
				}
				if (boundaries.size() > (imagegraph::Limits::MaximumLinks - result.Graph.Links.size()) / 1)
					return Fail(failure, "group boundary routes exceed link limits");
				for (const auto &[id, boundary] : boundaries) {
					if (boundary.Direction == PortDirection::Input) {
						const bool animated = std::any_of(
							result.Graph.Keyframes.begin(),
							result.Graph.Keyframes.end(),
							[&](const auto &key) { return key.NodeId == id && key.Port == "parent_value"; }
						);
						const bool linked = std::any_of(
							result.Graph.Links.begin(), result.Graph.Links.end(), [&](const auto &link) {
								return link.ToNode == boundary.JunctionId;
							}
						);
						if (linked || !animated) {
							if (!holdText(boundary.JunctionId) || !holdText(id) || !holdText("value") ||
								!holdText("parent_value"))
								return Fail(
									failure, "source boundary route names exceed the operation budget"
								);
							result.Graph.Links.push_back(
								{boundary.JunctionId, "value", std::string(id), "parent_value"}
							);
						}
					} else {
						if (!holdText(id) || !holdText(boundary.JunctionId) || !holdText("value") ||
							!holdText("value"))
							return Fail(failure, "source boundary route names exceed the operation budget");
						result.Graph.Links.push_back(
							{std::string(id), "value", boundary.JunctionId, "value"}
						);
					}
				}

				for (auto &output : result.Graph.Outputs) {
					for (const auto &stored : result.Source.Links)
						if (stored.ToNode == output.Id && stored.ToInputIndex == 0 &&
							!route(output.NodeId, output.Port, stored.FromIndex, true))
							return Fail(failure, "source project output uses an unknown group socket");
				}
				for (auto &output : result.Graph.Outputs) {
					for (size_t hops = 0; hops < boundaries.size(); ++hops) {
						const auto boundary =
							std::find_if(boundaries.begin(), boundaries.end(), [&](const auto &entry) {
								return entry.second.JunctionId == output.NodeId;
							});
						if (boundary == boundaries.end()) break;
						const auto incoming = std::find_if(
							result.Graph.Links.begin(), result.Graph.Links.end(), [&](const auto &link) {
								return link.ToNode == output.NodeId && link.ToPort == "value";
							}
						);
						if (incoming == result.Graph.Links.end())
							return Fail(failure, "source group output route has no producer");
						output.NodeId = incoming->FromNode;
						output.Port = incoming->FromPort;
					}
				}
				for (Node &node : result.Graph.Nodes) {
					if (node.Id == result.Graph.ProjectGlobalNodeId && node.Type == "pc.global_scope")
						continue;
					if (const auto group = groups.find(node.Id);
						group != groups.end() && result.Graph.Groups[group->second].OwnerNodeId.empty())
						continue;
					if (node.InstanceBase.empty() && !node.Type.starts_with("pxcx.opaque/") &&
						sources.at(node.Id)->contains("instanceBase"))
						node.InstanceBase = sources.at(node.Id)->at("instanceBase").get<std::string>();
					if (!membership(node)) return false;
				}
				const auto ownerOf = [&](std::string_view id) -> std::string_view {
					std::string_view boundaryScope;
					if (const auto found = boundaries.find(std::string(id)); found != boundaries.end())
						boundaryScope = found->second.GroupId;
					for (const auto &[child, boundary] : boundaries)
						if (boundary.JunctionId == id) boundaryScope = boundary.GroupId;
					const auto node = std::find_if(
						result.Graph.Nodes.begin(), result.Graph.Nodes.end(), [&](const Node &value) {
							return value.Id == id;
						}
					);
					std::string_view scope = !boundaryScope.empty()				? boundaryScope
											 : node == result.Graph.Nodes.end() ? std::string_view{}
																				: node->GroupId;
					for (size_t depth = 0; !scope.empty() && depth < groups.size(); ++depth) {
						const auto group = groups.find(scope);
						if (group == groups.end() || result.Graph.Groups[group->second].OwnerNodeId.empty())
							break;
						scope = result.Graph.Groups[group->second].ParentId;
					}
					return scope;
				};
				for (const auto &link : result.Graph.Links) {
					const auto from = ownerOf(link.FromNode), to = ownerOf(link.ToNode);
					if (from == to) continue;
					const auto boundaryFor = [&](std::string_view id) {
						return std::find_if(boundaries.begin(), boundaries.end(), [&](const auto &entry) {
							return entry.second.JunctionId == id;
						});
					};
					const auto source = boundaryFor(link.FromNode), target = boundaryFor(link.ToNode);
					const bool leavesChild = source != boundaries.end() &&
											 source->second.Direction == PortDirection::Output &&
											 result.Graph.Groups[groups.at(from)].ParentId == to;
					const bool entersChild = target != boundaries.end() &&
											 target->second.Direction == PortDirection::Input &&
											 result.Graph.Groups[groups.at(to)].ParentId == from;
					if (!leavesChild && !entersChild)
						return Fail(failure, "source group link crosses an undeclared native boundary");
				}

				// Only represented boundary nodes disappear; their complete source remains in the archive.
				std::erase_if(result.Graph.Nodes, [&](const Node &node) {
					const auto group = groups.find(node.Id);
					if (group == groups.end() || !result.Graph.Groups[group->second].OwnerNodeId.empty())
						return false;
					if (node.Type.starts_with("pc.")) --result.CatalogueNodes;
					return true;
				});
				std::erase_if(result.Diagnostics, [&](const auto &diagnostic) {
					return (groups.contains(diagnostic.NodeId) || boundaries.contains(diagnostic.NodeId)) &&
						   diagnostic.Message != "source group display ordering remains in the archive" &&
						   diagnostic.Message != "source preview tool selection remains in the archive";
				});
				if (std::any_of(
						result.Graph.Groups.begin(), result.Graph.Groups.end(), [](const Group &group) {
							return group.ColorDepth != 1 || !group.InstanceBase.empty();
						}
					))
					result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
				if (!boundaries.empty())
					result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);

				if (!detail::CaptureGroupPrebinding(
						result.Graph, result.GroupPrebinding, previousDocumentBytes, operationBudget, failure
					))
					return false;
				if (!detail::ResolveImportedGroupInstances(
						root,
						result.Graph,
						failure,
						previousDocumentBytes,
						imagegraph::Limits::MaximumEvaluationBytes,
						&operationBudget
					))
					return false;
				return true;
			} catch (const std::bad_alloc &) {
				return Fail(failure, "source group projection exceeds the operation allocation budget");
			}
		}

		// Every link touching a catalogue node must land on a declared source index.
		bool CatalogueLinksRepresentable(
			const bake::PxcxArchive &archive,
			const bake::PxcxNodeFact &node,
			const imagegraph::CatalogueEntry &entry,
			std::string &reason,
			const imagegraph::Node *mappedNode,
			bool admitCommon
		);

		std::string CatalogueInputPort(const imagegraph::CatalogueEntry &entry, uint32_t index);

		// Output and input ports for a link end on a catalogue node.
		std::string CatalogueOutputPort(const imagegraph::CatalogueEntry &entry, uint32_t index) {
			if (entry.SourceNode == "Node_Pixel_Builder" && index >= 2 &&
				index - 2 < imagegraph::Limits::MaximumGroupPorts)
				return "collection_output_" + std::to_string(index - 2);
			if (entry.SourceNode == "Node_Array_Split" && index > 0 &&
				index <= imagegraph::Limits::MaximumDynamicOutputsPerNode)
				return "val_" + std::to_string(index);
			for (const imagegraph::CatalogueOutput &output : entry.Outputs)
				if (output.SourceIndex == static_cast<int32_t>(index)) return std::string(output.Id);
			// node_data.gml getOutputIndex: 1000 + n connects the bypass junction of input n.
			if (index >= 1000) {
				const uint32_t physical = index - 1000;
				if ((entry.Type == "pc.hlsl" || entry.Type == "pc.path_smooth" ||
					 entry.Type == "pc.path_bridge") &&
					physical >= uint32_t(entry.DynamicFixedLength) &&
					physical - uint32_t(entry.DynamicFixedLength) <
						imagegraph::MaximumDynamicInputsForType(entry.Type)) {
					const auto port = CatalogueInputPort(entry, physical);
					if (!port.empty()) return port + ".bypass";
				}
				if (const auto *input =
						imagegraph::FindCatalogueInputIndex(entry, static_cast<int32_t>(index - 1000)))
					return std::string(input->Id) + ".bypass";
			}
			return {};
		}
		std::string CatalogueInputPort(const imagegraph::CatalogueEntry &entry, uint32_t index) {
			if (entry.SourceNode == "Node_Pixel_Builder" && index >= 4 &&
				index - 4 < imagegraph::Limits::MaximumGroupPorts)
				return "collection_input_" + std::to_string(index - 4);
			if (const auto *input = imagegraph::FindCatalogueInputIndex(entry, static_cast<int32_t>(index)))
				return std::string(input->Id);
			if (entry.DynamicGroupLength <= 0 || static_cast<int32_t>(index) < entry.DynamicFixedLength)
				return {};
			const int32_t relative = static_cast<int32_t>(index) - entry.DynamicFixedLength;
			for (const imagegraph::CatalogueInput &input : entry.DynamicTemplate)
				if (input.SourceIndex == relative % entry.DynamicGroupLength)
					return std::string(input.Id) + "_" + std::to_string(relative / entry.DynamicGroupLength);
			return {};
		}
	}

	namespace {
		bool CatalogueLinksRepresentable(
			const bake::PxcxArchive &archive,
			const bake::PxcxNodeFact &node,
			const imagegraph::CatalogueEntry &entry,
			std::string &reason,
			const imagegraph::Node *mappedNode,
			bool admitCommon
		) {
			const auto declared = [&](uint32_t physical) {
				if ((entry.Type != "pc.hlsl" && entry.Type != "pc.path_smooth" &&
					 entry.Type != "pc.path_bridge") ||
					physical < uint32_t(entry.DynamicFixedLength))
					return true;
				const auto port = CatalogueInputPort(entry, physical);
				return mappedNode && std::any_of(
										 mappedNode->DynamicInputs.begin(),
										 mappedNode->DynamicInputs.end(),
										 [&](const auto &input) { return input.Id == port; }
									 );
			};
			for (const bake::PxcxLinkFact &link : archive.Links) {
				if (!admitCommon && CommonSourceRoute(link) &&
					(link.FromNode == node.Id || link.ToNode == node.Id)) {
					reason = "common source socket lifecycle is not represented";
					return false;
				}
				if ((link.FromNode == node.Id && !ReservedSourceOutput(link) && link.FromIndex >= 1000 &&
					 !declared(link.FromIndex - 1000)) ||
					(link.ToNode == node.Id && !link.DestinationUpdateTrigger &&
					 !declared(link.ToInputIndex))) {
					reason = entry.Type == "pc.hlsl"
								 ? "HLSL link refers to an undeclared dynamic source input"
							 : entry.Type == "pc.path_smooth"
								 ? "Smooth Path link refers to an undeclared dynamic source input"
								 : "Bridge Path link refers to an undeclared dynamic source input";
					return false;
				}
				if (link.FromNode == node.Id && !ReservedSourceOutput(link) &&
					CatalogueOutputPort(entry, link.FromIndex).empty()) {
					reason =
						"output index " + std::to_string(link.FromIndex) + " is not in the source catalogue";
					return false;
				}
				if (link.ToNode == node.Id && !link.DestinationUpdateTrigger &&
					CatalogueInputPort(entry, link.ToInputIndex).empty()) {
					reason = "linked input index " + std::to_string(link.ToInputIndex) +
							 " is not in the source catalogue";
					return false;
				}
			}
			return true;
		}

		// Project attributes: surface size, sampling, palette, surface format and 3D shading.
		void ProjectSettings(const Json &root, PxcxImport &result) {
			const auto attributes = root.find("attributes");
			if (attributes == root.end() || !attributes->is_object()) return;
			imagegraph::ProjectSettings project;
			const auto dimension = attributes->find("surface_dimension");
			std::array<double, 4> size{};
			if (dimension != attributes->end()) {
				if (!Numbers(*dimension, 2, size) || size[0] < 1 || size[1] < 1 ||
					size[0] > imagegraph::Limits::MaximumDimension ||
					size[1] > imagegraph::Limits::MaximumDimension || std::trunc(size[0]) != size[0] ||
					std::trunc(size[1]) != size[1])
					return;
				project.SurfaceWidth = static_cast<uint32_t>(size[0]);
				project.SurfaceHeight = static_cast<uint32_t>(size[1]);
			}
			int64_t mode = 0;
			if (attributes->contains("shader") && WholeNumber((*attributes)["shader"], mode) && mode >= 0 &&
				mode <= 1)
				project.Shader3D = mode;
			if (attributes->contains("color_depth") && WholeNumber((*attributes)["color_depth"], mode) &&
				mode >= 0 && mode <= 6)
				project.ColorDepth = mode;
			if (project.Shader3D != 0 || project.ColorDepth != 1)
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			if (attributes->contains("interpolate") && WholeNumber((*attributes)["interpolate"], mode) &&
				mode >= 0 && mode <= 6)
				project.Interpolation = mode;
			if (attributes->contains("oversample") && WholeNumber((*attributes)["oversample"], mode) &&
				mode >= 0 && mode <= 12)
				project.Oversample = mode;
			const auto palette = attributes->find("palette");
			if (palette != attributes->end() && palette->is_array() &&
				palette->size() <= imagegraph::Limits::MaximumProjectPaletteEntries) {
				std::vector<Colour> colours;
				for (const Json &entry : *palette) {
					Colour color;
					if (!PackedColour(entry, color)) return;
					colours.push_back(color);
				}
				project.Palette = std::move(colours);
			}
			result.Graph.Project = std::move(project);
		}

		bool ProjectRegions(
			const Json &root, PxcxImport &result, detail::ImportBudget &budget, std::string &failure
		) {
			const auto records = root.find("aRegion");
			if (records == root.end()) return true;
			if (!records->is_array() || records->size() > imagegraph::Limits::MaximumAnimationRegions)
				return Fail(failure, "source animation region count exceeds bounds");
			imagegraph::ProjectSettings project =
				result.Graph.Project.value_or(imagegraph::ProjectSettings{});
			if (!AdmitNativeSlots(project.AnimationRegions, records->size(), &budget))
				return Fail(failure, "source animation regions exceed operation bounds");
			for (const auto &record : *records) {
				imagegraph::AnimationRegion region;
				if (!record.is_object() || !record.contains("l") || !record["l"].is_string() ||
					!record.contains("c") || !PackedColour(record["c"], region.Color) ||
					!record.contains("fs") || !record["fs"].is_number() || !record.contains("fe") ||
					!record["fe"].is_number() ||
					!imagegraph::SplitFrameTime(record["fs"].get<double>(), region.Start) ||
					!imagegraph::SplitFrameTime(record["fe"].get<double>(), region.End) ||
					!AdmitNativeText(record["l"].get_ref<const std::string &>(), &budget))
					return Fail(failure, "source animation region has invalid bounded fields");
				region.Label = record["l"].get<std::string>();
				region.Color.Alpha = 255;
				std::array<char, imagegraph::Limits::MaximumSourceRegionIdBytes> storage{};
				constexpr std::string_view prefix = "pxc:region:";
				std::copy(prefix.begin(), prefix.end(), storage.begin());
				const auto encoded = std::to_chars(
					storage.data() + prefix.size(),
					storage.data() + storage.size(),
					project.AnimationRegions.size()
				);
				const std::string_view origin(storage.data(), size_t(encoded.ptr - storage.data()));
				if (encoded.ec != std::errc{} || !AdmitNativeText(origin, &budget))
					return Fail(failure, "source animation region origin exceeds operation bounds");
				region.SourceRegionId = origin;
				project.AnimationRegions.push_back(std::move(region));
			}
			if (!imagegraph::ValidProjectAnimationRegions(project))
				return Fail(failure, "source animation regions are invalid");
			result.Graph.Project = std::move(project);
			result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			return true;
		}

		// Source previewGrid restores conditionally; ordered previewRuler restores independently.
		void PreviewSettings(const Json &root, PxcxImport &result, const PxcxImportOptions &options) {
			const auto opaque = [&](std::string_view port, imagegraph::Status status, std::string reason) {
				result.Diagnostics.push_back(
					{status,
					 "",
					 std::string(port),
					 "PXCX preview settings stay opaque: " + std::move(reason) + "; source bytes retained"}
				);
			};
			if (const auto grid = root.find("previewGrid");
				options.SavePreviewSettings && grid != root.end()) {
				imagegraph::ProjectSettings project =
					result.Graph.Project.value_or(imagegraph::ProjectSettings{});
				bool valid = grid->is_object();
				if (valid) {
					for (const auto &[name, destination] :
						 {std::pair{"show", &project.PreviewGrid.Show},
						  std::pair{"snap", &project.PreviewGrid.Snap}}) {
						if (const auto value = grid->find(name); value != grid->end()) {
							if (!value->is_boolean())
								valid = false;
							else
								*destination = value->get<bool>();
						}
					}
					if (const auto value = grid->find("size"); value != grid->end()) {
						std::array<double, 4> size{};
						if (!Numbers(*value, 2, size) || size[0] < 0 || size[1] < 0)
							valid = false;
						else
							project.PreviewGrid.Size = {size[0], size[1]};
					}
				}
				if (!valid || !imagegraph::ValidProjectPreviewSettings(project))
					opaque("previewGrid", imagegraph::Status::InvalidValue, "grid show/snap/size is invalid");
				else {
					result.Graph.Project = std::move(project);
					result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
				}
			}
			if (const auto rulers = root.find("previewRuler"); rulers != root.end()) {
				if (!rulers->is_array()) {
					opaque(
						"previewRuler", imagegraph::Status::InvalidValue, "rulers must be an ordered array"
					);
					return;
				}
				if (rulers->size() > imagegraph::Limits::MaximumArrayElements) {
					opaque(
						"previewRuler", imagegraph::Status::LimitExceeded, "ruler count exceeds native limit"
					);
					return;
				}
				std::vector<imagegraph::PreviewRulerGuide> guides;
				guides.reserve(rulers->size());
				for (const Json &ruler : *rulers) {
					std::array<double, 4> pair{};
					if (!Numbers(ruler, 2, pair) || (pair[0] != 0 && pair[0] != 1)) {
						opaque(
							"previewRuler",
							imagegraph::Status::InvalidValue,
							"ruler axis/position pair is invalid"
						);
						return;
					}
					guides.push_back(
						{pair[0] == 0 ? imagegraph::PreviewRulerAxis::Horizontal
									  : imagegraph::PreviewRulerAxis::Vertical,
						 pair[1]}
					);
				}
				imagegraph::ProjectSettings project =
					result.Graph.Project.value_or(imagegraph::ProjectSettings{});
				project.PreviewRulers = std::move(guides);
				result.Graph.Project = std::move(project);
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
		}

		// The project animator: total frames, frame rate and end behavior (loop, stop, pingpong).
		void Timeline(const Json &root, PxcxImport &result) {
			const auto animator = root.find("animator");
			if (animator == root.end() || !animator->is_object()) return;
			int64_t frames = 0, playback = 0;
			if (!animator->contains("frames_total") || !WholeNumber((*animator)["frames_total"], frames) ||
				frames < 1 || static_cast<uint64_t>(frames) > imagegraph::Limits::MaximumTick)
				return;
			if (!animator->contains("playback") || !WholeNumber((*animator)["playback"], playback) ||
				playback < 0 || playback > 2)
				return;
			const auto rate = animator->find("framerate");
			if (rate == animator->end() || !rate->is_number() || !(rate->get<double>() > 0) ||
				!std::isfinite(rate->get<double>()))
				return;
			static constexpr std::array<std::string_view, 3> ENDS = {"loop", "stop", "pingpong"};
			imagegraph::TimelineSettings timeline;
			timeline.Frames = static_cast<uint64_t>(frames);
			timeline.First = 0;
			timeline.Last = static_cast<uint64_t>(frames) - 1;
			const auto unsupportedRange = [&](std::string_view port, std::string message) {
				result.Diagnostics.push_back(
					{imagegraph::Status::UnsupportedExecution,
					 "",
					 std::string(port),
					 "PXCX timeline stays opaque: " + std::move(message) + "; source bytes retained"}
				);
			};
			imagegraph::SourceAuthoringFrameBounds sourceBounds;
			for (const auto &[key, target] :
				 {std::pair{"frame_range_start", &sourceBounds.Start},
				  std::pair{"frame_range_end", &sourceBounds.End}}) {
				const auto found = animator->find(key);
				if (found == animator->end()) continue;
				if (found->is_null()) {
					target->Presence = imagegraph::SourceFrameBoundPresence::Null;
					continue;
				}
				if (!found->is_number() ||
					!imagegraph::SplitFrameTime(
						found->get<double>(), target->Value, false, imagegraph::Limits::MaximumTick + 1
					)) {
					unsupportedRange(key, "saved numeric endpoint exceeds native signed authoring profile");
					return;
				}
				target->Presence = imagegraph::SourceFrameBoundPresence::Explicit;
			}
			timeline.SourceBounds = sourceBounds;
			imagegraph::Diagnostic rangeDiagnostic;
			if (imagegraph::ProjectSourceTimelineWindow(timeline, rangeDiagnostic) !=
				imagegraph::Status::Ok) {
				unsupportedRange("frame_range", rangeDiagnostic.Message);
				return;
			}
			timeline.Playback = std::string(ENDS[static_cast<size_t>(playback)]);
			timeline.FramesPerSecond = rate->get<double>();
			result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			result.Graph.Timeline = std::move(timeline);
		}
	}

	bool ImportPxcxImageGraph(const bake::PxcxArchive &archive, PxcxImport &out, std::string &failure) {
		return ImportPxcxImageGraph(archive, out, failure, {});
	}

	static bool ImportPxcxImageGraphCandidate(
		const bake::PxcxArchive &archive,
		PxcxImport &out,
		std::string &failure,
		const PxcxImportOptions &options,
		bool admitCommon,
		bool &commonAdmissionRejected
	) try {
		failure.clear();
		commonAdmissionRejected = false;
		if (!options.MaximumOperationBytes ||
			options.MaximumOperationBytes > imagegraph::Limits::MaximumEvaluationBytes)
			return Fail(failure, "pxcx import operation byte limit is invalid");
		auto previousDocumentBytes = imagegraph::DocumentRetainedPayloadBytes(out.Graph);
		if (!previousDocumentBytes)
			return Fail(failure, "previous native projection has invalid retained payload");
		if (out.GroupPrebinding) {
			const auto priorSnapshotBytes = imagegraph::DocumentRetainedPayloadBytes(*out.GroupPrebinding);
			if (!priorSnapshotBytes || *priorSnapshotBytes > UINT64_MAX - *previousDocumentBytes)
				return Fail(failure, "previous local Group projection has invalid retained payload");
			*previousDocumentBytes += *priorSnapshotBytes;
		}
		const auto priorAdd = [&](uint64_t bytes) {
			if (bytes > UINT64_MAX - *previousDocumentBytes) return false;
			*previousDocumentBytes += bytes;
			return true;
		};
		if (out.GroupBootstrap.capacity() > UINT64_MAX / sizeof(PxcxGroupBootstrapRecord) ||
			out.GroupBindings.capacity() > UINT64_MAX / sizeof(imagegraph::GroupSubtypeBinding) ||
			!priorAdd(out.GroupBootstrap.capacity() * sizeof(PxcxGroupBootstrapRecord)) ||
			!priorAdd(out.GroupBindings.capacity() * sizeof(imagegraph::GroupSubtypeBinding)))
			return Fail(failure, "previous Group callback storage overflows");
		for (const auto &record : out.GroupBootstrap)
			if (!priorAdd(record.NodeId.capacity() + 1))
				return Fail(failure, "previous Group callback names overflow");
		for (const auto &binding : out.GroupBindings)
			if (!priorAdd(binding.NodeId.capacity() + 1) || !priorAdd(binding.OwnerId.capacity() + 1) ||
				!priorAdd(binding.Port.capacity() + 1))
				return Fail(failure, "previous Group binding names overflow");
		if (options.FrameCacheLayouts.size() > 64 || out.Options.FrameCacheLayouts.size() > 64)
			return Fail(failure, "source frame-cache layout observations exceed import bounds");
		if (out.Options.FrameCacheLayouts.capacity() >
				UINT64_MAX / sizeof(SourceFrameCacheLayoutObservation) ||
			!priorAdd(out.Options.FrameCacheLayouts.capacity() * sizeof(SourceFrameCacheLayoutObservation)))
			return Fail(failure, "previous source frame-cache observations overflow");
		for (const auto &observation : out.Options.FrameCacheLayouts)
			if (!priorAdd(observation.NodeId.capacity() + 1) ||
				!priorAdd(observation.DataHash.capacity() + 1))
				return Fail(failure, "previous source frame-cache observation text overflows");
		uint64_t layoutBytes = options.FrameCacheLayouts.size() * sizeof(SourceFrameCacheLayoutObservation);
		for (size_t index = 0; index < options.FrameCacheLayouts.size(); ++index) {
			const auto &observation = options.FrameCacheLayouts[index];
			if (observation.NodeId.empty() ||
				observation.NodeId.size() > imagegraph::Limits::MaximumTextBytes ||
				observation.DataHash.size() != 64 ||
				bake::SpriteCacheLayoutName(observation.Layout).empty() ||
				!std::all_of(observation.DataHash.begin(), observation.DataHash.end(), [](char value) {
					return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
				}))
				return Fail(failure, "source frame-cache layout observation has invalid identity or layout");
			for (size_t prior = 0; prior < index; ++prior)
				if (options.FrameCacheLayouts[prior].NodeId == observation.NodeId)
					return Fail(failure, "source frame-cache layout observation is duplicated");
			const uint64_t textBytes = std::max(observation.NodeId.size(), std::string{}.capacity()) +
									   std::max(observation.DataHash.size(), std::string{}.capacity()) + 2;
			if (textBytes > UINT64_MAX - layoutBytes)
				return Fail(failure, "source frame-cache layout observation bytes overflow");
			layoutBytes += textBytes;
		}
		if (*previousDocumentBytes > options.MaximumOperationBytes ||
			layoutBytes > options.MaximumOperationBytes - *previousDocumentBytes)
			return Fail(failure, "source frame-cache layout copies exceed import operation bounds");
		if (*previousDocumentBytes > options.MaximumOperationBytes)
			return Fail(failure, "previous native Group state exceeds import operation bounds");
		if (archive.OriginalBytes.empty())
			return Fail(failure, "pxcx import requires original archive bytes");
		PxcxImport result;
		result.Options = options;
		result.Graph.FormatVersion = 8;
		if (!bake::ReadPxcx(archive.OriginalBytes, result.Source, failure)) return false;
		if (result.Source.GraphJson != archive.GraphJson || result.Source.Nodes != archive.Nodes ||
			result.Source.Links != archive.Links || result.Source.MetadataNumber != archive.MetadataNumber ||
			result.Source.MetadataText != archive.MetadataText)
			return Fail(failure, "pxcx import fields differ from original archive bytes");
		if (archive.Nodes.size() > imagegraph::Limits::MaximumNodes ||
			archive.Links.size() > imagegraph::Limits::MaximumLinks)
			return Fail(failure, "pxcx import exceeds native node or link limit");
		Json root;
		try {
			root = Json::parse(result.Source.GraphJson.begin(), result.Source.GraphJson.end() - 1);
		} catch (const Json::exception &error) {
			return Fail(failure, "pxcx import JSON parse failed: " + std::string(error.what()));
		}
		const Json &sourceNodes = root.at("nodes");
		if (archive.MetadataNumber == SUPPORTED_VERSION) {
			ProjectSettings(root, result);
			const auto attributes = root.find("attributes");
			if (attributes != root.end() && attributes->is_object() && attributes->contains("color_depth")) {
				int64_t depth;
				if (!WholeNumber((*attributes)["color_depth"], depth) || depth < 0 || depth > 6)
					return Fail(failure, "source project color_depth is outside its verified range");
				if (!result.Graph.Project)
					return Fail(failure, "source project color_depth needs represented project dimensions");
				result.Graph.Project->ColorDepth = depth;
				if (depth != 1) result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			PreviewSettings(root, result, options);
			Timeline(root, result);
		}
		detail::ImportBudget operationBudget(options.MaximumOperationBytes);
		if (!operationBudget.Hold(*previousDocumentBytes) || !operationBudget.Hold(layoutBytes))
			return Fail(failure, "existing import document exceeds native projection operation bounds");
		if (archive.MetadataNumber == SUPPORTED_VERSION &&
			!ProjectRegions(root, result, operationBudget, failure))
			return false;
		GroupSourceSet canonicalGroups(
			std::less<>{}, detail::ImportAllocator<std::string_view>{operationBudget}
		);
		try {
			if (archive.MetadataNumber == SUPPORTED_VERSION &&
				!CanonicalInstanceGroups(root, canonicalGroups, failure))
				return false;
		} catch (const std::bad_alloc &) {
			return Fail(failure, "source instance class bookkeeping exceeds the operation allocation budget");
		}
		result.Graph.Nodes.reserve(archive.Nodes.size());
		result.Graph.Links.reserve(archive.Links.size());
		result.Diagnostics.reserve(archive.Nodes.size());
		std::unordered_map<std::string, size_t> indexById;
		indexById.reserve(archive.Nodes.size());
		std::vector<bool> native(archive.Nodes.size(), false);
		std::vector<const imagegraph::CatalogueEntry *> catalogued(archive.Nodes.size(), nullptr);
		SourceDepthResolver sourceDepth(root);
		size_t mappedAxisKeys = 0;
		for (size_t index = 0; index < archive.Nodes.size(); index++) {
			const bake::PxcxNodeFact &fact = archive.Nodes[index];
			const Json &source = sourceNodes[index];
			if (archive.MetadataNumber == SUPPORTED_VERSION && fact.Type == "Node_Gradient") {
				const auto *sourceEntry = imagegraph::FindCatalogueSource(fact.Type);
				const auto *range = sourceEntry
										? imagegraph::FindCatalogueInput(*sourceEntry, "gradient_map_range")
										: nullptr;
				if (range && range->SourceIndex == 16 && range->SourceKind == "Vec4" &&
					range->Type == imagegraph::ValueType::Vector4) {
					const auto *record = Input(source, static_cast<size_t>(range->SourceIndex));
					if (record && record->contains("anim") && !record->at("anim").is_boolean())
						return Fail(failure, "opaque gradient range animator mode is malformed");
				}
			}
			Node node{fact.Id, Opaque(fact.Type), {}, {fact.X, fact.Y}, {}};
			const auto displayName = source.find("name");
			if (displayName != source.end() && displayName->is_string()) {
				const auto &name = displayName->get_ref<const std::string &>();
				if (!AdmitNativeText(name, &operationBudget))
					return Fail(failure, "source display name exceeds native projection bounds");
				node.SourceDisplayName = name;
				if (!name.empty()) result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			const auto internalName = source.find("iname");
			if (internalName != source.end() && internalName->is_string()) {
				const auto &name = internalName->get_ref<const std::string &>();
				if (!AdmitNativeText(name, &operationBudget))
					return Fail(failure, "source internal name exceeds native projection bounds");
				node.SourceInternalName = name;
				if (!name.empty()) result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			bool mapped = false;
			if (archive.MetadataNumber == SUPPORTED_VERSION &&
				LinksRepresentable(archive, fact, admitCommon) &&
				!(admitCommon && fact.Type == "Node_Solid" &&
				  SourceSolidCommonLifecycle(source, archive, fact.Id))) {
				if (fact.Type == "Node_Solid")
					mapped = Solid(root, source, node);
				else if (fact.Type == "Node_Invert")
					mapped = Invert(source, node);
				else if (fact.Type == "Node_Blend")
					mapped = Blend(source, node);
				else if (fact.Type == "Node_Gradient")
					mapped = Gradient(root, source, node);
				else if (fact.Type == "Node_Number")
					mapped = Number(source, node);
				else if (fact.Type == "Node_Math")
					mapped = MathScalar(source, node) &&
							 (!Input(source, 1)->contains("from_node") || LinkedNumber(root, source));
				else if (fact.Type == "Node_Noise_Simplex")
					mapped = Simplex(root, source, node);
				else if (fact.Type == "Node_Checker")
					mapped = Checker(root, source, node);
				else if (fact.Type == "Node_Offset")
					mapped = Offset(source, node);
				else if (fact.Type == "Node_Polar")
					mapped = Polar(source, node);
				else if (fact.Type == "Node_Threshold")
					mapped = Threshold(source, node);
				else if (fact.Type == "Node_Tile")
					mapped = Tile(root, source, node);
				else if (fact.Type == "Node_Blur")
					mapped = Blur(source, node);
				else if (fact.Type == "Node_Color_adjust")
					mapped = ColorAdjust(source, node);
				else if (fact.Type == "Node_Posterize")
					mapped = Posterize(source, node);
				else if (fact.Type == "Node_Shape")
					mapped = Shape(root, source, node);
				else if (fact.Type == "Node_Vignette")
					mapped = Vignette(source, node);
				else if (fact.Type == "Node_Curve")
					mapped = CurveColor(source, node);
				else if (fact.Type == "Node_Colorize")
					mapped = Colorize(source, node);
			}
			const auto sourceGroup = source.find("group");
			if (sourceGroup != source.end() && sourceGroup->is_string() &&
				canonicalGroups.contains(sourceGroup->get_ref<const std::string &>()))
				mapped = false;
			if (mapped) {
				const auto *sourceEntry = imagegraph::FindCatalogueSource(fact.Type);
				if (sourceEntry && imagegraph::HasNativeExecutor(sourceEntry->Type))
					for (const auto &input : sourceEntry->Inputs) {
						if (input.SourceIndex < 0) continue;
						const auto *record = Input(source, uint32_t(input.SourceIndex));
						if (!record) continue;
						const auto raw = record->find("r");
						// Legacy fixed controls cannot retain an independently stored inactive animator.
						if ((raw != record->end() && raw->is_array() && !raw->empty()) ||
							record->contains("on_end") || record->contains("loop_range")) {
							mapped = false;
							break;
						}
					}
				const auto *depth =
					sourceEntry ? imagegraph::FindCatalogueInput(*sourceEntry, "attribute_color_depth")
								: nullptr;
				if (mapped && depth)
					mapped = !source.contains("instanceBase") && sourceDepth.Resolve(source) == 3 &&
							 sourceDepth.LegacyContextRgba8(source) &&
							 sourceDepth.LegacyInputsRgba8(source, *sourceEntry);
			}
			const imagegraph::CatalogueEntry *entry = nullptr;
			std::string reason;
			if (!mapped && archive.MetadataNumber == SUPPORTED_VERSION)
				entry = imagegraph::FindCatalogueSource(fact.Type);
			if (entry) {
				imagegraph::Document animation;
				if (CatalogueNode(source, *entry, fact.Id, node, animation, reason, &operationBudget) &&
					CatalogueLinksRepresentable(archive, fact, *entry, reason, &node, admitCommon) &&
					(entry->SourceNode != "Node_Array_Split" ||
					 std::all_of(archive.Links.begin(), archive.Links.end(), [&](const auto &link) {
						 return link.FromNode != fact.Id || link.FromIndex <= node.DynamicOutputs.size();
					 }))) {
					if (!node.SourceProperties.empty() || !node.SourceInputExpressions.empty() ||
						!node.DynamicOutputs.empty() ||
						std::any_of(
							node.DynamicInputs.begin(), node.DynamicInputs.end(), [](const auto &input) {
								return !input.SourceLayerName.empty() || !input.SourceInputId.empty();
							}
						))
						result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
					size_t axisCount = 0;
					if (node.SourceSeparatedVec2Animators)
						for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
							for (const auto &axis : input.Axes)
								axisCount += axis.Keys.size();
					const size_t ordinary = result.Graph.Keyframes.size() + animation.Keyframes.size();
					if (ordinary > imagegraph::Limits::MaximumKeyframes ||
						mappedAxisKeys > imagegraph::Limits::MaximumKeyframes - ordinary ||
						axisCount > imagegraph::Limits::MaximumKeyframes - ordinary - mappedAxisKeys)
						return Fail(failure, "PXC ordinary and separated axes exceed aggregate key bounds");
					mappedAxisKeys += axisCount;
					catalogued[index] = entry;
					result.CatalogueNodes++;
					if (animation.Keyframes.size() >
						imagegraph::Limits::MaximumKeyframes - result.Graph.Keyframes.size())
						return Fail(failure, "pxcx import exceeds native keyframe limit");
					if (animation.Tracks.size() >
						imagegraph::Limits::MaximumTracks - result.Graph.Tracks.size())
						return Fail(failure, "pxcx import exceeds native track limit");
					if (!AdmitNativeSlots(
							result.Graph.Keyframes,
							result.Graph.Keyframes.size() + animation.Keyframes.size(),
							&operationBudget
						) ||
						!AdmitNativeSlots(
							result.Graph.Tracks,
							result.Graph.Tracks.size() + animation.Tracks.size(),
							&operationBudget
						))
						return Fail(failure, "native animator publication exceeds operation bounds");
					for (imagegraph::Keyframe &keyframe : animation.Keyframes) {
						if (keyframe.Subframe != 0 || keyframe.NegativeFrame ||
							keyframe.Kind != imagegraph::KeyframeKind::Normal)
							result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
						result.Graph.Keyframes.push_back(std::move(keyframe));
					}
					for (imagegraph::AnimationTrack &track : animation.Tracks)
						result.Graph.Tracks.push_back(std::move(track));
				} else {
					if (operationBudget.Exceeded())
						return Fail(
							failure, "native animator construction exceeds operation bounds: " + reason
						);
					entry = nullptr;
					node.DynamicInputs.clear();
					node.DynamicOutputs.clear();
					node.SourceInputExpressions.clear();
					node.SourceProperties.clear();
					node.SourceSeparatedVec2Animators = {};
				}
			}
			if (!mapped && !entry) {
				node.Type = Opaque(fact.Type);
				node.Values.clear();
				OpaqueDiagnostic(
					result,
					fact,
					archive.MetadataNumber != SUPPORTED_VERSION
						? "PXCX save version has no semantic mapping; source bytes retained"
					: reason.empty() ? "PXCX node has no source catalogue entry; source bytes retained"
									 : "PXCX node stays opaque: " + reason + "; source bytes retained"
				);
			} else if (mapped) {
				native[index] = true;
				result.NativeNodes++;
			}
			if (!node.Type.starts_with("pxcx.opaque/") && fact.Type != "Node_Group" &&
				source.contains("instanceBase")) {
				const auto &base = source.at("instanceBase");
				if (!base.is_string() ||
					base.get_ref<const std::string &>().size() > bake::PxcxLimits::MaximumNodeTextBytes ||
					!AdmitNativeText(base.get_ref<const std::string &>(), &operationBudget))
					return Fail(
						failure,
						node.Type == "pc.hlsl" ? "HLSL instance base is not a bounded durable name"
											   : "source instance base is not a bounded durable name"
					);
				node.InstanceBase = base.get<std::string>();
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			detail::SourceNoiseFieldAnnotation noiseField;
			if (!detail::ReadSourceNoiseFieldAnnotation(source, node.Type, noiseField, failure)) return false;
			if (noiseField.OutputType) {
				if (node.Values.size() >= imagegraph::Limits::MaximumPropertiesPerNode ||
					!AdmitNativeSlots(node.Values, node.Values.size() + 1, &operationBudget) ||
					!AdmitNativeText("output_type", &operationBudget))
					return Fail(failure, "noise field annotation exceeds native import bounds");
				node.Values.push_back({"output_type", imagegraph::EnumValue{*noiseField.OutputType}});
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			if (noiseField.OverrideInstance) {
				if (!AdmitNativeSlots(
						node.InstanceOverrides, node.InstanceOverrides.size() + 1, &operationBudget
					) ||
					!AdmitNativeText("output_type", &operationBudget))
					return Fail(failure, "noise field override exceeds native import bounds");
				node.InstanceOverrides.push_back("output_type");
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			std::optional<std::string_view> cookedSelector;
			if (!detail::ReadCookedAnnotation(source, node.Type, cookedSelector, failure)) return false;
			if (node.Type == "pc.hlsl" && source.contains("atomic_game_engine"))
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			if (cookedSelector) {
				if (node.SourceProperties.size() >= imagegraph::Limits::MaximumPropertiesPerNode ||
					!AdmitNativeSlots(
						node.SourceProperties, node.SourceProperties.size() + 1, &operationBudget
					) ||
					!AdmitNativeText(detail::CookedSelector, &operationBudget) ||
					!AdmitNativeText(*cookedSelector, &operationBudget))
					return Fail(failure, "cooked shader annotation exceeds native import bounds");
				node.SourceProperties.push_back(
					{std::string(detail::CookedSelector), std::string(*cookedSelector)}
				);
				result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
			}
			indexById.emplace(fact.Id, index);
			result.Graph.Nodes.push_back(std::move(node));
		}
		for (const bake::PxcxLinkFact &link : archive.Links) {
			const size_t from = indexById.at(link.FromNode);
			const size_t to = indexById.at(link.ToNode);
			const std::string fromPort =
				ReservedSourceOutput(link) ? ReservedSourceOutputPort(link)
				: native[from]
					? std::string(detail::LegacyNativeOutputPort(archive.Nodes[from].Type, link.FromIndex))
				: catalogued[from] ? CatalogueOutputPort(*catalogued[from], link.FromIndex)
								   : OutputPort(link.FromIndex);
			std::string toPort = link.DestinationUpdateTrigger ? "pxcx.update_in_trigger"
								 : native[to]
									 ? std::string(NativeInput(archive.Nodes[to].Type, link.ToInputIndex))
								 : catalogued[to] ? CatalogueInputPort(*catalogued[to], link.ToInputIndex)
												  : std::string{};
			if (toPort.empty()) toPort = InputPort(link.ToInputIndex);
			result.Graph.Links.push_back({link.FromNode, fromPort, link.ToNode, toPort});
		}
		for (const bake::PxcxNodeFact &node : archive.Nodes) {
			if (node.Type != "Node_Project_Output") continue;
			if (result.Graph.Outputs.size() >= imagegraph::Limits::MaximumOutputs)
				return Fail(failure, "pxcx import exceeds native output count");
			for (const bake::PxcxLinkFact &link : archive.Links) {
				if (link.ToNode != node.Id || link.ToInputIndex != 0) continue;
				const size_t from = indexById.at(link.FromNode);
				result.Graph.Outputs.push_back(
					{node.Id,
					 link.FromNode,
					 ReservedSourceOutput(link) ? ReservedSourceOutputPort(link)
					 : native[from]
						 ? std::string(
							   detail::LegacyNativeOutputPort(archive.Nodes[from].Type, link.FromIndex)
						   )
					 : catalogued[from] ? CatalogueOutputPort(*catalogued[from], link.FromIndex)
										: OutputPort(link.FromIndex)}
				);
				result.Diagnostics.push_back(
					{imagegraph::Status::InvalidOutput,
					 node.Id,
					 {},
					 ReservedSourceOutput(link)
						 ? "PXCX output uses a common source socket whose lifecycle is not represented"
						 : "PXCX output is projected; animation and export settings remain in the source "
						   "archive"}
				);
				break;
			}
		}
		if (archive.MetadataNumber == SUPPORTED_VERSION) {
			for (auto &node : result.Graph.Nodes) {
				const auto *entry = imagegraph::FindCatalogueEntry(node.Type);
				if (!entry) continue;
				const auto saved =
					std::find_if(root.at("nodes").begin(), root.at("nodes").end(), [&](const auto &record) {
						return record.at("id").template get_ref<const std::string &>() == node.Id;
					});
				if (saved == root.at("nodes").end()) continue;
				if (!imagegraph::HasNativeExecutor(node.Type) && node.Type != "pc.hlsl") {
					if (node.Type != "pc.gradient") continue;
					const auto *range = imagegraph::FindCatalogueInput(*entry, "gradient_map_range");
					if (!range || range->SourceIndex != 16 || range->SourceKind != "Vec4" ||
						range->Type != imagegraph::ValueType::Vector4)
						return Fail(failure, "opaque gradient range source schema is not represented");
					const auto *record = Input(*saved, range->SourceIndex);
					if (!record || !record->contains("anim")) continue;
					if (!record->at("anim").is_boolean())
						return Fail(failure, "opaque gradient range animator mode is malformed");
					const std::string_view port = range->Id;
					const bool animated = record->at("anim").template get<bool>();
					if (!operationBudget.Hold(
							sizeof(std::string) + std::max(port.size(), std::string{}.capacity()) + 1
						))
						return Fail(failure, "opaque gradient range mode exceeds operation bounds");
					auto &modes = animated ? node.SourceAnimatedInputs : node.SourceStaticInputs;
					modes.reserve(1);
					modes.emplace_back(port);
					result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
					continue;
				}
				const auto eachSource = [&](const auto &visit) {
					for (const auto &input : entry->Inputs)
						if (input.SourceIndex >= 0 && !visit(input.Id, input.SourceIndex)) return false;
					for (const auto &input : node.DynamicInputs) {
						size_t group;
						const auto *source = imagegraph::FindDynamicTemplate(*entry, input.Id, group);
						if (source && !visit(
										  input.Id,
										  entry->DynamicFixedLength + group * entry->DynamicGroupLength +
											  source->SourceIndex
									  ))
							return false;
					}
					return true;
				};
				size_t animatedCount = 0, staticCount = 0;
				if (!eachSource([&](std::string_view, int32_t sourceIndex) {
						const auto *record = Input(*saved, sourceIndex);
						if (!record || !record->contains("anim")) {
							++staticCount;
							return true;
						}
						if (!record->at("anim").is_boolean())
							return Fail(failure, "source input animator mode is malformed");
						const bool animated = record->at("anim").template get<bool>();
						animatedCount += animated;
						staticCount += !animated;
						return true;
					}))
					return false;
				if (!operationBudget.Hold((animatedCount + staticCount) * sizeof(std::string)))
					return Fail(failure, "source input animator slots exceed operation bounds");
				node.SourceAnimatedInputs.reserve(animatedCount);
				node.SourceStaticInputs.reserve(staticCount);
				if (!eachSource([&](std::string_view port, int32_t sourceIndex) {
						const auto *record = Input(*saved, sourceIndex);
						const bool animated =
							record && record->contains("anim") && record->at("anim").template get<bool>();
						if (!operationBudget.Hold(std::max(port.size(), std::string{}.capacity()) + 1))
							return Fail(failure, "source input animator modes exceed operation bounds");
						(animated ? node.SourceAnimatedInputs : node.SourceStaticInputs).emplace_back(port);
						result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
						return true;
					}))
					return false;
			}
		}
		if (archive.MetadataNumber == SUPPORTED_VERSION &&
			!ProjectGlobals(root, result.Graph, operationBudget, failure))
			return false;
		if (archive.MetadataNumber == SUPPORTED_VERSION &&
			!ProjectOrdinaryGroups(
				root, result, canonicalGroups, failure, *previousDocumentBytes, operationBudget, admitCommon
			))
			return false;
		if (archive.MetadataNumber == SUPPORTED_VERSION) {
			if (!detail::ProjectInlineCollections(root, result.Graph, operationBudget, failure)) return false;
			if (result.GroupPrebinding &&
				!detail::ProjectInlineCollections(root, *result.GroupPrebinding, operationBudget, failure))
				return false;
		}
		if (archive.MetadataNumber == SUPPORTED_VERSION &&
			!ProjectSourceCommonOwners(root, result.Graph, operationBudget, failure))
			return false;

		if (archive.MetadataNumber == SUPPORTED_VERSION && result.GroupPrebinding &&
			!ProjectSourceCommonOwners(root, *result.GroupPrebinding, operationBudget, failure))
			return false;
		if (admitCommon && std::any_of(archive.Links.begin(), archive.Links.end(), CommonSourceRoute)) {
			for (const auto &link : result.Graph.Links) {
				const bool sourceCommon = link.FromPort == "pxcx.update_in_trigger" ||
										  link.FromPort == "pxcx.updated_out_trigger" ||
										  link.FromPort.starts_with("pxcx.metadata.");
				if ((sourceCommon && !detail::SourceCommonNativeAvailable(result.Graph, link.FromNode)) ||
					(link.ToPort == "pxcx.update_in_trigger" &&
					 !detail::SourceCommonNativeAvailable(result.Graph, link.ToNode))) {
					commonAdmissionRejected = true;
					return false;
				}
			}
			imagegraph::Plan commonPlan;
			imagegraph::Diagnostic commonDiagnostic;
			const auto available = operationBudget.Available();
			if (!available) return Fail(failure, "common route admission exceeds import operation bounds");
			const auto status =
				imagegraph::CompileSourceCommonRuntime(result.Graph, commonPlan, commonDiagnostic, available);
			if (status != imagegraph::Status::Ok) {
				if (status != imagegraph::Status::UnsupportedExecution)
					return Fail(failure, commonDiagnostic.Message);
				commonAdmissionRejected = true;
				return false;
			}
		}

		const auto &bootstrapGraph = result.GroupPrebinding ? *result.GroupPrebinding : result.Graph;
		const size_t bootstrapCount =
			std::count_if(bootstrapGraph.Nodes.begin(), bootstrapGraph.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.group_input";
			});
		uint64_t bootstrapBytes = bootstrapCount * sizeof(PxcxGroupBootstrapRecord);
		for (const auto &node : bootstrapGraph.Nodes) {
			if (node.Type != "pc.group_input") continue;
			const uint64_t nameBytes = std::max(node.Id.size(), std::string{}.capacity()) + 1;
			if (nameBytes > UINT64_MAX - bootstrapBytes) {
				failure = "group bootstrap identity storage overflows";
				return false;
			}
			bootstrapBytes += nameBytes;
		}
		if (!operationBudget.Hold(bootstrapBytes)) {
			failure = "group bootstrap records exceed import operation bounds";
			return false;
		}
		result.GroupBootstrap.reserve(bootstrapCount);
		for (const auto &node : bootstrapGraph.Nodes) {
			if (node.Type != "pc.group_input") continue;
			const bool animated =
				std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), "subtype") !=
				node.SourceAnimatedInputs.end();
			result.GroupBootstrap.push_back(
				{std::string(node.Id),
				 animated ? imagegraph::GroupSubtypeAnimator::Animated
						  : imagegraph::GroupSubtypeAnimator::Static}
			);
		}
		const auto nodeById = [&](std::string_view id) -> const Node * {
			for (const auto &node : result.Graph.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		};
		const auto eachInput = [&](const Node &node, const auto &visit) {
			const auto *entry = imagegraph::FindCatalogueEntry(node.Type);
			if (!entry || !imagegraph::HasNativeExecutor(node.Type)) return true;
			for (const auto &input : entry->Inputs)
				if (input.SourceIndex >= 0 &&
					!(node.Type == "pc.group_input" && input.Id == "parent_value") && !visit(input.Id))
					return false;
			for (const auto &input : node.DynamicInputs) {
				size_t group;
				const auto *source = imagegraph::FindDynamicTemplate(*entry, input.Id, group);
				if (source && source->SourceIndex >= 0 && !visit(input.Id)) return false;
			}
			return true;
		};
		const auto ownerFor = [&](const Node &node) -> const Node * {
			const Node *owner = &node;
			for (size_t hop = 0; owner && !owner->InstanceBase.empty() && hop < result.Graph.Nodes.size();
				 ++hop)
				owner = nodeById(owner->InstanceBase);
			return owner && owner->InstanceBase.empty() ? owner : nullptr;
		};
		const auto animatedMode = [](const Node &node, std::string_view port) {
			return std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) !=
						   node.SourceAnimatedInputs.end()
					   ? imagegraph::GroupSubtypeAnimator::Animated
					   : imagegraph::GroupSubtypeAnimator::Static;
		};
		const auto parentInputOwner = [&](const Node &node) -> const Node * {
			const Node *owner = &node;
			for (size_t hop = 0; owner && !owner->SourceParentInputBase.empty(); ++hop) {
				if (hop >= result.Graph.Nodes.size()) return nullptr;
				owner = nodeById(owner->SourceParentInputBase);
				if (owner && owner->Type != "pc.group_input") return nullptr;
			}
			return owner;
		};
		size_t bindingCount = 0;
		uint64_t bindingBytes = 0;
		for (const auto &node : result.Graph.Nodes) {
			if (node.InstanceBase.empty()) continue;
			const Node *owner = ownerFor(node);
			if (!owner || owner->Type != node.Type)
				return Fail(failure, "source input animator owner chain is invalid");
			if (!eachInput(node, [&](std::string_view port) {
					uint64_t bytes = sizeof(imagegraph::GroupSubtypeBinding);
					for (const auto name : {std::string_view(node.Id), std::string_view(owner->Id), port}) {
						const auto text = std::max(name.size(), std::string{}.capacity()) + 1;
						if (text > UINT64_MAX - bytes) return false;
						bytes += text;
					}
					if (bytes > UINT64_MAX - bindingBytes) return false;
					bindingBytes += bytes;
					++bindingCount;
					return true;
				}))
				return Fail(failure, "source animator binding bytes overflow");
		}
		for (const auto &node : result.Graph.Nodes) {
			if (node.SourceParentInputBase.empty()) continue;
			const Node *owner = parentInputOwner(node);
			if (node.Type != "pc.group_input" || !owner)
				return Fail(failure, "source Collection parent input owner chain is invalid");
			uint64_t bytes = sizeof(imagegraph::GroupSubtypeBinding);
			for (const auto name :
				 {std::string_view(node.Id), std::string_view(owner->Id), std::string_view("parent_value")}) {
				const auto text = std::max(name.size(), std::string{}.capacity()) + 1;
				if (text > UINT64_MAX - bytes)
					return Fail(failure, "source Collection parent binding bytes overflow");
				bytes += text;
			}
			if (bytes > UINT64_MAX - bindingBytes)
				return Fail(failure, "source Collection parent binding bytes overflow");
			bindingBytes += bytes;
			++bindingCount;
		}
		if (!operationBudget.Hold(bindingBytes))
			return Fail(failure, "source animator bindings exceed import operation bounds");
		result.GroupBindings.reserve(bindingCount);
		for (const auto &node : result.Graph.Nodes) {
			if (node.InstanceBase.empty()) continue;
			const Node *owner = ownerFor(node);
			if (!eachInput(node, [&](std::string_view port) {
					const Node *getter = &node;
					for (size_t hop = 0;
						 getter && !getter->InstanceBase.empty() && hop < result.Graph.Nodes.size();
						 ++hop) {
						if (std::find(
								getter->InstanceOverrides.begin(), getter->InstanceOverrides.end(), port
							) != getter->InstanceOverrides.end())
							break;
						getter = nodeById(getter->InstanceBase);
					}
					if (!getter) return false;
					result.GroupBindings.push_back(
						{node.Id,
						 owner->Id,
						 animatedMode(*getter, port),
						 animatedMode(*owner, port),
						 std::string(port)}
					);
					return true;
				}))
				return Fail(failure, "source animator getter provenance is absent");
		}
		for (const auto &node : result.Graph.Nodes) {
			if (node.SourceParentInputBase.empty()) continue;
			const Node *owner = parentInputOwner(node);
			if (!owner) return Fail(failure, "source Collection parent input owner chain is invalid");
			const auto mode = animatedMode(*owner, "parent_value");
			result.GroupBindings.push_back({node.Id, owner->Id, mode, mode, "parent_value"});
			result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 10u);
		}

		if (std::any_of(result.Graph.Keyframes.begin(), result.Graph.Keyframes.end(), [](const auto &key) {
				return !key.SourceKeyId.empty();
			}))
			result.Graph.FormatVersion = std::max(result.Graph.FormatVersion, 9u);
		size_t finalKeys = result.Graph.Keyframes.size();
		for (const auto &node : result.Graph.Nodes)
			if (node.SourceSeparatedVec2Animators)
				for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
					for (const auto &axis : input.Axes) {
						if (finalKeys > imagegraph::Limits::MaximumKeyframes ||
							axis.Keys.size() > imagegraph::Limits::MaximumKeyframes - finalKeys)
							return Fail(
								failure,
								"PXC projected ordinary and separated axes exceed aggregate key bounds"
							);
						finalKeys += axis.Keys.size();
					}
		if (!options.FrameCacheLayouts.empty()) {
			const auto cook = [&](imagegraph::Document &graph) {
				const auto before = imagegraph::DocumentRetainedPayloadBytes(graph);
				if (!before) return Fail(failure, "source frame-cache projection payload is invalid");
				graph.FormatVersion = std::max(graph.FormatVersion, 9u);
				imagegraph::Diagnostic diagnostic;
				const auto status = CookSourceFrameCachesLoading(
					graph, options.FrameCacheLayouts, graph, diagnostic, operationBudget.Available()
				);
				if (status != imagegraph::Status::Ok) return Fail(failure, diagnostic.Message);
				const auto after = imagegraph::DocumentRetainedPayloadBytes(graph);
				if (!after || (*after > *before && !operationBudget.Hold(*after - *before)))
					return Fail(failure, "source frame-cache receipts exceed import operation bounds");
				return true;
			};
			if (!cook(result.Graph) || (result.GroupPrebinding && !cook(*result.GroupPrebinding)))
				return false;
		}
		out = std::move(result);
		return true;
	} catch (const std::bad_alloc &) {
		return Fail(failure, "pxcx import allocation failed before installation");
	}

	bool ImportPxcxImageGraph(
		const bake::PxcxArchive &archive,
		PxcxImport &out,
		std::string &failure,
		const PxcxImportOptions &options
	) {
		bool rejected = false;
		if (ImportPxcxImageGraphCandidate(archive, out, failure, options, true, rejected)) return true;
		if (!rejected) return false;
		// Unsupported source lifecycle stays opaque after the provisional candidate has been destroyed.
		return ImportPxcxImageGraphCandidate(archive, out, failure, options, false, rejected);
	}

	PxcxSubgraph ExtractPxcxNativeSubgraph(const PxcxImport &imported, std::string_view nodeId) {
		PxcxSubgraph result;
		result.Graph.FormatVersion = 3;
		if (nodeId.empty() || nodeId.size() > imagegraph::Limits::MaximumTextBytes) {
			result.Diagnostic = {
				imagegraph::Status::InvalidValue,
				std::string(nodeId),
				{},
				"selected node id is empty or exceeds the native text limit"
			};
			result.Compilation = result.Diagnostic.Code;
			return result;
		}
		const auto &nodes = imported.Graph.Nodes;
		const auto &links = imported.Graph.Links;
		if (nodes.size() > imagegraph::Limits::MaximumNodes ||
			links.size() > imagegraph::Limits::MaximumLinks) {
			result.Diagnostic = {
				imagegraph::Status::LimitExceeded,
				std::string(nodeId),
				{},
				"imported graph exceeds native node or link limits"
			};
			result.Compilation = result.Diagnostic.Code;
			return result;
		}
		std::unordered_map<std::string, size_t> indexById;
		indexById.reserve(nodes.size());
		for (size_t index = 0; index < nodes.size(); index++)
			indexById.emplace(nodes[index].Id, index);
		const auto selected = indexById.find(std::string(nodeId));
		if (selected == indexById.end() || !imagegraph::FindSchema(nodes[selected->second].Type)) {
			result.Diagnostic = {
				imagegraph::Status::UnknownNode,
				std::string(nodeId),
				{},
				"selected PXCX node has no native image mapping"
			};
			result.Compilation = result.Diagnostic.Code;
			return result;
		}
		const imagegraph::NodeSchema *schema = imagegraph::FindSchema(nodes[selected->second].Type);
		const std::string_view selectedOutput =
			nodes[selected->second].Type == "image.shape" ? "colored" : "image";
		const bool imageOutput =
			std::any_of(schema->Ports.begin(), schema->Ports.end(), [&](const auto &port) {
				return port.Id == selectedOutput && port.Direction == imagegraph::PortDirection::Output;
			});
		if (!imageOutput) {
			result.Diagnostic = {
				imagegraph::Status::InvalidOutput,
				std::string(nodeId),
				std::string(selectedOutput),
				"selected mapped node has no native image output"
			};
			result.Compilation = result.Diagnostic.Code;
			return result;
		}
		std::vector<std::vector<size_t>> incoming(nodes.size());
		for (size_t index = 0; index < links.size(); index++) {
			const auto target = indexById.find(links[index].ToNode);
			if (target != indexById.end()) incoming[target->second].push_back(index);
		}
		std::vector<bool> included(nodes.size(), false);
		std::vector<size_t> pending{selected->second};
		while (!pending.empty()) {
			const size_t index = pending.back();
			pending.pop_back();
			if (included[index]) continue;
			included[index] = true;
			for (size_t edgeIndex : incoming[index]) {
				const imagegraph::Link &edge = links[edgeIndex];
				const auto source = indexById.find(edge.FromNode);
				if (source == indexById.end()) continue;
				if (imagegraph::FindSchema(nodes[source->second].Type)) {
					pending.push_back(source->second);
				} else {
					result.Cuts.push_back(
						{imagegraph::Status::UnknownNode,
						 edge.ToNode,
						 edge.ToPort,
						 "cut opaque dependency " + edge.FromNode + "." + edge.FromPort + " -> " +
							 edge.ToNode + "." + edge.ToPort}
					);
				}
			}
		}
		result.Graph.Nodes.reserve(nodes.size());
		result.Graph.Links.reserve(links.size());
		for (size_t index = 0; index < nodes.size(); index++)
			if (included[index]) result.Graph.Nodes.push_back(nodes[index]);
		for (const imagegraph::Link &edge : links) {
			const auto source = indexById.find(edge.FromNode);
			const auto target = indexById.find(edge.ToNode);
			if (source != indexById.end() && target != indexById.end() && included[source->second] &&
				included[target->second])
				result.Graph.Links.push_back(edge);
		}
		result.Graph.Outputs.push_back(
			{std::string(nodeId), std::string(nodeId), std::string(selectedOutput)}
		);
		imagegraph::Plan plan;
		result.Compilation = imagegraph::Compile(result.Graph, plan, result.Diagnostic);
		return result;
	}
}
