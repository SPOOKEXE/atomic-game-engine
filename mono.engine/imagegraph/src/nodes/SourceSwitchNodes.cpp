#include "../ValuePayload.hpp"
#include "Families.hpp"
#include "SourceComparison.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		struct SwitchPair {
			size_t Group = 0;
			std::string_view Key, Payload;
		};
		bool SwitchPairs(
			NodeContext &c,
			std::array<SwitchPair, Limits::MaximumDynamicInputsPerNode / 2> &pairs,
			size_t &count
		) {
			if (c.Authored.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode)
				return c.Fail(Status::LimitExceeded, "switch exceeds dynamic input limit");
			for (const auto &input : c.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *slot = FindDynamicTemplate(c.Entry, input.Id, group);
				if (!slot || (slot->SourceIndex != 0 && slot->SourceIndex != 1) ||
					(slot->Type != ValueType::Any && input.Type != slot->Type))
					return c.Fail(
						Status::InvalidValue, "switch has an invalid source pair declaration", input.Id
					);
				size_t i = 0;
				while (i < count && pairs[i].Group != group)
					++i;
				if (i == count) {
					if (count == pairs.size())
						return c.Fail(Status::LimitExceeded, "switch exceeds source pair limit", input.Id);
					pairs[count++].Group = group;
				}
				auto &port = slot->SourceIndex == 0 ? pairs[i].Key : pairs[i].Payload;
				if (!port.empty())
					return c.Fail(Status::InvalidValue, "switch duplicates a source pair member", input.Id);
				port = input.Id;
			}
			for (const auto &pair : std::span(pairs).first(count))
				if (pair.Key.empty() || pair.Payload.empty())
					return c.Fail(
						Status::InvalidValue,
						"switch requires complete source input pairs",
						pair.Key.empty() ? pair.Payload : pair.Key
					);
			std::sort(pairs.begin(), pairs.begin() + count, [](const auto &a, const auto &b) {
				return a.Group < b.Group;
			});
			return true;
		}
		bool SwitchImageTree(
			const std::vector<ImageArrayItem> &items,
			size_t images,
			size_t depth,
			uint64_t &count,
			uint64_t &bytes
		) {
			if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count)
				return false;
			count += items.size();
			bytes += items.size() * sizeof(ImageArrayItem);
			for (const auto &item : items) {
				if (const auto *i = std::get_if<size_t>(&item.Data)) {
					if (*i >= images) return false;
				} else if (!SwitchImageTree(
							   std::get<std::vector<ImageArrayItem>>(item.Data),
							   images,
							   depth + 1,
							   count,
							   bytes
						   ))
					return false;
				if (bytes > Limits::MaximumArrayBytes) return false;
			}
			return true;
		}
		bool SwitchPublish(NodeContext &c, std::string_view port) {
			if (const auto *image = SourceComparisonSurface(c, port)) {
				auto *result = c.NewImage("result", image->Width, image->Height, image->Format);
				if (!result) return false;
				result->Pixels = image->Pixels;
				result->Hash = image->Hash;
			} else {
				for (const auto &[id, images] : c.ImageArrays)
					if (id == port && images) {
						if (images->Images.size() > Limits::MaximumArrayElements)
							return c.Fail(
								Status::LimitExceeded, "switch image array exceeds element limit", port
							);
						uint64_t count = 0,
								 bytes = sizeof(ImageArray) + images->Images.size() * sizeof(Image);
						if (!SwitchImageTree(images->Items, images->Images.size(), 1, count, bytes))
							return c.Fail(
								Status::LimitExceeded, "switch image array shape exceeds bounds", port
							);
						for (const auto &image : images->Images) {
							if (bytes > Limits::MaximumArrayBytes)
								return c.Fail(
									Status::LimitExceeded, "switch image array metadata exceeds bounds", port
								);
							if (!ValidSurfaceLayout(
									image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes
								) ||
								image.Pixels.size() > Limits::MaximumArrayBytes - bytes)
								return c.Fail(
									Status::LimitExceeded, "switch image array payload exceeds bounds", port
								);
							bytes += image.Pixels.size();
						}
						if (!c.ReserveOutput(bytes + std::string{}.capacity(), "result")) return false;
						c.OutputImageArrays.emplace_back("result", *images);
						return true;
					}
				const Value missing = double{0};
				const auto *value = c.Find(port);
				if (!value) value = &missing;
				const auto bytes = ValueClonePayloadBytes(*value);
				if (!bytes)
					return c.Fail(
						Status::LimitExceeded, "switch selected payload exceeds clone bounds", port
					);
				if (!c.ReserveOutput(*bytes, "result")) return false;
				c.SetValue("result", *value);
			}
			return c.FailureCode == Status::Ok;
		}
		bool Switch(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.switch");
			std::array<SwitchPair, Limits::MaximumDynamicInputsPerNode / 2> pairs{};
			size_t count = 0;
			if (!SwitchPairs(c, pairs, count)) return false;
			const bool threshold = c.Authored.Type == "pc.threshold_switch";
			SourceComparisonKey selector;
			if (!SourceComparisonReadKey(c, "index", selector, !threshold)) return false;
			double number = 0;
			bool hasNumber = false, frameMode = false;
			if (threshold) {
				const double mode = c.SourceChoice("type");
				if (c.FailureCode != Status::Ok) return false;
				if (mode != 0 && mode != 1)
					return c.Fail(
						Status::InvalidValue, "threshold switch type must be Number or Frame", "type"
					);
				c.SetSourceUpdateOnFrame(mode == 1);
				if (mode == 1) {
					frameMode = true;
					number =
						double(
							FrameTimeToReal({c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame})
						) +
						1;
					hasNumber = true;
				}
				if (c.FailureCode != Status::Ok) return false;
			}
			std::string_view selected = "default_value";
			uint64_t work = 0;
			for (const auto &pair : std::span(pairs).first(count)) {
				SourceComparisonKey key;
				if (!SourceComparisonReadKey(c, pair.Key, key, !threshold)) return false;
				if (key.Type == SourceComparisonKey::Kind::Text && key.Text.empty()) continue;
				const bool textOrdering = threshold && !frameMode &&
										  selector.Type == SourceComparisonKey::Kind::Text &&
										  key.Type == SourceComparisonKey::Kind::Text;
				const uint64_t needed =
					(key.Text.size() + (frameMode ? 0 : selector.Text.size())) * (textOrdering ? 2 : 1);
				if (needed > SourceComparisonWorkLimit - work)
					return c.Fail(Status::LimitExceeded, "switch exceeds comparison work limit", pair.Key);
				work += needed;
				bool matches = false;
				if (threshold) {
					if (!frameMode && selector.Type == SourceComparisonKey::Kind::Text &&
						key.Type == SourceComparisonKey::Kind::Text) {
						if (!SourceComparisonTextGreaterEqual(c, selector.Text, key.Text, matches, pair.Key))
							return false;
					} else if (!frameMode && selector.Type == SourceComparisonKey::Kind::Undefined &&
							   key.Type == SourceComparisonKey::Kind::Undefined)
						matches = true;
					else {
						double limit = 0;
						if (!frameMode) hasNumber = SourceComparisonNumber(c, selector, number, "index");
						const bool hasLimit = SourceComparisonNumber(c, key, limit, pair.Key);
						if (c.FailureCode != Status::Ok) return false;
						matches = hasNumber && hasLimit &&
								  (!frameMode && selector.Integer && key.Integer
									   ? *selector.Integer >= *key.Integer
									   : number >= limit || std::abs(number - limit) <= 1e-5);
					}
				} else if (!SourceComparisonEqual(c, selector, key, matches, pair.Key))
					return false;
				if (matches) selected = pair.Payload;
			}
			return SwitchPublish(c, selected);
		}
	}
	std::span<const ExecutorEntry> SourceSwitchExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.switch", Switch, true}, {"pc.threshold_switch", Switch, true}
		};
		return entries;
	}
}
