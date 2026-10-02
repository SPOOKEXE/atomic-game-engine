#pragma once

// An analytical oracle for the benchmark's plain Frames. It reads authored
// ancestry and scalar lengths, never the layout's measured extents or helpers.
#include <engine/core/types/Rect.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace layout_fixture {
	inline void Require(bool condition) {
		if (!condition) throw std::runtime_error("plain GUI tree analytical layout oracle failed");
	}
	inline bool Near(float actual, float expected) {
		// Parent corner addition/subtraction rounds separately in production.
		return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= 0.001f;
	}
	inline void CheckVector(engine::core::Vector2 actual, engine::core::Vector2 expected) {
		Require(Near(actual.X, expected.X) && Near(actual.Y, expected.Y));
	}
	inline void CheckResolved(const engine::gui::Resolved &actual, const engine::gui::Resolved &expected) {
		CheckVector(actual.AbsolutePosition, expected.AbsolutePosition);
		CheckVector(actual.AbsoluteSize, expected.AbsoluteSize);
		Require(Near(actual.AbsoluteRotation, expected.AbsoluteRotation));
		CheckVector(actual.Clip.Min, expected.Clip.Min);
		CheckVector(actual.Clip.Max, expected.Clip.Max);
		Require(actual.TextSize == expected.TextSize);
		CheckVector(actual.TextBounds, expected.TextBounds);
		Require(
			actual.Depth == expected.Depth && actual.Order == expected.Order &&
			actual.Rendered == expected.Rendered && actual.TextFits == expected.TextFits
		);
		for (size_t index = 0; index < std::size(actual.Reserved); ++index)
			Require(actual.Reserved[index] == expected.Reserved[index]);
	}
	inline size_t CheckTree(
		engine::ecs::Store &store,
		engine::ecs::Entity collector,
		const engine::gui::Screen &screen,
		size_t expectedCount
	) {
		using namespace engine;
		// This fixture deliberately uses the default display profile, without
		// inset, logical scaling, labels, automatic-size or layout modifiers.
		Require(
			screen.TopInset == 0 && screen.FramebufferScale == 1 && screen.InterfaceScale == 1 &&
			screen.SafeArea.Left == 0 && screen.SafeArea.Top == 0 && screen.SafeArea.Right == 0 &&
			screen.SafeArea.Bottom == 0 && screen.Occluded.Left == 0 && screen.Occluded.Top == 0 &&
			screen.Occluded.Right == 0 && screen.Occluded.Bottom == 0
		);
		const core::Rect canvas{{0, 0}, {screen.Width, screen.Height}};
		gui::Resolved collectorExpected;
		collectorExpected.AbsoluteSize = {screen.Width, screen.Height};
		collectorExpected.Clip = canvas;
		collectorExpected.Rendered = true;
		const auto *collectorActual = store.Get<gui::Resolved>(collector);
		Require(collectorActual != nullptr);
		CheckResolved(*collectorActual, collectorExpected);
		const auto *area = store.Get<gui::Canvas>(collector);
		const auto *transform = store.Get<gui::CanvasTransform>(collector);
		Require(area != nullptr && transform != nullptr);
		CheckVector(area->Area.Min, canvas.Min);
		CheckVector(area->Area.Max, canvas.Max);
		CheckVector(transform->Origin, {0, 0});
		CheckVector(transform->Scale, {1, 1});
		size_t count = 0;
		store.Each<const gui::Element>([&](ecs::Entity node, const gui::Element &) {
			std::array<ecs::Entity, 257> chain;
			size_t depth = 0;
			for (auto ancestor = node; ancestor != collector; ancestor = store.ParentOf(ancestor)) {
				Require(ancestor != ecs::NULL_ENTITY && depth < chain.size());
				chain[depth++] = ancestor;
			}
			gui::Resolved expected;
			expected.AbsoluteSize = {screen.Width, screen.Height};
			expected.Clip = canvas;
			expected.Rendered = true;
			for (size_t index = depth; index > 0; --index) {
				const auto *element = store.Get<gui::Element>(chain[index - 1]);
				Require(
					element != nullptr && element->Automatic == gui::AutomaticSize::None &&
					element->Constraint == gui::SizeConstraint::RelativeXY &&
					store.Get<gui::Label>(chain[index - 1]) == nullptr
				);
				if (index != depth) {
					const auto *parent = store.Get<gui::Element>(chain[index]);
					if (parent->ClipsDescendants) {
						expected.Clip.Min.X = std::max(expected.Clip.Min.X, expected.AbsolutePosition.X);
						expected.Clip.Min.Y = std::max(expected.Clip.Min.Y, expected.AbsolutePosition.Y);
						expected.Clip.Max.X = std::min(
							expected.Clip.Max.X, expected.AbsolutePosition.X + expected.AbsoluteSize.X
						);
						expected.Clip.Max.Y = std::min(
							expected.Clip.Max.Y, expected.AbsolutePosition.Y + expected.AbsoluteSize.Y
						);
					}
				}
				const core::Vector2 size{
					element->Size.X.Scale * expected.AbsoluteSize.X + element->Size.X.Offset,
					element->Size.Y.Scale * expected.AbsoluteSize.Y + element->Size.Y.Offset
				};
				expected.AbsolutePosition.X += element->Position.X.Scale * expected.AbsoluteSize.X +
											   element->Position.X.Offset - element->AnchorPoint.X * size.X;
				expected.AbsolutePosition.Y += element->Position.Y.Scale * expected.AbsoluteSize.Y +
											   element->Position.Y.Offset - element->AnchorPoint.Y * size.Y;
				expected.AbsoluteSize = size;
				expected.AbsoluteRotation += element->Rotation;
				expected.Rendered = expected.Rendered && element->Visible;
			}
			expected.Depth = static_cast<int32_t>(depth);
			const auto *actual = store.Get<gui::Resolved>(node);
			Require(actual != nullptr);
			if (expected.Rendered)
				CheckResolved(*actual, expected);
			else
				Require(!actual->Rendered);
			++count;
		});
		Require(count == expectedCount);
		return count;
	}
	inline bool CanonicalEnabled() {
		static const bool enabled = [] {
			const char *value = std::getenv("ATOMIC_GUI_LAYOUT_CANONICAL");
			return value != nullptr && std::string_view(value) == "1";
		}();
		return enabled;
	}
	inline void DumpResolved(engine::ecs::Store &store, size_t row, size_t call) {
		using namespace engine;
		struct Record {
			ecs::Entity Entity;
			ecs::Entity Parent;
			gui::Resolved Value;
		};
		std::vector<Record> records;
		store.Each<const gui::Resolved>([&](ecs::Entity entity, const gui::Resolved &value) {
			if (records.size() == 10001) throw std::runtime_error("GUI canonical record bound exceeded");
			records.push_back({entity, store.ParentOf(entity), value});
		});
		std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
			return a.Entity.Id < b.Entity.Id;
		});
		// Entity words identify this fixed fixture's creation/generation order,
		// not a new persistent world format. Explicit fields exclude padding.
		std::printf(
			"# gui-layout-canonical row=%zu call=%zu records=%zu words_per_record=21 bench_timing_valid=0 "
			"hex=",
			row,
			call,
			records.size()
		);
		std::array<char, 4096> block;
		size_t used = 0;
		constexpr std::string_view hex = "0123456789abcdef";
		const auto word = [&](uint32_t value) {
			if (used + 8 > block.size()) {
				std::fwrite(block.data(), 1, used, stdout);
				used = 0;
			}
			for (size_t digit = 0; digit < 8; ++digit)
				block[used++] = hex[(value >> ((7 - digit) * 4)) & 15u];
		};
		for (const auto &record : records) {
			const auto &v = record.Value;
			word(static_cast<uint32_t>(record.Entity.Id >> 32u));
			word(static_cast<uint32_t>(record.Entity.Id));
			word(static_cast<uint32_t>(record.Parent.Id >> 32u));
			word(static_cast<uint32_t>(record.Parent.Id));
			word(std::bit_cast<uint32_t>(v.AbsolutePosition.X));
			word(std::bit_cast<uint32_t>(v.AbsolutePosition.Y));
			word(std::bit_cast<uint32_t>(v.AbsoluteSize.X));
			word(std::bit_cast<uint32_t>(v.AbsoluteSize.Y));
			word(std::bit_cast<uint32_t>(v.AbsoluteRotation));
			word(std::bit_cast<uint32_t>(v.Clip.Min.X));
			word(std::bit_cast<uint32_t>(v.Clip.Min.Y));
			word(std::bit_cast<uint32_t>(v.Clip.Max.X));
			word(std::bit_cast<uint32_t>(v.Clip.Max.Y));
			word(static_cast<uint32_t>(v.TextSize));
			word(std::bit_cast<uint32_t>(v.TextBounds.X));
			word(std::bit_cast<uint32_t>(v.TextBounds.Y));
			word(static_cast<uint32_t>(v.Depth));
			word(static_cast<uint32_t>(v.Order));
			word(v.Rendered);
			word(v.TextFits);
			word(static_cast<uint32_t>(v.Reserved[0]) | (static_cast<uint32_t>(v.Reserved[1]) << 8u));
		}
		std::fwrite(block.data(), 1, used, stdout);
		std::putchar('\n');
	}
} // namespace layout_fixture
