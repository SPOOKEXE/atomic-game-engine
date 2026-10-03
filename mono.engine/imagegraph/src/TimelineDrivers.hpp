#pragma once

// Scalar key drivers follow node_keyframe_driver.gml; curve drivers sample its 32-interval curveMap.

#include "AudioKeyDriver.hpp"
#include "ValuePayload.hpp"
#include "SourceGradientValue.hpp"
#include "nodes/Curve.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	inline bool ApplyLinearDriver(double value, double time, double speed, double &result) {
		const double driven = value + time * speed;
		if (!std::isfinite(value) || !std::isfinite(time) || !std::isfinite(speed) || !std::isfinite(driven))
			return false;
		result = driven;
		return true;
	}

	inline double DriverRoundHalfEven(double value) {
		const double lower = std::floor(value);
		const double fraction = value - lower;
		if (fraction < .5) return lower;
		if (fraction > .5) return lower + 1;
		return std::fmod(lower, 2) == 0 ? lower : lower + 1;
	}

	inline bool ApplySnapDriver(double value, double spacing, double &result) {
		if (!std::isfinite(value) || !std::isfinite(spacing)) return false;
		const double driven = spacing == 0 ? value : DriverRoundHalfEven(value / spacing) * spacing;
		if (!std::isfinite(driven)) return false;
		result = driven;
		return true;
	}

	inline bool DriverBounceRatio(
		double ratio,
		double ease,
		int64_t amount,
		double amplitude,
		double steepness,
		bool elastic,
		double &result
	) {
		// Source controls have no work limit; native evaluation bounds the loop before running it.
		if (amount > 1024 || !std::isfinite(ratio) || !std::isfinite(ease) || !std::isfinite(amplitude) ||
			!std::isfinite(steepness))
			return false;
		double totalAmplitude = 0, power = 1;
		for (int64_t index = 0; index < amount; index++) {
			totalAmplitude += index ? power : .5;
			power *= amplitude;
		}
		const double normalization = 1 / totalAmplitude;
		double driven = 1;
		if (ratio < .5 * normalization) {
			const double segmentRatio = ratio / (.5 * normalization);
			driven = 1 - (1 - std::pow(segmentRatio, steepness)) * (1 - ease);
		} else {
			power = 1;
			double previous = 0;
			for (int64_t index = 0; index < amount; index++) {
				const double next = previous + (index ? power : .5) * normalization;
				if (ratio >= previous && ratio < next) {
					const double segmentRatio = (ratio - previous) / (next - previous);
					const double edge = segmentRatio < .5 ? 1 - segmentRatio * 2 : (segmentRatio - .5) * 2;
					const double deviation = (1 - std::pow(edge, steepness)) * (1 - ease);
					driven = elastic && index % 2 ? 1 + deviation : 1 - deviation;
					break;
				}
				power *= amplitude;
				previous = next;
			}
		}
		if (!std::isfinite(driven)) return false;
		result = driven;
		return true;
	}

	inline bool
	SourceRawQuaternionSlerp(const Quaternion &from, const Quaternion &to, double ratio, Quaternion &result) {
		const double firstNorm =
			1 / std::sqrt(from.X * from.X + from.Y * from.Y + from.Z * from.Z + from.W * from.W);
		// The pinned source multiplies the second tuple by its norm. Keep this source-specific behavior.
		const double secondNorm = std::sqrt(to.X * to.X + to.Y * to.Y + to.Z * to.Z + to.W * to.W);
		Quaternion first{from.X * firstNorm, from.Y * firstNorm, from.Z * firstNorm, from.W * firstNorm};
		Quaternion last{to.X * secondNorm, to.Y * secondNorm, to.Z * secondNorm, to.W * secondNorm};
		double dot = first.X * last.X + first.Y * last.Y + first.Z * last.Z + first.W * last.W;
		if (dot < 0) {
			dot = -dot;
			last = {-last.X, -last.Y, -last.Z, -last.W};
		}
		Quaternion value;
		if (dot > .9995)
			value = {
				CurveLerp(first.X, last.X, ratio),
				CurveLerp(first.Y, last.Y, ratio),
				CurveLerp(first.Z, last.Z, ratio),
				CurveLerp(first.W, last.W, ratio)
			};
		else {
			const double theta0 = std::acos(dot), theta = theta0 * ratio;
			const double second = std::sin(theta) / std::sin(theta0);
			const double initial = std::cos(theta) - dot * second;
			value = {
				first.X * initial + last.X * second,
				first.Y * initial + last.Y * second,
				first.Z * initial + last.Z * second,
				first.W * initial + last.W * second
			};
		}
		if (!ValidRuntimeValue(value)) return false;
		result = value;
		return true;
	}

	inline bool SourceQuaternionFromEuler(double x, double y, double z, Quaternion &result) {
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
		const double halfX = -x / 2 * (std::numbers::pi / 180);
		const double halfY = -y / 2 * (std::numbers::pi / 180);
		const double halfZ = -z / 2 * (std::numbers::pi / 180);
		const double qX = std::cos(halfZ) * std::sin(halfX);
		const double qY = std::sin(halfZ) * std::sin(halfX);
		const double qZ = std::sin(halfZ) * std::cos(halfX);
		const double qW = std::cos(halfZ) * std::cos(halfX);
		const double sineY = std::sin(halfY), cosineY = std::cos(halfY);
		const Quaternion value{
			qX * cosineY - qZ * sineY,
			qW * sineY + qY * cosineY,
			qZ * cosineY + qX * sineY,
			qW * cosineY - qY * sineY
		};
		if (!ValidRuntimeValue(value)) return false;
		result = value;
		return true;
	}

	// Source choice keys interpolate as real numbers, even when their stored literal is an enum.
	inline bool SourceEnumNumericPayload(const Value &input, Value &output) {
		if (const auto *number = std::get_if<double>(&input))
			output = *number;
		else if (const auto *integer = std::get_if<int64_t>(&input))
			output = static_cast<double>(*integer);
		else if (const auto *choice = std::get_if<EnumValue>(&input))
			output = static_cast<double>(choice->Value);
		else if (const auto *array = std::get_if<ArrayValue>(&input)) {
			if (!array->Nested.empty() || !ValidRuntimeValue(input) ||
				(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
				return false;
			ArrayValue result{ValueType::Scalar, {}};
			result.Elements.reserve(array->Elements.size());
			for (const auto &element : array->Elements)
				result.Elements.emplace_back(
					array->ElementType == ValueType::Scalar ? std::get<double>(element)
															: static_cast<double>(std::get<int64_t>(element))
				);
			output = std::move(result);
		} else
			return false;
		return ValidRuntimeValue(output);
	}

	// The callback sees continuous interpolation and source endpoints. Integer rounding happens once,
	// after the driver, as __NodeValue_Int.valueProcess does. Area includes both stored choice fields.
	template <class ScalarDriver>
	Status ApplySourceDriverComponents(
		const Value &first,
		const Value &last,
		double baseRatio,
		ScalarDriver &&driver,
		Value &result,
		int64_t quaternionMode = -1,
		bool hasInterval = true,
		bool rawSourceQuaternion = false
	) {
		if (first.index() != last.index()) return Status::TypeMismatch;
		if (!std::isfinite(baseRatio) || !ValidRuntimeValue(first) || !ValidRuntimeValue(last))
			return Status::InvalidValue;
		auto scalar = [&](double from, double to, double &driven) {
			return driver(CurveLerp(from, to, baseRatio), from, to, driven) && std::isfinite(driven);
		};
		auto integer = [&](int64_t from, int64_t to, int64_t &driven) {
			double value;
			if (!scalar(static_cast<double>(from), static_cast<double>(to), value)) return false;
			value = DriverRoundHalfEven(value);
			if (value < -0x1p63 || value >= 0x1p63) return false;
			driven = static_cast<int64_t>(value);
			return true;
		};
		if (const auto *from = std::get_if<double>(&first)) {
			double value;
			if (!scalar(*from, std::get<double>(last), value)) return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<int64_t>(&first)) {
			int64_t value;
			if (!integer(*from, std::get<int64_t>(last), value)) return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<Colour>(&first)) {
			const auto &to = std::get<Colour>(last);
			const auto pack = [](const Colour &c) {
				return uint32_t(c.Red) | (uint32_t(c.Green) << 8) | (uint32_t(c.Blue) << 16) |
					   (uint32_t(c.Alpha) << 24);
			};
			const auto channel = [&](uint8_t a, uint8_t b) {
				return static_cast<uint8_t>(
					std::clamp(DriverRoundHalfEven(CurveLerp(a, b, baseRatio)), 0.0, 255.0)
				);
			};
			const Colour merged{
				channel(from->Red, to.Red),
				channel(from->Green, to.Green),
				channel(from->Blue, to.Blue),
				channel(from->Alpha, to.Alpha)
			};
			double driven;
			if (!driver(pack(merged), pack(*from), pack(to), driven) || !std::isfinite(driven))
				return Status::InvalidValue;
			if (driven < 0 || driven > UINT32_MAX || driven != std::floor(driven))
				return Status::UnsupportedExecution;
			const uint32_t packed = static_cast<uint32_t>(driven);
			result =
				Colour{uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)};
		} else if (const auto *from = std::get_if<Vector2>(&first)) {
			const auto &to = std::get<Vector2>(last);
			Vector2 value;
			if (!scalar(from->X, to.X, value.X) || !scalar(from->Y, to.Y, value.Y))
				return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<Vector3>(&first)) {
			const auto &to = std::get<Vector3>(last);
			Vector3 value;
			if (!scalar(from->X, to.X, value.X) || !scalar(from->Y, to.Y, value.Y) ||
				!scalar(from->Z, to.Z, value.Z))
				return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<Vector4>(&first)) {
			const auto &to = std::get<Vector4>(last);
			Vector4 value;
			if (!scalar(from->X, to.X, value.X) || !scalar(from->Y, to.Y, value.Y) ||
				!scalar(from->Z, to.Z, value.Z) || !scalar(from->W, to.W, value.W))
				return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<Quaternion>(&first)) {
			if (quaternionMode < 0) return Status::UnsupportedExecution;
			if (quaternionMode > 1) return Status::InvalidValue;
			const auto &to = std::get<Quaternion>(last);
			Quaternion base = *from, value;
			if (hasInterval) {
				if (quaternionMode == 0) {
					if (!SourceRawQuaternionSlerp(*from, to, baseRatio, base)) return Status::InvalidValue;
				} else
					base = {
						CurveLerp(from->X, to.X, baseRatio),
						CurveLerp(from->Y, to.Y, baseRatio),
						CurveLerp(from->Z, to.Z, baseRatio),
						CurveLerp(from->W, to.W, baseRatio)
					};
			}
			if (!driver(base.X, from->X, to.X, value.X) || !driver(base.Y, from->Y, to.Y, value.Y) ||
				!driver(base.Z, from->Z, to.Z, value.Z) || !driver(base.W, from->W, to.W, value.W) ||
				!ValidRuntimeValue(value))
				return Status::InvalidValue;
			if (quaternionMode == 1 && !rawSourceQuaternion &&
				!SourceQuaternionFromEuler(value.X, value.Y, value.Z, value))
				return Status::InvalidValue;
			result = value;
		} else if (const auto *from = std::get_if<Area>(&first)) {
			const auto &to = std::get<Area>(last);
			Area value;
			double shape, mode;
			if (!scalar(from->CenterX, to.CenterX, value.CenterX) ||
				!scalar(from->CenterY, to.CenterY, value.CenterY) ||
				!scalar(from->HalfWidth, to.HalfWidth, value.HalfWidth) ||
				!scalar(from->HalfHeight, to.HalfHeight, value.HalfHeight) ||
				!scalar(from->Shape, to.Shape, shape) || !scalar(from->Mode, to.Mode, mode))
				return Status::InvalidValue;
			if (shape != std::floor(shape) || mode != std::floor(mode) || shape < 0 || shape > 1 ||
				mode < 0 || mode > 2)
				return Status::UnsupportedExecution;
			value.Shape = static_cast<uint8_t>(shape);
			value.Mode = static_cast<uint8_t>(mode);
			result = value;
		} else if (const auto *from = std::get_if<ArrayValue>(&first)) {
			const auto &to = std::get<ArrayValue>(last);
			if (!from->Nested.empty() || !to.Nested.empty() || from->ElementType != to.ElementType ||
				(from->ElementType != ValueType::Scalar && from->ElementType != ValueType::Integer))
				return Status::UnsupportedExecution;
			ArrayValue value{from->ElementType, {}};
			const size_t count = std::min(from->Elements.size(), to.Elements.size());
			if (count == 0) {
				if (!hasInterval) {
					result = ArrayValue{from->ElementType, {}};
					return Status::Ok;
				}
				double empty;
				if (!driver(0, 0, 0, empty) || !std::isfinite(empty)) return Status::InvalidValue;
				result = empty;
				return Status::Ok;
			}
			value.Elements.reserve(count);
			for (size_t index = 0; index < count; index++) {
				if (from->ElementType == ValueType::Scalar) {
					double element;
					if (!scalar(
							std::get<double>(from->Elements[index]),
							std::get<double>(to.Elements[index]),
							element
						))
						return Status::InvalidValue;
					value.Elements.emplace_back(element);
				} else {
					int64_t element;
					if (!integer(
							std::get<int64_t>(from->Elements[index]),
							std::get<int64_t>(to.Elements[index]),
							element
						))
						return Status::InvalidValue;
					value.Elements.emplace_back(element);
				}
			}
			result = std::move(value);
		} else
			return Status::UnsupportedExecution;
		return Status::Ok;
	}

	using DriverCurveMap = std::array<double, 33>;
	inline bool BuildDriverCurveMap(const Curve &curve, DriverCurveMap &result) {
		if (curve.Anchors.size() > Limits::MaximumCurveAnchors ||
			(curve.Anchors.empty() && curve.Header[2] == 1))
			return false;
		for (double number : curve.Header)
			if (!std::isfinite(number)) return false;
		for (const auto &anchor : curve.Anchors)
			for (double number : anchor)
				if (!std::isfinite(number)) return false;
		DriverCurveMap map;
		for (size_t index = 0; index < map.size(); index++) {
			// With no anchors the source fallback reads its third-from-last field, the minimum header.
			map[index] = curve.Anchors.empty()
							 ? CurveLerp(
								   curve.Header[3],
								   curve.Header[3] == 0 && curve.Header[4] == 0 ? 1 : curve.Header[4],
								   curve.Header[3]
							   )
							 : EvalCurveX(curve, static_cast<double>(index) / 32, .00001);
			if (!std::isfinite(map[index])) return false;
		}
		result = map;
		return true;
	}
	inline double SampleDriverCurveMap(const DriverCurveMap &map, double ratio) {
		if (std::isnan(ratio)) return 0;
		const double position = std::clamp(ratio, 0.0, 1.0) * 32;
		const size_t lower = static_cast<size_t>(std::floor(position));
		const size_t upper = static_cast<size_t>(std::ceil(position));
		return lower == upper ? map[lower] : CurveLerp(map[lower], map[upper], position - lower);
	}
	inline bool ValidSourceDriver(const KeyframeSourceDriver &driver) {
		return std::visit(
			[](const auto &control) {
				using T = std::decay_t<decltype(control)>;
				if constexpr (std::is_same_v<T, KeyframeAudioDriver>)
					return ValidAudioKeyDriver(control);
				else if constexpr (std::is_same_v<T, KeyframeLinearDriver>)
					return std::isfinite(control.Speed);
				else if constexpr (std::is_same_v<T, KeyframeSnapDriver>)
					return std::isfinite(control.Size);
				else if constexpr (std::is_same_v<T, KeyframeSineDriver>)
					return std::isfinite(control.Frequency) && std::isfinite(control.Amplitude) &&
						   std::isfinite(control.Phase) && std::isfinite(control.Smooth);
				else if constexpr (std::is_same_v<T, KeyframeCurveDriver>) {
					DriverCurveMap map;
					return BuildDriverCurveMap(control.Data, map);
				} else
					return control.Amount <= 1024 && std::isfinite(control.Spacing) &&
						   std::isfinite(control.Curve);
			},
			driver
		);
	}

	inline Status ApplySourceDriver(
		const KeyframeSourceDriver *driver,
		const Value &from,
		const Value &to,
		double ease,
		double ratio,
		double time,
		bool interval,
		int64_t quaternionMode,
		Value &result,
		double totalFrames = 1,
		bool rawSourceQuaternion = false,
		const EvaluationRequest *request = nullptr
	) {
		if (const auto *gradient = std::get_if<Gradient>(&from)) {
			const auto *target = std::get_if<Gradient>(&to);
			if (!target) return Status::TypeMismatch;
			if (driver) {
				if (!ValidSourceDriver(*driver)) return Status::InvalidValue;
				const bool endpointRemap = std::holds_alternative<KeyframeBounceDriver>(*driver) ||
										   std::holds_alternative<KeyframeElasticDriver>(*driver) ||
										   std::holds_alternative<KeyframeCurveDriver>(*driver);
				// Numeric struct operations in active source drivers have no defined
				// gradient result.
				if (interval || !endpointRemap) return Status::UnsupportedExecution;
			}
			if (!interval) {
				result = *gradient;
				return Status::Ok;
			}
			Gradient candidate;
			const Status status = LerpSourceGradient(*gradient, *target, ease, candidate);
			if (status == Status::Ok) result = std::move(candidate);
			return status;
		}
		double audioOffset = 0;
		if (driver)
			if (const auto *audio = std::get_if<KeyframeAudioDriver>(driver)) {
				const Status status =
					request ? ResolveAudioKeyOffset(*audio, *request, audioOffset) : Status::InvalidValue;
				if (status != Status::Ok) return status;
			}
		if (driver && std::holds_alternative<KeyframeSineDriver>(*driver) &&
			(!std::isfinite(totalFrames) || totalFrames <= 0))
			return Status::InvalidValue;
		DriverCurveMap map{};
		double drivenRatio = ease;
		if (driver) {
			if (!ValidSourceDriver(*driver)) return Status::InvalidValue;
			if (!interval) {
				// Source endpoint-remapping drivers return the last raw key before evaluating a ratio.
			} else if (const auto *curve = std::get_if<KeyframeCurveDriver>(driver)) {
				if (!BuildDriverCurveMap(curve->Data, map)) return Status::InvalidValue;
				drivenRatio = SampleDriverCurveMap(map, ratio);
			} else {
				bool ok = true;
				std::visit(
					[&](const auto &control) {
						using T = std::decay_t<decltype(control)>;
						if constexpr (std::is_same_v<T, KeyframeBounceDriver> ||
									  std::is_same_v<T, KeyframeElasticDriver>)
							ok = DriverBounceRatio(
								ratio,
								ease,
								control.Amount,
								control.Spacing,
								control.Curve,
								std::is_same_v<T, KeyframeElasticDriver>,
								drivenRatio
							);
					},
					*driver
				);
				if (!ok) return Status::InvalidValue;
			}
		}
		auto scalar = [&](double value, double first, double last, double &driven) {
			if (!driver) {
				driven = value;
				return true;
			}
			return std::visit(
				[&](const auto &control) {
					using T = std::decay_t<decltype(control)>;
					if constexpr (std::is_same_v<T, KeyframeAudioDriver>) {
						driven = value + audioOffset;
						return std::isfinite(driven);
					} else if constexpr (std::is_same_v<T, KeyframeLinearDriver>)
						return ApplyLinearDriver(value, time, control.Speed, driven);
					else if constexpr (std::is_same_v<T, KeyframeSnapDriver>)
						return ApplySnapDriver(value, control.Size, driven);
					else if constexpr (std::is_same_v<T, KeyframeSineDriver>) {
						double envelope = 1;
						if (control.Smooth > 0) {
							const double edge =
								std::min({1.0, 2 * ratio / control.Smooth, 2 * (1 - ratio) / control.Smooth});
							envelope = edge * edge * (3 - 2 * edge);
						}
						driven = value + std::sin(
											 (control.Phase + time * control.Frequency / totalFrames) *
											 std::numbers::pi * 2
										 ) * control.Amplitude *
											 envelope;
						return totalFrames > 0 && std::isfinite(totalFrames) && std::isfinite(driven);
					} else {
						driven = interval ? CurveLerp(first, last, drivenRatio) : first;
						return std::isfinite(driven);
					}
				},
				*driver
			);
		};
		// Empty source arrays interpolate to scalar zero. Endpoint-remapping drivers would instead
		// attempt arithmetic on whole arrays, which has no verified numeric payload.
		if (driver && interval &&
			(std::holds_alternative<KeyframeBounceDriver>(*driver) ||
			 std::holds_alternative<KeyframeElasticDriver>(*driver) ||
			 std::holds_alternative<KeyframeCurveDriver>(*driver))) {
			if (const auto *array = std::get_if<ArrayValue>(&from);
				array && (array->Elements.empty() || std::get<ArrayValue>(to).Elements.empty()))
				return Status::UnsupportedExecution;
		}
		return ApplySourceDriverComponents(
			from, to, interval ? ease : 0, scalar, result, quaternionMode, interval, rawSourceQuaternion
		);
	}

}
