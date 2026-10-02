#pragma once
#include "AudioPayload.hpp"
#include "FluidPayload.hpp"
#include "Mesh2DPayload.hpp"
#include "MeshPayload.hpp"
#include "PixelBuilderPayload.hpp"
#include "ScenePayload.hpp"
#include "SdfPayload.hpp"
#include "SourcePathPayload.hpp"
#include "SourcePathPayload3D.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>

namespace engine::imagegraph::detail {
	inline constexpr ValueType VALUE_PAYLOAD_TYPES[] = {
		ValueType::Boolean, ValueType::Integer,	   ValueType::Scalar,		  ValueType::Text,
		ValueType::Colour,	ValueType::Vector2,	   ValueType::Array,		  ValueType::Gradient,
		ValueType::Area,	ValueType::Curve,	   ValueType::Vector4,		  ValueType::Path2D,
		ValueType::Vector3, ValueType::Quaternion, ValueType::Enum,			  ValueType::AudioBit,
		ValueType::Matrix,	ValueType::Mesh,	   ValueType::Material3D,	  ValueType::Light3D,
		ValueType::Scene3D, ValueType::Mesh2D,	   ValueType::Struct,		  ValueType::Any,
		ValueType::Image,	ValueType::Buffer,	   ValueType::NodeRef,		  ValueType::Path3D,
		ValueType::PcxNode, ValueType::PixelBox,   ValueType::DynamicSurface, ValueType::Array,
		ValueType::Sdf,		ValueType::FluidDomain
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
		if constexpr (std::is_same_v<T, ArraySelectorValue>)
			return ValueType::Array;
		else if constexpr (std::is_same_v<T, bool>)
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
		else if constexpr (std::is_same_v<T, PcxExpressionValue>)
			return ValueType::PcxNode;
		else if constexpr (std::is_same_v<T, PixelBoxValue>)
			return ValueType::PixelBox;
		else if constexpr (std::is_same_v<T, DynamicSurfaceValue>)
			return ValueType::DynamicSurface;
		else if constexpr (std::is_same_v<T, PathValue3D>)
			return ValueType::Path3D;
		else if constexpr (std::is_same_v<T, Path2D>)
			return ValueType::Path2D;
		else if constexpr (std::is_same_v<T, EnumValue>)
			return ValueType::Enum;
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return ValueType::Matrix;
		else if constexpr (std::is_same_v<T, AudioBit>)
			return ValueType::AudioBit;
		else if constexpr (std::is_same_v<T, MeshValue2D>)
			return ValueType::Mesh2D;
		else if constexpr (std::is_same_v<T, StructValue>)
			return ValueType::Struct;
		else if constexpr (std::is_same_v<T, UndefinedValue>)
			return ValueType::Any;
		else if constexpr (std::is_same_v<T, SurfaceValue>)
			return ValueType::Image;
		else if constexpr (std::is_same_v<T, BufferValue>)
			return ValueType::Buffer;
		else if constexpr (std::is_same_v<T, ExecutionThreadValue>)
			return ValueType::NodeRef;
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return ValueType::Mesh;
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return ValueType::Material3D;
		else if constexpr (std::is_same_v<T, LightValue3D>)
			return ValueType::Light3D;
		else if constexpr (std::is_same_v<T, SceneValue3D>)
			return ValueType::Scene3D;
		else if constexpr (std::is_same_v<T, SdfValue>)
			return ValueType::Sdf;
		else if constexpr (std::is_same_v<T, FluidDomainValue>)
			return ValueType::FluidDomain;
		else {
			static_assert(std::is_same_v<T, ArrayValue>);
			return ValueType::Array;
		}
	}

	template <class T> inline uint64_t PayloadOwnedBytes(const T &item);
	template <class T> inline uint64_t RetainedPayloadBytes(const T &item);
	template <class T> inline bool ValidPayload(const T &item, bool runtime);
	inline bool ValidStructPayload(const StructValue &item);
	inline bool ValidPcxPayload(const PcxExpressionValue &item);
	inline bool ValidSelectorPayload(const ArraySelectorValue &item);

	template <class T> inline uint64_t PayloadOwnedBytes(const T &item) {
		if constexpr (std::is_same_v<T, DynamicSurfaceValue>)
			return PixelBuilderStorageBytes(item, false);
		else if constexpr (std::is_same_v<T, PixelBoxValue>)
			return item.Data ? sizeof(PixelBoxData) : 0;
		else if constexpr (std::is_same_v<T, ArraySelectorValue>)
			return item.Data ? sizeof(ArraySelectorData) + PayloadOwnedBytes(item.Data->Values) +
								   item.Data->CumulativeWeights.size() * sizeof(double)
							 : 0;
		else if constexpr (std::is_same_v<T, SurfaceValue>)
			return item.Data.Pixels.size();
		else if constexpr (std::is_same_v<T, BufferValue>)
			return item.Bytes.size();
		else if constexpr (std::is_same_v<T, ExecutionThreadValue>)
			return item.Id.size();
		else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
			if (!item.Data) return 0;
			uint64_t bytes = sizeof(PcxExpressionData) +
							 item.Data->Instructions.size() * sizeof(PcxInstruction) +
							 item.Data->Bindings.size() * sizeof(std::pair<std::string, Value>);
			for (const auto &node : item.Data->Instructions)
				bytes += node.Operation.size() + node.Arguments.size() * sizeof(uint32_t) +
						 std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, node.Literal);
			for (const auto &[name, value] : item.Data->Bindings)
				bytes +=
					name.size() + std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, value);
			return bytes;
		} else if constexpr (std::is_same_v<T, StructValue>) {
			if (!item.Data) return 0;
			uint64_t bytes =
				sizeof(StructData) + item.Data->Fields.size() * sizeof(std::pair<std::string, Value>);
			for (const auto &[key, value] : item.Data->Fields)
				bytes +=
					key.size() + std::visit([](const auto &leaf) { return PayloadOwnedBytes(leaf); }, value);
			return bytes;
		} else if constexpr (std::is_same_v<T, std::string>)
			return item.size();
		else if constexpr (std::is_same_v<T, Gradient>)
			return item.Keys.size() * sizeof(engine::imagegraph::GradientKey);
		else if constexpr (std::is_same_v<T, Curve>)
			return item.Anchors.size() * sizeof(std::array<double, 6>);
		else if constexpr (std::is_same_v<T, Path2D>)
			return SourcePath2DBytes<false>(item);
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return item.Values.size() * sizeof(double);
		else if constexpr (std::is_same_v<T, AudioBit>)
			return AudioSampleCount(item.Samples, item.Channels) * sizeof(double) +
				   item.Channels.size() * sizeof(std::vector<double>);
		else if constexpr (std::is_same_v<T, MeshValue2D>)
			return Mesh2DStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, PathValue3D>)
			return item.Data ? SourcePath3DBytes<true>(*item.Data) : 0;
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return MeshStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return MaterialStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, LightValue3D>)
			return item.Data ? sizeof(LightData3D) : 0;
		else if constexpr (std::is_same_v<T, SceneValue3D>)
			return SceneStorageBytes<false>(item);
		else if constexpr (std::is_same_v<T, SdfValue>)
			return SdfStorageBytes(item, false);
		else if constexpr (std::is_same_v<T, FluidDomainValue>)
			return FluidStorageBytes<false>(item);
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
		if constexpr (std::is_same_v<T, DynamicSurfaceValue>)
			return PixelBuilderStorageBytes(item, true);
		else if constexpr (std::is_same_v<T, PixelBoxValue>)
			return item.Data ? sizeof(PixelBoxData) : 0;
		else if constexpr (std::is_same_v<T, ArraySelectorValue>)
			return item.Data ? sizeof(ArraySelectorData) + RetainedPayloadBytes(item.Data->Values) +
								   item.Data->CumulativeWeights.capacity() * sizeof(double)
							 : 0;
		else if constexpr (std::is_same_v<T, SurfaceValue>)
			return item.Data.Pixels.capacity();
		else if constexpr (std::is_same_v<T, BufferValue>)
			return item.Bytes.capacity();
		else if constexpr (std::is_same_v<T, ExecutionThreadValue>)
			return item.Id.capacity();
		else if constexpr (std::is_same_v<T, PcxExpressionValue>) {
			if (!item.Data) return 0;
			uint64_t bytes = sizeof(PcxExpressionData) +
							 item.Data->Instructions.capacity() * sizeof(PcxInstruction) +
							 item.Data->Bindings.capacity() * sizeof(std::pair<std::string, Value>);
			for (const auto &node : item.Data->Instructions)
				bytes +=
					node.Operation.capacity() + node.Arguments.capacity() * sizeof(uint32_t) +
					std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, node.Literal);
			for (const auto &[name, value] : item.Data->Bindings)
				bytes += name.capacity() +
						 std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, value);
			return bytes;
		} else if constexpr (std::is_same_v<T, StructValue>) {
			if (!item.Data) return 0;
			uint64_t bytes =
				sizeof(StructData) + item.Data->Fields.capacity() * sizeof(std::pair<std::string, Value>);
			for (const auto &[key, value] : item.Data->Fields)
				bytes += key.capacity() +
						 std::visit([](const auto &leaf) { return RetainedPayloadBytes(leaf); }, value);
			return bytes;
		} else if constexpr (std::is_same_v<T, std::string>)
			return item.capacity();
		else if constexpr (std::is_same_v<T, Gradient>)
			return item.Keys.capacity() * sizeof(GradientKey);
		else if constexpr (std::is_same_v<T, Curve>)
			return item.Anchors.capacity() * sizeof(std::array<double, 6>);
		else if constexpr (std::is_same_v<T, Path2D>)
			return SourcePath2DBytes<true>(item);
		else if constexpr (std::is_same_v<T, MatrixValue>)
			return item.Values.capacity() * sizeof(double);
		else if constexpr (std::is_same_v<T, AudioBit>) {
			uint64_t bytes = item.Samples.capacity() * sizeof(double) +
							 item.Channels.capacity() * sizeof(std::vector<double>);
			for (const auto &channel : item.Channels)
				bytes += channel.capacity() * sizeof(double);
			return bytes;
		} else if constexpr (std::is_same_v<T, MeshValue2D>)
			return Mesh2DStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, PathValue3D>)
			return item.Data ? sizeof(PathData3D) + item.Data->Anchors.capacity() * sizeof(PathAnchor3D) +
								   item.Data->Transforms.capacity() * sizeof(PathTransform3D) +
								   (item.Data->Source2D ? RetainedPayloadBytes(*item.Data->Source2D) : 0)
							 : 0;
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return MeshStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return MaterialStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, LightValue3D>)
			return item.Data ? sizeof(LightData3D) : 0;
		else if constexpr (std::is_same_v<T, SceneValue3D>)
			return SceneStorageBytes<true>(item);
		else if constexpr (std::is_same_v<T, SdfValue>)
			return SdfStorageBytes(item, true);
		else if constexpr (std::is_same_v<T, FluidDomainValue>)
			return FluidStorageBytes<true>(item);
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
		if constexpr (std::is_same_v<T, ArraySelectorValue>)
			return runtime && ValidSelectorPayload(item);
		else if constexpr (std::is_same_v<T, UndefinedValue>)
			return runtime;
		else if constexpr (std::is_same_v<T, SurfaceValue>)
			return runtime &&
				   ValidSurfaceLayout(item.Data, Limits::MaximumDimension, Limits::MaximumArrayBytes) &&
				   FiniteSurfaceSamples(item.Data);
		else if constexpr (std::is_same_v<T, BufferValue>)
			return runtime && item.Bytes.size() <= Limits::MaximumArrayBytes;
		else if constexpr (std::is_same_v<T, ExecutionThreadValue>)
			return runtime && !item.Id.empty() && item.Id.size() <= Limits::MaximumTextBytes;
		else if constexpr (std::is_same_v<T, DynamicSurfaceValue>)
			return runtime && ValidPixelBuilderPayload(item);
		else if constexpr (std::is_same_v<T, PixelBoxValue>) {
			if (!item.Data) return true;
			for (double component : item.Data->BaseBounds)
				if (!std::isfinite(component)) return false;
			if (item.Data->FixedBounds)
				for (double component : *item.Data->FixedBounds)
					if (!std::isfinite(component)) return false;
			for (double component : item.Data->Anchors)
				if (!std::isfinite(component)) return false;
			for (double component : item.Data->DimensionBounds)
				if (!std::isfinite(component)) return false;
			for (uint8_t mode : item.Data->AnchorModes)
				if (mode > 3) return false;
			for (uint8_t mode : item.Data->PreviousAnchorModes)
				if (mode > 3) return false;
			return item.Data->DimensionBoundModes[0] == 0 && item.Data->DimensionBoundModes[1] == 0;
		} else if constexpr (std::is_same_v<T, PcxExpressionValue>)
			return runtime && ValidPcxPayload(item);
		else if constexpr (std::is_same_v<T, StructValue>)
			return runtime && ValidStructPayload(item);
		else if constexpr (std::is_same_v<T, double>)
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
		else if constexpr (std::is_same_v<T, MeshValue2D>)
			return runtime && ValidMesh2DPayload(item);
		else if constexpr (std::is_same_v<T, MeshValue3D>)
			return runtime && ValidMeshPayload(item);
		else if constexpr (std::is_same_v<T, MaterialValue3D>)
			return runtime && ValidMaterialPayload(item);
		else if constexpr (std::is_same_v<T, LightValue3D>)
			return runtime && ValidLightPayload(item);
		else if constexpr (std::is_same_v<T, SceneValue3D>)
			return runtime && ValidScenePayload(item);
		else if constexpr (std::is_same_v<T, SdfValue>)
			return runtime && ValidSdfPayload(item);
		else if constexpr (std::is_same_v<T, FluidDomainValue>)
			return runtime && ValidFluidPayload(item);
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
		} else if constexpr (std::is_same_v<T, PathValue3D>) {
			return !item.Data || ValidSourcePath3D(*item.Data);
		} else if constexpr (std::is_same_v<T, Path2D>) {
			return ValidSourcePath2D(item);
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
										   ) &&
										   FiniteSurfaceSamples(child);
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

	inline bool ValidSelectorPayload(const ArraySelectorValue &item) {
		if (!item.Data || !std::isfinite(item.Data->TotalWeight)) return false;
		const auto &array = item.Data->Values;
		const auto &weights = item.Data->CumulativeWeights;
		const size_t count = !array.Items.empty()	 ? array.Items.size()
							 : !array.Nested.empty() ? array.Nested.size()
													 : array.Elements.size();
		if (weights.size() != count || count > Limits::MaximumArrayElements) return false;
		for (double weight : weights)
			if (!std::isfinite(weight)) return false;
		const auto noSelector = [&](const auto &self, const auto &items, size_t depth) -> bool {
			if (depth > Limits::MaximumArrayDepth) return false;
			for (const auto &entry : items) {
				if (const auto *leaf = std::get_if<ElementValue>(&entry.Data)) {
					if (std::holds_alternative<ArraySelectorValue>(*leaf)) return false;
				} else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&entry.Data)) {
					if (!self(self, *children, depth + 1)) return false;
				}
			}
			return true;
		};
		if (!noSelector(noSelector, array.Items, 1)) return false;
		for (const auto &leaf : array.Elements)
			if (std::holds_alternative<ArraySelectorValue>(leaf)) return false;
		for (const auto &row : array.Nested)
			for (const auto &leaf : row)
				if (std::holds_alternative<ArraySelectorValue>(leaf)) return false;
		return ValidPayload(array, true) && PayloadOwnedBytes(item) <= Limits::MaximumArrayBytes;
	}

	inline bool ValidStructPayload(const StructValue &item) {
		size_t count = 0;
		uint64_t bytes = 0;
		const auto valid = [&](const auto &self, const auto &leaf, size_t depth) -> bool {
			using T = std::decay_t<decltype(leaf)>;
			if (depth > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements) return false;
			if constexpr (std::is_same_v<T, StructValue>) {
				if (!leaf.Data) return true;
				bytes +=
					sizeof(StructData) + leaf.Data->Fields.size() * sizeof(std::pair<std::string, Value>);
				if (bytes > Limits::MaximumArrayBytes) return false;
				for (const auto &[key, value] : leaf.Data->Fields) {
					if (key.size() > Limits::MaximumTextBytes) return false;
					bytes += key.size();
					if (bytes > Limits::MaximumArrayBytes ||
						!std::visit([&](const auto &child) { return self(self, child, depth + 1); }, value))
						return false;
				}
				return true;
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				bytes += leaf.Elements.size() * sizeof(ElementValue) +
						 leaf.Nested.size() * sizeof(std::vector<ElementValue>);
				for (const auto &row : leaf.Nested)
					bytes += row.size() * sizeof(ElementValue);
				if (bytes > Limits::MaximumArrayBytes) return false;
				if (!leaf.Items.empty()) {
					const auto visitItems = [&](const auto &visit, const auto &items, size_t level) -> bool {
						if (level > Limits::MaximumArrayDepth) return false;
						bytes += items.size() * sizeof(SourceArrayItem);
						if (bytes > Limits::MaximumArrayBytes) return false;
						for (const auto &entry : items) {
							if (++count > Limits::MaximumArrayElements) return false;
							if (!std::visit(
									[&](const auto &child) -> bool {
										using C = std::decay_t<decltype(child)>;
										if constexpr (std::is_same_v<C, ElementValue>)
											return std::visit(
												[&](const auto &element) {
													return self(self, element, level + 1);
												},
												child
											);
										else if constexpr (std::is_same_v<C, Image>) {
											bytes += child.Pixels.size();
											return bytes <= Limits::MaximumArrayBytes &&
												   FiniteSurfaceSamples(child) &&
												   ValidSurfaceLayout(
													   child,
													   Limits::MaximumDimension,
													   Limits::MaximumArrayBytes
												   );
										} else
											return visit(visit, child, level + 1);
									},
									entry.Data
								))
								return false;
						}
						return true;
					};
					return visitItems(visitItems, leaf.Items, depth + 1);
				}
				for (const auto &element : leaf.Elements)
					if (!std::visit([&](const auto &child) { return self(self, child, depth + 1); }, element))
						return false;
				for (const auto &row : leaf.Nested)
					for (const auto &element : row)
						if (!std::visit(
								[&](const auto &child) { return self(self, child, depth + 2); }, element
							))
							return false;
				return true;
			} else {
				bytes += PayloadOwnedBytes(leaf);
				return bytes <= Limits::MaximumArrayBytes && ValidPayload(leaf, true);
			}
		};
		return valid(valid, item, 1);
	}

	inline bool ValidPcxPayload(const PcxExpressionValue &item) {
		size_t count = 0;
		uint64_t bytes = 0;
		const auto visit = [&](const auto &self, const auto &value, size_t depth) -> bool {
			using T = std::decay_t<decltype(value)>;
			if (depth > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements) return false;
			if constexpr (std::is_same_v<T, PcxExpressionValue>) {
				if (!value.Data || value.Data->Instructions.empty() ||
					value.Data->Instructions.size() > 4096 ||
					value.Data->Root >= value.Data->Instructions.size())
					return false;
				bytes += sizeof(PcxExpressionData) +
						 value.Data->Instructions.size() * sizeof(PcxInstruction) +
						 value.Data->Bindings.size() * sizeof(std::pair<std::string, Value>);
				for (size_t i = 0; i < value.Data->Instructions.size(); ++i) {
					const auto &node = value.Data->Instructions[i];
					bytes += node.Operation.size() + node.Arguments.size() * sizeof(uint32_t);
					if (node.Operation.size() > Limits::MaximumTextBytes || node.Arguments.size() > 4096)
						return false;
					for (auto child : node.Arguments)
						if (child >= i) return false;
					if (!std::visit(
							[&](const auto &leaf) { return self(self, leaf, depth + 1); }, node.Literal
						))
						return false;
				}
				for (const auto &[name, child] : value.Data->Bindings) {
					bytes += name.size();
					if (name.size() > Limits::MaximumTextBytes ||
						!std::visit([&](const auto &leaf) { return self(self, leaf, depth + 1); }, child))
						return false;
				}
			} else if constexpr (std::is_same_v<T, StructValue>) {
				if (value.Data) {
					bytes += sizeof(StructData) +
							 value.Data->Fields.size() * sizeof(std::pair<std::string, Value>);
					for (const auto &[name, child] : value.Data->Fields) {
						bytes += name.size();
						if (!std::visit([&](const auto &leaf) { return self(self, leaf, depth + 1); }, child))
							return false;
					}
				}
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				const auto items = [&](const auto &nested, const auto &entries, size_t level) -> bool {
					if (level > Limits::MaximumArrayDepth) return false;
					bytes += entries.size() * sizeof(SourceArrayItem);
					for (const auto &entry : entries) {
						if (++count > Limits::MaximumArrayElements ||
							!std::visit(
								[&](const auto &child) -> bool {
									using C = std::decay_t<decltype(child)>;
									if constexpr (std::is_same_v<C, ElementValue>)
										return std::visit(
											[&](const auto &leaf) { return self(self, leaf, level + 1); },
											child
										);
									else if constexpr (std::is_same_v<C, Image>) {
										bytes += child.Pixels.size();
										return bytes <= Limits::MaximumArrayBytes &&
											   ValidSurfaceLayout(
												   child, Limits::MaximumDimension, Limits::MaximumArrayBytes
											   ) &&
											   FiniteSurfaceSamples(child);
									} else
										return nested(nested, child, level + 1);
								},
								entry.Data
							))
							return false;
					}
					return bytes <= Limits::MaximumArrayBytes;
				};
				if (!value.Items.empty()) return items(items, value.Items, depth + 1);
				bytes += value.Elements.size() * sizeof(ElementValue) +
						 value.Nested.size() * sizeof(std::vector<ElementValue>);
				for (const auto &element : value.Elements)
					if (!std::visit([&](const auto &child) { return self(self, child, depth + 1); }, element))
						return false;
				for (const auto &row : value.Nested) {
					bytes += row.size() * sizeof(ElementValue);
					for (const auto &element : row)
						if (!std::visit(
								[&](const auto &child) { return self(self, child, depth + 2); }, element
							))
							return false;
				}
			} else {
				bytes += PayloadOwnedBytes(value);
				if (!ValidPayload(value, true)) return false;
			}
			return bytes <= Limits::MaximumArrayBytes;
		};
		return visit(visit, item, 1);
	}

	inline bool ValidValuePayload(const Value &value, bool runtime) {
		return std::visit([&](const auto &item) { return ValidPayload(item, runtime); }, value);
	}
	inline bool ValidRuntimeValue(const Value &value) {
		return ValidValuePayload(value, true);
	}
}
