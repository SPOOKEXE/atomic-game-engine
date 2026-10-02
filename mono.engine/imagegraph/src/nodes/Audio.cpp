#include <engine/imagegraph/FrameTime.hpp>
// Audio analysis follows the pinned audio node source and the default FFT extension path.

#include "../AudioPayload.hpp"
#include "AudioWindowGeometry.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {

		bool AudioLoudnessFlat(NodeContext &context) {
			const Value *input = context.Find("audio_data");
			const auto *data = input ? std::get_if<ArrayValue>(input) : nullptr;
			if (!data || data->ElementType != ValueType::Scalar)
				return context.Fail(
					Status::InvalidValue, "Audio Volume requires scalar mono samples", "audio_data"
				);
			if (data->Elements.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Audio Volume exceeds the sample budget", "audio_data"
				);
			double squaredTotal = 0;
			for (const ElementValue &element : data->Elements) {
				const auto *sample = std::get_if<double>(&element);
				if (!sample || !std::isfinite(*sample))
					return context.Fail(
						Status::InvalidValue, "Audio Volume samples must be finite scalars", "audio_data"
					);
				const double square = *sample * *sample;
				if (!std::isfinite(square) || !std::isfinite(squaredTotal + square))
					return context.Fail(
						Status::InvalidValue, "Audio Volume sample energy exceeds finite range", "audio_data"
					);
				squaredTotal += square;
			}
			if (data->Elements.empty()) {
				context.SetValue("loudness", 0.0);
				return true;
			}
			// The pinned source takes sqrt before log10. The pre-1.18 documentation omits sqrt.
			const double rms = std::sqrt(squaredTotal / data->Elements.size());
			if (rms == 0)
				return context.Fail(
					Status::InvalidValue, "nonempty silence yields non-finite loudness", "audio_data"
				);
			const double loudness = 10 * std::log10(rms);
			if (!std::isfinite(loudness))
				return context.Fail(
					Status::InvalidValue, "Audio Volume produced non-finite loudness", "audio_data"
				);
			context.SetValue("loudness", loudness);
			return true;
		}

		bool FftFlat(NodeContext &context) {
			const Value *input = context.Find("data");
			const auto *data = input ? std::get_if<ArrayValue>(input) : nullptr;
			if (!data || data->ElementType != ValueType::Scalar)
				return context.Fail(Status::InvalidValue, "FFT data must be a scalar array", "data");
			const size_t count = data->Elements.size();
			if (count > Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "FFT data exceeds the array budget", "data");
			for (const ElementValue &element : data->Elements) {
				const auto *sample = std::get_if<double>(&element);
				if (!sample || !std::isfinite(*sample))
					return context.Fail(Status::InvalidValue, "FFT data must contain finite scalars", "data");
			}
			ArrayValue output{ValueType::Scalar, {}};
			if (count <= 1) {
				context.SetValue("array", std::move(output));
				return context.FailureCode == Status::Ok;
			}
			const double choice = context.SourceChoice("preprocess_function");
			if (context.FailureCode != Status::Ok) return false;
			if (!std::isfinite(choice) || choice < 0 || choice > UINT32_MAX)
				return context.Fail(
					Status::UnsupportedExecution,
					"FFT preprocess choice is outside source uint32 range",
					"preprocess_function"
				);
			// Fractional buffer_u32 coercion is outside the pinned source; do not guess its rounding.
			if (choice != std::trunc(choice))
				return context.Fail(
					Status::UnsupportedExecution,
					"fractional FFT buffer_u32 choice coercion is unverified",
					"preprocess_function"
				);
			// The source extension reads the whole control as uint32 before its window switch.
			const uint32_t window = static_cast<uint32_t>(choice);
			const size_t paddedCount = std::bit_ceil(count);
			if (!context.ReserveOutput(
					(paddedCount / 2 + 1) * sizeof(ElementValue) +
						std::max(std::string_view("array").size(), std::string{}.capacity()),
					"data"
				))
				return false;
			auto spectrumCharge =
				context.ReserveWorkspace(paddedCount * sizeof(std::complex<double>), "data");
			if (!spectrumCharge) return false;
			std::vector<std::complex<double>> spectrum(paddedCount);
			for (size_t index = 0; index < count; index++) {
				const double phase = 2 * std::numbers::pi * static_cast<double>(index) / count;
				double gain = 1;
				if (window == 1) gain = .5 * (1 - std::cos(phase));
				if (window == 2) gain = .42 - .5 * std::cos(phase) + .08 * std::cos(2 * phase);
				spectrum[index] = std::get<double>(data->Elements[index]) * gain;
			}
			for (size_t index = 1, reversed = 0; index < paddedCount; index++) {
				size_t bit = paddedCount / 2;
				while (reversed & bit) {
					reversed ^= bit;
					bit /= 2;
				}
				reversed ^= bit;
				if (index < reversed) std::swap(spectrum[index], spectrum[reversed]);
			}
			for (size_t width = 2; width <= paddedCount; width *= 2) {
				for (size_t start = 0; start < paddedCount; start += width) {
					for (size_t index = 0; index < width / 2; index++) {
						const double phase = -2 * std::numbers::pi * static_cast<double>(index) / width;
						const auto odd = std::complex<double>(std::cos(phase), std::sin(phase)) *
										 spectrum[start + index + width / 2];
						const auto even = spectrum[start + index];
						spectrum[start + index] = even + odd;
						spectrum[start + index + width / 2] = even - odd;
					}
				}
			}
			output.Elements.reserve(paddedCount / 2 + 1);
			// The extension emits Nyquist first and rounds each magnitude through float.
			for (size_t index = paddedCount / 2 + 1; index > 0; index--) {
				const double rawMagnitude = std::abs(spectrum[index - 1]);
				if (!std::isfinite(rawMagnitude) || rawMagnitude > std::numeric_limits<float>::max())
					return context.Fail(Status::InvalidValue, "FFT magnitude is not finite", "data");
				output.Elements.emplace_back(static_cast<double>(static_cast<float>(rawMagnitude)));
			}
			context.SetValue("array", std::move(output));
			return true;
		}
		bool AudioWindow(NodeContext &context) {
			const Value *input = context.Find("audio_data");
			const auto *audio = input ? std::get_if<AudioBit>(input) : nullptr;
			AudioWindowGeometry geometry;
			if (!ReadAudioWindowGeometry(context, audio, geometry)) return false;
			const int64_t step = geometry.Step;
			const size_t channels = geometry.Channels;
			const double start = geometry.Start, end = geometry.End;
			const size_t samplesPerChannel = static_cast<size_t>(std::ceil((end - start) / step));
			const uint64_t bytes = channels * sizeof(std::vector<ElementValue>) +
								   channels * samplesPerChannel * sizeof(ElementValue);
			if (!context.ReserveOutput(
					bytes + std::max(std::string_view("bit_array").size(), std::string{}.capacity()), "width"
				))
				return false;
			ArrayValue output{ValueType::Scalar, {}};
			output.Nested.reserve(channels);
			for (size_t channel = 0; channel < channels; channel++) {
				const auto source = AudioChannel(*audio, channel);
				auto &row = output.Nested.emplace_back();
				row.reserve(samplesPerChannel);
				for (double position = start; position < end; position += step)
					row.emplace_back(source[static_cast<size_t>(position)]);
			}
			context.SetValue("bit_array", std::move(output));
			return context.FailureCode == Status::Ok;
		}

		bool AnalyzeChannels(NodeContext &context, bool fft) {
			const std::string_view port = fft ? "data" : "audio_data";
			const Value *input = context.Find(port);
			const auto *data = input ? std::get_if<ArrayValue>(input) : nullptr;
			if (!data || data->Nested.empty()) return fft ? FftFlat(context) : AudioLoudnessFlat(context);
			if (!data->Elements.empty() || data->ElementType != ValueType::Scalar ||
				data->Nested.size() > Limits::MaximumAudioChannels)
				return context.Fail(
					Status::InvalidValue,
					"audio analysis requires exclusive bounded scalar channel rows",
					port
				);
			size_t total = 0;
			for (const auto &row : data->Nested) {
				if (row.size() > Limits::MaximumArrayElements - total)
					return context.Fail(
						Status::LimitExceeded, "audio analysis exceeds the aggregate sample budget", port
					);
				total += row.size();
			}
			const std::string_view outputPort = fft ? "array" : "loudness";
			const bool single = data->Nested.size() == 1;
			const uint64_t shapeBytes =
				single
					? 0
					: data->Nested.size() * (fft ? sizeof(std::vector<ElementValue>) : sizeof(ElementValue));
			if (!context.ReserveOutput(
					shapeBytes + std::max(outputPort.size(), std::string{}.capacity()), port
				))
				return false;
			ArrayValue result{ValueType::Scalar, {}};
			if (!single) {
				if (fft)
					result.Nested.reserve(data->Nested.size());
				else
					result.Elements.reserve(data->Nested.size());
			}
			// Each copied input plane and FFT workspace share the parent's live allocation ledger.
			for (const auto &row : data->Nested) {
				uint64_t inputBytes =
					sizeof(std::pair<std::string_view, Value>) + row.size() * sizeof(ElementValue);
				for (const auto &element : row)
					inputBytes +=
						std::visit([](const auto &value) { return RetainedPayloadBytes(value); }, element);
				const Value *choice = fft ? context.Find("preprocess_function") : nullptr;
				if (choice) inputBytes += sizeof(std::pair<std::string_view, const Value *>);
				auto inputCharge = context.ReserveWorkspace(inputBytes, port);
				if (!inputCharge) return false;
				NodeContext plane(
					context.Authored, context.Entry, context.Request, context.AllocationBudget()
				);
				plane.ByteBudget = context.ByteBudget;
				plane.Values.reserve(1);
				plane.Values.emplace_back(port, ArrayValue{ValueType::Scalar, row});
				if (choice) {
					plane.ValueViews.reserve(1);
					plane.ValueViews.emplace_back("preprocess_function", choice);
				}
				if (!(fft ? FftFlat(plane) : AudioLoudnessFlat(plane)) || plane.FailureCode != Status::Ok)
					return context.Fail(plane.FailureCode, std::move(plane.FailureMessage), port);
				auto &value = plane.OutputValues.front().Data;
				// Keep the plane's port and output-container charge until their storage is destroyed.
				auto payloadCharge = plane.OutputCharge.Split(RetainedPayloadBytes(value));
				if (!payloadCharge || !context.OutputCharge.Merge(std::move(*payloadCharge)))
					return context.Fail(
						Status::InvalidOutput, "audio plane output lacks its allocation admission", port
					);
				if (single) {
					context.SetValue(outputPort, std::move(value));
					return context.FailureCode == Status::Ok;
				}
				if (fft)
					result.Nested.push_back(std::move(std::get<ArrayValue>(value).Elements));
				else
					result.Elements.emplace_back(std::get<double>(value));
			}
			context.SetValue(outputPort, std::move(result));
			return context.FailureCode == Status::Ok;
		}
		bool Fft(NodeContext &context) {
			return AnalyzeChannels(context, true);
		}
		bool AudioLoudness(NodeContext &context) {
			return AnalyzeChannels(context, false);
		}

	}

	std::span<const ExecutorEntry> AudioExecutors() {
		static constexpr std::array ENTRIES{
			ExecutorEntry{"pc.fft", Fft},
			ExecutorEntry{"pc.audio_loudness", AudioLoudness},
			ExecutorEntry{"pc.audio_window", AudioWindow}
		};
		return ENTRIES;
	}
}
