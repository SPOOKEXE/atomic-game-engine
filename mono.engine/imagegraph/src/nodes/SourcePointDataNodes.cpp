#include "ArraySource.hpp"
#include "Processor.hpp"

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FrameTime.hpp>

#include <cmath>
#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t POINT_WORK_LIMIT = 1u << 24;
		double PointRound(double x) {
			const double low = std::floor(x), f = x - low;
			return f < .5 ? low : f > .5 ? low + 1 : std::fmod(low, 2) == 0 ? low : low + 1;
		}
		bool PointReserve(NodeContext &c, ArrayValue &a, size_t n) {
			if (n > Limits::MaximumArrayElements ||
				n > POINT_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount))
				return c.Fail(Status::LimitExceeded, "Point processor batch exceeds bounded work", "points");
			if (!c.ReserveOutput(n * sizeof(ElementValue), "points")) return false;
			a.ElementType = ValueType::Vector2;
			a.Elements.reserve(n);
			return true;
		}
		bool ScatterFibonacci(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.scatter_point_fibonacci");
			const double amount = PointRound(c.Scalar("amount", 16));
			if (!std::isfinite(amount) || amount < 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Source Fibonacci negative or nonfinite array size is undefined",
					"amount"
				);
			if (amount > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Fibonacci amount exceeds bounded array", "amount");
			auto center = UnitVector(c, "center", c.Project.SurfaceWidth, c.Project.SurfaceHeight);
			auto scale = c.Vec2("scale", {1, 1});
			double direction = c.Scalar("rotation"), length = 0;
			const double turn = c.Scalar("rotation_2", (1 + std::sqrt(5.)) / 2) * 360,
						 step = c.Scalar("step", 1);
			ArrayValue out;
			if (!PointReserve(c, out, size_t(amount))) return false;
			for (size_t i = 0; i < size_t(amount); ++i) {
				const double radians = direction * std::numbers::pi / 180;
				const Vector2 p{
					center.X + length * std::cos(radians) * scale.X,
					center.Y - length * std::sin(radians) * scale.Y
				};
				if (!std::isfinite(p.X) || !std::isfinite(p.Y))
					return c.Fail(
						Status::UnsupportedExecution, "Source Fibonacci point is nonfinite", "points"
					);
				out.Elements.emplace_back(p);
				direction += turn;
				length += step;
			}
			c.SetValue("points", std::move(out));
			return c.FailureCode == Status::Ok;
		}
		bool ScatterLattice(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.scatter_point_lattice");
			const auto sub = c.Vec2("subdivision", {2, 2});
			const double nx = std::max(PointRound(sub.X) + 1, 0.), ny = std::max(PointRound(sub.Y) + 1, 0.);
			if (!std::isfinite(nx) || !std::isfinite(ny))
				return c.Fail(Status::InvalidValue, "Lattice subdivision is nonfinite", "subdivision");
			if (nx == 0 || ny == 0) {
				ArrayValue empty{ValueType::Vector2, {}};
				c.SetValue("points", std::move(empty));
				return c.FailureCode == Status::Ok;
			}
			if (nx > Limits::MaximumArrayElements || ny > Limits::MaximumArrayElements ||
				(ny && nx > Limits::MaximumArrayElements / ny))
				return c.Fail(
					Status::LimitExceeded, "Lattice subdivision exceeds bounded array", "subdivision"
				);
			Area area{};
			if (const auto *value = c.Find("point_area")) {
				const auto *a = std::get_if<Area>(value);
				if (!a) return c.Fail(Status::InvalidValue, "Lattice requires a typed area", "point_area");
				area = *a;
			}
			const bool reference = c.Integer("point_area_unit", 1) == 1;
			if (area.Mode == 1) {
				const double width = reference ? 1 : c.Project.SurfaceWidth,
							 height = reference ? 1 : c.Project.SurfaceHeight;
				const auto a = area;
				area.CenterX = (width - a.CenterX + a.HalfWidth) / 2;
				area.CenterY = (a.CenterY + height - a.HalfHeight) / 2;
				area.HalfWidth = std::abs(width - a.CenterX - a.HalfWidth) / 2;
				area.HalfHeight = std::abs(a.CenterY - height + a.HalfHeight) / 2;
			} else if (area.Mode == 2) {
				const auto a = area;
				area.CenterX = (a.CenterX + a.HalfWidth) / 2;
				area.CenterY = (a.CenterY + a.HalfHeight) / 2;
				area.HalfWidth = std::abs(a.CenterX - a.HalfWidth) / 2;
				area.HalfHeight = std::abs(a.CenterY - a.HalfHeight) / 2;
			} else if (area.Mode != 0)
				return c.Fail(
					Status::UnsupportedExecution, "Lattice area mode is not represented", "point_area"
				);
			if (reference) {
				area.CenterX *= c.Project.SurfaceWidth;
				area.HalfWidth *= c.Project.SurfaceWidth;
				area.CenterY *= c.Project.SurfaceHeight;
				area.HalfHeight *= c.Project.SurfaceHeight;
			}
			ArrayValue out;
			const size_t xcount = size_t(nx), ycount = size_t(ny);
			if (!PointReserve(c, out, xcount * ycount)) return false;
			for (size_t row = 0; row < ycount; ++row)
				for (size_t col = 0; col < xcount; ++col) {
					const double ax = xcount <= 1 ? .5 : double(col) / (xcount - 1),
								 ay = ycount <= 1 ? .5 : double(row) / (ycount - 1);
					const double x0 = area.CenterX - area.HalfWidth, x1 = area.CenterX + area.HalfWidth,
								 y0 = area.CenterY - area.HalfHeight, y1 = area.CenterY + area.HalfHeight;
					Vector2 p{x0 + (x1 - x0) * ax, y0 + (y1 - y0) * ay};
					if (!std::isfinite(p.X) || !std::isfinite(p.Y))
						return c.Fail(
							Status::InvalidValue, "Lattice area produces nonfinite points", "point_area"
						);
					out.Elements.emplace_back(p);
				}
			c.SetValue("points", std::move(out));
			return c.FailureCode == Status::Ok;
		}
		using PointItems = source_array::Items;
		const PointItems *PointChildren(const SourceArrayItem &item) {
			return std::get_if<PointItems>(&item.Data);
		}
		bool PointNumber(const SourceArrayItem &item, double &n) {
			const auto *leaf = std::get_if<ElementValue>(&item.Data);
			if (!leaf) return false;
			if (const auto *p = std::get_if<double>(leaf))
				n = *p;
			else if (const auto *p = std::get_if<int64_t>(leaf))
				n = double(*p);
			else if (const auto *p = std::get_if<bool>(leaf))
				n = *p ? 1 : 0;
			else
				return false;
			return std::isfinite(n);
		}
		bool PointRead(const SourceArrayItem &item, Vector2 &p) {
			const auto *a = PointChildren(item);
			return a && a->size() >= 2 && PointNumber((*a)[0], p.X) && PointNumber((*a)[1], p.Y);
		}
		bool SegmentFilter(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.segment_filter");
			const auto *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			const DataReplayEntry *prior = nullptr;
			if (owner) {
				Diagnostic d;
				if (ValidateDataReplay(*owner, c.ByteBudget, d) != Status::Ok)
					return c.Fail(d.Code, d.Message);
				for (const auto &entry : owner->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) prior = &entry;
			}
			const auto *value = c.Find("segment");
			const auto *input = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!input)
				return c.Fail(
					Status::InvalidValue, "Segment Filter requires an array of points or paths", "segment"
				);
			// Expanding packed vectors introduces at most five tree slots per existing leaf.
			const auto clone = ValueClonePayloadBytes(*value);
			if (!clone)
				return c.Fail(Status::LimitExceeded, "Segment input exceeds bounded payload", "segment");
			const uint64_t remaining = Limits::MaximumEvaluationBytes;
			if (*clone > remaining / 6)
				return c.Fail(
					Status::LimitExceeded, "Segment input expansion exceeds byte budget", "segment"
				);
			auto workspace = c.ReserveWorkspace(*clone * 6, "segment");
			if (!workspace) return false;
			auto tree = source_array::FromValues(*input);
			source_array::TreeCost cost;
			if (!source_array::Measure(tree, cost))
				return c.Fail(Status::LimitExceeded, "Segment input exceeds bounded tree", "segment");
			const bool empty =
				tree.empty() || (PointChildren(tree.front()) && PointChildren(tree.front())->empty());
			ArrayValue out{ValueType::Vector2, {}};
			if (empty) {
				if (prior && !prior->Values.empty()) {
					const Value &old = prior->Values.back().Data;
					const auto bytes = ValueClonePayloadBytes(old);
					if (!bytes || !c.ReserveOutput(*bytes, "segments")) return false;
					c.SetValue("segments", old);
					return c.FailureCode == Status::Ok;
				}
				if (!c.ReserveOutput(sizeof(std::vector<ElementValue>), "segments")) return false;
				out.Nested.emplace_back();
				c.SetValue("segments", std::move(out));
				return c.FailureCode == Status::Ok;
			}
			Vector2 first;
			const bool single = PointRead(tree.front(), first);
			const double angle = c.Scalar("angle"), spread = c.Scalar("spread", 15);
			const bool both = c.Boolean("both_side", true);
			if (!std::isfinite(angle) || !std::isfinite(spread))
				return c.Fail(Status::InvalidValue, "Segment angle and spread must be finite", "angle");
			const size_t slots = cost.Nodes;
			if (slots > POINT_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount))
				return c.Fail(Status::LimitExceeded, "Segment batch exceeds bounded comparisons", "segment");
			const uint64_t bytes = slots * (sizeof(ElementValue) + sizeof(std::vector<ElementValue>));
			if (!c.ReserveOutput(
					bytes * 2 + sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
						std::max(c.Authored.Id.size(), std::string{}.capacity()),
					"segments"
				))
				return false;
			const auto path = [&](const PointItems &points) -> bool {
				if (points.empty()) return true;
				Vector2 before;
				if (!PointRead(points.front(), before))
					return c.Fail(
						Status::InvalidValue, "Segment point requires two finite coordinates", "segment"
					);
				std::vector<ElementValue> run;
				for (size_t i = 1; i < points.size(); ++i) {
					Vector2 next;
					if (!PointRead(points[i], next))
						return c.Fail(
							Status::InvalidValue, "Segment point requires two finite coordinates", "segment"
						);
					const double dir =
						std::atan2(before.Y - next.Y, next.X - before.X) * 180 / std::numbers::pi;
					const auto difference = [&](double target) {
						return std::abs(std::remainder(dir - target, 360.));
					};
					double distance = difference(angle);
					if (both) distance = std::min(distance, difference(angle + 180));
					if (distance < spread) {
						if (run.empty()) run.emplace_back(before);
						run.emplace_back(next);
					} else if (!run.empty()) {
						out.Nested.push_back(std::move(run));
						run = {};
					}
					before = next;
				}
				if (!run.empty()) out.Nested.push_back(std::move(run));
				return true;
			};
			if (single) {
				if (!path(tree)) return false;
			} else
				for (const auto &item : tree) {
					const auto *points = PointChildren(item);
					if (!points)
						return c.Fail(Status::InvalidValue, "Segment path must be an array", "segment");
					if (!path(*points)) return false;
				}
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Subframe = c.Request.Subframe;
			state.NegativeFrame = c.Request.NegativeFrame;
			state.Initialized = true;
			state.PreviousFrame = double(FrameTimeToReal({state.Tick, state.Subframe, state.NegativeFrame}));
			state.Values.push_back({state.Tick, Value{out}});
			c.SetValue("segments", std::move(out));
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(state));
			return true;
		}
	}
	std::span<const ExecutorEntry> SourcePointDataExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.scatter_point_fibonacci", ScatterFibonacci},
			{"pc.scatter_point_lattice", ScatterLattice},
			{"pc.segment_filter", SegmentFilter}
		};
		return entries;
	}
}
