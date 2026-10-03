#include "../SourcePathShiftMemo.hpp"
#include "../SourceRandom.hpp"
#include "../ValuePayload.hpp"
#include "Curve.hpp"
#include "Families.hpp"
#include "Processor.hpp"
#include "SourcePlotRaster.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		double Frame(const NodeContext &c) {
			return double(FrameTimeToReal({c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame}));
		}
		double Frames(const NodeContext &c) {
			return c.Timeline ? double(c.Timeline->Frames) : 1.;
		}
		bool Publish(NodeContext &c, std::string_view port, double value) {
			if (!std::isfinite(value))
				return c.Fail(
					Status::UnsupportedExecution, "source animation produced a nonfinite number", port
				);
			c.SetValue(port, value);
			return c.FailureCode == Status::Ok;
		}
		double Smooth(double x, int64_t type) {
			switch (type) {
			case 0:
				return x * x * (3 - 2 * x);
			case 1:
				return x * x / (2 * x * x - 2 * x + 1);
			case 2:
				return x * x * x / (3 * x * x - 3 * x + 1);
			case 3:
				return .5 - .5 * std::cos(std::numbers::pi * x);
			default:
				return x;
			}
		}
		double Random1D(double seed) {
			const double base = std::floor(seed), fraction = seed - std::trunc(seed);
			const auto seedWord = [](double value) {
				double wrapped = std::fmod(value, 4294967296.);
				if (wrapped < 0) wrapped += 4294967296.;
				return uint32_t(wrapped);
			};
			SourceRandom first(seedWord(base));
			const double a = first.Range(0, 1);
			if (fraction == 0) return a;
			SourceRandom second(seedWord(base + 1));
			return a + (second.Range(0, 1) - a) * fraction;
		}
		bool Function(NodeContext &c) {
			const auto type = c.Authored.Type;
			const double frame = Frame(c), frames = Frames(c);
			double result = 0;
			if (type == "pc.fn_math") {
				const double a = c.Scalar("value_1"), b = c.Scalar("value_2");
				switch (int64_t(c.SourceChoice("operation", 2))) {
				case 0:
					result = a + b;
					break;
				case 1:
					result = a - b;
					break;
				case 2:
					result = a * b;
					break;
				}
			} else if (type == "pc.fn_smooth_step") {
				result = Smooth(c.Scalar("value"), int64_t(c.SourceChoice("type")));
			} else if (type == "pc.fn_ease") {
				const auto range = c.Vec2("range", {0, 1}), amount = c.Vec2("amount", {.1, .9});
				const double x = frame / frames, out = 1 - amount.Y;
				if (x >= range.X && x <= range.Y) {
					const double a = amount.X == 0 ? 1 : (x - range.X) / amount.X;
					const double b = out == 0 ? 1 : 1 - (x - (range.Y - out)) / out;
					result = Smooth(std::clamp(std::min(a, b), 0., 1.), int64_t(c.SourceChoice("smooth")));
				}
			} else if (type == "pc.fn_wave_table") {
				std::optional<AllocationReservation> waveCharge;
				const std::array<int64_t, 3> defaults{0, 1, 2};
				std::vector<int64_t> custom;
				std::span<const int64_t> waves = defaults;
				if (const auto *value = c.Find("attribute_wavetable")) {
					const auto *array = std::get_if<ArrayValue>(value);
					if (!array || array->Elements.empty() ||
						array->Elements.size() > Limits::MaximumArrayElements)
						return c.Fail(
							Status::InvalidValue,
							"wavetable must contain bounded wave indices",
							"attribute_wavetable"
						);
					waveCharge =
						c.ReserveWorkspace(array->Elements.size() * sizeof(int64_t), "attribute_wavetable");
					if (!waveCharge) return false;
					custom.reserve(array->Elements.size());
					for (const auto &item : array->Elements) {
						const auto *integer = std::get_if<int64_t>(&item);
						const auto *choice = std::get_if<EnumValue>(&item);
						const auto *scalar = std::get_if<double>(&item);
						const double index = integer  ? double(*integer)
											 : choice ? double(choice->Value)
											 : scalar ? *scalar
													  : -1.;
						if (!std::isfinite(index) || index < 0 || index > 3 || index != std::trunc(index))
							return c.Fail(
								Status::TypeMismatch,
								"wavetable entries must be integral wave indices from zero to three",
								"attribute_wavetable"
							);
						custom.push_back(int64_t(index));
					}
					waves = custom;
				}
				const double pattern = std::abs(c.Scalar("pattern")), base = std::floor(pattern),
							 blend = pattern - std::trunc(pattern);
				const double phase = c.Scalar("phase"), period = c.Scalar("period", 8);
				const double x = c.SourceChoice("speed_control") == 0
									 ? frame / frames * c.Scalar("frequency", 2) - phase
									 : (frame - phase) / period;
				if (!std::isfinite(x) || !std::isfinite(base))
					return c.Fail(Status::UnsupportedExecution, "source wavetable phase is nonfinite");
				const auto sample = [&](double index) {
					const int64_t wave = waves[size_t(std::fmod(index, double(waves.size())))];
					const double fraction = x - std::trunc(x), offset = x + .5,
								 offsetFraction = offset - std::trunc(offset);
					switch (wave) {
					case 0:
						return std::sin(x * std::numbers::pi * 2);
					case 1:
						return (1 - std::floor(fraction * 2)) * 2 - 1;
					case 2:
						return std::abs(offsetFraction - .5) * 4 - 1;
					case 3:
						return offsetFraction * 2 - 1;
					default:
						return 0.;
					}
				};
				const double raw = sample(base) * (1 - blend) + sample(base + 1) * blend;
				const auto range = c.Vec2("range", {0, 1});
				result = range.X + (range.Y - range.X) * (raw * .5 + .5);
			} else {
				const auto range = c.Vec2("range", {0, 1});
				const double frequency = c.Scalar("frequency", 4);
				const double step = frequency == 0 ? 1 : std::max(1., frames / frequency);
				const double lower = std::floor(frame / step) * step, upper = std::min(frames, lower + step);
				const auto clip = c.Integer("clip", 3);
				const double seed = double(c.Request.Seed) + c.Scalar("seed");
				if (!std::isfinite(seed))
					return c.Fail(Status::InvalidValue, "wiggler seed must be finite", "seed");
				const double a = (clip & 1) && lower <= 0 ? .5 : Random1D(seed + lower);
				const double b = (clip & 2) && upper >= frames ? .5 : Random1D(seed + upper);
				const double mix = .5 - .5 * std::cos(std::numbers::pi * (frame - lower) / (upper - lower));
				result = range.X + (range.Y - range.X) * (a + (b - a) * mix);
			}
			return Publish(c, "output", result);
		}
		const DataReplayEntry *PreviousData(NodeContext &c) {
			const auto *replay = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			if (!replay) return nullptr;
			Diagnostic diagnostic;
			if (ValidateDataReplay(*replay, c.ByteBudget, diagnostic) != Status::Ok) {
				c.Fail(diagnostic.Code, diagnostic.Message);
				return nullptr;
			}
			for (const auto &entry : replay->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) return &entry;
			return nullptr;
		}
		DataReplayEntry DataIdentity(const NodeContext &c) {
			DataReplayEntry state;
			state.NodeId = c.Authored.Id;
			state.ProcessorRow = c.ProcessorRow;
			state.Tick = c.Request.Tick;
			state.Subframe = c.Request.Subframe;
			state.NegativeFrame = c.Request.NegativeFrame;
			state.Initialized = true;
			state.PreviousFrame = Frame(c);
			return state;
		}
		bool Counter(NodeContext &c) {
			const double start = c.Scalar("start", 1), speed = c.Scalar("speed", 1);
			const int64_t mode = c.Integer("mode");
			const double delta = mode == 0 ? speed : speed / (Frames(c) - 1);
			if (!c.Boolean("async"))
				return Publish(c, "value", mode == 0 ? start + Frame(c) * speed : Frame(c) * delta);
			if (!c.CurrentData && !c.Request.DataReplay)
				return c.Fail(
					Status::UnsupportedExecution, "async counter requires an explicit data replay owner"
				);
			const auto *previous = PreviousData(c);
			if (c.FailureCode != Status::Ok) return false;
			const bool reset = Frame(c) == 0 || c.Boolean("reset");
			const bool same = previous && previous->Tick == c.Request.Tick &&
							  previous->Subframe == c.Request.Subframe &&
							  previous->NegativeFrame == c.Request.NegativeFrame;
			const double value = reset	? (mode == 0 ? start : 0)
								 : same ? previous->PreviousValue
										: (previous ? previous->PreviousValue : 0) + delta;
			if (!std::isfinite(value))
				return c.Fail(Status::UnsupportedExecution, "source counter progress is nonfinite", "value");
			if (!c.ReserveOutput(
					sizeof(DataReplayEntry) + std::max(c.Authored.Id.size(), std::string{}.capacity())
				))
				return false;
			auto state = DataIdentity(c);
			state.PreviousValue = value;
			c.DataUpdates.push_back(std::move(state));
			return Publish(c, "value", value);
		}
		bool DelayValue(NodeContext &c) {
			if (!c.CurrentData && !c.Request.DataReplay)
				return c.Fail(
					Status::UnsupportedExecution, "value delay requires an explicit data replay owner"
				);
			if (c.Request.NegativeFrame || c.Request.Subframe != 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"value delay requires integer nonnegative source cache frames"
				);
			const auto *previous = PreviousData(c);
			if (c.FailureCode != Status::Ok) return false;
			const auto *input = c.Find("value");
			if (!input) return c.Fail(Status::InvalidValue, "value delay needs a linked value", "value");
			if (!ValidValuePayload(*input, true))
				return c.Fail(Status::InvalidValue, "value delay input payload is invalid", "value");
			const auto cloned = ValueClonePayloadBytes(*input);
			if (!cloned)
				return c.Fail(Status::LimitExceeded, "value delay clone exceeds bounded payload", "value");
			const bool replacing =
				previous &&
				std::any_of(previous->Values.begin(), previous->Values.end(), [&](const auto &frame) {
					return frame.Frame == c.Request.Tick;
				});
			const uint64_t count = (previous ? previous->Values.size() : 0) + !replacing;
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "value delay history exceeds frame budget");
			uint64_t bytes = sizeof(DataReplayEntry) +
							 std::max(c.Authored.Id.size(), std::string{}.capacity()) +
							 count * sizeof(DataReplayValueFrame) + *cloned;
			if (previous) {
				for (const auto &frame : previous->Values) {
					const auto clone = ValueClonePayloadBytes(frame.Data);
					if (!clone || *clone > Limits::MaximumEvaluationBytes -
											   std::min(bytes, Limits::MaximumEvaluationBytes))
						return c.Fail(Status::LimitExceeded, "value delay history clone exceeds byte budget");
					bytes += *clone;
				}
			}
			if (!c.ReserveOutput(bytes, "value history")) return false;
			auto state = DataIdentity(c);
			state.Values.reserve(size_t(count));
			if (previous)
				for (const auto &frame : previous->Values)
					if (frame.Frame != c.Request.Tick) {
						state.Values.push_back(frame);
						// Previous-frame tags belong to a discarded evaluation namespace.
						StripSourcePathShiftIdentities(state.Values.back().Data);
					}
			state.Values.push_back({c.Request.Tick, *input});
			std::sort(state.Values.begin(), state.Values.end(), [](const auto &a, const auto &b) {
				return a.Frame < b.Frame;
			});
			double target = Frame(c) - double(c.Integer("frames", 1));
			const int64_t overflow = c.Integer("overflow");
			if (overflow == 0) target = std::clamp(target, 0., Frames(c) - 1);
			if (overflow == 1) target = std::fmod(target + Frames(c), Frames(c));
			const auto *fallback = c.Find("default");
			const Value empty = double{0}, missing = int64_t{-4};
			const Value *output = fallback ? fallback : &missing;
			if (target >= 0 && target < Frames(c)) {
				output = &empty;
				for (const auto &frame : state.Values)
					if (frame.Frame == uint64_t(target)) {
						output = &frame.Data;
						break;
					}
			}
			const auto outputBytes = ValueClonePayloadBytes(*output);
			if (!outputBytes || !c.ReserveOutput(*outputBytes, "value")) return false;
			c.SetValue("value", *output);
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(state));
			return true;
		}

		double RoundEven(double value) {
			const double lower = std::floor(value), fraction = value - lower;
			return fraction < .5			   ? lower
				   : fraction > .5			   ? lower + 1
				   : std::fmod(lower, 2.) == 0 ? lower
											   : lower + 1;
		}
		const Image *CachedSurface(const NodeContext &c, size_t row, double frame) {
			if (!std::isfinite(frame) || frame < 0 || frame > Limits::MaximumTick) return nullptr;
			const auto *state = c.CurrentSurfaces ? c.CurrentSurfaces : c.Request.SurfaceReplay;
			if (!state) return nullptr;
			for (const auto &entry : state->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == row &&
					entry.Frame == uint64_t(frame))
					return &entry.Input;
			return nullptr;
		}
		bool SurfaceCopy(
			NodeContext &c, const Image *image, std::string_view port, uint32_t width = 1, uint32_t height = 1
		) {
			if (image) {
				width = image->Width;
				height = image->Height;
			}
			auto *out = c.NewImage(port, width, height, SurfaceFormat::RGBA8Unorm);
			if (!out) return false;
			if (image)
				for (uint32_t y = 0; y < height; ++y)
					for (uint32_t x = 0; x < width; ++x) {
						auto pixel = ReadPixel(*image, x, y);
						if (DescribeSurfaceFormat(image->Format)->Channels == 1 &&
							c.Authored.Type != "pc.rate_remap" && c.Authored.Type != "pc.revert")
							pixel = {pixel[0], pixel[0], pixel[0], 1};
						if (!WritePixel(*out, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "animation surface sample exceeds range", port
							);
					}
			return true;
		}
		bool CacheSurface(NodeContext &c, const Image &input, size_t row, uint64_t frame) {
			const uint64_t bytes = sizeof(SurfaceFrameReplayEntry) +
								   std::max(c.Authored.Id.size(), std::string{}.capacity()) +
								   input.Pixels.size();
			if (!c.ReserveOutput(bytes, "surface history")) return false;
			SurfaceFrameReplayEntry entry;
			entry.NodeId = c.Authored.Id;
			entry.ProcessorRow = row;
			entry.Frame = frame;
			entry.Input.Width = input.Width;
			entry.Input.Height = input.Height;
			entry.Input.Format = input.Format;
			entry.Input.Hash = input.Hash;
			entry.Input.Pixels = input.Pixels;
			c.SurfaceUpdates.push_back(std::move(entry));
			return true;
		}
		bool TemporalSurface(NodeContext &c) {
			const auto *state = c.CurrentSurfaces ? c.CurrentSurfaces : c.Request.SurfaceReplay;
			if (!state)
				return c.Fail(
					Status::UnsupportedExecution,
					"animation surface node requires an explicit surface replay owner"
				);
			if (c.Request.NegativeFrame || c.Request.Subframe != 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"animation surface caches require integer nonnegative frames"
				);
			const auto type = c.Authored.Type;
			const auto *input = c.Input(type == "pc.revert" ? "surface_in" : "surface");
			if (!input) return c.Fail(Status::InvalidValue, "animation surface input is required", "surface");
			const double time = Frame(c), total = Frames(c);
			double target = time;
			size_t row = c.ProcessorRow;
			std::string_view output = type == "pc.revert" ? "output" : "surface";
			bool capture = true;
			uint64_t cacheFrame = c.Request.Tick;
			const Image *selected = nullptr;
			if (type == "pc.delay") {
				if (c.Boolean("use_array")) {
					const int64_t chosen = c.Integer("array_index");
					if (chosen < 0) return SurfaceCopy(c, CachedSurface(c, row, double(state->Tick)), output);
					if (uint64_t(chosen) >= Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "delay array cache index exceeds row budget", "array_index"
						);
					row = size_t(chosen);
				}
				target = time - double(c.Integer("frames", 1));
				const auto overflow = c.Integer("overflow");
				if (overflow == 0) target = std::clamp(target, 0., total - 1);
				if (overflow == 1) target = std::fmod(target + total, total);
			} else if (type == "pc.anim_loop") {
				const double start = double(c.Integer("loop_start", 1)) - 1;
				double range = double(c.Integer("loop_range", 4));
				if (range == 0) range = total - double(c.Integer("loop_start", 1));
				const double amount = c.Boolean("infinite", true) ? std::numeric_limits<double>::infinity()
																  : double(c.Integer("loop_amount", 1));
				const bool before = time<start, after = std::floor((time - start) / range)> amount;
				if (before || after) {
					const bool pass = c.Integer(before ? "pre_loop" : "post_loop", before ? 0 : 1) == 0;
					return SurfaceCopy(c, pass ? input : nullptr, output, input->Width, input->Height);
				}
				if (range <= 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"source loop range produces invalid cache indices",
						"loop_range"
					);
				target = time - start;
				if (c.Integer("loop_type") == 0)
					target = std::fmod(target, range);
				else {
					const double period = range * 2 - 2;
					if (period == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"source pingpong loop has a zero period",
							"loop_range"
						);
					target = std::fmod(target, period);
					if (target >= range) target = period - target;
				}
				capture = time < start + range;
				if (target < 0 || target > Limits::MaximumTick)
					return c.Fail(Status::InvalidValue, "source loop cache index is invalid");
				cacheFrame = uint64_t(target);
			} else if (type == "pc.rate_remap") {
				bool failed = false;
				if (CopyWhenInactive(c, failed)) return !failed;
				const double fps = c.Timeline ? c.Timeline->FramesPerSecond : 30;
				const double step = fps / c.Scalar("framerate", 10);
				target = std::floor(time / step) * step;
			} else if (type == "pc.revert") {
				target = total - time - 1;
			} else {
				const double step = c.Scalar("delay_step", 1),
							 groups = std::floor(double(c.ProcessorCount) / step);
				if (!std::isfinite(groups) || groups == 0 || step <= 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"source stagger step produces a nonfinite progress",
						"delay_step"
					);
				const auto *value = c.Find("stagger_curve");
				const auto *curve = value ? std::get_if<Curve>(value) : nullptr;
				if (!curve) return c.Fail(Status::InvalidValue, "stagger curve is required", "stagger_curve");
				target = RoundEven(
					time - EvalCurveX(*curve, std::floor(double(row) / step) / groups) *
							   c.Scalar("delay_frame", 1) * groups
				);
				const auto overflow = c.Integer("overflow");
				if (overflow == 1) target = std::clamp(target, 0., total - 1);
				if (overflow == 2) target = std::fmod(std::fmod(target, total) + total, total);
			}
			if (!std::isfinite(target))
				return c.Fail(Status::UnsupportedExecution, "source animation cache target is nonfinite");
			if (capture && !CacheSurface(c, *input, row, cacheFrame)) return false;
			if (target >= 0 && target < total) {
				selected = capture && uint64_t(target) == cacheFrame ? input : CachedSurface(c, row, target);
			}
			if (type == "pc.revert" && !selected && state->Initialized)
				selected = CachedSurface(c, row, total - double(state->Tick) - 1);
			const bool fixedSize = type == "pc.delay" || type == "pc.stagger";
			return SurfaceCopy(
				c, selected, output, fixedSize ? input->Width : 1, fixedSize ? input->Height : 1
			);
		}

	} // namespace
	std::span<const ExecutorEntry> SourceAnimationExecutors() {
		static constexpr ExecutorEntry entries[]{
			{"pc.plot_linear", SourcePlot, true},
			{"pc.anim_loop", TemporalSurface, true},
			{"pc.delay", TemporalSurface, true},
			{"pc.rate_remap", TemporalSurface, true},
			{"pc.revert", TemporalSurface, true},
			{"pc.stagger", TemporalSurface, true},
			{"pc.counter", Counter, true},
			{"pc.delay_value", DelayValue, true},
			{"pc.fn_math", Function, true},
			{"pc.fn_smooth_step", Function, true},
			{"pc.fn_ease", Function, true},
			{"pc.fn_wave_table", Function, true},
			{"pc.wiggler", Function, true}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
