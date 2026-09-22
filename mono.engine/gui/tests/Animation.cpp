#include <engine/gui/Animation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_SUITE_ID("engine.gui.animation")

using Catch::Approx;
using engine::core::Color3;
using engine::core::EasingDirection;
using engine::core::EasingStyle;
using engine::core::TweenInfo;
using engine::core::UDim2;
using namespace engine::gui;

namespace {
	PresentationTrack NumberTrack(PresentationProperty property, float from, float to) {
		PresentationTrack track;
		track.Property = property;
		REQUIRE(track.Add(PresentationKey{0.0f, PresentationValue::FromNumber(from)}));
		REQUIRE(track.Add(PresentationKey{1.0f, PresentationValue::FromNumber(to)}));
		return track;
	}

	const PresentationOverride &Only(const PresentationOverrides &overrides) {
		REQUIRE(overrides.Values().size() == 1);
		return overrides.Values().front();
	}
}

TEST_CASE(
	"presentation sampling is explicit, eased, and does not mutate authored tracks", "[gui][animation]"
) {
	UIAnimation animation;
	animation.Tween = TweenInfo(2.0f, EasingStyle::Linear, EasingDirection::In);
	REQUIRE(animation.AddTrack(NumberTrack(PresentationProperty::Rotation, 10.0f, 50.0f)));

	PresentationOverrides early;
	REQUIRE(SamplePresentation(animation, 0.5, early));
	CHECK(Only(early).Value.Number == Approx(20.0f));

	PresentationOverrides late;
	REQUIRE(SamplePresentation(animation, 1.5, late));
	CHECK(Only(late).Value.Number == Approx(40.0f));
	CHECK(animation.Tracks().front().Keys().front().Value.Number == Approx(10.0f));
	CHECK(animation.Tracks().front().Keys().back().Value.Number == Approx(50.0f));
}

TEST_CASE("presentation sampling holds terminal values and handles reversed repeats", "[gui][animation]") {
	UIAnimation animation;
	animation.Tween = TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, 1, true);
	REQUIRE(animation.AddTrack(NumberTrack(PresentationProperty::BackgroundTransparency, 0.0f, 1.0f)));

	PresentationOverrides forward;
	REQUIRE(SamplePresentation(animation, 0.25, forward));
	CHECK(Only(forward).Value.Number == Approx(0.25f));

	PresentationOverrides reverse;
	REQUIRE(SamplePresentation(animation, 1.25, reverse));
	CHECK(Only(reverse).Value.Number == Approx(0.75f));

	PresentationOverrides finished;
	REQUIRE(SamplePresentation(animation, 8.0, finished));
	CHECK(Only(finished).Value.Number == Approx(0.0f));
}

TEST_CASE("presentation tracks enforce a bounded typed authored format", "[gui][animation]") {
	CHECK_FALSE(ValidPresentationProperty(static_cast<PresentationProperty>(255)));
	CHECK(PresentationAffectsLayout(PresentationProperty::Position));
	CHECK(PresentationAffectsLayout(PresentationProperty::Size));
	CHECK_FALSE(PresentationAffectsLayout(PresentationProperty::Rotation));

	PresentationTrack position;
	position.Property = PresentationProperty::Position;
	CHECK_FALSE(position.Add(PresentationKey{0.0f, PresentationValue::FromNumber(1.0f)}));
	REQUIRE(position.Add(PresentationKey{0.0f, PresentationValue::FromUDim2(UDim2{0.0f, 0.0f, 0.0f, 0.0f})}));
	CHECK_FALSE(position.Add(
		PresentationKey{
			0.5f,
			PresentationValue::FromUDim2(UDim2{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f})
		}
	));
	CHECK_FALSE(position.Add(PresentationKey{-0.1f, PresentationValue::FromUDim2(UDim2{})}));
	UIAnimation animation;
	REQUIRE(animation.AddTrack(position));
	CHECK_FALSE(animation.AddTrack(position));
	PresentationTrack mutated = NumberTrack(PresentationProperty::Rotation, 0.0f, 1.0f);
	mutated.Property = PresentationProperty::Position;
	CHECK_FALSE(animation.AddTrack(mutated));
	mutated.Property = static_cast<PresentationProperty>(255);
	CHECK_FALSE(animation.AddTrack(mutated));

	PresentationTrack colour;
	colour.Property = PresentationProperty::TextColor;
	for (size_t index = 0; index < PresentationTrack::MAXIMUM_KEYS; index++) {
		REQUIRE(colour.Add(
			PresentationKey{
				static_cast<float>(index) / static_cast<float>(PresentationTrack::MAXIMUM_KEYS - 1),
				PresentationValue::FromColor(Color3{static_cast<float>(index), 0.0f, 0.0f}),
			}
		));
	}
	CHECK_FALSE(colour.Add(PresentationKey{1.0f, PresentationValue::FromColor(Color3{})}));
}

TEST_CASE("presentation markers are bounded metadata and sampling rejects invalid time", "[gui][animation]") {
	UIAnimation animation;
	animation.Tween = TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In);
	REQUIRE(animation.AddTrack(NumberTrack(PresentationProperty::Rotation, 0.0f, 1.0f)));
	REQUIRE(animation.AddMarker(AnimationMarker{engine::core::Name("half"), 0.5f}));
	CHECK_FALSE(animation.AddMarker(AnimationMarker{engine::core::Name("early"), 0.25f}));
	CHECK_FALSE(animation.AddMarker(AnimationMarker{engine::core::Name("half"), 0.75f}));
	CHECK_FALSE(animation.AddMarker(
		AnimationMarker{
			engine::core::Name(std::string(UIAnimation::MAXIMUM_MARKER_NAME_BYTES + 1, 'x')), 0.75f
		}
	));
	CHECK(animation.Markers().front().Name == engine::core::Name("half"));

	PresentationOverrides sampled;
	CHECK_FALSE(SamplePresentation(animation, std::numeric_limits<double>::infinity(), sampled));
	CHECK_FALSE(SamplePresentation(animation, -1.0, sampled));
	animation.Tween.Style = static_cast<EasingStyle>(255);
	CHECK_FALSE(SamplePresentation(animation, 0.5, sampled));
}

TEST_CASE("endless presentation repeats do not narrow extreme finite elapsed time", "[gui][animation]") {
	UIAnimation animation;
	animation.Tween = TweenInfo(1.0f, EasingStyle::Linear, EasingDirection::In, -1, true);
	REQUIRE(animation.AddTrack(NumberTrack(PresentationProperty::Rotation, 0.0f, 1.0f)));

	PresentationOverrides sampled;
	REQUIRE(SamplePresentation(animation, std::numeric_limits<double>::max(), sampled));
	CHECK(std::isfinite(Only(sampled).Value.Number));
}
