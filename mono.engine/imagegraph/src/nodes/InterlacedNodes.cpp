#include "Families.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool Interlaced(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			if (!context.CurrentSurfaces)
				return context.Fail(
					Status::UnsupportedExecution, "Interlace requires an explicit surface replay owner"
				);
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
			const int64_t axis = context.Integer("axis"), delay = context.Integer("delay", 1);
			if (axis < 0 || axis > 1)
				return context.Fail(Status::InvalidValue, "Interlace axis is invalid", "axis");
			// The pinned call passes inputs[6] (Mask feather) to shader_set_f_map. That input has no
			// mapping control, so the size map is disabled even when Size itself is mapped.
			const double size =
				context.Boolean("size_mapped") ? context.Vec2("size_map_range").X : context.Scalar("size", 1);
			if (!std::isfinite(size) || size == 0)
				return context.Fail(
					Status::InvalidValue, "Interlace size must be finite and nonzero", "size"
				);
			long double frame = static_cast<long double>(context.Request.Tick) - delay;
			if (context.Boolean("loop")) {
				const uint64_t frames = context.Timeline ? context.Timeline->Frames : 1;
				if (!frames)
					return context.Fail(Status::InvalidValue, "Interlace loop needs timeline frames", "loop");
				frame = std::fmod(frame + frames, static_cast<long double>(frames));
			}
			const Image *previous = nullptr;
			if (frame >= 0 && frame <= Limits::MaximumTick)
				for (const auto &entry : context.CurrentSurfaces->Entries)
					if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == context.ProcessorRow &&
						entry.Frame == static_cast<uint64_t>(frame)) {
						previous = &entry.Input;
						break;
					}
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			const bool invert = context.Boolean("invert");
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const double coordinate = (axis == 0 ? y : x) / size;
					const double parity = coordinate - std::floor(coordinate / 2) * 2;
					const bool current = (parity < 1) != invert;
					const Rgba colour =
						current ? ReadPixel(*source, x, y)
						: previous
							? SampleNearest(*previous, (x + .5) / source->Width, (y + .5) / source->Height)
							: Rgba{};
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Interlace sample exceeds surface range", "surface_out"
						);
				}
			const uint64_t bytes = sizeof(SurfaceFrameReplayEntry) +
								   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
								   source->Pixels.capacity();
			if (!context.ReserveOutput(bytes)) return false;
			context.SurfaceUpdates.reserve(1);
			context.SurfaceUpdates.push_back(
				{context.Authored.Id, context.Request.Tick, context.ProcessorRow, *source}
			);
			FinishProcessor(context, *source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> TemporalExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.interlaced", Interlaced, true}};
		return entries;
	}
}
