#include <engine/imagegraphio/PxcxImport.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
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

		std::string_view NativeOutput(std::string_view type, uint32_t index) {
			if (type == "Node_Number") return index == 0 ? "number" : std::string_view{};
			if (type == "Node_Math") return index == 0 ? "result" : std::string_view{};
			if (type == "Node_Shape") {
				constexpr std::array<std::string_view, 4> outputs = {"colored", "mask", "height", "uv"};
				return index < outputs.size() ? outputs[index] : std::string_view{};
			}
			return index == 0 ? "image" : std::string_view{};
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
		bool LinksRepresentable(const bake::PxcxArchive &archive, const bake::PxcxNodeFact &node) {
			for (const bake::PxcxLinkFact &link : archive.Links) {
				if (link.FromNode == node.Id && NativeOutput(node.Type, link.FromIndex).empty()) return false;
				if (link.ToNode == node.Id && NativeInput(node.Type, link.ToInputIndex).empty()) return false;
			}
			return true;
		}
		void OpaqueDiagnostic(PxcxImport &result, const bake::PxcxNodeFact &node, std::string reason) {
			result.Diagnostics.push_back({imagegraph::Status::UnknownNode, node.Id, {}, std::move(reason)});
		}
	}

	bool ImportPxcxImageGraph(const bake::PxcxArchive &archive, PxcxImport &out, std::string &failure) {
		failure.clear();
		if (archive.OriginalBytes.empty())
			return Fail(failure, "pxcx import requires original archive bytes");
		PxcxImport result;
		result.Graph.FormatVersion = 3;
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
		result.Graph.Nodes.reserve(archive.Nodes.size());
		result.Graph.Links.reserve(archive.Links.size());
		result.Diagnostics.reserve(archive.Nodes.size());
		std::unordered_map<std::string, size_t> indexById;
		indexById.reserve(archive.Nodes.size());
		std::vector<bool> native(archive.Nodes.size(), false);
		for (size_t index = 0; index < archive.Nodes.size(); index++) {
			const bake::PxcxNodeFact &fact = archive.Nodes[index];
			const Json &source = sourceNodes[index];
			Node node{fact.Id, Opaque(fact.Type), {}, {fact.X, fact.Y}, {}};
			bool mapped = false;
			if (archive.MetadataNumber == SUPPORTED_VERSION && LinksRepresentable(archive, fact)) {
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
			if (!mapped) {
				node.Type = Opaque(fact.Type);
				node.Values.clear();
				OpaqueDiagnostic(
					result,
					fact,
					archive.MetadataNumber == SUPPORTED_VERSION
						? "PXCX node or control has no supported native mapping; source bytes retained"
						: "PXCX save version has no semantic mapping; source bytes retained"
				);
			} else {
				native[index] = true;
				result.NativeNodes++;
			}
			indexById.emplace(fact.Id, index);
			result.Graph.Nodes.push_back(std::move(node));
		}
		for (const bake::PxcxLinkFact &link : archive.Links) {
			const size_t from = indexById.at(link.FromNode);
			const size_t to = indexById.at(link.ToNode);
			const std::string fromPort =
				native[from] ? std::string(NativeOutput(archive.Nodes[from].Type, link.FromIndex))
							 : OutputPort(link.FromIndex);
			const std::string_view mappedInput =
				native[to] ? NativeInput(archive.Nodes[to].Type, link.ToInputIndex) : std::string_view{};
			const std::string toPort =
				mappedInput.empty() ? InputPort(link.ToInputIndex) : std::string(mappedInput);
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
					 native[from] ? std::string(NativeOutput(archive.Nodes[from].Type, link.FromIndex))
								  : OutputPort(link.FromIndex)}
				);
				result.Diagnostics.push_back(
					{imagegraph::Status::InvalidOutput,
					 node.Id,
					 {},
					 "PXCX output is projected; animation and export settings remain in the source archive"}
				);
				break;
			}
		}
		out = std::move(result);
		return true;
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
