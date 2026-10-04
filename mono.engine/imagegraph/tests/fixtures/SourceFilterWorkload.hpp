#pragma once
#include "FilterLiteralPixels.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <stdexcept>
namespace engine::imagegraph::testing {
	// Four persisted processor rows execute real kernels; every byte has an independent source-equation
	// oracle.
	struct SourceFilterWorkload {
		static constexpr size_t Rows = 4, Side = 16;
		size_t Variant = 0;
		Document Authored;
		Plan Compiled;
		Diagnostic Failure;
		std::array<RequestImageSource, 1> Sources;
		ImageArray Output;
		uint64_t InputHash = 14695981039346656037ULL;
		explicit SourceFilterWorkload(size_t variant) : Variant(variant) {
			if (variant >= FilterLiteralPixels.size()) Fail("variant");
			const bool kuwahara = variant < 3;
			const int64_t mode = int64_t(kuwahara ? variant : (variant - 3) % 3);
			Authored.FormatVersion = 9;
			Authored.Nodes = {
				{"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
				{"radii", "pc.array", "", {}, {}},
				{"filter",
				 kuwahara ? "pc.kuwahara" : "pc.blobify",
				 "",
				 {},
				 {{kuwahara ? "types" : "shape", EnumValue{mode}},
				  {"interpolate", EnumValue{1}},
				  {"oversample", EnumValue{3}},
				  {"attribute_color_depth", EnumValue{3}}}}
			};
			for (size_t row = 0; row < Rows; ++row)
				Authored.Nodes[1].DynamicInputs.push_back(
					{"input_" + std::to_string(row), ValueType::Integer, int64_t{2}}
				);
			if (kuwahara) {
				if (mode == 2) Authored.Nodes[2].Values.push_back({"zero_crossing", 2.5});
			} else {
				Authored.Nodes[2].Values.insert(
					Authored.Nodes[2].Values.end(),
					{{"distance", variant >= 6}, {"threshold", .25}, {"smoothness", .5}}
				);
			}
			Authored.Links = {
				{"capture", "image", "filter", "surface_in"}, {"radii", "array", "filter", "radius"}
			};
			Authored.Outputs = {{"image", "filter", "surface_out"}};
			const auto serialized = Write(Authored);
			Document restored;
			if (Read(serialized, restored, Failure) != Status::Ok || restored != Authored)
				Fail("native roundtrip");
			Authored = std::move(restored);
			if (Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
			Image image{Side, Side, std::vector<uint8_t>(Side * Side * 4), 0};
			for (size_t y = 0; y < Side; ++y)
				for (size_t x = 0; x < Side; ++x) {
					const size_t i = (y % 4) * 4 + x % 4, p = (y * Side + x) * 4;
					image.Pixels[p] = uint8_t(i * 13);
					image.Pixels[p + 1] = uint8_t((15 - i) * 13);
					image.Pixels[p + 2] = uint8_t((i % 4) * 60);
					image.Pixels[p + 3] = 255;
				}
			image.Hash = SurfaceHash(image);
			Sources[0] = {"source", std::move(image)};
			for (unsigned char byte : serialized)
				Hash(byte);
			for (uint8_t byte : Sources[0].Data.Pixels)
				Hash(byte);
			EvaluationRequest request;
			request.ImageSources = Sources;
			EvaluationSnapshot snapshot;
			if (EvaluateNodeInputs(Authored, Compiled, "filter", request, snapshot, Failure) != Status::Ok)
				Fail("controls");
			bool radius = false, source = false;
			for (const auto &value : snapshot.Values())
				if (value.Port == "radius") {
					const auto *a = std::get_if<ArrayValue>(&value.Data);
					if (!a || a->Elements.size() != Rows || !value.Linked) Fail("original radius rows");
					for (const auto &leaf : a->Elements) {
						const auto *n = std::get_if<int64_t>(&leaf);
						if (!n || *n != 2) Fail("literal radius control");
					}
					radius = true;
				}
			for (const auto &input : snapshot.Images())
				if (input.Port == "surface_in") {
					if (input.Data != Sources[0].Data) Fail("original captured pixels");
					source = true;
				}
			if (!radius || !source) Fail("complete resolved controls");
		}
		[[noreturn]] void Fail(std::string_view phase) const {
			throw std::runtime_error(
				"source filter workload " + std::to_string(Variant) + " " + std::string(phase) + ": " +
				Failure.NodeId + ":" + Failure.Port + ":" + Failure.Message
			);
		}
		void Hash(uint8_t byte) {
			InputHash ^= byte;
			InputHash *= 1099511628211ULL;
		}
		Status TryEvaluate(uint32_t maximumDimension = Limits::MaximumDimension) {
			EvaluationRequest request;
			request.ImageSources = Sources;
			request.MaximumImageDimension = maximumDimension;
			return EvaluateArray(Authored, Compiled, "image", request, Output, Failure);
		}
		void Evaluate() {
			if (TryEvaluate() != Status::Ok) Fail("evaluate");
		}
		uint64_t Verify() const {
			if (Output.Images.size() != Rows || Output.Items.size() != Rows) Fail("outer rows");
			uint64_t hash = 14695981039346656037ULL;
			for (size_t row = 0; row < Rows; ++row) {
				const auto &image = Output.Images[row];
				const auto *index = std::get_if<size_t>(&Output.Items[row].Data);
				if (!index || *index != row) Fail("row index");
				if (image.Width != Side || image.Height != Side ||
					image.Format != SurfaceFormat::RGBA8Unorm || image.Pixels.size() != Side * Side * 4 ||
					image.Hash != SurfaceHash(image))
					Fail("dimensions format hash");
				for (size_t byte = 0; byte < image.Pixels.size(); ++byte) {
					if (image.Pixels[byte] != FilterLiteralPixels[Variant][byte])
						Fail("independent literal pixels");
					hash ^= image.Pixels[byte];
					hash *= 1099511628211ULL;
				}
			}
			return hash;
		}
	};
}
