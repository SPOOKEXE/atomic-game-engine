#pragma once
#include <engine/imagegraph/ComposerLuaHost.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <array>

namespace studio::detail {
	// A pending preview observes one immutable Lua generation. Other authoring
	// capabilities can still run newer frames on the same VM.
	struct ImageGraphLuaReceipts {
		struct Entry {
			engine::imagegraph::HostNodeCapture Capture;
			uint64_t Seed = 0, Bytes = 0;
			std::optional<engine::imagegraph::TimelineSettings> Timeline;
			std::optional<engine::imagegraph::SurfaceFormat> OutputFormat;
			int64_t Interpolation = 1;
		};
		std::optional<engine::imagegraph::FrameTime> Frame;
		std::array<std::optional<Entry>, 64> Entries;
		uint64_t Bytes = sizeof(Entries);
		bool Active = false;
		void Clear() {
			for (auto &entry : Entries)
				entry.reset();
			Frame.reset();
			Bytes = sizeof(Entries);
			Active = false;
		}
		void Begin(engine::imagegraph::FrameTime frame) {
			if (Frame != frame) {
				Clear();
				Frame = frame;
			}
		}
		bool Applies(const engine::imagegraph::EvaluationRequest &request) const {
			return Active && Frame == engine::imagegraph::GetFrameTime(request);
		}
		template <class Measure>
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::ComposerLuaHost &provider,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure,
			const Measure &measure
		) try {
			using namespace engine::imagegraph;
			const auto oldOutput = measure(output);
			if (!oldOutput || Bytes > invocation.MaximumOperationBytes ||
				*oldOutput > invocation.MaximumOperationBytes - Bytes) {
				failure = "Pending Lua receipts exceed the operation byte budget";
				return false;
			}
			const uint64_t available = invocation.MaximumOperationBytes - Bytes - *oldOutput;
			const auto matches = [&](const Entry &entry) {
				if (entry.Seed != invocation.Request.Seed ||
					bool(entry.Timeline) != bool(invocation.Timeline) ||
					(entry.Timeline && *entry.Timeline != *invocation.Timeline) ||
					entry.OutputFormat != invocation.OutputFormat ||
					entry.Interpolation != invocation.Interpolation ||
					entry.Capture.Authored != invocation.Authored ||
					entry.Capture.Inputs.size() != invocation.Inputs.size() ||
					!std::equal(
						entry.Capture.Inputs.begin(), entry.Capture.Inputs.end(), invocation.Inputs.begin()
					) ||
					entry.Capture.InputImages.size() != invocation.Images.size())
					return false;
				for (size_t i = 0; i < invocation.Images.size(); ++i) {
					const auto &image = invocation.Images[i];
					if (!image.Data || entry.Capture.InputImages[i].Port != image.Port ||
						entry.Capture.InputImages[i].Hash != SurfaceHash(*image.Data))
						return false;
				}
				return true;
			};
			std::optional<Entry> *empty = nullptr;
			for (auto &entry : Entries) {
				if (!entry) {
					if (!empty) empty = &entry;
					continue;
				}
				if (entry->Capture.Authored.Id != invocation.Authored.Id) continue;
				if (!matches(*entry)) {
					failure = "Pending Lua inputs changed within the captured preview generation";
					return false;
				}
				if (entry->Bytes > available) {
					failure = "Pending Lua receipt clone exceeds the operation byte budget";
					return false;
				}
				HostNodeCapture candidate = entry->Capture;
				output = std::move(candidate);
				return true;
			}
			if (!empty) {
				failure = "Pending preview exceeds its Lua receipt count limit";
				return false;
			}
			const uint64_t timelineMinimum =
				invocation.Timeline ? invocation.Timeline->Playback.size() + 1 : 0;
			if (timelineMinimum > available / 3) {
				failure = "Pending Lua timeline metadata exceeds its byte budget";
				return false;
			}
			auto timeline =
				invocation.Timeline ? std::optional<TimelineSettings>(*invocation.Timeline) : std::nullopt;
			const uint64_t timelineBytes = timeline ? timeline->Playback.capacity() + 1 : 0;
			if (timelineBytes > available / 3) {
				failure = "Pending Lua timeline storage exceeds its byte budget";
				return false;
			}
			const uint64_t captureAllowance = available - timelineBytes;
			HostNodeInvocation bounded = invocation;
			bounded.MaximumOperationBytes = captureAllowance / 2;
			HostNodeCapture candidate;
			if (!provider.Capture(bounded, candidate, failure)) return false;
			const auto bytes = measure(candidate);
			if (!bytes || *bytes > captureAllowance / 2) {
				failure = "Pending Lua receipt exceeds the operation byte budget";
				return false;
			}
			Entry entry{
				candidate,
				invocation.Request.Seed,
				*bytes + timelineBytes,
				std::move(timeline),
				invocation.OutputFormat,
				invocation.Interpolation
			};
			*empty = std::move(entry);
			Bytes += *bytes + timelineBytes;
			output = std::move(candidate);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "Pending Lua receipt allocation was refused";
			return false;
		}
	};
	struct ImageGraphLuaReceiptScope {
		ImageGraphLuaReceipts &Receipts;
		bool Previous;
		explicit ImageGraphLuaReceiptScope(ImageGraphLuaReceipts &receipts)
			: Receipts(receipts), Previous(receipts.Active) {
			receipts.Active = true;
		}
		~ImageGraphLuaReceiptScope() {
			Receipts.Active = Previous;
		}
	};
} // namespace studio::detail
