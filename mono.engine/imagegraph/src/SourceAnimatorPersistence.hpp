#pragma once

#include "SourceSeparatedVec2.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	inline bool SourceAnimatorAdd(uint64_t &bytes, uint64_t amount) {
		if (amount > UINT64_MAX - bytes) return false;
		bytes += amount;
		return true;
	}

	inline bool SourceAnimatorText(uint64_t &bytes, const std::string &text, bool clone) {
		if (text.size() > Limits::MaximumTextBytes) return false;
		const uint64_t amount = clone ? std::max(text.size(), std::string{}.capacity()) : text.capacity();
		return SourceAnimatorAdd(bytes, amount);
	}

	inline bool SourceAnimatorKey(uint64_t &bytes, const Keyframe &key, bool clone, size_t &aggregateKeys) {
		const auto payload = KeyframePayloadBytes(key);
		if (!payload || key.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes ||
			key.NodeId.size() > Limits::MaximumTextBytes || key.Port.size() > Limits::MaximumTextBytes ||
			key.Interpolation.size() > Limits::MaximumTextBytes || !ValidFrameTime(GetFrameTime(key)) ||
			(key.Kind != KeyframeKind::Normal && key.Kind != KeyframeKind::Adder) ||
			(key.Interpolation != "source" && key.Interpolation != "step" && key.Interpolation != "linear" &&
			 key.Interpolation != "cubic") ||
			(key.Interpolation == "source") != key.Ease.has_value() ||
			aggregateKeys == Limits::MaximumKeyframes)
			return false;
		++aggregateKeys;
		if (key.Ease) {
			if (key.Ease->InType.size() > Limits::MaximumTextBytes ||
				key.Ease->OutType.size() > Limits::MaximumTextBytes)
				return false;
			const auto validSide = [](std::string_view side) {
				return side == "linear" || side == "bezier" || side == "cut";
			};
			if (!validSide(key.Ease->InType) || !validSide(key.Ease->OutType) ||
				!std::isfinite(key.Ease->In.X) || !std::isfinite(key.Ease->In.Y) ||
				!std::isfinite(key.Ease->Out.X) || !std::isfinite(key.Ease->Out.Y))
				return false;
		}
		if (key.SourceDriver && (key.Interpolation != "source" || key.SineDriver ||
								 !ValidKeyframeSourceDriver(*key.SourceDriver)))
			return false;
		if (key.SourceDriver)
			if (const auto *audio = std::get_if<KeyframeAudioDriver>(&*key.SourceDriver);
				audio && (audio->SourceId.size() > Limits::MaximumTextBytes ||
						  audio->Metric.size() > Limits::MaximumTextBytes))
				return false;
		if (key.SineDriver) {
			const auto &driver = *key.SineDriver;
			if (key.Interpolation != "source" || !std::isfinite(driver.Frequency) ||
				!std::isfinite(driver.Amplitude) || !std::isfinite(driver.Phase) ||
				!std::isfinite(driver.Smooth))
				return false;
		}
		if (clone) {
			return SourceAnimatorAdd(bytes, *payload - sizeof(Keyframe));
		}
		if (!SourceAnimatorText(bytes, key.NodeId, false) || !SourceAnimatorText(bytes, key.Port, false) ||
			!SourceAnimatorText(bytes, key.Interpolation, false) ||
			!SourceAnimatorAdd(bytes, key.SourceKeyId.capacity()) ||
			!SourceAnimatorAdd(bytes, RetainedPayloadBytes(key.Data)))
			return false;
		if (key.Ease && (!SourceAnimatorText(bytes, key.Ease->InType, false) ||
						 !SourceAnimatorText(bytes, key.Ease->OutType, false)))
			return false;
		if (key.SourceDriver) {
			if (const auto *curve = std::get_if<KeyframeCurveDriver>(&*key.SourceDriver)) {
				if (curve->Data.Anchors.capacity() > UINT64_MAX / sizeof(std::array<double, 6>) ||
					!SourceAnimatorAdd(bytes, curve->Data.Anchors.capacity() * sizeof(std::array<double, 6>)))
					return false;
			} else if (const auto *audio = std::get_if<KeyframeAudioDriver>(&*key.SourceDriver)) {
				if (!SourceAnimatorText(bytes, audio->SourceId, false) ||
					!SourceAnimatorText(bytes, audio->Metric, false))
					return false;
			}
		}
		return true;
	}

	inline bool SourceAnimatorEnum(GroupSubtypeAnimator value) {
		return value == GroupSubtypeAnimator::Static || value == GroupSubtypeAnimator::Animated;
	}
	inline bool SourceAnimatorEnum(GroupAxisStorage value) {
		return static_cast<uint8_t>(value) <= static_cast<uint8_t>(GroupAxisStorage::Shared);
	}
	inline bool SourceAnimatorEnum(ValueType value) {
		return static_cast<uint8_t>(value) <= static_cast<uint8_t>(ValueType::Font);
	}

	// Counts retained capacities or the exact slots and owned bytes needed by a clone.
	inline std::optional<uint64_t> SourceAnimatorStateBytes(const SourceAnimatorState &state, bool clone) {
		constexpr size_t maximumBindings = Limits::MaximumNodes * Limits::MaximumArrayElements;
		if (state.Bindings.size() > maximumBindings || state.Detached.size() > Limits::MaximumArrayElements ||
			state.DetachedValues.size() > Limits::MaximumArrayElements)
			return std::nullopt;
		uint64_t bytes = sizeof(SourceAnimatorState);
		const auto slots = [&](size_t count, size_t capacity, size_t maximum, size_t itemBytes) {
			const size_t selected = clone ? count : capacity;
			return count <= maximum && selected <= maximum && selected <= UINT64_MAX / itemBytes &&
				   SourceAnimatorAdd(bytes, selected * itemBytes);
		};
		const auto text = [&](const std::string &value) { return SourceAnimatorText(bytes, value, clone); };
		if (!slots(
				state.Bindings.size(), state.Bindings.capacity(), maximumBindings, sizeof(GroupSubtypeBinding)
			) ||
			!slots(
				state.Detached.size(),
				state.Detached.capacity(),
				Limits::MaximumArrayElements,
				sizeof(DetachedSourceAnimator)
			) ||
			!slots(
				state.DetachedValues.size(),
				state.DetachedValues.capacity(),
				Limits::MaximumArrayElements,
				sizeof(GroupSubtypeOverlay)
			))
			return std::nullopt;

		for (const auto &binding : state.Bindings) {
			if (binding.NodeId.empty() || binding.OwnerId.empty() || binding.Port.empty() ||
				!SourceAnimatorEnum(binding.Getter) || !SourceAnimatorEnum(binding.Writer) ||
				!SourceAnimatorEnum(binding.Axes.Storage) || !SourceAnimatorEnum(binding.Axes.Writer) ||
				!text(binding.NodeId) || !text(binding.OwnerId) || !text(binding.Port) ||
				!text(binding.AnimatorPort) || !text(binding.Axes.OwnerId) || !text(binding.Axes.Port) ||
				!text(binding.Axes.InstanceBase))
				return std::nullopt;
		}
		for (const auto &detached : state.Detached) {
			if (detached.Id.empty() || detached.OwnerId.empty() || detached.OriginalPort.empty() ||
				!SourceAnimatorEnum(detached.Writer) || !SourceAnimatorEnum(detached.Type) ||
				!text(detached.Id) || !text(detached.OwnerId) || !text(detached.OriginalPort))
				return std::nullopt;
			if (detached.Track) {
				if (!text(detached.Track->NodeId) || !text(detached.Track->Port) ||
					!text(detached.Track->End) ||
					(detached.Track->QuaternionMode && *detached.Track->QuaternionMode != 0 &&
					 *detached.Track->QuaternionMode != 1))
					return std::nullopt;
			}
		}

		size_t aggregateKeys = 0;
		size_t aggregateKeySlots = 0;
		for (const auto &overlay : state.DetachedValues) {
			if (overlay.NodeId.empty() || overlay.Port.empty() || !text(overlay.NodeId) ||
				!text(overlay.Port) || (overlay.Fixed && !overlay.Keys.empty()))
				return std::nullopt;
			if (overlay.Fixed) {
				if (clone) {
					const auto fixed = ValueClonePayloadBytes(*overlay.Fixed);
					if (!fixed || !SourceAnimatorAdd(bytes, *fixed - sizeof(Value))) return std::nullopt;
				} else if (!ValidRuntimeValue(*overlay.Fixed) ||
						   !SourceAnimatorAdd(bytes, RetainedPayloadBytes(*overlay.Fixed))) {
					return std::nullopt;
				}
			}
			const size_t keySlots = clone ? overlay.Keys.size() : overlay.Keys.capacity();
			if (overlay.Keys.size() > Limits::MaximumKeyframes || keySlots > Limits::MaximumKeyframes ||
				keySlots > UINT64_MAX / sizeof(Keyframe) ||
				aggregateKeySlots > Limits::MaximumKeyframes - keySlots ||
				!SourceAnimatorAdd(bytes, keySlots * sizeof(Keyframe)))
				return std::nullopt;
			aggregateKeySlots += keySlots;
			for (const auto &key : overlay.Keys)
				if (key.NodeId != overlay.NodeId || key.Port != overlay.Port ||
					!SourceAnimatorKey(bytes, key, clone, aggregateKeys))
					return std::nullopt;
			if (overlay.SeparatedVec2) {
				if (overlay.SeparatedVec2->Port != overlay.Port) return std::nullopt;
				for (const auto &axis : overlay.SeparatedVec2->Axes) {
					const size_t axisSlots = clone ? axis.Keys.size() : axis.Keys.capacity();
					if (axis.Keys.size() > Limits::MaximumKeyframes || axisSlots > Limits::MaximumKeyframes ||
						aggregateKeys > Limits::MaximumKeyframes - axis.Keys.size() ||
						aggregateKeySlots > Limits::MaximumKeyframes - axisSlots)
						return std::nullopt;
					for (const auto &key : axis.Keys)
						if (key.NodeId != overlay.NodeId || key.Port != overlay.Port) return std::nullopt;
					aggregateKeys += axis.Keys.size();
					aggregateKeySlots += axisSlots;
				}
				const auto separated = SeparatedAnimatorBytes(*overlay.SeparatedVec2, !clone);
				if (!separated || !SourceAnimatorAdd(bytes, *separated)) return std::nullopt;
			}
		}
		return bytes;
	}

	Status ValidateSourceAnimatorState(const Document &document, Diagnostic &diagnostic);
}
