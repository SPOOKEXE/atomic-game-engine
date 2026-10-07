#pragma once

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
			Bokeh32,
			FoldGreyscale,
			FoldMap,
			GaussianRandom,
			GaussianConversion,
			AnisoBlend,
			AnisoMapped,
			RasterRGB
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
			if (IsNoise()) {
				BuildNoise();
			} else if (IsPalette()) {
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
		bool IsNoise() const {
			return Workload >= Kind::FoldGreyscale;
		}
		bool IsFieldSample() const {
			return Workload == Kind::RasterRGB;
		}
		uint32_t OutputSide() const {
			return IsFieldSample() ? 32 : IsNoise() ? 64 : Side;
		}
		const char *OutputFormat() const {
			return IsFieldSample() ? "Vector3"
				   : IsPalette()   ? "ColourArray"
				   : IsNoise()	   ? "RGBA32Float"
								   : "RGBA8Unorm";
		}
		void BuildNoise() {
			const bool fold = Workload == Kind::FoldGreyscale || Workload == Kind::FoldMap || IsFieldSample();
			const bool gaussian = Workload == Kind::GaussianRandom || Workload == Kind::GaussianConversion;
			Node generator{
				"generator",
				fold	   ? "pc.fold_noise"
				: gaussian ? "pc.noise_gaussian"
						   : "pc.noise_aniso",
				"",
				{},
				{}
			};
			generator.Values = {
				{"dimension", Vector2{double(OutputSide()), double(OutputSide())}},
				{"dimension_unit", EnumValue{0}},
				{"attribute_color_depth", EnumValue{5}}
			};
			if (fold) {
				generator.Values.insert(
					generator.Values.end(),
					{{"position", Vector2{.13, -.19}},
					 {"position_unit", EnumValue{0}},
					 {"scale", Vector2{.7, .4}},
					 {"iteration", int64_t{3}},
					 {"stretch", 1.7},
					 {"amplitude", 1.2},
					 {"detail", Vector2{2.3, .8}},
					 {"mode", EnumValue{Workload == Kind::FoldGreyscale ? 0 : 1}},
					 {"rotation", 23.0},
					 {"level_in", Vector2{.1, 1.2}},
					 {"level_out", Vector2{-.2, 1.3}}}
				);
			} else if (gaussian) {
				generator.Values.insert(
					generator.Values.end(),
					{{"seed", 17.25},
					 {"position", Vector2{.13, -.19}},
					 {"scale", Vector2{.7, .4}},
					 {"rotation", 23.0},
					 {"mean", .5},
					 {"varience", .5},
					 {"use_conversion", Workload == Kind::GaussianConversion}}
				);
			} else {
				const bool mapped = Workload == Kind::AnisoMapped;
				generator.Values.insert(
					generator.Values.end(),
					{{"seed", 17.25},
					 {"seed_2", 71.5},
					 {"position", Vector2{.13, -.19}},
					 {"position_unit", EnumValue{0}},
					 {"render_mode", EnumValue{mapped ? 1 : 0}},
					 {"tile", mapped},
					 {"x_amount_mapped", mapped},
					 {"y_amount_mapped", mapped},
					 {"rotation_mapped", mapped}}
				);
				if (mapped)
					// grug default primary controls select the authored synthetic map ranges.
					generator.Values.insert(
						generator.Values.end(),
						{{"x_amount_map_range", Vector2{2, 4}},
						 {"y_amount_map_range", Vector2{4, 8}},
						 {"rotation_map_range", Vector2{0, 90}}}
					);
				else
					generator.Values.insert(
						generator.Values.end(), {{"x_amount", 2.0}, {"y_amount", 8.0}, {"rotation", 0.0}}
					);
			}
			if (IsFieldSample()) generator.Values.push_back({"output_type", EnumValue{3}});
			Authored.Nodes.push_back(std::move(generator));
			if (Workload == Kind::GaussianConversion || Workload == Kind::AnisoMapped) {
				Authored.Nodes.push_back(
					{"map",
					 "pc.solid",
					 "",
					 {},
					 {{"dimension", Vector2{double(OutputSide()), double(OutputSide())}},
					  {"dimension_unit", EnumValue{0}},
					  {"color", Colour{128, 128, 128, 255}}}}
				);
				if (Workload == Kind::GaussianConversion)
					for (const auto port : {"conv_surf_1", "conv_surf_2"})
						Authored.Links.push_back({"map", "surface_out", "generator", port});
				else
					for (const auto port : {"x_amount_map", "y_amount_map", "rotation_map"})
						Authored.Links.push_back({"map", "surface_out", "generator", port});
			}
			if (IsFieldSample()) {
				Authored.Nodes.push_back(
					{"sample",
					 "value.sample_noise",
					 "",
					 {},
					 {{"output_type", EnumValue{3}}, {"position", Vector2{.25, .75}}}}
				);
				Authored.Links.push_back({"generator", "field", "sample", "field"});
				Authored.Outputs = {{"out", "sample", "value"}};
			} else
				Authored.Outputs = {{"out", "generator", "surface_out"}};
		}
		static float Fraction(float value) {
			return value - std::floor(value);
		}
		static float Blend(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		static float AnisoRandom(float x, float y, float seed) {
			const auto endpoint = [&](float integerSeed) {
				const float shifted = integerSeed + 453.456f;
				const float remainder = shifted - 100.f * std::floor(shifted / 100.f);
				return Fraction(
					std::sin((x * 12.9898f + y * 78.233f) * remainder * 12.588f) * 43758.5453123f
				);
			};
			return Blend(endpoint(std::floor(seed)), endpoint(std::floor(seed) + 1.f), Fraction(seed));
		}
		// grug source equations form an oracle, never a captured engine image.
		SurfacePixel NoiseReference(uint32_t x, uint32_t y) const {
			const float side = float(OutputSide()), u = (float(x) + .5f) / side, v = (float(y) + .5f) / side;
			const bool fold = Workload == Kind::FoldGreyscale || Workload == Kind::FoldMap || IsFieldSample();
			if (fold) {
				const float angle = 23.f * .017453292519943295f, cosine = std::cos(angle),
							sine = std::sin(angle);
				const float dx = u - .13f / side, dy = v - (-.19f) / side;
				float px = (dx * cosine - dy * sine) * .7f, py = (dx * sine + dy * cosine) * .4f;
				for (int iteration = 0; iteration < 3; ++iteration) {
					const float oldX = px, oldY = py;
					px += std::cos(oldY * 2.3f) / 3.f;
					py += std::cos(oldX * 2.3f + 1.7f) / 3.f;
					const float cosineX = px, cosineY = py;
					px = (cosineX + std::sin(cosineY * .8f + 1.7f) / 2.f) * 1.2f;
					py = (cosineY + std::sin(cosineX * .8f) / 2.f) * 1.2f;
				}
				const float fx = px - 2.f * std::floor(px / 2.f) - 1.f,
							fy = py - 2.f * std::floor(py / 2.f) - 1.f;
				const float length = std::sqrt(fx * fx + fy * fy);
				SurfacePixel expected = Workload == Kind::FoldGreyscale
											? SurfacePixel{length, length, length, 1.f + length}
											: SurfacePixel{std::abs(fx), std::abs(fy), 0, 1};
				for (size_t channel = 0; channel < 3; ++channel)
					expected[channel] = Blend(-.2f, 1.3f, (float(expected[channel]) - .1f) / (1.2f - .1f));
				return expected;
			}
			if (Workload == Kind::GaussianRandom || Workload == Kind::GaussianConversion) {
				const float angle = 23.f * .017453292519943295f, cosine = std::cos(angle),
							sine = std::sin(angle);
				const float px = (u * cosine - v * sine) * .7f - .13f,
							py = (u * sine + v * cosine) * .4f - (-.19f);
				const auto random = [&](float ox, float oy) {
					const float modulo = 17.25f - 100000.f * std::floor(17.25f / 100000.f);
					return Fraction(
						std::sin((px + ox) * 78.233f + (py + oy) * 128.852f) * (43758.5453f + modulo / 10.f)
					);
				};
				const float first = Workload == Kind::GaussianConversion
										? float(128. / 255.)
										: std::max(random(3.9613f, 1.6452f), .001f);
				const float second =
					Workload == Kind::GaussianConversion ? float(128. / 255.) : random(.1654f, 2.9873f);
				const float value = .5f + std::sqrt(-2.f * std::log(first)) *
											  std::cos(2.f * 3.14159265358979323846f * second) * .5f;
				return {value, value, value, 1};
			}
			const bool mapped = Workload == Kind::AnisoMapped;
			const float sample = float(128. / 255.), brightness = (sample + sample + sample) / 3.f;
			const float nx = mapped ? Blend(2, 4, brightness) : 2, ny = mapped ? Blend(4, 8, brightness) : 8;
			const float angle = (mapped ? Blend(0, 90, brightness) : 0) * .017453292519943295f;
			const float dx = u - .13f / side, dy = v - (-.19f) / side;
			const float px = dx * std::cos(angle) - dy * std::sin(angle),
						py = dx * std::sin(angle) + dy * std::cos(angle);
			const float row = std::floor(py * ny), absRow = std::abs(row);
			const float yy = (absRow - 289.653f * std::floor(absRow / 289.653f)) * (row < 0	  ? -1.f
																					: row > 0 ? 1.f
																							  : 0.f);
			float xx = (px + AnisoRandom(1, yy, 17.25f)) * nx;
			if (mapped) xx = Fraction(Fraction(xx / 2.f) + 1.f) * 2.f;
			const float value = mapped ? Fraction(xx)
									   : Blend(
											 AnisoRandom(std::floor(xx), yy, 71.5f),
											 AnisoRandom(std::floor(xx) + 1.f, yy, 71.5f),
											 Fraction(xx)
										 );
			return {value, value, value, 1};
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
			if (IsPalette() || IsFieldSample())
				Check(EvaluateValue(Authored, Compiled, "out", {}, Colours, diagnostic), diagnostic);
			else
				Check(Evaluate(Authored, Compiled, "out", {}, Pixels, diagnostic), diagnostic);
		}
		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ULL; };
			if (IsNoise()) {
				if (IsFieldSample()) {
					const auto *sample = std::get_if<Vector3>(&Colours.Data);
					if (!sample) Fail("raster RGB sample shape");
					const auto expected = NoiseReference(OutputSide() / 4, OutputSide() * 3 / 4);
					const std::array actual{sample->X, sample->Y, sample->Z};
					for (size_t channel = 0; channel < 3; ++channel) {
						if (!std::isfinite(actual[channel]) ||
							std::abs(actual[channel] - expected[channel]) > 1e-5)
							Fail("raster RGB source equation");
						const auto word = std::bit_cast<uint64_t>(actual[channel]);
						for (unsigned shift = 0; shift < 64; shift += 8)
							byte(uint8_t(word >> shift));
					}
				} else {
					if (Pixels.Width != OutputSide() || Pixels.Height != OutputSide() ||
						Pixels.Format != SurfaceFormat::RGBA32Float ||
						Pixels.Pixels.size() != size_t(OutputSide()) * OutputSide() * 16)
						Fail("noise output dimensions/format");
					for (uint32_t y = 0; y < OutputSide(); ++y)
						for (uint32_t x = 0; x < OutputSide(); ++x) {
							SurfacePixel actual{};
							if (!LoadSurfacePixel(Pixels, x, y, actual)) Fail("noise pixel read");
							const auto expected = NoiseReference(x, y);
							for (size_t channel = 0; channel < 4; ++channel)
								if (!std::isfinite(actual[channel]) ||
									std::abs(actual[channel] - expected[channel]) > 1e-5)
									Fail("noise source equation");
						}
					for (auto value : Pixels.Pixels)
						byte(value);
					if (Pixels.Hash != SurfaceHash(Pixels)) Fail("noise canonical surface hash");
				}
			} else if (IsPalette()) {
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
			if (IsFieldSample()) {
				const auto previous = Colours;
				auto rejected = Authored;
				rejected.Nodes[0].Values[0].Data = Vector2{128, 128};
				Plan rejectedPlan;
				Check(Compile(rejected, rejectedPlan, diagnostic), diagnostic);
				if (EvaluateValue(rejected, rejectedPlan, "out", {}, Colours, diagnostic) !=
						Status::LimitExceeded ||
					Colours.Data != previous.Data)
					Fail("raster RGB failed budget changed output");
			} else if (IsPalette()) {
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
