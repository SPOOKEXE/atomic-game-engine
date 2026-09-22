#pragma once

// Bounded, headless sampling for UI presentation animations.
//
// An animation is an authored value, while its sample is derived presentation
// state. The pure sampler never receives a Store and therefore cannot change a
// saved or replicated property. A compile host attaches the value through an
// `AnimationPlayback` modifier and supplies explicit seconds from its own
// clock.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::ecs {
	class Store;
}

namespace engine::gui {

	// The small visual and layout property allowlist a presentation track may
	// target. A class property name is deliberately not accepted here: sampling
	// an arbitrary member would turn a visual layer into an alternate authored
	// write path.
	enum class PresentationProperty : uint8_t {
		BackgroundColor,
		BackgroundTransparency,
		ImageColor,
		ImageTransparency,
		TextColor,
		TextTransparency,
		Rotation,
		Position,
		Size,
	};

	enum class PresentationValueType : uint8_t {
		Color,
		Number,
		UDim2,
	};

	// Checks an authored target enum before a sampler uses it as an allowlist
	// key. Saved bytes may contain an out-of-range ordinal.
	bool ValidPresentationProperty(PresentationProperty property);

	// Whether a sampled property requires its target subtree to be laid out
	// again. Other allowlisted properties only affect compiled presentation.
	bool PresentationAffectsLayout(PresentationProperty property);

	// A typed track value. Only the field selected by Type is meaningful.
	struct PresentationValue {
		PresentationValueType Type = PresentationValueType::Number;
		core::Color3 Color{};
		float Number = 0.0f;
		core::UDim2 UDim2{};

		static PresentationValue FromColor(core::Color3 value);
		static PresentationValue FromNumber(float value);
		static PresentationValue FromUDim2(core::UDim2 value);
	};

	// One normalized key on a presentation track.
	struct PresentationKey {
		float Time = 0.0f;
		PresentationValue Value;
	};

	// A typed, bounded track. Keys are monotonic normalized values in [0, 1].
	class PresentationTrack {
	  public:
		static constexpr size_t MAXIMUM_KEYS = 16;

		PresentationProperty Property = PresentationProperty::BackgroundColor;

		// Refuses a value with the wrong property type, non-finite data, a time
		// outside [0, 1], a decreasing time, or a key beyond the fixed capacity.
		bool Add(PresentationKey key);
		std::span<const PresentationKey> Keys() const;

	  private:
		std::array<PresentationKey, MAXIMUM_KEYS> Values{};
		size_t Count = 0;
	};

	struct AnimationMarker {
		core::Name Name;
		float Time = 0.0f;
	};

	// An authored presentation clip. It remains a standalone value so the
	// `AnimationPlayback` attachment can serialize it without turning sampled
	// overrides into authoritative component fields.
	class UIAnimation {
	  public:
		static constexpr size_t MAXIMUM_TRACKS = 16;
		static constexpr size_t MAXIMUM_MARKERS = 16;
		static constexpr size_t MAXIMUM_MARKER_NAME_BYTES = 64;

		core::TweenInfo Tween;

		bool AddTrack(const PresentationTrack &track);
		bool AddMarker(AnimationMarker marker);
		std::span<const PresentationTrack> Tracks() const;
		std::span<const AnimationMarker> Markers() const;

	  private:
		std::array<PresentationTrack, MAXIMUM_TRACKS> TrackValues{};
		std::array<AnimationMarker, MAXIMUM_MARKERS> MarkerValues{};
		size_t TrackCount = 0;
		size_t MarkerCount = 0;
	};

	struct PresentationOverride {
		PresentationProperty Property = PresentationProperty::BackgroundColor;
		PresentationValue Value;
	};

	// The bounded result passed to a compiler or renderer-facing presentation
	// adapter. It is rebuilt from the clip each sample and never stored in ECS.
	class PresentationOverrides {
	  public:
		static constexpr size_t MAXIMUM_OVERRIDES = UIAnimation::MAXIMUM_TRACKS;

		bool Add(PresentationOverride override);
		const PresentationOverride *Find(PresentationProperty property) const;
		std::span<const PresentationOverride> Values() const;

	  private:
		std::array<PresentationOverride, MAXIMUM_OVERRIDES> Entries{};
		size_t Count = 0;
	};

	// Samples a clip at explicit seconds. Returns false for invalid time or a
	// malformed tween. A finite tween holds its terminal value; an endless tween
	// repeats according to TweenInfo without reading a wall clock.
	bool SamplePresentation(const UIAnimation &animation, double seconds, PresentationOverrides &out);

	// Whether a valid clip can produce a different sample at a later explicit
	// time. This lets a retained compiler include time in its cache key only
	// while a clip is scheduled or in flight.
	bool PresentationIsMoving(const UIAnimation &animation, double seconds);

	// Samples every authored animation modifier below `scope`. A null scope
	// means the whole store. The sample is local derived state and is rebuilt
	// before layout or compilation reads it.
	void AdvancePresentationAnimations(ecs::Store &store, double seconds, ecs::Entity scope = {});
}
