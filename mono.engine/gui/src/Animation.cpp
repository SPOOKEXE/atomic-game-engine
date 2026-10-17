#include <engine/ecs/Store.hpp>
#include <engine/gui/Animation.hpp>
#include <engine/gui/Components.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::gui {

	namespace {
		PresentationValueType TypeFor(PresentationProperty property) {
			switch (property) {
			case PresentationProperty::BackgroundColor:
			case PresentationProperty::ImageColor:
			case PresentationProperty::TextColor:
				return PresentationValueType::Color;
			case PresentationProperty::Position:
			case PresentationProperty::Size:
				return PresentationValueType::UDim2;
			case PresentationProperty::BackgroundTransparency:
			case PresentationProperty::ImageTransparency:
			case PresentationProperty::TextTransparency:
			case PresentationProperty::Rotation:
				return PresentationValueType::Number;
			}
			return PresentationValueType::Number;
		}

		bool ValidValueType(PresentationValueType type) {
			return type == PresentationValueType::Color || type == PresentationValueType::Number ||
				   type == PresentationValueType::UDim2;
		}

		bool ValidTween(const core::TweenInfo &tween) {
			return tween.Style <= core::EasingStyle::Bounce &&
				   tween.Direction <= core::EasingDirection::InOut;
		}

		bool Finite(const PresentationValue &value) {
			switch (value.Type) {
			case PresentationValueType::Color:
				return std::isfinite(value.Color.R) && std::isfinite(value.Color.G) &&
					   std::isfinite(value.Color.B);
			case PresentationValueType::Number:
				return std::isfinite(value.Number);
			case PresentationValueType::UDim2:
				return std::isfinite(value.UDim2.X.Scale) && std::isfinite(value.UDim2.X.Offset) &&
					   std::isfinite(value.UDim2.Y.Scale) && std::isfinite(value.UDim2.Y.Offset);
			}
			return false;
		}

		PresentationValue Blend(const PresentationValue &from, const PresentationValue &to, float alpha) {
			PresentationValue result = from;
			switch (from.Type) {
			case PresentationValueType::Color:
				result.Color = core::Color3{
					from.Color.R + (to.Color.R - from.Color.R) * alpha,
					from.Color.G + (to.Color.G - from.Color.G) * alpha,
					from.Color.B + (to.Color.B - from.Color.B) * alpha,
				};
				break;
			case PresentationValueType::Number:
				result.Number = from.Number + (to.Number - from.Number) * alpha;
				break;
			case PresentationValueType::UDim2:
				result.UDim2 = core::UDim2{
					from.UDim2.X.Scale + (to.UDim2.X.Scale - from.UDim2.X.Scale) * alpha,
					from.UDim2.X.Offset + (to.UDim2.X.Offset - from.UDim2.X.Offset) * alpha,
					from.UDim2.Y.Scale + (to.UDim2.Y.Scale - from.UDim2.Y.Scale) * alpha,
					from.UDim2.Y.Offset + (to.UDim2.Y.Offset - from.UDim2.Y.Offset) * alpha,
				};
				break;
			}
			return result;
		}

		bool Progress(const core::TweenInfo &tween, double seconds, float &out) {
			if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(tween.Time) ||
				!std::isfinite(tween.DelayTime) || tween.Time <= 0.0f || tween.DelayTime < 0.0f) {
				return false;
			}
			if (seconds <= static_cast<double>(tween.DelayTime)) {
				out = 0.0f;
				return true;
			}

			const double elapsed = seconds - static_cast<double>(tween.DelayTime);
			const double length = static_cast<double>(tween.Time);
			const bool endless = tween.RepeatCount < 0;
			const uint64_t passes = endless ? 0 : static_cast<uint64_t>(tween.RepeatCount) + 1;
			bool reversed = false;
			double local = elapsed;
			if (endless) {
				local = std::fmod(elapsed, length);
				reversed = std::fmod(std::floor(elapsed / length), 2.0) != 0.0;
			} else if (elapsed >= length * static_cast<double>(passes)) {
				reversed = tween.Reverses && ((passes - 1) & 1U) != 0;
				local = length;
			} else {
				const uint64_t pass = static_cast<uint64_t>(std::floor(elapsed / length));
				reversed = tween.Reverses && (pass & 1U) != 0;
				local -= static_cast<double>(pass) * length;
			}

			float alpha = static_cast<float>(local / length);
			if (tween.Reverses && reversed) {
				alpha = 1.0f - alpha;
			}
			out = tween.Evaluate(alpha);
			return true;
		}

		bool SampleTrack(const PresentationTrack &track, float alpha, PresentationValue &out) {
			const std::span<const PresentationKey> keys = track.Keys();
			if (keys.empty()) {
				return false;
			}
			if (alpha <= keys.front().Time || keys.size() == 1) {
				out = keys.front().Value;
				return true;
			}
			for (size_t index = 1; index < keys.size(); index++) {
				if (alpha > keys[index].Time) {
					continue;
				}
				const PresentationKey &before = keys[index - 1];
				const PresentationKey &after = keys[index];
				const float span = after.Time - before.Time;
				out = span <= 0.0f ? after.Value
								   : Blend(before.Value, after.Value, (alpha - before.Time) / span);
				return true;
			}
			out = keys.back().Value;
			return true;
		}

		bool Same(const PresentationValue &left, const PresentationValue &right) {
			return left.Type == right.Type && left.Color.R == right.Color.R &&
				   left.Color.G == right.Color.G && left.Color.B == right.Color.B &&
				   left.Number == right.Number && left.UDim2.X.Scale == right.UDim2.X.Scale &&
				   left.UDim2.X.Offset == right.UDim2.X.Offset && left.UDim2.Y.Scale == right.UDim2.Y.Scale &&
				   left.UDim2.Y.Offset == right.UDim2.Y.Offset;
		}

		bool Same(const PresentationOverrides &left, const PresentationOverrides &right) {
			const std::span<const PresentationOverride> before = left.Values();
			const std::span<const PresentationOverride> after = right.Values();
			if (before.size() != after.size()) {
				return false;
			}
			for (size_t index = 0; index < before.size(); index++) {
				if (before[index].Property != after[index].Property ||
					!Same(before[index].Value, after[index].Value)) {
					return false;
				}
			}
			return true;
		}

		bool Same(const PresentationState &left, const PresentationState &right) {
			return left.Active == right.Active && left.Moving == right.Moving &&
				   Same(left.Overrides, right.Overrides);
		}
	}

	bool ValidPresentationProperty(PresentationProperty property) {
		return property <= PresentationProperty::Size;
	}

	bool PresentationAffectsLayout(PresentationProperty property) {
		return property == PresentationProperty::Position || property == PresentationProperty::Size;
	}

	PresentationValue PresentationValue::FromColor(core::Color3 value) {
		PresentationValue result;
		result.Type = PresentationValueType::Color;
		result.Color = value;
		return result;
	}

	PresentationValue PresentationValue::FromNumber(float value) {
		PresentationValue result;
		result.Type = PresentationValueType::Number;
		result.Number = value;
		return result;
	}

	PresentationValue PresentationValue::FromUDim2(core::UDim2 value) {
		PresentationValue result;
		result.Type = PresentationValueType::UDim2;
		result.UDim2 = value;
		return result;
	}

	bool PresentationTrack::Add(PresentationKey key) {
		if (Count == Values.size() || !ValidPresentationProperty(Property) ||
			!ValidValueType(key.Value.Type) || key.Value.Type != TypeFor(Property) ||
			!std::isfinite(key.Time) || key.Time < 0.0f || key.Time > 1.0f || !Finite(key.Value) ||
			(Count > 0 && key.Time < Values[Count - 1].Time)) {
			return false;
		}
		Values[Count++] = key;
		return true;
	}

	std::span<const PresentationKey> PresentationTrack::Keys() const {
		return std::span<const PresentationKey>(Values.data(), Count);
	}

	bool UIAnimation::AddTrack(const PresentationTrack &track) {
		const std::span<const PresentationKey> keys = track.Keys();
		if (TrackCount == TrackValues.size() || keys.empty() || !ValidPresentationProperty(track.Property)) {
			return false;
		}
		float previous = 0.0f;
		for (size_t index = 0; index < keys.size(); index++) {
			const PresentationKey &key = keys[index];
			if (!ValidValueType(key.Value.Type) || key.Value.Type != TypeFor(track.Property) ||
				!std::isfinite(key.Time) || key.Time < 0.0f || key.Time > 1.0f || !Finite(key.Value) ||
				(index > 0 && key.Time < previous)) {
				return false;
			}
			previous = key.Time;
		}
		if (std::any_of(
				TrackValues.begin(),
				TrackValues.begin() + static_cast<ptrdiff_t>(TrackCount),
				[&](const PresentationTrack &existing) { return existing.Property == track.Property; }
			)) {
			return false;
		}
		TrackValues[TrackCount++] = track;
		return true;
	}

	bool UIAnimation::AddMarker(AnimationMarker marker) {
		if (MarkerCount == MarkerValues.size() || !marker.Name.IsValid() ||
			marker.Name.Text().size() > MAXIMUM_MARKER_NAME_BYTES || !std::isfinite(marker.Time) ||
			marker.Time < 0.0f || marker.Time > 1.0f ||
			(MarkerCount > 0 && marker.Time < MarkerValues[MarkerCount - 1].Time)) {
			return false;
		}
		if (std::any_of(
				MarkerValues.begin(),
				MarkerValues.begin() + static_cast<ptrdiff_t>(MarkerCount),
				[&](const AnimationMarker &existing) { return existing.Name == marker.Name; }
			)) {
			return false;
		}
		MarkerValues[MarkerCount++] = marker;
		return true;
	}

	std::span<const PresentationTrack> UIAnimation::Tracks() const {
		return std::span<const PresentationTrack>(TrackValues.data(), TrackCount);
	}

	std::span<const AnimationMarker> UIAnimation::Markers() const {
		return std::span<const AnimationMarker>(MarkerValues.data(), MarkerCount);
	}

	bool PresentationOverrides::Add(PresentationOverride override) {
		if (Count == Entries.size()) {
			return false;
		}
		Entries[Count++] = override;
		return true;
	}

	const PresentationOverride *PresentationOverrides::Find(PresentationProperty property) const {
		const auto found = std::find_if(
			Entries.begin(),
			Entries.begin() + static_cast<ptrdiff_t>(Count),
			[property](const PresentationOverride &entry) { return entry.Property == property; }
		);
		return found == Entries.begin() + static_cast<ptrdiff_t>(Count) ? nullptr : &*found;
	}

	std::span<const PresentationOverride> PresentationOverrides::Values() const {
		return std::span<const PresentationOverride>(Entries.data(), Count);
	}

	bool SamplePresentation(const UIAnimation &animation, double seconds, PresentationOverrides &out) {
		float progress = 0.0f;
		if (!ValidTween(animation.Tween) || !Progress(animation.Tween, seconds, progress)) {
			return false;
		}

		PresentationOverrides sampled;
		for (const PresentationTrack &track : animation.Tracks()) {
			PresentationValue value;
			if (SampleTrack(track, progress, value) &&
				(!Finite(value) || !sampled.Add(PresentationOverride{track.Property, value}))) {
				return false;
			}
		}
		out = sampled;
		return true;
	}

	bool PresentationIsMoving(const UIAnimation &animation, double seconds) {
		if (!std::isfinite(seconds) || seconds < 0.0 || animation.Tracks().empty() ||
			!ValidTween(animation.Tween)) {
			return false;
		}

		if (animation.Tween.RepeatCount < 0) {
			return true;
		}
		const uint64_t passes = static_cast<uint64_t>(animation.Tween.RepeatCount) + 1;
		const double terminal = static_cast<double>(animation.Tween.DelayTime) +
								static_cast<double>(animation.Tween.Time) * static_cast<double>(passes);
		return seconds < terminal;
	}

	void AdvancePresentationAnimations(ecs::Store &store, double seconds, ecs::Entity scope) {
		struct Pending {
			ecs::Entity Entity;
			PresentationState State;
			bool Restart = false;
			bool WriteState = false;
		};

		std::vector<Pending> pending;
		store.Each<const AnimationPlayback>([&](ecs::Entity entity, const AnimationPlayback &playback) {
			if (scope != ecs::NULL_ENTITY && entity != scope && !store.IsDescendantOf(entity, scope)) {
				return;
			}

			PresentationState next;
			bool restart = false;
			if (playback.Playing && std::isfinite(seconds) && std::isfinite(playback.StartedAt)) {
				const double startedAt = playback.StartedAt < 0.0 ? seconds : playback.StartedAt;
				const double elapsed = seconds - startedAt;
				const double sampleAt = std::max(elapsed, 0.0);
				next.Active = SamplePresentation(playback.Clip, sampleAt, next.Overrides);
				next.Moving = next.Active && (elapsed < 0.0 || PresentationIsMoving(playback.Clip, sampleAt));
				restart = playback.StartedAt < 0.0;
			}

			const PresentationState *current = store.Get<PresentationState>(entity);
			if (restart || current == nullptr || !Same(*current, next)) {
				pending.push_back(
					Pending{entity, next, restart, current == nullptr || !Same(*current, next)}
				);
			}
		});

		for (const Pending &change : pending) {
			if (change.Restart) {
				AnimationPlayback *playback = store.GetMutable<AnimationPlayback>(change.Entity);
				if (playback != nullptr) {
					playback->StartedAt = seconds;
				}
			}
			if (change.WriteState) {
				store.Set(change.Entity, change.State);
			}
		}
	}
}
