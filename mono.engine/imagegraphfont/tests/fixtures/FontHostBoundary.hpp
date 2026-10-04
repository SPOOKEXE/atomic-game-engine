#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace engine::imagegraphfont::testing {
	using namespace imagegraph;
	inline constexpr std::string_view BoundaryBitmapFont = R"(STARTFONT 2.1
FONT -engine-test-medium-r-normal--10-100-72-72-c-80-iso10646-1
SIZE 10 72 72
FONTBOUNDINGBOX 5 7 -1 -2
STARTPROPERTIES 4
FONT_ASCENT 8
FONT_DESCENT 2
CHARSET_REGISTRY "ISO10646"
CHARSET_ENCODING "1"
ENDPROPERTIES
CHARS 2
STARTCHAR A
ENCODING 65
SWIDTH 800 0
DWIDTH 8 0
BBX 5 7 -1 -2
BITMAP
70
88
88
F8
88
88
88
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 400 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
ENDFONT
)";
	inline constexpr std::array<uint8_t, 7> BoundaryBitmapRows{0x70, 0x88, 0x88, 0xf8, 0x88, 0x88, 0x88};
	inline uint64_t BoundaryHash(std::string_view value) {
		uint64_t hash = 14695981039346656037ULL;
		for (const unsigned char byte : value) {
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash;
	}
	struct BoundaryFontFile {
		std::filesystem::path Directory, Path;
		BoundaryFontFile() {
			Directory = std::filesystem::temp_directory_path() /
						("engine-font-boundary-" +
						 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			if (!std::filesystem::create_directory(Directory))
				throw std::runtime_error("font fixture directory");
			Path = Directory / "immutable.bdf";
			std::ofstream stream(Path, std::ios::binary);
			stream.write(BoundaryBitmapFont.data(), BoundaryBitmapFont.size());
			stream.close();
			if (!stream.good()) {
				std::error_code error;
				std::filesystem::remove_all(Directory, error);
				throw std::runtime_error("immutable BDF fixture publication");
			}
		}
		~BoundaryFontFile() {
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	};
	struct BoundaryTrace final : SourceFontProvider {
		SourceFontProvider *Target = nullptr;
		std::array<uint32_t, 4> Characters{};
		size_t Count = 0;
		bool Observe(
			const SourceFontRequest &request,
			uint64_t cap,
			SourceFontObservation &output,
			std::string &failure
		) override {
			if (!Target || request.Characters.size() > Characters.size()) {
				failure = "fixture character bound";
				return false;
			}
			if (!Target->Observe(request, cap, output, failure)) return false;
			Count = request.Characters.size();
			std::copy(request.Characters.begin(), request.Characters.end(), Characters.begin());
			return true;
		}
	};
	struct FontHostBoundary {
		enum class Operation { Coverage, Distance, UnicodeText, OutlineCoverage, OutlineDistance };
		static constexpr size_t UnicodeRepetitions = 128;
		Operation Kind;
		BoundaryFontFile File;
		std::filesystem::path SelectedPath;
		uint64_t SelectedFileBytes = BoundaryBitmapFont.size();
		GraphFontInputs Owner;
		GraphFontConfiguration Configuration;
		assets::ContentPolicy Policy;
		SourceFontContext Held;
		EvaluationRequest Evaluation;
		SourceFontRequest Request;
		SourceFontObservation Observation;
		BoundaryTrace Trace;
		Document Graph;
		Plan Compiled;
		Image Output;
		Diagnostic DiagnosticValue;
		std::string Failure;
		uint64_t InputHash = 0;
		explicit FontHostBoundary(Operation kind) : Kind(kind) {
			SelectedPath = File.Path;
			if (Outline()) {
				auto root = std::filesystem::path(__FILE__).lexically_normal();
				for (size_t parent = 0; parent < 5; ++parent)
					root = root.parent_path();
				SelectedPath = root / "mono.vendor/fonts/Inter.ttf";
				std::ifstream stream(SelectedPath, std::ios::binary | std::ios::ate);
				if (!stream || stream.tellg() != std::streamoff{876576}) Fail("pinned outline font size");
				std::string bytes(876576, '\0');
				stream.seekg(0);
				stream.read(bytes.data(), bytes.size());
				if (!stream || BoundaryHash(bytes) != 14893761530522866524ULL)
					Fail("pinned outline font content");
				SelectedFileBytes = bytes.size();
			}
			Configuration.Context.AliasMapKnown = true;
			Configuration.Context.DefaultFontPath = std::string{};
			Configuration.Context.Playing = false;
			Configuration.Context.TextCaseProfile = FontTextCaseProfile::UnicodeDefault;
			Configuration.ReadGrants = {{"text", SelectedPath, false, "font"}};
			Policy.Allow(assets::FormOfName(SelectedPath.string()), true);
			if (!Owner.Replace(Configuration, Policy, Limits::MaximumEvaluationBytes, DiagnosticValue))
				Fail("owner");
			Graph.FormatVersion = 9;
			std::string text;
			for (size_t i = 0; i < UnicodeRepetitions; ++i)
				text += "a\xc3\x9f";
			Graph.Nodes = {
				{"text",
				 "pc.text",
				 "",
				 {},
				 {{"text", text},
				  {"font", SelectedPath.string()},
				  {"size", int64_t{Outline() ? 64 : 10}},
				  {"change_case", EnumValue{2}},
				  {"interpolate", EnumValue{1}},
				  {"oversample", EnumValue{3}}}}
			};
			Graph.Outputs = {{"out", "text", "surface_out"}};
			const auto saved = Write(Graph);
			Document restored;
			if (Read(saved, restored, DiagnosticValue) != Status::Ok || restored != Graph) Fail("roundtrip");
			Graph = std::move(restored);
			if (Compile(Graph, Compiled, DiagnosticValue) != Status::Ok) Fail("compile");
			Request.Authored = Graph.Nodes[0];
			Request.Context = Configuration.Context;
			Request.ResolvedPath = SelectedPath.string();
			Request.Role = "font";
			Request.PixelSize = Outline() ? 64 : 10;
			Request.Characters =
				Outline() ? std::vector<uint32_t>{32, 87, 105} : std::vector<uint32_t>{32, 65, 66};
			Request.SignedDistanceField = Distance();
			// Temporary path is an exact capability, not part of the content-defined input fingerprint.
			InputHash = (Outline() ? 14893761530522866524ULL : BoundaryHash(BoundaryBitmapFont)) ^
						BoundaryHash(text) ^ uint64_t(kind);
		}
		bool Outline() const {
			return Kind == Operation::OutlineCoverage || Kind == Operation::OutlineDistance;
		}
		bool Distance() const {
			return Kind == Operation::Distance || Kind == Operation::OutlineDistance;
		}
		[[noreturn]] void Fail(std::string_view phase) const {
			throw std::runtime_error(
				"font boundary " + std::string(phase) + ":" + Failure + DiagnosticValue.Message
			);
		}
		bool Run(uint64_t maximum = Limits::MaximumEvaluationBytes) {
			if (!Owner.Bind(false, Held, Evaluation, maximum, DiagnosticValue)) return false;
			if (Kind == Operation::UnicodeText) {
				Trace.Target = Evaluation.FontProvider;
				Evaluation.FontProvider = &Trace;
				return Evaluate(Graph, Compiled, "out", Evaluation, Output, DiagnosticValue, maximum) ==
					   Status::Ok;
			}
			return Evaluation.FontProvider->Observe(Request, maximum, Observation, Failure);
		}

		void VerifyCounters(const std::vector<core::Counter> &counters) const {
			const auto value = [&](std::string_view name) {
				for (const auto &counter : counters)
					if (counter.Name.Text() == name) return counter.Value;
				Fail("missing actual byte/operation counter");
			};
			if (value("imagegraphfont.font.input_bytes") != SelectedFileBytes ||
				value("imagegraphfont.font.converted_glyphs") != (Outline() ? (Distance() ? 2 : 3) : 1))
				Fail("file read/conversion counts");
			if (value("imagegraphfont.font.converted_payload_bytes") <= 0) Fail("converted pixels counter");
			if (Kind == Operation::UnicodeText &&
				(value("imagegraph.font.case_input_bytes") != 3 * UnicodeRepetitions ||
				 value("imagegraph.font.case_output_bytes") != 3 * UnicodeRepetitions))
				Fail("Unicode byte counters");
		}
		uint64_t VerifyOutline() const {
			const auto &font = *Observation.Font->Data;
			const bool distance = Distance();
			if (font.Characters != FontCharacterProfile::UnicodeScalar || font.Glyphs.size() != 3 ||
				font.Frames.size() != (distance ? 2u : 3u) ||
				font.Raster != (distance ? FontRasterProfile::NativeSignedDistance
										 : FontRasterProfile::NativeGlyphCoverage) ||
				font.DistanceSpread != (distance ? 8 : 0)) {
				std::string details = "outline profile/shape: characters=" +
									  std::to_string(static_cast<unsigned>(font.Characters)) +
									  " raster=" + std::to_string(static_cast<unsigned>(font.Raster)) +
									  " spread=" + std::to_string(font.DistanceSpread) +
									  " glyphs=" + std::to_string(font.Glyphs.size()) +
									  " frames=" + std::to_string(font.Frames.size());
				for (const auto &glyph : font.Glyphs) {
					details += " glyph=" + std::to_string(glyph.Character) +
							   ":present=" + std::to_string(glyph.Present) +
							   ":frame=" + (glyph.Frame ? std::to_string(*glyph.Frame) : "none") +
							   ":advance=" + std::to_string(glyph.Advance) +
							   ":padding=" + std::to_string(glyph.DistancePaddingPixels);
				}
				for (const auto &frame : font.Frames)
					details += " frame=" + std::to_string(frame.Width) + "x" + std::to_string(frame.Height);
				Fail(details);
			}
			constexpr std::array<uint32_t, 3> points{32, 87, 105};
			constexpr std::array<double, 3> rawAdvances{18, 63.0625, 15.5};
			constexpr std::array<double, 3> roundedAdvances{18, 63, 16};
			for (size_t index = 0; index < 3; ++index) {
				const auto &glyph = font.Glyphs[index];
				if (glyph.Character != points[index] || !glyph.Present ||
					glyph.Advance != (distance ? rawAdvances[index] : roundedAdvances[index]))
					Fail("SFNT outline advance");
				if (!index) {
					if (glyph.DistancePaddingPixels ||
						(distance ? glyph.Frame.has_value() : glyph.Frame != 0u))
						Fail("outline space bitmap");
				} else if (!glyph.Frame || *glyph.Frame != index - (distance ? 1 : 0) ||
						   glyph.DistancePaddingPixels != (distance ? 8 : 0))
					Fail("outline glyph frame index/padding");
			}
			uint64_t hash = 14695981039346656037ULL;
			for (const auto &frame : font.Frames) {
				if (!frame.Width || !frame.Height || frame.Width > 128 || frame.Height > 128 ||
					frame.Format != SurfaceFormat::RGBA8Unorm ||
					frame.Pixels.size() != size_t(frame.Width) * frame.Height * 4)
					Fail("bounded outline frame layout");
				// MONO rounds a collapsed outline box to one pixel per axis. The
				// empty space outline succeeds without raster writes, so its
				// real frame is exactly white RGB with zero coverage.
				const bool blankSpace = !distance && &frame == &font.Frames.front();
				if (blankSpace && (frame.Width != 1 || frame.Height != 1 || frame.Pixels[0] != 255 ||
								   frame.Pixels[1] != 255 || frame.Pixels[2] != 255 || frame.Pixels[3] != 0))
					Fail("literal blank monochrome space frame");
				bool inside = false, outside = false;
				for (size_t pixel = 0; pixel < size_t(frame.Width) * frame.Height; ++pixel) {
					for (size_t channel = 0; channel < 3; ++channel)
						if (frame.Pixels[pixel * 4 + channel] != 255) Fail("outline RGB");
					const auto alpha = frame.Pixels[pixel * 4 + 3];
					inside |= alpha > 128;
					outside |= alpha < 128;
					hash ^= alpha;
					hash *= 1099511628211ULL;
				}
				if (blankSpace ? (inside || !outside) : (!inside || !outside))
					Fail("outline stroke/background signs");
				if (distance && frame.Pixels[3] >= 128) Fail("outside padded outline corner");
			}
			if (!SourceFontObservationRetainedBytes(Observation)) Fail("outline retained payload");
			return hash;
		}
		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto append = [&](uint8_t value) {
				hash ^= value;
				hash *= 1099511628211ULL;
			};
			if (Kind == Operation::UnicodeText) {
				// The source getter requests the raw and transformed character sets together.
				if (Trace.Count != 4 || Trace.Characters != std::array<uint32_t, 4>{65, 83, 97, 223})
					Fail("Unicode raw/cased character set");
				if (Output.Width != 8 * UnicodeRepetitions || Output.Height != 10 ||
					Output.Pixels.size() != 8 * UnicodeRepetitions * 10 * 4 ||
					Output.Format != SurfaceFormat::RGBA8Unorm)
					Fail("Unicode Text dimensions");
				for (size_t y = 0; y < 10; ++y)
					for (size_t x = 0; x < Output.Width; ++x) {
						const size_t glyph = (x + 1) / 8, column = (x + 1) % 8;
						const uint8_t expected = y >= 3 && column < 5 && glyph < UnicodeRepetitions &&
														 (BoundaryBitmapRows[y - 3] & (0x80 >> column))
													 ? 255
													 : 0;
						for (size_t channel = 0; channel < 4; ++channel) {
							const auto value = Output.Pixels[(y * Output.Width + x) * 4 + channel];
							if (value != expected) Fail("Unicode literal BDF pixels");
							append(value);
						}
					}
				if (Output.Hash != SurfaceHash(Output)) Fail("Unicode surface hash");
				return hash;
			}
			if (Observation.Request != Request || Observation.Presence != SourceFontPresence::Present ||
				!Observation.Font || !Observation.Font->Data)
				Fail("owned observation");
			const auto &font = *Observation.Font->Data;
			const bool distance = Distance();
			if (Outline()) return VerifyOutline();
			if (font.Raster != (distance ? FontRasterProfile::NativeSignedDistance
										 : FontRasterProfile::NativeGlyphCoverage) ||
				font.Characters != FontCharacterProfile::UnicodeScalar || font.LineHeight != 10 ||
				font.Glyphs.size() != 3 || font.Frames.size() != 1 || font.GlyphMapComplete ||
				font.DistanceSpread != (distance ? 8 : 0))
				Fail("font metrics/profile");
			const auto &space = font.Glyphs[0], &a = font.Glyphs[1], &missing = font.Glyphs[2];
			if (space.Character != 32 || !space.Present || space.Frame || space.Advance != 4 ||
				a.Character != 65 || !a.Present || !a.Frame || *a.Frame != 0 || a.Advance != 8 ||
				missing.Character != 66 || missing.Present || missing.Frame || missing.Advance != 0 ||
				a.Offset != (distance ? Vector2{-9, -5} : Vector2{-1, 3}) ||
				a.DistancePaddingPixels != (distance ? 8 : 0))
				Fail("glyph metrics");
			const auto &frame = font.Frames[0];
			if (frame.Width != (distance ? 21u : 5u) || frame.Height != (distance ? 23u : 7u) ||
				frame.Pixels.size() != size_t(frame.Width) * frame.Height * 4)
				Fail("frame dimensions");
			for (size_t pixel = 0; pixel < size_t(frame.Width) * frame.Height; ++pixel) {
				for (size_t channel = 0; channel < 3; ++channel)
					if (frame.Pixels[pixel * 4 + channel] != 255) Fail("white glyph RGB");
				const auto alpha = frame.Pixels[pixel * 4 + 3];
				if (!distance && alpha != ((BoundaryBitmapRows[pixel / 5] & (0x80 >> (pixel % 5))) ? 255 : 0))
					Fail("literal BDF coverage");
				append(alpha);
			}
			if (distance) {
				// The symmetric crossbar center has zero Sobel gradient, so the pinned
				// bsdf approximation returns zero distance. Its left stroke has
				// nonzero gradient; the hole and padded corner remain outside.
				const auto alpha = [&](size_t x, size_t y) {
					return frame.Pixels[(y * frame.Width + x) * 4 + 3];
				};
				if (alpha(10, 11) != 128 || alpha(8, 11) <= 128 || alpha(10, 10) >= 128 ||
					alpha(0, 0) >= 128) {
					std::string samples = "distance geometry signs: stroke=" + std::to_string(alpha(10, 11)) +
										  " hole=" + std::to_string(alpha(10, 10)) +
										  " padding=" + std::to_string(alpha(0, 0));
					for (size_t y = 8; y < 15; ++y) {
						samples += " row" + std::to_string(y) + "=";
						for (size_t x = 8; x < 13; ++x)
							samples += std::to_string(alpha(x, y)) + ",";
					}
					Fail(samples);
				}
			}
			if (!SourceFontObservationRetainedBytes(Observation)) Fail("retained observation validation");
			return hash;
		}
	};
}
