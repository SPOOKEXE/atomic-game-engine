#include "ArraySource.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		using namespace source_array;
		bool CopySequenceImage(NodeContext &c, const Image &source) {
			if (!ValidSurfaceLayout(source, c.Request.MaximumImageDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(source))
				return c.Fail(Status::InvalidValue, "Sequence surface storage is invalid", "surface_in");
			auto *out = c.NewImage("surface_out", source.Width, source.Height, source.Format);
			if (!out) return false;
			std::copy(source.Pixels.begin(), source.Pixels.end(), out->Pixels.begin());
			out->Hash = SurfaceHash(*out);
			return true;
		}
		bool CopySequenceValue(NodeContext &c, const Value &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value))
				return CopySequenceImage(c, surface->Data);
			const auto bytes = ValueClonePayloadBytes(value);
			if (!bytes)
				return c.Fail(Status::LimitExceeded, "Sequence value clone exceeds bounds", "surface_out");
			if (!c.ReserveOutput(*bytes, "surface_out")) return false;
			c.SetValue("surface_out", value);
			return c.FailureCode == Status::Ok;
		}
		bool PublishSequenceItem(NodeContext &c, const SourceArrayItem &item) {
			if (const auto *image = std::get_if<Image>(&item.Data)) return CopySequenceImage(c, *image);
			if (const auto *children = std::get_if<Items>(&item.Data)) {
				TreeCost cost;
				if (!Measure(*children, cost))
					return c.Fail(
						Status::LimitExceeded, "Selected sequence row exceeds bounds", "surface_out"
					);
				auto charge = c.ReserveWorkspace(cost.Bytes, "surface_out");
				if (!charge) return false;
				return Publish(c, Items(*children), "surface_out", ValueType::Image);
			}
			const auto &leaf = std::get<ElementValue>(item.Data);
			return std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, SurfaceValue>)
						return CopySequenceImage(c, value.Data);
					else {
						if (!c.ReserveOutput(RetainedPayloadBytes(value), "surface_out")) return false;
						c.SetValue("surface_out", value);
						return c.FailureCode == Status::Ok;
					}
				},
				leaf
			);
		}
		bool SequenceAnimation(NodeContext &c) {
			const ImageArray *images = nullptr;
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "surface_in") images = array;
			const Value *value = c.Find("surface_in");
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			// Source forwards a nonarray before frame/index arithmetic.
			if (!images && !array) {
				if (const auto *image = c.Input("surface_in")) return CopySequenceImage(c, *image);
				if (value) return CopySequenceValue(c, *value);
				return c.Fail(Status::InvalidValue, "Sequence surface input is missing", "surface_in");
			}
			const auto *sequenceValue = c.Find("sequence");
			const auto *sequence = sequenceValue ? std::get_if<ArrayValue>(sequenceValue) : nullptr;
			if (!sequence || !sequence->Nested.empty() || !sequence->Items.empty())
				return c.Fail(
					Status::InvalidValue, "Sequence requires a flat numeric index array", "sequence"
				);
			if (sequence->Elements.size() > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Sequence index count exceeds bounds", "sequence");
			const size_t count = images ? images->Items.size()
										: (!array->Items.empty()	? array->Items.size()
										   : !array->Nested.empty() ? array->Nested.size()
																	: array->Elements.size());
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Sequence frame count exceeds bounds", "surface_in");
			const size_t length = sequence->Elements.empty() ? count : sequence->Elements.size();
			const double speed = c.Scalar("speed", 1), mode = c.SourceChoice("overflow");
			if (c.FailureCode != Status::Ok) return false;
			const double frame =
				double(FrameTimeToReal({c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame}));
			double index = std::floor(frame * speed);
			if (!std::isfinite(index))
				return c.Fail(Status::InvalidValue, "Sequence frame times Speed must be finite", "speed");
			if (mode == 3 && index >= double(length)) {
				c.SetValue("surface_out", int64_t{-4});
				return c.FailureCode == Status::Ok;
			}
			if (length == 0) {
				c.SetValue("surface_out", double{0});
				return c.FailureCode == Status::Ok;
			}
			if (mode == 0)
				index = std::clamp(index, 0., double(length - 1));
			else if (mode == 2) {
				const double period = double(length) * 2 - 2;
				if (period == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Single-frame source Ping Pong performs modulo zero; runtime coercion requires "
						"reference evidence",
						"overflow"
					);
				index = std::abs(std::fmod(index, period));
				if (index >= double(length)) index = period - index;
			}
			// array_safe_get(sequence,index,,loop) repairs negative indices before modulo.
			index = std::fmod(index, double(length));
			if (index < 0) index += double(length);
			double selected = index;
			if (!sequence->Elements.empty()) {
				const auto &entry = sequence->Elements[size_t(index)];
				if (const auto *integer = std::get_if<int64_t>(&entry))
					selected = double(*integer);
				else if (const auto *number = std::get_if<double>(&entry))
					selected = *number;
				else
					return c.Fail(
						Status::InvalidValue, "Sequence entries must be numeric source indices", "sequence"
					);
			}
			if (!std::isfinite(selected))
				return c.Fail(Status::InvalidValue, "Sequence index must be finite", "sequence");
			if (selected == -4) {
				c.SetValue("surface_out", int64_t{-4});
				return c.FailureCode == Status::Ok;
			}
			// array_safe_get_fast checks real bounds before the existing native array-index
			// profile truncates a nonnegative in-range index. Invalid entries return numeric zero.
			if (selected < 0 || selected >= double(count)) {
				c.SetValue("surface_out", double{0});
				return c.FailureCode == Status::Ok;
			}
			TreeCost cost;
			if (images) {
				if (!ImageCost(*images, images->Items, cost, 1))
					return c.Fail(
						Status::InvalidValue, "Sequence image array shape is invalid", "surface_in"
					);
			} else {
				cost.Bytes = RetainedPayloadBytes(*array);
				if (!array->Items.empty()) {
					TreeCost shape;
					if (!MeasureSource(array->Items, shape))
						return c.Fail(
							Status::LimitExceeded, "Sequence source tree exceeds bounds", "surface_in"
						);
					cost.Nodes = shape.Nodes;
					cost.Bytes += shape.Bytes;
				} else {
					cost.Nodes = array->Nested.size();
					const auto add = [&](const auto &row) {
						for (const auto &leaf : row)
							cost.Nodes += 1 + PackedCount(leaf);
					};
					add(array->Elements);
					for (const auto &row : array->Nested)
						add(row);
					cost.Bytes += cost.Nodes * sizeof(SourceArrayItem);
				}
			}
			if (cost.Nodes > Limits::MaximumArrayElements || cost.Bytes > Limits::MaximumArrayBytes)
				return c.Fail(Status::LimitExceeded, "Sequence source clone exceeds bounds", "surface_in");
			auto charge = c.ReserveWorkspace(cost.Bytes, "surface_in");
			if (!charge) return false;
			const auto items = images ? FromImages(*images, images->Items) : FromValues(*array);
			return PublishSequenceItem(c, items[size_t(selected)]);
		}
	}
	std::span<const ExecutorEntry> SourceSequenceAnimationExecutors() {
		static constexpr ExecutorEntry entries[] = {{"pc.sequence_anim", SequenceAnimation}};
		return entries;
	}
}
