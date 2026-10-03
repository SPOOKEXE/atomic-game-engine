#pragma once

#include "ImageGraphComposerCadence.hpp"
#include "ImageGraphPreviewResult.hpp"

#include <array>
#include <span>

namespace studio::detail {
	// The source picks a top-level row modulo its length. Deeper navigation is a
	// native panel feature; the owned sequence tree is never flattened or packed.
	struct ImageGraphPreviewSequence {
		struct Key {
			ImageGraphComposerCadence::Identity Identity;
			engine::imagegraph::FrameTime Frame;
			uint8_t PlaybackObservation = 0;
			bool operator==(const Key &) const = default;
		};
		struct Selection {
			const engine::imagegraph::Image *Image = nullptr;
			size_t Count = 0, Depth = 0;
			bool Branch = false;
		};
		engine::imagegraph::ImageArray Data;
		Key Completed;
		std::array<uint64_t, engine::imagegraph::Limits::MaximumArrayDepth + 1> Indices{};
		size_t Levels = 1;
		bool HaveSequence = false, Valid = false;
		uint64_t RetainedBytes() const noexcept {
			const uint64_t data = ImageGraphSequenceBytes(Data);
			const uint64_t fixed = sizeof(*this) - sizeof(Data);
			if (data > UINT64_MAX - fixed || Completed.Identity.Output.capacity() > UINT64_MAX - fixed - data)
				return UINT64_MAX;
			return fixed + data + Completed.Identity.Output.capacity();
		}
		bool Matches(const Key &key) const {
			return HaveSequence && Valid && Completed == key;
		}
		void Invalidate(bool resetSelection = false) {
			Valid = false;
			if (resetSelection) {
				Indices = {};
				Levels = 1;
			}
		}
		void Clear() {
			*this = {};
		}
		static Selection
		Select(const engine::imagegraph::ImageArray &data, std::span<const uint64_t> indices) noexcept {
			using engine::imagegraph::ImageArrayItem;
			if (indices.empty() || indices.size() > engine::imagegraph::Limits::MaximumArrayDepth + 1)
				return {};
			if (data.Items.empty()) {
				if (data.Images.empty()) return {};
				return {&data.Images[indices[0] % data.Images.size()], data.Images.size(), 0, false};
			}
			const auto *items = &data.Items;
			for (size_t depth = 0; depth < indices.size(); ++depth) {
				if (items->empty()) return {nullptr, 0, depth, false};
				const auto &item = (*items)[indices[depth] % items->size()];
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					return {
						*index < data.Images.size() ? &data.Images[*index] : nullptr,
						items->size(),
						depth,
						false
					};
				}
				if (depth + 1 == indices.size()) return {nullptr, items->size(), depth, true};
				items = &std::get<std::vector<ImageArrayItem>>(item.Data);
			}
			return {};
		}
		Selection Selected() const noexcept {
			return HaveSequence ? Select(Data, {Indices.data(), Levels}) : Selection{};
		}
		bool KeepsSelection(const Key &key) const noexcept {
			return HaveSequence && Completed.Identity.Owner == key.Identity.Owner &&
				   Completed.Identity.Output == key.Identity.Output &&
				   Completed.Identity.Revision == key.Identity.Revision &&
				   Completed.Identity.InputRevision == key.Identity.InputRevision;
		}
		Selection
		SelectReplacement(const engine::imagegraph::ImageArray &candidate, const Key &key) const noexcept {
			if (KeepsSelection(key)) return Select(candidate, {Indices.data(), Levels});
			const uint64_t zero = 0;
			return Select(candidate, {&zero, 1});
		}

		// Candidate ownership remains with the caller until the selected display
		// upload succeeds. This admits old/new overlap before any publication.
		bool Admit(
			const engine::imagegraph::ImageArray &candidate,
			const Key &key,
			uint64_t maximumBytes,
			engine::imagegraph::Diagnostic &diagnostic
		) const {
			using namespace engine::imagegraph;
			if (!ValidateImageGraphSequence(candidate, diagnostic)) return false;
			const uint64_t oldBytes = RetainedBytes(), bytes = ImageGraphSequenceBytes(candidate);
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || oldBytes > maximumBytes ||
				bytes > maximumBytes - oldBytes ||
				key.Identity.Output.capacity() > maximumBytes - oldBytes - bytes) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "Completed sequence replacement exceeds caller allowance"
				};
				return false;
			}
			diagnostic = {};
			return true;
		}
		void Publish(engine::imagegraph::ImageArray &&candidate, Key &&key) noexcept {
			if (!KeepsSelection(key)) {
				Indices = {};
				Levels = 1;
			}
			Data = std::move(candidate);
			Completed = std::move(key);
			HaveSequence = Valid = true;
		}
	};
}
