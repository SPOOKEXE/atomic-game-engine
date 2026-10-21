#include "FrameObservation.hpp"
#include "ImageChecks.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/net/Transport.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Scenario.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <client/Client.hpp>
#include <client/Replicated.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

TEST_SUITE_ID("client.portalwalk")
TEST_DEPENDS("client.presentationhost")
TEST_DEPENDS("client.portalsession")
TEST_DEPENDS("engine.testing.scenario")

enum class RoomShader { None, Material, Spatial, SpatialOverlay, Lens };
enum class PortalWalkFault { None, Delay, RestartDestinationPresentation, DropAcknowledgement };

static bool ReadyPortalDemand(const nlohmann::json &portal) {
	return portal.value("demand_status", "ready") == "ready";
}

// The old-world trailing view deliberately shows the admitted body through the pane.
// Its display profile differs from a primary eye, even while input stays first-person.
static bool IsTrailingSourceView(const nlohmann::json &sample, const nlohmann::json &previous) {
	try {
		const auto &input = sample.at("input_world").get_ref<const std::string &>();
		const auto &view = sample.at("view_world").get_ref<const std::string &>();
		const auto &retained = sample.at("retained_world").get_ref<const std::string &>();
		const auto &observed = sample.at("observed_world").get_ref<const std::string &>();
		const auto &eye = sample.at("eye_world").get_ref<const std::string &>();
		const auto &previousInput = previous.at("input_world").get_ref<const std::string &>();
		const auto &previousRetained = previous.at("retained_world").get_ref<const std::string &>();
		return !input.empty() && !view.empty() && !observed.empty() && !eye.empty() &&
			   sample.at("view_slot").is_number_integer() && sample.at("view_slot") == 5 &&
			   !sample.at("observed_successor").get<bool>() && view == retained && view != input &&
			   observed != eye && !sample.at("eye_image").get<bool>() &&
			   sample.at("eye_route_hops").is_number_integer() && sample.at("eye_route_hops") == 0 &&
			   previousInput == input && previousRetained == retained;
	} catch (const nlohmann::json::exception &) {
		return false;
	}
}

TEST_CASE(
	"[client] trailing source display classification preserves primary eye checks",
	"[client][portal-trailing-source]"
) {
	const nlohmann::json previous{
		{"input_world", "client.portal.approach.1"}, {"retained_world", "client.replica"}
	};
	const nlohmann::json trailing{
		{"view_slot", 5},
		{"observed_successor", false},
		{"view_world", "client.replica"},
		{"retained_world", "client.replica"},
		{"input_world", "client.portal.approach.1"},
		{"observed_world", "server.world"},
		{"eye_world", "walk.destination"},
		{"eye_image", false},
		{"eye_route_hops", 0}
	};
	CHECK(IsTrailingSourceView(trailing, previous));
	auto changed = trailing;
	changed["eye_player"] = 1;
	changed["eye_rig"] = 4294967325ULL;
	CHECK(IsTrailingSourceView(changed, previous));
	changed["eye_player"] = "invalid identity";
	changed["eye_rig"] = nullptr;
	CHECK(IsTrailingSourceView(changed, previous));
	for (const auto &patch :
		 {nlohmann::json{{"view_slot", 0}},
		  {{"observed_successor", true}},
		  {{"view_world", "client.portal.approach.1"}},
		  {{"retained_world", "other"}},
		  {{"eye_world", "server.world"}},
		  {{"eye_image", true}},
		  {{"eye_route_hops", 1}},
		  {{"observed_world", ""}},
		  {{"view_slot", 5.5}},
		  {{"observed_successor", "false"}}}) {
		changed = trailing;
		changed.update(patch);
		CHECK_FALSE(IsTrailingSourceView(changed, previous));
	}
	// The ordinary native views before the trailing interval have no observed world.
	changed = trailing;
	changed["view_slot"] = 0;
	changed["view_world"] = "client.portal.approach.1";
	changed.erase("observed_world");
	changed.erase("observed_successor");
	CHECK_FALSE(IsTrailingSourceView(changed, previous));
	// A primary observation can keep slot five after the eye crosses into its world.
	changed = trailing;
	changed["eye_world"] = "server.world";
	changed["eye_route_hops"] = 1;
	CHECK_FALSE(IsTrailingSourceView(changed, previous));
	for (const auto &entry : trailing.items()) {
		changed = trailing;
		changed.erase(entry.key());
		CHECK_FALSE(IsTrailingSourceView(changed, previous));
	}
	for (const char *field : {"input_world", "retained_world"}) {
		auto changedPrevious = previous;
		changedPrevious[field] = "adoption changed this world";
		CHECK_FALSE(IsTrailingSourceView(trailing, changedPrevious));
		changedPrevious.erase(field);
		CHECK_FALSE(IsTrailingSourceView(trailing, changedPrevious));
	}
}

static engine::core::CFrame JsonCameraFrame(const nlohmann::json &pose) {
	const auto &position = pose.at("position");
	const auto &rotation = pose.at("rotation");
	engine::core::CFrame camera{
		{position.at(0).get<float>(), position.at(1).get<float>(), position.at(2).get<float>()}
	};
	camera.QuaternionX = rotation.at(0).get<float>();
	camera.QuaternionY = rotation.at(1).get<float>();
	camera.QuaternionZ = rotation.at(2).get<float>();
	camera.QuaternionW = rotation.at(3).get<float>();
	return camera;
}

static engine::core::Vector3 JsonVector(const nlohmann::json &value) {
	return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

// A camera alone may cross a pane while its followed body remains behind it.
// Only an intersection inside this fixture's finite aperture excuses that split
// view. Map the followed head into the displayed eye and portal-view space.
static bool FollowingArmCrossesPortal(const nlohmann::json &sample) {
	using namespace engine;
	const scene::SeamTransform eyeFromInput{
		.Frame = JsonCameraFrame(sample.at("eye_from_input")),
		.Origin = JsonVector(sample.at("eye_origin")),
		.Scale = sample.at("eye_scale").get<float>()
	};
	const auto root = JsonCameraFrame(sample.at("presented_predicted_root"));
	const auto head = eyeFromInput.Point(root.Position + JsonVector(sample.at("head_offset")));
	const auto eye = JsonCameraFrame(sample.at("camera")).Position;
	for (const auto &portal : sample.at("portal_views")) {
		const auto centre = JsonVector(portal.at("centre"));
		const auto normal = JsonVector(portal.at("normal"));
		const float headSide = (head - centre).Dot(normal);
		const float eyeSide = (eye - centre).Dot(normal);
		if ((headSide < 0) == (eyeSide < 0)) continue;
		const float difference = headSide - eyeSide;
		if (std::abs(difference) < 1e-6f) continue;
		const float along = headSide / difference;
		if (along < 0 || along > 1) continue;
		const auto offset = head + (eye - head) * along - centre;
		const auto first = JsonVector(portal.at("first"));
		const auto second = JsonVector(portal.at("second"));
		if (std::abs(offset.Dot(first)) <= first.Dot(first) &&
			std::abs(offset.Dot(second)) <= second.Dot(second))
			return true;
	}
	return false;
}

// Lit skin can fall below the bright-pixel oracle while the default rig turns.
// Accept its chromaticity only where the accepted eye projects that rig's head.
static bool HasProjectedHeadSkin(SDL_Surface *image, const nlohmann::json &sample) {
	using namespace engine;
	if (!image || image->w <= 0 || image->h <= 0) return false;
	try {
		const auto &capture = sample.at("eye_capture");
		if (!sample.at("eye_image").get<bool>() || sample.at("eye_composed").get<bool>() ||
			capture.at("scope") != "complete-world" || capture.at("projection") != "eye" ||
			capture.at("width") != image->w || capture.at("height") != image->h ||
			capture.at("image") != sample.at("eye_image_handle") ||
			capture.at("image").get<uint64_t>() == 0 || capture.at("frustum").size() != 6)
			return false;
		const auto root = JsonCameraFrame(sample.at("presented_predicted_root"));
		const scene::SeamTransform warp{
			.Frame = JsonCameraFrame(sample.at("eye_from_input")),
			.Origin = JsonVector(sample.at("eye_origin")),
			.Scale = sample.at("eye_scale").get<float>()
		};
		const auto camera =
			JsonCameraFrame({{"position", capture.at("position")}, {"rotation", capture.at("orientation")}});
		const auto validRotation = [](const core::CFrame &frame) {
			const float norm = frame.QuaternionX * frame.QuaternionX + frame.QuaternionY * frame.QuaternionY +
							   frame.QuaternionZ * frame.QuaternionZ + frame.QuaternionW * frame.QuaternionW;
			return std::isfinite(norm) && std::abs(norm - 1.0f) < .001f;
		};
		const auto frustum = capture.at("frustum").get<std::array<float, 6>>();
		if (!validRotation(root) || !validRotation(warp.Frame) || !validRotation(camera) ||
			!std::isfinite(warp.Scale) || warp.Scale <= 0 ||
			!std::all_of(frustum.begin(), frustum.end(), [](float value) { return std::isfinite(value); }) ||
			frustum[1] <= frustum[0] || frustum[3] <= frustum[2] || frustum[4] <= 0 ||
			frustum[5] <= frustum[4])
			return false;
		float left = float(image->w), right = 0, top = float(image->h), bottom = 0;
		// Characters.cpp defines the default head as 1.5 x 1 x 1.5, centered two metres above the root.
		for (float x : {-.75f, .75f})
			for (float y : {1.5f, 2.5f})
				for (float z : {-.75f, .75f}) {
					const auto local =
						camera.PointToObjectSpace(warp.Point(root.PointToWorldSpace({x, y, z})));
					const float depth = -local.Z;
					if (!std::isfinite(depth) || depth < frustum[4] || depth > frustum[5]) return false;
					const float scale = frustum[4] / depth;
					const float pixelX =
						(local.X * scale - frustum[0]) / (frustum[1] - frustum[0]) * image->w;
					const float pixelY =
						(1 - (local.Y * scale - frustum[2]) / (frustum[3] - frustum[2])) * image->h;
					if (!std::isfinite(pixelX) || !std::isfinite(pixelY)) return false;
					left = std::min(left, pixelX);
					right = std::max(right, pixelX);
					top = std::min(top, pixelY);
					bottom = std::max(bottom, pixelY);
				}
		const int firstX = int(std::floor(std::clamp(left, 0.0f, float(image->w))));
		const int lastX = int(std::ceil(std::clamp(right, 0.0f, float(image->w))));
		const int firstY = int(std::floor(std::clamp(top, 0.0f, float(image->h))));
		const int lastY = int(std::ceil(std::clamp(bottom, 0.0f, float(image->h))));
		for (int y = firstY; y < lastY; ++y)
			for (int x = firstX; x < lastX; ++x) {
				Uint8 red = 0, green = 0, blue = 0, alpha = 0;
				if (!SDL_ReadSurfacePixel(image, x, y, &red, &green, &blue, &alpha)) return false;
				if (green > 0 && 10 * red > 7 * green && 10 * red < 14 * green && 2 * blue < green)
					return true;
			}
	} catch (const nlohmann::json::exception &) {
		return false;
	}
	return false;
}

TEST_CASE("[client] dark portal skin requires the projected head", "[client][portal-skin-oracle]") {
	const nlohmann::json pose{{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}};
	nlohmann::json sample{
		{"presented_predicted_root", {{"position", {0, 0, -10}}, {"rotation", {0, 0, 0, 1}}}},
		{"eye_from_input", pose},
		{"eye_origin", {0, 0, 0}},
		{"eye_scale", 1},
		{"eye_image", true},
		{"eye_composed", false},
		{"eye_image_handle", 1},
		{"eye_capture",
		 {{"position", {0, 0, 0}},
		  {"orientation", {0, 0, 0, 1}},
		  {"frustum", {-.1, .1, -.1, .1, .1, 500}},
		  {"width", 128},
		  {"height", 128},
		  {"scope", "complete-world"},
		  {"projection", "eye"},
		  {"image", 1}}}
	};
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
		SDL_CreateSurface(128, 128, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface
	);
	REQUIRE(image);
	const auto fill = [&](SDL_Rect *rectangle, Uint8 red, Uint8 green, Uint8 blue) {
		REQUIRE(SDL_FillSurfaceRect(
			image.get(), rectangle, SDL_MapSurfaceRGBA(image.get(), red, green, blue, 255)
		));
	};
	for (const auto color :
		 {std::array<Uint8, 3>{0, 0, 0}, {59, 84, 170}, {100, 100, 100}, {33, 84, 153}, {170, 70, 20}}) {
		fill(nullptr, color[0], color[1], color[2]);
		CHECK_FALSE(HasProjectedHeadSkin(image.get(), sample));
	}
	fill(nullptr, 59, 84, 170);
	// Independent pinhole projection puts the head around x=64, y=51.
	SDL_Rect headPixel{64, 51, 1, 1};
	fill(&headPixel, 98, 87, 32);
	CHECK(HasProjectedHeadSkin(image.get(), sample));
	fill(&headPixel, 59, 84, 170);
	SDL_Rect unrelatedYellow{8, 8, 8, 8};
	fill(&unrelatedYellow, 255, 255, 0);
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), sample));
	fill(&headPixel, 98, 87, 32);
	auto invalid = sample;
	invalid.erase("eye_from_input");
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
	invalid = sample;
	invalid["eye_capture"]["image"] = 2;
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
	invalid = sample;
	invalid["eye_capture"]["frustum"][4] = 0;
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
	invalid = sample;
	invalid["eye_composed"] = true;
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
	invalid = sample;
	invalid["eye_capture"]["width"] = 64;
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
	invalid = sample;
	invalid["presented_predicted_root"]["position"] = {0, 0, 10};
	CHECK_FALSE(HasProjectedHeadSkin(image.get(), invalid));
}

static std::array<bool, 4> ReturnKeys(const nlohmann::json &sample) {
	const auto &position = sample.at("predicted_root").at("position");
	const auto &orientation = sample.at("control_basis").at("rotation");
	engine::core::CFrame basis;
	basis.QuaternionX = orientation.at(0).get<float>();
	basis.QuaternionY = orientation.at(1).get<float>();
	basis.QuaternionZ = orientation.at(2).get<float>();
	basis.QuaternionW = orientation.at(3).get<float>();
	const float yaw = sample.at("control_angles").at(1).get<float>();
	const auto forward = basis.VectorToWorldSpace({-std::sin(yaw), 0, -std::cos(yaw)});
	const auto side = basis.VectorToWorldSpace({std::cos(yaw), 0, -std::sin(yaw)});
	// Aim beyond the center of the finite pane so steering continues through its plane.
	const float dx = -position.at(0).get<float>();
	const float dz = -1.5f - position.at(2).get<float>();
	const float along = dx * forward.X + dz * forward.Z;
	const float across = dx * side.X + dz * side.Z;
	const float threshold = .18f * std::max(std::abs(along), std::abs(across));
	return {along > threshold, across < -threshold, along<-threshold, across> threshold};
}

enum class PortalWalkResult { Pending, Complete, TimedOut };

struct PortalWalkCompletion {
	size_t SettledFrames = 0;

	PortalWalkResult Observe(
		bool returned,
		bool limitReached,
		const std::array<size_t, 3> &cameraSamples,
		const std::array<size_t, 3> &cameraModeSamples,
		const std::array<size_t, 3> &moveSamples,
		const std::array<size_t, 2> &nativePoseSamples,
		const std::array<size_t, 3> &lookSamples,
		const std::array<float, 3> &minimumLookX,
		const std::array<float, 3> &maximumLookX,
		bool faultCovered
	) {
		SettledFrames = returned ? SettledFrames + 1 : 0;
		const auto sampled = [](size_t count) { return count > 0; };
		bool lookCovered = true;
		for (size_t stage = 0; stage < 2; ++stage)
			lookCovered &= lookSamples[stage] >= 10 && maximumLookX[stage] - minimumLookX[stage] > .005f;
		if (SettledFrames >= 32 && moveSamples[2] >= 13 && lookCovered && faultCovered &&
			std::all_of(cameraSamples.begin(), cameraSamples.end(), sampled) &&
			std::all_of(cameraModeSamples.begin(), cameraModeSamples.end(), sampled) &&
			std::all_of(moveSamples.begin(), moveSamples.end(), sampled) &&
			std::all_of(nativePoseSamples.begin(), nativePoseSamples.end(), sampled))
			return PortalWalkResult::Complete;
		return limitReached ? PortalWalkResult::TimedOut : PortalWalkResult::Pending;
	}
};

TEST_CASE(
	"[client] portal walks finish only after coverage and a settled return",
	"[client][portal-walk-completion]"
) {
	PortalWalkCompletion progress;
	std::array<size_t, 3> cameras{1, 1, 1};
	std::array<size_t, 3> modes{1, 1, 1};
	std::array<size_t, 3> moves{1, 1, 13};
	std::array<size_t, 2> nativePoses{1, 1};
	std::array<size_t, 3> looks{10, 10, 0};
	std::array<float, 3> minimumLookX{};
	std::array<float, 3> maximumLookX{.01f, .01f, 0};
	bool faultCovered = true;
	const auto observe = [&](bool returned = true, bool timedOut = false) {
		return progress.Observe(
			returned,
			timedOut,
			cameras,
			modes,
			moves,
			nativePoses,
			looks,
			minimumLookX,
			maximumLookX,
			faultCovered
		);
	};
	CHECK(observe(false) == PortalWalkResult::Pending);
	CHECK(observe(false, true) == PortalWalkResult::TimedOut);
	for (size_t frame = 0; frame < 31; ++frame)
		CHECK(observe() == PortalWalkResult::Pending);
	CHECK(progress.SettledFrames == 31);
	CHECK(observe(false) == PortalWalkResult::Pending);
	CHECK(progress.SettledFrames == 0);
	for (size_t frame = 0; frame < 31; ++frame)
		CHECK(observe() == PortalWalkResult::Pending);
	CHECK(observe() == PortalWalkResult::Complete);
	for (size_t stage = 0; stage < 3; ++stage) {
		CAPTURE(stage);
		cameras[stage] = 0;
		CHECK(observe() == PortalWalkResult::Pending);
		cameras[stage] = 1;
		modes[stage] = 0;
		CHECK(observe() == PortalWalkResult::Pending);
		modes[stage] = 1;
		const size_t completeMoves = moves[stage];
		moves[stage] = 0;
		CHECK(observe() == PortalWalkResult::Pending);
		moves[stage] = completeMoves;
	}
	for (size_t leg = 0; leg < nativePoses.size(); ++leg) {
		CAPTURE(leg);
		nativePoses[leg] = 0;
		CHECK(observe() == PortalWalkResult::Pending);
		CHECK(observe(true, true) == PortalWalkResult::TimedOut);
		nativePoses[leg] = 1;
		CHECK(observe() == PortalWalkResult::Complete);
	}
	moves[2] = 12;
	CHECK(observe() == PortalWalkResult::Pending);
	moves[2] = 13;
	for (size_t stage = 0; stage < 2; ++stage) {
		CAPTURE(stage);
		looks[stage] = 9;
		CHECK(observe() == PortalWalkResult::Pending);
		looks[stage] = 10;
		maximumLookX[stage] = .005f;
		CHECK(observe() == PortalWalkResult::Pending);
		maximumLookX[stage] = .01f;
	}
	faultCovered = false;
	CHECK(observe() == PortalWalkResult::Pending);
	CHECK(observe(true, true) == PortalWalkResult::TimedOut);
	faultCovered = true;
	CHECK(observe(true, true) == PortalWalkResult::Complete);
}

TEST_CASE("[client] return portal steering enters the finite pane", "[client][portal-return-steering]") {
	using namespace engine;
	for (const auto &[x, z, yaw, quaternion] :
		 {std::tuple{2.5625782f, -19.500595f, -.11900002f, -1.0f},
		  std::tuple{2.6970184f, -30.649317f, -.24500000f, -1.0f}}) {
		nlohmann::json sample{
			{"predicted_root", {{"position", {x, 2.5f, z}}}},
			{"control_angles", {0.0f, yaw}},
			{"control_basis", {{"rotation", {0.0f, 0.0f, 0.0f, quaternion}}}}
		};
		const auto keys = ReturnKeys(sample);
		CHECK(keys[2]);
		core::CFrame basis;
		basis.QuaternionW = quaternion;
		const auto forward = basis.VectorToWorldSpace({-std::sin(yaw), 0, -std::cos(yaw)});
		const auto side = basis.VectorToWorldSpace({std::cos(yaw), 0, -std::sin(yaw)});
		const float moveX = (static_cast<int>(keys[0]) - static_cast<int>(keys[2])) * forward.X +
							(static_cast<int>(keys[3]) - static_cast<int>(keys[1])) * side.X;
		const float moveZ = (static_cast<int>(keys[0]) - static_cast<int>(keys[2])) * forward.Z +
							(static_cast<int>(keys[3]) - static_cast<int>(keys[1])) * side.Z;
		const float toward = -x * moveX + (-1.5f - z) * moveZ;
		CAPTURE(x, z, yaw, keys, toward);
		CHECK(toward > 0);
	}
}

struct PortalWalkFrame {
	std::shared_ptr<const client::FrameObservation> Current;
	std::shared_ptr<const client::FrameObservation> Previous;
};

struct PortalWalkRequirement {};

class PortalWalkAssertions {
	struct Property {
		size_t Samples = 0;
		size_t Failed = 0;
		int FirstFailure = -1;
		std::string Detail;
	};
	std::map<std::string, Property> Properties;
	std::shared_ptr<std::atomic<bool>> FailureSignal;

  public:
	explicit PortalWalkAssertions(std::shared_ptr<std::atomic<bool>> failureSignal = {})
		: FailureSignal(std::move(failureSignal)) {}
	bool Observe(std::string_view name, bool passed, int frame, std::string_view detail = {}) {
		auto &property = Properties[std::string(name)];
		property.Samples++;
		if (!passed) {
			if (FailureSignal) FailureSignal->store(true, std::memory_order_relaxed);
			if (!property.Failed) {
				property.FirstFailure = frame;
				property.Detail = detail.substr(0, 512);
			}
			property.Failed++;
		}
		return passed;
	}
	size_t FailureCount() const {
		size_t count = 0;
		for (const auto &[name, property] : Properties)
			count += property.Failed;
		return count;
	}
	void Complete(engine::testing::CheckResult &result) const {
		for (const auto &[name, property] : Properties)
			result.Record(
				name,
				property.Failed == 0,
				"samples=" + std::to_string(property.Samples) +
					" failures=" + std::to_string(property.Failed) +
					" first_frame=" + std::to_string(property.FirstFailure) + " " + property.Detail
			);
	}
};

// SDL reads a bounded private copy of the canonical bytes; workers cannot write
// through the immutable engine observation, and no colour conversion is applied.
class PortalWalkSurface {
	std::vector<uint8_t> Pixels;
	std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> Surface{nullptr, SDL_DestroySurface};

  public:
	explicit PortalWalkSurface(const std::shared_ptr<const client::FrameObservation> &frame) {
		if (!frame || !client::tests::CanonicalImage(frame->Image)) return;
		Pixels = frame->Image.Pixels;
		Surface.reset(SDL_CreateSurfaceFrom(
			int(frame->Image.Width),
			int(frame->Image.Height),
			SDL_PIXELFORMAT_RGBA32,
			Pixels.data(),
			int(frame->Image.RowStride)
		));
	}
	explicit operator bool() const {
		return bool(Surface);
	}
	SDL_Surface *get() const {
		return Surface.get();
	}
	SDL_Surface *operator->() const {
		return Surface.get();
	}
};

// Only the owner writes joined first/terminal failures. Both adjacent pairs share
// one variant artifact budget; each path replaces its previous evidence.
static void WritePortalWalkFailure(
	const std::filesystem::path &directory,
	const nlohmann::json &variant,
	const std::shared_ptr<const client::FrameObservation> &current,
	const std::shared_ptr<const client::FrameObservation> &previous,
	const nlohmann::json &diagnostics,
	size_t &remainingBytes
) noexcept {
	try {
		const size_t maximumFailureBytes = remainingBytes;
		auto manifestFacts = variant;
		manifestFacts["failure_diagnostics"] = diagnostics;
		const std::string variantDocument = manifestFacts.dump(2);
		if (variantDocument.size() > maximumFailureBytes)
			throw std::runtime_error("failure variant exceeds byte limit");
		size_t bytes = variantDocument.size();
		std::array<std::string, 2> metadata;
		const std::array frames{previous, current};
		for (size_t index = 0; index < frames.size(); ++index) {
			if (!frames[index]) continue;
			const auto &owned = *frames[index];
			if (!client::tests::CanonicalImage(owned.Image) || owned.Image.Width != 128 ||
				owned.Image.Height != 128)
				throw std::runtime_error("failure capture is not the bounded canonical walk image");
			auto facts = owned.Metadata;
			const auto &camera = owned.Image.CameraFrame;
			facts["native_observation"] = {
				{"generation", owned.Generation},
				{"client_frame", owned.ClientFrame},
				{"renderer_frame", owned.Image.Frame},
				{"token", owned.Image.Token},
				{"world", owned.Image.WorldName},
				{"view_slot", owned.Image.ViewSlot},
				{"snapshot", owned.Image.SnapshotId},
				{"pipeline", owned.Pipeline},
				{"camera",
				 {{"position", {camera.Position.X, camera.Position.Y, camera.Position.Z}},
				  {"rotation",
				   {camera.QuaternionX, camera.QuaternionY, camera.QuaternionZ, camera.QuaternionW}}}}
			};
			try {
				const auto authored = JsonCameraFrame(owned.Metadata.at("camera"));
				facts["native_observation"]["camera_fuzzy_eq"] = camera.FuzzyEq(authored, .0001f);
				facts["native_observation"]["camera_components_equal"] =
					camera.Position == authored.Position && camera.Rotation() == authored.Rotation();
			} catch (const std::exception &error) {
				facts["native_observation"]["camera_annotation_error"] =
					std::string(error.what()).substr(0, 512);
			}
			metadata[index] = facts.dump();
			if (owned.Image.Pixels.size() > maximumFailureBytes - bytes)
				throw std::runtime_error("failure image pair exceeds byte limit");
			bytes += owned.Image.Pixels.size();
			if (metadata[index].size() > maximumFailureBytes - bytes)
				throw std::runtime_error("failure metadata pair exceeds byte limit");
			bytes += metadata[index].size();
		}
		// Reserve once for both first and terminal evidence, even if a write fails.
		remainingBytes -= bytes;
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);
		std::ofstream manifest(directory / "variant.json");
		manifest << variantDocument;
		if (!manifest) throw std::runtime_error("failure variant could not be written");
		for (size_t index = 0; index < frames.size(); ++index) {
			if (!frames[index]) continue;
			const auto stem = directory / std::to_string(frames[index]->ClientFrame);
			PortalWalkSurface image(frames[index]);
			if (!image || !SDL_SaveBMP(image.get(), (stem.string() + ".bmp").c_str()))
				throw std::runtime_error("failure image could not be written");
			std::ofstream output(stem.string() + ".json");
			output << metadata[index];
			if (!output) throw std::runtime_error("failure metadata could not be written");
		}
		std::cout << "portal walk failure evidence " << directory << '\n';
	} catch (const std::exception &error) {
		// Evidence failure must not replace the check failure or stop the walk.
		std::cout << "portal walk failure evidence unavailable " << directory << ": " << error.what() << '\n';
	}
}

template <typename Body>
static engine::testing::ScenarioCheck<PortalWalkFrame>
PortalWalkCheck(std::string name, std::shared_ptr<std::atomic<bool>> failureSignal, Body body) {
	auto callback = std::make_shared<Body>(std::move(body));
	engine::testing::ScenarioCheck<PortalWalkFrame> check;
	check.Name = std::move(name);
	check.SourceSuite = "client.portalwalk";
	check.Requirements.Hooks = {{"portal-walk.frame", 1}};
	check.Observe = [callback, failureSignal](const auto &sample, auto &result) {
		try {
			(*callback)(&sample.Payload, result);
		} catch (...) {
			// The scenario runner records the exception after the owner can retain its evidence.
			failureSignal->store(true, std::memory_order_relaxed);
			throw;
		}
	};
	check.Complete = [callback](auto, auto &result) { (*callback)(nullptr, result); };
	return check;
}

#define WALK_CHECK(...) (void)assertions.Observe(#__VA_ARGS__, static_cast<bool>((__VA_ARGS__)), frame)
#define WALK_CHECK_FALSE(...)                                                                                \
	(void)assertions.Observe("not " #__VA_ARGS__, !static_cast<bool>((__VA_ARGS__)), frame)
#define WALK_REQUIRE(...)                                                                                    \
	do {                                                                                                     \
		if (!assertions.Observe(#__VA_ARGS__, static_cast<bool>((__VA_ARGS__)), frame))                      \
			throw PortalWalkRequirement{};                                                                   \
	} while (false)
#define WALK_REQUIRE_FALSE(...)                                                                              \
	do {                                                                                                     \
		if (!assertions.Observe("not " #__VA_ARGS__, !static_cast<bool>((__VA_ARGS__)), frame))              \
			throw PortalWalkRequirement{};                                                                   \
	} while (false)

static void RunPortalWalk(
	double worldTickRate,
	bool firstPerson,
	bool explicitSubject,
	bool holdThroughAdoption,
	bool lateClear,
	bool imageHandoff = false,
	bool warmPortal = false,
	bool ownedContent = false,
	RoomShader roomShader = RoomShader::None,
	PortalWalkFault fault = PortalWalkFault::None,
	uint32_t captureFrameRate = 60,
	client::Options::NetworkImpairment impairment = {},
	std::vector<float> frameSchedule = {}
) {
	using namespace engine;
	const bool authoredShaders = roomShader != RoomShader::None;
	const bool spatialOverlay = roomShader == RoomShader::SpatialOverlay || roomShader == RoomShader::Lens;
	CAPTURE(roomShader);
	CAPTURE(
		worldTickRate,
		captureFrameRate,
		firstPerson,
		explicitSubject,
		holdThroughAdoption,
		lateClear,
		warmPortal,
		fault
	);
	CAPTURE(
		impairment.RoundTripSeconds,
		impairment.JitterSeconds,
		impairment.LossChance,
		impairment.DuplicateChance,
		impairment.ReorderChance,
		frameSchedule.size()
	);
	const auto programs = core::Paths::Base().parent_path();
	const auto serverProgram = programs / "server" / core::Paths::Program("server");
	const auto clientProgram = programs / "client" / core::Paths::Program("client");
	if (!std::filesystem::exists(serverProgram) || !std::filesystem::exists(clientProgram))
		SKIP("build both product programs before this test");
	const auto scenePath = core::Paths::Base() / "portal-client-walk.agame";
	std::string sceneSource =
		std::string("local ownedRoomContent = ") + (ownedContent ? "true\n" : "false\n") + R"(
local floor = Instance.new(ownedRoomContent and "MeshPart" or "Part")
if ownedRoomContent then floor.MeshId = "engine.Cube" end
floor.Anchored = true
floor.Size = Vector3.new(100, 2, 100)
floor.Position = Vector3.new(0, -1, 0)
floor.Color = game.JobId == "walk.destination" and Color3.new(0.15, 0.25, 0.9) or Color3.new(0.9, 0.3, 0.1)
floor.Parent = workspace
local pane = Instance.new("Part")
pane.Anchored = true
pane.Size = Vector3.new(10, 10, 0.4)
pane.Position = Vector3.new(0, 3, -3)
pane.Parent = workspace
local beyond = Instance.new("Part")
beyond.Anchored = true
beyond.CanCollide = false
beyond.Transparency = 1
beyond.Size = pane.Size
beyond.CFrame = CFrame.new(0, 3, -3.4) * CFrame.Angles(0, math.pi, 0)
beyond.Parent = workspace
local portal = Instance.new("Portal")
portal.Destination = beyond
portal.DestinationWorld = game.JobId == "walk.destination" and "server.world" or "walk.destination"
portal.Parent = pane
)";
	if (fault != PortalWalkFault::None)
		sceneSource += R"(
local obstruction = Instance.new("Part")
obstruction.Anchored = true
obstruction.Size = Vector3.new(4, 4, 0.2)
obstruction.Position = Vector3.new(0, 4, 5)
obstruction.Parent = workspace
)";
	if (ownedContent)
		sceneSource += R"(
local image = Instance.new("EditableImage")
image.Name = "RoomColour"
image.Parent = workspace
assert(image:Resize(2, 2))
image:DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), floor.Color)
floor.Color = Color3.new(1, 1, 1)
floor.TextureID = image.ContentId
)";
	if (authoredShaders)
		sceneSource += R"shader(
local shader = Instance.new("ShaderScript")
shader.Name = "RoomShader"
shader.Source = "#version 450\nlayout(location=0) out vec4 colour;\nvoid main(){colour=" ..
    (game.JobId == "walk.destination" and "vec4(0,0,1,1)" or "vec4(1,0,0,1)") .. ";}"
shader.Parent = workspace
floor.Color = Color3.new(1, 1, 1)
)shader";
	if (roomShader == RoomShader::Material)
		sceneSource += R"(
local material = Instance.new("Material")
material.Shader = shader.Name
material.Parent = floor
)";
	if (roomShader == RoomShader::Spatial || spatialOverlay)
		sceneSource += R"(
local image = Instance.new("EditableImage")
image.Name = "RoomWhite"
image.Parent = workspace
assert(image:Resize(2, 2))
image:DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), Color3.new(1, 1, 1))
local canvas = Instance.new("SurfaceGui")
canvas.Face = Enum.NormalId.Top
canvas.CanvasSize = Vector2.new(100, 100)
canvas.AlwaysOnTop = false
canvas.Parent = floor
local picture = Instance.new("ImageLabel")
picture.BackgroundTransparency = 1
picture.Size = UDim2.new(1, 0, 1, 0)
picture.Image = image.ContentId
picture.Shader = "RoomShader"
picture.Parent = canvas
)";
	if (spatialOverlay)
		sceneSource += R"(
if game.JobId == "walk.destination" then
    local anchor = Instance.new("Part")
    anchor.Name = "TopMarker"
    anchor.Anchored = true
    anchor.CanCollide = false
    anchor.Transparency = 1
    anchor.Size = Vector3.new(2, 1, 0.1)
    anchor.Position = Vector3.new(2, 5, -14)
    anchor.Parent = workspace
    local canvas = Instance.new("SurfaceGui")
    canvas.Face = Enum.NormalId.Back
    canvas.CanvasSize = Vector2.new(100, 50)
    canvas.AlwaysOnTop = true
    canvas.LightInfluence = 0
    canvas.Parent = anchor
    local marker = Instance.new("Frame")
    marker.Size = UDim2.new(1, 0, 1, 0)
    marker.BackgroundColor3 = Color3.new(0, 1, 0)
    marker.BorderSizePixel = 0
    marker.Parent = canvas
end
)";
	if (roomShader == RoomShader::Lens)
		sceneSource += R"lens(
local lensShader = Instance.new("LensShader")
lensShader.Name = "RoomLens"
lensShader.Source = [[#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 colour;
layout(set=2,binding=0) uniform sampler2D sceneColour;
layout(set=2,binding=1) uniform sampler2D sceneDepth;
void main(){
    vec2 sampleUv = uv;
    if (uv.x > 0.25 && uv.x < 0.95 && uv.y < 0.75)
        sampleUv.x += ]] .. (game.JobId == "walk.destination" and "-0.0625" or "0.03125") .. [[;
    colour = texture(sceneColour, sampleUv);
}]]
lensShader.Parent = workspace
local lens = Instance.new("ShaderLens")
lens.Name = "RoomLensEffect"
lens.Shader = lensShader.Name
lens.Radius = 100
lens.Strength = 1
lens.Parent = workspace
)lens";
	scene::RegisterSceneClasses();
	script::RegisterScriptComponents();
	world::Universe authored;
	for (const auto *name : {"server.world", "walk.destination"}) {
		const auto id = authored.Create(
			{.Name = core::Name(name),
			 .TickRate = worldTickRate,
			 .GlobalSimulatedNetworkLatency = fault == PortalWalkFault::Delay ? 150.0 : 0.0}
		);
		authored.Enter(id, [&](ecs::Store &store) {
			scene::InstallServices(store);
			script::SourceCache programs;
			programs.Set(core::Name("walk.luau"), sceneSource);
			programs.Set(
				core::Name("camera.luau"),
				std::string("local lateClear = ") + (lateClear ? "true\n" : "false\n") +
					"local explicitSubject = " + (explicitSubject ? "true\n" : "false\n") +
					"local authority = \"" + name + R"("
local players
local boundCharacter
local cleared = false
local restored = false
game:GetService("RunService").Heartbeat:Connect(function()
    if not players then
        local arrived, service = pcall(function() return game:GetService("Players") end)
        if not arrived then return end
        players = service
    end
    local player = players.LocalPlayer
    if not player or not player:IsA("Player") then return end
    local character = player.Character
    if not character or not character:IsA("Model") then return end
    local humanoid = character and character:FindFirstChildOfClass("Humanoid")
    local camera = workspace.CurrentCamera
    local replaced = boundCharacter and boundCharacter ~= character
    if explicitSubject and humanoid and camera and boundCharacter ~= character then
        if not (lateClear and authority == "walk.destination" and not restored) then
            camera.CameraSubject = humanoid
        end
        boundCharacter = character
    end
    if lateClear and authority == "server.world" and replaced and not cleared and camera then
        camera.CameraSubject = nil
        cleared = true
        print("portal-camera-cleared", authority)
    end
    if lateClear and authority == "walk.destination" and not restored and camera and humanoid and game:GetService("UserInputService"):IsKeyDown(Enum.KeyCode.R) then
        camera.CameraSubject = humanoid
        restored = true
        print("portal-camera-restored", authority)
    end
    local valid = humanoid and camera and camera.CameraSubject == humanoid
    print("portal-camera", authority, valid and "bound" or "unbound")
    local root = humanoid and humanoid.RootPart
    if valid and root then
        local distance = (camera.CFrame.Position - root.Position - Vector3.new(0, 1.5, 0)).Magnitude
        print("portal-camera-distance", authority, distance < 1 and "first" or "third", humanoid.MoveDirection.Magnitude < 0.001 and "rest" or "moving", distance, distance < 6 and "blocked" or "clear", camera.CFrame.LookVector.X)
    end
end)
)"
			);
			store.SetResource(programs);
			REQUIRE(store.SetParent(
				script::MakeScript(store, "walk.luau", "Walk"), store.FindFirstRoot("ServerScriptService")
			));
			REQUIRE(store.SetParent(
				script::MakeScript(store, "camera.luau", "CameraObserver", true),
				store.FindFirstRoot("ReplicatedFirst")
			));
		});
	}
	std::string saveError;
	REQUIRE(game::SaveGame(authored, core::Name("portal walk"), scenePath, saveError));
	const auto configPath = core::Paths::Base() / "portal-client-walk.ini";
	const auto faultReportPath = core::Paths::Base() / "portal-client-walk-fault-report.txt";
	std::filesystem::remove(faultReportPath);
	std::ofstream(configPath).close();
	const auto storeRoot = core::Paths::Base() / "portal-client-walk-store";
	std::error_code storeError;
	std::filesystem::remove_all(storeRoot, storeError);
	REQUIRE_FALSE(storeError);
	const auto localPath = core::Paths::Base() / "portal-client-empty.luau";
	std::ofstream(localPath) << "return\n";
	auto reservation = net::MakeUdpTransport(0);
	REQUIRE(reservation);
	const auto port = reservation->Local().Port;
	reservation->Close();
	// Server-side portal and physics observations, one JSONL file per process,
	// all stamped with one trace id so a crossing reads as a single trail.
	constexpr uint64_t OBSERVATION_TRACE = 0x5057414c4b;
	const auto observationDirectory =
		core::Paths::Base() / ("portal-client-walk-observations-" + std::to_string(port));
	std::filesystem::remove_all(observationDirectory);
	parallel::Process server;
	std::vector<std::string> serverArguments{
		"--listen",
		std::to_string(port),
		"--game",
		scenePath.string(),
		"--remote-world",
		"walk.destination",
		"--presentation-program",
		clientProgram.string(),
		"--config",
		configPath.string(),
		"--datastore-root",
		storeRoot.string(),
		"--mcp-port",
		"-1",
		"--observe-dir",
		observationDirectory.string(),
		"--observe-trace",
		std::to_string(OBSERVATION_TRACE)
	};
	if (fault == PortalWalkFault::RestartDestinationPresentation) {
		serverArguments.insert(
			serverArguments.end(), {"--test-restart-presentation-world", "walk.destination"}
		);
	}
	if (fault == PortalWalkFault::DropAcknowledgement)
		serverArguments.emplace_back("--test-drop-next-portal-crossed-acknowledgement");
	if (fault != PortalWalkFault::None) {
		serverArguments.emplace_back("--test-portal-fault-report");
		serverArguments.emplace_back(faultReportPath.string());
	}
	REQUIRE(server.Start(serverProgram, serverArguments));
	struct Observations {
		std::mutex Mutex;
		bool Joined = false;
		bool PortalReady = false;
		size_t CapturedFrames = 0;
		PortalWalkCompletion Completion;
		PortalWalkResult Result = PortalWalkResult::Pending;
		bool CameraCleared = false, CameraRestored = false;
		std::vector<std::string> Adoptions;
		std::vector<std::string> Refusals;
		std::array<size_t, 3> CameraSamples{};
		std::array<size_t, 3> CameraModeSamples{};
		std::array<bool, 3> Moving{};
		std::array<size_t, 3> LookSamples{};
		std::array<size_t, 3> ObstructedSamples{};
		std::array<size_t, 3> PointerFrames{};
		std::array<float, 3> MinimumLookX{};
		std::array<float, 3> MaximumLookX{};
		std::array<uint64_t, 3> LastMoveTick{};
		uint64_t LastSubmission = 0;
		std::array<size_t, 3> MoveSamples{};
		std::array<size_t, 2> NativePoseSamples{};
		size_t AdoptedClearSamples = 0;
		std::optional<size_t> RestorePressedAtMove;
		bool DuplicateMove = false;
		std::vector<std::string> CameraFailures;
		std::string PendingAdoption;
		std::string PendingInputWorld;
		std::string ReturnSteeringDestination;
		bool ReturnEnteredAperture = false;
		bool ReturnStopped = false;
		float ReturnClosestLateral = std::numeric_limits<float>::infinity();
		std::array<bool, 4> PressedReturnKeys{};
		bool WalkingOn = false;
		// The outbound walk let go of W short of the floor's end and strafes
		// until adoption, so a late hold still sees a moving body.
		bool OutboundStopped = false;
		size_t OutboundStrafeFrames = 0;
		size_t WalkingOnHiddenFrames = 0;
		std::optional<std::array<float, 3>> PreviousReturnPosition;
		std::optional<uint64_t> FirstSteeringFrame;
		size_t SteeringFrames = 0;
		size_t SteeringEvents = 0;
		size_t MisalignedSubmittedMoves = 0;
		std::optional<uint64_t> FirstMisalignedFrame;
		std::array<float, 3> FirstMisalignedMove{};
		// Summed from each impaired socket's close report.
		uint64_t ImpairedArrived = 0, ImpairedDropped = 0, ImpairedDuplicated = 0, ImpairedReordered = 0,
				 ImpairedDelayed = 0;
	} observed;
	std::filesystem::path captureDirectory;
	size_t maximumFrames = 0;
	auto runDeadline = std::chrono::steady_clock::time_point::max();
	bool faultEvidenceCovered = fault != PortalWalkFault::DropAcknowledgement;
	auto nextFaultReportPoll = std::chrono::steady_clock::time_point::min();
	const auto key = [](SDL_Scancode scan, SDL_Keycode code, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.scancode = scan;
		event.key.key = code;
		event.key.down = down;
		(void)SDL_PushEvent(&event);
	};
	const auto turning = [](bool down) {
		SDL_Event button{};
		button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		button.button.button = SDL_BUTTON_RIGHT;
		button.button.down = down;
		(void)SDL_PushEvent(&button);
	};
	const auto returnKey = [&](size_t index, bool down) {
		constexpr std::array<SDL_Scancode, 4> scans{
			SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D
		};
		constexpr std::array<SDL_Keycode, 4> codes{SDLK_W, SDLK_A, SDLK_S, SDLK_D};
		key(scans[index], codes[index], down);
	};
	const auto DriveFrame = [&](const client::FrameObservation *previous, uint64_t capturedFrame) {
		std::lock_guard lock(observed.Mutex);
		observed.CapturedFrames = static_cast<size_t>(capturedFrame + 1);
		if (previous) {
			const auto &sample = previous->Metadata;
			if (warmPortal && !observed.PortalReady && sample.contains("portal_views"))
				for (const auto &portal : sample.at("portal_views"))
					observed.PortalReady |=
						portal.value("external", false) && portal.value("image", uint64_t{0}) != 0;
			if (sample.contains("portal_handoff") && sample.at("portal_handoff").value("commit", false)) {
				observed.PendingAdoption = sample.at("portal_handoff").value("destination", "");
				observed.PendingInputWorld = sample.value("input_world", "");
			} else if (!observed.PendingAdoption.empty() && !sample.contains("portal_handoff") &&
					   sample.value("input_world", "") != observed.PendingInputWorld &&
					   observed.Adoptions.size() < 2) {
				observed.Adoptions.push_back(observed.PendingAdoption);
				observed.PendingAdoption.clear();
				observed.PendingInputWorld.clear();
				turning(false);
				if (observed.Adoptions.size() == 1 && observed.OutboundStopped) {
					key(SDL_SCANCODE_D, SDLK_D, false);
					key(SDL_SCANCODE_A, SDLK_A, false);
				}
				if (imageHandoff && fault == PortalWalkFault::None && observed.Adoptions.size() == 1) {
					observed.ReturnSteeringDestination = observed.Adoptions.back();
					// A portal may carry a directional key that was pressed before
					// its input owner changed. Release all four before the route
					// settles, otherwise that old motion can enter the next mouth.
					for (size_t index = 0; index < observed.PressedReturnKeys.size(); ++index) {
						returnKey(index, false);
						observed.PressedReturnKeys[index] = false;
					}
				} else if (imageHandoff && fault == PortalWalkFault::None && observed.Adoptions.size() == 2) {
					// The scripted return has reached its terminal world. Release
					// every steering key before later samples can start a third pass.
					for (size_t index = 0; index < observed.PressedReturnKeys.size(); ++index) {
						if (!observed.PressedReturnKeys[index]) continue;
						returnKey(index, false);
						observed.PressedReturnKeys[index] = false;
					}
				} else if (!holdThroughAdoption)
					key(observed.Adoptions.size() == 1 ? SDL_SCANCODE_W : SDL_SCANCODE_S,
						observed.Adoptions.size() == 1 ? SDLK_W : SDLK_S,
						false);
			}
			// The settled destination walks on until its trailing camera has left the
			// return pane behind it, so that pane's demand is seen hidden before the
			// steered return turns back towards it. The camera's lag behind the body
			// varies with the tick rate, so the driver waits for the hidden demand
			// itself; each hidden frame is still validated below. The floor ends at
			// z -50, and a walk that reaches -40 without it turns back and fails there.
			// A slow link holds adoption for seconds while W stays down, and the
			// floor ends at z -50. The prediction keeps every held input, and the
			// committed body catches up to it, so let go short of the edge. Then
			// swing left and right in place, since the hold that starts once the
			// crossing is confirmed can come later still and must see motion.
			if (observed.Adoptions.empty() && !observed.OutboundStopped &&
				sample.contains("predicted_root") &&
				sample.at("predicted_root").at("position").at(2).get<float>() < -40.0f) {
				observed.OutboundStopped = true;
				key(SDL_SCANCODE_W, SDLK_W, false);
			}
			if (observed.Adoptions.empty() && observed.OutboundStopped &&
				observed.OutboundStrafeFrames++ % 40 == 0) {
				const bool right = (observed.OutboundStrafeFrames / 40) % 2 == 0;
				key(SDL_SCANCODE_D, SDLK_D, right);
				key(SDL_SCANCODE_A, SDLK_A, !right);
			}
			if (observed.WalkingOn && observed.Adoptions.size() == 1 && sample.contains("predicted_root")) {
				for (const auto &portal : sample.at("portal_views"))
					observed.WalkingOnHiddenFrames +=
						portal.value("external", false) && portal.value("demand_status", "") == "hidden";
				if (observed.WalkingOnHiddenFrames >= 3 ||
					sample.at("predicted_root").at("position").at(2).get<float>() < -40.0f)
					observed.WalkingOn = false;
			}
			// Steer only once the destination camera has settled, as the outbound
			// walk waits before pressing W. Steering from adoption keeps the body
			// moving, so the stage never records the rest samples it is checked on.
			if (imageHandoff && fault == PortalWalkFault::None && observed.Adoptions.size() == 1 &&
				observed.Moving[1] && !observed.WalkingOn && sample.value("subject_is_humanoid", false) &&
				sample.contains("predicted_root") && sample.contains("control_basis") &&
				sample.contains("control_angles")) {
				observed.FirstSteeringFrame.emplace(capturedFrame - 1);
				++observed.SteeringFrames;
				const auto &position = sample.at("predicted_root").at("position");
				const float x = position.at(0).get<float>();
				const float y = position.at(1).get<float>();
				const float z = position.at(2).get<float>();
				if (z < -3.2f)
					observed.ReturnClosestLateral = std::min(observed.ReturnClosestLateral, std::abs(x));
				if (observed.PreviousReturnPosition && observed.PreviousReturnPosition->at(2) < -3.2f &&
					z >= -3.2f && std::abs(x) < 4.5f && y > -1.5f && y < 7.5f)
					observed.ReturnEnteredAperture = true;
				observed.PreviousReturnPosition = std::array{x, y, z};
				if (observed.SteeringFrames > 3 && sample.contains("submitted_move_direction")) {
					const auto &submitted = sample.at("submitted_move_direction");
					const float toward =
						-x * submitted.at(0).get<float>() + (-1.5f - z) * submitted.at(2).get<float>();
					if (toward <= 0) {
						++observed.MisalignedSubmittedMoves;
						if (!observed.FirstMisalignedFrame) {
							observed.FirstMisalignedFrame = capturedFrame - 1;
							observed.FirstMisalignedMove = {
								submitted.at(0).get<float>(),
								submitted.at(1).get<float>(),
								submitted.at(2).get<float>()
							};
						}
					}
				}
				// Stop once the server has begun the return crossing. Stopping on the
				// prediction alone can leave the authoritative body short of the plane.
				// Circling the aim point until adoption is observed can carry the body
				// back through, which is a real third crossing, and walking on leaves
				// the camera far past the pane before the crossing is confirmed.
				observed.ReturnStopped |=
					observed.ReturnEnteredAperture && sample.contains("portal_handoff") &&
					sample.at("portal_handoff").value("destination", "") == "server.world";
				const auto desired = observed.ReturnStopped ? std::array<bool, 4>{} : ReturnKeys(sample);
				for (size_t index = 0; index < desired.size(); ++index)
					if (desired[index] != observed.PressedReturnKeys[index]) {
						returnKey(index, desired[index]);
						observed.PressedReturnKeys[index] = desired[index];
						++observed.SteeringEvents;
					}
			}
			const size_t stage = observed.Adoptions.size();
			if (stage > 0 && stage <= observed.NativePoseSamples.size() &&
				sample.contains("native_prediction") &&
				sample.at("native_prediction").value("applied_pose_tick", uint64_t{0}) != 0)
				++observed.NativePoseSamples[stage - 1];
			if (lateClear && stage == 1 && sample.value("subject", uint64_t{1}) == 0 &&
				!sample.value("subject_automatic", true) && sample.contains("native_prediction"))
				++observed.AdoptedClearSamples;
			const uint64_t submitted = sample.value("submitted_move_tick", uint64_t{0});
			if (submitted < observed.LastSubmission) observed.DuplicateMove = true;
			if (submitted > observed.LastSubmission) {
				observed.LastSubmission = submitted;
				observed.LastMoveTick[stage] = submitted;
				++observed.MoveSamples[stage];
				if (lateClear && stage == 1) {
					if (!observed.RestorePressedAtMove && observed.MoveSamples[stage] >= 12 &&
						observed.AdoptedClearSamples >= 3) {
						observed.RestorePressedAtMove = observed.MoveSamples[stage];
						key(SDL_SCANCODE_R, SDLK_R, true);
					} else if (observed.RestorePressedAtMove &&
							   observed.MoveSamples[stage] == *observed.RestorePressedAtMove + 2)
						key(SDL_SCANCODE_R, SDLK_R, false);
				}
				if (holdThroughAdoption && stage > 0 && observed.MoveSamples[stage] == 12 &&
					!(imageHandoff && fault == PortalWalkFault::None && stage == 1))
					key(stage == 1 ? SDL_SCANCODE_W : SDL_SCANCODE_S, stage == 1 ? SDLK_W : SDLK_S, false);
			}
			const bool returned = observed.Adoptions.size() == 2 && !sample.contains("portal_handoff") &&
								  sample.value("eye_world", "") == "server.world" &&
								  !sample.value("eye_image", true);
			if (observed.Result == PortalWalkResult::Pending) {
				const auto now = std::chrono::steady_clock::now();
				if (returned && observed.Completion.SettledFrames >= 31 && !faultEvidenceCovered &&
					now >= nextFaultReportPoll) {
					// A settled camera can precede the server's dropped-acknowledgement retry.
					std::ifstream report(faultReportPath);
					const std::string markers(std::istreambuf_iterator<char>(report), {});
					faultEvidenceCovered = markers.find("dropped-lease-adopted") != std::string::npos &&
										   markers.find("recovered-lease-adopted") != std::string::npos;
					nextFaultReportPoll = now + std::chrono::milliseconds(100);
				}
				const auto result = observed.Completion.Observe(
					returned,
					observed.CapturedFrames >= maximumFrames || now >= runDeadline,
					observed.CameraSamples,
					observed.CameraModeSamples,
					observed.MoveSamples,
					observed.NativePoseSamples,
					observed.LookSamples,
					observed.MinimumLookX,
					observed.MaximumLookX,
					faultEvidenceCovered &&
						(fault == PortalWalkFault::None || observed.ObstructedSamples[0] > 0)
				);
				if (result != PortalWalkResult::Pending) {
					SDL_Event quit{};
					quit.type = SDL_EVENT_QUIT;
					if (SDL_PushEvent(&quit)) observed.Result = result;
				}
			}
		}
		const size_t stage = observed.Adoptions.size();
		if (stage < 2 && observed.Moving[stage]) {
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.xrel = (++observed.PointerFrames[stage] / 40) % 2 == 0 ? 2.0f : -2.0f;
			(void)SDL_PushEvent(&motion);
		}
	};
	auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg &entry) {
		std::string_view message(entry.payload.data(), entry.payload.size());
		std::lock_guard lock(observed.Mutex);
		if (const size_t report = message.find("impaired link closed: "); report != std::string_view::npos) {
			const auto field = [&](std::string_view name) {
				const size_t at = message.find(name, report);
				uint64_t value = 0;
				if (at != std::string_view::npos)
					std::from_chars(
						message.data() + at + name.size(), message.data() + message.size(), value
					);
				return value;
			};
			observed.ImpairedArrived += field("arrived=");
			observed.ImpairedDropped += field("dropped=");
			observed.ImpairedDuplicated += field("duplicated=");
			observed.ImpairedReordered += field("reordered=");
			observed.ImpairedDelayed += field("delayed=");
			return;
		}
		if (message == "[scriptluau] [script] portal-camera-cleared\tserver.world") {
			observed.CameraCleared = true;
			return;
		}
		if (message == "[scriptluau] [script] portal-camera-restored\twalk.destination") {
			observed.CameraRestored = true;
			return;
		}

		constexpr std::string_view distancePrefix = "[scriptluau] [script] portal-camera-distance\t";
		if (message.starts_with(distancePrefix)) {
			const size_t stage = observed.Adoptions.size();
			const std::string_view authority = stage == 1 ? "walk.destination" : "server.world";
			message.remove_prefix(distancePrefix.size());
			if (!message.starts_with(authority)) return;
			const bool blocked = message.find("\tblocked\t") != std::string_view::npos;
			if (blocked) ++observed.ObstructedSamples[stage];
			const std::string_view mode = firstPerson ? "\tfirst\t" : "\tthird\t";
			// During movement the predicted eye and authoritative root describe different times.
			if (observed.Moving[stage]) {
				const auto coordinate = message.substr(message.rfind('\t') + 1);
				float lookX = 0;
				const auto parsed =
					std::from_chars(coordinate.data(), coordinate.data() + coordinate.size(), lookX);
				if (parsed.ec != std::errc{}) {
					if (observed.CameraFailures.size() < 8) observed.CameraFailures.emplace_back(message);
					return;
				}
				if (observed.LookSamples[stage] == 0) {
					observed.MinimumLookX[stage] = lookX;
					observed.MaximumLookX[stage] = lookX;
				}
				observed.MinimumLookX[stage] = std::min(observed.MinimumLookX[stage], lookX);
				observed.MaximumLookX[stage] = std::max(observed.MaximumLookX[stage], lookX);
				++observed.LookSamples[stage];
				return;
			}
			if (message.find("\trest\t") == std::string_view::npos) return;
			if (message.find(mode) != std::string_view::npos || (!firstPerson && blocked)) {
				++observed.CameraModeSamples[stage];
				if (stage < 2 && observed.CameraModeSamples[stage] >= 3 &&
					(stage != 0 || !warmPortal || observed.PortalReady)) {
					const bool settled = observed.Moving[stage];
					observed.Moving[stage] = true;
					turning(true);
					if (stage == 0 ? !observed.OutboundStopped
								   : !(imageHandoff && fault == PortalWalkFault::None))
						key(stage == 0 ? SDL_SCANCODE_W : SDL_SCANCODE_S, stage == 0 ? SDLK_W : SDLK_S, true);
					else if (stage == 1 && !settled) {
						// Walk on through the steering state, so steering releases W itself.
						returnKey(0, true);
						observed.PressedReturnKeys[0] = true;
						observed.WalkingOn = true;
					}
				}
			} else if (observed.CameraModeSamples[stage] >= 3) {
				if (observed.CameraFailures.size() < 8) observed.CameraFailures.emplace_back(message);
			} else {
				observed.CameraModeSamples[stage] = 0;
			}
			return;
		}
		constexpr std::string_view cameraPrefix = "[scriptluau] [script] portal-camera\t";
		if (message.starts_with(cameraPrefix)) {
			const size_t stage = observed.Adoptions.size();
			const std::string_view authority = stage == 1 ? "walk.destination" : "server.world";
			message.remove_prefix(cameraPrefix.size());
			if (!message.starts_with(authority)) return;
			if (message.ends_with("\tbound"))
				++observed.CameraSamples[stage];
			else if (!(lateClear && observed.CameraCleared && !observed.CameraRestored) &&
					 observed.CameraSamples[stage] != 0 && observed.CameraFailures.size() < 8)
				observed.CameraFailures.emplace_back(message);
			return;
		}
		if (!message.starts_with("[client] ")) return;
		message.remove_prefix(std::string_view("[client] ").size());
		if (message.starts_with("joined:") && !observed.Joined) {
			observed.Joined = true;
			SDL_Event focus{};
			focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
			(void)SDL_PushEvent(&focus);
			if (firstPerson) {
				SDL_Event zoom{};
				zoom.type = SDL_EVENT_MOUSE_WHEEL;
				zoom.wheel.y = 100;
				(void)SDL_PushEvent(&zoom);
			}
		}
		if (message.starts_with("portal transfer refused:") && observed.Refusals.size() < 8)
			observed.Refusals.emplace_back(message);
	});
	struct SinkLifetime {
		spdlog::sink_ptr Sink;
		core::LogLevel PreviousRowLevel;
		~SinkLifetime() {
			std::erase(core::Log::Logger().sinks(), Sink);
			core::Log::SetLevel("replication-row", PreviousRowLevel);
		}
	} attached{sink, core::Log::LevelOf("replication-row")};
	if (const char *levels = std::getenv("ATOMIC_ENGINE_LOG_LEVEL");
		levels && std::string_view(levels).find("replication-row=trace") != std::string_view::npos)
		core::Log::SetLevel("replication-row", core::LogLevel::Trace);
	core::Log::Logger().sinks().push_back(sink);
	client::Options options;
	options.Headless = true;
	options.Width = 128;
	options.Height = 128;
	options.ScriptPath = localPath.string();
	options.ConnectAddress = net::Endpoint::LoopbackIPv4(port).Text();
	options.PlayKey = std::string(64, '3');
	constexpr int WALK_TIMEOUT_SECONDS = 180;
	options.ProfileSeconds = WALK_TIMEOUT_SECONDS;
	options.MaximumFrameRate = captureFrameRate;
	options.Uncapped = true;
	// Coverage ends the walk; missing milestones reach the wall or frame guard.
	options.MaximumFrames = static_cast<int64_t>(WALK_TIMEOUT_SECONDS) * captureFrameRate;
	maximumFrames = static_cast<size_t>(options.MaximumFrames);
	options.Impairment = impairment;
	options.CaptureFrameSchedule = frameSchedule;
	captureDirectory = core::Paths::Base() /
					   ("portal-client-walk-" + std::to_string(static_cast<int>(worldTickRate)) +
						(firstPerson ? "-first" : "-third") + (explicitSubject ? "-explicit" : "") +
						(holdThroughAdoption ? "-held" : "") +
						(lateClear ? (imageHandoff ? "-clear-image-frames" : "-clear-frames") : "-frames"));
	if (warmPortal) captureDirectory += "-warm";
	if (ownedContent) captureDirectory += "-owned";
	if (roomShader == RoomShader::Material) captureDirectory += "-shaders";
	if (roomShader == RoomShader::Spatial) captureDirectory += "-spatial-shaders";
	if (roomShader == RoomShader::SpatialOverlay) captureDirectory += "-spatial-overlay";
	if (roomShader == RoomShader::Lens) captureDirectory += "-captured-lens";
	if (captureFrameRate != 60) captureDirectory += "-render" + std::to_string(captureFrameRate);
	if (impairment.Active())
		captureDirectory += "-rtt" + std::to_string(int(impairment.RoundTripSeconds * 1000.0 + .5)) +
							"-jitter" + std::to_string(int(impairment.JitterSeconds * 1000.0 + .5)) +
							"-loss" + std::to_string(int(impairment.LossChance * 100.0f + .5f));
	if (!frameSchedule.empty()) captureDirectory += "-stall";
	// Each fault keeps its own evidence rather than the next variant's.
	if (fault != PortalWalkFault::None)
		captureDirectory += "-fault" + std::to_string(static_cast<int>(fault));
	if (const char *profile = std::getenv("PORTAL_PROFILE_SNAPSHOT");
		profile && std::string_view(profile) == "1")
		options.ProfileSnapshot = captureDirectory / "frame-graph-snapshot.txt";
	// Frame intervals include observation readback and checks; GPU pass timings do not.
	options.FrameTimings = captureDirectory.string() + "-frame-timings.csv";
	struct PixelSummary {
		size_t AwaitingImageBodyFrames = 0;
		size_t FirstContactFrames = 0;
		nlohmann::json Diagnostics = nlohmann::json::object();
		bool CompleteFailed = false;
	};
	const auto pixelSummary = std::make_shared<PixelSummary>();
	const auto streamedCheckFailed = std::make_shared<std::atomic<bool>>(false);
	const auto terminalFrame = std::make_shared<std::shared_ptr<const client::FrameObservation>>();
	bool characterSeen = false;
	bool explicitSubjectSeen = false;
	std::optional<float> previousYaw;
	float maximumEyeHeightError = 0;
	int maximumEyeHeightFrame = -1;
	size_t sourceClearSamples = 0, adoptedClearSamples = 0, clearedAdoptions = 0;
	size_t handoffMoveSamples = 0;
	size_t retainedMoveSamples = 0, nativeReturnSamples = 0, nativeHeldSamples = 0;
	size_t resumedNativeSamples = 0;
	std::array<size_t, 2> shaderRoomSamples{};
	size_t shaderObservedSamples = 0;
	size_t spatialOverlayAdoptions = 0;
	size_t observedEyeSamples = 0;
	bool returned = false;
	bool firstCaptureSeen = false;
	bool sourceReplicaSeen = false;
	bool firstAdoptionSeen = false;
	bool outboundMotion = false, returnMotion = false;
	size_t exactFaultCameraSamples = 0;
	size_t faultClipSamples = 0;
	size_t faultVisibleFrames = 0;
	size_t faultBodyFrames = 0;
	size_t hiddenPortalSamples = 0;
	size_t awaitingImageBodyFrames = 0;
	// Impaired frames drawn before the client held any state for a room behind a
	// pane. Only these may be empty; afterwards a last-known copy draws the room.
	size_t firstContactFrames = 0;
	std::vector<std::string> nativeWorlds;
	std::unordered_map<uint64_t, int> crossedReadyWithoutTerminal;
	std::vector<testing::ScenarioCheck<PortalWalkFrame>> scenarioChecks;
	scenarioChecks.push_back(PortalWalkCheck(
		"frame.handoff-provenance",
		streamedCheckFailed,
		[=, assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame) : -1;
			if (!frameFacts) {
				try {
					for (const auto &[attempt, crossedReadyFrame] : crossedReadyWithoutTerminal) {
						assertions.Observe(
							"crossed-ready attempt " + std::to_string(attempt) + " lacks a terminal outcome",
							false,
							crossedReadyFrame
						);
					}
				} catch (const PortalWalkRequirement &) {
					// The failed requirement is recorded; independent checks and later frames continue.
				}
				assertions.Complete(result);
				return;
			}
			try {
				const auto &facts = *frameFacts;
				const auto &sample = facts.Current->Metadata;
				if (imageHandoff && fault == PortalWalkFault::None && !firstAdoptionSeen) {
					const bool sourceReplica = sample.value("input_world", "") == "client.replica";
					if (sourceReplicaSeen && !sourceReplica) {
						firstAdoptionSeen = true;
					} else if (sourceReplica && sample.value("view_world", "") == "client.replica" &&
							   !sample.at("portal_views").empty()) {
						const auto &portal = sample.at("portal_views").at(0);
						const bool captured =
							portal.at("capture").is_object() && portal.at("image").get<uint64_t>() != 0;
						if (firstCaptureSeen) {
							WALK_CHECK(captured);
						}
						firstCaptureSeen |= captured;
					}
					sourceReplicaSeen |= sourceReplica;
				}
				if (sample.contains("portal_handoff")) {
					const auto &handoff = sample.at("portal_handoff");
					const auto attempt = handoff.at("attempt").get<uint64_t>();
					if (handoff.value("crossed", false) && handoff.value("ready", false))
						crossedReadyWithoutTerminal.try_emplace(attempt, frame);
					if (handoff.value("commit", false) || handoff.value("committed", false) ||
						handoff.value("refused", false))
						crossedReadyWithoutTerminal.erase(attempt);
				}
				if (sample.value("eye_image", false)) {
					WALK_REQUIRE(sample.contains("eye_capture"));
					WALK_REQUIRE(sample.at("eye_capture").is_object());
					// A composed eye is a new image drawn from the capture and the current body.
					if (!sample.value("eye_composed", false))
						WALK_CHECK(sample.at("eye_capture").at("image") == sample.at("eye_image_handle"));
					WALK_CHECK(sample.at("eye_capture").at("projection") == "eye");
					WALK_CHECK(sample.at("eye_capture").at("request").get<uint64_t>() != 0);
				}

			} catch (const PortalWalkRequirement &) {
				// The failed requirement is recorded; independent checks and later frames continue.
			} catch (const std::exception &error) {
				assertions.Observe("observation schema exception", false, frame, error.what());
			}
		}
	));
	scenarioChecks.push_back(PortalWalkCheck(
		"camera.clear-and-native-clock",
		streamedCheckFailed,
		[=, assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame) : -1;
			if (!frameFacts) {
				try {
					if (lateClear) {
						WALK_CHECK(sourceClearSamples > 0);
						WALK_CHECK(adoptedClearSamples >= 3);
					}
					if (imageHandoff) WALK_CHECK(nativeHeldSamples > 0);
					if (lateClear && firstPerson && fault == PortalWalkFault::None)
						WALK_CHECK(resumedNativeSamples > 0);
					WALK_CHECK(nativeWorlds.size() == 2);
				} catch (const PortalWalkRequirement &) {
					// The failed requirement is recorded; independent checks and later frames continue.
				}
				assertions.Complete(result);
				return;
			}
			try {
				const auto &facts = *frameFacts;
				const auto &sample = facts.Current->Metadata;
				const nlohmann::json *previousSample = facts.Previous ? &facts.Previous->Metadata : nullptr;
				const bool clearedFrame = lateClear && sample.value("subject", uint64_t{1}) == 0 &&
										  !sample.value("subject_automatic", true);
				if (previousSample && previousSample->contains("portal_handoff") &&
					previousSample->at("input_world") != sample.at("input_world"))
					returned |=
						previousSample->at("portal_handoff").value("destination", "") == "server.world";
				if (lateClear && firstPerson && !clearedFrame && !returned &&
					sample.value("subject_is_humanoid", false) &&
					sample.value("eye_world", "") == "walk.destination" &&
					sample.contains("native_prediction") && !sample.value("eye_image", false) &&
					sample.at("view_world") == sample.at("input_world")) {
					++resumedNativeSamples;
					const auto presented = JsonCameraFrame(sample.at("camera"));
					const auto inputCamera = JsonCameraFrame(sample.at("input_camera"));
					WALK_CHECK((presented.Position - inputCamera.Position).Magnitude() < .0001f);
					WALK_CHECK(
						std::abs(glm::dot(presented.Rotation(), inputCamera.Rotation())) > 1.0f - 1e-6f
					);
				}
				if (clearedFrame && sample.value("retained_character", false)) ++sourceClearSamples;
				if (sample.value("retained_character", false) && !sample.value("eye_image", false))
					++nativeHeldSamples;
				if (clearedFrame && sample.contains("native_prediction")) ++adoptedClearSamples;
				if (clearedFrame && previousSample && previousSample->value("subject", uint64_t{1}) == 0 &&
					previousSample->value("eye_world", "") == sample.value("eye_world", "")) {
					float displacement = 0, rotationDot = 0;
					for (size_t axis = 0; axis < 3; ++axis) {
						const float delta = sample.at("camera").at("position").at(axis).get<float>() -
											previousSample->at("camera").at("position").at(axis).get<float>();
						displacement += delta * delta;
					}
					for (size_t axis = 0; axis < 4; ++axis)
						rotationDot += sample.at("camera").at("rotation").at(axis).get<float>() *
									   previousSample->at("camera").at("rotation").at(axis).get<float>();
					WALK_CHECK(displacement < .000001f);
					WALK_CHECK(std::abs(rotationDot) > .99999f);
				}
				if (sample.value("loading_portals", false) && sample.contains("predicted_velocity")) {
					const auto &velocity = sample.at("predicted_velocity");
					WALK_CHECK(std::abs(velocity.at(0).get<float>()) < .001f);
					WALK_CHECK(std::abs(velocity.at(2).get<float>()) < .001f);
				}
				if (sample.contains("native_prediction")) {
					const auto &native = sample.at("native_prediction");
					WALK_CHECK(
						native.at("applied_input_tick").get<uint64_t>() <=
						sample.at("submitted_move_tick").get<uint64_t>()
					);
					if (native.at("applied_pose_tick").get<uint64_t>() != 0) {
						const auto world = sample.at("input_world").get<std::string>();
						if (std::find(nativeWorlds.begin(), nativeWorlds.end(), world) == nativeWorlds.end())
							nativeWorlds.push_back(world);
					}
				}

			} catch (const PortalWalkRequirement &) {
				// The failed requirement is recorded; independent checks and later frames continue.
			} catch (const std::exception &error) {
				assertions.Observe("observation schema exception", false, frame, error.what());
			}
		}
	));
	scenarioChecks.push_back(PortalWalkCheck(
		"handoff.motion-and-capture-continuity",
		streamedCheckFailed,
		[=, assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame) : -1;
			if (!frameFacts) {
				try {
					if (lateClear) WALK_CHECK(clearedAdoptions == 1);
					if (spatialOverlay) WALK_CHECK(spatialOverlayAdoptions == 1);
					WALK_CHECK(handoffMoveSamples > 0);
					WALK_CHECK(outboundMotion);
					WALK_CHECK(returnMotion);
				} catch (const PortalWalkRequirement &) {
					// The failed requirement is recorded; independent checks and later frames continue.
				}
				assertions.Complete(result);
				return;
			}
			try {
				const auto &facts = *frameFacts;
				const auto &sample = facts.Current->Metadata;
				const nlohmann::json *previousSample = facts.Previous ? &facts.Previous->Metadata : nullptr;
				const bool clearedFrame = lateClear && sample.value("subject", uint64_t{1}) == 0 &&
										  !sample.value("subject_automatic", true);
				if (previousSample && previousSample->contains("portal_handoff") &&
					previousSample->at("input_world") != sample.at("input_world")) {
					returned = returned || previousSample->at("portal_handoff").value("destination", "") ==
											   "server.world";
					WALK_CHECK(sample.contains("predicted_root"));
					if (!firstPerson && !lateClear &&
						previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
						// These rooms have only orange and blue floors. Yellow belongs to
						// the avatar and must survive the source replica's retirement.
						for (const int bodyFrame : {frame - 1, frame}) {
							PortalWalkSurface body(bodyFrame == frame ? facts.Current : facts.Previous);
							WALK_REQUIRE(body);
							size_t yellowPixels = 0;
							bool readable = true;
							for (int y = 0; y < body->h; ++y) {
								for (int x = 0; x < body->w; ++x) {
									Uint8 red = 0, green = 0, blue = 0, alpha = 0;
									readable &=
										SDL_ReadSurfacePixel(body.get(), x, y, &red, &green, &blue, &alpha);
									yellowPixels += red > 90 && green > 90 && blue < 70 &&
													red > .7f * green && red < 1.4f * green;
								}
							}
							WALK_REQUIRE(readable);
							WALK_CHECK(yellowPixels > 0);
						}
					}
					if (lateClear &&
						previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
						WALK_CHECK(clearedFrame);
						++clearedAdoptions;
					}
					if (spatialOverlay &&
						previousSample->at("portal_handoff").value("destination", "") == "walk.destination") {
						++spatialOverlayAdoptions;
						for (const int markerFrame : {frame - 1, frame}) {
							const auto &markerSample = markerFrame == frame ? sample : *previousSample;
							const auto &portals = markerSample.at("portal_views");
							WALK_REQUIRE(portals.size() == 1);
							const auto &capture = portals.at(0).at("capture");
							WALK_REQUIRE(capture.is_object());
							if (markerFrame != frame) {
								const auto &handoff = previousSample->at("portal_handoff");
								if (handoff.at("proceed_eye_source_remote").get<bool>()) {
									WALK_CHECK(capture.at("producer") == "walk.destination");
									WALK_CHECK(capture.at("scope") == "opaque-lighting");
									WALK_CHECK(capture.at("spatial_overlay_image").get<uint64_t>() != 0);
									if (roomShader == RoomShader::Lens) {
										WALK_CHECK(capture.at("lens_count") == 1);
										WALK_REQUIRE(capture.at("lenses").size() == 1);
										WALK_CHECK(capture.at("lenses").at(0).at("shader") == "RoomLens");
										WALK_CHECK(
											capture.at("lenses").at(0).at("program_hash") !=
											std::string(64, '0')
										);
										WALK_CHECK(std::isfinite(capture.at("lens_time").get<float>()));
									}
								} else {
									// Local replicas bake spatial UI and lenses into the complete capture.
									WALK_CHECK(capture.at("producer") == handoff.at("proceed_eye_source"));
									WALK_CHECK(capture.at("producer") == sample.at("input_world"));
									WALK_CHECK(capture.at("scope") == "complete-world");
									WALK_CHECK(capture.at("spatial_overlay_image").get<uint64_t>() == 0);
									WALK_CHECK(capture.at("lens_count") == 0);
									WALK_CHECK(capture.at("lenses").empty());
								}
							}
							const auto &position = capture.at("position");
							core::CFrame camera({position.at(0), position.at(1), position.at(2)});
							const auto &orientation = capture.at("orientation");
							camera.QuaternionX = orientation.at(0);
							camera.QuaternionY = orientation.at(1);
							camera.QuaternionZ = orientation.at(2);
							camera.QuaternionW = orientation.at(3);
							const auto &frustum = capture.at("frustum");
							PortalWalkSurface image(markerFrame == frame ? facts.Current : facts.Previous);
							WALK_REQUIRE(image);
							// Sample the marker's interior through the accepted destination camera.
							for (const float markerX : {1.6f, 2.0f, 2.4f}) {
								const auto local = camera.PointToObjectSpace({markerX, 5.0f, -13.95f});
								WALK_REQUIRE(local.Z < 0);
								const float scale = frustum.at(4).get<float>() / -local.Z;
								const float u = (local.X * scale - frustum.at(0).get<float>()) /
												(frustum.at(1).get<float>() - frustum.at(0).get<float>());
								const float v = (local.Y * scale - frustum.at(2).get<float>()) /
												(frustum.at(3).get<float>() - frustum.at(2).get<float>());
								// Destination shifts right, then the eye world's lens shifts the portal left.
								const int shift =
									roomShader == RoomShader::Lens
										? image->w / 16 -
											  (markerSample.at("presented_eye_world") == "server.world"
												   ? image->w / 32
												   : 0)
										: 0;
								const int x = static_cast<int>(u * image->w) + shift;
								const int y = static_cast<int>((1 - v) * image->h);
								WALK_REQUIRE(x >= 0);
								WALK_REQUIRE(x < image->w);
								WALK_REQUIRE(y >= 0);
								WALK_REQUIRE(y < image->h);
								Uint8 red = 0, green = 0, blue = 0, alpha = 0;
								WALK_REQUIRE(
									SDL_ReadSurfacePixel(image.get(), x, y, &red, &green, &blue, &alpha)
								);
								WALK_CHECK(green > 90);
								WALK_CHECK(green > 2 * red);
								WALK_CHECK(green > 2 * blue);
								if (roomShader == RoomShader::Lens && markerX == 1.6f) {
									// The interior sample can overlap the shifted marker as the camera
									// approaches. Use a distinct point close to its original left edge for
									// the vacated check.
									const auto edge = camera.PointToObjectSpace({1.2f, 5.0f, -13.95f});
									const float edgeScale = frustum.at(4).get<float>() / -edge.Z;
									const float edgeU =
										(edge.X * edgeScale - frustum.at(0).get<float>()) /
										(frustum.at(1).get<float>() - frustum.at(0).get<float>());
									const int vacatedX = static_cast<int>(edgeU * image->w);
									WALK_REQUIRE(SDL_ReadSurfacePixel(
										image.get(), vacatedX, y, &red, &green, &blue, &alpha
									));
									WALK_CHECK_FALSE((green > 90 && green > 2 * red && green > 2 * blue));
								}
							}
						}
					}
					if (imageHandoff && fault == PortalWalkFault::None) {
						WALK_REQUIRE(
							previousSample->value("presented_eye_world", "") ==
							sample.value("presented_eye_world", "")
						);
						const auto bluePixels =
							[&](const std::shared_ptr<const client::FrameObservation> &owned) {
								PortalWalkSurface image(owned);
								WALK_REQUIRE(image);
								size_t blue = 0;
								bool readable = true;
								for (int y = 0; y < image->h; ++y)
									for (int x = 0; x < image->w; ++x) {
										Uint8 red = 0, green = 0, colourBlue = 0, alpha = 0;
										readable &= SDL_ReadSurfacePixel(
											image.get(), x, y, &red, &green, &colourBlue, &alpha
										);
										blue += 2 * colourBlue > 3 * red && 2 * colourBlue > 3 * green;
									}
								WALK_REQUIRE(readable);
								return blue;
							};
						const auto before = bluePixels(facts.Previous);
						const auto after = bluePixels(facts.Current);
						WALK_CHECK(
							before > (previousSample->at("portal_handoff").value("destination", "") ==
											  "walk.destination"
										  ? 512
										  : 64)
						);
						// Allow the moving destination character to obscure part of the floor.
						// A plain pane or the source floor cannot supply this blue landmark.
						WALK_CHECK(after >= before / 2);
					}
					WALK_CHECK(
						sample.at("submitted_move_tick").get<uint64_t>() >=
						previousSample->at("submitted_move_tick").get<uint64_t>()
					);
				}
				if (previousSample && previousSample->contains("portal_handoff") &&
					sample.contains("portal_handoff") &&
					previousSample->at("portal_handoff").value("proceed", false) &&
					sample.at("portal_handoff").value("proceed", false) &&
					previousSample->at("input_world") == sample.at("input_world") &&
					sample.at("tick").get<uint64_t>() > previousSample->at("tick").get<uint64_t>()) {
					WALK_CHECK(
						sample.at("submitted_move_tick").get<uint64_t>() >
						previousSample->at("submitted_move_tick").get<uint64_t>()
					);
					++handoffMoveSamples;
				}
				if (sample.contains("portal_handoff") &&
					sample.at("portal_handoff").contains("completed_motion")) {
					const auto &handoff = sample.at("portal_handoff");
					const auto &motion = handoff.at("completed_motion");
					WALK_CHECK(
						motion.at("input_tick").get<uint64_t>() <=
						sample.at("submitted_move_tick").get<uint64_t>()
					);
					WALK_CHECK(motion.at("destination_tick").get<uint64_t>() != 0);
					WALK_CHECK(motion.at("destination_incarnation").get<uint64_t>() != 0);
					outboundMotion |= handoff.value("destination", "") == "walk.destination";
					returnMotion |= handoff.value("destination", "") == "server.world";
				}
				if (sample.contains("portal_input_history")) {
					const auto &history = sample.at("portal_input_history");
					WALK_CHECK(history.at("last_recorded_tick") == sample.at("submitted_move_tick"));
					WALK_CHECK(history.at("retained").get<size_t>() <= client::PortalInputHistory::CAPACITY);
					WALK_CHECK(history.at("discarded").get<uint64_t>() == 0);
					if (history.at("applied_destination_tick").get<uint64_t>() != 0) {
						WALK_REQUIRE(sample.contains("portal_handoff"));
						const auto destination = sample.at("portal_handoff").value("destination", "");
						WALK_CHECK(sample.value("prediction_authority_world", "") == destination);
					}
				}

			} catch (const PortalWalkRequirement &) {
				// The failed requirement is recorded; independent checks and later frames continue.
			} catch (const std::exception &error) {
				assertions.Observe("observation schema exception", false, frame, error.what());
			}
		}
	));
	scenarioChecks.push_back(PortalWalkCheck(
		"image.body-content-and-camera",
		streamedCheckFailed,
		[=, assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			auto &lastFrame = *terminalFrame;
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame)
										 : (lastFrame ? static_cast<int>(lastFrame->ClientFrame) : -1);
			if (!frameFacts) {
				const size_t failuresBeforeComplete = assertions.FailureCount();
				try {
					WALK_CHECK(retainedMoveSamples > 0);
					assertions.Observe(
						"maximumEyeHeightError < .05f",
						maximumEyeHeightError < .05f,
						maximumEyeHeightFrame,
						"maximum_error=" + std::to_string(maximumEyeHeightError)
					);
					WALK_CHECK(explicitSubjectSeen == explicitSubject);
					if (fault != PortalWalkFault::None) {
						WALK_CHECK(faultVisibleFrames > 16);
						WALK_CHECK(faultBodyFrames > 0);
					}
					if (authoredShaders) {
						WALK_CHECK(shaderRoomSamples[0] > 10);
						WALK_CHECK(shaderRoomSamples[1] > 10);
						WALK_CHECK(shaderObservedSamples > 0);
					}
					WALK_REQUIRE(lastFrame);
					if (lastFrame) {
						WALK_CHECK(lastFrame->Metadata.value("eye_world", "") == "server.world");
						PortalWalkSurface capture(lastFrame);
						WALK_REQUIRE(capture);
						WALK_CHECK(capture->w == options.Width);
						WALK_CHECK(capture->h == options.Height);
						size_t visiblePixels = 0;
						bool readable = true;
						for (int y = 0; y < capture->h; ++y)
							for (int x = 0; x < capture->w; ++x) {
								Uint8 red = 0, green = 0, blue = 0, alpha = 0;
								readable &=
									SDL_ReadSurfacePixel(capture.get(), x, y, &red, &green, &blue, &alpha);
								visiblePixels += red != 0 || green != 0 || blue != 0;
							}
						WALK_CHECK(readable);
						pixelSummary->Diagnostics["terminal_frame"] = frame;
						pixelSummary->Diagnostics["terminal_visible_pixels"] = visiblePixels;
						pixelSummary->Diagnostics["terminal_area"] = options.Width * options.Height;
						assertions.Observe(
							"visiblePixels > static_cast<size_t>(options.Width * options.Height) / 8",
							visiblePixels > static_cast<size_t>(options.Width * options.Height) / 8,
							frame,
							"visible_pixels=" + std::to_string(visiblePixels) +
								" area=" + std::to_string(options.Width * options.Height)
						);
					}
					pixelSummary->AwaitingImageBodyFrames = awaitingImageBodyFrames;
					pixelSummary->FirstContactFrames = firstContactFrames;
				} catch (const PortalWalkRequirement &) {
					// The failed requirement is recorded; independent checks and later frames continue.
				} catch (const std::exception &error) {
					assertions.Observe("terminal observation schema exception", false, frame, error.what());
				}
				pixelSummary->CompleteFailed = assertions.FailureCount() > failuresBeforeComplete;
				assertions.Complete(result);
				lastFrame.reset();
				return;
			}
			lastFrame = frameFacts->Current;
			try {
				const auto &facts = *frameFacts;
				const auto &sample = facts.Current->Metadata;
				const nlohmann::json *previousSample = facts.Previous ? &facts.Previous->Metadata : nullptr;
				const bool clearedFrame = lateClear && sample.value("subject", uint64_t{1}) == 0 &&
										  !sample.value("subject_automatic", true);
				characterSeen |= sample.value("subject_is_humanoid", false);
				if (sample.value("subject_is_humanoid", false)) {
					WALK_CHECK(sample.at("far").get<float>() == scene::Camera{}.FarPlane);
					explicitSubjectSeen |= !sample.at("subject_automatic").get<bool>();
					if (!explicitSubject || explicitSubjectSeen)
						WALK_CHECK(sample.at("subject_automatic").get<bool>() == !explicitSubject);
				}
				if (characterSeen) {
					WALK_CHECK((sample.value("subject_is_humanoid", false) || clearedFrame));
					if (firstPerson && !clearedFrame) {
						WALK_REQUIRE(sample.contains("player_user_id"));
						if (previousSample && IsTrailingSourceView(sample, *previousSample)) {
							WALK_CHECK_FALSE(sample.contains("eye_player"));
							WALK_CHECK(sample.at("eye_rig") == 0);
						} else {
							WALK_CHECK(sample.contains("eye_player"));
							if (sample.contains("eye_player"))
								WALK_CHECK(sample.at("eye_player") == sample.at("player_user_id"));
						}
					} else {
						WALK_CHECK_FALSE(sample.contains("eye_player"));
					}
					if (sample.value("retained_character", false)) {
						WALK_CHECK(sample.contains("retained_root"));
						WALK_CHECK_FALSE(sample.contains("authoritative_root"));
						WALK_REQUIRE(sample.contains("predicted_root"));
						if (previousSample && previousSample->value("retained_character", false) &&
							previousSample->value("input_world", "") == sample.value("input_world", "")) {
							const auto &before =
								previousSample->at("presented_predicted_root").at("position");
							const auto &after = sample.at("presented_predicted_root").at("position");
							float displacement = 0;
							for (size_t axis = 0; axis < 3; ++axis) {
								const float delta =
									after.at(axis).get<float>() - before.at(axis).get<float>();
								displacement += delta * delta;
							}
							const auto &velocity = sample.at("predicted_velocity");
							float speedSquared = 0;
							for (const auto &axis : velocity)
								speedSquared += axis.get<float>() * axis.get<float>();
							// Presentation time is the tick plus alpha. A replica clock that drops its
							// lead shows the same instant twice, which is not a held body stopping.
							const double presentedAt =
								sample.at("tick").get<double>() + sample.at("alpha").get<double>();
							const double presentedBefore = previousSample->at("tick").get<double>() +
														   previousSample->at("alpha").get<double>();
							if (speedSquared > 1 && presentedAt > presentedBefore + .1 &&
								sample.at("submitted_move_tick") >
									previousSample->at("submitted_move_tick")) {
								WALK_CHECK(displacement > 0.0001f);
								++retainedMoveSamples;
							}
						}
					}
					const float yaw = sample.at("control_angles").at(1).get<float>();
					if (previousYaw) {
						WALK_CHECK(std::abs(yaw - *previousYaw) < .03f);
					}
					previousYaw = yaw;
					const bool firstContact =
						impairment.Active() && !sample.value("portal_destinations_seen", true);
					firstContactFrames += firstContact;
					// Foreign eyes render in the persistent viewport world. A missing reply
					// must fail even when no frame ever claims to have a ready image.
					if (fault == PortalWalkFault::None && !firstContact &&
						sample.value("view_world", "") == "client.world") {
						WALK_CHECK(sample.value("eye_image", false));
					}
					// The eye stays a fixed head offset above the presented player. A world
					// Y value is not invariant once that player moves through a portal. A
					// cleared subject holds the camera still (checked above) while the
					// player walks on, so those frames have no head offset to keep.
					if (!clearedFrame && sample.contains("presented_predicted_root")) {
						const float eyeHeight = sample.at("camera").at("position").at(1).get<float>();
						const float rootHeight =
							sample.at("presented_predicted_root").at("position").at(1).get<float>();
						const float eyeHeightError = std::abs(eyeHeight - rootHeight - 1.5f);
						if (eyeHeightError > maximumEyeHeightError) {
							maximumEyeHeightError = eyeHeightError;
							maximumEyeHeightFrame = frame;
							auto peak = nlohmann::json::object();
							for (const char *key :
								 {"camera",
								  "input_camera",
								  "control_angles",
								  "control_basis",
								  "head_offset",
								  "predicted_root",
								  "presented_predicted_root",
								  "tick",
								  "alpha",
								  "eye_world",
								  "input_world",
								  "eye_from_input",
								  "eye_origin",
								  "eye_scale",
								  "native_prediction"}) {
								if (sample.contains(key)) peak[key] = sample.at(key);
							}
							const auto peakText = peak.dump();
							pixelSummary->Diagnostics["peak_height_metadata"] =
								peakText.size() <= 8192 ? peakText : "peak metadata exceeds diagnostic bound";
							pixelSummary->Diagnostics["maximum_eye_height_error"] = maximumEyeHeightError;
							pixelSummary->Diagnostics["maximum_eye_height_frame"] = maximumEyeHeightFrame;
						}
						assertions.Observe(
							"eye height error < .05f",
							eyeHeightError < .05f,
							frame,
							"error=" + std::to_string(eyeHeightError) + " eye_y=" +
								std::to_string(eyeHeight) + " root_y=" + std::to_string(rootHeight)
						);
					}
					PortalWalkSurface eye(facts.Current);
					WALK_REQUIRE(eye);
					if (authoredShaders || (ownedContent && sample.contains("observed_world"))) {
						Uint8 roomRed = 0, roomGreen = 0, roomBlue = 0, roomAlpha = 0;
						WALK_REQUIRE(SDL_ReadSurfacePixel(
							eye.get(), eye->w / 8, eye->h - 2, &roomRed, &roomGreen, &roomBlue, &roomAlpha
						));
						const auto eyeWorld = sample.value("observed_world", sample.value("eye_world", ""));
						bool blueRoom = eyeWorld == "walk.destination";
						const auto vector = [](const auto &values) {
							return core::Vector3{
								values.at(0).template get<float>(),
								values.at(1).template get<float>(),
								values.at(2).template get<float>()
							};
						};
						core::CFrame camera(vector(sample.at("camera").at("position")));
						const auto &rotation = sample.at("camera").at("rotation");
						camera.QuaternionX = rotation.at(0);
						camera.QuaternionY = rotation.at(1);
						camera.QuaternionZ = rotation.at(2);
						camera.QuaternionW = rotation.at(3);
						for (const auto &portal : sample.at("portal_views")) {
							if (!portal.contains("capture") || !portal.at("capture").is_object()) continue;
							const auto &frustum = portal.at("capture").at("frustum");
							const float u = (eye->w / 8 + .5f) / eye->w;
							const float v = 1.f - (eye->h - 1.5f) / eye->h;
							const float near = frustum.at(4);
							const auto ray = camera.VectorToWorldSpace(
								{std::lerp(frustum.at(0).get<float>(), frustum.at(1).get<float>(), u) / near,
								 std::lerp(frustum.at(2).get<float>(), frustum.at(3).get<float>(), v) / near,
								 -1}
							);
							const auto normal = vector(portal.at("normal"));
							const float denominator = normal.Dot(ray);
							if (ray.Y >= 0 || std::abs(denominator) < 1e-6f) continue;
							const auto centre = vector(portal.at("centre"));
							const float distance = (centre - camera.Position).Dot(normal) / denominator;
							const auto offset = camera.Position + ray * distance - centre;
							const auto first = vector(portal.at("first")),
									   second = vector(portal.at("second"));
							// The room behind an aperture supplies this pixel if its plane precedes the
							// floor.
							if (distance > 0 && distance < -camera.Position.Y / ray.Y &&
								std::abs(offset.Dot(first)) < first.Dot(first) &&
								std::abs(offset.Dot(second)) < second.Dot(second))
								blueRoom = !blueRoom;
						}
						if (authoredShaders) {
							++shaderRoomSamples[blueRoom ? 1 : 0];
							shaderObservedSamples += sample.contains("observed_world");
						}
						if (blueRoom)
							WALK_CHECK(roomBlue > roomRed * 2);
						else
							WALK_CHECK(roomRed > roomBlue * 2);
					}
					Uint8 red = 0, green = 0, blue = 0, alpha = 0;
					bool visible = false;
					bool readable = true;
					const auto bodyInputWorld = sample.value("input_world", "");
					const auto bodyEyeWorld = sample.value("eye_world", "");
					const auto bodyRasterWorld = sample.value("presented_eye_world", "");
					// The explicit trailing-source override can rasterize a different chart
					// from camera history. Its existing checks own that interval.
					const bool sameEyeChart = !bodyEyeWorld.empty() && bodyEyeWorld == bodyRasterWorld;
					const bool sourceBody = bodyInputWorld == "client.replica";
					const bool adoptedSubject = !firstPerson && !clearedFrame && sameEyeChart &&
												sample.contains("native_prediction") &&
												sample.value("subject_is_humanoid", false);
					const bool followingArmCrosses = adoptedSubject && FollowingArmCrossesPortal(sample);
					const bool adoptedBody = adoptedSubject && !followingArmCrosses;
					const bool expectBody = !firstPerson && !clearedFrame && (sourceBody || adoptedBody);
					size_t yellowPixels = 0;
					for (int y = eye->h - 1; y >= 0 && (!visible || expectBody); --y) {
						for (int x = 0; x < eye->w && (!visible || expectBody); ++x) {
							readable &= SDL_ReadSurfacePixel(eye.get(), x, y, &red, &green, &blue, &alpha);
							visible |= red != 0 || green != 0 || blue != 0;
							yellowPixels += red > 90 && green > 90 && blue < 70 && red > .7f * green &&
											red < 1.4f * green;
						}
					}
					WALK_REQUIRE(readable);
					if (fault == PortalWalkFault::None && !firstContact) WALK_CHECK(visible);
					if (fault != PortalWalkFault::None && visible) ++faultVisibleFrames;
					// Prediction can carry the body fully beyond the plane before its
					// authority changes worlds. Keep checking the adopted body too, except
					// while its following camera alone looks through a finite aperture.
					if (expectBody && fault == PortalWalkFault::None) {
						// Under impairment the destination image can arrive after the body has
						// passed the plane. That frame must name the state, not hide it.
						const auto &views = sample.at("portal_views");
						const bool awaitingImage =
							std::any_of(views.begin(), views.end(), [](const auto &portal) {
								return portal.value("presentation", "") == "awaiting-image";
							});
						if (impairment.Active() && (awaitingImage || firstContact) && yellowPixels == 0)
							++awaitingImageBodyFrames;
						else
							WALK_CHECK((yellowPixels > 0 || HasProjectedHeadSkin(eye.get(), sample)));
					}
					if (fault != PortalWalkFault::None && expectBody && yellowPixels > 0) ++faultBodyFrames;
					if (sample.value("eye_image", false)) {
						WALK_REQUIRE(SDL_ReadSurfacePixel(
							eye.get(), eye->w / 2, eye->h - 1, &red, &green, &blue, &alpha
						));
						// Both rooms have a floor below the level eye. A resident handle
						// alone is insufficient: the bound image must reach this draw slot.
						if (!firstContact) WALK_CHECK((red != 0 || green != 0 || blue != 0));
					}
				}

			} catch (const PortalWalkRequirement &) {
				// The failed requirement is recorded; independent checks and later frames continue.
			} catch (const std::exception &error) {
				assertions.Observe("observation schema exception", false, frame, error.what());
			}
		}
	));
	scenarioChecks.push_back(PortalWalkCheck(
		"portal.demand-and-camera-mapping",
		streamedCheckFailed,
		[=, assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame) : -1;
			if (!frameFacts) {
				try {
					if (imageHandoff) {
						if (frameSchedule.empty()) WALK_CHECK(hiddenPortalSamples > 0);
						WALK_CHECK(nativeReturnSamples > 16);
					}
					if (imageHandoff && !firstPerson) WALK_CHECK(observedEyeSamples > 0);
					if (fault != PortalWalkFault::None) {
						WALK_CHECK(exactFaultCameraSamples > 0);
						WALK_CHECK(faultClipSamples > 16);
					}
				} catch (const PortalWalkRequirement &) {
					// The failed requirement is recorded; independent checks and later frames continue.
				}
				assertions.Complete(result);
				return;
			}
			try {
				const auto &facts = *frameFacts;
				const auto &sample = facts.Current->Metadata;
				const nlohmann::json *previousSample = facts.Previous ? &facts.Previous->Metadata : nullptr;
				if (previousSample && previousSample->contains("portal_handoff") &&
					previousSample->at("input_world") != sample.at("input_world"))
					returned |=
						previousSample->at("portal_handoff").value("destination", "") == "server.world";
				if (imageHandoff)
					for (const auto &portal : sample.at("portal_views")) {
						if (!portal.value("external", false)) continue;
						const auto status = portal.value("demand_status", "ready");
						if (status == "ready") continue;
						WALK_REQUIRE(status == "hidden");
						const auto reason = portal.value("hidden_reason", "none");
						WALK_REQUIRE((reason == "frustum" || reason == "clip_plane"));
						WALK_CHECK(portal.value("frustum_visible", false) == (reason == "clip_plane"));
						const auto &centre = portal.at("centre");
						if (reason == "clip_plane") {
							const auto &warpJson = portal.at("warp");
							scene::SeamTransform warp{
								.Frame = JsonCameraFrame(warpJson.at("frame")),
								.Origin = JsonVector(warpJson.at("origin")),
								.Scale = warpJson.at("scale").get<float>()
							};
							const auto camera = JsonCameraFrame(portal.at("demand_camera"));
							const auto sourceCentre = JsonVector(centre);
							const auto sourceNormal = JsonVector(portal.at("normal"));
							const auto side = (camera.Position - sourceCentre).Dot(sourceNormal);
							const auto normal = warp.Rotate(sourceNormal) * (side >= 0 ? -1.0f : 1.0f);
							const auto point = warp.Point(sourceCentre) -
											   normal * scene::PortalClipBias(warp.Length(std::abs(side)));
							WALK_CHECK(normal.Dot(warp.Place(camera).Position) - normal.Dot(point) >= -1e-4f);
						}
						++hiddenPortalSamples;
					}
				if (fault != PortalWalkFault::None)
					for (const auto &portal : sample.at("portal_views")) {
						if (!portal.value("external", false) || !ReadyPortalDemand(portal) ||
							!portal.contains("capture") || !portal.at("capture").is_object())
							continue;
						const auto &clip = portal.at("capture").at("clip_plane");
						WALK_CHECK(clip.size() == 4);
						const float normal = std::abs(clip.at(0).get<float>()) +
											 std::abs(clip.at(1).get<float>()) +
											 std::abs(clip.at(2).get<float>());
						WALK_CHECK(normal > .99f);
						++faultClipSamples;
					}
				if (imageHandoff && sample.contains("observed_world")) {
					WALK_CHECK_FALSE(sample.value("eye_image", true));
					WALK_CHECK(sample.at("view_world") != sample.at("input_world"));
					WALK_CHECK(sample.at("observed_tick").get<uint64_t>() > 0);
					WALK_CHECK(sample.at("instances").get<size_t>() > 0);
					for (const auto &portal : sample.at("portal_views")) {
						if (!portal.value("external", false) || !ReadyPortalDemand(portal)) continue;
						WALK_REQUIRE(portal.contains("capture"));
						if (fault != PortalWalkFault::None && !portal.at("capture").is_object()) continue;
						WALK_REQUIRE(portal.at("capture").is_object());
						WALK_CHECK(portal.at("capture").at("producer") == sample.at("input_world"));
					}
					++observedEyeSamples;
					if (sample.value("observed_successor", false)) {
						WALK_REQUIRE(sample.contains("portal_handoff"));
						const auto &handoff = sample.at("portal_handoff");
						WALK_CHECK(handoff.value("scene_admitted", false));
						WALK_CHECK(handoff.value("scene_joined", false));
						WALK_CHECK(handoff.value("scene_live", false));
						WALK_CHECK_FALSE(handoff.value("scene_rejected", true));
						WALK_CHECK_FALSE(handoff.value("refused", true));
						WALK_CHECK(handoff.at("failure").get<std::string>().empty());
						WALK_CHECK(sample.at("observed_world") == handoff.at("destination"));
						if (handoff.value("drawing_player", false)) WALK_CHECK(handoff.value("ready", false));
					}
				}
				if (imageHandoff && !sample.value("eye_image", false) &&
					(sample.contains("observed_world") || returned)) {
					for (const auto &portal : sample.at("portal_views")) {
						if (!portal.value("external", false) || !ReadyPortalDemand(portal)) continue;
						const auto rasterCamera = JsonCameraFrame(sample.at("camera"));
						const auto demandCamera = JsonCameraFrame(portal.at("demand_camera"));
						WALK_CHECK((rasterCamera.Position - demandCamera.Position).Magnitude() < .0001f);
						WALK_CHECK(
							std::abs(glm::dot(rasterCamera.Rotation(), demandCamera.Rotation())) >
							1.0f - 1e-6f
						);
						WALK_REQUIRE(portal.contains("capture"));
						if (fault != PortalWalkFault::None && !portal.at("capture").is_object()) continue;
						WALK_REQUIRE(portal.at("capture").is_object());
						const auto &capturedEye = portal.at("capture").at("position");
						const auto capturedRevision =
							portal.at("capture").at("camera_revision").get<uint64_t>();
						const auto demandRevision = portal.at("demand_camera_revision").get<uint64_t>();
						const auto &capturedRotation = portal.at("capture").at("orientation");
						if (capturedRevision == demandRevision) {
							const auto &warpJson = portal.at("warp");
							const scene::SeamTransform warp{
								.Frame = JsonCameraFrame(warpJson.at("frame")),
								.Origin = JsonVector(warpJson.at("origin")),
								.Scale = warpJson.at("scale").get<float>()
							};
							const auto expected = warp.Place(JsonCameraFrame(portal.at("demand_camera")));
							const auto positionError = JsonVector(capturedEye) - expected.Position;
							WALK_CHECK(positionError.Dot(positionError) < 0.0001f * 0.0001f);
							float rotationDot = 0;
							const auto expectedRotation = expected.Rotation();
							for (size_t axis = 0; axis < 4; ++axis)
								rotationDot +=
									capturedRotation.at(axis).get<float>() * expectedRotation[axis];
							WALK_CHECK(std::abs(rotationDot) > 1.0f - 1e-6f);
						} else {
							WALK_CHECK(portal.at("capture").at("accepted_age_ms").get<float>() < 250.0f);
						}
						if (fault != PortalWalkFault::None) ++exactFaultCameraSamples;
					}
				}
				if (imageHandoff && returned && !sample.value("eye_image", false)) {
					for (const auto &portal : sample.at("portal_views")) {
						if (!portal.value("external", false) || !ReadyPortalDemand(portal)) continue;
						WALK_CHECK(portal.at("image").get<uint64_t>() != 0);
						WALK_REQUIRE(portal.contains("capture"));
						WALK_CHECK(portal.at("capture").at("scope") == "complete-world");
						++nativeReturnSamples;
					}
				}
			} catch (const PortalWalkRequirement &) {
				// The failed requirement is recorded; independent checks and later frames continue.
			} catch (const std::exception &error) {
				assertions.Observe("observation schema exception", false, frame, error.what());
			}
		}
	));
	scenarioChecks.push_back(PortalWalkCheck(
		"frame.owned-rgba-provenance",
		streamedCheckFailed,
		[generation = uint64_t{0},
		 count = uint64_t{0},
		 rendererFrame = uint64_t{0},
		 assertions = PortalWalkAssertions{streamedCheckFailed}](
			const PortalWalkFrame *frameFacts, testing::CheckResult &result
		) mutable {
			const int frame = frameFacts ? static_cast<int>(frameFacts->Current->ClientFrame) : -1;
			if (!frameFacts) {
				assertions.Observe("required frame stream", count > 0, frame);
				assertions.Complete(result);
				return;
			}
			const auto &owned = *frameFacts->Current;
			if (!generation) generation = owned.Generation;
			assertions.Observe("contiguous client frame", owned.ClientFrame == count++, frame);
			assertions.Observe(
				"subscription generation", owned.Generation != 0 && owned.Generation == generation, frame
			);
			assertions.Observe("metadata frame", owned.Metadata.at("frame") == owned.ClientFrame, frame);
			const auto &image = owned.Image;
			assertions.Observe("canonical complete RGBA", client::tests::CanonicalImage(image), frame);
			assertions.Observe(
				"exact capture dimensions",
				image.Width == 128 && image.Height == 128 && image.RowStride == 128 * 4 &&
					image.Pixels.size() == 128 * 128 * 4,
				frame
			);
			assertions.Observe(
				"capture world",
				image.WorldName == owned.Metadata.value("view_world", "") && !image.WorldName.empty(),
				frame
			);
			assertions.Observe(
				"capture snapshot", image.SnapshotId == owned.Metadata.value("snapshot_id", ""), frame
			);
			assertions.Observe(
				"capture pipeline",
				image.Pipeline.Text() == owned.Pipeline && image.PipelineRevision != 0,
				frame
			);
			assertions.Observe("capture native token", image.Token != 0 && image.Frame != 0, frame);
			assertions.Observe("increasing renderer frame", image.Frame > rendererFrame, frame);
			rendererFrame = image.Frame;
			assertions.Observe(
				"metadata generation",
				owned.Metadata.value("observation_generation", uint64_t{0}) == generation,
				frame
			);
			assertions.Observe(
				"capture view slot", image.ViewSlot == owned.Metadata.value("view_slot", size_t(-1)), frame
			);
			const auto &metadataCamera = owned.Metadata.at("camera");
			const auto &position = metadataCamera.at("position");
			const auto &rotation = metadataCamera.at("rotation");
			// Both values copy the accepted view. Compare their stored components;
			// an angular metric can report rounding error even for identical quaternions.
			assertions.Observe(
				"capture camera pose",
				image.CameraFrame.Position.X == position.at(0).get<float>() &&
					image.CameraFrame.Position.Y == position.at(1).get<float>() &&
					image.CameraFrame.Position.Z == position.at(2).get<float>() &&
					image.CameraFrame.QuaternionX == rotation.at(0).get<float>() &&
					image.CameraFrame.QuaternionY == rotation.at(1).get<float>() &&
					image.CameraFrame.QuaternionZ == rotation.at(2).get<float>() &&
					image.CameraFrame.QuaternionW == rotation.at(3).get<float>(),
				frame
			);
		}
	));
	testing::ScenarioKey scenarioKey;
	scenarioKey.Scene = "portal-client-walk";
	scenarioKey.ContentRevision = sceneSource;
	scenarioKey.Configuration = nlohmann::json{
		{"tick_rate", worldTickRate},
		{"first_person", firstPerson},
		{"explicit_subject", explicitSubject},
		{"held_input", holdThroughAdoption},
		{"late_clear", lateClear},
		{"image_handoff", imageHandoff},
		{"warm_portal", warmPortal},
		{"owned_content", ownedContent},
		{"shader", roomShader}
	}.dump();
	scenarioKey.Seed = impairment.Seed;
	scenarioKey.TickSchedule = "native-fixed-tick=" + std::to_string(worldTickRate);
	scenarioKey.FrameSchedule =
		nlohmann::json{{"rate", captureFrameRate}, {"schedule", frameSchedule}}.dump();
	scenarioKey.Actions = "portal-walk-owner-controls-one-completed-frame-lag.v1";
	scenarioKey.Transport = "native-client-server";
	scenarioKey.Faults = nlohmann::json{
		{"fault", fault},
		{"rtt", impairment.RoundTripSeconds},
		{"jitter", impairment.JitterSeconds},
		{"loss", impairment.LossChance},
		{"duplicate", impairment.DuplicateChance},
		{"reorder", impairment.ReorderChance}
	}.dump();
	scenarioKey.CaptureConfiguration = "all-frames:128x128:canonical-rgba:no-disk";
	scenarioKey.ObservationSchema = "client.completed-frame.v1";
	scenarioKey.CheckSet = "client.portalwalk.preserved-oracles.v1";
	testing::ScenarioOptions scenarioOptions;
	scenarioOptions.Timeout = std::chrono::seconds(WALK_TIMEOUT_SECONDS + 10);
	scenarioOptions.MaximumRetainedSamples = 1;
	scenarioOptions.MaximumRetainedBytes = 32u * 1024u * 1024u;
	scenarioOptions.MaximumObservationBytes = 32u * 1024u * 1024u;
	const nlohmann::json failureVariant{
		{"scene", scenarioKey.Scene},
		{"content_revision", scenarioKey.ContentRevision},
		{"configuration", scenarioKey.Configuration},
		{"seed", scenarioKey.Seed},
		{"tick_schedule", scenarioKey.TickSchedule},
		{"frame_schedule", scenarioKey.FrameSchedule},
		{"actions", scenarioKey.Actions},
		{"transport", scenarioKey.Transport},
		{"faults", scenarioKey.Faults},
		{"capture", scenarioKey.CaptureConfiguration},
		{"schema", scenarioKey.ObservationSchema},
		{"checks", scenarioKey.CheckSet}
	};
	uint64_t variantHash = 14695981039346656037ull;
	for (const unsigned char byte : failureVariant.dump()) {
		variantHash ^= byte;
		variantHash *= 1099511628211ull;
	}
	const auto failureDirectory = core::Paths::Base() / "portal-walk-failures" / std::to_string(variantHash);
	bool failureWritten = false;
	size_t remainingFailureBytes = 32u * 1024u * 1024u;
	PortalWalkFrame terminalEvidence;
	size_t terminalCopyPeakBytes = 0;
	testing::ScenarioSession<PortalWalkFrame> session(
		std::move(scenarioKey), std::move(scenarioChecks), scenarioOptions
	);
	client::Client player;
	testing::ScenarioDriver<PortalWalkFrame> driver;
	driver.Run = [&](auto &owner) {
		PortalWalkFrame latestFrames;
		client::FrameObservationOptions frameOptions;
		frameOptions.MaxPendingFrames = 4;
		frameOptions.MaxOwnedBytes = 32u * 1024u * 1024u;
		auto lease = client::SubscribeClientFrames(player, frameOptions, [&](auto current) {
			const auto &previous = latestFrames.Current;
			DriveFrame(previous.get(), current->ClientFrame);
			const size_t bytes =
				sizeof(PortalWalkFrame) + current->OwnedBytes + (previous ? previous->OwnedBytes : 0);
			owner.Publish({"portal-walk.frame", current->ClientFrame, {current, previous}, bytes});
			if (!failureWritten && streamedCheckFailed->load(std::memory_order_relaxed)) {
				failureWritten = true;
				WritePortalWalkFailure(
					failureDirectory,
					failureVariant,
					current,
					previous,
					pixelSummary->Diagnostics,
					remainingFailureBytes
				);
			}
			latestFrames.Previous = std::move(latestFrames.Current);
			latestFrames.Current = std::move(current);
		});
		if (!lease) throw std::runtime_error(lease.Failure());
		if (!player.Initialise(options)) throw std::runtime_error("portal walk client initialization failed");
		runDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(WALK_TIMEOUT_SECONDS);
		const int exitCode = player.Run();
		player.Shutdown();
		// Source charges conservatively cover a value copy with no larger container
		// capacities. Both the live lease and these two independent copies share the cap.
		const size_t currentBytes = latestFrames.Current ? latestFrames.Current->OwnedBytes : 0;
		const size_t previousBytes = latestFrames.Previous ? latestFrames.Previous->OwnedBytes : 0;
		// Reserve the compact peak diagnostic plus JSON node overhead under the same cap.
		const size_t liveBytes = lease.OwnedBytes() + 16u * 1024u;
		if (liveBytes <= frameOptions.MaxOwnedBytes &&
			currentBytes <= frameOptions.MaxOwnedBytes - liveBytes &&
			previousBytes <= frameOptions.MaxOwnedBytes - liveBytes - currentBytes) {
			terminalCopyPeakBytes = liveBytes + currentBytes + previousBytes;
			if (latestFrames.Current)
				terminalEvidence.Current =
					std::make_shared<const client::FrameObservation>(*latestFrames.Current);
			if (latestFrames.Previous)
				terminalEvidence.Previous =
					std::make_shared<const client::FrameObservation>(*latestFrames.Previous);
		} else {
			std::cout << "portal walk terminal evidence exceeds combined owned-byte budget\n";
		}
		*terminalFrame = terminalEvidence.Current;
		latestFrames = {};
		if (!lease.Finish()) throw std::runtime_error(lease.Failure());
		if (lease.OwnedBytes() != 0)
			throw std::runtime_error("portal walk retained leased frames after finish");
		if (lease.CapturedFrames() != observed.CapturedFrames)
			throw std::runtime_error("portal walk missing completed frames");
		if (exitCode != 0) throw std::runtime_error("portal walk native run failed");
	};
	driver.Finish = [&] { player.Shutdown(); };
	const auto scenarioReport = session.Run(driver);
	if (pixelSummary->CompleteFailed && terminalEvidence.Current) {
		WritePortalWalkFailure(
			failureDirectory / "terminal",
			failureVariant,
			terminalEvidence.Current,
			terminalEvidence.Previous,
			pixelSummary->Diagnostics,
			remainingFailureBytes
		);
	}
	terminalEvidence = {};
	if (!scenarioReport.Passed())
		std::cout << "portal walk failure diagnostics " << pixelSummary->Diagnostics.dump() << '\n';
	std::cout << "portal shared scenario setups=" << scenarioReport.Metrics.SetupCount
			  << " frames=" << scenarioReport.Metrics.PublishedSamples
			  << " bytes=" << scenarioReport.Metrics.PublishedBytes
			  << " check_dispatches=" << scenarioReport.Metrics.CheckDispatches
			  << " check_ns=" << scenarioReport.Metrics.CheckNanoseconds
			  << " terminal_copy_peak_bytes=" << terminalCopyPeakBytes
			  << " driver_ns=" << scenarioReport.Metrics.DriverNanoseconds << '\n';
	for (const auto &failure : scenarioReport.Failures) {
		INFO(failure);
		CHECK(false);
	}
	for (const auto &check : scenarioReport.Checks) {
		for (const auto &finding : check.Findings) {
			INFO(check.Name << ": " << finding.Property << " " << finding.Message);
			CHECK(finding.Passed);
		}
		CHECK(check.Passed);
	}
	CHECK(scenarioReport.Metrics.SetupCount == 1);
	CHECK(scenarioReport.Metrics.PublishedSamples == observed.CapturedFrames);
	int capturedFrames = 0;
	bool completed = false;
	size_t settledReturnFrames = 0;
	{
		std::lock_guard lock(observed.Mutex);
		capturedFrames = static_cast<int>(observed.CapturedFrames);
		completed = observed.Result == PortalWalkResult::Complete;
		settledReturnFrames = observed.Completion.SettledFrames;
	}
	CAPTURE(capturedFrames, settledReturnFrames, maximumFrames);
	if (!completed) {
		std::cout << "portal walk incomplete maximum_frames=" << maximumFrames
				  << " captured_frames=" << capturedFrames << " settled_return_frames=" << settledReturnFrames
				  << '\n';
	}
	CHECK(completed);
	if (imageHandoff && fault == PortalWalkFault::None) {
		std::lock_guard lock(observed.Mutex);
		CAPTURE(
			observed.ReturnClosestLateral,
			observed.FirstSteeringFrame,
			observed.SteeringFrames,
			observed.SteeringEvents,
			observed.MisalignedSubmittedMoves,
			observed.FirstMisalignedFrame,
			observed.FirstMisalignedMove
		);
		CHECK(observed.ReturnEnteredAperture);
	}
	if (warmPortal) {
		std::lock_guard lock(observed.Mutex);
		CHECK(observed.PortalReady);
	}
	if (impairment.Active()) {
		std::lock_guard lock(observed.Mutex);
		// One row per impaired walk for the acceptance report, and proof that
		// every requested fault really reached this client's sockets.
		std::cout << "portal product impairment rtt_ms=" << int(impairment.RoundTripSeconds * 1000.0 + .5)
				  << " jitter_ms=" << int(impairment.JitterSeconds * 1000.0 + .5)
				  << " loss_percent=" << int(impairment.LossChance * 100.0f + .5f)
				  << " arrived=" << observed.ImpairedArrived << " dropped=" << observed.ImpairedDropped
				  << " duplicated=" << observed.ImpairedDuplicated
				  << " reordered=" << observed.ImpairedReordered << " delayed=" << observed.ImpairedDelayed
				  << " adoptions=" << observed.Adoptions.size()
				  << " awaiting_image_body_frames=" << pixelSummary->AwaitingImageBodyFrames
				  << " first_contact_frames=" << pixelSummary->FirstContactFrames << '\n';
		CHECK(observed.ImpairedArrived > 0);
		if (impairment.LossChance > 0.0f) CHECK(observed.ImpairedDropped > 0);
		if (impairment.DuplicateChance > 0.0f) CHECK(observed.ImpairedDuplicated > 0);
		if (impairment.ReorderChance > 0.0f) CHECK(observed.ImpairedReordered > 0);
		if (impairment.RoundTripSeconds > 0.0 || impairment.JitterSeconds > 0.0)
			CHECK(observed.ImpairedDelayed > 0);
	}
	REQUIRE(server.RequestStop());
	const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	auto stopped = server.Poll();
	while (stopped.Alive() && std::chrono::steady_clock::now() < stopDeadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		stopped = server.Poll();
	}
	CHECK(stopped.Reason == parallel::ExitReason::Exited);
	CHECK(stopped.Code == 0);
	if (fault != PortalWalkFault::None) {
		std::ifstream report(faultReportPath);
		const std::string markers(std::istreambuf_iterator<char>(report), {});
		if (fault == PortalWalkFault::RestartDestinationPresentation)
			CHECK(markers.find("restarted-presentation-producer") != std::string::npos);
		if (fault == PortalWalkFault::DropAcknowledgement) {
			CHECK(markers.find("dropped-lease-adopted") != std::string::npos);
			CHECK(markers.find("recovered-lease-adopted") != std::string::npos);
		}
	}
	std::lock_guard lock(observed.Mutex);
	CAPTURE(observed.Refusals, observed.MoveSamples, observed.LastMoveTick, observed.CameraFailures);
	CHECK(observed.Joined);
	if (lateClear) {
		CHECK(observed.CameraCleared);
		CHECK(observed.CameraRestored);
	}
	{
		// Portal observations from every server process after the run.
		std::vector<nlohmann::json> rows;
		for (const auto &file : std::filesystem::directory_iterator(observationDirectory)) {
			std::ifstream input(file.path());
			for (std::string line; std::getline(input, line);)
				if (!line.empty()) rows.push_back(nlohmann::json::parse(line));
		}
		std::vector<std::string> trail;
		size_t begun = 0;
		bool traced = true;
		std::unordered_map<std::string, uint64_t> baselines;
		std::vector<std::string> replayed;
		for (const auto &row : rows) {
			const std::string hook = row.at("hook");
			if (!hook.starts_with("portal.")) continue;
			traced &= row.at("trace").get<uint64_t>() == OBSERVATION_TRACE;
			const std::string world = row.at("world");
			const std::string at = world + "@" + std::to_string(row.at("tick").get<uint64_t>());
			if (hook == "portal.crossing") {
				begun += row.at("begun").get<bool>();
				trail.push_back(
					at + " crossing " + (row.at("begun").get<bool>() ? "begun" : "refused") + " to " +
					row.at("destination").get<std::string>() + " offsets " + row.at("prior_offset").dump() +
					" -> " + row.at("current_offset").dump() + " " + row.at("reason").get<std::string>()
				);
			} else if (hook == "portal.handoff") {
				trail.push_back(
					at + " handoff " + row.at("event").get<std::string>() + " peer " +
					row.at("peer").get<std::string>() + " input " + row.at("input_tick").dump()
				);
			} else if (hook == "portal.arrival") {
				baselines[world] = row.at("baseline_input_tick");
				trail.push_back(at + " arrival baseline " + row.at("baseline_input_tick").dump());
			} else if (hook == "portal.input" && row.at("route") == "forwarded" &&
					   baselines.contains(world) &&
					   row.at("input_tick").get<uint64_t>() <= baselines[world]) {
				replayed.push_back(at + " input " + row.at("input_tick").dump());
			}
		}
		// Kept beside the capture sequence, replacing the previous run's.
		const auto kept = std::filesystem::path(captureDirectory.string() + "-observations");
		std::filesystem::remove_all(kept);
		std::filesystem::rename(observationDirectory, kept);
		CAPTURE(trail, replayed, kept);
		CHECK_FALSE(rows.empty());
		CHECK(traced);
		// Input the sealed baseline already contains must not be applied twice.
		CHECK(replayed.empty());
		// The walk crosses out and back. A third crossing is the body walking back
		// through a pane, which these rows name with its world and offsets.
		if (fault == PortalWalkFault::None) CHECK(begun == 2);
	}
	REQUIRE(observed.Adoptions.size() == 2);
	CHECK(observed.Adoptions[0] == "walk.destination");
	CHECK(observed.Adoptions[1] == "server.world");

	CAPTURE(observed.CameraSamples, observed.CameraModeSamples, observed.CameraFailures);
	for (const auto samples : observed.CameraSamples)
		CHECK(samples > 0);
	for (const auto samples : observed.CameraModeSamples)
		CHECK(samples > 0);
	if (fault != PortalWalkFault::None) {
		// The outbound walk passes the obstruction in the source room. How long it
		// lingers there is walk timing, not behaviour, so no count is required.
		CHECK(observed.ObstructedSamples[0] > 0);
	}
	CHECK(observed.CameraFailures.empty());
	CAPTURE(observed.LookSamples, observed.MinimumLookX, observed.MaximumLookX);
	for (size_t stage = 0; stage < 2; ++stage) {
		CHECK(observed.LookSamples[stage] >= 10);
		CHECK(observed.MaximumLookX[stage] - observed.MinimumLookX[stage] > 0.005f);
	}
#if ENGINE_LOG_COMPILED_LEVEL <= ENGINE_LOG_LEVEL_TRACE
	CAPTURE(observed.MoveSamples, observed.LastMoveTick);
	for (const auto samples : observed.MoveSamples)
		CHECK(samples > 0);
	CHECK_FALSE(observed.DuplicateMove);
#endif
}

#undef WALK_CHECK
#undef WALK_CHECK_FALSE
#undef WALK_REQUIRE
#undef WALK_REQUIRE_FALSE

TEST_CASE(
	"faulted product portal round trips retain humanoid camera continuity",
	"[slow][client][portal-product-fault-roundtrip][gpu][.]"
) {
	const auto fault = GENERATE(
		PortalWalkFault::Delay,
		PortalWalkFault::RestartDestinationPresentation,
		PortalWalkFault::DropAcknowledgement
	);
	RunPortalWalk(60, false, true, true, false, true, false, false, RoomShader::None, fault);
}

TEST_CASE(
	"Client walks through product portal images and returns", "[slow][client][portal-product-walk][gpu][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	const bool firstPerson = GENERATE(false, true);
	const bool explicitSubject = GENERATE(false, true);
	const bool holdThroughAdoption = GENERATE(false, true);
	RunPortalWalk(worldTickRate, firstPerson, explicitSubject, holdThroughAdoption, false);
}

TEST_CASE(
	"Client retains a cleared camera through portal adoption and resumes follow",
	"[slow][client][portal-product-camera-clear][gpu][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	const bool firstPerson = GENERATE(false, true);
	RunPortalWalk(worldTickRate, firstPerson, true, true, true);
}

TEST_CASE(
	"portal image handoff preserves the destination room landmark",
	"[slow][client][portal-product-image-handoff][gpu][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	if (std::getenv("PORTAL_PRODUCT_60_ONLY") && worldTickRate != 60.0) return;
	RunPortalWalk(worldTickRate, false, true, true, true, true);
}

TEST_CASE(
	"portal image handoff at 30 FPS keeps both worlds at 60 Hz",
	"[slow][client][portal-product-image-handoff-matched-30fps][gpu][.]"
) {
	RunPortalWalk(
		60, false, true, true, true, true, false, false, RoomShader::None, PortalWalkFault::None, 30
	);
}

TEST_CASE(
	"player crosses after the initial portal image is available",
	"[slow][client][gpu][portal-product-warm-image-handoff][.]"
) {
	const double worldTickRate = GENERATE(30.0, 60.0);
	RunPortalWalk(worldTickRate, false, true, true, true, true, true);
}

TEST_CASE(
	"portal worlds retain their editable room colours across adoption",
	"[slow][client][gpu][portal-product-content-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, true);
}

TEST_CASE(
	"portal worlds resolve same-name authored shaders across both adoptions",
	"[slow][client][gpu][portal-product-shader-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Material);
}

TEST_CASE(
	"portal worlds resolve same-name spatial GUI shaders across both adoptions",
	"[slow][client][gpu][portal-product-spatial-shader-owner][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Spatial);
}

TEST_CASE(
	"portal destination top GUI survives ordered capture and adoption",
	"[slow][client][gpu][portal-product-spatial-overlay][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::SpatialOverlay);
}

TEST_CASE(
	"product crossing applies captured destination lens to the spatial marker",
	"[slow][client][gpu][portal-product-captured-lens][.]"
) {
	RunPortalWalk(60, false, true, true, true, true, true, false, RoomShader::Lens);
}

TEST_CASE(
	"portal image handoff at 144 and 240 FPS keeps both worlds at 60 Hz",
	"[slow][client][portal-product-image-handoff-high-fps][gpu][.]"
) {
	const uint32_t frameRate = GENERATE(144u, 240u);
	RunPortalWalk(
		60, false, true, true, true, true, false, false, RoomShader::None, PortalWalkFault::None, frameRate
	);
}

TEST_CASE(
	"portal image handoff survives variable frame intervals and long stalls",
	"[slow][client][portal-product-frame-stall][gpu][.]"
) {
	// Deterministic variable pacing: ordinary, fast and slow frames, with a
	// 0.2 s stall every 12 frames. A handoff spans about 30 frames, so at least
	// two stalls land inside each one as well as across the approach and return.
	const std::vector<float> schedule{
		1.0f / 60.0f,
		1.0f / 144.0f,
		1.0f / 60.0f,
		1.0f / 30.0f,
		1.0f / 60.0f,
		1.0f / 144.0f,
		1.0f / 60.0f,
		1.0f / 60.0f,
		1.0f / 30.0f,
		1.0f / 144.0f,
		1.0f / 60.0f,
		.2f
	};
	RunPortalWalk(
		60,
		false,
		true,
		true,
		true,
		true,
		false,
		false,
		RoomShader::None,
		PortalWalkFault::None,
		60,
		{},
		schedule
	);
}

TEST_CASE(
	"portal image handoff under round trip, jitter, loss, duplicate and reorder impairment",
	"[slow][client][portal-product-impairment][gpu][.]"
) {
	const int rttMilliseconds = GENERATE(0, 50, 150, 300);
	const int jitterMilliseconds = GENERATE(0, 30);
	const int lossPercent = GENERATE(0, 1, 5);
	// PORTAL_IMPAIRMENT_ROW=rtt,jitter,loss[;rtt,jitter,loss...] reruns only those
	// cells, in grid order, while diagnosing them.
	if (const char *only = std::getenv("PORTAL_IMPAIRMENT_ROW")) {
		const std::string cell = std::to_string(rttMilliseconds) + "," + std::to_string(jitterMilliseconds) +
								 "," + std::to_string(lossPercent);
		if ((";" + std::string(only) + ";").find(";" + cell + ";") == std::string::npos) return;
	}
	client::Options::NetworkImpairment impairment;
	impairment.RoundTripSeconds = rttMilliseconds / 1000.0;
	impairment.JitterSeconds = jitterMilliseconds / 1000.0;
	impairment.LossChance = lossPercent / 100.0f;
	// Duplicate and reorder injection ride on every row, so even the zero
	// latency, zero loss row exercises them.
	impairment.DuplicateChance = .01f;
	impairment.ReorderChance = .01f;
	impairment.Seed = uint32_t(rttMilliseconds * 131 + jitterMilliseconds * 17 + lossPercent + 1);
	RunPortalWalk(
		60,
		false,
		true,
		true,
		true,
		true,
		false,
		false,
		RoomShader::None,
		PortalWalkFault::None,
		60,
		impairment
	);
}
