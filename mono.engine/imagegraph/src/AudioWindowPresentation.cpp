#include "nodes/AudioWindowGeometry.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/AudioWindowPresentation.hpp>

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>
namespace engine::imagegraph {
	namespace {
		uint64_t GeometryKey(const AudioBit &audio) {
			uint64_t hash = 14695981039346656037ULL;
			const auto word = [&](uint64_t value) {
				for (size_t i = 0; i < 8; ++i) {
					hash ^= (value >> (i * 8)) & 255;
					hash *= 1099511628211ULL;
				}
			};
			word(std::bit_cast<uint64_t>(audio.SampleRate));
			word(detail::AudioChannelCount(audio));
			const auto samples = detail::AudioChannel(audio, 0);
			word(samples.size());
			for (double sample : samples)
				word(std::bit_cast<uint64_t>(sample));
			return hash;
		}
		Status ResolveImpl(
			const Document &document,
			const Plan &plan,
			std::string_view nodeId,
			const EvaluationRequest &request,
			uint64_t byteBudget,
			AudioWindowPresentation &output,
			Diagnostic &diagnostic
		) {
			const auto fail = [&](Status status, const char *text) {
				diagnostic = {status, std::string(nodeId), {}, text};
				return status;
			};
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
					return candidate.Id == nodeId;
				});
			if (node == document.Nodes.end() || node->Type != "pc.audio_window")
				return fail(Status::InvalidValue, "Audio Window observation requires an Audio Window node");
			const auto *entry = FindCatalogueEntry(node->Type);
			if (!entry) return fail(Status::InvalidValue, "Audio Window catalogue entry is unavailable");
			detail::EvaluationBudget ledger(std::min(byteBudget, Limits::MaximumEvaluationBytes));
			auto old = ledger.Reserve(output.Points.capacity() * sizeof(Vector2));
			if (!old)
				return fail(Status::LimitExceeded, "retained Audio Window points exceed observation cap");
			auto captured = ledger.Reserve(0);
			EvaluationSnapshot snapshot;
			const auto status =
				EvaluateNodeInputs(document, plan, nodeId, request, snapshot, diagnostic, ledger.Available());
			if (status != Status::Ok) return status;
			if (!captured || !captured->Resize(snapshot.RetainedBytes()))
				return fail(Status::LimitExceeded, "Audio Window snapshot exceeds observation cap");
			core::Metrics::Count(
				"imagegraph.audio_window.snapshot_retained_payload_bytes", snapshot.RetainedBytes()
			);
			auto views =
				ledger.Reserve(snapshot.Values().size() * sizeof(std::pair<std::string_view, const Value *>));
			if (!views)
				return fail(Status::LimitExceeded, "Audio Window view scratch exceeds observation cap");
			detail::NodeContext context(*node, *entry, request, ledger);
			context.Timeline = document.Timeline ? &*document.Timeline : nullptr;
			context.ValueViews.reserve(snapshot.Values().size());
			if (!views->Resize(
					context.ValueViews.capacity() * sizeof(std::pair<std::string_view, const Value *>)
				))
				return fail(
					Status::LimitExceeded, "Audio Window actual view capacity exceeds observation cap"
				);
			for (const auto &input : snapshot.Values())
				context.ValueViews.emplace_back(input.Port, &input.Data);
			context.ProcessorOriginalValues = context.ValueViews;
			const auto *input = context.Find("audio_data");
			const auto *audio = input ? std::get_if<AudioBit>(input) : nullptr;
			detail::AudioWindowGeometry geometry;
			if (!detail::ReadAudioWindowGeometry(context, audio, geometry)) {
				diagnostic = {
					context.FailureCode, std::string(nodeId), context.FailurePort, context.FailureMessage
				};
				return context.FailureCode;
			}
			const uint64_t key = GeometryKey(*audio);
			const auto samples = detail::AudioChannel(*audio, 0);
			const size_t count = std::min(size_t{320}, samples.size());
			bool changed = key != output.GeometryKey || output.Points.size() != count;
			// Fingerprints accelerate comparison; exact displayed point words prevent hash collision reuse.
			if (!changed)
				for (size_t i = 0; i < count; ++i) {
					const size_t index = count == 1 ? 0 : i * (samples.size() - 1) / (count - 1);
					if (std::bit_cast<uint64_t>(output.Points[i].X) !=
							std::bit_cast<uint64_t>(double(index) / samples.size()) ||
						std::bit_cast<uint64_t>(output.Points[i].Y) !=
							std::bit_cast<uint64_t>(samples[index])) {
						changed = true;
						break;
					}
				}
			auto pointCharge = ledger.Reserve(0);
			std::vector<Vector2> points;
			if (changed) {
				ENGINE_PROFILE("imagegraph.audio_window.geometry");
				if (!pointCharge || !pointCharge->Resize(count * sizeof(Vector2)))
					return fail(
						Status::LimitExceeded, "Audio Window candidate points exceed observation cap"
					);
				points.reserve(count);
				if (!pointCharge->Resize(points.capacity() * sizeof(Vector2)))
					return fail(
						Status::LimitExceeded, "Audio Window actual point capacity exceeds observation cap"
					);
				for (size_t i = 0; i < count; ++i) {
					const size_t index = count == 1 ? 0 : i * (samples.size() - 1) / (count - 1);
					points.push_back({double(index) / samples.size(), samples[index]});
				}
				core::Metrics::Count(
					"imagegraph.audio_window.allocated_payload_bytes", points.capacity() * sizeof(Vector2)
				);
				core::Metrics::Count("imagegraph.audio_window.allocations", count ? 1 : 0);
			} else
				core::Metrics::Count("imagegraph.audio_window.geometry_cache_hits", 1);
			if (changed) output.Points = std::move(points);
			output.GeometryKey = key;
			output.Packets = geometry.Packets;
			output.Channels = geometry.Channels;
			output.SampleRate = audio->SampleRate;
			output.Cursor = geometry.Packets ? geometry.Offset / geometry.Packets : 0;
			output.Start = geometry.Packets ? geometry.Start / geometry.Packets : 0;
			output.End = geometry.Packets ? geometry.End / geometry.Packets : 0;
			diagnostic = {};
			return Status::Ok;
		}
	}
	Status ResolveAudioWindowPresentation(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint64_t byteBudget,
		AudioWindowPresentation &output,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.audio_window.observe");
		try {
			return ResolveImpl(document, plan, nodeId, request, byteBudget, output, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "Audio Window observation allocation failed"};
		} catch (const std::length_error &) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "Audio Window observation allocation length exceeded"
			};
		}
		return diagnostic.Code;
	}
}
