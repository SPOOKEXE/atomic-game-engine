#pragma once
#include "SourcePathShape.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <optional>
#include <vector>
namespace engine::imagegraph::detail {
	inline constexpr uint64_t MAXIMUM_SOURCE_STRAND_POINTS = 65536, MAXIMUM_SOURCE_STRAND_WORK = 16777216;
	inline bool SourceStrandFinite(std::array<double, 2> p) {
		return std::isfinite(p[0]) && std::isfinite(p[1]);
	}
	inline bool ValidSourceStrand(const SourceStrandState &state) {
		if (state.Hairs.size() > Limits::MaximumArrayElements) return false;
		uint64_t points = 0;
		for (const auto &hair : state.Hairs) {
			points += hair.Points.size();
			if (points > MAXIMUM_SOURCE_STRAND_POINTS || hair.Points.empty() || hair.SourceId < 100000 ||
				hair.SourceId > 999999 || hair.Lengths.size() != hair.Points.size() ||
				hair.RestAngles.size() != hair.Points.size())
				return false;
			for (double v :
				 {hair.Direction,
				  hair.CurlFrequency,
				  hair.CurlSize,
				  hair.Tension,
				  hair.Spring,
				  hair.AngularTension,
				  hair.RootStrength,
				  hair.RootForce,
				  hair.Restitution})
				if (!std::isfinite(v)) return false;
			for (const auto &point : hair.Points)
				if (!SourceStrandFinite(point.Position) || !SourceStrandFinite(point.Previous) ||
					!SourceStrandFinite(point.PreviousPrevious) || !SourceStrandFinite(point.Delta) ||
					!std::isfinite(point.AirResistance) || (point.IkX && !std::isfinite(*point.IkX)) ||
					(point.IkY && !std::isfinite(*point.IkY)))
					return false;
			for (double v : hair.Lengths)
				if (!std::isfinite(v)) return false;
			for (double v : hair.RestAngles)
				if (!std::isfinite(v)) return false;
		}
		return true;
	}
	inline uint64_t SourceStrandBytes(const SourceStrandState &state) {
		uint64_t bytes = sizeof(SourceStrandState) + state.Hairs.capacity() * sizeof(SourceStrandHair);
		for (const auto &hair : state.Hairs)
			bytes += hair.Points.capacity() * sizeof(SourceStrandPoint) +
					 (hair.Lengths.capacity() + hair.RestAngles.capacity()) * sizeof(double);
		return bytes;
	}
	inline std::array<double, 2> SourceStrandDirection(double distance, double angle) {
		const double radians = angle * std::numbers::pi / 180.;
		return {
			SourceShapeLengthdirComponent(distance * std::cos(radians)),
			SourceShapeLengthdirComponent(-distance * std::sin(radians))
		};
	}

	inline double SourceStrandPointDirection(std::array<double, 2> from, std::array<double, 2> to) {
		const double x = to[0] - from[0], y = to[1] - from[1];
		if (x == 0) return y > 0 ? 270. : y < 0 ? 90. : 0.;
		double angle = std::atan2(y, x) * 180. / std::numbers::pi;
		const double scaled = angle * 1000000.;
		const double magnitude = std::abs(scaled), integer = std::floor(magnitude),
					 fraction = magnitude - integer;
		double rounded = integer;
		if (fraction > .5 || (fraction == .5 && uint64_t(integer) % 2)) rounded += 1;
		angle = std::copysign(rounded, scaled) / 1000000.;
		return angle <= 0 ? -angle : 360. - angle;
	}
	inline double SourceStrandAngleDifference(double destination, double source) {
		return std::fmod(std::fmod(destination - source, 360.) + 540., 360.) - 180.;
	}
	inline double SourceStrandDistance(std::array<double, 2> a, std::array<double, 2> b) {
		const double x = b[0] - a[0], y = b[1] - a[1];
		return std::sqrt(x * x + y * y);
	}

	inline std::array<double, 2> SourceStrandAdd(std::array<double, 2> a, std::array<double, 2> b) {
		return {a[0] + b[0], a[1] + b[1]};
	}
	inline std::array<double, 2> SourceStrandSubtract(std::array<double, 2> a, std::array<double, 2> b) {
		return {a[0] - b[0], a[1] - b[1]};
	}
	inline Status SourceStrandRefusal(Diagnostic &diagnostic, Status status, const char *message) {
		diagnostic.Code = status;
		diagnostic.Message = message;
		return status;
	}
	inline Status SourceStrandGravity(
		const SourceStrandState &input,
		double gravity,
		double direction,
		uint64_t maximumBytes,
		SourceStrandState &output,
		Diagnostic &diagnostic
	) {
		if (!ValidSourceStrand(input) || !std::isfinite(gravity) || !std::isfinite(direction))
			return SourceStrandRefusal(
				diagnostic, Status::InvalidValue, "Strand gravity requires finite source state and controls"
			);
		const uint64_t retained = SourceStrandBytes(input);
		const uint64_t displaced = &input == &output ? 0 : SourceStrandBytes(output);
		if (retained > maximumBytes || displaced > maximumBytes - retained ||
			retained > maximumBytes - retained - displaced)
			return SourceStrandRefusal(
				diagnostic, Status::LimitExceeded, "Strand gravity clone overlap exceeds byte budget"
			);
		try {
			SourceStrandState candidate = input;
			const auto force = SourceStrandDirection(gravity, direction);
			for (auto &hair : candidate.Hairs)
				for (size_t i = hair.Free ? 0 : 1; i < hair.Points.size(); ++i)
					hair.Points[i].Position = SourceStrandAdd(
						SourceStrandAdd(hair.Points[i].Position, hair.Points[i].Delta), force
					);
			if (!ValidSourceStrand(candidate))
				return SourceStrandRefusal(
					diagnostic, Status::InvalidValue, "Strand gravity produced nonfinite source state"
				);
			output = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return SourceStrandRefusal(diagnostic, Status::LimitExceeded, "Strand gravity allocation failed");
		}
	}
	inline Status SourceStrandUpdate(
		const SourceStrandState &input,
		uint32_t steps,
		uint64_t maximumBytes,
		SourceStrandState &output,
		Diagnostic &diagnostic
	) {
		if (!ValidSourceStrand(input) || steps > 4096)
			return SourceStrandRefusal(
				diagnostic, Status::InvalidValue, "Strand update requires bounded source state and Step"
			);
		uint64_t work = 0;
		for (const auto &hair : input.Hairs) {
			work += hair.Points.size() * uint64_t(steps) * 4 * 3;
			if (work > MAXIMUM_SOURCE_STRAND_WORK)
				return SourceStrandRefusal(
					diagnostic, Status::LimitExceeded, "Strand update exceeds bounded constraint visits"
				);
			if (hair.CurlFrequency != 0) {
				const double gap = hair.Points.size() / hair.CurlFrequency;
				if (gap > 0 && gap < hair.Points.size() && gap != std::trunc(gap))
					return SourceStrandRefusal(
						diagnostic,
						Status::UnsupportedExecution,
						"Fractional source curl point indices "
						"require captured GameMaker coercion"
					);
			}
		}
		const uint64_t retained = SourceStrandBytes(input),
					   displaced = &input == &output ? 0 : SourceStrandBytes(output);
		if (retained > maximumBytes || displaced > maximumBytes - retained ||
			retained > maximumBytes - retained - displaced)
			return SourceStrandRefusal(
				diagnostic, Status::LimitExceeded, "Strand update clone overlap exceeds byte budget"
			);
		try {
			SourceStrandState candidate = input;
			for (auto &hair : candidate.Hairs) {
				for (uint32_t tick = 0; tick < steps; ++tick) {
					const double rest = hair.Restitution / steps;
					for (size_t i = hair.Free ? 0 : 1; i < hair.Points.size(); ++i) {
						auto &p = hair.Points[i];
						// The source skips propagation when either axis is below restitution.
						if (std::abs(p.Delta[0]) < rest || std::abs(p.Delta[1]) < rest) continue;
						p.Position[0] += p.Delta[0] / steps;
						p.Position[1] += p.Delta[1] / steps;
					}
					for (uint32_t iteration = 0; iteration < 4; ++iteration) {
						for (size_t i = 1; i < hair.Points.size(); ++i) {
							auto &p0 = hair.Points[i - 1];
							auto &p1 = hair.Points[i];
							const double direction = SourceStrandPointDirection(p0.Position, p1.Position);
							const double distance = SourceStrandDistance(p0.Position, p1.Position);
							const double length = distance + (hair.Lengths[i] - distance) * hair.Tension;
							const auto delta = SourceStrandDirection(distance - length, direction);
							if (hair.Free) {
								p0.Position = SourceStrandAdd(p0.Position, {delta[0] / 2, delta[1] / 2});
								p1.Position = SourceStrandSubtract(p1.Position, {delta[0] / 2, delta[1] / 2});
							} else {
								if (i == 1) hair.RootForce += distance - length;
								p1.Position = SourceStrandSubtract(p1.Position, delta);
							}
						}
						double previousAngle = hair.RestAngles[0];
						for (size_t i = 1; i < hair.Points.size(); ++i) {
							const auto &p0 = hair.Points[i - 1];
							auto &p1 = hair.Points[i];
							const double oldDirection = SourceStrandPointDirection(p0.Previous, p1.Previous);
							const double direction = SourceStrandPointDirection(p0.Position, p1.Position);
							const double distance = SourceStrandDistance(p0.Position, p1.Position);
							const double falloff = 1 - double(i) / hair.Points.size();
							double angle =
								direction +
								SourceStrandAngleDifference(previousAngle + hair.RestAngles[i], direction) *
									hair.AngularTension * hair.AngularTension * falloff * falloff;
							angle += SourceStrandAngleDifference(oldDirection, angle) * (1 - hair.Spring) *
									 (1 - hair.Spring);
							p1.Position =
								SourceStrandAdd(p0.Position, SourceStrandDirection(distance, angle));
							previousAngle = angle;
						}
						const double gap = hair.CurlFrequency == 0 ? std::numeric_limits<double>::infinity()
																   : hair.Points.size() / hair.CurlFrequency;
						if (gap > 0 && gap < hair.Points.size())
							for (size_t i = size_t(gap); i < hair.Points.size(); ++i) {
								const auto &p0 = hair.Points[i - size_t(gap)];
								auto &p1 = hair.Points[i];
								const double distance = SourceStrandDistance(p0.Position, p1.Position);
								if (distance < 1) continue;
								const double length =
									distance +
									(hair.Lengths[i] * hair.CurlSize * gap - distance) * hair.Spring;
								if (length <= hair.Restitution) continue;
								p1.Position = SourceStrandAdd(
									p0.Position,
									SourceStrandDirection(
										length, SourceStrandPointDirection(p0.Position, p1.Position)
									)
								);
							}
					}
				}
				// Source motionDelta clears force before checking attachment.
				hair.RootForce = 0;
				for (size_t i = hair.Free ? 0 : 1; i < hair.Points.size(); ++i) {
					auto &p = hair.Points[i];
					p.Delta = SourceStrandSubtract(p.Position, p.Previous);
					p.Previous = p.Position;
				}
				if (hair.RootStrength > -1 && hair.RootForce > hair.RootStrength) hair.Free = true;
			}
			if (!ValidSourceStrand(candidate))
				return SourceStrandRefusal(
					diagnostic, Status::InvalidValue, "Strand constraints produced nonfinite source state"
				);
			output = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return SourceStrandRefusal(diagnostic, Status::LimitExceeded, "Strand update allocation failed");
		}
	}
	struct SourceStrandCreateSettings {
		uint32_t Hairs = 8, Segments = 4;
		std::array<double, 2> Position{.5, .5}, Length{4, 4}, RootStrength{-1, -1};
		double Elasticity = .05, Spring = .8, Structure = .2, Restitution = .01;
		double CurlFrequency = 0, CurlSize = 1;
	};
	inline Status SourceStrandCreateUniformPoint(
		const SourceStrandCreateSettings &settings,
		std::span<const SourceBuiltinRandomDraw> observations,
		uint64_t maximumBytes,
		SourceStrandState &output,
		Diagnostic &diagnostic,
		const SourceStrandState *prior = nullptr
	) {
		if (settings.Hairs > Limits::MaximumArrayElements ||
			settings.Segments >= MAXIMUM_SOURCE_STRAND_POINTS || !SourceStrandFinite(settings.Position) ||
			!SourceStrandFinite(settings.Length) || !SourceStrandFinite(settings.RootStrength))
			return SourceStrandRefusal(
				diagnostic, Status::InvalidValue, "Strand create requires bounded finite controls"
			);
		for (double value :
			 {settings.Elasticity,
			  settings.Spring,
			  settings.Structure,
			  settings.Restitution,
			  settings.CurlFrequency,
			  settings.CurlSize})
			if (!std::isfinite(value))
				return SourceStrandRefusal(
					diagnostic, Status::InvalidValue, "Strand create control is nonfinite"
				);
		if (prior && !ValidSourceStrand(*prior))
			return SourceStrandRefusal(diagnostic, Status::InvalidValue, "Strand previous state is invalid");
		const uint32_t first = prior ? std::min<size_t>(settings.Hairs, prior->Hairs.size()) : 0;
		const bool randomRoot = settings.RootStrength[0] != settings.RootStrength[1];
		if (observations.size() != (settings.Hairs - first) * (randomRoot ? 3u : 2u))
			return SourceStrandRefusal(
				diagnostic,
				Status::UnsupportedExecution,
				"Strand constructor requires exact ordered builtin call observations"
			);
		size_t cursor = 0;
		for (uint32_t index = first; index < settings.Hairs; ++index) {
			const auto &length = observations[cursor++], &id = observations[cursor++];
			if (length.Operation != SourceBuiltinRandomOperation::RandomRange ||
				length.Lower != settings.Length[0] || length.Upper != settings.Length[1] ||
				!std::isfinite(length.Result) || length.Result < std::min(length.Lower, length.Upper) ||
				length.Result > std::max(length.Lower, length.Upper) ||
				id.Operation != SourceBuiltinRandomOperation::IRandomRange || id.Lower != 100000 ||
				id.Upper != 999999 || !std::isfinite(id.Result) || id.Result != std::trunc(id.Result) ||
				id.Result < 100000 || id.Result > 999999)
				return SourceStrandRefusal(
					diagnostic, Status::InvalidValue, "Strand length or source ID observation is invalid"
				);
			if (randomRoot) {
				const auto &root = observations[cursor++];
				if (root.Operation != SourceBuiltinRandomOperation::RandomRange ||
					root.Lower != settings.RootStrength[0] || root.Upper != settings.RootStrength[1] ||
					!std::isfinite(root.Result) || root.Result < std::min(root.Lower, root.Upper) ||
					root.Result > std::max(root.Lower, root.Upper))
					return SourceStrandRefusal(
						diagnostic, Status::InvalidValue, "Strand seeded root strength observation is invalid"
					);
			}
		}
		uint64_t candidateBytes =
			sizeof(SourceStrandState) +
			std::max<size_t>(settings.Hairs, prior ? prior->Hairs.size() : 0) * sizeof(SourceStrandHair) +
			uint64_t(settings.Hairs - first) * (settings.Segments + 1) *
				(sizeof(SourceStrandPoint) + 2 * sizeof(double));
		uint64_t oldPoints = 0;
		if (prior)
			for (const auto &hair : prior->Hairs)
				oldPoints += hair.Points.size();
		if (oldPoints + uint64_t(settings.Hairs - first) * (settings.Segments + 1) >
			MAXIMUM_SOURCE_STRAND_POINTS)
			return SourceStrandRefusal(
				diagnostic, Status::LimitExceeded, "Strand growth exceeds point budget"
			);
		if (prior)
			for (const auto &hair : prior->Hairs) {
				candidateBytes += hair.Points.capacity() * sizeof(SourceStrandPoint) +
								  (hair.Lengths.capacity() + hair.RestAngles.capacity()) * sizeof(double);
			}
		const uint64_t previousBytes = prior ? SourceStrandBytes(*prior) : 0;
		const uint64_t displaced = prior == &output ? 0 : SourceStrandBytes(output);
		if (displaced > maximumBytes || previousBytes > maximumBytes - displaced ||
			candidateBytes > maximumBytes - displaced - previousBytes)
			return SourceStrandRefusal(
				diagnostic, Status::LimitExceeded, "Strand constructor overlap exceeds byte budget"
			);
		try {
			SourceStrandState candidate;
			candidate.Loop = true;
			candidate.Hairs.reserve(std::max<size_t>(settings.Hairs, prior ? prior->Hairs.size() : 0));
			if (prior)
				for (const auto &hair : prior->Hairs)
					candidate.Hairs.push_back(hair);
			cursor = 0;
			for (uint32_t index = first; index < settings.Hairs; ++index) {
				const double length = observations[cursor++].Result;
				SourceStrandHair hair;
				hair.SourceId = uint32_t(observations[cursor++].Result);
				hair.RootStrength = randomRoot ? observations[cursor++].Result : settings.RootStrength[0];
				hair.Direction = 360. * index / settings.Hairs;
				hair.CurlFrequency = settings.CurlFrequency;
				hair.CurlSize = settings.CurlSize;
				hair.Tension = 1 - settings.Elasticity;
				hair.Spring = settings.Spring;
				hair.AngularTension = settings.Structure;
				hair.Restitution = settings.Restitution;
				hair.Points.reserve(settings.Segments + 1);
				hair.Lengths.assign(settings.Segments + 1, length);
				hair.RestAngles.assign(settings.Segments + 1, 0);
				hair.RestAngles[0] = hair.Direction;
				std::array<double, 2> position = settings.Position;
				const auto offset = SourceStrandDirection(length, hair.Direction);
				for (uint32_t point = 0; point <= settings.Segments; ++point) {
					hair.Points.push_back(
						{.Position = position, .Previous = position, .PreviousPrevious = position}
					);
					position = SourceStrandAdd(position, offset);
				}
				candidate.Hairs.push_back(std::move(hair));
			}
			for (uint32_t index = 0; index < settings.Hairs; ++index) {
				auto &hair = candidate.Hairs[index];
				if (!hair.Free) hair.Points[0].Position = settings.Position;
				hair.Tension = 1 - settings.Elasticity;
				hair.Spring = settings.Spring;
				hair.AngularTension = settings.Structure;
				hair.Restitution = settings.Restitution;
				hair.CurlFrequency = settings.CurlFrequency;
				hair.CurlSize = settings.CurlSize;
			}
			if (!ValidSourceStrand(candidate))
				return SourceStrandRefusal(
					diagnostic, Status::InvalidValue, "Strand constructor produced nonfinite state"
				);
			output = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return SourceStrandRefusal(
				diagnostic, Status::LimitExceeded, "Strand constructor allocation failed"
			);
		}
	}

} // namespace engine::imagegraph::detail
