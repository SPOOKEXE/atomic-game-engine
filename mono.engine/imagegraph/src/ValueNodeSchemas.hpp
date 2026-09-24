#pragma once

// Private schemas for source-backed, device-independent value nodes.

#include <engine/imagegraph/Document.hpp>

#include <array>
#include <string_view>

namespace engine::imagegraph::detail {
	inline const NodeSchema *FindValueNodeSchema(std::string_view type) {
		using D = PortDirection;
		using T = ValueType;
		static constexpr std::array<PortSchema, 2> NUMBER_PORTS{
			{{"value", T::Scalar, D::Input}, {"number", T::Scalar, D::Output}}
		};
		static constexpr std::array<PropertySchema, 1> NUMBER_PROPS{{{"value", T::Scalar}}};
		static const NodeSchema NUMBER{"value.number", NUMBER_PORTS, NUMBER_PROPS};
		static constexpr std::array<PortSchema, 2> BOOLEAN_PORTS{
			{{"value", T::Boolean, D::Input}, {"boolean", T::Boolean, D::Output}}
		};
		static constexpr std::array<PropertySchema, 1> BOOLEAN_PROPS{{{"value", T::Boolean}}};
		static const NodeSchema BOOLEAN{"value.boolean", BOOLEAN_PORTS, BOOLEAN_PROPS};
		static constexpr std::array<PortSchema, 2> TEXT_PORTS{
			{{"value", T::Text, D::Input}, {"text", T::Text, D::Output}}
		};
		static constexpr std::array<PropertySchema, 1> TEXT_PROPS{{{"value", T::Text}}};
		static const NodeSchema TEXT{"value.text", TEXT_PORTS, TEXT_PROPS};
		static constexpr std::array<PortSchema, 3> VECTOR_PORTS{
			{{"x", T::Scalar, D::Input}, {"y", T::Scalar, D::Input}, {"vector", T::Vector2, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> VECTOR_PROPS{{{"x", T::Scalar}, {"y", T::Scalar}}};
		static const NodeSchema VECTOR{"value.vector2", VECTOR_PORTS, VECTOR_PROPS};
		static constexpr std::array<PortSchema, 6> MATH_PORTS{
			{{"a", T::Scalar, D::Input},
			 {"b", T::Scalar, D::Input},
			 {"amount", T::Scalar, D::Input},
			 {"from", T::Vector2, D::Input},
			 {"to", T::Vector2, D::Input},
			 {"result", T::Scalar, D::Output}}
		};
		static constexpr std::array<PropertySchema, 7> MATH_PROPS{
			{{"a", T::Scalar},
			 {"b", T::Scalar},
			 {"amount", T::Scalar},
			 {"from", T::Vector2},
			 {"to", T::Vector2},
			 {"mode", T::Integer},
			 {"degrees", T::Boolean}}
		};
		static const NodeSchema MATH{"value.math", MATH_PORTS, MATH_PROPS};
		static constexpr std::array<PortSchema, 3> COMPARE_PORTS{
			{{"a", T::Scalar, D::Input}, {"b", T::Scalar, D::Input}, {"result", T::Boolean, D::Output}}
		};
		static constexpr std::array<PropertySchema, 3> COMPARE_PROPS{
			{{"a", T::Scalar}, {"b", T::Scalar}, {"mode", T::Integer}}
		};
		static const NodeSchema COMPARE{"value.compare", COMPARE_PORTS, COMPARE_PROPS};
		static constexpr std::array<PortSchema, 3> LOGIC_PORTS{
			{{"a", T::Boolean, D::Input}, {"b", T::Boolean, D::Input}, {"result", T::Boolean, D::Output}}
		};
		static constexpr std::array<PropertySchema, 3> LOGIC_PROPS{
			{{"a", T::Boolean}, {"b", T::Boolean}, {"mode", T::Integer}}
		};
		static const NodeSchema LOGIC{"value.logic", LOGIC_PORTS, LOGIC_PROPS};
		static constexpr std::array<PortSchema, 5> RGB_PORTS{
			{{"red", T::Scalar, D::Input},
			 {"green", T::Scalar, D::Input},
			 {"blue", T::Scalar, D::Input},
			 {"alpha", T::Scalar, D::Input},
			 {"colour", T::Colour, D::Output}}
		};
		static constexpr std::array<PropertySchema, 5> RGB_PROPS{
			{{"red", T::Scalar},
			 {"green", T::Scalar},
			 {"blue", T::Scalar},
			 {"alpha", T::Scalar},
			 {"normalized", T::Boolean}}
		};
		static const NodeSchema RGB{"value.color_rgb", RGB_PORTS, RGB_PROPS};
		static constexpr std::array<PortSchema, 5> HSV_PORTS{
			{{"hue", T::Scalar, D::Input},
			 {"saturation", T::Scalar, D::Input},
			 {"value", T::Scalar, D::Input},
			 {"alpha", T::Scalar, D::Input},
			 {"colour", T::Colour, D::Output}}
		};
		static constexpr std::array<PropertySchema, 5> HSV_PROPS{
			{{"hue", T::Scalar},
			 {"saturation", T::Scalar},
			 {"value", T::Scalar},
			 {"alpha", T::Scalar},
			 {"normalized", T::Boolean}}
		};
		static const NodeSchema HSV{"value.color_hsv", HSV_PORTS, HSV_PROPS};
		static constexpr std::array<PortSchema, 9> COLOR_DATA_PORTS{
			{{"colour", T::Colour, D::Input},
			 {"red", T::Scalar, D::Output},
			 {"green", T::Scalar, D::Output},
			 {"blue", T::Scalar, D::Output},
			 {"hue", T::Scalar, D::Output},
			 {"saturation", T::Scalar, D::Output},
			 {"value", T::Scalar, D::Output},
			 {"brightness", T::Scalar, D::Output},
			 {"alpha", T::Scalar, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> COLOR_DATA_PROPS{
			{{"colour", T::Colour}, {"normalized", T::Boolean}}
		};
		static const NodeSchema COLOR_DATA{"value.color_data", COLOR_DATA_PORTS, COLOR_DATA_PROPS};
		static constexpr std::array<PortSchema, 4> COLOR_MIX_PORTS{
			{{"from", T::Colour, D::Input},
			 {"to", T::Colour, D::Input},
			 {"mix", T::Scalar, D::Input},
			 {"colour", T::Colour, D::Output}}
		};
		static constexpr std::array<PropertySchema, 4> COLOR_MIX_PROPS{
			{{"from", T::Colour}, {"to", T::Colour}, {"mix", T::Scalar}, {"space", T::Integer}}
		};
		static const NodeSchema COLOR_MIX{"value.color_mix", COLOR_MIX_PORTS, COLOR_MIX_PROPS};
		static constexpr std::array<PortSchema, 2> VEC_UNARY_PORTS{
			{{"vector", T::Vector2, D::Input}, {"result", T::Scalar, D::Output}}
		};
		static constexpr std::array<PropertySchema, 1> VEC_UNARY_PROPS{{{"vector", T::Vector2}}};
		static const NodeSchema MAGNITUDE{"value.vector_magnitude", VEC_UNARY_PORTS, VEC_UNARY_PROPS};
		static constexpr std::array<PortSchema, 2> NORMALIZE_PORTS{
			{{"vector", T::Vector2, D::Input}, {"result", T::Vector2, D::Output}}
		};
		static const NodeSchema NORMALIZE{"value.vector_normalize", NORMALIZE_PORTS, VEC_UNARY_PROPS};
		static constexpr std::array<PropertySchema, 2> DIRECTION_PROPS{
			{{"vector", T::Vector2}, {"radians", T::Boolean}}
		};
		static const NodeSchema DIRECTION{"value.vector_direction", VEC_UNARY_PORTS, DIRECTION_PROPS};
		static constexpr std::array<PortSchema, 3> VEC_BINARY_PORTS{
			{{"a", T::Vector2, D::Input}, {"b", T::Vector2, D::Input}, {"result", T::Scalar, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> VEC_BINARY_PROPS{
			{{"a", T::Vector2}, {"b", T::Vector2}}
		};
		static const NodeSchema DOT{"value.vector_dot", VEC_BINARY_PORTS, VEC_BINARY_PROPS};
		static const NodeSchema CROSS{"value.vector_cross", VEC_BINARY_PORTS, VEC_BINARY_PROPS};
		static constexpr std::array<PortSchema, 3> COUNT_PORTS{
			{{"text", T::Text, D::Input}, {"find", T::Text, D::Input}, {"count", T::Integer, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> COUNT_PROPS{{{"text", T::Text}, {"find", T::Text}}};
		static const NodeSchema COUNT{"value.text_count", COUNT_PORTS, COUNT_PROPS};
		static constexpr std::array<PortSchema, 4> REPLACE_PORTS{
			{{"text", T::Text, D::Input},
			 {"find", T::Text, D::Input},
			 {"replacement", T::Text, D::Input},
			 {"result", T::Text, D::Output}}
		};
		static constexpr std::array<PropertySchema, 4> REPLACE_PROPS{
			{{"text", T::Text}, {"find", T::Text}, {"replacement", T::Text}, {"all", T::Boolean}}
		};
		static const NodeSchema REPLACE{"value.text_replace", REPLACE_PORTS, REPLACE_PROPS};
		static constexpr std::array<PortSchema, 1> COMBINE_PORTS{{{"text", T::Text, D::Output}}};
		static const NodeSchema COMBINE{"value.text_combine", COMBINE_PORTS, {}, true};
		static constexpr std::array<PortSchema, 3> SPLIT_PORTS{
			{{"text", T::Text, D::Input}, {"delimiter", T::Text, D::Input}, {"array", T::Array, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> SPLIT_PROPS{
			{{"text", T::Text}, {"delimiter", T::Text}}
		};
		static const NodeSchema SPLIT{"value.text_split", SPLIT_PORTS, SPLIT_PROPS};
		static constexpr std::array<PortSchema, 3> LENGTH_PORTS{
			{{"text", T::Text, D::Input}, {"mode", T::Integer, D::Input}, {"length", T::Integer, D::Output}}
		};
		static constexpr std::array<PropertySchema, 2> LENGTH_PROPS{
			{{"text", T::Text}, {"mode", T::Integer}}
		};
		static const NodeSchema LENGTH{"value.text_length", LENGTH_PORTS, LENGTH_PROPS};
		static constexpr std::array<PortSchema, 4> TEXT_RANGE_PORTS{
			{{"text", T::Text, D::Input},
			 {"index", T::Integer, D::Input},
			 {"amount", T::Integer, D::Input},
			 {"text", T::Text, D::Output}}
		};
		static constexpr std::array<PropertySchema, 3> TEXT_RANGE_PROPS{
			{{"text", T::Text}, {"index", T::Integer}, {"amount", T::Integer}}
		};
		static const NodeSchema GET_CHAR{"value.text_get_char", TEXT_RANGE_PORTS, TEXT_RANGE_PROPS};
		static const NodeSchema DELETE{"value.text_delete", TEXT_RANGE_PORTS, TEXT_RANGE_PROPS};
		static constexpr std::array<PortSchema, 2> AUDIO_RECORDING_PORTS{
			{{"samples", T::Array, D::Output}, {"audio", T::AudioBit, D::Output}}
		};
		static constexpr std::array<PropertySchema, 1> AUDIO_RECORDING_PROPS{{{"source_id", T::Text}}};
		static const NodeSchema AUDIO_RECORDING{
			"image.audio_recording", AUDIO_RECORDING_PORTS, AUDIO_RECORDING_PROPS
		};
		static constexpr std::array<PortSchema, 2> AUDIO_VOLUME_PORTS{
			{{"samples", T::Array, D::Input}, {"loudness", T::Scalar, D::Output}}
		};
		static const NodeSchema AUDIO_VOLUME{"image.audio_volume", AUDIO_VOLUME_PORTS, {}};
		static constexpr std::array<PortSchema, 8> AUDIO_WINDOW_PORTS{
			{{"audio", T::AudioBit, D::Input},
			 {"samples", T::Array, D::Input},
			 {"width", T::Integer, D::Input},
			 {"location", T::Scalar, D::Input},
			 {"cursor_location", T::Enum, D::Input},
			 {"step", T::Integer, D::Input},
			 {"match_timeline", T::Boolean, D::Input},
			 {"samples", T::Array, D::Output}}
		};
		static constexpr std::array<PropertySchema, 5> AUDIO_WINDOW_PROPS{
			{{"width", T::Integer},
			 {"location", T::Scalar},
			 {"cursor_location", T::Enum},
			 {"step", T::Integer},
			 {"match_timeline", T::Boolean}}
		};
		static const NodeSchema AUDIO_WINDOW{"image.audio_window", AUDIO_WINDOW_PORTS, AUDIO_WINDOW_PROPS};

		static const std::array<const NodeSchema *, 26> SCHEMAS{
			{&NUMBER,		&BOOLEAN,	  &TEXT,   &VECTOR,		&MATH,		&COMPARE,
			 &LOGIC,		&RGB,		  &HSV,	   &COLOR_DATA, &COLOR_MIX, &MAGNITUDE,
			 &NORMALIZE,	&DIRECTION,	  &DOT,	   &CROSS,		&COUNT,		&REPLACE,
			 &COMBINE,		&SPLIT,		  &LENGTH, &GET_CHAR,	&DELETE,	&AUDIO_RECORDING,
			 &AUDIO_VOLUME, &AUDIO_WINDOW}
		};
		for (const NodeSchema *schema : SCHEMAS)
			if (schema->Type == type) return schema;
		return nullptr;
	}
}
