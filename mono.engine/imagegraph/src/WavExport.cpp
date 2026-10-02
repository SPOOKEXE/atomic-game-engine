#include "EvaluationBudget.hpp"
#include "SourceChoice.hpp"

#include <engine/imagegraph/WavExport.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace engine::imagegraph {
	namespace {
		double Quantize(double value, double low, double high) {
			value = std::clamp(value, low, high);
			const double base = std::floor(value), fraction = value - base;
			return base + (fraction > .5 || (fraction == .5 && std::fmod(base, 2.) != 0.) ? 1. : 0.);
		}
		Status Fail(Diagnostic &error, Status status, std::string_view port, const char *message) {
			error = {status, {}, std::string(port), message};
			return status;
		}
	}

	static Status EncodeWavExportImpl(
		const ArrayValue &channels,
		const WavExportSettings &settings,
		uint64_t maximumBytes,
		std::vector<std::byte> &bytes,
		Diagnostic &diagnostic
	) {
		diagnostic = {};
		if ((channels.ElementType != ValueType::Scalar && channels.ElementType != ValueType::Integer) ||
			!channels.Elements.empty() || !channels.Items.empty() || channels.Nested.empty())
			return Fail(
				diagnostic,
				Status::InvalidValue,
				"audio_data",
				"WAV export requires nested numeric channel rows"
			);
		if (channels.Nested.size() > Limits::MaximumAudioChannels)
			return Fail(
				diagnostic, Status::LimitExceeded, "audio_data", "WAV export exceeds the channel limit"
			);
		if (settings.Format != WavExportFormat::Unsigned8 && settings.Format != WavExportFormat::Signed16)
			return Fail(diagnostic, Status::InvalidValue, "bit_depth", "unknown WAV PCM format");
		const uint64_t samples = channels.Nested.front().size();
		if (samples > Limits::MaximumAudioClipSamples / channels.Nested.size())
			return Fail(
				diagnostic,
				Status::LimitExceeded,
				"audio_data",
				"WAV export exceeds the aggregate sample limit"
			);
		const uint32_t width = settings.Format == WavExportFormat::Unsigned8 ? 1 : 2;
		const uint32_t alignment = uint32_t(channels.Nested.size()) * width;
		if (!settings.SampleRate || settings.SampleRate > std::numeric_limits<uint32_t>::max() / alignment)
			return Fail(
				diagnostic, Status::InvalidValue, "sample", "WAV sample rate or byte rate is unrepresentable"
			);
		const double low = width == 1 ? 0 : -32768, high = width == 1 ? 255 : 32767;
		const double range = settings.DataRange.Y - settings.DataRange.X;
		if (settings.Remap && samples &&
			(!std::isfinite(settings.DataRange.X) || !std::isfinite(settings.DataRange.Y) ||
			 !std::isfinite(range) || range == 0))
			return Fail(
				diagnostic, Status::InvalidValue, "data_range", "WAV remap requires finite distinct endpoints"
			);
		// Validate conversion before allocation, including overflow in the source remap formula.
		const auto mapped = [&](double value) {
			return settings.Remap ? (value - settings.DataRange.X) / range * (high - low) + low : value;
		};
		const auto sampleValue = [&](const ElementValue &element) -> std::optional<double> {
			if (channels.ElementType == ValueType::Scalar) {
				if (const auto *sample = std::get_if<double>(&element)) return *sample;
			} else if (const auto *sample = std::get_if<int64_t>(&element))
				return double(*sample);
			return std::nullopt;
		};
		for (const auto &channel : channels.Nested) {
			if (channel.size() != samples)
				return Fail(
					diagnostic, Status::InvalidValue, "audio_data", "WAV channels have unequal sample counts"
				);
			for (const auto &element : channel) {
				const auto sample = sampleValue(element);
				if (!sample || !std::isfinite(*sample) || !std::isfinite(mapped(*sample)))
					return Fail(
						diagnostic,
						Status::InvalidValue,
						"audio_data",
						"WAV sample conversion must remain finite"
					);
			}
		}
		const uint64_t dataBytes = samples * alignment, fileBytes = 44 + dataBytes + (dataBytes & 1);
		const uint64_t cap = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		detail::EvaluationBudget budget(cap);
		auto previous = budget.Reserve(bytes.capacity());
		if (!previous)
			return Fail(
				diagnostic, Status::LimitExceeded, "audio_data", "existing WAV bytes exceed export cap"
			);
		auto replacement = budget.Reserve(fileBytes);
		if (!replacement)
			return Fail(
				diagnostic, Status::LimitExceeded, "audio_data", "WAV replacement exceeds live export cap"
			);
		std::vector<std::byte> candidate;
		candidate.reserve(size_t(fileBytes));
		if (!replacement->Resize(candidate.capacity()))
			return Fail(
				diagnostic, Status::LimitExceeded, "audio_data", "WAV byte capacity exceeds live export cap"
			);
		const auto integer = [&](uint32_t value, unsigned count) {
			for (unsigned index = 0; index < count; ++index)
				candidate.push_back(std::byte((value >> (8 * index)) & 255));
		};
		const auto tag = [&](const char *text) {
			for (unsigned i = 0; i < 4; ++i)
				candidate.push_back(std::byte(text[i]));
		};
		tag("RIFF");
		integer(uint32_t(fileBytes - 8), 4);
		tag("WAVE");
		tag("fmt ");
		integer(16, 4);
		integer(1, 2);
		integer(uint32_t(channels.Nested.size()), 2);
		integer(settings.SampleRate, 4);
		integer(settings.SampleRate * alignment, 4);
		integer(alignment, 2);
		integer(width * 8, 2);
		tag("data");
		integer(uint32_t(dataBytes), 4);
		for (size_t frame = 0; frame < samples; ++frame)
			for (const auto &channel : channels.Nested) {
				const auto sample = int32_t(Quantize(mapped(*sampleValue(channel[frame])), low, high));
				integer(uint32_t(sample), width);
			}
		if (dataBytes & 1) integer(0, 1);
		bytes = std::move(candidate);
		return Status::Ok;
	}
	template <typename Inputs>
	static Status PrepareResolvedWavExportImpl(
		const Inputs &inputs,
		uint64_t inputBytes,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	) {
		const uint64_t cap = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const uint64_t oldBytes = output.Bytes.capacity() + output.Path.capacity() + 1;
		if (oldBytes > cap)
			return Fail(diagnostic, Status::LimitExceeded, {}, "existing WAV export exceeds cap");
		const auto find = [&](std::string_view port) -> const Value * {
			for (const auto &value : inputs)
				if (value.Port == port) return &value.Data;
			return nullptr;
		};
		const Value *pathValue = find("path"), *audioValue = find("audio_data"), *rateValue = find("sample"),
					*choiceValue = find("bit_depth"), *remapValue = find("remap_data"),
					*rangeValue = find("data_range");
		const auto *path = pathValue ? std::get_if<std::string>(pathValue) : nullptr;
		// The pinned unconnected Float control starts as one empty channel.
		detail::EvaluationBudget scratchBudget(cap);
		auto retained = scratchBudget.Reserve(oldBytes + inputBytes);
		if (!retained) return Fail(diagnostic, Status::LimitExceeded, {}, "WAV snapshot exceeds live cap");
		std::optional<detail::AllocationReservation> defaultCharge;
		ArrayValue defaultChannels;
		uint64_t defaultBytes = 0;
		if (!audioValue) {
			defaultBytes = sizeof(std::vector<ElementValue>);
			defaultCharge = scratchBudget.Reserve(defaultBytes);
			if (!defaultCharge)
				return Fail(
					diagnostic, Status::LimitExceeded, "audio_data", "default WAV channel exceeds live cap"
				);
			defaultChannels.ElementType = ValueType::Scalar;
			defaultChannels.Nested.reserve(1);
			defaultBytes = defaultChannels.Nested.capacity() * sizeof(std::vector<ElementValue>);
			if (!defaultCharge->Resize(defaultBytes))
				return Fail(
					diagnostic,
					Status::LimitExceeded,
					"audio_data",
					"default WAV channel capacity exceeds cap"
				);
			defaultChannels.Nested.emplace_back();
		}
		const auto *channels = audioValue ? std::get_if<ArrayValue>(audioValue) : &defaultChannels;
		const auto *rate = rateValue ? std::get_if<int64_t>(rateValue) : nullptr;
		const auto *remap = remapValue ? std::get_if<bool>(remapValue) : nullptr;
		const auto *range = rangeValue ? std::get_if<Vector2>(rangeValue) : nullptr;
		if (!path || path->empty() || path->find('\0') != std::string::npos ||
			path->size() > Limits::MaximumTextBytes)
			return Fail(
				diagnostic, Status::InvalidValue, "path", "WAV export path must be bounded nonempty text"
			);
		if (!channels || !rate || !remap || !range || !choiceValue)
			return Fail(
				diagnostic,
				Status::UnsupportedExecution,
				{},
				"WAV export controls require resolved scalar getters"
			);
		const auto *entry = FindCatalogueEntry("pc.wav_file_write");
		const auto *input = entry ? FindCatalogueInput(*entry, "bit_depth") : nullptr;
		const auto raw = detail::SourceChoiceNumber(*choiceValue);
		const auto choice = input && raw ? detail::NormalizeSourceChoice(*input, *raw, false) : std::nullopt;
		if (!choice || (*choice != 0 && *choice != 1))
			return Fail(
				diagnostic,
				Status::UnsupportedExecution,
				"bit_depth",
				"fractional WAV bit-depth coercion is unverified"
			);
		if (*rate <= 0 || uint64_t(*rate) > std::numeric_limits<uint32_t>::max())
			return Fail(diagnostic, Status::InvalidValue, "sample", "WAV sample rate is unrepresentable");
		const bool suffix = !path->ends_with(".wav");
		const uint64_t pathLength = path->size() + (suffix ? 4 : 0);
		const uint64_t pathBytes = std::max<uint64_t>(pathLength, std::string{}.capacity()) + 1;
		if (inputBytes > cap - oldBytes || defaultBytes > cap - oldBytes - inputBytes ||
			pathBytes > cap - oldBytes - inputBytes - defaultBytes)
			return Fail(diagnostic, Status::LimitExceeded, "path", "WAV destination exceeds live export cap");
		WavExport candidate;
		candidate.Path.reserve(size_t(pathLength));
		const uint64_t actualPathBytes = candidate.Path.capacity() + 1;
		if (actualPathBytes > cap - oldBytes - inputBytes - defaultBytes)
			return Fail(
				diagnostic, Status::LimitExceeded, "path", "WAV path capacity exceeds live export cap"
			);
		const auto status = EncodeWavExport(
			*channels,
			{uint32_t(*rate),
			 *choice == 0 ? WavExportFormat::Unsigned8 : WavExportFormat::Signed16,
			 *remap,
			 *range},
			cap - oldBytes - inputBytes - defaultBytes - actualPathBytes,
			candidate.Bytes,
			diagnostic
		);
		if (status != Status::Ok) return status;
		candidate.Path = *path;
		if (suffix) candidate.Path += ".wav";
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}

	static Status PrepareWavExportImpl(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	) {
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &value) {
			return value.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.wav_file_write")
			return Fail(diagnostic, Status::InvalidValue, {}, "WAV export requires a WAV File Out target");
		const uint64_t cap = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const uint64_t oldBytes = output.Bytes.capacity() + output.Path.capacity() + 1;
		if (oldBytes > cap)
			return Fail(diagnostic, Status::LimitExceeded, {}, "existing WAV export exceeds cap");
		EvaluationSnapshot snapshot;
		if (const auto status =
				EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic, cap - oldBytes);
			status != Status::Ok)
			return status;
		return PrepareResolvedWavExportImpl(
			snapshot.Values(), snapshot.RetainedBytes(), cap, output, diagnostic
		);
	}

	Status EncodeWavExport(
		const ArrayValue &channels,
		const WavExportSettings &settings,
		uint64_t maximumBytes,
		std::vector<std::byte> &bytes,
		Diagnostic &diagnostic
	) {
		try {
			return EncodeWavExportImpl(channels, settings, maximumBytes, bytes, diagnostic);
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation failed");
		} catch (const std::length_error &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation length exceeded");
		}
	}
	Status PrepareResolvedWavExport(
		std::span<const AuthoredValue> inputs,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	) {
		try {
			return PrepareResolvedWavExportImpl(inputs, 0, maximumBytes, output, diagnostic);
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation failed");
		} catch (const std::length_error &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation length exceeded");
		}
	}

	Status PrepareWavExport(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint64_t maximumBytes,
		WavExport &output,
		Diagnostic &diagnostic
	) {
		try {
			return PrepareWavExportImpl(document, plan, nodeId, request, maximumBytes, output, diagnostic);
		} catch (const std::bad_alloc &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation failed");
		} catch (const std::length_error &) {
			return Fail(diagnostic, Status::LimitExceeded, {}, "WAV allocation length exceeded");
		}
	}

}
