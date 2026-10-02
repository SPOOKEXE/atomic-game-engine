#pragma once

#include "AudioPayload.hpp"
#include "MeshPayload.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>

namespace engine::imagegraph::detail {
	inline constexpr ValueType VALUE_PAYLOAD_TYPES[] = {
		ValueType::Boolean,
		ValueType::Integer,
		ValueType::Scalar,
		ValueType::Text,
		ValueType::Colour,
		ValueType::Vector2,
		ValueType::Array,
		ValueType::Gradient,
		ValueType::Area,
		ValueType::Curve,
		ValueType::Vector4,
		ValueType::Path2D,
		ValueType::Vector3,
		ValueType::Quaternion,
		ValueType::Enum,
		ValueType::AudioBit,
		ValueType::Matrix,
		ValueType::Mesh,
		ValueType::Material3D
	};
	static_assert(std::size(VALUE_PAYLOAD_TYPES) == std::variant_size_v<Value>);
	inline ValueType PayloadType(const Value &value) {
		return VALUE_PAYLOAD_TYPES[value.index()];
	}
	inline bool RepresentableArrayElementType(ValueType type) {
		return type != ValueType::Array &&
			   std::find(std::begin(VALUE_PAYLOAD_TYPES), std::end(VALUE_PAYLOAD_TYPES), type) !=
				   std::end(VALUE_PAYLOAD_TYPES);
	}

	inline std::optional<ElementValue> ArrayElement(const Value &value) {
		return std::visit(
			[](const auto &item) -> std::optional<ElementValue> {
				if constexpr (std::is_same_v<std::decay_t<decltype(item)>, ArrayValue>)
					return std::nullopt;
				else
					return ElementValue{item};
			},
			value
		);
	}

	inline std::optional<ElementValue> ArrayElement(Value &&value) {
		return std::visit(
			[](auto &&item) -> std::optional<ElementValue> {
				if constexpr (std::is_same_v<std::decay_t<decltype(item)>, ArrayValue>)
					return std::nullopt;
				else
					return ElementValue{std::move(item)};
			},
			std::move(value)
		);
	}

	template <class T> inline ValueType PayloadType(const T &) {
		if constexpr (std::is_same_v<T, bool>)
			return ValueType::Boolean;
		else if constexpr (std::is_same_v<T, int64_t>)
			return ValueType::Integer;
		else if constexpr (std::is_same_v<T, double>)
			return ValueType::Scalar;
		else if constexpr (std::is_same_v<T, std::string>)
			return ValueType::Text;
		else if constexpr (std::is_same_v<T, Colour>)
			return ValueType::Colour;
		else if constexpr (std::is_same_v<T, Vector2>)
			return ValueType::Vector2;
		else if constexpr (std::is_same_v<T, Vector3>)
			return ValueType::Vector3;
		else if constexpr (std::is_same_v<T, Vector4>)
			return ValueType::Vector4;
		else if constexpr (std::is_same_v<T, Quaternion>)
			return ValueType::Quaternion;
		else if constexpr (std::is_same_v<T, Gradient>)
			return ValueType::Gradient;
		else if constexpr (std::is_same_v<T, Area>)
			return ValueType::Area;
		else if constexpr (std::is_same_v<T, Curve>)
			return ValueType::Curve;
		else if constexpr (std::is_same_v<T, Path2D>)
			return ValueType::Path2D;
		else if constexpr (std::is_same_v<T, EnumValue>)
			return ValueType::Enum;
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return ValueType::Matrix;
		else if constexpr (std::is_same_v<T, AudioBit>)
			return ValueType::AudioBit;
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return ValueType::Mesh;
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return ValueType::Material3D;
		else {
			static_assert(std::is_same_v<T, ArrayValue>);
			return ValueType::Array;
		}
	}

	template <class T> inline uint64_t PayloadOwnedBytes(const T &item) {
		if constexpr (std::is_same_v<T, std::string>)
			return item.size();
		else if constexpr (std::is_same_v<T, Gradient>)
			return item.Keys.size() * sizeof(engine::imagegraph::GradientKey);
		else if constexpr (std::is_same_v<T, Curve>)
			return item.Anchors.size() * sizeof(std::array<double, 6>);
		else if constexpr (std::is_same_v<T, Path2D>)
			return item.Anchors.size() * sizeof(PathAnchor) + item.Weights.size() * sizeof(PathWeight);
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return item.Values.size() * sizeof(double);
		else if constexpr (std::is_same_v<T, AudioBit>)
			return AudioSampleCount(item.Samples, item.Channels) * sizeof(double) +
				   item.Channels.size() * sizeof(std::vector<double>);
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return MeshStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return MaterialStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, SourceArrayItem>) {
			return std::visit(
				[](const auto &child) -> uint64_t {
					using C = std::decay_t<decltype(child)>;
					if constexpr (std::is_same_v<C, ElementValue>)
						return std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, child);
					else if constexpr (std::is_same_v<C, Image>)
						return child.Pixels.size();
					else {
						uint64_t bytes = child.size() * sizeof(SourceArrayItem);
						for (const auto &entry : child)
							bytes += PayloadOwnedBytes(entry);
						return bytes;
					}
				},
				item.Data
			);
		} else if constexpr (std::is_same_v<T, ArrayValue>) {
			uint64_t bytes = item.Items.size() * sizeof(SourceArrayItem);
			for (const auto &entry : item.Items)
				bytes += PayloadOwnedBytes(entry);
			bytes += item.Elements.size() * sizeof(ElementValue) +
					 item.Nested.size() * sizeof(std::vector<ElementValue>);
			for (const auto &element : item.Elements)
				bytes += std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, element);
			for (const auto &row : item.Nested) {
				bytes += row.size() * sizeof(ElementValue);
				for (const auto &element : row)
					bytes += std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, element);
			}
			return bytes;
		} else
			return 0;
	}

	// Retained storage uses container capacities; logical payload sizing remains size-based above.
	template <class T> inline uint64_t RetainedPayloadBytes(const T &item) {
		if constexpr (std::is_same_v<T, std::string>)
			return item.capacity();
		else if constexpr (std::is_same_v<T, Gradient>)
			return item.Keys.capacity() * sizeof(GradientKey);
		else if constexpr (std::is_same_v<T, Curve>)
			return item.Anchors.capacity() * sizeof(std::array<double, 6>);
		else if constexpr (std::is_same_v<T, Path2D>)
			return item.Anchors.capacity() * sizeof(PathAnchor) +
				   item.Weights.capacity() * sizeof(PathWeight);
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return item.Values.capacity() * sizeof(double);
		else if constexpr (std::is_same_v<T, AudioBit>) {
			uint64_t bytes = item.Samples.capacity() * sizeof(double) +
							 item.Channels.capacity() * sizeof(std::vector<double>);
			for (const auto &channel : item.Channels)
				bytes += channel.capacity() * sizeof(double);
			return bytes;
		} else if constexpr (std::is_same_v<T, MeshValue3D>)
			return MeshStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return MaterialStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, SourceArrayItem>) {
			return std::visit(
				[](const auto &child) -> uint64_t {
					using C = std::decay_t<decltype(child)>;
					if constexpr (std::is_same_v<C, ElementValue>)
						return std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, child);
					else if constexpr (std::is_same_v<C, Image>)
						return child.Pixels.capacity();
					else {
						uint64_t bytes = child.capacity() * sizeof(SourceArrayItem);
						for (const auto &entry : child)
							bytes += RetainedPayloadBytes(entry);
						return bytes;
					}
				},
				item.Data
			);
		} else if constexpr (std::is_same_v<T, ArrayValue>) {
			uint64_t bytes = item.Items.capacity() * sizeof(SourceArrayItem);
			for (const auto &entry : item.Items)
				bytes += RetainedPayloadBytes(entry);
			bytes += item.Elements.capacity() * sizeof(ElementValue) +
					 item.Nested.capacity() * sizeof(std::vector<ElementValue>);
			for (const auto &element : item.Elements)
				bytes += std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, element);
			for (const auto &row : item.Nested) {
				bytes += row.capacity() * sizeof(ElementValue);
				for (const auto &element : row)
					bytes += std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, element);
			}
			return bytes;
		} else if constexpr (std::is_same_v<T, Value>)
			return std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, item);
		else
			return 0;
	}

	inline uint64_t ValuePayloadBytes(const Value &value) {
		return sizeof(Value) + std::visit([](const auto &item) { return PayloadOwnedBytes(item); }, value);
	}

	template <class T> inline bool ValidPayload(const T &item, bool runtime) {
		if constexpr (std::is_same_v<T, double>)
			return std::isfinite(item);
		else if constexpr (std::is_same_v<T, std::string>)
			return item.size() <= Limits::MaximumTextBytes;
		else if constexpr (std::is_same_v<T, Vector2>)
			return std::isfinite(item.X) && std::isfinite(item.Y);
		else if constexpr (std::is_same_v<T, Vector3>)
			return std::isfinite(item.X) && std::isfinite(item.Y) && std::isfinite(item.Z);
		else if constexpr (std::is_same_v<T, Vector4> || std::is_same_v<T, Quaternion>)
			return std::isfinite(item.X) && std::isfinite(item.Y) && std::isfinite(item.Z) &&
				   std::isfinite(item.W);
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return runtime && ValidMeshPayload(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return runtime && ValidMaterialPayload(item);
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return item.Columns > 0 && item.Rows > 0 && item.Values.size() <= Limits::MaximumArrayElements &&
				   uint64_t(item.Columns) * item.Rows == item.Values.size() &&
				   std::all_of(item.Values.begin(), item.Values.end(), [](double number) {
					   return std::isfinite(number);
				   });
		else if constexpr (std::is_same_v<T, AudioBit>)
			return std::isfinite(item.SampleRate) && item.SampleRate > 0 &&
				   ValidAudioPlanes(item.Samples, item.Channels, Limits::MaximumAudioClipSamples);
		else if constexpr (std::is_same_v<T, Gradient>) {
			if (item.Mode > 6 || (!runtime && item.Keys.empty()) ||
				item.Keys.size() > Limits::MaximumGradientKeys)
				return false;
			double previous = -1;
			for (const auto &key : item.Keys) {
				if (!std::isfinite(key.Time) ||
					(!runtime && (key.Time < 0 || key.Time > 1 || key.Time < previous)))
					return false;
				previous = key.Time;
			}
			return true;
		} else if constexpr (std::is_same_v<T, Area>)
			return std::isfinite(item.CenterX) && std::isfinite(item.CenterY) &&
				   std::isfinite(item.HalfWidth) && std::isfinite(item.HalfHeight) && item.Shape <= 1 &&
				   item.Mode <= 2;
		else if constexpr (std::is_same_v<T, Curve>) {
			if (item.Anchors.size() < 2 || item.Anchors.size() > Limits::MaximumCurveAnchors) return false;
			for (double number : item.Header)
				if (!std::isfinite(number)) return false;
			for (const auto &anchor : item.Anchors)
				for (double number : anchor)
					if (!std::isfinite(number)) return false;
			return true;
		} else if constexpr (std::is_same_v<T, Path2D>) {
			if (item.Anchors.size() > Limits::MaximumPathAnchors ||
				item.Weights.size() > Limits::MaximumPathWeights)
				return false;
			for (const auto &anchor : item.Anchors)
				for (double number : anchor.Controls)
					if (!std::isfinite(number)) return false;
			for (const auto &weight : item.Weights)
				if (!std::isfinite(weight.Position) || !std::isfinite(weight.Weight)) return false;
			return true;
		} else if constexpr (std::is_same_v<T, ArrayValue>) {
			if (item.ElementType == ValueType::Any && item.Items.empty() && item.Elements.empty() &&
				item.Nested.empty())
				return true;
			if (!item.Items.empty()) {
				if (item.ElementType != ValueType::Any || !item.Elements.empty() || !item.Nested.empty())
					return false;
				size_t count = 0;
				const auto valid = [&](const auto &self, const auto &items, size_t depth) -> bool {
					if (depth > Limits::MaximumArrayDepth ||
						items.size() > Limits::MaximumArrayElements - count)
						return false;
					count += items.size();
					for (const auto &entry : items) {
						const bool ok = std::visit(
							[&](const auto &child) -> bool {
								using C = std::decay_t<decltype(child)>;
								if constexpr (std::is_same_v<C, ElementValue>)
									return std::visit(
										[&](const auto &leaf) { return ValidPayload(leaf, runtime); }, child
									);
								else if constexpr (std::is_same_v<C, Image>)
									return runtime &&
										   ValidSurfaceLayout(
											   child, Limits::MaximumDimension, Limits::MaximumArrayBytes
										   );
								else
									return self(self, child, depth + 1);
							},
							entry.Data
						);
						if (!ok) return false;
					}
					return true;
				};
				return valid(valid, item.Items, 1) && PayloadOwnedBytes(item) <= Limits::MaximumArrayBytes;
			}
			if (!RepresentableArrayElementType(item.ElementType) ||
				(!item.Nested.empty() && (!runtime || !item.Elements.empty())) ||
				item.Nested.size() > Limits::MaximumArrayElements ||
				item.Elements.size() > Limits::MaximumArrayElements)
				return false;
			size_t count = item.Elements.size();
			auto validRow = [&](const auto &row) {
				for (const auto &element : row) {
					if (!std::visit(
							[&](const auto &leaf) {
								return PayloadType(leaf) == item.ElementType && ValidPayload(leaf, runtime);
							},
							element
						))
						return false;
				}
				return true;
			};
			if (!validRow(item.Elements)) return false;
			for (const auto &row : item.Nested) {
				if (row.size() > Limits::MaximumArrayElements - count) return false;
				count += row.size();
				if (!validRow(row)) return false;
			}
			return PayloadOwnedBytes(item) <= Limits::MaximumArrayBytes;
		} else
			return true;
	}

	inline bool ValidValuePayload(const Value &value, bool runtime) {
		return std::visit([&](const auto &item) { return ValidPayload(item, runtime); }, value);
	}
	inline bool ValidRuntimeValue(const Value &value) {
		return ValidValuePayload(value, true);
	}
}
