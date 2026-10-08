#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.group-render")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t GROUPS = 8;
	constexpr uint64_t BUDGET = 4 * 1024 * 1024;
	constexpr size_t OPERATIONS = GROUPS + 9;
	struct Fixture {
		static constexpr size_t ProcessOperations = OPERATIONS;
		static constexpr const char *RootName = "imagegraph.group_bench.sample";
		static constexpr const char *ProfileName = "group-profile", *SpanName = "group-span";
		static void PrintWorkload(size_t) {}
		Document Enabled, Disabled, Resumed, Animated;
		Plan EnabledPlan, DisabledPlan, ResumedPlan, AnimatedPlan;
		GroupRenderSession Session;
		Diagnostic Failure;
		uint64_t RetainedBytes = 0;
		static void Check(bool condition, const char *message) {
			if (!condition) throw std::runtime_error(message);
		}
		Fixture() {
			Enabled.FormatVersion = 10;
			for (size_t i = 0; i < GROUPS; ++i) {
				const auto id = std::to_string(i);
				const auto groupId = "group" + id;
				Enabled.Nodes.push_back({"source" + id, "pc.number_simple", groupId, {}, {{"value", 3.0}}});
				Enabled.Nodes.push_back({"control" + id, "pc.group_output", groupId, {}, {}});
				Enabled.Nodes.push_back({"consumer" + id, "pc.number_simple", "", {}, {{"value", 99.0}}});
				Group group{groupId, "Group " + id};
				group.Ports = {{"result", "socket" + id, PortDirection::Output, "control" + id}};
				Enabled.Groups.push_back(std::move(group));
				Enabled.Junctions.push_back({"socket" + id, groupId, ValueType::Any, std::nullopt});
				Enabled.Links.push_back({"source" + id, "number", "control" + id, "value"});
				Enabled.Links.push_back({"control" + id, "value", "socket" + id, "value"});
				Enabled.Links.push_back({"socket" + id, "value", "consumer" + id, "value"});
				Enabled.Outputs.push_back({"held" + id, "control" + id, "value"});
				Enabled.Outputs.push_back({"result" + id, "consumer" + id, "number"});
			}
			Disabled = Enabled;
			for (auto &group : Disabled.Groups)
				group.RenderActive = false;
			for (size_t i = 0; i < GROUPS; ++i)
				Disabled.Nodes[3 * i].Values.front().Data = 7.0;
			Resumed = Disabled;
			for (auto &group : Resumed.Groups)
				group.RenderActive = true;
			Animated = Resumed;
			Animated.Nodes[3 * (GROUPS - 1)].SourceAnimatedInputs = {"value"};
			Check(Compile(Enabled, EnabledPlan, Failure) == Status::Ok, "enabled compile refused");
			Check(Compile(Disabled, DisabledPlan, Failure) == Status::Ok, "disabled compile refused");
			Check(Compile(Resumed, ResumedPlan, Failure) == Status::Ok, "resumed compile refused");
			Check(Compile(Animated, AnimatedPlan, Failure) == Status::Ok, "animated compile refused");
			Run();
			const auto bytes = RetainedBytes;
			Run();
			Check(RetainedBytes == bytes, "scheduler retained storage changed between cycles");
		}
		void CheckPureCache() {
			Check(Session.Purities.size() == GROUPS, "scheduler purity cache has unexpected size");
			for (const auto &row : Session.Purities)
				Check(row.State == SourceGroupPurity::Pure, "ordinary process refreshed cached purity");
		}
		void Process(
			const Document &document, const Plan &plan, GroupRenderMode mode, std::string_view group = {}
		) {
			Check(
				ProcessGroupRender(document, plan, {}, {mode, group}, Session, Failure, BUDGET) == Status::Ok,
				"group scheduler operation refused"
			);
			CheckPureCache();
		}
		void
		Verify(const Document &document, double held, double result, bool controlReady, bool consumerReady) {
			for (size_t i = 0; i < GROUPS; ++i) {
				const auto id = std::to_string(i);
				for (const auto &selection :
					 {std::pair{"held" + id, held}, std::pair{"result" + id, result}}) {
					CacheGroupReplayOutput output;
					Check(
						ReadGroupRenderOutput(document, selection.first, Session, output, Failure, BUDGET) ==
							Status::Ok,
						"held socket read refused"
					);
					Check(
						output.Data && std::holds_alternative<double>(*output.Data) &&
							std::get<double>(*output.Data) == selection.second,
						"held socket value changed"
					);
				}
				Check(
					Session.Ready("control" + id) == controlReady &&
						Session.Ready("consumer" + id) == consumerReady,
					"readiness diverged from scheduler operation"
				);
			}
			Check(
				Session.Nodes.size() == Enabled.Nodes.size() &&
					Session.Outputs.Nodes.size() == Enabled.Nodes.size(),
				"scheduler retained an unexpected node count"
			);
		}
		void Run() {
			Session = {};
			{
				ENGINE_PROFILE("imagegraph.group_bench.load_purity");
				Check(
					RefreshSourceGroupPurity(Enabled, EnabledPlan, {}, {}, Session, Failure, BUDGET) ==
						Status::Ok,
					"cold purity refresh refused"
				);
				CheckPureCache();
				for (const auto &row : Session.Nodes)
					Check(
						!row.Rendered && row.FrameActivity == SourceFrameActivity::Static,
						"purity load executed children or lost source constructor "
						"activity"
					);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.warm_full");
				Process(Enabled, EnabledPlan, GroupRenderMode::AutomaticFull);
				Verify(Enabled, 3, 3, true, true);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.disabled_partial");
				Process(Disabled, DisabledPlan, GroupRenderMode::AutomaticPartial);
				Verify(Disabled, 3, 3, true, true);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.disabled_full");
				Process(Disabled, DisabledPlan, GroupRenderMode::AutomaticFull);
				Verify(Disabled, 3, 3, false, false);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.force_groups");
				for (const auto &group : Disabled.Groups)
					Process(Disabled, DisabledPlan, GroupRenderMode::ForceGroup, group.Id);
				Verify(Disabled, 7, 3, true, false);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.resume_partial");
				Process(Disabled, DisabledPlan, GroupRenderMode::AutomaticPartial);
				Verify(Disabled, 7, 7, true, true);
				Process(Resumed, ResumedPlan, GroupRenderMode::AutomaticFull);
				Verify(Resumed, 7, 7, true, true);
			}
			{
				ENGINE_PROFILE("imagegraph.group_bench.animation_purity");
				Process(Animated, AnimatedPlan, GroupRenderMode::AutomaticFull);
				Verify(Animated, 7, 7, true, true);
				Check(
					RefreshSourceGroupPurity(
						Animated,
						AnimatedPlan,
						{},
						{SourcePurityRefreshEvent::AnimationMode},
						Session,
						Failure,
						BUDGET
					) == Status::Ok,
					"animation-mode purity refresh refused"
				);
				for (const auto &row : Session.Purities)
					Check(
						row.State ==
							(row.GroupId == "group7" ? SourceGroupPurity::Nonpure : SourceGroupPurity::Pure),
						"explicit lifecycle event did not refresh purity"
					);
				Verify(Animated, 7, 7, true, true);
			}
			RetainedBytes = RetainedGroupRenderSessionBytes(Session);
			{
				ENGINE_PROFILE("imagegraph.group_bench.budget_refusal");
				Check(
					ProcessGroupRender(Animated, AnimatedPlan, {}, {}, Session, Failure, RetainedBytes) ==
						Status::LimitExceeded,
					"scheduler accepted a budget with no replacement space"
				);
				Check(
					RetainedGroupRenderSessionBytes(Session) == RetainedBytes,
					"refusal changed retained bytes"
				);
				Verify(Resumed, 7, 7, true, true);
			}
			Check(RetainedBytes < BUDGET, "retained scheduler state exceeded cap");
			Metrics::SetGauge("imagegraph.group_bench.retained_bytes", double(RetainedBytes));
			Metrics::Count("imagegraph.group_bench.operations", OPERATIONS);
			engine::testing::Consume(RetainedBytes);
		}
	};
	// Sender selectors use the prior completed pulse; receiver names are authored constants.
	struct TunnelFixture {
		static constexpr size_t PAIRS = 16, ProcessOperations = 7;
		static constexpr const char *RootName = "imagegraph.tunnel_bench.sample";
		static constexpr const char *ProfileName = "tunnel-profile", *SpanName = "tunnel-span";
		static void PrintWorkload(size_t call) {
			std::printf(
				"# tunnel-workload call=%zu senders=%zu receivers=%zu selector_sources=%zu "
				"payload_sources=%zu pulses=6 observations=%zu refusals=1\n",
				call,
				PAIRS,
				PAIRS,
				PAIRS,
				PAIRS,
				PAIRS * 6
			);
		}
		Document Enabled, Disabled, Resumed;
		Plan EnabledPlan, DisabledPlan, ResumedPlan;
		GroupRenderSession Session;
		Diagnostic Failure;
		uint64_t RetainedBytes = 0;
		TunnelFixture() {
			Enabled.FormatVersion = 10;
			Enabled.Groups = {{"send", "Send"}, {"receive", "Receive"}};
			for (size_t i = 0; i < PAIRS; ++i) {
				const auto id = std::to_string(i), signal = "signal" + id;
				Enabled.Nodes.push_back({"name" + id, "pc.string", "send", {}, {{"text", signal}}});
				Enabled.Nodes.push_back(
					{"value" + id, "pc.number_simple", "send", {}, {{"value", double(i + 1)}}}
				);
				Enabled.Nodes.push_back(
					{"sender" + id, "pc.tunnel_in", "send", {}, {{"scope", EnumValue{0}}}}
				);
				Enabled.Nodes.push_back(
					{"receiver" + id, "pc.tunnel_out", "receive", {}, {{"name", signal}}}
				);
				Enabled.Links.push_back({"name" + id, "text", "sender" + id, "name"});
				Enabled.Links.push_back({"value" + id, "number", "sender" + id, "value_in"});
				Enabled.Outputs.push_back({"out" + id, "receiver" + id, "value_out"});
			}
			Disabled = Enabled;
			Disabled.Groups.front().RenderActive = false;
			for (size_t i = 0; i < PAIRS; ++i) {
				Disabled.Nodes[4 * i].Values.front().Data = "signal" + std::to_string((i + 1) % PAIRS);
				Disabled.Nodes[4 * i + 1].Values.front().Data = double(i + 17);
			}
			Resumed = Disabled;
			Resumed.Groups.front().RenderActive = true;
			Fixture::Check(
				Compile(Enabled, EnabledPlan, Failure) == Status::Ok, "tunnel enabled compile refused"
			);
			Fixture::Check(
				Compile(Disabled, DisabledPlan, Failure) == Status::Ok, "tunnel disabled compile refused"
			);
			Fixture::Check(
				Compile(Resumed, ResumedPlan, Failure) == Status::Ok, "tunnel resumed compile refused"
			);
			Run();
			const auto bytes = RetainedBytes;
			Run();
			Fixture::Check(bytes == RetainedBytes, "tunnel retained storage changed between cycles");
		}
		void Verify(const Document &document, size_t offset, bool rotated, bool senderReady) {
			for (size_t i = 0; i < PAIRS; ++i) {
				const auto id = std::to_string(i);
				CacheGroupReplayOutput output;
				Fixture::Check(
					ReadGroupRenderOutput(document, "out" + id, Session, output, Failure, BUDGET) ==
						Status::Ok,
					"tunnel observation refused"
				);
				const auto source = rotated ? (i + PAIRS - 1) % PAIRS : i;
				Fixture::Check(
					output.Data && std::holds_alternative<double>(*output.Data) &&
						std::get<double>(*output.Data) == double(source + offset),
					"tunnel prior-registry value changed"
				);
				Fixture::Check(
					output.Domain && output.Domain->Type == ValueType::Scalar, "tunnel scalar domain changed"
				);
				Fixture::Check(
					Session.Ready("sender" + id) == senderReady && Session.Ready("receiver" + id),
					"tunnel readiness changed"
				);
			}
			Fixture::Check(
				Session.Nodes.size() == Enabled.Nodes.size() &&
					Session.Outputs.Nodes.size() == Enabled.Nodes.size(),
				"tunnel retained node count changed"
			);
		}
		void Run() {
			Session = {};
			EvaluationRequest request;
			const auto pulse = [&](const Document &document, const Plan &plan, GroupRenderMode mode) {
				Fixture::Check(
					ProcessGroupRender(document, plan, request, {mode}, Session, Failure, BUDGET) ==
						Status::Ok,
					"tunnel pulse refused"
				);
				++request.Tick;
			};
			{
				ENGINE_PROFILE("imagegraph.tunnel_bench.warm_registry");
				pulse(Enabled, EnabledPlan, GroupRenderMode::AutomaticFull);
				pulse(Enabled, EnabledPlan, GroupRenderMode::AutomaticFull);
				Verify(Enabled, 1, false, true);
			}
			{
				ENGINE_PROFILE("imagegraph.tunnel_bench.frozen_senders");
				pulse(Disabled, DisabledPlan, GroupRenderMode::AutomaticFull);
				Verify(Disabled, 1, false, false);
				pulse(Disabled, DisabledPlan, GroupRenderMode::AutomaticPartial);
				Verify(Disabled, 1, false, false);
			}
			{
				ENGINE_PROFILE("imagegraph.tunnel_bench.resume_registry");
				pulse(Resumed, ResumedPlan, GroupRenderMode::AutomaticFull);
				Verify(Resumed, 17, false, true);
				pulse(Resumed, ResumedPlan, GroupRenderMode::AutomaticFull);
				Verify(Resumed, 17, true, true);
			}
			RetainedBytes = RetainedGroupRenderSessionBytes(Session);
			{
				ENGINE_PROFILE("imagegraph.tunnel_bench.budget_refusal");
				Fixture::Check(
					ProcessGroupRender(Resumed, ResumedPlan, request, {}, Session, Failure, RetainedBytes) ==
						Status::LimitExceeded,
					"tunnel replacement cap accepted"
				);
				Fixture::Check(
					RetainedGroupRenderSessionBytes(Session) == RetainedBytes,
					"tunnel refusal changed residency"
				);
				Verify(Resumed, 17, true, true);
			}
			Metrics::SetGauge("imagegraph.tunnel_bench.retained_bytes", double(RetainedBytes));
			Metrics::Count("imagegraph.tunnel_bench.senders", PAIRS);
			Metrics::Count("imagegraph.tunnel_bench.receivers", PAIRS);
			Metrics::Count("imagegraph.tunnel_bench.pulses", 6);
			Metrics::Count("imagegraph.tunnel_bench.observations", PAIRS * 6);
			Metrics::Count("imagegraph.tunnel_bench.operations", ProcessOperations);
			engine::testing::Consume(RetainedBytes);
		}
	};
	struct Reading {
		float Milliseconds = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		uint64_t Retained = 0;
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
	};
	template <class GraphType> struct Profile {
		GraphType Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		void Measure() {
			Fixture::Check(Count < Readings.size(), "group profile supports one to five samples");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE(GraphType::RootName);
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			reading.Retained = Graph.RetainedBytes;
			Fixture::Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"group profile dropped scopes"
			);
			size_t roots = 0, operations = 0;
			for (const auto &span : FrameGraph::Spans()) {
				Fixture::Check(!span.Reported, "CPU scheduler profile contains reported work");
				if (span.Parent == FrameGraph::NO_PARENT) {
					Fixture::Check(span.Name == GraphType::RootName, "scheduler profile has unexpected root");
					++roots;
				}
				if (span.Name == "imagegraph.group_process") ++operations;
				reading.Spans.emplace_back(std::string(span.Name), span);
			}
			Fixture::Check(
				roots == 1 && operations == GraphType::ProcessOperations,
				"scheduler profile missed processing scopes"
			);
			Metrics::Drain();
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_GROUP_PRESET");
			for (size_t i = 0; i < Count; ++i) {
				const auto &r = Readings[i];
				GraphType::PrintWorkload(i + 1);
				std::printf(
					"# %s preset=%s backend=cpu groups=%zu nodes=%zu "
					"operations=%zu tick=0 "
					"budget_bytes=%llu call=%zu warmup=%d owner_ms=%.6f "
					"unmarked_ms=%.6f retained_bytes=%llu "
					"heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu "
					"live_bytes=%lld "
					"peak_bytes=%lld profiler_overhead_bytes=%lld\n",
					GraphType::ProfileName,
					preset ? preset : "unreported",
					Graph.Enabled.Groups.size(),
					Graph.Enabled.Nodes.size(),
					GraphType::ProcessOperations,
					static_cast<unsigned long long>(BUDGET),
					i + 1,
					i < 8,
					r.Milliseconds,
					r.Unmarked,
					static_cast<unsigned long long>(r.Retained),
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(r.After.TotalBytes - r.Before.TotalBytes),
					static_cast<unsigned long long>(r.After.TotalBlocks - r.Before.TotalBlocks),
					static_cast<long long>(r.After.LiveBytes),
					static_cast<long long>(r.After.PeakBytes),
					static_cast<long long>(r.After.OverheadBytes)
				);
				for (size_t j = 0; j < r.Spans.size(); ++j) {
					const auto &[name, span] = r.Spans[j];
					std::printf(
						"# %s call=%zu index=%zu parent=%u depth=%u "
						"start_ms=%.6f inclusive_ms=%.6f "
						"self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						GraphType::SpanName,
						i + 1,
						j,
						span.Parent,
						span.Depth,
						span.StartMilliseconds,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						span.Reported,
						name.c_str()
					);
				}
			}
		}
	};
} // namespace
BENCH(
	"CPU eight source groups warm-disable-reset-force-resume bounded "
	"scheduler cycle",
	1
) {
	static Profile<Fixture> profile;
	profile.Measure();
}

BENCH("CPU sixteen sender-receiver pairs prior registry frozen groups and resume", 1) {
	static Profile<TunnelFixture> profile;
	profile.Measure();
}
