#pragma once
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace studio::detail {
	inline std::optional<int64_t> HlslArgumentKind(const engine::imagegraph::Value &value) {
		using namespace engine::imagegraph;
		if (const auto *choice = std::get_if<EnumValue>(&value)) return choice->Value;
		if (const auto *integer = std::get_if<int64_t>(&value)) return *integer;
		if (const auto *scalar = std::get_if<double>(&value); scalar && std::isfinite(*scalar) &&
															  *scalar >= 0 && *scalar <= 8 &&
															  std::floor(*scalar) == *scalar)
			return int64_t(*scalar);
		return std::nullopt;
	}
	inline std::optional<engine::imagegraph::ValueType> HlslArgumentType(int64_t kind) {
		using engine::imagegraph::ValueType;
		if (kind < 0 || kind > 8) return {};
		constexpr ValueType types[] = {
			ValueType::Scalar,
			ValueType::Integer,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Array,
			ValueType::Image,
			ValueType::Colour
		};
		return types[kind];
	}
	inline bool HlslRefreshControl(std::string_view port) {
		return port.starts_with("argument_type_") || port.starts_with("argument_name_");
	}
	inline size_t HlslArrayLength(const engine::imagegraph::ArrayValue &value) {
		if (!value.Items.empty()) return value.Items.size();
		if (!value.Nested.empty()) return value.Nested.size();
		return value.Elements.size();
	}
	// Source overrideValue replaces the animator only when the current shape needs normalization.
	inline std::optional<engine::imagegraph::Value>
	HlslArgumentOverride(int64_t kind, const engine::imagegraph::Value &current) {
		using namespace engine::imagegraph;
		const auto *array = std::get_if<ArrayValue>(&current);
		if (kind >= 2 && kind <= 6) {
			constexpr size_t lengths[]{2, 3, 4, 9, 16};
			const size_t length = lengths[kind - 2];
			if (array && HlslArrayLength(*array) == length) return {};
			ArrayValue zeros;
			zeros.ElementType = ValueType::Scalar;
			zeros.Elements.assign(length, 0.0);
			return Value{std::move(zeros)};
		}
		if (!array) return {};
		if (kind == 7) return Value{int64_t{-4}};
		if (kind == 8) return Value{int64_t{0}};
		return Value{0.0};
	}
	inline bool StageHlslArgumentRefresh(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		const engine::imagegraph::DynamicInput &edited,
		const engine::imagegraph::EvaluationSnapshot &inputs,
		engine::imagegraph::Diagnostic &error,
		std::vector<engine::imagegraph::SourceAnimatorReplacement> *replacedAnimators = nullptr
	) {
		using namespace engine::imagegraph;
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &held) {
			return held.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.hlsl" || !HlslRefreshControl(edited.Id))
			return true;
		if (edited.Default)
			for (auto &value : node->Values)
				if (value.Port == edited.Id) value.Data = *edited.Default;
		const auto resolved = [&](std::string_view port) -> const Value * {
			if (edited.Id == port && edited.Default) return &*edited.Default;
			for (const auto &input : inputs.Values())
				if (input.Port == port) return &input.Data;
			return nullptr;
		};
		for (auto &input : node->DynamicInputs) {
			constexpr std::string_view prefix = "argument_value_";
			if (!input.Id.starts_with(prefix)) continue;
			const std::string suffix(input.Id.substr(prefix.size()));
			const auto *selector = resolved("argument_type_" + suffix);
			const auto *current = resolved(input.Id);
			const auto *name = resolved("argument_name_" + suffix);
			const auto kind = selector ? HlslArgumentKind(*selector) : std::nullopt;
			const auto type = kind ? HlslArgumentType(*kind) : std::nullopt;
			if (!type || !name || !std::get_if<std::string>(name)) {
				error = {
					Status::InvalidValue,
					std::string(nodeId),
					input.Id,
					"Shader refresh requires the captured argument name and type"
				};
				return false;
			}
			if (std::get<std::string>(*name).empty()) {
				error = {
					Status::UnsupportedExecution,
					std::string(nodeId),
					"argument_name_" + suffix,
					"Empty source shader names delete their argument group; remove the group explicitly"
				};
				return false;
			}
			const bool image =
				std::any_of(inputs.Images().begin(), inputs.Images().end(), [&](const auto &held) {
					return held.Port == input.Id;
				});
			if (!current && !image) {
				error = {
					Status::InvalidValue,
					std::string(nodeId),
					input.Id,
					"Shader refresh requires one captured current argument generation"
				};
				return false;
			}
			const bool changedType = input.Type != *type;
			input.Type = *type;
			if (changedType)
				std::erase_if(document.Links, [&](const auto &link) {
					return link.ToNode == nodeId && link.ToPort == input.Id;
				});
			const Value surfaceSentinel = int64_t{-4};
			auto replacement = HlslArgumentOverride(*kind, current ? *current : surfaceSentinel);
			if (!replacement) continue;
			if (replacedAnimators) replacedAnimators->push_back({node->Id, input.Id});
			input.Default = *replacement;
			for (auto &value : node->Values)
				if (value.Port == input.Id) value.Data = *replacement;
			std::erase_if(document.Keyframes, [&](const auto &key) {
				return key.NodeId == nodeId && key.Port == input.Id;
			});
			if (document.Keyframes.size() >= Limits::MaximumKeyframes) {
				error = {
					Status::LimitExceeded, std::string(nodeId), input.Id, "Shader animator key limit reached"
				};
				return false;
			}
			document.Keyframes.push_back(
				{std::string(nodeId), input.Id, 0, std::move(*replacement), "source", KeyframeEase{}}
			);
			if (std::none_of(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
					return track.NodeId == nodeId && track.Port == input.Id;
				})) {
				if (document.Tracks.size() >= Limits::MaximumTracks) {
					error = {
						Status::LimitExceeded,
						std::string(nodeId),
						input.Id,
						"Shader animator track limit reached"
					};
					return false;
				}
				document.Tracks.push_back({std::string(nodeId), input.Id, "wrap", -1});
			}
			document.FormatVersion = std::max(document.FormatVersion, 9u);
		}
		return true;
	}
}
