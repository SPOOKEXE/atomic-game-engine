#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace engine::imagegraph::testing {
	// Owned one-pixel bitmap coverage gives a literal oracle independent of layout implementation.
	struct FontTextWorkload {
		static constexpr size_t Rows = 8;
		Document Authored;
		Plan Compiled;
		SourceFontContext Context;
		std::array<RequestImageSource, 1> Sources;
		ImageArray Output;
		Diagnostic Failure;
		uint64_t InputHash = 14695981039346656037ULL;

		FontTextWorkload() {
			Authored.FormatVersion = 9;
			Authored.Nodes = {
				{"source", "image.captured", "", {}, {{"source_id", std::string{"glyphs"}}}},
				{"bitmap", "pc.font_bitmap", "", {}, {{"string_map", std::string{"A"}}, {"separation", 0.0}}},
				{"font", "pc.font_data", "", {}, {}},
				{"strings", "pc.array", "", {}, {{"type", EnumValue{4}}}},
				{"text", "pc.text", "", {}, {{"interpolate", EnumValue{1}}, {"oversample", EnumValue{3}}}}
			};
			for (size_t row = 0; row < Rows; ++row)
				Authored.Nodes[3].DynamicInputs.push_back(
					{"input_" + std::to_string(row), ValueType::Text, std::string(Glyphs(row), 'A')}
				);
			Authored.Links = {
				{"source", "image", "bitmap", "font_surfaces"},
				{"bitmap", "font", "font", "font"},
				{"font", "font", "text", "font"},
				{"strings", "array", "text", "text"}
			};
			Authored.Outputs = {{"image", "text", "surface_out"}};
			const auto serialized = Write(Authored);
			Document restored;
			if (Read(serialized, restored, Failure) != Status::Ok || restored != Authored)
				Fail("native roundtrip");
			Authored = std::move(restored);
			if (Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
			Context.AliasMapKnown = true;
			Context.DefaultFontPath = std::string{};
			Context.Playing = false;
			Sources[0] = {"glyphs", {1, 1, {255, 255, 255, 255}}};
			Sources[0].Data.Hash = SurfaceHash(Sources[0].Data);
			for (const unsigned char byte : serialized)
				Hash(byte);
			for (const auto byte : Sources[0].Data.Pixels)
				Hash(byte);
		}
		static constexpr size_t Glyphs(size_t row) {
			return 512 + 64 * row;
		}
		void Hash(uint8_t byte) {
			InputHash ^= byte;
			InputHash *= 1099511628211ULL;
		}
		[[noreturn]] void Fail(std::string_view phase) const {
			throw std::runtime_error(
				"Font/Text workload " + std::string(phase) + ": " + Failure.NodeId + ':' + Failure.Port +
				':' + Failure.Message
			);
		}
		Status TryEvaluate(uint32_t maximumDimension = Limits::MaximumDimension) {
			EvaluationRequest request;
			request.SourceFonts = &Context;
			request.ImageSources = Sources;
			request.MaximumImageDimension = maximumDimension;
			return EvaluateArray(Authored, Compiled, "image", request, Output, Failure);
		}
		void Evaluate() {
			if (TryEvaluate() != Status::Ok) Fail("evaluate");
		}
		uint64_t Verify() const {
			if (Output.Images.size() != Rows || Output.Items.size() != Rows) Fail("outer shape");
			uint64_t hash = 14695981039346656037ULL;
			for (size_t row = 0; row < Rows; ++row) {
				const auto &image = Output.Images[row];
				const auto *index = std::get_if<size_t>(&Output.Items[row].Data);
				if (!index || *index != row) Fail("flat row index");
				if (image.Width != Glyphs(row) || image.Height != 1 ||
					image.Format != SurfaceFormat::RGBA8Unorm || image.Pixels.size() != 4 * Glyphs(row) ||
					image.Hash != SurfaceHash(image))
					Fail("row dimensions/format/hash");
				for (const auto byte : image.Pixels) {
					if (byte != 255) Fail("literal glyph coverage");
					hash ^= byte;
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}
	};
}
