#include "Processor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t POINTS_N_WORK_LIMIT = 64000000;
		struct PointsNPortName {
			std::array<char, 32> Bytes{};
			size_t Size = 0;
			std::string_view View() const {
				return {Bytes.data(), Size};
			}
		};
		PointsNPortName PointsNPort(std::string_view prefix, uint64_t ordinal) {
			PointsNPortName p;
			std::copy(prefix.begin(), prefix.end(), p.Bytes.begin());
			const auto result =
				std::to_chars(p.Bytes.data() + prefix.size(), p.Bytes.data() + p.Bytes.size(), ordinal);
			p.Size = size_t(result.ptr - p.Bytes.data());
			return p;
		}
		template <class Variant> std::optional<double> PointsNNumber(const Variant &value) {
			if (const auto *v = std::get_if<double>(&value)) return *v;
			if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
			if (const auto *v = std::get_if<EnumValue>(&value)) return double(v->Value);
			return std::nullopt;
		}
		template <class VectorFn, class NumberFn>
		void PointsNItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		);
		template <class Variant, class VectorFn, class NumberFn>
		void PointsNVisit(const Variant &value, bool vectors, VectorFn &vector, NumberFn &number) {
			if (const auto n = PointsNNumber(value)) {
				number(*n);
			} else if (const auto *v = std::get_if<Vector2>(&value)) {
				vector(*v);
			} else if constexpr (std::is_same_v<Variant, Value>) {
				if (const auto *a = std::get_if<ArrayValue>(&value)) {
					const auto row = [&](const std::vector<ElementValue> &elements) {
						if (vectors && !elements.empty()) {
							const auto x = PointsNNumber(elements[0]);
							const auto y =
								elements.size() > 1 ? PointsNNumber(elements[1]) : std::optional<double>{0};
							if (x && y) {
								vector(Vector2{*x, *y});
								return;
							}
						}
						for (const auto &e : elements)
							PointsNVisit(e, vectors, vector, number);
					};
					row(a->Elements);
					for (const auto &elements : a->Nested)
						row(elements);
					PointsNItems(a->Items, vectors, vector, number);
				}
			}
		}
		template <class VectorFn, class NumberFn>
		void PointsNItems(
			const std::vector<SourceArrayItem> &items, bool vectors, VectorFn &vector, NumberFn &number
		) {
			if (vectors && !items.empty()) {
				const auto *x = std::get_if<ElementValue>(&items[0].Data);
				const auto *y = items.size() > 1 ? std::get_if<ElementValue>(&items[1].Data) : nullptr;
				const auto a = x ? PointsNNumber(*x) : std::nullopt;
				const auto b = y ? PointsNNumber(*y) : std::optional<double>{0};
				if (a && b) {
					vector(Vector2{*a, *b});
					return;
				}
			}
			for (const auto &item : items) {
				if (const auto *e = std::get_if<ElementValue>(&item.Data))
					PointsNVisit(*e, vectors, vector, number);
				else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
					PointsNItems(*children, vectors, vector, number);
				else if (vectors) {
					const auto &image = std::get<Image>(item.Data);
					vector(Vector2{double(image.Width), double(image.Height)});
				}
			}
		}
		const Value *PointsNOriginal(const NodeContext &c, std::string_view port) {
			for (auto input = c.ProcessorOriginalValues.rbegin(); input != c.ProcessorOriginalValues.rend();
				 ++input)
				if (input->first == port) return input->second;
			for (const auto &[id, value] : c.Values)
				if (id == port) return &value;
			return c.Find(port);
		}
		bool PointsNDimensions(NodeContext &c, uint32_t &w, uint32_t &h, Vector2 &raw) {
			if (!ResolveDimension(c, "dimension", w, h)) return false;
			raw = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension") && c.Integer("dimension_unit", 1) == 1) {
				raw.X *= c.Project.SurfaceWidth;
				raw.Y *= c.Project.SurfaceHeight;
			}
			if (!std::isfinite(raw.X) || !std::isfinite(raw.Y) || raw.X <= 0 || raw.Y <= 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"PointsN gradient source dimensions must be positive",
					"dimension"
				);
			return true;
		}
		Vector2 PointsNPosition(
			const NodeContext &c, std::string_view id, std::string_view unit, Vector2 fallback, Vector2 raw
		) {
			auto p = c.Vec2(id, fallback);
			if (!c.IsLinked(id) && c.Integer(unit, 1) == 1) {
				p.X *= raw.X;
				p.Y *= raw.Y;
			}
			return p;
		}
		Rgba PointsNColour(const NodeContext &c, std::string_view id) {
			const auto p = c.Get<Colour>(id, {255, 255, 255, 255});
			return {p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
		}
		bool PointsNBatchWork(NodeContext &c, size_t count) {
			if (c.ProcessorRow != 0) return true;
			bool project = c.Integer("dimension_unit", 1) == 1;
			auto ignore = [](Vector2) {};
			auto units = [&](double n) { project = project || n == 1; };
			if (const auto *v = PointsNOriginal(c, "dimension_unit")) PointsNVisit(*v, false, ignore, units);
			uint64_t width = 1, height = 1;
			bool valid = true;
			auto dimension = [&](Vector2 p) {
				if (!c.IsLinked("dimension") && project) {
					p.X *= c.Project.SurfaceWidth;
					p.Y *= c.Project.SurfaceHeight;
				}
				if (!std::isfinite(p.X) || !std::isfinite(p.Y) || p.X <= 0 || p.Y <= 0 ||
					p.X > Limits::MaximumDimension + .5 || p.Y > Limits::MaximumDimension + .5) {
					valid = false;
					return;
				}
				const auto side = [](double v) {
					const auto f = std::floor(v), part = v - f;
					return uint64_t(std::max(1., f + (part > .5 || (part == .5 && std::fmod(f, 2.) != 0))));
				};
				width = std::max(width, side(p.X));
				height = std::max(height, side(p.Y));
			};
			auto scalar = [&](double n) { dimension({n, n}); };
			if (const auto *v = PointsNOriginal(c, "dimension"))
				PointsNVisit(*v, true, dimension, scalar);
			else
				dimension(c.Vec2("dimension", {1, 1}));
			for (const auto &[port, frames] : c.ImageArrays)
				if (port == "dimension" && frames)
					for (const auto &im : frames->Images)
						dimension({double(im.Width), double(im.Height)});
			if (!valid || width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return c.Fail(
					Status::LimitExceeded,
					"N-point gradient batch dimensions exceed native limits",
					"dimension"
				);
			// Three point passes cover Exponential/Gaussian; include the final pixel
			// write.
			const auto work = uint64_t(3 * count + 1);
			if (width * height > POINTS_N_WORK_LIMIT / std::max(size_t{1}, c.ProcessorCount) / work)
				return c.Fail(
					Status::LimitExceeded,
					"N-point gradient exceeds entire processor batch work budget",
					"dimension"
				);
			return true;
		}
		struct PointsNControl {
			Vector2 Point{};
			Rgba Color{};
			double Influence = 6;
		};
	} // namespace
	bool SourceGradientPointsN(NodeContext &c) {
		size_t count = 0;
		for (const auto &input : c.Authored.DynamicInputs) {
			size_t group = 0;
			const auto *slot = FindDynamicTemplate(c.Entry, input.Id, group);
			if (slot && slot->SourceIndex == 0) ++count;
		}
		if (count > 64)
			return c.Fail(
				Status::UnsupportedExecution, "N-point shader has only 64 uniform point slots", "point_i"
			);
		if (!PointsNBatchWork(c, count)) return false;
		uint32_t width = 0, height = 0;
		Vector2 raw{};
		if (!PointsNDimensions(c, width, height, raw)) return false;
		const auto mode = c.Integer("blend_mode");
		if (mode < 0 || mode > 2)
			return c.Fail(Status::InvalidValue, "N-point gradient blend mode is invalid", "blend_mode");
		std::array<PointsNControl, 64> points{};
		for (size_t i = 0; i < count; ++i) {
			const auto point = PointsNPort("point_i_", i), unit = PointsNPort("point_i_unit_", i),
					   color = PointsNPort("color_i_", i), influence = PointsNPort("influence_i_", i);
			auto &p = points[i];
			p.Point = PointsNPosition(c, point.View(), unit.View(), {0, 0}, raw);
			p.Point.X /= raw.X;
			p.Point.Y /= raw.Y;
			p.Color = PointsNColour(c, color.View());
			p.Influence = c.Scalar(influence.View(), 6);
			if (!std::isfinite(p.Point.X) || !std::isfinite(p.Point.Y) || !std::isfinite(p.Influence))
				return c.Fail(Status::InvalidValue, "N-point gradient controls must be finite", point.View());
			if (mode != 0 && p.Influence == 0)
				return c.Fail(
					Status::UnsupportedExecution, "N-point shader divides by zero influence", influence.View()
				);
		}
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		auto *output = c.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const Vector2 uv{(x + .5) / width, (y + .5) / height};
				Rgba result{0, 0, 0, 1};
				std::array<double, 64> weight{};
				double denominator = 0;
				if (mode == 0 && count) {
					double maximum = 0;
					for (size_t i = 0; i < count; ++i) {
						weight[i] = std::hypot(points[i].Point.X - uv.X, points[i].Point.Y - uv.Y);
						maximum = std::max(maximum, weight[i]);
					}
					maximum *= 2;
					if (maximum == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"N-point shader has zero maximum distance",
							"point_i"
						);
					for (size_t i = 0; i < count; ++i) {
						weight[i] = std::pow((maximum - weight[i]) / maximum, points[i].Influence);
						denominator += weight[i] * weight[i];
					}
					denominator = std::sqrt(denominator);
				} else if (mode == 1) {
					for (size_t i = 0; i < count; ++i) {
						const auto distance = std::hypot(points[i].Point.X - uv.X, points[i].Point.Y - uv.Y),
								   range = points[i].Influence / raw.X;
						weight[i] = std::exp(-std::pow(distance / range, 2.));
						denominator += weight[i];
					}
				} else if (mode == 2) {
					for (size_t i = 0; i < count; ++i) {
						const auto distance = std::hypot(points[i].Point.X - uv.X, points[i].Point.Y - uv.Y),
								   range = points[i].Influence / raw.X;
						weight[i] = std::max(0., (range - distance) / range);
					}
				}
				if (count && mode != 2 && (denominator == 0 || !std::isfinite(denominator)))
					return c.Fail(
						Status::UnsupportedExecution,
						"N-point shader weight denominator is zero or nonfinite",
						"influence_i"
					);
				for (size_t i = 0; i < count; ++i) {
					const auto w = mode == 2 ? weight[i] : weight[i] / denominator;
					if (!std::isfinite(w))
						return c.Fail(
							Status::UnsupportedExecution, "N-point shader weight is nonfinite", "influence_i"
						);
					for (size_t channel = 0; channel < 4; ++channel)
						result[channel] += points[i].Color[channel] * w;
				}
				if (!WritePixel(*output, x, y, result))
					return c.Fail(
						Status::InvalidValue, "N-point gradient sample exceeds surface range", "surface_out"
					);
			}
		return c.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
