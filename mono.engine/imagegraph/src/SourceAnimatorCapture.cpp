#include "EvaluationBudget.hpp"
#include "TimelineDrivers.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/SourceAnimatorCapture.hpp>

#include <array>
#include <new>
#include <optional>

namespace engine::imagegraph {
	namespace capture_detail {
		uint32_t Packed(const Colour &colour) {
			return uint32_t(colour.Red) | uint32_t(colour.Green) << 8 | uint32_t(colour.Blue) << 16 |
				   uint32_t(colour.Alpha) << 24;
		}
		template <class T> std::optional<double> Numeric(const T &value) {
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
				return double(value);
			else if constexpr (std::is_same_v<T, EnumValue>)
				return double(value.Value);
			else if constexpr (std::is_same_v<T, Colour>)
				return double(Packed(value));
			else
				return std::nullopt;
		}
		bool PointDriver(const KeyframeSourceDriver &driver) {
			return std::holds_alternative<KeyframeLinearDriver>(driver) ||
				   std::holds_alternative<KeyframeSineDriver>(driver) ||
				   std::holds_alternative<KeyframeSnapDriver>(driver);
		}
		std::optional<uint64_t> PromotionBytes(const Value &value) {
			return std::visit(
				[](const auto &item) -> std::optional<uint64_t> {
					using T = std::decay_t<decltype(item)>;
					if (Numeric(item)) return 0;
					if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
								  std::is_same_v<T, Vector4>)
						return 0;
					else if constexpr (std::is_same_v<T, Area>)
						return 6 * sizeof(ElementValue);
					else if constexpr (std::is_same_v<T, ArrayValue>) {
						if (!item.Nested.empty()) return std::nullopt;
						for (const auto &leaf : item.Elements)
							if (!std::visit(
									[](const auto &scalar) { return Numeric(scalar).has_value(); }, leaf
								))
								return std::nullopt;
						return item.Elements.size() * sizeof(ElementValue);
					} else
						return std::nullopt;
				},
				value
			);
		}
		Value Promoted(const Value &value) {
			return std::visit(
				[](const auto &item) -> Value {
					using T = std::decay_t<decltype(item)>;
					if (const auto number = Numeric(item)) return *number;
					if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
								  std::is_same_v<T, Vector4>)
						return item;
					else if constexpr (std::is_same_v<T, Area>) {
						ArrayValue result{ValueType::Scalar, {}};
						result.Elements.reserve(6);
						for (double number : std::array{
								 item.CenterX,
								 item.CenterY,
								 item.HalfWidth,
								 item.HalfHeight,
								 double(item.Shape),
								 double(item.Mode)
							 })
							result.Elements.emplace_back(number);
						return result;
					} else if constexpr (std::is_same_v<T, ArrayValue>) {
						ArrayValue result{ValueType::Scalar, {}};
						result.Elements.reserve(item.Elements.size());
						for (const auto &leaf : item.Elements)
							result.Elements.emplace_back(
								*std::visit([](const auto &scalar) { return Numeric(scalar); }, leaf)
							);
						return result;
					} else
						return 0.0;
				},
				value
			);
		}
	}
	Status CaptureSourceDisabledAnimatorValue(
		std::span<const Keyframe> keys,
		const SourceAnimatorCaptureOptions &options,
		SourceAnimatorCapture &out,
		Diagnostic &diagnostic
	) {
		const auto fail = [&](Status code, std::string reason) {
			diagnostic = {
				code,
				keys.empty() ? std::string{} : keys.front().NodeId,
				keys.empty() ? std::string{} : keys.front().Port,
				std::move(reason)
			};
			return code;
		};
		if (keys.empty() || keys.size() > Limits::MaximumKeyframes || !ValidFrameTime(options.Time) ||
			!options.TotalFrames || options.TotalFrames > Limits::MaximumTick ||
			options.AvailableOwnedBytes > Limits::MaximumEvaluationBytes ||
			(options.Kind != SourceAnimatorCaptureKind::Value &&
			 options.Kind != SourceAnimatorCaptureKind::Trigger))
			return fail(Status::InvalidValue, "source animator capture request is invalid");
		FrameTime represented;
		const double time = double(FrameTimeToReal(options.Time));
		if (!SplitFrameTime(time, represented) || represented != options.Time)
			return fail(
				Status::UnsupportedExecution, "source animator capture time has no exact source real"
			);
		for (size_t index = 0; index < keys.size(); ++index) {
			const auto &key = keys[index];
			FrameTime sourceTime;
			if (!SplitFrameTime(double(FrameTimeToReal(GetFrameTime(key))), sourceTime) ||
				sourceTime != GetFrameTime(key))
				return fail(
					Status::UnsupportedExecution, "source animator key time has no exact source real"
				);
			if (key.NodeId != keys.front().NodeId || key.Port != keys.front().Port ||
				key.Interpolation != "source" || key.SineDriver || !KeyframePayloadBytes(key) ||
				!ValidFrameTime(GetFrameTime(key)) ||
				(key.Kind != KeyframeKind::Normal && key.Kind != KeyframeKind::Adder) ||
				(index && CompareFrameTime(GetFrameTime(keys[index - 1]), GetFrameTime(key)) >= 0))
				return fail(Status::InvalidValue, "source animator capture keys are invalid");
		}
		const auto &first = keys.front();
		if (std::holds_alternative<Quaternion>(first.Data))
			return fail(
				Status::UnsupportedExecution,
				"source raw quaternion capture requires original angle representation"
			);
		const auto *driver = keys.size() == 1 && first.SourceDriver &&
									 CompareFrameTime(options.Time, GetFrameTime(first)) >= 0 &&
									 options.Kind == SourceAnimatorCaptureKind::Value &&
									 capture_detail::PointDriver(*first.SourceDriver)
								 ? &*first.SourceDriver
								 : nullptr;
		const auto promotion =
			driver ? capture_detail::PromotionBytes(first.Data) : std::optional<uint64_t>{0};
		if (!promotion)
			return fail(Status::UnsupportedExecution, "source animator raw driver value is unverified");
		const uint64_t resident = options.Kind == SourceAnimatorCaptureKind::Trigger ? 0
								  : driver											 ? *promotion
										   : detail::ValuePayloadBytes(first.Data) - sizeof(Value);
		if (*promotion > options.AvailableOwnedBytes || resident > options.AvailableOwnedBytes - *promotion)
			return fail(Status::LimitExceeded, "source animator capture exceeds caller payload headroom");
		detail::EvaluationBudget budget(options.AvailableOwnedBytes);
		auto scratchCharge = budget.Reserve(*promotion);
		auto resultCharge = budget.Reserve(resident);
		if (!scratchCharge || !resultCharge)
			return fail(Status::LimitExceeded, "source animator capture exceeds caller payload headroom");
		Value promoted;
		SourceAnimatorCapture result;
		try {
			if (options.Kind == SourceAnimatorCaptureKind::Trigger)
				result.Data = false;
			else if (driver) {
				promoted = capture_detail::Promoted(first.Data);
				const auto status = detail::ApplySourceDriver(
					driver,
					promoted,
					promoted,
					0,
					.5,
					time,
					false,
					-1,
					result.Data,
					double(options.TotalFrames)
				);
				if (status != Status::Ok) return fail(status, "source animator raw driver capture failed");
			} else
				result.Data = first.Data;
		} catch (const std::bad_alloc &) {
			return fail(Status::LimitExceeded, "source animator capture allocation failed");
		}
		if (!detail::ValidRuntimeValue(result.Data))
			return fail(Status::InvalidValue, "source animator capture produced an invalid value");
		result.ResidentOwnedBytes = resident;
		result.PeakOwnedBytes = budget.Peak();
		out = std::move(result);
		diagnostic = {};
		return Status::Ok;
	}
}
