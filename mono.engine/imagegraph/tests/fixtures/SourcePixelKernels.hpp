#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace engine::imagegraph::testing {
	// Constant filter fields give an analytical oracle while exercising every kernel tap.
	// Atlas tiles and shuffled palettes exercise ordered, nonuniform data independently.
	struct SourcePixelKernelFixture {
		enum class Kind {
			Atlas16,
			Atlas64,
			PaletteRGB,
			PaletteReverse,
			EdgeSobel,
			EdgeLaplacian,
			Bokeh8,
			Bokeh32
		};
		Kind Workload;
		Document Authored;
		Plan Compiled;
		Image Pixels;
		EvaluatedValue Colours;
		uint64_t ExpectedHash = 0;
		static constexpr uint32_t Side = 128;
		explicit SourcePixelKernelFixture(Kind kind) : Workload(kind) {
			Authored.FormatVersion = 9;
			if (IsPalette()) {
				ArrayValue palette{ValueType::Colour, {}};
				for (size_t i = 0; i < 512; ++i) {
					const auto level = uint8_t(((i * 137) % 512) / 2);
					palette.Elements.emplace_back(Colour{level, level, level, 255});
				}
				Authored.Nodes = {
					{"sort",
					 "pc.palette_sort",
					 "",
					 {},
					 {{"palette_in", palette},
					  {"order", EnumValue{kind == Kind::PaletteRGB ? 6 : 10}},
					  {"sort_order", std::string{"r"}}}}
				};
				Authored.Outputs = {{"out", "sort", "sorted_palette"}};
			} else if (IsAtlas()) {
				const size_t across = kind == Kind::Atlas16 ? 4 : 8;
				const uint32_t tileSide = Side / uint32_t(across);
				ArrayValue tiles{ValueType::Atlas, {}};
				for (size_t i = 0; i < across * across; ++i) {
					AtlasValue tile;
					auto &data = tile.Data.emplace();
					data.Kind = AtlasKind::SurfaceAtlas;
					data.Position = {double((i % across) * tileSide), double((i / across) * tileSide)};
					data.Scale = {1, 1};
					data.Dimension = {double(tileSide), double(tileSide)};
					data.Alpha = 1;
					data.Surface.Data = Solid(tileSide, TileColour(i));
					tiles.Elements.emplace_back(std::move(tile));
				}
				Authored.Junctions = {{"tiles", "", ValueType::Array, tiles}};
				Authored.Nodes = {
					{"draw",
					 "pc.atlas_draw",
					 "",
					 {},
					 {{"use_base_dimension", false},
					  {"dimension", Vector2{Side, Side}},
					  {"dimension_unit", EnumValue{0}},
					  {"interpolate", EnumValue{1}}}}
				};
				Authored.Links = {{"tiles", "value", "draw", "input_1"}};
				Authored.Outputs = {{"out", "draw", "surface"}};
			} else {
				Authored.Nodes = {
					{"source",
					 "pc.solid",
					 "",
					 {},
					 {{"dimension", Vector2{Side, Side}},
					  {"dimension_unit", EnumValue{0}},
					  {"color", Colour{64, 128, 192, 128}}}}
				};
				Node filter{"filter", IsEdge() ? "pc.edge_detect" : "pc.blur_bokeh", "", {}, {}};
				if (IsEdge())
					filter.Values = {{"algorithm", EnumValue{kind == Kind::EdgeSobel ? 0 : 2}}};
				else
					filter.Values = {{"iteration", int64_t{kind == Kind::Bokeh8 ? 8 : 32}}, {"strength", 8.}};
				Authored.Nodes.push_back(std::move(filter));
				Authored.Links = {{"source", "surface_out", "filter", "surface_in"}};
				Authored.Outputs = {{"out", "filter", "surface_out"}};
			}
			Diagnostic diagnostic;
			Check(Compile(Authored, Compiled, diagnostic), diagnostic);
			Run();
			ExpectedHash = Verify();
			Run();
			if (Verify() != ExpectedHash) Fail("preflight output changed");
			CheckBudget();
		}
		bool IsPalette() const {
			return Workload == Kind::PaletteRGB || Workload == Kind::PaletteReverse;
		}
		bool IsAtlas() const {
			return Workload == Kind::Atlas16 || Workload == Kind::Atlas64;
		}
		bool IsEdge() const {
			return Workload == Kind::EdgeSobel || Workload == Kind::EdgeLaplacian;
		}
		static Colour TileColour(size_t tile) {
			return {uint8_t(tile * 3), uint8_t(255 - tile * 3), 64, 255};
		}
		static Image Solid(uint32_t side, Colour colour) {
			Image image{side, side, std::vector<uint8_t>(size_t(side) * side * 4)};
			for (size_t offset = 0; offset < image.Pixels.size(); offset += 4) {
				image.Pixels[offset] = colour.Red;
				image.Pixels[offset + 1] = colour.Green;
				image.Pixels[offset + 2] = colour.Blue;
				image.Pixels[offset + 3] = colour.Alpha;
			}
			image.Hash = SurfaceHash(image);
			return image;
		}
		[[noreturn]] static void Fail(std::string_view reason) {
			throw std::runtime_error(std::string(reason));
		}
		static void Check(Status status, const Diagnostic &diagnostic) {
			if (status != Status::Ok)
				Fail(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		}
		void Run() {
			Diagnostic diagnostic;
			if (IsPalette())
				Check(EvaluateValue(Authored, Compiled, "out", {}, Colours, diagnostic), diagnostic);
			else
				Check(Evaluate(Authored, Compiled, "out", {}, Pixels, diagnostic), diagnostic);
		}
		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
			if (IsPalette()) {
				const auto *palette = std::get_if<ArrayValue>(&Colours.Data);
				if (!palette || palette->ElementType != ValueType::Colour ||
					palette->Elements.size() != 512 || !palette->Nested.empty() || !palette->Items.empty())
					Fail("palette shape");
				for (size_t i = 0; i < 512; ++i) {
					const auto level = uint8_t(Workload == Kind::PaletteRGB ? 255 - i / 2 : i / 2);
					const auto &colour = std::get<Colour>(palette->Elements[i]);
					if (colour != Colour{level, level, level, 255})
						Fail("palette ordered analytical colours");
					for (auto value : {colour.Red, colour.Green, colour.Blue, colour.Alpha})
						byte(value);
				}
			} else {
				if (Pixels.Width != Side || Pixels.Height != Side ||
					Pixels.Format != SurfaceFormat::RGBA8Unorm ||
					Pixels.Pixels.size() != size_t(Side) * Side * 4)
					Fail("kernel output dimensions/format");
				for (uint32_t y = 0; y < Side; ++y)
					for (uint32_t x = 0; x < Side; ++x) {
						Colour expected = IsEdge() ? Colour{0, 0, 0, 128} : Colour{32, 64, 96, 128};
						if (IsAtlas()) {
							const uint32_t across = Workload == Kind::Atlas16 ? 4 : 8;
							expected = TileColour((y / (Side / across)) * across + x / (Side / across));
						}
						const std::array<uint8_t, 4> channels{
							expected.Red, expected.Green, expected.Blue, expected.Alpha
						};
						for (size_t channel = 0; channel < 4; ++channel) {
							const auto actual = Pixels.Pixels[(size_t(y) * Side + x) * 4 + channel];
							if (actual != channels[channel]) Fail("kernel analytical pixel");
							byte(actual);
						}
					}
				if (Pixels.Hash != SurfaceHash(Pixels)) Fail("canonical surface hash");
			}
			return hash;
		}
		void CheckBudget() {
			Diagnostic diagnostic;
			if (IsPalette()) {
				const auto previous = Colours;
				auto rejected = Authored;
				rejected.Nodes[0].Values[1].Data = EnumValue{10};
				rejected.Nodes[0].Values[2].Data = std::string(128, 'R');
				Plan rejectedPlan;
				Check(Compile(rejected, rejectedPlan, diagnostic), diagnostic);
				if (EvaluateValue(rejected, rejectedPlan, "out", {}, Colours, diagnostic) !=
						Status::LimitExceeded ||
					Colours.Data != previous.Data)
					Fail("palette failed budget changed output");
			} else {
				const auto previous = Pixels;
				if (Evaluate(Authored, Compiled, "out", {}, Pixels, diagnostic, 1) != Status::LimitExceeded ||
					Pixels != previous)
					Fail("kernel failed budget changed output");
			}
		}
	};
}
