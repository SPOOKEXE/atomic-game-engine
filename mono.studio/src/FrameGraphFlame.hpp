#pragma once

// Snapshot-owned flame metadata and row buckets. Painting keeps original span order.
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <span>
#include <studio/Diagnostics.hpp>

namespace studio::frame_graph_detail {
	inline constexpr size_t ACCOUNTING_PASS = 0;
	inline constexpr size_t CPU_PASS = 1;
	inline constexpr size_t GPU_PASS = 2;

	inline size_t FlamePassOf(const DiagnosticSpan &span, uint32_t source, size_t accountingBegin) {
		if (span.Category == engine::core::ProfileCategory::Gpu) return GPU_PASS;
		return source >= accountingBegin ? ACCOUNTING_PASS : CPU_PASS;
	}

	template <class Cache>
	void BuildFlameCache(
		std::span<const DiagnosticSpan> spans,
		std::span<const uint32_t> rows,
		std::span<const uint32_t> sources,
		size_t accountingBegin,
		uint32_t rowCapacity,
		Cache &cache
	) {
		cache.CpuRows = 0;
		cache.GpuRows = 0;
		cache.GpuMaximumMilliseconds = 0;
		cache.GpuMaximumSamples = 0;
		cache.HasOverlap = false;
		for (auto &pass : cache.Passes) {
			pass.RowOffsets.assign(static_cast<size_t>(rowCapacity) + 1, 0);
			pass.Visible.clear();
			pass.OriginalIndices.clear();
			pass.Prepared = false;
		}
		for (uint32_t index = 0; index < spans.size(); index++) {
			const auto &span = spans[index];
			const uint32_t source = sources.empty() ? index : sources[index];
			const size_t kind = FlamePassOf(span, source, accountingBegin);
			cache.Passes[kind].RowOffsets[rows[index] + 1]++;
			if (kind == GPU_PASS) {
				cache.GpuRows = std::max(cache.GpuRows, rows[index] + 1);
				if (span.Milliseconds > cache.GpuMaximumMilliseconds) {
					cache.GpuMaximumMilliseconds = span.Milliseconds;
					cache.GpuMaximumSamples = span.Occurrences;
				}
			} else {
				cache.CpuRows = std::max(cache.CpuRows, rows[index] + 1);
				cache.HasOverlap |= rows[index] > span.Depth;
			}
		}
		for (size_t kind = 0; kind < cache.Passes.size(); kind++) {
			auto &pass = cache.Passes[kind];
			pass.RowOffsets.resize(static_cast<size_t>(kind == GPU_PASS ? cache.GpuRows : cache.CpuRows) + 1);
			for (size_t row = 1; row < pass.RowOffsets.size(); row++)
				pass.RowOffsets[row] += pass.RowOffsets[row - 1];
			pass.Indices.resize(pass.RowOffsets.back());
			pass.OriginalIndices.reserve(pass.Indices.size());
			pass.WriteOffsets.assign(pass.RowOffsets.begin(), pass.RowOffsets.end() - 1);
		}
		for (uint32_t index = 0; index < spans.size(); index++) {
			const uint32_t source = sources.empty() ? index : sources[index];
			auto &pass = cache.Passes[FlamePassOf(spans[index], source, accountingBegin)];
			pass.Indices[pass.WriteOffsets[rows[index]]++] = index;
			pass.OriginalIndices.push_back(index);
		}
	}

	template <class Pass>
	void PrepareFlameRows(Pass &pass, float originY, float rowHeight, float clipMinimum, float clipMaximum) {
		if (clipMaximum <= clipMinimum) {
			pass.Visible.clear();
			pass.FirstRow = 0;
			pass.EndRow = 0;
			pass.Prepared = true;
			return;
		}
		const uint32_t count = static_cast<uint32_t>(pass.RowOffsets.size() - 1);
		const float ceiling = static_cast<float>(count);
		// One guard row on each edge absorbs floating-point boundary rounding.
		const auto first = static_cast<uint32_t>(
			std::clamp(std::floor((clipMinimum - originY) / rowHeight) - 1.0f, 0.0f, ceiling)
		);
		const auto end = std::max(
			first,
			static_cast<uint32_t>(
				std::clamp(std::ceil((clipMaximum - originY) / rowHeight) + 1.0f, 0.0f, ceiling)
			)
		);
		if (pass.Prepared && pass.FirstRow == first && pass.EndRow == end) return;
		pass.FirstRow = first;
		pass.EndRow = end;
		pass.Prepared = true;
		if (first == 0 && end == count) {
			pass.Visible = pass.OriginalIndices;
			return;
		}
		pass.Visible.assign(
			pass.Indices.begin() + pass.RowOffsets[first], pass.Indices.begin() + pass.RowOffsets[end]
		);
		// Rows do not overlap vertically, but preserve original command and hover priority too.
		std::sort(pass.Visible.begin(), pass.Visible.end());
	}
	inline unsigned int
	UncachedFlameColour(engine::core::ProfileCategory category, unsigned int packedAccent) {
		const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(packedAccent);

		float hue = 0.0f;
		float saturation = 0.0f;
		float value = 0.0f;
		ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, hue, saturation, value);

		// Keep idle subdued so a long display wait does not dominate the graph.
		if (category == engine::core::ProfileCategory::Idle) {
			return IM_COL32(70, 74, 86, 190);
		}

		// Spread neighbouring categories around the accent hue. The enum-sized
		// check requires an explicit colour choice when a category is added.
		constexpr float TURN[] = {
			0.00f, // engine
			0.52f, // render

			// Device work must be easy to distinguish from CPU recording.
			0.22f, // GPU

			0.14f, // ECS
			0.86f, // physics
			0.72f, // simulation
			0.30f, // script
			0.42f, // network
			0.62f, // assets
			0.00f, // idle - returned above, and here so the array lines up
		};
		static_assert(
			std::size(TURN) == static_cast<size_t>(engine::core::ProfileCategory::Count),
			"A ProfileCategory was added without a hue turn in TURN."
		);

		const auto index = static_cast<size_t>(category);
		hue += TURN[index < std::size(TURN) ? index : 0];
		hue -= static_cast<float>(static_cast<int>(hue));

		float red = 0.0f;
		float green = 0.0f;
		float blue = 0.0f;
		ImGui::ColorConvertHSVtoRGB(
			hue, std::max(saturation, 0.45f), std::max(value, 0.70f), red, green, blue
		);

		return IM_COL32(
			static_cast<int>(red * 255.0f),
			static_cast<int>(green * 255.0f),
			static_cast<int>(blue * 255.0f),
			235
		);
	}

	inline const auto &FlameCategoryPalette(unsigned int accent) {
		struct Palette {
			std::array<unsigned int, static_cast<size_t>(engine::core::ProfileCategory::Count)> Colours{};
			unsigned int Accent = 0;
			bool Ready = false;
		};
		static thread_local Palette palette;
		if (!palette.Ready || palette.Accent != accent) {
			for (size_t index = 0; index < palette.Colours.size(); index++)
				palette.Colours[index] =
					UncachedFlameColour(static_cast<engine::core::ProfileCategory>(index), accent);
			palette.Accent = accent;
			palette.Ready = true;
		}
		return palette.Colours;
	}

}
