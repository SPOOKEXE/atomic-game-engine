#include "FontTextBatch.hpp"

#include "FontNativeCase.hpp"
#include "FontPayload.hpp"
#include "FontTextCase.hpp"
#include "FontUnicode.hpp"
#include "SourceBuiltinRandomContext.hpp"
#include "nodes/Path.hpp"
#include "nodes/SourceInterpret.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t FontTextWorkLimit = 16 * 1024 * 1024;
		const Value *FontTextField(const StructValue &structure, std::string_view key) {
			if (!structure.Data) return nullptr;
			for (const auto &[name, value] : structure.Data->Fields)
				if (name == key) return &value;
			return nullptr;
		}
		const StructValue *PreviousTextRow(const FontTextBatch &batch, size_t row) {
			if (!batch.PreviousRows) return nullptr;
			const auto &rows = *batch.PreviousRows;
			if (row >= rows.Elements.size()) return nullptr;
			return std::get_if<StructValue>(&rows.Elements[row]);
		}
		bool PreviousFontTextRow(NodeContext &c, FontTextBatch &batch, FontTextRow &row) {
			row.Atlas.ElementType = ValueType::Atlas;
			const auto *previous = PreviousTextRow(batch, c.ProcessorRow);
			if (!previous) return true;
			const auto *formatValue = FontTextField(*previous, "format");
			const auto *format = formatValue ? std::get_if<EnumValue>(formatValue) : nullptr;
			const auto *atlasValue = FontTextField(*previous, "atlas");
			const auto *atlas = atlasValue ? std::get_if<ArrayValue>(atlasValue) : nullptr;
			if (!format || format->Value < 0 || format->Value > int64_t(SurfaceFormat::R32Float) || !atlas ||
				atlas->ElementType != ValueType::Atlas || !atlas->Items.empty() || !atlas->Nested.empty())
				return c.Fail(Status::InvalidValue, "retained Text row state is malformed", "draw_data");
			row.Format = static_cast<SurfaceFormat>(format->Value);
			const auto bytes = ValueClonePayloadBytes(*atlasValue);
			if (!bytes)
				return c.Fail(Status::InvalidValue, "retained Text Atlas state is invalid", "draw_data");
			auto charge = c.ReserveWorkspace(*bytes, "draw_data");
			if (!charge) return false;
			row.Atlas = *atlas;
			row.RetainedAtlasCharge = std::move(*charge);
			return true;
		}
		bool TextChoice(NodeContext &c, std::string_view port, int64_t fallback, int64_t &value) {
			value = c.Integer(port, fallback);
			return c.FailureCode == Status::Ok;
		}
		bool TextColour(NodeContext &c, std::string_view port, Colour fallback, Colour &colour) {
			const auto *raw = c.Find(port);
			if (!raw) {
				colour = fallback;
				return true;
			}
			const auto decoded =
				std::visit([](const auto &leaf) { return InterpretPackedColour(leaf); }, *raw);
			if (!decoded)
				return c.Fail(
					Status::UnsupportedExecution, "Text colour requires a resolved packed source colour", port
				);
			colour = *decoded;
			return true;
		}
		bool TextVec2(NodeContext &c, std::string_view port, Vector2 fallback, Vector2 &output) {
			if (const auto *surface = c.Input(port)) {
				output = {double(surface->Width), double(surface->Height)};
				return true;
			}
			if (const auto *value = c.Find(port))
				if (const auto *surface = std::get_if<SurfaceValue>(value)) {
					output = {double(surface->Data.Width), double(surface->Data.Height)};
					return true;
				}
			output = c.Vec2(port, fallback);
			return c.FailureCode == Status::Ok && std::isfinite(output.X) && std::isfinite(output.Y);
		}
		bool CloneTextImage(
			NodeContext &c, std::string_view port, std::optional<Image> &output, AllocationReservation &charge
		) {
			const auto *image = c.Input(port);
			if (!image)
				if (const auto *raw = c.Find(port))
					if (const auto *surface = std::get_if<SurfaceValue>(raw)) image = &surface->Data;
			if (!image) return true;
			if (!ValidSurfaceLayout(*image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes) ||
				!FiniteSurfaceSamples(*image))
				return c.Fail(Status::InvalidValue, "Text source image is malformed", port);
			auto copy = c.ReserveWorkspace(sizeof(Image) + image->Pixels.capacity(), port);
			if (!copy) return false;
			output = *image;
			return charge.Merge(std::move(*copy));
		}
		bool TextWork(NodeContext &c, FontTextRow &row, uint64_t amount) {
			if (amount > FontTextWorkLimit - row.Work)
				return c.Fail(Status::LimitExceeded, "Text row exceeds bounded raster work", "surface_out");
			row.Work += amount;
			return true;
		}
	}
	bool BeginFontTextBatch(NodeContext &c, size_t count, FontTextBatch &batch) {
		ENGINE_PROFILE("imagegraph.text.prepare_batch");
		if (!count || count > Limits::MaximumArrayElements)
			return c.Fail(Status::LimitExceeded, "Text batch row count exceeds source limits");
		auto charge = c.ReserveWorkspace(count * sizeof(FontTextRow), "attribute_array_process");
		if (!charge) return false;
		batch.TableCharge = std::move(*charge);
		batch.Rows.reserve(count);
		batch.ExpectedRows = count;
		const auto *replay = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
		const StructValue *previous = nullptr;
		if (replay) {
			Diagnostic diagnostic;
			if (ValidateDataReplay(*replay, c.ByteBudget, diagnostic) != Status::Ok)
				return c.Fail(diagnostic.Code, diagnostic.Message);
			for (const auto &entry : replay->Entries)
				if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == 0 && !entry.Values.empty()) {
					if (previous) return c.Fail(Status::DuplicateId, "Text replay state is duplicated");
					previous = std::get_if<StructValue>(&entry.Values.back().Data);
					if (!previous)
						return c.Fail(
							Status::InvalidValue, "Text replay state does not carry named font and row state"
						);
				}
		}
		if (previous) {
			for (const auto role : {std::string_view{"primary"}, std::string_view{"fallback"}}) {
				const auto *raw = FontTextField(*previous, role);
				if (!raw) return c.Fail(Status::InvalidValue, "Text replay omits a named font field");
				if (std::holds_alternative<UndefinedValue>(*raw)) continue;
				const auto *font = std::get_if<FontValue>(raw);
				if (!font) return c.Fail(Status::InvalidValue, "Text replay font field is malformed");
				if (role == "primary") {
					if (!CloneTextFont(c, *font, batch.Fonts.Primary, batch.Fonts.PrimaryCharge, "font"))
						return false;
				} else if (!CloneTextFont(
							   c, *font, batch.Fonts.Fallback, batch.Fonts.FallbackCharge, "fallback_font"
						   ))
					return false;
			}
			const auto *rows = FontTextField(*previous, "rows");
			batch.PreviousRows = rows ? std::get_if<ArrayValue>(rows) : nullptr;
			if (!batch.PreviousRows || batch.PreviousRows->ElementType != ValueType::Struct ||
				!batch.PreviousRows->Items.empty() || !batch.PreviousRows->Nested.empty())
				return c.Fail(Status::InvalidValue, "Text replay row list is malformed");
		} else if (c.Request.SourceFonts) {
			const SourceFontInitialTextState *seed = nullptr;
			for (const auto &state : c.Request.SourceFonts->InitialTextFonts)
				if (state.NodeId == c.Authored.Id) seed = &state;
			if (seed) {
				if (seed->Primary &&
					!CloneTextFont(c, *seed->Primary, batch.Fonts.Primary, batch.Fonts.PrimaryCharge, "font"))
					return false;
				if (seed->Fallback &&
					!CloneTextFont(
						c, *seed->Fallback, batch.Fonts.Fallback, batch.Fonts.FallbackCharge, "fallback_font"
					))
					return false;
			} else if (c.Request.SourceFonts->InitialFont && !CloneTextFont(
																 c,
																 *c.Request.SourceFonts->InitialFont,
																 batch.Fonts.Primary,
																 batch.Fonts.PrimaryCharge,
																 "font"
															 ))
				return false;
		}
		return true;
	}
	bool QuoteFontTextMeasurementRow(NodeContext &c, FontTextBatch &batch) {
		const auto *raw = c.Find("text");
		const auto *text = raw ? std::get_if<std::string>(raw) : nullptr;
		if (!text)
			return c.Fail(Status::UnsupportedExecution, "Text requires a processor-selected string", "text");
		const bool full = c.Boolean("use_full_text_size");
		const double width = c.Scalar("max_line_width");
		int64_t changeCase = 0;
		if (!TextChoice(c, "change_case", 0, changeCase) || c.FailureCode != Status::Ok) return false;
		if (changeCase < 0 || changeCase > 3)
			return c.Fail(
				Status::UnsupportedExecution, "Text case choice is outside source branches", "change_case"
			);
		std::optional<std::string_view> observed;
		std::string failure;
		const auto status =
			FindFontTextCase(*text, uint8_t(changeCase), c.Request.SourceFonts, observed, failure);
		if (status != Status::Ok) return c.Fail(status, std::move(failure), "change_case");
		if (changeCase && !observed) {
			if (text->size() > (FontTextWorkLimit - batch.CaseWork) / 32)
				return c.Fail(
					Status::LimitExceeded, "native Text case batch exceeds work bound", "change_case"
				);
			batch.CaseWork += uint64_t(text->size()) * 32;
		}
		const bool trim = c.Boolean("trim");
		if ((!full || width == 0) && !trim) return true;
		uint64_t bytes = 0, points = 0, spaces = 0, run = 0;
		const auto emit = [&](uint32_t point) {
			bytes += point < 0x80 ? 1 : point < 0x800 ? 2 : point < 0x10000 ? 3 : 4;
			run = point == 0x20 ? run + 1 : 0;
			spaces = std::max(spaces, run);
			return ++points <= Limits::MaximumArrayElements && bytes <= Limits::MaximumTextBytes;
		};
		if (changeCase && !observed) {
			FontScalarCursor cursor{*text};
			uint32_t point = 0;
			bool titleStart = true;
			while (cursor.Next(point)) {
				const auto *mapping =
					changeCase == 3 && !titleStart ? nullptr : NativeCaseMapping(point, changeCase != 1);
				if (mapping) {
					for (size_t i = 0; i < mapping->Count; ++i)
						if (!emit(mapping->Output[i]))
							return c.Fail(
								Status::LimitExceeded,
								"native Text case output exceeds source limits",
								"change_case"
							);
				} else if (!emit(point))
					return c.Fail(
						Status::LimitExceeded, "native Text case output exceeds source limits", "change_case"
					);
				titleStart = point == 0x20;
			}
			if (cursor.Invalid)
				return c.Fail(Status::InvalidValue, "native Text casing requires valid UTF-8", "change_case");
			// Contextual sigma substitutes a same-width non-space scalar, so this quote is exact.
		} else {
			const auto source = observed ? *observed : std::string_view{*text};
			bytes = source.size();
			for (const auto byte : source) {
				run = byte == ' ' ? run + 1 : 0;
				spaces = std::max(spaces, run);
			}
		}
		if (!std::isfinite(width) || bytes > Limits::MaximumTextBytes)
			return c.Fail(
				Status::InvalidValue, "native Text measurement input is malformed", "max_line_width"
			);
		if (trim) {
			// Scalar decoding, token indexing and copying are bounded linear passes, quoted before fonts.
			if (bytes > (FontTextWorkLimit - batch.TrimWork) / 8)
				return c.Fail(Status::LimitExceeded, "native Text trim batch exceeds work bound", "range");
			batch.TrimWork += bytes * 8;
		}
		if (!full || width == 0 || !bytes) return true;
		const uint64_t perByte = NativeFontMeasurementWorkPerByte +
								 (width >= 1 && !(width >= 10000000 && width < 10000001) ? 2 * spaces : 0);
		const auto *fallbackValue = c.Find("fallback_font");
		const auto *fallbackPath = fallbackValue ? std::get_if<std::string>(fallbackValue) : nullptr;
		const uint64_t roles = fallbackPath && !fallbackPath->empty() ? 2 : 1;
		const uint64_t available = (FontTextWorkLimit - batch.MeasurementWork) / roles;
		if (available < NativeFontMeasurementWorkPerByte ||
			bytes > (available - NativeFontMeasurementWorkPerByte) / perByte)
			return c.Fail(
				Status::LimitExceeded, "native Text measurement batch exceeds work bound", "max_line_width"
			);
		batch.MeasurementWork += (bytes * perByte + NativeFontMeasurementWorkPerByte) * roles;
		return true;
	}
	bool PrepareFontTextRow(NodeContext &c, FontTextBatch &batch) {
		ENGINE_PROFILE("imagegraph.text.prepare_row");
		if (batch.Admitted || c.ProcessorRow != batch.PreparedRows ||
			batch.PreparedRows >= batch.ExpectedRows)
			return c.Fail(
				Status::InvalidValue, "Text row preparation order differs from its admitted schedule"
			);
		auto &row = batch.Rows.emplace_back();
		if (!PreviousFontTextRow(c, batch, row)) return false;
		const auto *raw = c.Find("text");
		const auto *text = raw ? std::get_if<std::string>(raw) : nullptr;
		if (!text)
			return c.Fail(Status::UnsupportedExecution, "Text requires a processor-selected string", "text");
		int64_t changeCase = 0, trimType = 0, dimension = 1, halign = 0, valign = 0, paletteMode = 0;
		if (!TextChoice(c, "change_case", 0, changeCase) || !TextChoice(c, "trim_type", 0, trimType) ||
			!TextChoice(c, "dimension", 1, dimension) || !TextChoice(c, "h_align", 0, halign) ||
			!TextChoice(c, "v_align", 0, valign) || !TextChoice(c, "color_by_letter_select", 0, paletteMode))
			return false;
		if (changeCase < 0 || changeCase > 3 || trimType < 0 || trimType > 2 || paletteMode < 0 ||
			paletteMode > 2)
			return c.Fail(
				Status::UnsupportedExecution, "Text choice lies outside represented source branches"
			);
		const auto size = c.Integer("size", 16);
		if (size < 1 || size > 512)
			return c.Fail(Status::LimitExceeded, "Text font size must be between 1 and 512", "size");
		FontTextLayoutOptions options;
		options.ChangeCase = uint8_t(changeCase);
		options.TrimType = uint8_t(trimType);
		options.Trim = c.Boolean("trim");
		options.FullTextSize = c.Boolean("use_full_text_size");
		options.Monospaced = c.Boolean("monospaced");
		options.SplitWord = c.Boolean("split_word", true);
		options.Tracking = c.Scalar("letter_spacing");
		options.LineGap = c.Scalar("line_height");
		options.MaximumLineWidth = c.Scalar("max_line_width");
		if (!TextVec2(c, "range", {0, 1}, options.Range)) return false;

		std::string caseFailure;
		std::string nativeCased;
		AllocationReservation nativeCaseCharge;
		const auto caseStatus = FindFontTextCase(
			*text, uint8_t(changeCase), c.Request.SourceFonts, options.ObservedCasedText, caseFailure
		);
		if (caseStatus != Status::Ok) return c.Fail(caseStatus, std::move(caseFailure), "change_case");
		if (changeCase && !options.ObservedCasedText) {
			const auto maximum = c.AvailableBytes();
			auto reservation = c.ReserveWorkspace(maximum, "change_case");
			if (!reservation) return false;
			const auto nativeStatus =
				NativeFontTextCase(*text, uint8_t(changeCase), maximum, nativeCased, caseFailure);
			if (nativeStatus != Status::Ok)
				return c.Fail(nativeStatus, std::move(caseFailure), "change_case");
			if (!reservation->Resize(nativeCased.capacity() + 1))
				return c.Fail(
					Status::LimitExceeded, "native Text case retained bytes exceed budget", "change_case"
				);
			nativeCaseCharge = std::move(*reservation);
			options.ObservedCasedText = nativeCased;
		}

		const bool antialias = c.Boolean("anti_aliasing"), sdf = c.Boolean("use_sdf");
		(void)c.Boolean("round_position", true);
		(void)c.Integer("blend_mode", 1);
		Vector2 unusedRange;
		if (!TextVec2(c, "character_range", {32, 128}, unusedRange) || c.FailureCode != Status::Ok)
			return false;
		AllocationReservation measurementCharge;
		std::optional<FontMeasurement> fullMeasurement;
		if (options.FullTextSize && options.MaximumLineWidth != 0 && !text->empty()) {
			const auto rawText =
				options.ObservedCasedText ? *options.ObservedCasedText : std::string_view{*text};
			auto reserved = c.ReserveWorkspace(
				sizeof(FontMeasurement) + std::max(rawText.size(), std::string{}.capacity()), "max_line_width"
			);
			if (!reserved) return false;
			fullMeasurement.emplace(
				FontMeasurement{std::string{rawText}, options.MaximumLineWidth, -1, 0, 0}
			);
			if (!reserved->Resize(sizeof(FontMeasurement) + fullMeasurement->Text.capacity()))
				return c.Fail(
					Status::LimitExceeded, "Text measurement request backing exceeds budget", "max_line_width"
				);
			measurementCharge = std::move(*reserved);
		}
		if (!SelectTextFont(
				c,
				*text,
				uint8_t(changeCase),
				options.ObservedCasedText,
				options.Monospaced,
				options.MaximumLineWidth != 0 && !options.SplitWord,
				uint32_t(size),
				antialias,
				sdf,
				fullMeasurement ? std::span<const FontMeasurement>{&*fullMeasurement, 1}
								: std::span<const FontMeasurement>{},
				batch.Fonts,
				row.SelectedFont
			))
			return false;
		row.Empty = text->empty();
		if (row.Empty) {
			const auto layout = CheckedSurfaceLayout(1, 1, row.Format, Limits::MaximumOutputBytes);
			if (!layout) return c.Fail(Status::InvalidValue, "retained empty Text format is invalid");
			row.Surface = *layout;
			row.OutputBytes = layout->Bytes + RetainedPayloadBytes(row.Atlas) + 2 * std::string{}.capacity();
			row.Work = 1;
			++batch.PreparedRows;
			return true;
		}
		uint64_t layoutBytes =
			sizeof(FontTextLayout) +
			4 * (text->size() + (options.ObservedCasedText ? options.ObservedCasedText->size() : 0) +
				 MaximumFontGlyphs + 32) +
			Limits::MaximumArrayElements * (sizeof(FontTextLine) + 32);
		if (options.Trim && options.TrimType)
			layoutBytes += sizeof(std::vector<std::string_view>) +
						   (Limits::MaximumArrayElements + 1) * sizeof(std::string_view);
		if (options.FullTextSize && options.MaximumLineWidth != 0) {
			const auto extra = FontTextNativeMeasurementAdmissionBytes(
				*row.SelectedFont.Font.Data,
				options.ObservedCasedText ? options.ObservedCasedText->size() : text->size()
			);
			if (!extra || *extra > c.AvailableBytes() || layoutBytes > c.AvailableBytes() - *extra)
				return c.Fail(
					Status::LimitExceeded,
					"Text native measurement layout exceeds staging budget",
					"max_line_width"
				);
			layoutBytes += *extra;
		}
		auto layoutCharge = c.ReserveWorkspace(layoutBytes, "text");
		if (!layoutCharge) return false;
		std::string failure;
		const auto layoutStatus = BuildFontTextLayout(
			*row.SelectedFont.Font.Data, *text, options, layoutBytes, row.Layout, failure
		);
		if (layoutStatus != Status::Ok) return c.Fail(layoutStatus, std::move(failure), "text");
		uint64_t retainedLayout = sizeof(FontTextLayout) + row.Layout.RawText.capacity() +
								  row.Layout.Text.capacity() +
								  row.Layout.Lines.capacity() * sizeof(FontTextLine);
		for (const auto &line : row.Layout.Lines)
			retainedLayout += line.Text.capacity();
		if (!layoutCharge->Resize(retainedLayout))
			return c.Fail(
				Status::LimitExceeded, "Text retained layout capacities exceed staging budget", "text"
			);
		row.LayoutCharge = std::move(*layoutCharge);
		row.GenerateAtlas = c.Boolean("atlas");
		row.ClearBackground = c.Boolean("render_background");
		if (!TextColour(c, "bg_color", {0, 0, 0, 255}, row.BackgroundColour)) return false;
		row.BackgroundColour.Alpha = 255;
		Colour mainColour;
		if (!TextColour(c, "color", {255, 255, 255, 255}, mainColour)) return false;
		if (!CloneTextImage(c, "texture", row.Texture, row.InputsCharge) ||
			!CloneTextImage(c, "background", row.Background, row.InputsCharge))
			return false;
		if (row.Texture &&
			!ObserveBitmapTextTexture(c, row.Layout, uint32_t(size), antialias, row.SelectedFont))
			return false;
		row.Raster.Texture = row.Texture ? &*row.Texture : nullptr;
		row.Raster.DistanceAntialias = antialias;
		row.Raster.DebugTexture = c.Boolean("attribute_debug_texture");
		row.Raster.NativeBitmapTexture =
			c.Request.SourceFonts &&
			c.Request.SourceFonts->BitmapTextureProfile == FontBitmapTextureProfile::NativeFrameUv;
		row.Raster.Sampler = ReadSampler(c);
		if (!SupportedSampler(c, row.Raster.Sampler)) return false;
		const bool wave = c.Boolean("wave"), fit = c.Boolean("scale_to_fit"),
				   rotate = c.Boolean("rotate_along_path", true);
		const double amplitude = c.Scalar("wave_amplitude", 4), waveScale = c.Scalar("wave_scale", 30),
					 phase = -c.Scalar("wave_phase"), shape = c.Scalar("wave_shape"),
					 shift = c.Scalar("path_shift");
		Vector2 fixed, offset;
		if (!TextVec2(
				c, "fixed_dimension", {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)}, fixed
			) ||
			!TextVec2(c, "offset", {}, offset))
			return false;
		if (const auto fallback = c.IsCatalogueDefault("fixed_dimension"); fallback && *fallback)
			fixed = {double(c.Project.SurfaceWidth), double(c.Project.SurfaceHeight)};
		const auto padding = c.Get<Vector4>("padding");
		const auto *pathValue = c.Find("path");
		const auto *path = pathValue ? std::get_if<Path2D>(pathValue) : nullptr;
		PathRuntime runtime;
		const bool usePath = path && (!path->Anchors.empty() || path->SourceOperation);
		if (usePath && !runtime.Init(c, *path)) return false;
		double canvasWidth = (usePath || dimension == 0) ? fixed.X : row.Layout.Width;
		double canvasHeight =
			(usePath || dimension == 0) ? fixed.Y : row.Layout.Height + (wave ? 2 * std::abs(amplitude) : 0);
		double scale = 1;
		if (!usePath && dimension == 0 && fit)
			scale = std::min(fixed.X / row.Layout.Width, fixed.Y / row.Layout.Height);
		canvasWidth += padding.X + padding.Z;
		canvasHeight += padding.Y + padding.W;
		if (!std::isfinite(canvasWidth) || !std::isfinite(canvasHeight) || !std::isfinite(scale) ||
			!std::isfinite(amplitude) || !std::isfinite(waveScale) || !std::isfinite(phase) ||
			!std::isfinite(shape) || !std::isfinite(shift) || !std::isfinite(padding.X) ||
			!std::isfinite(padding.Y) || !std::isfinite(padding.Z) || !std::isfinite(padding.W))
			return c.Fail(
				Status::InvalidValue, "Text dimensions and transforms must remain finite", "fixed_dimension"
			);
		if (canvasWidth > c.Request.MaximumImageDimension || canvasHeight > c.Request.MaximumImageDimension)
			return c.Fail(Status::LimitExceeded, "Text canvas exceeds maximum dimensions", "fixed_dimension");
		row.Width = uint32_t(std::max(1.0, std::floor(canvasWidth)));
		row.Height = uint32_t(std::max(1.0, std::floor(canvasHeight)));
		const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		row.Format = *format;
		const auto surface =
			CheckedSurfaceLayout(row.Width, row.Height, row.Format, Limits::MaximumOutputBytes);
		if (!surface)
			return c.Fail(Status::LimitExceeded, "Text surface layout exceeds output bytes", "surface_out");
		row.Surface = *surface;
		const auto *paletteValue = c.Find("color_by_letter");
		const auto *palette = paletteValue ? std::get_if<ArrayValue>(paletteValue) : nullptr;
		if (!palette || !palette->Nested.empty() || (!palette->Items.empty() && !palette->Elements.empty()))
			return c.Fail(
				Status::UnsupportedExecution,
				"Text palette requires flat packed colour leaves",
				"color_by_letter"
			);
		const size_t paletteSize = palette->Items.empty() ? palette->Elements.size() : palette->Items.size();
		if (!paletteSize || paletteSize > Limits::MaximumArrayElements)
			return c.Fail(
				Status::UnsupportedExecution,
				"empty Text palette has no represented source colour",
				"color_by_letter"
			);
		const auto drawCount = row.Layout.CharacterCount;
		auto drawCharge = c.ReserveWorkspace(
			drawCount * (sizeof(FontGlyphPlacement) + sizeof(FontGlyphRasterFootprint) +
						 (row.GenerateAtlas ? sizeof(FontTextAtlasDraw) : 0)),
			"text"
		);
		if (!drawCharge) return false;
		row.Draws.reserve(drawCount);
		if (row.GenerateAtlas) row.AtlasDraws.reserve(drawCount);
		row.Footprints.reserve(drawCount);
		row.DrawCharge = std::move(*drawCharge);
		const SourceBuiltinRandomCapture *random = nullptr;
		if ((wave && shape >= 3) || paletteMode == 2)
			if (!FindSourceBuiltinRandomCapture(c, random)) return false;
		size_t drawIndex = 0;
		const auto observedDraw = [&](SourceBuiltinRandomOperation operation,
									  double low,
									  double high,
									  double &value) {
			if (!random || drawIndex == random->Draws.size())
				return c.Fail(
					Status::UnsupportedExecution,
					"Text random drawing lacks an ordered source observation",
					"seed"
				);
			const auto &draw = random->Draws[drawIndex++];
			if (draw.Operation != operation || draw.Lower != low || draw.Upper != high ||
				!std::isfinite(draw.Result) || draw.Result < low || draw.Result > high)
				return c.Fail(
					Status::InvalidValue, "Text random observation differs from its ordered request", "seed"
				);
			value = draw.Result;
			return true;
		};
		double y = padding.Y;
		if (dimension == 0) {
			if (valign == 1) y = (canvasHeight - row.Layout.Height * scale) / 2;
			if (valign == 2) y = canvasHeight - padding.W - row.Layout.Height * scale;
		}
		if (wave) y += std::abs(amplitude);
		if (usePath) {
			y = 0;
			if (valign == 1) y = (row.Layout.Height - row.SelectedFont.Font.Data->LineHeight) / 2;
			if (valign == 2) y = row.Layout.Height;
		}
		for (const auto &line : row.Layout.Lines) {
			double x = padding.Z;
			if (halign == 1) x = (canvasWidth - line.Width * scale) / 2;
			if (halign == 2) x = canvasWidth - padding.X - line.Width * scale;
			if (usePath) {
				x = shift;
				if (halign == 1) x += runtime.Length(0) / 2 - line.Width / 2;
				if (halign == 2) x += runtime.Length(0) - line.Width;
			}
			FontScalarCursor cursor{line.Text};
			uint32_t character = 0;
			size_t index = 1;
			while (cursor.Next(character)) {
				FontGlyphPlacement placement;
				placement.Character = character;
				placement.Position = {x, y};
				placement.Scale = {scale, scale};
				if (usePath) {
					const auto first = runtime.PointDistance(x, 0), second = runtime.PointDistance(x + .1, 0);
					placement.Rotation =
						rotate ? -std::atan2(second.Y - first.Y, second.X - first.X) * 180 / std::numbers::pi
							   : 0;
					const double normal = (placement.Rotation + 90) * std::numbers::pi / 180;
					placement.Position = {first.X + y * std::cos(normal), first.Y - y * std::sin(normal)};
					placement.Scale = {1, 1};
					placement.VerticalOrigin = valign == 1 ? row.SelectedFont.Font.Data->LineHeight / 2 : 0;
				}
				if (wave) {
					const double angle = phase + index * waveScale;
					const double sine = std::sin(angle * std::numbers::pi / 180) * amplitude;
					const double square = sine < 0 ? -amplitude : amplitude;
					const double tau = std::fmod(std::abs(angle + 90), 360);
					const double triangle = ((tau > 180 ? 360 - tau : tau) / 180 * 2 - 1) * amplitude;
					double displacement = sine;
					if (shape >= 0 && shape < 1)
						displacement = sine + (triangle - sine) * (shape - std::floor(shape));
					else if (shape >= 1 && shape < 2)
						displacement = triangle + (square - triangle) * (shape - std::floor(shape));
					else if (shape >= 2 && shape < 3)
						displacement =
							std::fmod(std::abs(angle), 360) > 360 * (.5 - (shape - std::floor(shape)) / 2)
								? -amplitude
								: amplitude;
					else if (shape >= 3) {
						if (!observedDraw(SourceBuiltinRandomOperation::RandomRange, -1, 1, displacement))
							return false;
						displacement *= amplitude;
					}
					if (usePath) {
						const double normal = (placement.Rotation + 180) * std::numbers::pi / 180;
						placement.Position.X += displacement * std::cos(normal);
						placement.Position.Y -= displacement * std::sin(normal);
					} else
						placement.Position.Y += displacement;
				}
				size_t paletteIndex = row.Draws.size() % paletteSize;
				if (paletteMode == 1) {
					const auto period = paletteSize * 2 - 1;
					const auto step = row.Draws.size() % period;
					paletteIndex = step >= paletteSize ? period - step : step;
				}
				if (paletteMode == 2) {
					double selected = 0;
					if (!observedDraw(
							SourceBuiltinRandomOperation::IRandom, 0, double(paletteSize - 1), selected
						))
						return false;
					if (std::trunc(selected) != selected)
						return c.Fail(
							Status::InvalidValue,
							"Text palette observation index must be integral",
							"color_by_letter"
						);
					paletteIndex = size_t(selected);
				}
				const auto *leaf = palette->Items.empty()
									   ? &palette->Elements[paletteIndex]
									   : std::get_if<ElementValue>(&palette->Items[paletteIndex].Data);
				const auto colour =
					leaf ? std::visit(
							   [](const auto &rawColour) { return InterpretPackedColour(rawColour); }, *leaf
						   )
						 : std::optional<Colour>{};
				if (!colour)
					return c.Fail(
						Status::UnsupportedExecution,
						"Text palette leaf is not a packed source colour",
						"color_by_letter"
					);
				placement.Tint = {
					uint8_t(uint32_t(mainColour.Red) * colour->Red / 255),
					uint8_t(uint32_t(mainColour.Green) * colour->Green / 255),
					uint8_t(uint32_t(mainColour.Blue) * colour->Blue / 255),
					uint8_t(uint32_t(mainColour.Alpha) * colour->Alpha / 255)
				};
				placement.Position.X += offset.X;
				placement.Position.Y += offset.Y;
				const double advance = FontScalarAdvance(*row.SelectedFont.Font.Data, character);
				if (!usePath && options.Monospaced)
					placement.Position.X += (row.Layout.MonoWidth - advance) / 2;
				FontGlyphRasterFootprint footprint;
				const auto measured = MeasureFontGlyphRaster(
					*row.SelectedFont.Font.Data,
					placement,
					row.Raster,
					row.Width,
					row.Height,
					footprint,
					failure
				);
				if (measured != Status::Ok) return c.Fail(measured, std::move(failure), "text");
				if (!TextWork(c, row, footprint.Work)) return false;
				row.Draws.push_back(placement);
				row.Footprints.push_back(footprint);
				x += ((options.Monospaced ? row.Layout.MonoWidth : advance) + options.Tracking) *
					 (usePath ? 1 : scale);
				++index;
			}
			y += (line.Height + options.LineGap) * (usePath ? -1 : scale);
		}
		if (random && drawIndex != random->Draws.size())
			return c.Fail(Status::InvalidValue, "Text random observation contains unused draws", "seed");
		if (c.Request.RequireSourceGpuRasterCoverage && !row.Draws.empty())
			return c.Fail(
				Status::UnsupportedExecution,
				"Text glyph GPU coverage requires source raster observations",
				"surface_out"
			);
		if (!TextWork(c, row, uint64_t(row.Width) * row.Height * (row.Background ? 8 : 1))) return false;
		uint64_t atlasBytes = RetainedPayloadBytes(row.Atlas);
		if (row.GenerateAtlas) {
			row.Atlas = ArrayValue{};
			row.Atlas.ElementType = ValueType::Atlas;
			row.RetainedAtlasCharge.Reset();
			atlasBytes = sizeof(ArrayValue) + row.Draws.size() * sizeof(ElementValue);
			for (const auto &draw : row.Draws) {
				FontTextAtlasDraw atlas;
				const double width =
					(options.Monospaced ? row.Layout.MonoWidth
										: FontScalarAdvance(*row.SelectedFont.Font.Data, draw.Character)) *
					draw.Scale.X;
				const double height = row.SelectedFont.Font.Data->LineHeight * draw.Scale.Y;
				if (!std::isfinite(width) || !std::isfinite(height) ||
					width > c.Request.MaximumImageDimension || height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Text Atlas glyph dimensions exceed limits", "draw_data"
					);
				atlas.Width = uint32_t(std::max(1.0, std::floor(width)));
				atlas.Height = uint32_t(std::max(1.0, std::floor(height)));
				const auto surface = CheckedSurfaceLayout(
					atlas.Width, atlas.Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumArrayBytes
				);
				if (!surface)
					return c.Fail(
						Status::LimitExceeded, "Text Atlas glyph surface exceeds owned bytes", "draw_data"
					);
				atlas.Surface = *surface;
				atlas.Placement = draw;
				atlas.Placement.Position = {};
				atlas.Placement.VerticalOrigin = 0;
				FontTextRasterOptions raster;
				raster.Sampler = {1, 3};
				raster.Blend = FontTextBlend::AlphaAdd;
				const auto measured = MeasureFontGlyphRaster(
					*row.SelectedFont.Font.Data,
					atlas.Placement,
					raster,
					atlas.Width,
					atlas.Height,
					atlas.Footprint,
					failure
				);
				if (measured != Status::Ok) return c.Fail(measured, std::move(failure), "draw_data");
				if (!TextWork(c, row, atlas.Footprint.Work + uint64_t(row.Width) * row.Height)) return false;
				const uint64_t bytes = sizeof(AtlasData) + atlas.Surface.Bytes + row.Surface.Bytes;
				if (bytes > Limits::MaximumArrayBytes - atlasBytes)
					return c.Fail(
						Status::LimitExceeded, "Text Atlas list exceeds owned value bytes", "draw_data"
					);
				atlasBytes += bytes;
				row.AtlasDraws.push_back(atlas);
			}
		}

		row.OutputBytes = row.Surface.Bytes + atlasBytes + sizeof(ArrayValue) +
						  std::max(std::string{}.capacity(), std::string_view{"surface_out"}.size()) +
						  std::max(std::string{}.capacity(), std::string_view{"draw_data"}.size());
		row.TemporaryBytes = row.Background ? row.Surface.Bytes : 0;
		if (c.FailureCode != Status::Ok) return false;
		++batch.PreparedRows;
		return true;
	}
	bool AdmitFontTextBatch(NodeContext &c, FontTextBatch &batch) {
		ENGINE_PROFILE("imagegraph.text.admit_batch");
		if (batch.Admitted || batch.PreparedRows != batch.ExpectedRows)
			return c.Fail(Status::InvalidValue, "Text batch preparation is incomplete");
		uint64_t outputBytes = 0, temporaryBytes = 0,
				 statePayload = sizeof(StructData) +
								3 * (sizeof(std::pair<std::string, Value>) + std::string{}.capacity()) +
								sizeof(ArrayValue) + batch.Rows.size() * sizeof(ElementValue);
		for (const auto *font : {&batch.Fonts.Primary, &batch.Fonts.Fallback})
			if (*font) {
				const uint64_t bytes = FontStorageBytes(**font, true);
				if (bytes > Limits::MaximumArrayBytes - statePayload)
					return c.Fail(
						Status::LimitExceeded, "Text retained font state exceeds value bytes", "font"
					);
				statePayload += bytes;
			}
		for (const auto &row : batch.Rows) {
			if (row.Work > FontTextWorkLimit - batch.Work)
				return c.Fail(
					Status::LimitExceeded,
					"Text whole processor batch exceeds bounded raster work",
					"surface_out"
				);
			batch.Work += row.Work;
			if (row.OutputBytes > Limits::MaximumEvaluationBytes - outputBytes)
				return c.Fail(
					Status::LimitExceeded,
					"Text whole processor batch exceeds candidate output bytes",
					"surface_out"
				);
			outputBytes += row.OutputBytes;
			temporaryBytes = std::max(temporaryBytes, row.TemporaryBytes);
			uint64_t atlasBytes = RetainedPayloadBytes(row.Atlas);
			if (row.GenerateAtlas && !row.Empty) {
				atlasBytes = sizeof(ArrayValue) + row.AtlasDraws.size() * sizeof(ElementValue);
				for (const auto &draw : row.AtlasDraws)
					atlasBytes += sizeof(AtlasData) + draw.Surface.Bytes + row.Surface.Bytes;
			}
			const uint64_t bytes = sizeof(StructData) +
								   2 * (sizeof(std::pair<std::string, Value>) + std::string{}.capacity()) +
								   atlasBytes;
			if (bytes > Limits::MaximumArrayBytes - statePayload)
				return c.Fail(
					Status::LimitExceeded,
					"Text retained row and Atlas state exceeds value bytes",
					"draw_data"
				);
			statePayload += bytes;
		}
		const uint64_t stateBytes = statePayload + sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
									std::max(c.Authored.Id.size(), std::string{}.capacity() * 2);
		auto stateCharge = c.ReserveWorkspace(stateBytes, "draw_data");
		if (!stateCharge) return false;
		batch.StateCharge = std::move(*stateCharge);
		auto outputCharge = c.ReserveWorkspace(outputBytes, "surface_out");
		if (!outputCharge) return false;
		batch.OutputCharge = std::move(*outputCharge);
		auto temporaryCharge = c.ReserveWorkspace(temporaryBytes, "background");
		if (!temporaryCharge) return false;
		batch.TemporaryCharge = std::move(*temporaryCharge);
		// Metadata is admitted before any row image. ClearOutputs keeps this storage across rows.
		if (!c.ReserveOutput(0, "surface_out")) return false;
		batch.State.NodeId = c.Authored.Id;
		batch.State.ProcessorRow = 0;
		batch.State.Tick = c.Request.Tick;
		batch.State.Subframe = c.Request.Subframe;
		batch.State.NegativeFrame = c.Request.NegativeFrame;
		batch.State.Initialized = true;
		batch.State.PreviousFrame =
			double(FrameTimeToReal({c.Request.Tick, c.Request.Subframe, c.Request.NegativeFrame}));
		batch.State.Values.reserve(1);
		c.DataUpdates.reserve(1);
		StructValue state;
		auto &fields = state.Data.emplace().Fields;
		fields.reserve(3);
		fields.push_back(
			{"primary",
			 batch.Fonts.Primary ? Value{std::move(*batch.Fonts.Primary)} : Value{UndefinedValue{}}}
		);
		fields.push_back(
			{"fallback",
			 batch.Fonts.Fallback ? Value{std::move(*batch.Fonts.Fallback)} : Value{UndefinedValue{}}}
		);
		ArrayValue rows;
		rows.ElementType = ValueType::Struct;
		rows.Elements.reserve(batch.Rows.size());
		for (const auto &row : batch.Rows) {
			StructValue entry;
			auto &values = entry.Data.emplace().Fields;
			values.reserve(2);
			values.push_back({"format", EnumValue{int64_t(row.Format)}});
			ArrayValue atlas;
			atlas.ElementType = ValueType::Atlas;
			values.push_back({"atlas", std::move(atlas)});
			rows.Elements.push_back(std::move(entry));
		}
		fields.push_back({"rows", std::move(rows)});
		batch.State.Values.push_back({c.Request.Tick, std::move(state)});
		batch.Fonts.Primary.reset();
		batch.Fonts.Fallback.reset();
		batch.Fonts.PrimaryCharge.Reset();
		batch.Fonts.FallbackCharge.Reset();
		batch.Admitted = true;
		return true;
	}
	namespace {
		bool CompositeTextBackground(NodeContext &c, const FontTextRow &row, Image &output) {
			if (!row.Background) return true;
			Image composed{row.Width, row.Height, std::vector<uint8_t>(row.Surface.Bytes)};
			composed.Format = row.Format;
			core::Metrics::Count(
				"imagegraph.text.background_allocated_payload_bytes", composed.Pixels.size()
			);
			core::Metrics::Count("imagegraph.text.background_allocations", 1);
			const auto normal = [](const Rgba &source, const Rgba &destination) {
				Rgba result;
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = source[channel] * source[3] + destination[channel] * (1 - source[3]);
				return result;
			};
			const bool single = DescribeSurfaceFormat(row.Background->Format)->Channels == 1;
			for (uint32_t y = 0; y < row.Height; ++y)
				for (uint32_t x = 0; x < row.Width; ++x) {
					auto background =
						SampleNearest(*row.Background, (x + .5) / row.Width, (y + .5) / row.Height);
					if (single) background = {background[0], background[0], background[0], 1};
					if (!WritePixel(composed, x, y, normal(background, {})) ||
						!WritePixel(
							composed, x, y, normal(ReadPixel(output, x, y), ReadPixel(composed, x, y))
						))
						return c.Fail(
							Status::UnsupportedExecution,
							"Text background composition cannot represent finite pixels",
							"background"
						);
				}
			output.Pixels = std::move(composed.Pixels);
			return true;
		}
		bool DrawTextAtlas(NodeContext &c, FontTextRow &row, const Image &original) {
			if (row.Empty || !row.GenerateAtlas) return true;
			row.Atlas.Elements.reserve(row.AtlasDraws.size());
			for (size_t i = 0; i < row.AtlasDraws.size(); ++i) {
				const auto &draw = row.AtlasDraws[i];
				AtlasValue value;
				auto &atlas = value.Data.emplace();
				atlas.Surface.Data = {draw.Width, draw.Height, std::vector<uint8_t>(draw.Surface.Bytes)};
				FontTextRasterOptions options;
				options.Sampler = {1, 3};
				options.Blend = FontTextBlend::AlphaAdd;
				std::string failure;
				const auto status = DrawFontGlyphRaster(
					*row.SelectedFont.Font.Data,
					draw.Placement,
					options,
					draw.Footprint,
					atlas.Surface.Data,
					failure
				);
				if (status != Status::Ok) return c.Fail(status, std::move(failure), "draw_data");
				atlas.Surface.Data.Hash = SurfaceHash(atlas.Surface.Data);
				atlas.Position = row.Draws[i].Position;
				atlas.Dimension = {double(draw.Width), double(draw.Height)};
				atlas.OriginalDimension = {double(original.Width), double(original.Height)};
				atlas.OriginalSurface = SurfaceValue{original};
				core::Metrics::Count(
					"imagegraph.text.atlas_allocated_payload_bytes",
					draw.Surface.Bytes + original.Pixels.size()
				);
				core::Metrics::Count("imagegraph.text.atlas_allocations", 2);
				if (!ValidAtlasPayload(value))
					return c.Fail(Status::InvalidOutput, "Text Atlas candidate is invalid", "draw_data");
				row.Atlas.Elements.push_back(std::move(value));
			}
			return true;
		}
	}
	bool RenderFontTextRow(NodeContext &c, FontTextBatch &batch) {
		if (!batch.Admitted || c.ProcessorRow >= batch.Rows.size())
			return c.Fail(Status::InvalidValue, "Text render has no admitted row plan");
		auto &row = batch.Rows[c.ProcessorRow];
		auto charge = batch.OutputCharge.Split(row.OutputBytes);
		if (!charge || !c.OutputCharge.Merge(std::move(*charge)))
			return c.Fail(Status::InvalidValue, "Text row output reservation is unavailable");
		auto *output = c.NewImage("surface_out", row.Width, row.Height, row.Format);
		if (!output) return false;
		if (!row.Empty && row.ClearBackground) {
			const Rgba clear{
				row.BackgroundColour.Red / 255.0,
				row.BackgroundColour.Green / 255.0,
				row.BackgroundColour.Blue / 255.0,
				1
			};
			for (uint32_t y = 0; y < row.Height; ++y)
				for (uint32_t x = 0; x < row.Width; ++x)
					if (!WritePixel(*output, x, y, clear))
						return c.Fail(Status::InvalidOutput, "Text background clear cannot represent pixels");
		}
		std::string failure;
		for (size_t i = 0; i < row.Draws.size(); ++i) {
			const auto status = DrawFontGlyphRaster(
				*row.SelectedFont.Font.Data, row.Draws[i], row.Raster, row.Footprints[i], *output, failure
			);
			if (status != Status::Ok) return c.Fail(status, std::move(failure), "surface_out");
		}
		if (!row.Empty && !CompositeTextBackground(c, row, *output)) return false;
		output->Hash = SurfaceHash(*output);
		if (!DrawTextAtlas(c, row, *output)) return false;
		c.SetValue("draw_data", row.Atlas);
		if (!c.SetOutputDomain("draw_data", {ValueType::Array, std::nullopt, std::nullopt}) ||
			c.FailureCode != Status::Ok)
			return false;
		auto &state = std::get<StructValue>(batch.State.Values[0].Data);
		auto &rows = std::get<ArrayValue>(state.Data->Fields[2].second);
		auto &entry = std::get<StructValue>(rows.Elements[c.ProcessorRow]);
		entry.Data->Fields[1].second = std::move(row.Atlas);
		row.RetainedAtlasCharge.Reset();
		if (c.ProcessorRow + 1 == batch.Rows.size()) {
			if (!ValidRuntimeValue(batch.State.Values[0].Data))
				return c.Fail(Status::InvalidOutput, "Text retained state exceeds runtime value limits");
			const uint64_t retained = RetainedDataReplayEntryBytes(batch.State);
			if (retained > batch.StateCharge.Bytes())
				return c.Fail(Status::InvalidOutput, "Text retained state exceeded its admitted footprint");
			if (!c.OutputCharge.Merge(std::move(batch.StateCharge)))
				return c.Fail(Status::InvalidValue, "Text retained state reservation cannot transfer");
			c.DataUpdates.push_back(std::move(batch.State));
		}
		return c.FailureCode == Status::Ok;
	}
}
