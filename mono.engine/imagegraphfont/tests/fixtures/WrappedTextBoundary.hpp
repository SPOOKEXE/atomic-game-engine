#pragma once

#include "FontHostBoundary.hpp"

#include <bit>

namespace engine::imagegraphfont::testing {
	struct WrappedTextBoundary {
		enum class Operation { Paragraph, Spaces, Batch, TrimmedText };
		Operation Kind;
		FontHostBoundary Host{FontHostBoundary::Operation::Coverage};
		std::vector<FontMeasurement> Requests, Measured;
		uint64_t InputHash = BoundaryHash(BoundaryBitmapFont), RequestBytes = 0;
		explicit WrappedTextBoundary(Operation kind) : Kind(kind) {
			if (!Host.Run()) Host.Fail("wrapped font decode");
			(void)Host.Verify();
			if (kind == Operation::Spaces) {
				Requests = {{std::string(256, ' '), 4, -1, 0, 0}};
			} else if (kind == Operation::Batch) {
				for (size_t row = 0; row < 64; ++row)
					Requests.push_back({"A A", 12 + double(row) / 128, -1, 0, 0});
			} else {
				std::string paragraph;
				for (size_t line = 0; line < 128; ++line)
					paragraph += "A A\n";
				Requests = {{std::move(paragraph), 12, -1, 0, 0}};
			}
			for (const auto &request : Requests) {
				RequestBytes += request.Text.size();
				InputHash = (InputHash ^ BoundaryHash(request.Text)) * 1099511628211ULL;
				InputHash =
					(InputHash ^ std::bit_cast<uint64_t>(request.MaximumLineWidth)) * 1099511628211ULL;
				InputHash = (InputHash ^ std::bit_cast<uint64_t>(request.LineGap)) * 1099511628211ULL;
			}
			InputHash ^= uint64_t(kind) + Requests.size();
			if (kind != Operation::TrimmedText) return;
			Host.Graph.Nodes[0].Values = {
				{"text", Requests[0].Text},
				{"font", Host.SelectedPath.string()},
				{"size", int64_t{10}},
				{"max_line_width", int64_t{12}},
				{"interpolate", EnumValue{1}},
				{"trim", true},
				{"range", Vector2{.75, .25}},
				{"use_full_text_size", true}
			};
			Document restored;
			if (Read(Write(Host.Graph), restored, Host.DiagnosticValue) != Status::Ok ||
				restored != Host.Graph)
				Host.Fail("wrapped roundtrip");
			Host.Graph = std::move(restored);
			if (Compile(Host.Graph, Host.Compiled, Host.DiagnosticValue) != Status::Ok)
				Host.Fail("wrapped compile");
		}
		bool Run(uint64_t maximum = Limits::MaximumEvaluationBytes) {
			if (Kind == Operation::TrimmedText) {
				if (!Host.Owner.Bind(false, Host.Held, Host.Evaluation, maximum, Host.DiagnosticValue))
					return false;
				return Evaluate(
						   Host.Graph,
						   Host.Compiled,
						   "out",
						   Host.Evaluation,
						   Host.Output,
						   Host.DiagnosticValue,
						   maximum
					   ) == Status::Ok;
			}
			return MeasureNativeSourceFont(
					   *Host.Observation.Font, Requests, maximum, 16u * 1024u * 1024u, Measured, Host.Failure
				   ) == Status::Ok;
		}
		void VerifyCounters(const std::vector<core::Counter> &counters) const {
			const auto value = [&](std::string_view name) {
				for (const auto &counter : counters)
					if (counter.Name.Text() == name) return counter.Value;
				Host.Fail("missing wrapped counter");
			};
			if (value("imagegraph.font.measure_request_bytes") != RequestBytes ||
				value("imagegraph.font.measure_requests") != Requests.size() ||
				value("imagegraph.font.measure_workspace_bytes") <= 0 ||
				value("imagegraph.font.measure_candidate_bytes") <= 0 ||
				value("imagegraph.font.measure_work_units") <= 0)
				Host.Fail("wrapped byte/work counters");
			if (Kind == Operation::TrimmedText && (value("imagegraph.font.trim_input_bytes") != 512 ||
												   value("imagegraph.font.trim_output_bytes") != 256 ||
												   value("imagegraph.font.trim_tokens") != 512))
				Host.Fail("signed trim counters");
		}
		uint64_t Verify() const {
			if (Kind == Operation::TrimmedText) {
				const auto &image = Host.Output;
				if (image.Width != 8 || image.Height != 2560 || image.Pixels.size() != 8 * 2560 * 4 ||
					image.Format != SurfaceFormat::RGBA8Unorm || image.Hash != SurfaceHash(image))
					Host.Fail("full paragraph extent");
				bool ink = false;
				for (size_t y = 0; y < image.Height; ++y)
					for (size_t x = 0; x < image.Width; ++x) {
						const auto alpha = image.Pixels[(y * image.Width + x) * 4 + 3];
						ink |= alpha != 0;
						if (y >= 1920 && alpha) Host.Fail("trimmed paragraph blank tail");
					}
				if (!ink) Host.Fail("trimmed paragraph ink");
				return image.Hash;
			}
			if (Measured.size() != Requests.size()) Host.Fail("wrapped result count");
			for (size_t row = 0; row < Measured.size(); ++row) {
				const auto &actual = Measured[row], &request = Requests[row];
				// BDF A advances eight, space four. Each A A line wraps into two ten-pixel lines.
				const double width = Kind == Operation::Spaces ? 1016 : 8;
				const double height = Kind == Operation::Spaces		 ? 1270
									  : Kind == Operation::Paragraph ? 2560
																	 : 20;
				if (actual.Text != request.Text || actual.MaximumLineWidth != request.MaximumLineWidth ||
					actual.LineGap != -1 || actual.Width != width || actual.Height != height)
					Host.Fail("literal wrapped dimensions");
			}
			// The source space-trim branch swaps endpoints. 256 spaces produce 127 lines,
			// ending with the first 254 spaces, rather than normal empty whitespace lines.
			return InputHash ^ uint64_t(Measured[0].Width) ^ (uint64_t(Measured[0].Height) << 32);
		}
	};
}
