#include <engine/gui/ShapedText.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <SheenBidi/SheenBidi.h>
#include <algorithm>
#include <cmath>
#include <hb-ft.h>
#include <hb.h>
#include <limits>
#include <linebreak.h>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utf8proc.h>
#include <utility>

namespace engine::gui {

	namespace {
		constexpr size_t MAXIMUM_SHAPED_LINES = 1024;
		struct FreeTypeLibrary {
			FT_Library Value = nullptr;

			FreeTypeLibrary() {
				FT_Init_FreeType(&Value);
			}

			~FreeTypeLibrary() {
				if (Value != nullptr) {
					FT_Done_FreeType(Value);
				}
			}
		};

		FT_Library Library() {
			static FreeTypeLibrary library;
			return library.Value;
		}

		struct OpenFace {
			const FontPackageFace *Definition = nullptr;
			FT_Face Value = nullptr;
		};

		struct FaceSlice {
			size_t Face = 0;
			uint32_t Begin = 0;
			uint32_t End = 0;
		};

		void Close(std::vector<OpenFace> &faces) {
			for (OpenFace &face : faces) {
				if (face.Value != nullptr) {
					FT_Done_Face(face.Value);
				}
			}
		}

		bool Open(const FontPackageFace &definition, FT_Face &out) {
			out = nullptr;
			if (Library() == nullptr || definition.Bytes.empty()) {
				return false;
			}
			return FT_New_Memory_Face(
					   Library(),
					   reinterpret_cast<const FT_Byte *>(definition.Bytes.data()),
					   static_cast<FT_Long>(definition.Bytes.size()),
					   0,
					   &out
				   ) == 0;
		}

		bool Decode(
			std::string_view text,
			std::vector<uint32_t> &codepointOffsets,
			std::vector<utf8proc_int32_t> &codepoints
		) {
			for (size_t offset = 0; offset < text.size();) {
				utf8proc_int32_t codepoint = 0;
				const utf8proc_ssize_t consumed = utf8proc_iterate(
					reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
					static_cast<utf8proc_ssize_t>(text.size() - offset),
					&codepoint
				);
				if (consumed <= 0) {
					return false;
				}
				codepointOffsets.push_back(static_cast<uint32_t>(offset));
				codepoints.push_back(codepoint);
				offset += static_cast<size_t>(consumed);
			}
			return true;
		}

		void Boundaries(
			std::string_view text,
			const std::vector<uint32_t> &offsets,
			const std::vector<utf8proc_int32_t> &codepoints,
			ShapedText &out
		) {
			out.GraphemeBoundaries.push_back(0);
			utf8proc_int32_t state = 0;
			for (size_t index = 1; index < codepoints.size(); index++) {
				if (utf8proc_grapheme_break_stateful(codepoints[index - 1], codepoints[index], &state) != 0) {
					out.GraphemeBoundaries.push_back(offsets[index]);
				}
			}
			out.GraphemeBoundaries.push_back(static_cast<uint32_t>(text.size()));

			std::vector<char> breaks(text.size());
			if (!text.empty()) {
				set_linebreaks_utf8(
					reinterpret_cast<const utf8_t *>(text.data()), text.size(), nullptr, breaks.data()
				);
			}
			out.LineBreakBoundaries.push_back(0);
			for (size_t index = 0; index < breaks.size(); index++) {
				if (breaks[index] == LINEBREAK_ALLOWBREAK || breaks[index] == LINEBREAK_MUSTBREAK) {
					out.LineBreakBoundaries.push_back(static_cast<uint32_t>(index + 1));
				}
			}
			if (out.LineBreakBoundaries.back() != text.size()) {
				out.LineBreakBoundaries.push_back(static_cast<uint32_t>(text.size()));
			}
		}

		std::vector<size_t> FallbackOrder(const std::vector<OpenFace> &faces, FontFace role) {
			std::vector<size_t> order;
			for (size_t index = 0; index < faces.size(); index++) {
				if (faces[index].Definition->Role == role) {
					order.push_back(index);
				}
			}
			for (size_t index = 0; index < faces.size(); index++) {
				if (faces[index].Definition->Role != role) {
					order.push_back(index);
				}
			}
			return order;
		}

		size_t FaceFor(
			const std::vector<OpenFace> &faces, const std::vector<size_t> &order, utf8proc_int32_t codepoint
		) {
			for (const size_t index : order) {
				if (FT_Get_Char_Index(faces[index].Value, static_cast<FT_ULong>(codepoint)) != 0) {
					return index;
				}
			}
			return order.front();
		}

		size_t FaceForGrapheme(
			const std::vector<OpenFace> &faces,
			const std::vector<size_t> &order,
			const std::vector<utf8proc_int32_t> &codepoints,
			size_t begin,
			size_t end
		) {
			for (const size_t face : order) {
				const bool supportsEveryCodepoint = std::all_of(
					codepoints.begin() + static_cast<std::ptrdiff_t>(begin),
					codepoints.begin() + static_cast<std::ptrdiff_t>(end),
					[&faces, face](utf8proc_int32_t codepoint) {
						return FT_Get_Char_Index(faces[face].Value, static_cast<FT_ULong>(codepoint)) != 0;
					}
				);
				if (supportsEveryCodepoint) {
					return face;
				}
			}
			for (const size_t face : order) {
				if (FT_Get_Char_Index(faces[face].Value, 0xFFFD) != 0) {
					return face;
				}
			}
			return FaceFor(faces, order, codepoints[begin]);
		}

		bool ShapeSlice(
			const OpenFace &face,
			const std::vector<uint32_t> &offsets,
			const std::vector<utf8proc_int32_t> &codepoints,
			size_t beginIndex,
			size_t endIndex,
			uint32_t sourceEnd,
			bool rightToLeft,
			ShapedText &out
		) {
			hb_font_t *font = hb_ft_font_create_referenced(face.Value);
			hb_buffer_t *buffer = hb_buffer_create();
			if (font == nullptr || buffer == nullptr) {
				if (buffer != nullptr) {
					hb_buffer_destroy(buffer);
				}
				if (font != nullptr) {
					hb_font_destroy(font);
				}
				return false;
			}

			hb_buffer_set_content_type(buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
			hb_buffer_set_direction(buffer, rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
			for (size_t index = beginIndex; index < endIndex; index++) {
				const utf8proc_int32_t codepoint = codepoints[index];
				const hb_codepoint_t shaped =
					FT_Get_Char_Index(face.Value, static_cast<FT_ULong>(codepoint)) != 0
						? static_cast<hb_codepoint_t>(codepoint)
						: static_cast<hb_codepoint_t>(0xFFFD);
				hb_buffer_add(buffer, shaped, offsets[index]);
			}
			hb_buffer_guess_segment_properties(buffer);
			// Source spans, carets and selection are addressed in UTF-8 boundaries.
			// A discretionary ligature collapses two independently stylable source
			// characters into one cluster, so keep those characters separate until
			// the API can represent an intra-ligature paint split.
			hb_feature_t features[1]{};
			hb_feature_from_string("liga=0", -1, &features[0]);
			hb_shape(font, buffer, features, 1);

			unsigned int count = 0;
			const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
			const hb_glyph_position_t *position = hb_buffer_get_glyph_positions(buffer, &count);
			const uint32_t glyphOffset = static_cast<uint32_t>(out.Glyphs.size());
			float pen = out.Advance;
			for (unsigned int index = 0; index < count; index++) {
				if (out.Glyphs.size() == MAXIMUM_SHAPED_GLYPHS) {
					hb_buffer_destroy(buffer);
					hb_font_destroy(font);
					return false;
				}
				const float advanceX = static_cast<float>(position[index].x_advance) / 64.0f;
				const float advanceY = static_cast<float>(position[index].y_advance) / 64.0f;
				out.Glyphs.push_back(
					ShapedGlyph{
						face.Definition->Name,
						info[index].codepoint,
						info[index].cluster,
						pen + static_cast<float>(position[index].x_offset) / 64.0f,
						-static_cast<float>(position[index].y_offset) / 64.0f,
						advanceX,
						advanceY,
					}
				);
				pen += advanceX;
			}
			out.Advance = pen;
			out.Runs.push_back(
				ShapedRun{
					face.Definition->Name,
					glyphOffset,
					static_cast<uint32_t>(out.Glyphs.size()) - glyphOffset,
					offsets[beginIndex],
					sourceEnd,
					rightToLeft,
				}
			);
			hb_buffer_destroy(buffer);
			hb_font_destroy(font);
			return true;
		}
	}

	bool FontPackage::Add(core::Name name, FontFace role, std::span<const std::byte> bytes) {
		const size_t usedBytes = std::accumulate(
			Entries.begin(), Entries.end(), size_t{0}, [](size_t total, const FontPackageFace &entry) {
				return total + entry.Bytes.size();
			}
		);
		if (!name.IsValid() || bytes.empty() || bytes.size() > MAXIMUM_FONT_PACKAGE_BYTES ||
			Entries.size() == MAXIMUM_FONT_PACKAGE_FACES ||
			bytes.size() > MAXIMUM_FONT_PACKAGE_TOTAL_BYTES - usedBytes || Library() == nullptr) {
			return false;
		}
		if (std::any_of(Entries.begin(), Entries.end(), [name](const FontPackageFace &entry) {
				return entry.Name == name;
			})) {
			return false;
		}

		FontPackageFace candidate;
		candidate.Name = name;
		candidate.Role = role;
		candidate.Bytes.assign(bytes.begin(), bytes.end());
		FT_Face face = nullptr;
		if (!Open(candidate, face)) {
			return false;
		}
		FT_Done_Face(face);
		const auto fold = [this](uint8_t byte) { Fingerprint = (Fingerprint ^ byte) * 1099511628211ull; };
		const auto foldSize = [&fold](size_t size) {
			const uint64_t width = size;
			for (unsigned byte = 0; byte < 8; byte++)
				fold(static_cast<uint8_t>(width >> (byte * 8)));
		};
		foldSize(name.Text().size());
		for (char byte : name.Text())
			fold(static_cast<uint8_t>(byte));
		fold(static_cast<uint8_t>(role));
		foldSize(bytes.size());
		for (std::byte byte : bytes)
			fold(std::to_integer<uint8_t>(byte));
		Entries.push_back(std::move(candidate));
		return true;
	}

	const std::vector<FontPackageFace> &FontPackage::Faces() const {
		return Entries;
	}

	uint64_t FontPackage::Signature() const {
		return Fingerprint;
	}

	ShapedText ShapeParagraph(const FontPackage &package, const TextShapeRequest &request) {
		ShapedText out;
		if (request.Text.size() > MAXIMUM_SHAPED_TEXT_BYTES) {
			out.Status = TextShapeStatus::TooLarge;
			return out;
		}

		std::vector<uint32_t> offsets;
		std::vector<utf8proc_int32_t> codepoints;
		offsets.reserve(request.Text.size());
		codepoints.reserve(request.Text.size());
		if (!Decode(request.Text, offsets, codepoints)) {
			out.Status = TextShapeStatus::InvalidUtf8;
			return out;
		}
		Boundaries(request.Text, offsets, codepoints, out);
		if (package.Faces().empty()) {
			out.Status = TextShapeStatus::EmptyPackage;
			return out;
		}
		if (!std::isfinite(request.PixelSize) || request.PixelSize <= 0.0f ||
			request.PixelSize > MAXIMUM_SHAPED_PIXEL_SIZE) {
			out.Status = TextShapeStatus::NoUsableFont;
			return out;
		}
		if (request.Text.empty()) {
			return out;
		}
		const long pixelHeight = std::lround(request.PixelSize);
		if (pixelHeight <= 0 || pixelHeight > std::numeric_limits<FT_UInt>::max()) {
			out.Status = TextShapeStatus::NoUsableFont;
			return out;
		}

		std::vector<OpenFace> faces;
		faces.reserve(package.Faces().size());
		for (const FontPackageFace &definition : package.Faces()) {
			FT_Face face = nullptr;
			if (!Open(definition, face)) {
				continue;
			}
			if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelHeight)) != 0) {
				FT_Done_Face(face);
				continue;
			}
			faces.push_back(OpenFace{&definition, face});
		}
		if (faces.empty()) {
			out.Status = TextShapeStatus::NoUsableFont;
			return out;
		}

		const std::vector<size_t> order = FallbackOrder(faces, request.Role);
		const SBCodepointSequence sequence{SBStringEncodingUTF8, request.Text.data(), request.Text.size()};
		SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
		if (algorithm == nullptr) {
			Close(faces);
			out.Status = TextShapeStatus::InvalidUtf8;
			return out;
		}

		const SBLevel baseLevel = request.Direction == TextDirection::LeftToRight	? 0
								  : request.Direction == TextDirection::RightToLeft ? 1
																					: SBLevelDefaultLTR;
		SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, 0, request.Text.size(), baseLevel);
		if (paragraph == nullptr) {
			SBAlgorithmRelease(algorithm);
			Close(faces);
			out.Status = TextShapeStatus::InvalidUtf8;
			return out;
		}
		const SBUInteger length = SBParagraphGetLength(paragraph);
		SBLineRef line = SBParagraphCreateLine(paragraph, 0, length);
		bool withinGlyphLimit = true;
		if (line != nullptr) {
			const SBRun *runs = SBLineGetRunsPtr(line);
			const SBUInteger count = SBLineGetRunCount(line);
			for (SBUInteger runIndex = 0; runIndex < count; runIndex++) {
				const uint32_t begin = runs[runIndex].offset;
				const uint32_t end = begin + runs[runIndex].length;
				const bool rightToLeft = (runs[runIndex].level & 1u) != 0;
				std::vector<FaceSlice> slices;
				size_t sliceBegin = 0;
				size_t previousFace = std::numeric_limits<size_t>::max();
				for (size_t index = 0; index < offsets.size();) {
					if (offsets[index] < begin) {
						index++;
						continue;
					}
					if (offsets[index] >= end) {
						break;
					}
					size_t graphemeEnd = index + 1;
					while (graphemeEnd < offsets.size() && !std::binary_search(
															   out.GraphemeBoundaries.begin(),
															   out.GraphemeBoundaries.end(),
															   offsets[graphemeEnd]
														   )) {
						graphemeEnd++;
					}
					const size_t selected = FaceForGrapheme(faces, order, codepoints, index, graphemeEnd);
					if (previousFace != std::numeric_limits<size_t>::max() && selected != previousFace) {
						slices.push_back(
							FaceSlice{
								previousFace, static_cast<uint32_t>(sliceBegin), static_cast<uint32_t>(index)
							}
						);
					}
					if (previousFace == std::numeric_limits<size_t>::max()) {
						sliceBegin = index;
					}
					previousFace = selected;
					index = graphemeEnd;
				}
				if (previousFace != std::numeric_limits<size_t>::max()) {
					const size_t endIndex = static_cast<size_t>(
						std::lower_bound(offsets.begin(), offsets.end(), end) - offsets.begin()
					);
					slices.push_back(
						FaceSlice{
							previousFace, static_cast<uint32_t>(sliceBegin), static_cast<uint32_t>(endIndex)
						}
					);
				}
				if (rightToLeft) {
					std::reverse(slices.begin(), slices.end());
				}
				for (const FaceSlice &slice : slices) {
					const uint32_t sourceEnd = slice.End < offsets.size()
												   ? offsets[slice.End]
												   : static_cast<uint32_t>(request.Text.size());
					if (!ShapeSlice(
							faces[slice.Face],
							offsets,
							codepoints,
							slice.Begin,
							slice.End,
							sourceEnd,
							rightToLeft,
							out
						)) {
						withinGlyphLimit = false;
						break;
					}
				}
				if (!withinGlyphLimit) {
					break;
				}
			}
			SBLineRelease(line);
		}

		SBParagraphRelease(paragraph);
		SBAlgorithmRelease(algorithm);
		Close(faces);
		if (!withinGlyphLimit) {
			out.Status = TextShapeStatus::TooLarge;
			out.Glyphs.clear();
			out.Runs.clear();
			out.Advance = 0.0f;
		}
		return out;
	}

	ShapedText ShapeText(const FontPackage &package, const TextShapeRequest &request) {
		if (request.Text.find('\n') == std::string_view::npos) return ShapeParagraph(package, request);
		if (request.Text.size() > MAXIMUM_SHAPED_TEXT_BYTES) {
			ShapedText refused;
			refused.Status = TextShapeStatus::TooLarge;
			return refused;
		}

		ShapedText out;
		uint32_t paragraph = 0;
		while (paragraph <= request.Text.size()) {
			const size_t found = request.Text.find('\n', paragraph);
			const uint32_t end =
				static_cast<uint32_t>(found == std::string_view::npos ? request.Text.size() : found);
			ShapedText part = ShapeParagraph(
				package,
				TextShapeRequest{
					request.Text.substr(paragraph, end - paragraph),
					request.Role,
					request.Direction,
					request.PixelSize
				}
			);
			if (part.Status != TextShapeStatus::Ok) return part;
			if (out.Glyphs.size() + part.Glyphs.size() > MAXIMUM_SHAPED_GLYPHS) {
				ShapedText refused;
				refused.Status = TextShapeStatus::TooLarge;
				return refused;
			}
			for (ShapedGlyph glyph : part.Glyphs) {
				glyph.SourceByte += paragraph;
				out.Glyphs.push_back(glyph);
			}
			const uint32_t offset = static_cast<uint32_t>(out.Glyphs.size() - part.Glyphs.size());
			for (ShapedRun run : part.Runs) {
				run.GlyphOffset += offset;
				run.SourceBegin += paragraph;
				run.SourceEnd += paragraph;
				out.Runs.push_back(run);
			}
			for (uint32_t boundary : part.GraphemeBoundaries) {
				const uint32_t adjusted = boundary + paragraph;
				if (out.GraphemeBoundaries.empty() || out.GraphemeBoundaries.back() != adjusted)
					out.GraphemeBoundaries.push_back(adjusted);
			}
			for (uint32_t boundary : part.LineBreakBoundaries) {
				const uint32_t adjusted = boundary + paragraph;
				if (out.LineBreakBoundaries.empty() || out.LineBreakBoundaries.back() != adjusted)
					out.LineBreakBoundaries.push_back(adjusted);
			}
			out.Advance = std::max(out.Advance, part.Advance);
			if (found == std::string_view::npos) break;
			const uint32_t next = end + 1;
			out.GraphemeBoundaries.push_back(next);
			out.LineBreakBoundaries.push_back(next);
			paragraph = next;
		}
		return out;
	}

	ShapedText LayoutText(
		const FontPackage &package,
		const TextShapeRequest &request,
		float width,
		bool wrapped,
		TextTruncate truncate,
		float lineHeight,
		std::span<const TextStyleSpan> styles
	) {
		const auto refused = [] {
			ShapedText value;
			value.Status = TextShapeStatus::TooLarge;
			return value;
		};
		if (styles.size() > 64) return refused();
		for (const TextStyleSpan &style : styles) {
			if (style.Begin >= style.End || style.End > request.Text.size() ||
				(style.PixelSize > 0.0f &&
				 (!std::isfinite(style.PixelSize) || style.PixelSize > MAXIMUM_SHAPED_PIXEL_SIZE)))
				return refused();
		}
		// Shape once before deciding breaks. The old code shaped each candidate and
		// each style fragment, which made wrapping quadratic and made a span edge a
		// false bidi paragraph boundary. The retained glyph clusters are the single
		// source of advances for wrapping, interaction and paint.
		ShapedText shaped = ShapeText(package, request);
		if (shaped.Status != TextShapeStatus::Ok || request.Text.empty()) return shaped;

		// A context shape always sees the whole paragraph. We select only its
		// clusters owned by the authored span, so Arabic joining and bidi levels do
		// not change at a rich-text boundary. The base shape supplies visual order.
		struct ContextShape {
			FontFace Role = FontFace::Regular;
			float PixelSize = 0.0f;
			ShapedText Text;
			std::unordered_map<uint32_t, std::vector<uint32_t>> Clusters;
		};
		const auto indexed = [](FontFace role, float pixelSize, ShapedText text) {
			ContextShape context{
				.Role = role,
				.PixelSize = pixelSize,
				.Text = std::move(text),
				.Clusters = {},
			};
			for (uint32_t index = 0; index < context.Text.Glyphs.size(); ++index) {
				context.Clusters[context.Text.Glyphs[index].SourceByte].push_back(index);
			}
			return context;
		};
		std::vector<ContextShape> contexts;
		contexts.push_back(indexed(request.Role, request.PixelSize, shaped));
		const auto contextFor = [&](FontFace role, float pixelSize) -> ContextShape * {
			for (ContextShape &context : contexts)
				if (context.Role == role && context.PixelSize == pixelSize) return &context;
			if (contexts.size() == 64) return nullptr;
			ShapedText contextual =
				ShapeText(package, TextShapeRequest{request.Text, role, request.Direction, pixelSize});
			if (contextual.Status != TextShapeStatus::Ok) return nullptr;
			contexts.push_back(indexed(role, pixelSize, std::move(contextual)));
			return &contexts.back();
		};
		const auto styleFor = [&styles](uint32_t source) -> const TextStyleSpan * {
			const auto found =
				std::find_if(styles.begin(), styles.end(), [source](const TextStyleSpan &style) {
					return source >= style.Begin && source < style.End;
				});
			return found == styles.end() ? nullptr : &*found;
		};
		ShapedText contextual;
		contextual.GraphemeBoundaries = shaped.GraphemeBoundaries;
		contextual.LineBreakBoundaries = shaped.LineBreakBoundaries;
		std::unordered_set<uint32_t> emitted;
		emitted.reserve(shaped.Glyphs.size());
		float pen = 0.0f;
		for (const ShapedGlyph &baseGlyph : shaped.Glyphs) {
			if (!emitted.insert(baseGlyph.SourceByte).second) continue;
			const TextStyleSpan *style = styleFor(baseGlyph.SourceByte);
			const FontFace role = style != nullptr ? style->Font : request.Role;
			const float pixelSize =
				style != nullptr && style->PixelSize > 0.0f ? style->PixelSize : request.PixelSize;
			ContextShape *context = contextFor(role, pixelSize);
			if (context == nullptr) {
				contextual.Status = TextShapeStatus::TooLarge;
				break;
			}
			const auto cluster = context->Clusters.find(baseGlyph.SourceByte);
			if (cluster == context->Clusters.end() || cluster->second.empty()) {
				contextual.Status = TextShapeStatus::NoUsableFont;
				break;
			}
			float left = std::numeric_limits<float>::max();
			float right = -std::numeric_limits<float>::max();
			for (const uint32_t index : cluster->second) {
				const ShapedGlyph &glyph = context->Text.Glyphs[index];
				left = std::min(left, glyph.X);
				right = std::max(right, glyph.X + glyph.AdvanceX);
			}
			if (left > right) {
				left = baseGlyph.X;
				right = baseGlyph.X + baseGlyph.AdvanceX;
			}
			const ShapedRun *baseRun = nullptr;
			for (const ShapedRun &run : shaped.Runs)
				if (run.SourceBegin <= baseGlyph.SourceByte && baseGlyph.SourceByte < run.SourceEnd) {
					baseRun = &run;
					break;
				}
			const bool rtl = baseRun != nullptr && baseRun->RightToLeft;
			const auto next = std::upper_bound(
				shaped.GraphemeBoundaries.begin(), shaped.GraphemeBoundaries.end(), baseGlyph.SourceByte
			);
			const uint32_t sourceEnd =
				next == shaped.GraphemeBoundaries.end() ? static_cast<uint32_t>(request.Text.size()) : *next;
			for (const uint32_t index : cluster->second) {
				const ShapedGlyph &candidate = context->Text.Glyphs[index];
				ShapedGlyph glyph = candidate;
				glyph.X = pen + candidate.X - left;
				glyph.PixelSize = pixelSize;
				glyph.Font = role;
				const uint32_t glyphOffset = static_cast<uint32_t>(contextual.Glyphs.size());
				contextual.Glyphs.push_back(glyph);
				if (contextual.Glyphs.size() > MAXIMUM_SHAPED_GLYPHS) {
					contextual.Status = TextShapeStatus::TooLarge;
					break;
				}
				if (contextual.Runs.empty() || contextual.Runs.back().Face != glyph.Face ||
					contextual.Runs.back().RightToLeft != rtl) {
					contextual.Runs.push_back(
						ShapedRun{glyph.Face, glyphOffset, 1, baseGlyph.SourceByte, sourceEnd, rtl}
					);
				} else {
					ShapedRun &run = contextual.Runs.back();
					run.GlyphCount++;
					run.SourceBegin = std::min(run.SourceBegin, baseGlyph.SourceByte);
					run.SourceEnd = std::max(run.SourceEnd, sourceEnd);
				}
			}
			if (contextual.Status != TextShapeStatus::Ok) break;
			pen += right - left;
		}
		if (contextual.Status != TextShapeStatus::Ok) return contextual;
		contextual.Advance = pen;
		shaped = std::move(contextual);

		// A segment tree turns every wrapping candidate into a logarithmic range
		// lookup. Logical source ranges may be non-contiguous in visual bidi order,
		// so a scalar prefix sum would be wrong here.
		struct RangeBounds {
			float Left = std::numeric_limits<float>::max();
			float Right = -std::numeric_limits<float>::max();
		};
		const size_t boundaryCount = shaped.GraphemeBoundaries.size() - 1;
		size_t treeBase = 1;
		while (treeBase < boundaryCount)
			treeBase <<= 1;
		std::vector<RangeBounds> sourceBounds(treeBase * 2);
		for (const ShapedGlyph &glyph : shaped.Glyphs) {
			const auto boundary = std::upper_bound(
				shaped.GraphemeBoundaries.begin(), shaped.GraphemeBoundaries.end(), glyph.SourceByte
			);
			if (boundary == shaped.GraphemeBoundaries.begin()) continue;
			const size_t source = static_cast<size_t>(boundary - shaped.GraphemeBoundaries.begin() - 1);
			RangeBounds &bounds = sourceBounds[treeBase + source];
			bounds.Left = std::min(bounds.Left, glyph.X);
			bounds.Right = std::max(bounds.Right, glyph.X + glyph.AdvanceX);
		}
		for (size_t index = treeBase - 1; index > 0; --index) {
			sourceBounds[index].Left =
				std::min(sourceBounds[index * 2].Left, sourceBounds[index * 2 + 1].Left);
			sourceBounds[index].Right =
				std::max(sourceBounds[index * 2].Right, sourceBounds[index * 2 + 1].Right);
		}
		std::unordered_map<uint32_t, std::vector<uint32_t>> glyphsForSource;
		glyphsForSource.reserve(shaped.GraphemeBoundaries.size());
		for (uint32_t index = 0; index < shaped.Glyphs.size(); ++index) {
			glyphsForSource[shaped.Glyphs[index].SourceByte].push_back(index);
		}
		std::vector<uint32_t> runForGlyph(shaped.Glyphs.size(), std::numeric_limits<uint32_t>::max());
		for (uint32_t run = 0; run < shaped.Runs.size(); ++run) {
			const ShapedRun &value = shaped.Runs[run];
			for (uint32_t glyph = value.GlyphOffset; glyph < value.GlyphOffset + value.GlyphCount; ++glyph) {
				if (glyph < runForGlyph.size()) runForGlyph[glyph] = run;
			}
		}
		const auto boundaryIndex = [&shaped](uint32_t source) {
			return static_cast<size_t>(
				std::lower_bound(shaped.GraphemeBoundaries.begin(), shaped.GraphemeBoundaries.end(), source) -
				shaped.GraphemeBoundaries.begin()
			);
		};
		ShapedText out;
		out.GraphemeBoundaries = shaped.GraphemeBoundaries;
		out.LineBreakBoundaries = shaped.LineBreakBoundaries;
		const float available = std::isfinite(width) ? std::max(width, 0.0f) : 0.0f;
		const auto lineWidth = [&sourceBounds, treeBase, &boundaryIndex](uint32_t begin, uint32_t end) {
			size_t left = boundaryIndex(begin) + treeBase;
			size_t right = boundaryIndex(end) + treeBase;
			RangeBounds result;
			while (left < right) {
				if ((left & 1u) != 0u) {
					result.Left = std::min(result.Left, sourceBounds[left].Left);
					result.Right = std::max(result.Right, sourceBounds[left++].Right);
				}
				if ((right & 1u) != 0u) {
					--right;
					result.Left = std::min(result.Left, sourceBounds[right].Left);
					result.Right = std::max(result.Right, sourceBounds[right].Right);
				}
				left >>= 1;
				right >>= 1;
			}
			return result.Left <= result.Right ? result.Right - result.Left : 0.0f;
		};
		const auto append = [&](uint32_t begin, uint32_t end) {
			if (out.Glyphs.size() > MAXIMUM_SHAPED_GLYPHS || out.Lines.size() == MAXIMUM_SHAPED_LINES) {
				out.Status = TextShapeStatus::TooLarge;
				return false;
			}
			float left = std::numeric_limits<float>::max();
			float right = -std::numeric_limits<float>::max();
			float ascent = 0.0f;
			float descent = 0.0f;
			const uint32_t glyphOffset = static_cast<uint32_t>(out.Glyphs.size());
			std::vector<uint32_t> lineGlyphs;
			for (size_t boundary = boundaryIndex(begin); boundary < boundaryIndex(end); ++boundary) {
				const auto found = glyphsForSource.find(shaped.GraphemeBoundaries[boundary]);
				if (found != glyphsForSource.end())
					lineGlyphs.insert(lineGlyphs.end(), found->second.begin(), found->second.end());
			}
			std::sort(lineGlyphs.begin(), lineGlyphs.end());
			for (const uint32_t sourceIndex : lineGlyphs) {
				const ShapedGlyph &source = shaped.Glyphs[sourceIndex];
				left = std::min(left, source.X);
				right = std::max(right, source.X + source.AdvanceX);
				ShapedGlyph glyph = source;
				ascent = std::max(ascent, glyph.PixelSize * 0.8f);
				descent = std::max(descent, glyph.PixelSize * 0.2f);
				out.Glyphs.push_back(glyph);
			}
			if (out.Glyphs.size() > MAXIMUM_SHAPED_GLYPHS) {
				out.Status = TextShapeStatus::TooLarge;
				return false;
			}
			const float origin = left <= right ? left : 0.0f;
			for (uint32_t index = glyphOffset; index < out.Glyphs.size(); ++index)
				out.Glyphs[index].X -= origin;
			const uint32_t line = static_cast<uint32_t>(out.Lines.size());
			uint32_t previousRun = std::numeric_limits<uint32_t>::max();
			for (uint32_t index = 0; index < lineGlyphs.size(); ++index) {
				const uint32_t sourceRun = runForGlyph[lineGlyphs[index]];
				if (sourceRun == std::numeric_limits<uint32_t>::max()) continue;
				if (sourceRun == previousRun) {
					out.Runs.back().GlyphCount++;
					continue;
				}
				const ShapedRun &source = shaped.Runs[sourceRun];
				out.Runs.push_back(
					ShapedRun{
						source.Face,
						glyphOffset + index,
						1,
						std::max(begin, source.SourceBegin),
						std::min(end, source.SourceEnd),
						source.RightToLeft
					}
				);
				previousRun = sourceRun;
			}
			const float advance = left <= right ? right - left : 0.0f;
			if (ascent == 0.0f && descent == 0.0f) {
				ascent = request.PixelSize * 0.8f;
				descent = request.PixelSize * 0.2f;
			}
			const float baseline =
				out.Lines.empty()
					? 0.0f
					: out.Lines.back().Baseline +
						  (out.Lines.back().Ascent + out.Lines.back().Descent) * std::max(lineHeight, 0.0f);
			out.Lines.push_back(
				ShapedLine{
					glyphOffset,
					static_cast<uint32_t>(out.Glyphs.size()) - glyphOffset,
					begin,
					end,
					advance,
					baseline,
					ascent,
					descent
				}
			);
			for (size_t boundaryIndexValue = boundaryIndex(begin); boundaryIndexValue <= boundaryIndex(end);
				 ++boundaryIndexValue) {
				const uint32_t boundary = out.GraphemeBoundaries[boundaryIndexValue];
				const ShapedRun *run = nullptr;
				for (const ShapedRun &candidate : out.Runs) {
					if (candidate.SourceBegin <= boundary && boundary <= candidate.SourceEnd) {
						run = &candidate;
						break;
					}
				}
				float stop = boundary == end ? advance : 0.0f;
				bool found = false;
				for (const uint32_t sourceIndex : lineGlyphs) {
					const ShapedGlyph &glyph = shaped.Glyphs[sourceIndex];
					if (glyph.SourceByte != boundary) continue;
					const bool rtl = run != nullptr && run->RightToLeft;
					const float edge = rtl ? glyph.X + glyph.AdvanceX - origin : glyph.X - origin;
					stop = found ? (rtl ? std::max(stop, edge) : std::min(stop, edge)) : edge;
					found = true;
				}
				if (!found && boundary == end && run != nullptr && run->RightToLeft) stop = 0.0f;
				out.CaretStops.push_back(ShapedCaretStop{line, boundary, stop});
			}
			out.Advance = std::max(out.Advance, advance);
			return true;
		};

		uint32_t paragraph = 0;
		while (paragraph <= request.Text.size()) {
			const size_t found = request.Text.find('\n', paragraph);
			const uint32_t end =
				static_cast<uint32_t>(found == std::string_view::npos ? request.Text.size() : found);
			if (paragraph == end) {
				if (!append(paragraph, end)) break;
			} else if (!wrapped || available <= 0.0f) {
				uint32_t fit = end;
				ShapedText dots;
				if (truncate == TextTruncate::AtEnd && available > 0.0f &&
					lineWidth(paragraph, end) > available) {
					dots = ShapeText(
						package, TextShapeRequest{"...", request.Role, request.Direction, request.PixelSize}
					);
					fit = paragraph;
					for (uint32_t boundary : out.GraphemeBoundaries) {
						if (boundary <= paragraph || boundary > end ||
							lineWidth(paragraph, boundary) + dots.Advance > available)
							break;
						fit = boundary;
					}
					// An oversized first grapheme still owns one line. This mirrors the
					// wrapping rule and avoids deleting an emoji or combining cluster.
					if (fit == paragraph)
						for (uint32_t boundary : out.GraphemeBoundaries)
							if (boundary > paragraph) {
								fit = std::min(boundary, end);
								break;
							}
				}
				if (!append(paragraph, fit)) break;
				if (fit < end && dots.Status == TextShapeStatus::Ok && !out.Lines.empty()) {
					if (out.Glyphs.size() + dots.Glyphs.size() > MAXIMUM_SHAPED_GLYPHS) {
						out.Status = TextShapeStatus::TooLarge;
						break;
					}
					ShapedLine &line = out.Lines.back();
					const uint32_t offset = static_cast<uint32_t>(out.Glyphs.size());
					for (ShapedGlyph glyph : dots.Glyphs) {
						glyph.SourceByte = fit;
						glyph.X += line.Advance;
						glyph.Synthetic = true;
						out.Glyphs.push_back(glyph);
					}
					line.GlyphCount += static_cast<uint32_t>(dots.Glyphs.size());
					line.Advance += dots.Advance;
					out.Runs.push_back(
						ShapedRun{
							dots.Glyphs.empty() ? core::Name{} : dots.Glyphs.front().Face,
							offset,
							static_cast<uint32_t>(dots.Glyphs.size()),
							fit,
							fit,
							false
						}
					);
					out.Advance = std::max(out.Advance, line.Advance);
				}
			} else {
				for (uint32_t begin = paragraph; begin < end;) {
					uint32_t fit = begin;
					for (auto boundary = std::upper_bound(
							 out.LineBreakBoundaries.begin(), out.LineBreakBoundaries.end(), begin
						 );
						 boundary != out.LineBreakBoundaries.end() && *boundary <= end;
						 ++boundary) {
						if (lineWidth(begin, *boundary) > available) break;
						fit = *boundary;
					}
					if (fit == begin)
						for (uint32_t boundary : out.GraphemeBoundaries)
							if (boundary > begin) {
								fit = std::min(boundary, end);
								break;
							}
					if (fit == begin || !append(begin, fit)) break;
					begin = fit;
				}
			}
			if (out.Status != TextShapeStatus::Ok || found == std::string_view::npos) break;
			paragraph = end + 1;
		}
		return out;
	}

	void ApplyTextStyles(
		ShapedText &text,
		std::span<const TextStyleSpan> styles,
		core::Color3 tint,
		float transparency,
		FontFace font,
		float pixelSize
	) {
		for (ShapedGlyph &glyph : text.Glyphs) {
			const auto found =
				std::find_if(styles.begin(), styles.end(), [&glyph](const TextStyleSpan &candidate) {
					return glyph.SourceByte >= candidate.Begin && glyph.SourceByte < candidate.End;
				});
			const TextStyleSpan *style = found == styles.end() ? nullptr : &*found;
			glyph.Tint = style != nullptr ? style->Tint : tint;
			glyph.Transparency = style != nullptr ? style->Transparency : transparency;
			glyph.PixelSize = style != nullptr && style->PixelSize > 0.0f ? style->PixelSize : pixelSize;
			glyph.Font = style != nullptr ? style->Font : font;
			glyph.Underline = style != nullptr && style->Underline;
			glyph.Strike = style != nullptr && style->Strike;
		}
	}

	ShapedCaret CaretFor(const ShapedText &text, uint32_t sourceByte) {
		ShapedCaret caret;
		if (text.Lines.empty()) return caret;
		const auto boundary =
			std::lower_bound(text.GraphemeBoundaries.begin(), text.GraphemeBoundaries.end(), sourceByte);
		const uint32_t source =
			boundary == text.GraphemeBoundaries.end() ? text.GraphemeBoundaries.back() : *boundary;
		for (size_t lineIndex = 0; lineIndex < text.Lines.size(); lineIndex++) {
			const ShapedLine &line = text.Lines[lineIndex];
			if (lineIndex + 1 < text.Lines.size() && source >= text.Lines[lineIndex + 1].SourceBegin)
				continue;
			for (const ShapedCaretStop &stop : text.CaretStops) {
				if (stop.Line == lineIndex && stop.SourceByte == source)
					return ShapedCaret{stop.Line, stop.X};
			}
			caret.Line = static_cast<uint32_t>(lineIndex);
			caret.X = line.Advance;
			for (uint32_t index = line.GlyphOffset; index < line.GlyphOffset + line.GlyphCount; index++) {
				const ShapedGlyph &glyph = text.Glyphs[index];
				if (glyph.SourceByte >= source) {
					caret.X = glyph.X;
					break;
				}
			}
			return caret;
		}
		return caret;
	}

	uint32_t SourceAt(const ShapedText &text, uint32_t lineIndex, float x) {
		if (lineIndex >= text.Lines.size()) return 0;
		float distance = std::numeric_limits<float>::max();
		uint32_t sourceAt = text.Lines[lineIndex].SourceBegin;
		for (const ShapedCaretStop &stop : text.CaretStops) {
			if (stop.Line != lineIndex) continue;
			const float candidate = std::fabs(stop.X - x);
			if (candidate < distance) {
				distance = candidate;
				sourceAt = stop.SourceByte;
			}
		}
		if (distance != std::numeric_limits<float>::max()) return sourceAt;
		const ShapedLine &line = text.Lines[lineIndex];
		uint32_t source = line.SourceBegin;
		for (uint32_t glyphIndex = line.GlyphOffset; glyphIndex < line.GlyphOffset + line.GlyphCount;
			 glyphIndex++) {
			const ShapedGlyph &glyph = text.Glyphs[glyphIndex];
			if (glyph.Synthetic) continue;
			if (x < glyph.X + glyph.AdvanceX * 0.5f) return glyph.SourceByte;
			source = glyph.SourceByte;
		}
		return source;
	}

	std::vector<ShapedSelection> SelectionFor(const ShapedText &text, uint32_t begin, uint32_t end) {
		std::vector<ShapedSelection> selection;
		if (begin > end) std::swap(begin, end);
		if (begin == end) return selection;
		for (size_t lineIndex = 0; lineIndex < text.Lines.size(); lineIndex++) {
			const ShapedLine &line = text.Lines[lineIndex];
			const uint32_t first = std::max(begin, line.SourceBegin);
			const uint32_t last = std::min(end, line.SourceEnd);
			if (first >= last) continue;

			std::vector<std::pair<float, float>> islands;
			for (uint32_t glyphIndex = line.GlyphOffset; glyphIndex < line.GlyphOffset + line.GlyphCount;
				 glyphIndex++) {
				const ShapedGlyph &glyph = text.Glyphs[glyphIndex];
				if (glyph.Synthetic || glyph.SourceByte < first || glyph.SourceByte >= last) continue;
				islands.emplace_back(glyph.X, glyph.X + glyph.AdvanceX);
			}
			std::sort(islands.begin(), islands.end());
			for (const auto &[left, right] : islands) {
				if (!selection.empty() && selection.back().Line == lineIndex &&
					left <= selection.back().EndX) {
					selection.back().EndX = std::max(selection.back().EndX, right);
				} else {
					selection.push_back(ShapedSelection{static_cast<uint32_t>(lineIndex), left, right});
				}
			}
		}
		return selection;
	}
}
