#pragma once

#include <engine/imagegraph/AudioCapture.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	inline bool ValidAudioKeyDriver(const KeyframeAudioDriver &driver) {
		if (driver.SourceId.empty() || driver.SourceId.size() > 255 ||
			driver.Channel >= Limits::MaximumAudioChannels || !std::isfinite(driver.Gain) ||
			!std::isfinite(driver.Bias) ||
			(driver.Metric != "rms" && driver.Metric != "peak" && driver.Metric != "mean"))
			return false;
		return std::all_of(driver.SourceId.begin(), driver.SourceId.end(), [](unsigned char c) {
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
				   c == '-';
		});
	}

	inline Status ResolveAudioKeyOffset(
		const KeyframeAudioDriver &driver, const EvaluationRequest &request, double &result
	) {
		if (!ValidAudioKeyDriver(driver)) return Status::InvalidValue;
		Diagnostic diagnostic;
		const Status valid = ValidateAudioCaptureFrames(request.AudioFrames, diagnostic);
		if (valid != Status::Ok) return valid;
		const AudioCaptureFrame *capture = nullptr;
		for (const auto &frame : request.AudioFrames)
			if (frame.SourceId == driver.SourceId && frame.Tick == request.Tick) capture = &frame;
		if (!capture) return Status::InvalidValue;
		std::span<const double> samples;
		if (capture->Channels.empty()) {
			if (driver.Channel) return Status::InvalidValue;
			samples = capture->Samples;
		} else {
			if (driver.Channel >= capture->Channels.size()) return Status::InvalidValue;
			samples = capture->Channels[driver.Channel];
		}
		// Scale energy before squaring so every finite sample has a finite RMS.
		double peak = 0;
		for (double sample : samples)
			peak = std::max(peak, std::abs(sample));
		long double sum = 0;
		if (peak != 0)
			for (double sample : samples) {
				const long double scaled = sample / peak;
				sum += driver.Metric == "rms" ? scaled * scaled : scaled;
			}
		long double metric = peak;
		if (driver.Metric != "peak")
			metric = samples.empty() ? 0
					 : driver.Metric == "rms"
						 ? static_cast<long double>(peak) * std::sqrt(sum / samples.size())
						 : static_cast<long double>(peak) * sum / samples.size();
		const long double offset = metric * driver.Gain + driver.Bias;
		const double value = static_cast<double>(offset);
		if (!std::isfinite(value)) return Status::InvalidValue;
		result = value;
		return Status::Ok;
	}
}
