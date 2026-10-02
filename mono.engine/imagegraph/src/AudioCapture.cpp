#include "AudioPayload.hpp"

#include <engine/imagegraph/AudioCapture.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace engine::imagegraph {
	namespace {
		bool SafeSourceId(std::string_view id) {
			if (id.empty() || id.size() > 255) return false;
			for (const unsigned char character : id) {
				if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
					  (character >= '0' && character <= '9') || character == '_' || character == '-'))
					return false;
			}
			return true;
		}

		template <class T> bool ParseUnsigned(std::string_view token, T &value) {
			if (token.empty()) return false;
			const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
			return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
		}

		bool ParseSample(std::string_view token, double &value) {
			if (token.empty()) return false;
			const auto parsed =
				std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
			return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
				   std::isfinite(value);
		}

		void SetCaptureDiagnostic(
			Diagnostic &diagnostic, Status code, std::string message, std::string port = {}
		) {
			diagnostic = {code, {}, std::move(port), std::move(message)};
		}

		void RemoveCarriageReturn(std::string &line) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
		}
	}

	Status ValidateAudioCaptureFrames(std::span<const AudioCaptureFrame> frames, Diagnostic &diagnostic) {
		if (frames.size() > Limits::MaximumAudioCaptureFrames) {
			SetCaptureDiagnostic(
				diagnostic, Status::LimitExceeded, "audio capture exceeds the 4096-frame limit"
			);
			return diagnostic.Code;
		}
		std::set<std::pair<std::string, uint64_t>> keys;
		size_t totalSamples = 0;
		for (const AudioCaptureFrame &frame : frames) {
			if (!SafeSourceId(frame.SourceId)) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"audio capture source ID must use 1 to 255 ASCII letters, digits, underscores or hyphens",
					"source_id"
				);
				return diagnostic.Code;
			}
			if (frame.Tick > Limits::MaximumTick) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture tick exceeds the graph tick limit",
					"tick"
				);
				return diagnostic.Code;
			}
			if (!std::isfinite(frame.SampleRate) || frame.SampleRate < 0.0) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"audio capture sample rate must be finite and non-negative",
					"sample_rate"
				);
				return diagnostic.Code;
			}
			if (!keys.emplace(frame.SourceId, frame.Tick).second) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::DuplicateId,
					"audio capture repeats a source ID and tick pair",
					"source_id"
				);
				return diagnostic.Code;
			}
			if (frame.Channels.size() > Limits::MaximumAudioChannels) {
				SetCaptureDiagnostic(
					diagnostic, Status::LimitExceeded, "audio capture exceeds the channel limit", "channels"
				);
				return diagnostic.Code;
			}
			const size_t frameSamples = detail::AudioSampleCount(frame.Samples, frame.Channels);
			if (frameSamples > Limits::MaximumAudioSamplesPerFrame ||
				frameSamples > Limits::MaximumAudioCaptureSamples - totalSamples) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture exceeds its per-frame or aggregate sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			if (!detail::ValidAudioPlanes(frame.Samples, frame.Channels)) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"audio capture requires finite equal-length channel planes or mono samples",
					"samples"
				);
				return diagnostic.Code;
			}
			if (!frame.Channels.empty() && frame.SampleRate <= 0) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"planar audio requires a positive sample rate",
					"sample_rate"
				);
				return diagnostic.Code;
			}
			totalSamples += frameSamples;
		}
		diagnostic = {};
		return Status::Ok;
	}

	Status
	ReadAudioCapture(std::string_view text, std::vector<AudioCaptureFrame> &frames, Diagnostic &diagnostic) {
		if (text.size() > Limits::MaximumAudioCaptureDocumentBytes) {
			SetCaptureDiagnostic(
				diagnostic, Status::LimitExceeded, "audio capture exceeds the 8 MiB text limit"
			);
			return diagnostic.Code;
		}
		std::istringstream input{std::string(text)};
		input.imbue(std::locale::classic());
		std::string line;
		if (!std::getline(input, line)) {
			SetCaptureDiagnostic(diagnostic, Status::Malformed, "audio capture header is missing");
			return diagnostic.Code;
		}
		RemoveCarriageReturn(line);
		const bool versionOne = line == "audio-capture 1";
		const bool versionTwo = line == "audio-capture 2";
		const bool versionThree = line == "audio-capture 3";
		const bool hasSampleRate = versionTwo || versionThree;
		if (!versionOne && !versionTwo && !versionThree) {
			SetCaptureDiagnostic(
				diagnostic, Status::UnsupportedVersion, "expected audio-capture version 1, 2 or 3"
			);
			return diagnostic.Code;
		}

		std::vector<AudioCaptureFrame> parsedFrames;
		size_t totalSamples = 0;
		while (std::getline(input, line)) {
			RemoveCarriageReturn(line);
			if (line.empty()) continue;
			std::istringstream record(line);
			record.imbue(std::locale::classic());
			std::string tag;
			std::string sourceId;
			std::string tickToken;
			std::string sampleRateToken;
			std::string countToken;
			std::string channelsToken;
			if (!(record >> tag >> std::quoted(sourceId) >> tickToken) ||
				(hasSampleRate && !(record >> sampleRateToken)) ||
				(versionThree && !(record >> channelsToken)) || !(record >> countToken) || tag != "frame") {
				SetCaptureDiagnostic(
					diagnostic, Status::Malformed, "audio capture frame record is malformed"
				);
				return diagnostic.Code;
			}
			AudioCaptureFrame frame;
			uint64_t sampleCount = 0;
			uint64_t channelCount = 0;
			if (!ParseUnsigned(tickToken, frame.Tick) || !ParseUnsigned(countToken, sampleCount) ||
				(versionThree && !ParseUnsigned(channelsToken, channelCount)) ||
				(hasSampleRate &&
				 (!ParseSample(sampleRateToken, frame.SampleRate) || frame.SampleRate <= 0.0))) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::Malformed,
					"audio capture tick, sample rate or sample count is malformed"
				);
				return diagnostic.Code;
			}
			frame.SourceId = std::move(sourceId);
			if (channelCount > Limits::MaximumAudioChannels) {
				SetCaptureDiagnostic(
					diagnostic, Status::LimitExceeded, "audio capture exceeds the channel limit", "channels"
				);
				return diagnostic.Code;
			}
			const size_t planes = std::max(uint64_t{1}, channelCount);
			if (sampleCount > Limits::MaximumAudioSamplesPerFrame / planes) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture exceeds the per-frame sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			const size_t aggregateCount = static_cast<size_t>(sampleCount) * planes;
			if (sampleCount > Limits::MaximumAudioSamplesPerFrame ||
				sampleCount > Limits::MaximumAudioCaptureSamples) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture frame exceeds the sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			if (aggregateCount > Limits::MaximumAudioCaptureSamples - totalSamples) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture exceeds its aggregate sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			totalSamples += aggregateCount;
			if (channelCount) frame.Channels.resize(channelCount);
			std::string token;
			for (size_t plane = 0; plane < planes; plane++) {
				auto &samples = channelCount ? frame.Channels[plane] : frame.Samples;
				samples.reserve(static_cast<size_t>(sampleCount));
				for (uint64_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
					double sample = 0;
					if (!(record >> token) || !ParseSample(token, sample)) {
						SetCaptureDiagnostic(
							diagnostic,
							Status::Malformed,
							"audio capture sample is malformed or non-finite",
							"samples"
						);
						return diagnostic.Code;
					}
					samples.push_back(sample);
				}
			}
			if (record >> token) {
				SetCaptureDiagnostic(
					diagnostic, Status::Malformed, "audio capture frame contains trailing fields"
				);
				return diagnostic.Code;
			}
			parsedFrames.push_back(std::move(frame));
			if (parsedFrames.size() > Limits::MaximumAudioCaptureFrames) {
				SetCaptureDiagnostic(
					diagnostic, Status::LimitExceeded, "audio capture exceeds the 4096-frame limit"
				);
				return diagnostic.Code;
			}
		}
		if (!input.eof()) {
			SetCaptureDiagnostic(diagnostic, Status::Malformed, "audio capture could not be read completely");
			return diagnostic.Code;
		}
		const Status validation = ValidateAudioCaptureFrames(parsedFrames, diagnostic);
		if (validation != Status::Ok) return validation;
		frames = std::move(parsedFrames);
		return Status::Ok;
	}

	Status
	WriteAudioCapture(std::span<const AudioCaptureFrame> frames, std::string &text, Diagnostic &diagnostic) {
		const Status validation = ValidateAudioCaptureFrames(frames, diagnostic);
		if (validation != Status::Ok) return validation;
		std::ostringstream output;
		output.imbue(std::locale::classic());
		const bool hasChannels =
			std::any_of(frames.begin(), frames.end(), [](const AudioCaptureFrame &frame) {
				return !frame.Channels.empty();
			});
		const bool hasSampleRate =
			std::any_of(frames.begin(), frames.end(), [](const AudioCaptureFrame &frame) {
				return frame.SampleRate > 0.0;
			});
		if (hasSampleRate && std::any_of(frames.begin(), frames.end(), [](const AudioCaptureFrame &frame) {
				return frame.SampleRate <= 0.0;
			})) {
			SetCaptureDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"audio-capture v2/v3 requires a positive sample rate on every frame",
				"sample_rate"
			);
			return diagnostic.Code;
		}
		output << "audio-capture "
			   << (hasChannels	   ? 3
				   : hasSampleRate ? 2
								   : 1)
			   << '\n'
			   << std::setprecision(std::numeric_limits<double>::max_digits10);
		for (const AudioCaptureFrame &frame : frames) {
			output << "frame " << std::quoted(frame.SourceId) << ' ' << frame.Tick << ' ';
			if (hasSampleRate) output << frame.SampleRate << ' ';
			if (hasChannels) output << frame.Channels.size() << ' ';
			output << (frame.Channels.empty() ? frame.Samples.size() : frame.Channels.front().size());
			if (frame.Channels.empty()) {
				for (const double sample : frame.Samples)
					output << ' ' << sample;
			} else {
				for (const auto &channel : frame.Channels)
					for (const double sample : channel)
						output << ' ' << sample;
			}
			output << '\n';
			if (output.tellp() > static_cast<std::streamoff>(Limits::MaximumAudioCaptureDocumentBytes)) {
				SetCaptureDiagnostic(
					diagnostic, Status::LimitExceeded, "audio capture exceeds the 8 MiB text limit"
				);
				return diagnostic.Code;
			}
		}
		std::string encoded = output.str();
		if (encoded.size() > Limits::MaximumAudioCaptureDocumentBytes) {
			SetCaptureDiagnostic(
				diagnostic, Status::LimitExceeded, "audio capture exceeds the 8 MiB text limit"
			);
			return diagnostic.Code;
		}
		text = std::move(encoded);
		diagnostic = {};
		return Status::Ok;
	}
}
