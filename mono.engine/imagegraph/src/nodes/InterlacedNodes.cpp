#include "Families.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		bool TimeRemap(NodeContext &context) {
			if (!context.CurrentSurfaces)
				return context.Fail(
					Status::UnsupportedExecution, "Time Remap requires an explicit surface replay owner"
				);
			if (context.Request.NegativeFrame || context.Request.Subframe != 0)
				return context.Fail(
					Status::UnsupportedExecution, "Time Remap cache requires integer nonnegative frames"
				);
			const Image *input = context.Input("surface_in"), *map = context.Input("map");
			if (!input || !map)
				return context.Fail(Status::InvalidValue, "Time Remap requires Surface In and Map");
			const int64_t life = context.Integer("max_life", 3);
			const uint64_t frames = context.Timeline ? context.Timeline->Frames : 1;
			if (life > int64_t(Limits::MaximumArrayElements))
				return context.Fail(
					Status::InvalidValue, "Time Remap life exceeds its bounded draw count", "max_life"
				);
			if (life == 0)
				return context.Fail(
					Status::UnsupportedExecution, "Zero Life requires nonfinite shader uniforms", "max_life"
				);
			if (!frames) return context.Fail(Status::InvalidValue, "Time Remap requires timeline frames");
			const bool cacheCurrent = context.Request.Tick <= frames;
			const uint64_t pixels = uint64_t(input->Width) * input->Height;
			const uint64_t draws = life < 0 ? 0 : uint64_t(life) + 1;
			uint64_t work = 64000000 / std::max(size_t{1}, context.ProcessorCount);
			const auto bounded = [&] {
				if (cacheCurrent) {
					if (pixels > work) return false;
					work -= pixels;
				}
				if (draws) {
					if (pixels > work / draws) return false;
					work -= pixels * draws;
					if (context.CurrentSurfaces->Entries.size() > work / draws) return false;
				}
				return true;
			};
			// Admit the whole row batch, cache clone, draws and worst-case history scans before allocating.
			if (!bounded())
				return context.Fail(
					Status::LimitExceeded, "Time Remap exceeds bounded batch and history work"
				);
			SurfaceFrameReplayEntry current;
			if (cacheCurrent) {
				const uint64_t cacheBytes = uint64_t(input->Width) * input->Height * 4;
				if (!context.ReserveOutput(
						sizeof(SurfaceFrameReplayEntry) +
						std::max(context.Authored.Id.size(), std::string{}.capacity()) + cacheBytes
					))
					return false;
				current.NodeId = context.Authored.Id;
				current.Frame = context.Request.Tick;
				current.ProcessorRow = context.ProcessorRow;
				current.Input = Image{input->Width, input->Height, std::vector<uint8_t>(cacheBytes)};
				// cacheCurrentFrame clones into surface_verify's default RGBA8 surface before remapping.
				for (uint32_t y = 0; y < input->Height; ++y)
					for (uint32_t x = 0; x < input->Width; ++x)
						if (!WritePixel(current.Input, x, y, ReadPixel(*input, x, y)))
							return context.Fail(Status::InvalidValue, "Time Remap cache sample is invalid");
				current.Input.Hash = SurfaceHash(current.Input);
			}
			const auto format = ResolveProcessorSurfaceFormat(context, input);
			if (!format) return false;
			Image *output = context.NewImage("surface_out", input->Width, input->Height, *format);
			if (!output) return false;
			const bool loop = context.Boolean("loop");
			const double step = 1. / life;
			for (int64_t age = 0; age <= life; ++age) {
				int64_t frame = int64_t(context.Request.Tick) - age;
				// The source performs one wrap by TOTAL_FRAMES-1, not a modulo operation.
				if (loop) {
					if (frame < 0) frame += int64_t(frames) - 1;
				} else
					frame = std::clamp(frame, int64_t{0}, int64_t(frames) - 1);
				const Image *cached =
					cacheCurrent && frame == int64_t(current.Frame) ? &current.Input : nullptr;
				if (!cached && frame >= 0)
					for (const auto &entry : context.CurrentSurfaces->Entries)
						if (entry.NodeId == context.Authored.Id &&
							entry.ProcessorRow == context.ProcessorRow && entry.Frame == uint64_t(frame)) {
							cached = &entry.Input;
							break;
						}
				if (!cached) continue;
				for (uint32_t y = 0; y < std::min(cached->Height, output->Height); ++y)
					for (uint32_t x = 0; x < std::min(cached->Width, output->Width); ++x) {
						const auto mask =
							SampleNearest(*map, (x + .5) / cached->Width, (y + .5) / cached->Height);
						const double brightness =
							(.2126 * mask[0] + .7152 * mask[1] + .0722 * mask[2]) * mask[3];
						if (brightness < age * step || brightness > age * step + step) continue;
						const auto source = ReadPixel(*cached, x, y);
						auto destination = ReadPixel(*output, x, y);
						for (size_t channel = 0; channel < 3; ++channel)
							destination[channel] = source[channel] + destination[channel] * (1 - source[3]);
						destination[3] += source[3];
						if (!WritePixel(*output, x, y, destination))
							return context.Fail(
								Status::InvalidValue, "Time Remap sample exceeds surface range"
							);
					}
			}
			if (cacheCurrent) context.SurfaceUpdates.push_back(std::move(current));
			return context.FailureCode == Status::Ok;
		}
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
		static constexpr ExecutorEntry entries[]{
			{"pc.interlaced", Interlaced, true}, {"pc.time_remap", TimeRemap, true}
		};
		return entries;
	}
}
