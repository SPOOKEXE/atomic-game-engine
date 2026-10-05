#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/SourceFrameCacheProject.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_frame_cache_project")
using namespace engine::imagegraph;
TEST_CASE(
	"Cache Last Frame rounds project time to even independently of scoped node time",
	"[frame_cache_groups][cache_project_observation]"
) {
	EvaluationRequest node;
	node.Tick = 2;
	node.Subframe = .25;
	node.SourceCachePlayback = SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	node.SourceCacheProject = SourceFrameCacheProjectObservation{{9, .5, false}, 10, false, false};
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	BindNativeSourceFrameCacheProjectPrefix(node);
	REQUIRE(node.SourceCacheProject->ProjectFrame == FrameTime{9, .5, false});
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCachePlayback->Sampling = SourceCacheSampling::NativePlayedPrefix;
	BindNativeSourceFrameCacheProjectPrefix(node);
	REQUIRE(node.SourceCacheProject->ProjectFrame == FrameTime{2, .25, false});
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectLastFrame = 2;
	node.SourceCacheProject->ProjectFrame = {2, .5, false};
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {3, .5, false};
	node.SourceCacheProject->ProjectLastFrame = 4;
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {4, .49, false};
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {4, .51, false};
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {4, .5, false};
	node.SourceCacheProject->ProjectLastFrame = 4.5;
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {2, .5, true};
	node.SourceCacheProject->ProjectLastFrame = -2;
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {3, .5, true};
	node.SourceCacheProject->ProjectLastFrame = -4;
	REQUIRE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame.Subframe = 1;
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
	node.SourceCacheProject->ProjectFrame = {4, 0, false};
	node.SourceCacheProject->ProjectLastFrame = std::numeric_limits<double>::quiet_NaN();
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(*node.SourceCacheProject));
}
TEST_CASE(
	"Source last endpoint preserves explicit range priority selected "
	"region and signed fractions",
	"[frame_cache_groups][cache_project_observation]"
) {
	TimelineSettings timeline{10, 0, 9, "stop", 24};
	timeline.SourceBounds.emplace();
	REQUIRE(SourceTimelineLastFrame(timeline, 7.5) == 6.5);
	SourceFrameCacheProjectObservation observation{
		{6, .5, false}, *SourceTimelineLastFrame(timeline, 7.5), false, false
	};
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(observation));
	observation.ProjectFrame = {6, 0, false};
	observation.ProjectLastFrame = 6;
	REQUIRE(SourceFrameCacheIsLastProjectFrame(observation));
	timeline.SourceBounds->End = {SourceFrameBoundPresence::Explicit, {2, .25, true}};
	observation.ProjectLastFrame = *SourceTimelineLastFrame(timeline, 7.5);
	REQUIRE(observation.ProjectLastFrame == -3.25);
	observation.ProjectFrame = {3, .25, true};
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(observation));
	observation.ProjectFrame = {3, 0, true};
	observation.ProjectLastFrame = -3;
	REQUIRE(SourceFrameCacheIsLastProjectFrame(observation));
	observation.ProjectFrame.Subframe = .5;
	REQUIRE_FALSE(SourceFrameCacheIsLastProjectFrame(observation));
	timeline.SourceBounds->End = {};
	REQUIRE(SourceTimelineLastFrame(timeline) == 9.);
	observation.ProjectLoading = true;
	observation.ProjectAppending = true;
	REQUIRE(observation.ProjectFrame == FrameTime{3, .5, true});
}

namespace {
	class ObservedText final : public HostNodeProvider {
	  public:
		size_t Calls = 0;
		std::string Text = "17";
		bool Capture(const HostNodeInvocation &call, HostNodeCapture &output, std::string &) override {
			++Calls;
			output.Authored = call.Authored;
			output.Tick = call.Request.Tick;
			output.Subframe = call.Request.Subframe;
			output.NegativeFrame = call.Request.NegativeFrame;
			output.Inputs.assign(call.Inputs.begin(), call.Inputs.end());
			output.Outputs = {{"content", Text}, {"path", std::string("fixture")}};
			return true;
		}
	};
}
TEST_CASE(
	"Cache host refreshes changed project observations and retains exact retry and last good refusal",
	"[frame_cache_groups][cache_project_observation]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
	document.Project = ProjectSettings{};
	document.Project->SurfaceWidth = 2;
	document.Project->SurfaceHeight = 1;
	document.Nodes = {
		{"file", "pc.text_file_read", "", {}, {}},
		{"length", "pc.string_length", "", {}, {}},
		{"input", "pc.solid", "", {}, {{"dimension_unit", EnumValue{0}}}},
		{"cache", "pc.cache", "", {}, {{"animated", false}}},
		{"probe", "pc.string_length", "", {}, {}}
	};
	document.Links = {
		{"file", "content", "length", "text"},
		{"length", "length", "input", "dimension"},
		{"input", "surface_out", "cache", "surface_in"},
		{"file", "content", "probe", "text"}
	};
	document.Outputs = {{"out", "cache", "cache_surface"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ObservedText provider;
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.HostProvider = &provider;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{4, .5, false}, 4.5, false, false};
	REQUIRE(host.Prepare(
		document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out", "probe"
	));
	REQUIRE(request.DataReplay);
	const auto retained = *request.DataReplay;
	REQUIRE_FALSE(retained.Entries.empty());
	REQUIRE(provider.Calls == 1);
	REQUIRE(host.Output("out"));
	REQUIRE(host.Output("out")->Width == 2);
	const auto observedText = [&]() -> const Value * {
		for (const auto &input : host.Snapshot().Values())
			if (input.Port == "text") return &input.Data;
		return nullptr;
	};
	REQUIRE(observedText());
	REQUIRE(*observedText() == Value{std::string("17")});
	request.SourceCacheProject->ProjectLastFrame = std::numeric_limits<double>::quiet_NaN();
	REQUIRE_FALSE(host.Prepare(
		document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out", "probe"
	));
	REQUIRE(diagnostic.Code == Status::InvalidValue);
	REQUIRE(*request.DataReplay == retained);
	REQUIRE(provider.Calls == 1);
	request.SourceCacheProject->ProjectLastFrame = 4.5;
	provider.Text = "777";
	request.SourceCacheProject->ProjectLoading = true;
	REQUIRE(host.Prepare(
		document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out", "probe"
	));
	REQUIRE(provider.Calls == 2);
	REQUIRE(host.Output("out"));
	REQUIRE(host.Output("out")->Width == 2);
	// The new observation recaptures this real upstream input. Cache's same-frame
	// slot remains authoritative even though that producer now evaluates to width three.
	REQUIRE(observedText());
	REQUIRE(*observedText() == Value{std::string("777")});
	const auto refreshed = *request.DataReplay;
	provider.Text = "9999";
	REQUIRE(host.Prepare(
		document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out", "probe"
	));
	REQUIRE(provider.Calls == 2);
	REQUIRE(*request.DataReplay == refreshed);
	REQUIRE(host.Output("out")->Width == 2);
	REQUIRE(observedText());
	REQUIRE(*observedText() == Value{std::string("777")});
}
