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
			if (frame.Samples.size() > Limits::MaximumAudioSamplesPerFrame ||
				frame.Samples.size() > Limits::MaximumAudioCaptureSamples - totalSamples) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture exceeds its per-frame or aggregate sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			totalSamples += frame.Samples.size();
			for (const double sample : frame.Samples) {
				if (!std::isfinite(sample)) {
					SetCaptureDiagnostic(
						diagnostic, Status::InvalidValue, "audio capture samples must be finite", "samples"
					);
					return diagnostic.Code;
				}
			}
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
		if (!versionOne && !versionTwo) {
			SetCaptureDiagnostic(
				diagnostic, Status::UnsupportedVersion, "expected audio-capture version 1 or 2"
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
			if (!(record >> tag >> std::quoted(sourceId) >> tickToken) ||
				(versionTwo && !(record >> sampleRateToken)) || !(record >> countToken) || tag != "frame") {
				SetCaptureDiagnostic(
					diagnostic, Status::Malformed, "audio capture frame record is malformed"
				);
				return diagnostic.Code;
			}
			AudioCaptureFrame frame;
			uint64_t sampleCount = 0;
			if (!ParseUnsigned(tickToken, frame.Tick) || !ParseUnsigned(countToken, sampleCount) ||
				(versionTwo &&
				 (!ParseSample(sampleRateToken, frame.SampleRate) || frame.SampleRate <= 0.0))) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::Malformed,
					"audio capture tick, sample rate or sample count is malformed"
				);
				return diagnostic.Code;
			}
			frame.SourceId = std::move(sourceId);
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
			if (sampleCount > Limits::MaximumAudioCaptureSamples - totalSamples) {
				SetCaptureDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"audio capture exceeds its aggregate sample limit",
					"samples"
				);
				return diagnostic.Code;
			}
			totalSamples += static_cast<size_t>(sampleCount);
			frame.Samples.reserve(static_cast<size_t>(sampleCount));
			std::string token;
			for (uint64_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
				double sample = 0.0;
				if (!(record >> token) || !ParseSample(token, sample)) {
					SetCaptureDiagnostic(
						diagnostic,
						Status::Malformed,
						"audio capture sample is malformed or non-finite",
						"samples"
					);
					return diagnostic.Code;
				}
				frame.Samples.push_back(sample);
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
				"audio-capture v2 requires a positive sample rate on every frame",
				"sample_rate"
			);
			return diagnostic.Code;
		}
		output << "audio-capture " << (hasSampleRate ? 2 : 1) << '\n'
			   << std::setprecision(std::numeric_limits<double>::max_digits10);
		for (const AudioCaptureFrame &frame : frames) {
			output << "frame " << std::quoted(frame.SourceId) << ' ' << frame.Tick << ' ';
			if (hasSampleRate) output << frame.SampleRate << ' ';
			output << frame.Samples.size();
			for (const double sample : frame.Samples)
				output << ' ' << sample;
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
