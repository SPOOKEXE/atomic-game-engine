#include <engine/imagegraphfont/GraphFontInputs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace engine::imagegraphfont {
	namespace {
		using namespace imagegraph;
		using Json = nlohmann::json;
		struct InvalidArtifact {};
		struct ArtifactBudgetExceeded {};
		struct ArtifactReadBudget {
			uint64_t Allowance = 0, NativeRetained = 0;
		};
		void Require(bool valid) {
			if (!valid) throw InvalidArtifact{};
		}
		bool Fail(Diagnostic &d, Status code, const char *message) {
			d = {code, {}, "font_inputs", message};
			return false;
		}
		void Fields(const Json &j, std::initializer_list<std::string_view> fields) {
			Require(j.is_object() && j.size() == fields.size());
			for (auto field : fields)
				Require(j.contains(field));
		}
		std::string Text(const Json &j, size_t maximum = Limits::MaximumTextBytes) {
			Require(j.is_string() && j.get_ref<const std::string &>().size() <= maximum);
			return j.get<std::string>();
		}
		std::filesystem::path Utf8Path(const Json &j) {
			const auto text = Text(j);
			return std::filesystem::path(std::u8string(text.begin(), text.end()));
		}

		uint64_t Unsigned(const Json &j, uint64_t maximum = std::numeric_limits<uint64_t>::max()) {
			Require(j.is_number_integer());
			Require(j.is_number_unsigned() || j.get<int64_t>() >= 0);
			auto value = j.get<uint64_t>();
			Require(value <= maximum);
			return value;
		}
		double Number(const Json &j) {
			Require(j.is_number());
			auto value = j.get<double>();
			Require(std::isfinite(value));
			return value;
		}
		bool Boolean(const Json &j) {
			Require(j.is_boolean());
			return j.get<bool>();
		}
		void Array(const Json &j, size_t maximum = Limits::MaximumNodes) {
			Require(j.is_array() && j.size() <= maximum);
		}
		template <class T, class Reader> std::optional<T> Optional(const Json &j, Reader read) {
			if (j.is_null()) return {};
			return read(j);
		}
		template <class T, class Writer> Json OptionalJson(const std::optional<T> &value, Writer write) {
			return value ? write(*value) : Json(nullptr);
		}
		std::string EnumText(const Json &j, std::initializer_list<std::string_view> values) {
			auto text = Text(j);
			Require(std::find(values.begin(), values.end(), text) != values.end());
			return text;
		}
		std::string RasterName(FontRasterProfile raster) {
			switch (raster) {
			case FontRasterProfile::BitmapSurface:
				return "bitmap_surface";
			case FontRasterProfile::NativeGlyphCoverage:
				return "native_glyph_coverage";
			case FontRasterProfile::SourceObserved:
				return "source_observed";
			case FontRasterProfile::NativeSignedDistance:
				return "native_signed_distance";
			}
			throw InvalidArtifact{};
		}
		FontRasterProfile Raster(const Json &j) {
			auto name = EnumText(
				j, {"bitmap_surface", "native_glyph_coverage", "source_observed", "native_signed_distance"}
			);
			if (name == "bitmap_surface") return FontRasterProfile::BitmapSurface;
			if (name == "native_glyph_coverage") return FontRasterProfile::NativeGlyphCoverage;
			if (name == "source_observed") return FontRasterProfile::SourceObserved;
			return FontRasterProfile::NativeSignedDistance;
		}
		std::string CharacterName(FontCharacterProfile characters) {
			switch (characters) {
			case FontCharacterProfile::Utf16:
				return "utf16";
			case FontCharacterProfile::UnicodeScalar:
				return "unicode_scalar";
			}
			throw InvalidArtifact{};
		}
		uint8_t ChangeCase(const Json &j) {
			const auto mode = EnumText(j, {"lowercase", "uppercase", "titlecase"});
			return mode == "lowercase" ? 1 : mode == "uppercase" ? 2 : 3;
		}
		std::string ChangeCaseName(uint8_t mode) {
			switch (mode) {
			case 1:
				return "lowercase";
			case 2:
				return "uppercase";
			case 3:
				return "titlecase";
			}
			throw InvalidArtifact{};
		}

		Image ReadImage(const Json &j) {
			Fields(j, {"width", "height", "format", "pixelsHex", "storedHash"});
			Image image;
			image.Width = uint32_t(Unsigned(j.at("width"), std::numeric_limits<uint32_t>::max()));
			image.Height = uint32_t(Unsigned(j.at("height"), std::numeric_limits<uint32_t>::max()));
			auto name = Text(j.at("format"));
			bool found = false;
			for (auto format :
				 {SurfaceFormat::RGBA8Unorm,
				  SurfaceFormat::RGBA4Unorm,
				  SurfaceFormat::RGBA16Float,
				  SurfaceFormat::RGBA32Float,
				  SurfaceFormat::R8Unorm,
				  SurfaceFormat::R16Float,
				  SurfaceFormat::R32Float}) {
				if (DescribeSurfaceFormat(format)->Name == name) {
					image.Format = format;
					found = true;
					break;
				}
			}
			Require(found);
			image.Hash = Unsigned(j.at("storedHash"));
			const auto &hex = j.at("pixelsHex");
			Require(hex.is_string());
			const auto &text = hex.get_ref<const std::string &>();
			auto layout =
				CheckedSurfaceLayout(image.Width, image.Height, image.Format, Limits::MaximumArrayBytes);
			Require(
				image.Width <= Limits::MaximumDimension && image.Height <= Limits::MaximumDimension &&
				layout && text.size() % 2 == 0 && text.size() / 2 == layout->Bytes
			);
			auto nibble = [](char c) -> uint8_t {
				if (c >= '0' && c <= '9') return uint8_t(c - '0');
				if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
				throw InvalidArtifact{};
			};
			image.Pixels.resize(text.size() / 2);
			for (size_t i = 0; i < image.Pixels.size(); ++i)
				image.Pixels[i] = uint8_t((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
			Require(FiniteSurfaceSamples(image));
			return image;
		}
		Json ImageJson(const Image &image) {
			Require(
				ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) &&
				FiniteSurfaceSamples(image)
			);
			const auto format = DescribeSurfaceFormat(image.Format);
			Require(bool(format));
			static constexpr char hex[] = "0123456789abcdef";
			std::string pixels(image.Pixels.size() * 2, '0');
			for (size_t i = 0; i < image.Pixels.size(); ++i) {
				pixels[i * 2] = hex[image.Pixels[i] >> 4];
				pixels[i * 2 + 1] = hex[image.Pixels[i] & 15];
			}
			return {
				{"width", image.Width},
				{"height", image.Height},
				{"format", format->Name},
				{"pixelsHex", std::move(pixels)},
				{"storedHash", image.Hash}
			};
		}
		FontMeasurement Measurement(const Json &j) {
			Fields(j, {"text", "maximumLineWidth", "lineGap", "width", "height"});
			return {
				Text(j.at("text")),
				Number(j.at("maximumLineWidth")),
				Number(j.at("lineGap")),
				Number(j.at("width")),
				Number(j.at("height"))
			};
		}
		Json MeasurementJson(const FontMeasurement &m) {
			return {
				{"text", m.Text},
				{"maximumLineWidth", m.MaximumLineWidth},
				{"lineGap", m.LineGap},
				{"width", m.Width},
				{"height", m.Height}
			};
		}
		std::vector<FontMeasurement> Measurements(const Json &j) {
			Array(j);
			std::vector<FontMeasurement> values;
			values.reserve(j.size());
			for (const auto &entry : j)
				values.push_back(Measurement(entry));
			return values;
		}
		Json MeasurementsJson(const std::vector<FontMeasurement> &measurements) {
			Json result = Json::array();
			for (const auto &m : measurements)
				result.push_back(MeasurementJson(m));
			return result;
		}
		FontValue Font(const Json &j) {
			Fields(
				j,
				{"raster",
				 "distanceSpread",
				 "characters",
				 "glyphMapComplete",
				 "frames",
				 "glyphs",
				 "measurements",
				 "lineHeight",
				 "missingAdvance",
				 "spaceAdvance",
				 "firstCharacter",
				 "lastCharacter",
				 "hasCharacterRange",
				 "sourceTexture",
				 "identity"}
			);
			FontValue value;
			auto &font = value.Data.emplace();
			font.Raster = Raster(j.at("raster"));
			font.DistanceSpread = uint8_t(Unsigned(j.at("distanceSpread"), 255));
			font.Characters = EnumText(j.at("characters"), {"utf16", "unicode_scalar"}) == "utf16"
								  ? FontCharacterProfile::Utf16
								  : FontCharacterProfile::UnicodeScalar;
			font.GlyphMapComplete = Boolean(j.at("glyphMapComplete"));
			const auto &frames = j.at("frames");
			Array(frames);
			font.Frames.reserve(frames.size());
			for (const auto &frame : frames)
				font.Frames.push_back(ReadImage(frame));
			const auto &glyphs = j.at("glyphs");
			Array(glyphs);
			font.Glyphs.reserve(glyphs.size());
			for (const auto &entry : glyphs) {
				Fields(
					entry,
					{"character",
					 "present",
					 "frame",
					 "advance",
					 "width",
					 "height",
					 "offset",
					 "textureRectangle",
					 "distancePaddingPixels"}
				);
				FontGlyph glyph;
				glyph.Character =
					uint32_t(Unsigned(entry.at("character"), std::numeric_limits<uint32_t>::max()));
				glyph.Present = Boolean(entry.at("present"));
				glyph.Frame = Optional<uint32_t>(entry.at("frame"), [](const Json &v) {
					return uint32_t(Unsigned(v, std::numeric_limits<uint32_t>::max()));
				});
				glyph.Advance = Number(entry.at("advance"));
				glyph.Width = Number(entry.at("width"));
				glyph.Height = Number(entry.at("height"));
				const auto &offset = entry.at("offset");
				Array(offset, 2);
				Require(offset.size() == 2);
				glyph.Offset = {Number(offset[0]), Number(offset[1])};
				glyph.TextureRectangle = Optional<Vector4>(entry.at("textureRectangle"), [](const Json &v) {
					Array(v, 4);
					Require(v.size() == 4);
					return Vector4{Number(v[0]), Number(v[1]), Number(v[2]), Number(v[3])};
				});
				glyph.DistancePaddingPixels = uint8_t(Unsigned(entry.at("distancePaddingPixels"), 255));
				font.Glyphs.push_back(glyph);
			}
			font.Measurements = Measurements(j.at("measurements"));
			font.LineHeight = Number(j.at("lineHeight"));
			font.MissingAdvance = Number(j.at("missingAdvance"));
			font.SpaceAdvance = Number(j.at("spaceAdvance"));
			font.FirstCharacter =
				uint32_t(Unsigned(j.at("firstCharacter"), std::numeric_limits<uint32_t>::max()));
			font.LastCharacter =
				uint32_t(Unsigned(j.at("lastCharacter"), std::numeric_limits<uint32_t>::max()));
			font.HasCharacterRange = Boolean(j.at("hasCharacterRange"));
			font.SourceTexture = Optional<Image>(j.at("sourceTexture"), ReadImage);
			font.Identity = Text(j.at("identity"));
			return value;
		}
		Json FontJson(const FontValue &value) {
			Require(bool(value.Data));
			const auto &font = *value.Data;
			Json frames = Json::array(), glyphs = Json::array();
			for (const auto &frame : font.Frames)
				frames.push_back(ImageJson(frame));
			for (const auto &g : font.Glyphs)
				glyphs.push_back(
					{{"character", g.Character},
					 {"present", g.Present},
					 {"frame", OptionalJson(g.Frame, [](uint32_t n) { return Json(n); })},
					 {"advance", g.Advance},
					 {"width", g.Width},
					 {"height", g.Height},
					 {"offset", Json::array({g.Offset.X, g.Offset.Y})},
					 {"textureRectangle",
					  OptionalJson(
						  g.TextureRectangle,
						  [](const Vector4 &v) { return Json::array({v.X, v.Y, v.Z, v.W}); }
					  )},
					 {"distancePaddingPixels", g.DistancePaddingPixels}}
				);
			return {
				{"raster", RasterName(font.Raster)},
				{"distanceSpread", font.DistanceSpread},
				{"characters", CharacterName(font.Characters)},
				{"glyphMapComplete", font.GlyphMapComplete},
				{"frames", std::move(frames)},
				{"glyphs", std::move(glyphs)},
				{"measurements", MeasurementsJson(font.Measurements)},
				{"lineHeight", font.LineHeight},
				{"missingAdvance", font.MissingAdvance},
				{"spaceAdvance", font.SpaceAdvance},
				{"firstCharacter", font.FirstCharacter},
				{"lastCharacter", font.LastCharacter},
				{"hasCharacterRange", font.HasCharacterRange},
				{"sourceTexture", OptionalJson(font.SourceTexture, ImageJson)},
				{"identity", font.Identity}
			};
		}
		SourceFontContext Context(const Json &j) {
			Fields(
				j,
				{"textCaseProfile",
				 "bitmapTextureProfile",
				 "aliasMapKnown",
				 "aliases",
				 "directory",
				 "applicationLocation",
				 "projectPath",
				 "defaultFontPath",
				 "initialFont",
				 "initialTextFonts",
				 "playing",
				 "textTransforms"}
			);
			SourceFontContext context;
			context.TextCaseProfile =
				EnumText(j.at("textCaseProfile"), {"recorded_only", "unicode_default"}) == "recorded_only"
					? FontTextCaseProfile::RecordedOnly
					: FontTextCaseProfile::UnicodeDefault;
			context.BitmapTextureProfile =
				EnumText(j.at("bitmapTextureProfile"), {"source_observed", "native_frame_uv"}) ==
						"source_observed"
					? FontBitmapTextureProfile::SourceObserved
					: FontBitmapTextureProfile::NativeFrameUv;
			context.AliasMapKnown = Boolean(j.at("aliasMapKnown"));
			const auto &aliases = j.at("aliases");
			Array(aliases);
			context.Aliases.reserve(aliases.size());
			for (const auto &entry : aliases) {
				Fields(entry, {"name", "path"});
				context.Aliases.emplace_back(Text(entry.at("name")), Text(entry.at("path")));
			}
			context.Directory =
				Optional<std::string>(j.at("directory"), [](const Json &v) { return Text(v); });
			context.ApplicationLocation =
				Optional<std::string>(j.at("applicationLocation"), [](const Json &v) { return Text(v); });
			context.ProjectPath =
				Optional<std::string>(j.at("projectPath"), [](const Json &v) { return Text(v); });
			context.DefaultFontPath =
				Optional<std::string>(j.at("defaultFontPath"), [](const Json &v) { return Text(v); });
			context.InitialFont = Optional<FontValue>(j.at("initialFont"), Font);
			const auto &initialText = j.at("initialTextFonts");
			Array(initialText);
			context.InitialTextFonts.reserve(initialText.size());
			for (const auto &entry : initialText) {
				Fields(entry, {"nodeId", "primary", "fallback"});
				context.InitialTextFonts.push_back(
					{Text(entry.at("nodeId")),
					 Optional<FontValue>(entry.at("primary"), Font),
					 Optional<FontValue>(entry.at("fallback"), Font)}
				);
			}

			context.Playing = Optional<bool>(j.at("playing"), Boolean);
			const auto &transforms = j.at("textTransforms");
			Array(transforms);
			context.TextTransforms.reserve(transforms.size());
			for (const auto &entry : transforms) {
				Fields(entry, {"original", "transformed", "changeCase"});
				context.TextTransforms.push_back(
					{Text(entry.at("original")),
					 Text(entry.at("transformed")),
					 ChangeCase(entry.at("changeCase"))}
				);
			}
			return context;
		}
		Json ContextJson(const SourceFontContext &context) {
			Require(
				context.TextCaseProfile == FontTextCaseProfile::RecordedOnly ||
				context.TextCaseProfile == FontTextCaseProfile::UnicodeDefault
			);
			Require(
				context.BitmapTextureProfile == FontBitmapTextureProfile::SourceObserved ||
				context.BitmapTextureProfile == FontBitmapTextureProfile::NativeFrameUv
			);
			Json aliases = Json::array(), transforms = Json::array(), initialText = Json::array();
			for (const auto &[name, path] : context.Aliases)
				aliases.push_back({{"name", name}, {"path", path}});
			for (const auto &t : context.TextTransforms)
				transforms.push_back(
					{{"original", t.Original},
					 {"transformed", t.Transformed},
					 {"changeCase", ChangeCaseName(t.ChangeCase)}}
				);

			for (const auto &entry : context.InitialTextFonts) {
				initialText.push_back(
					{{"nodeId", entry.NodeId},
					 {"primary", OptionalJson(entry.Primary, FontJson)},
					 {"fallback", OptionalJson(entry.Fallback, FontJson)}}
				);
			}
			auto stringJson = [](const std::string &s) { return Json(s); };
			return {
				{"textCaseProfile",
				 context.TextCaseProfile == FontTextCaseProfile::RecordedOnly ? "recorded_only"
																			  : "unicode_default"},
				{"bitmapTextureProfile",
				 context.BitmapTextureProfile == FontBitmapTextureProfile::SourceObserved
					 ? "source_observed"
					 : "native_frame_uv"},
				{"aliasMapKnown", context.AliasMapKnown},
				{"aliases", std::move(aliases)},
				{"directory", OptionalJson(context.Directory, stringJson)},
				{"applicationLocation", OptionalJson(context.ApplicationLocation, stringJson)},
				{"projectPath", OptionalJson(context.ProjectPath, stringJson)},
				{"defaultFontPath", OptionalJson(context.DefaultFontPath, stringJson)},
				{"initialFont", OptionalJson(context.InitialFont, FontJson)},
				{"initialTextFonts", std::move(initialText)},
				{"playing", OptionalJson(context.Playing, [](bool v) { return Json(v); })},
				{"textTransforms", std::move(transforms)}
			};
		}
		Node Authored(const Json &j, ArtifactReadBudget &budget) {
			Require(j.is_string());
			const auto &text = j.get_ref<const std::string &>();
			Require(text.size() <= Limits::MaximumDocumentBytes);
			Require(budget.NativeRetained <= budget.Allowance);
			Document document;
			Diagnostic diagnostic;
			// The entire residual operation allowance belongs exclusively to native parsing.
			// Already admitted artifact input, DOM and other typed candidates remain live outside it.
			const auto status =
				Read(std::string_view(text), document, diagnostic, budget.Allowance - budget.NativeRetained);
			if (status == Status::LimitExceeded) throw ArtifactBudgetExceeded{};
			Require(status == Status::Ok && document.Nodes.size() == 1);
			Document singleton;
			singleton.FormatVersion = 9;
			singleton.Nodes.swap(document.Nodes);
			Document empty;
			empty.FormatVersion = 9;
			Require(document == empty);
			const auto retained = DocumentRetainedPayloadBytes(singleton);
			Require(bool(retained));
			if (*retained > budget.Allowance - budget.NativeRetained) throw ArtifactBudgetExceeded{};
			budget.NativeRetained += *retained;
			return std::move(singleton.Nodes.front());
		}
		std::string AuthoredJson(const Node &node) {
			Document document;
			document.FormatVersion = 9;
			document.Nodes.push_back(node);
			const auto retained = DocumentRetainedPayloadBytes(document);
			Require(bool(retained));
			auto text = Write(document);
			// Generated counted records reproduce an already bounded node. The writer's structural
			// admission reserves this verification slice separately from pixel expansion.
			ArtifactReadBudget budget{*retained * 32, 0};
			Require(Authored(Json(text), budget) == node);
			return text;
		}

		SourceFontRequest Request(const Json &j, ArtifactReadBudget &budget) {
			Fields(
				j,
				{"authored",
				 "context",
				 "processorRow",
				 "tick",
				 "subframe",
				 "negativeFrame",
				 "role",
				 "resolvedPath",
				 "fontInput",
				 "pixelSize",
				 "antialias",
				 "signedDistanceField",
				 "characters",
				 "measurements"}
			);
			SourceFontRequest request;
			request.Authored = Authored(j.at("authored"), budget);
			request.Context = Context(j.at("context"));
			request.ProcessorRow = size_t(Unsigned(j.at("processorRow"), std::numeric_limits<size_t>::max()));
			request.Tick = Unsigned(j.at("tick"));
			request.Subframe = Number(j.at("subframe"));
			request.NegativeFrame = Boolean(j.at("negativeFrame"));
			request.Role = Text(j.at("role"));
			request.ResolvedPath = Text(j.at("resolvedPath"));
			request.FontInput = Optional<FontValue>(j.at("fontInput"), Font);
			request.PixelSize = uint32_t(Unsigned(j.at("pixelSize"), std::numeric_limits<uint32_t>::max()));
			request.Antialias = Boolean(j.at("antialias"));
			request.SignedDistanceField = Boolean(j.at("signedDistanceField"));
			const auto &characters = j.at("characters");
			Array(characters);
			request.Characters.reserve(characters.size());
			for (const auto &c : characters)
				request.Characters.push_back(uint32_t(Unsigned(c, std::numeric_limits<uint32_t>::max())));
			request.Measurements = Measurements(j.at("measurements"));
			return request;
		}
		Json RequestJson(const SourceFontRequest &request) {
			return {
				{"authored", AuthoredJson(request.Authored)},
				{"context", ContextJson(request.Context)},
				{"processorRow", request.ProcessorRow},
				{"tick", request.Tick},
				{"subframe", request.Subframe},
				{"negativeFrame", request.NegativeFrame},
				{"role", request.Role},
				{"resolvedPath", request.ResolvedPath},
				{"fontInput", OptionalJson(request.FontInput, FontJson)},
				{"pixelSize", request.PixelSize},
				{"antialias", request.Antialias},
				{"signedDistanceField", request.SignedDistanceField},
				{"characters", request.Characters},
				{"measurements", MeasurementsJson(request.Measurements)}
			};
		}
		GraphFontConfiguration Configuration(const Json &j, ArtifactReadBudget &budget) {
			Fields(j, {"schema", "context", "observations", "readGrants"});
			Require(Text(j.at("schema")) == "atomic.font_inputs.v1");
			GraphFontConfiguration result;
			result.Context = Context(j.at("context"));
			const auto &observations = j.at("observations");
			Array(observations);
			result.Observations.reserve(observations.size());
			for (const auto &entry : observations) {
				Fields(entry, {"request", "presence", "font"});
				SourceFontObservation observation;
				observation.Request = Request(entry.at("request"), budget);
				observation.Presence = EnumText(entry.at("presence"), {"present", "absent_file"}) == "present"
										   ? SourceFontPresence::Present
										   : SourceFontPresence::AbsentFile;
				observation.Font = Optional<FontValue>(entry.at("font"), Font);
				result.Observations.push_back(std::move(observation));
			}
			const auto &grants = j.at("readGrants");
			Array(grants);
			result.ReadGrants.reserve(grants.size());
			for (const auto &entry : grants) {
				Fields(entry, {"node", "file", "resource"});
				result.ReadGrants.push_back(
					{Text(entry.at("node")), Utf8Path(entry.at("file")), false, Text(entry.at("resource"))}
				);
			}
			return result;
		}
		Json ConfigurationJson(const GraphFontConfiguration &configuration) {
			Json observations = Json::array(), grants = Json::array();
			for (const auto &o : configuration.Observations) {
				Require(
					o.Presence == SourceFontPresence::Present || o.Presence == SourceFontPresence::AbsentFile
				);
				observations.push_back(
					{{"request", RequestJson(o.Request)},
					 {"presence", o.Presence == SourceFontPresence::Present ? "present" : "absent_file"},
					 {"font", OptionalJson(o.Font, FontJson)}}
				);
			}
			for (const auto &g : configuration.ReadGrants) {
				Require(!g.Write);
				auto path = g.File.generic_u8string();
				grants.push_back(
					{{"node", g.NodeId},
					 {"file", std::string(path.begin(), path.end())},
					 {"resource", g.Resource}}
				);
			}
			return {
				{"schema", "atomic.font_inputs.v1"},
				{"context", ContextJson(configuration.Context)},
				{"observations", std::move(observations)},
				{"readGrants", std::move(grants)}
			};
		}
		// SAX admits DOM strings, decoded candidate slots and key storage before DOM construction.
		// Parser token scratch is independently bounded by twice the borrowed input. Hex strings
		// include decoded pixel storage; native authored values use a separate residual byte budget.
		struct Preflight final : nlohmann::json_sax<Json> {
			enum class StringKind { Ordinary, PixelHex, Authored };
			struct Level {
				bool Object = false;
				StringKind Kind = StringKind::Ordinary;
				std::set<std::string> Keys;
			};
			std::array<Level, 64> Levels{};
			size_t Depth = 0, Nodes = 0;
			uint64_t Cost = 0, Maximum = 0, Scratch = 0, Keys = 0;
			bool BudgetExceeded = false;
			explicit Preflight(uint64_t maximum, uint64_t scratch) : Maximum(maximum), Scratch(scratch) {}
			bool Charge(uint64_t count) {
				if (count > Maximum - Cost) {
					BudgetExceeded = true;
					return false;
				}
				Cost += count;
				return true;
			}
			bool Atom(uint64_t size = 0, uint64_t scale = 4) {
				if (++Nodes > 1048576) return false;
				return Charge(256) && size <= Limits::MaximumDocumentBytes && Charge(size * scale);
			}
			bool null() override {
				return Atom();
			}
			bool boolean(bool) override {
				return Atom();
			}
			bool number_integer(number_integer_t) override {
				return Atom();
			}
			bool number_unsigned(number_unsigned_t) override {
				return Atom();
			}
			bool number_float(number_float_t value, const string_t &) override {
				return std::isfinite(value) && Atom();
			}
			bool string(string_t &value) override {
				const auto kind =
					Depth && Levels[Depth - 1].Object ? Levels[Depth - 1].Kind : StringKind::Ordinary;
				if (kind == StringKind::Authored) return Atom(value.size(), 2);
				return Atom(value.size(), kind == StringKind::PixelHex ? 3 : 4);
			}
			bool binary(binary_t &) override {
				return false;
			}
			bool start_object(std::size_t) override {
				if (Depth == Levels.size() || !Atom()) return false;
				Levels[Depth].Object = true;
				Levels[Depth++].Keys.clear();
				return true;
			}
			bool key(string_t &value) override {
				if (!Depth || !Levels[Depth - 1].Object || !Atom(value.size(), 6)) return false;
				const auto keyCost = 256 + value.size() * 2;
				if (Scratch > Maximum || Keys > Maximum - Scratch || keyCost > Maximum - Scratch - Keys) {
					BudgetExceeded = true;
					return false;
				}
				Keys += keyCost;
				Levels[Depth - 1].Kind = value == "authored"	? StringKind::Authored
										 : value == "pixelsHex" ? StringKind::PixelHex
																: StringKind::Ordinary;
				return Levels[Depth - 1].Keys.insert(value).second;
			}
			bool end_object() override {
				if (!Depth || !Levels[Depth - 1].Object) return false;
				Levels[--Depth].Keys.clear();
				return true;
			}
			bool start_array(std::size_t) override {
				if (Depth == Levels.size() || !Atom()) return false;
				Levels[Depth].Object = false;
				Levels[Depth++].Keys.clear();
				return true;
			}
			bool end_array() override {
				if (!Depth || Levels[Depth - 1].Object) return false;
				--Depth;
				return true;
			}
			bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
				return false;
			}
		};

		uint64_t PixelCapacities(const GraphFontConfiguration &configuration) {
			uint64_t bytes = 0;
			const auto addFont = [&](const std::optional<FontValue> &font) {
				if (!font) return;
				for (const auto &image : font->Data->Frames)
					bytes += image.Pixels.capacity();
				if (font->Data->SourceTexture) bytes += font->Data->SourceTexture->Pixels.capacity();
			};
			const auto addContext = [&](const SourceFontContext &context) {
				addFont(context.InitialFont);
				for (const auto &entry : context.InitialTextFonts) {
					addFont(entry.Primary);
					addFont(entry.Fallback);
				}
			};
			addContext(configuration.Context);
			for (const auto &observation : configuration.Observations) {
				addContext(observation.Request.Context);
				addFont(observation.Request.FontInput);
				addFont(observation.Font);
			}
			return bytes;
		}

		bool
		Semantics(const GraphFontConfiguration &configuration, uint64_t maximum, Diagnostic &diagnostic) {
			if (!GraphFontConfigurationRetainedBytes(configuration))
				return Fail(diagnostic, Status::InvalidValue, "font artifact payload is malformed");
			return ValidateSourceFontObservations(
					   &configuration.Context, configuration.Observations, maximum, diagnostic
				   ) == Status::Ok;
		}
	}
	bool ReadGraphFontConfiguration(
		std::string_view bytes,
		GraphFontConfiguration &destination,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	) try {
		const uint64_t maximum = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto old = GraphFontConfigurationRetainedBytes(destination);
		if (!old || *old > maximum || bytes.size() > Limits::MaximumDocumentBytes ||
			bytes.size() > (maximum - *old) / 3)
			return Fail(diagnostic, Status::LimitExceeded, "font artifact input exceeds coexistence budget");
		const auto available = maximum - *old - bytes.size();
		Preflight scan(available, bytes.size() * 2);
		if (!Json::sax_parse(bytes.begin(), bytes.end(), &scan))
			return Fail(
				diagnostic,
				scan.BudgetExceeded ? Status::LimitExceeded : Status::InvalidValue,
				"font artifact JSON is malformed or exceeds its bound"
			);
		const auto dom = Json::parse(bytes.begin(), bytes.end());
		ArtifactReadBudget nativeBudget{available - scan.Cost, 0};
		auto candidate = Configuration(dom, nativeBudget);
		const auto owned = GraphFontConfigurationRetainedBytes(candidate);
		if (!owned) return Fail(diagnostic, Status::InvalidValue, "font artifact payload is malformed");
		if (*owned > available || !Semantics(candidate, available, diagnostic))
			return *owned > available ? Fail(
											diagnostic,
											Status::LimitExceeded,
											"font artifact retained capacities exceed bound"
										)
									  : false;
		destination = std::move(candidate);
		diagnostic = {};
		return true;
	} catch (const ArtifactBudgetExceeded &) {
		return Fail(
			diagnostic, Status::LimitExceeded, "native font node parsing exceeds its operation allowance"
		);
	} catch (const InvalidArtifact &) {
		return Fail(diagnostic, Status::InvalidValue, "font artifact fields or payload are malformed");
	} catch (const nlohmann::json::exception &) {
		return Fail(diagnostic, Status::InvalidValue, "font artifact JSON is malformed");
	} catch (...) {
		return Fail(diagnostic, Status::LimitExceeded, "font artifact allocation failed");
	}
	bool WriteGraphFontConfiguration(
		const GraphFontConfiguration &configuration,
		std::string &destination,
		uint64_t maximumBytes,
		Diagnostic &diagnostic
	) try {
		const uint64_t maximum = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto owned = GraphFontConfigurationRetainedBytes(configuration);
		if (!owned) return Fail(diagnostic, Status::InvalidValue, "font artifact payload is malformed");
		const auto pixelBytes = PixelCapacities(configuration);
		Require(pixelBytes <= *owned);
		const auto structuralBytes = *owned - pixelBytes;
		if (destination.capacity() > maximum || structuralBytes > (maximum - destination.capacity()) / 64 ||
			pixelBytes > (maximum - destination.capacity() - structuralBytes * 64) / 12)
			return Fail(
				diagnostic, Status::LimitExceeded, "font artifact encoding exceeds coexistence budget"
			);
		if (!Semantics(configuration, maximum, diagnostic)) return false;
		const auto dom = ConfigurationJson(configuration);
		auto candidate = dom.dump(-1, ' ', false, Json::error_handler_t::strict);
		if (candidate.size() > Limits::MaximumDocumentBytes ||
			candidate.capacity() > maximum - destination.capacity() - *owned)
			return Fail(diagnostic, Status::LimitExceeded, "font artifact encoded bytes exceed bound");
		destination = std::move(candidate);
		diagnostic = {};
		return true;
	} catch (const ArtifactBudgetExceeded &) {
		return Fail(
			diagnostic, Status::LimitExceeded, "native font node parsing exceeds its operation allowance"
		);
	} catch (const InvalidArtifact &) {
		return Fail(diagnostic, Status::InvalidValue, "font artifact fields or payload are malformed");
	} catch (const nlohmann::json::exception &) {
		return Fail(diagnostic, Status::InvalidValue, "font artifact JSON is malformed");
	} catch (...) {
		return Fail(diagnostic, Status::LimitExceeded, "font artifact allocation failed");
	}
}
