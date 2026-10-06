#pragma once

#include <engine/imagegraph/Document.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph {
	struct SourceFontTextTransform {
		std::string Original, Transformed;
		uint8_t ChangeCase = 0;
		bool operator==(const SourceFontTextTransform &) const = default;
	};
	// Captured source font namespace and path prefixes. Core never scans font directories.
	enum class FontTextCaseProfile : uint8_t { RecordedOnly, UnicodeDefault };
	enum class FontBitmapTextureProfile : uint8_t { SourceObserved, NativeFrameUv };
	struct SourceFontInitialTextState {
		std::string NodeId;
		std::optional<FontValue> Primary, Fallback;
		bool operator==(const SourceFontInitialTextState &) const = default;
	};
	struct SourceFontContext {
		FontTextCaseProfile TextCaseProfile = FontTextCaseProfile::RecordedOnly;
		FontBitmapTextureProfile BitmapTextureProfile = FontBitmapTextureProfile::SourceObserved;
		bool AliasMapKnown = false;
		std::vector<std::pair<std::string, std::string>> Aliases;
		std::optional<std::string> Directory, ApplicationLocation, ProjectPath, DefaultFontPath;
		std::optional<FontValue> InitialFont;
		std::optional<bool> Playing;
		std::vector<SourceFontTextTransform> TextTransforms;
		// Exact per-instance retained fonts for a fresh held-playing evaluation.
		std::vector<SourceFontInitialTextState> InitialTextFonts;
		bool operator==(const SourceFontContext &) const = default;
	};
	struct SourceFontRequest {
		Node Authored;
		SourceFontContext Context;
		size_t ProcessorRow = 0;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		std::string Role, ResolvedPath;
		// bitmap_texture records bind the complete immutable native input; they never open files.
		std::optional<FontValue> FontInput;
		uint32_t PixelSize = 16;
		bool Antialias = false, SignedDistanceField = false;
		// Unique ordered Unicode scalars. Missing glyph presence must still be observed.
		std::vector<uint32_t> Characters;
		// Width/Height are zero in requests. Matching observations can provide source measurements.
		std::vector<FontMeasurement> Measurements;
		bool operator==(const SourceFontRequest &) const = default;
	};
	enum class SourceFontPresence : uint8_t { Present, AbsentFile };
	// A missing record differs from an observed absent file, which retains a prior source font.
	struct SourceFontObservation {
		SourceFontRequest Request;
		SourceFontPresence Presence = SourceFontPresence::Present;
		std::optional<FontValue> Font;
		bool operator==(const SourceFontObservation &) const = default;
	};
	// File decoding is an explicit process-local capability; recordings retain no provider pointer.
	class SourceFontProvider {
	  public:
		virtual ~SourceFontProvider() = default;
		// grug count owned provider storage live. Observe must admit growth beside its candidate.
		virtual uint64_t RetainedBytes() const {
			return 0;
		}
		virtual bool Observe(
			const SourceFontRequest &,
			uint64_t maximumOperationBytes,
			SourceFontObservation &,
			std::string &failure
		) = 0;
	};
	// Native full-text sizing uses real advances and line metrics, with bounded pinned wrap rules.
	// Exact matching source measurements take precedence. SourceObserved requires those records.
	// Borrowed inputs, prior result, candidate and workspace share maximumOperationBytes;
	// refusal preserves result. The work quote counts text scans and glyph/identity comparisons.
	inline constexpr uint64_t NativeFontMeasurementWorkPerByte = 32;
	// The pinned positive wrap branch can revisit a run of spaces. This checked
	// quote is also used by the Text batch before provider work or output pixels.
	std::optional<uint64_t> NativeSourceFontMeasurementWork(std::string_view text, double maximumLineWidth);
	Status MeasureNativeSourceFont(
		const FontValue &,
		std::span<const FontMeasurement> requests,
		uint64_t maximumOperationBytes,
		uint64_t maximumWorkUnits,
		std::vector<FontMeasurement> &result,
		std::string &failure
	);
	// Validates a borrowed complete font and measures its owned backing without cloning.
	std::optional<uint64_t> SourceFontValueRetainedBytes(const FontValue &);
	std::optional<uint64_t> SourceFontContextRetainedBytes(const SourceFontContext &);
	std::optional<uint64_t> SourceFontRequestRetainedBytes(const SourceFontRequest &);
	std::optional<uint64_t> SourceFontObservationRetainedBytes(const SourceFontObservation &);
	Status ValidateSourceFontObservations(
		const SourceFontContext *, std::span<const SourceFontObservation>, uint64_t maximumBytes, Diagnostic &
	);
}
