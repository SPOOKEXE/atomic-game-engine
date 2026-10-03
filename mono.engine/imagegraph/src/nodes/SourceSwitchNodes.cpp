#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t SwitchWorkLimit = 16 * 1024 * 1024;
		struct SwitchPair {
			size_t Group = 0;
			std::string_view Key, Payload;
		};
		struct SwitchKey {
			enum class Kind { Number, Text, Undefined, Identity, Surface } Type = Kind::Undefined;
			double Number = 0;
			std::optional<int64_t> Integer;
			std::string_view Text;
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
		// Pinned HTML5 yyGetReal/yyCompareVal use this bounded numeric-prefix grammar, not stringification.
		std::optional<double> SwitchTextNumber(std::string_view text) {
			constexpr std::array<std::string_view, 11> spaces{
				"\xc2\xa0",
				"\xe1\x9a\x80",
				"\xe2\x80\xa8",
				"\xe2\x80\xa9",
				"\xe2\x80\xaf",
				"\xe2\x81\x9f",
				"\xe3\x80\x80",
				"\xef\xbb\xbf",
				"\t",
				"\n",
				"\r"
			};
			while (!text.empty()) {
				if (text.front() == ' ' || text.front() == '\v' || text.front() == '\f') {
					text.remove_prefix(1);
					continue;
				}
				if (text.size() >= 3 && text.substr(0, 2) == "\xe2\x80" &&
					static_cast<unsigned char>(text[2]) >= 0x80 &&
					static_cast<unsigned char>(text[2]) <= 0x8a) {
					text.remove_prefix(3);
					continue;
				}
				bool removed = false;
				for (auto space : spaces)
					if (text.starts_with(space)) {
						text.remove_prefix(space.size());
						removed = true;
						break;
					}
				if (!removed) break;
			}
			const auto digit = [&](size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
			size_t start = !text.empty() && (text.front() == '+' || text.front() == '-') ? 1 : 0;
			size_t i = start;
			while (i - start < 30 && digit(i))
				++i;
			if (i < text.size() && text[i] == '.' && digit(i + 1)) {
				const auto fraction = ++i;
				while (i - fraction < 30 && digit(i))
					++i;
			} else if (i == start)
				return std::nullopt;
			if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
				auto exponent = i + 1;
				if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) ++exponent;
				if (digit(exponent)) {
					i = exponent + 1;
					if ((text[exponent] == '1' || text[exponent] == '2') && digit(i)) ++i;
				}
			}
			const auto begin = text.data() + (!text.empty() && text.front() == '+' ? 1 : 0);
			double number = 0;
			const auto parsed = std::from_chars(begin, text.data() + i, number);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + i && std::isfinite(number)
					   ? std::optional<double>(number)
					   : std::nullopt;
		}
		const Image *SwitchSurface(const NodeContext &c, std::string_view port) {
			for (const auto &[id, image] : c.Images)
				if (id == port) return image;
			return nullptr;
		}
		bool SwitchReadKey(NodeContext &c, std::string_view port, SwitchKey &key, bool textDefault) {
			if (SwitchSurface(c, port)) {
				key.Type = SwitchKey::Kind::Surface;
				return true;
			}
			for (const auto &[id, images] : c.ImageArrays)
				if (id == port && images) {
					key.Type = SwitchKey::Kind::Identity;
					return true;
				}
			const auto *value = c.Find(port);
			if (!value) {
				key.Type = textDefault ? SwitchKey::Kind::Text : SwitchKey::Kind::Number;
				return true;
			}
			if (!ValidRuntimeValue(*value))
				return c.Fail(Status::InvalidValue, "switch selector value is invalid", port);
			if (const auto *text = std::get_if<std::string>(value)) {
				key.Type = SwitchKey::Kind::Text;
				key.Text = *text;
			} else if (std::holds_alternative<UndefinedValue>(*value))
				key.Type = SwitchKey::Kind::Undefined;
			else if (const auto *real = std::get_if<double>(value)) {
				key.Type = SwitchKey::Kind::Number;
				key.Number = *real;
			} else if (const auto *integer = std::get_if<int64_t>(value)) {
				key.Type = SwitchKey::Kind::Number;
				key.Number = double(*integer);
				key.Integer = *integer;
			} else if (const auto *boolean = std::get_if<bool>(value)) {
				key.Type = SwitchKey::Kind::Number;
				key.Number = *boolean ? 1 : 0;
			} else if (const auto *choice = std::get_if<EnumValue>(value)) {
				key.Type = SwitchKey::Kind::Number;
				key.Number = double(choice->Value);
			} else if (const auto *colour = std::get_if<Colour>(value)) {
				key.Type = SwitchKey::Kind::Number;
				key.Number = double(
					uint32_t(colour->Red) | (uint32_t(colour->Green) << 8) | (uint32_t(colour->Blue) << 16) |
					(uint32_t(colour->Alpha) << 24)
				);
			} else if (std::holds_alternative<SurfaceValue>(*value))
				key.Type = SwitchKey::Kind::Surface;
			else
				key.Type = SwitchKey::Kind::Identity;
			return true;
		}
		bool SwitchEqual(
			NodeContext &c, const SwitchKey &a, const SwitchKey &b, bool &equal, std::string_view port
		) {
			using K = SwitchKey::Kind;
			equal = false;
			if (a.Type == K::Surface || b.Type == K::Surface)
				return c.Fail(
					Status::UnsupportedExecution,
					"switch surface selector needs source handle observations",
					port
				);
			if (a.Type == K::Identity && b.Type == K::Identity)
				return c.Fail(
					Status::UnsupportedExecution,
					"switch array or struct selector equality needs source identity observations",
					port
				);
			if (a.Type == K::Identity || b.Type == K::Identity) return true;
			if (a.Type == K::Undefined || b.Type == K::Undefined) {
				equal = a.Type == b.Type;
				return true;
			}
			if (a.Type == K::Text && b.Type == K::Text) {
				equal = a.Text == b.Text;
				return true;
			}
			if (a.Integer && b.Integer) {
				equal = *a.Integer == *b.Integer;
				return true;
			}
			const auto x = a.Type == K::Text ? SwitchTextNumber(a.Text) : std::optional<double>(a.Number);
			const auto y = b.Type == K::Text ? SwitchTextNumber(b.Text) : std::optional<double>(b.Number);
			equal = x && y && std::abs(*x - *y) <= 1e-5;
			return true;
		}
		struct SwitchUtf16 {
			std::string_view Bytes;
			uint16_t Pending = 0;
			bool Next(uint16_t &unit) {
				if (Pending) {
					unit = Pending;
					Pending = 0;
					return true;
				}
				if (Bytes.empty()) return false;
				const auto first = static_cast<uint8_t>(Bytes.front());
				size_t size = first < 0x80					   ? 1
							  : first >= 0xc2 && first <= 0xdf ? 2
							  : first >= 0xe0 && first <= 0xef ? 3
							  : first >= 0xf0 && first <= 0xf4 ? 4
															   : 0;
				if (!size || Bytes.size() < size) return false;
				uint32_t point = first & (size == 1 ? 0x7f : size == 2 ? 0x1f : size == 3 ? 0xf : 7);
				for (size_t i = 1; i < size; ++i) {
					const auto next = static_cast<uint8_t>(Bytes[i]);
					if ((next & 0xc0) != 0x80) return false;
					point = (point << 6) | (next & 0x3f);
				}
				if ((size == 2 && point < 0x80) || (size == 3 && point < 0x800) ||
					(size == 4 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) ||
					point > 0x10ffff)
					return false;
				Bytes.remove_prefix(size);
				if (point <= 0xffff)
					unit = uint16_t(point);
				else {
					point -= 0x10000;
					unit = uint16_t(0xd800 + (point >> 10));
					Pending = uint16_t(0xdc00 + (point & 0x3ff));
				}
				return true;
			}
		};
		// Source strings order UTF-16 code units; UTF-8 bytes reverse astral versus high-BMP ordering.
		bool SwitchTextGreaterEqual(
			NodeContext &c, std::string_view a, std::string_view b, bool &matches, std::string_view port
		) {
			for (auto text : {a, b}) {
				SwitchUtf16 check{text};
				uint16_t unit = 0;
				while (check.Next(unit)) {}
				if (!check.Bytes.empty())
					return c.Fail(
						Status::UnsupportedExecution,
						"threshold raw text has no valid Unicode source representation",
						port
					);
			}
			SwitchUtf16 left{a}, right{b};
			uint16_t x = 0, y = 0;
			while (true) {
				const bool hasX = left.Next(x), hasY = right.Next(y);
				if (!hasX || !hasY) {
					matches = hasX || !hasY;
					return true;
				}
				if (x != y) {
					matches = x > y;
					return true;
				}
			}
		}
		bool SwitchNumber(NodeContext &c, const SwitchKey &key, double &value, std::string_view port) {
			using K = SwitchKey::Kind;
			if (key.Type == K::Identity || key.Type == K::Surface)
				return c.Fail(
					Status::UnsupportedExecution,
					"threshold selector getter or array comparison needs source observations",
					port
				);
			if (key.Type == K::Undefined) return false;
			if (key.Type == K::Text) {
				const auto parsed = SwitchTextNumber(key.Text);
				if (!parsed)
					return c.Fail(
						Status::UnsupportedExecution,
						"threshold comparison cannot compare nonnumeric raw text",
						port
					);
				value = *parsed;
			} else
				value = key.Number;
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
			if (const auto *image = SwitchSurface(c, port)) {
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
			SwitchKey selector;
			if (!SwitchReadKey(c, "index", selector, !threshold)) return false;
			double number = 0;
			bool hasNumber = false, frameMode = false;
			if (threshold) {
				const double mode = c.SourceChoice("type");
				if (c.FailureCode != Status::Ok) return false;
				if (mode != 0 && mode != 1)
					return c.Fail(
						Status::InvalidValue, "threshold switch type must be Number or Frame", "type"
					);
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
				SwitchKey key;
				if (!SwitchReadKey(c, pair.Key, key, !threshold)) return false;
				if (key.Type == SwitchKey::Kind::Text && key.Text.empty()) continue;
				const bool textOrdering = threshold && !frameMode && selector.Type == SwitchKey::Kind::Text &&
										  key.Type == SwitchKey::Kind::Text;
				const uint64_t needed =
					(key.Text.size() + (frameMode ? 0 : selector.Text.size())) * (textOrdering ? 2 : 1);
				if (needed > SwitchWorkLimit - work)
					return c.Fail(Status::LimitExceeded, "switch exceeds comparison work limit", pair.Key);
				work += needed;
				bool matches = false;
				if (threshold) {
					if (!frameMode && selector.Type == SwitchKey::Kind::Text &&
						key.Type == SwitchKey::Kind::Text) {
						if (!SwitchTextGreaterEqual(c, selector.Text, key.Text, matches, pair.Key))
							return false;
					} else if (!frameMode && selector.Type == SwitchKey::Kind::Undefined &&
							   key.Type == SwitchKey::Kind::Undefined)
						matches = true;
					else {
						double limit = 0;
						if (!frameMode) hasNumber = SwitchNumber(c, selector, number, "index");
						const bool hasLimit = SwitchNumber(c, key, limit, pair.Key);
						if (c.FailureCode != Status::Ok) return false;
						matches = hasNumber && hasLimit &&
								  (!frameMode && selector.Integer && key.Integer
									   ? *selector.Integer >= *key.Integer
									   : number >= limit || std::abs(number - limit) <= 1e-5);
					}
				} else if (!SwitchEqual(c, selector, key, matches, pair.Key))
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
