#include "SourceCommonExecution.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.source-common-empty")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t OWNERS = 64;
	constexpr uint64_t BUDGET = 1024 * 1024;
	constexpr size_t PROFILE_CALLS = 13;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	bool SameStatefulOutputValue(
		const std::variant<Image, ImageArray, EvaluatedValue> &left,
		const std::variant<Image, ImageArray, EvaluatedValue> &right
	) {
		if (left.index() != right.index()) return false;
		return std::visit(
			[&](const auto &value) {
				using T = std::decay_t<decltype(value)>;
				const auto *other = std::get_if<T>(&right);
				if (!other) return false;
				if constexpr (std::is_same_v<T, ImageArray>)
					return value.Images == other->Images && value.Items == other->Items;
				else
					return value == *other;
			},
			left
		);
	}
	bool SameStatefulOutputs(
		const std::vector<StatefulNamedOutput> &left, const std::vector<StatefulNamedOutput> &right
	) {
		if (left.size() != right.size()) return false;
		for (size_t index = 0; index < left.size(); ++index)
			if (left[index].Id != right[index].Id ||
				!SameStatefulOutputValue(left[index].Output, right[index].Output))
				return false;
		return true;
	}
	struct Fixture {
		bool DisplayText = false;
		Document Graph;
		Plan Compiled;
		GroupRenderSession Session;
		GroupRenderSession Baseline;
		EvaluationRequest Request;
		std::array<std::string, OWNERS> Ids, AnimatorPorts;
		uint64_t StateFnv = 0;
		explicit Fixture(bool displayText) : DisplayText(displayText) {
			Graph.FormatVersion = 11;
			Graph.SourceAnimators.emplace();
			const std::string source = DisplayText ? "Node_Display_Text" : "Node_Frame";
			const std::string native = DisplayText ? "pc.display_text" : "pc.frame";
			Graph.Nodes.reserve(OWNERS);
			Graph.SourceCommonOwners.reserve(OWNERS);
			Graph.SourceAnimators->Detached.reserve(OWNERS);
			Graph.SourceAnimators->DetachedValues.reserve(OWNERS);
			for (size_t index = 0; index < OWNERS; ++index) {
				Ids[index] = "owner" + std::to_string(index);
				AnimatorPorts[index] = "native:animator:" + std::to_string(index);
				Node node;
				node.Id = Ids[index];
				node.Type = native;
				Graph.Nodes.push_back(std::move(node));
				SourceCommonOwnerRecord owner;
				owner.SourceOwnerId = Ids[index];
				owner.NativeOwnerId = Ids[index];
				owner.SourceType = source;
				owner.ShowUpdateTrigger = true;
				owner.OutMeta = true;
				owner.UpdateAnimatorOwnerId = Ids[index];
				owner.UpdateAnimatorPort = AnimatorPorts[index];
				Graph.SourceCommonOwners.push_back(std::move(owner));
				DetachedSourceAnimator writer;
				writer.OwnerId = Ids[index];
				writer.Id = AnimatorPorts[index];
				writer.OriginalPort = "pxcx.update_in_trigger";
				writer.Type = ValueType::Boolean;
				writer.Writer = GroupSubtypeAnimator::Animated;
				Graph.SourceAnimators->Detached.push_back(std::move(writer));
				GroupSubtypeOverlay payload;
				payload.NodeId = Ids[index];
				payload.Port = AnimatorPorts[index];
				Keyframe key;
				key.NodeId = Ids[index];
				key.Port = AnimatorPorts[index];
				key.Data = false;
				payload.Keys.push_back(std::move(key));
				Graph.SourceAnimators->DetachedValues.push_back(std::move(payload));
			}
			Diagnostic error;
			Check(
				CompileSourceCommonRuntime(Graph, Compiled, error) == Status::Ok,
				"empty graph compile refused"
			);
			Check(
				InitializeNativeSourceCommonRuntime(
					Graph, Compiled, {}, SourceNodeInitialState::Loaded, Session, error
				) == Status::Ok,
				"empty graph initialization refused"
			);
			Check(
				Session.Outputs.Nodes.size() == OWNERS && Session.Nodes.size() == OWNERS,
				"owner count changed"
			);
			for (size_t index = 0; index < OWNERS; ++index) {
				Check(Session.Outputs.Nodes[index].Outputs.empty(), "empty owner produced output");
				Check(!Session.Nodes[index].Rendered, "empty owner was rendered");
				Check(
					SourceCommonEmptyOwnerMatches(Graph.Nodes[index], source),
					"empty owner identity did not match"
				);
			}
			Request.SourceSafeMode = true;
			Baseline = Session;
			StateFnv = Hash();
		}
		uint64_t Hash() const {
			uint64_t hash = 14695981039346656037ull;
			auto byte = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ull; };
			auto word = [&](uint64_t value) {
				for (size_t shift = 0; shift < 64; shift += 8)
					byte((value >> shift) & 255);
			};
			for (size_t index = 0; index < OWNERS; ++index) {
				const auto &node = Session.Nodes[index];
				const auto &output = Session.Outputs.Nodes[index];
				for (unsigned char value : node.NodeId)
					byte(value);
				byte(0);
				word(node.Rendered);
				word(output.Outputs.size());
			}
			for (const auto &state : Session.Common.Owners) {
				for (unsigned char value : state.OwnerId)
					byte(value);
				byte(0);
				byte(state.Updated);
				for (unsigned char value : state.Name)
					byte(value);
				byte(0);
				word(std::bit_cast<uint64_t>(state.Position.X));
				word(std::bit_cast<uint64_t>(state.Position.Y));
			}
			return hash;
		}
		void Run() {
			size_t callbacks = 0;
			ENGINE_PROFILE("imagegraph.source_common.empty_callbacks");
			for (size_t index = 0; index < OWNERS; ++index) {
				detail::EvaluationBudget budget(BUDGET);
				detail::AllocationReservation charge;
				Diagnostic error;
				const auto mode = index % 2 ? detail::SourceCommonInvocationMode::SourceDoUpdate
											: detail::SourceCommonInvocationMode::DirectUpdate;
				Check(
					detail::InvokeSourceCommonCallback(
						Graph, Compiled, Request, index, mode, Session, budget, charge, error
					) == Status::Ok,
					"empty callback invocation refused"
				);
				++callbacks;
			}
			Check(callbacks == OWNERS, "empty callback invocation count changed");
		}
		void Verify() const {
			Check(Hash() == StateFnv, "empty callback changed semantic state");
			Check(Session.Outputs == Baseline.Outputs, "empty callback changed outputs");
			Check(Session.Nodes == Baseline.Nodes, "empty callback changed readiness");
			Check(Session.Common == Baseline.Common, "empty callback changed common sockets");
			Check(
				Session.CommonAnimators == Baseline.CommonAnimators, "empty callback changed animator state"
			);
			Check(
				Session.SourceCommonWrites == Baseline.SourceCommonWrites,
				"empty callback changed write journal"
			);
			Check(
				Session.SourceCommonInputs == Baseline.SourceCommonInputs,
				"empty callback changed input journal"
			);
			Check(
				Session.SourceCommonBindings == Baseline.SourceCommonBindings,
				"empty callback changed bindings"
			);
			Check(Session.Purities == Baseline.Purities, "empty callback changed purity state");
			Check(
				Session.Replay.Simulation == Baseline.Replay.Simulation,
				"empty callback changed simulation journal"
			);
			Check(
				Session.Replay.Surfaces == Baseline.Replay.Surfaces, "empty callback changed surface journal"
			);
			Check(Session.Replay.Random == Baseline.Replay.Random, "empty callback changed random journal");
			Check(Session.Replay.Data == Baseline.Replay.Data, "empty callback changed data journal");
			Check(Session.Replay.Rigid == Baseline.Replay.Rigid, "empty callback changed rigid journal");
			Check(
				SameStatefulOutputs(Session.Replay.Outputs, Baseline.Replay.Outputs),
				"empty callback changed replay output journal"
			);
			Check(Session.Outputs.Nodes.size() == OWNERS, "empty callback changed output owner count");
			for (size_t index = 0; index < OWNERS; ++index) {
				Check(Session.Outputs.Nodes[index].Outputs.empty(), "empty callback added output");
				Check(!Session.Nodes[index].Rendered, "empty callback marked owner rendered");
			}
			engine::testing::Consume(StateFnv);
		}
	};
	struct Reading {
		float Milliseconds = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
	};
	struct Profile {
		Fixture Graph;
		std::array<Reading, PROFILE_CALLS> Readings;
		size_t Count = 0;
		explicit Profile(bool displayText) : Graph(displayText) {}
		void Measure() {
			Check(Count < Readings.size(), "empty callback profile accepts 13 calls");
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
				ENGINE_PROFILE("imagegraph.source_common.empty_sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"empty callback profile dropped scopes"
			);
			size_t callbacks = 0, samples = 0;
			for (const auto &span : FrameGraph::Spans()) {
				callbacks += span.Name == "imagegraph.source_common.empty_callbacks";
				samples += span.Name == "imagegraph.source_common.empty_sample";
				Check(!span.Reported, "empty callback profile contains reported time");
				reading.Spans.emplace_back(std::string(span.Name), span);
			}
			Check(callbacks == 1 && samples == 1, "empty callback profile scope count changed");
			Metrics::Drain();
			Graph.Verify();
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_EMPTY_PRESET");
			for (size_t call = 0; call < Count; ++call) {
				const auto &reading = Readings[call];
				std::printf(
					"# source-common-empty preset=%s backend=cpu variant=%s owners=64 call=%zu warmup=%d "
					"callback_invocations=64 callback_scopes=1 sample_scopes=1 state_fnv=%llu "
					"owner_ms=%.6f unmarked_ms=%.6f heap_compiled=%d "
					"allocated_bytes=%llu allocated_blocks=%llu live_bytes=%lld peak_bytes=%lld "
					"interval_live_bytes_delta=%lld profiler_overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					Graph.DisplayText ? "display_text" : "frame",
					call + 1,
					call < 8,
					static_cast<unsigned long long>(Graph.StateFnv),
					reading.Milliseconds,
					reading.Unmarked,
					HeapProfile::IsCompiledIn(),
					static_cast<unsigned long long>(reading.After.TotalBytes - reading.Before.TotalBytes),
					static_cast<unsigned long long>(reading.After.TotalBlocks - reading.Before.TotalBlocks),
					static_cast<long long>(reading.After.LiveBytes),
					static_cast<long long>(reading.After.PeakBytes),
					static_cast<long long>(reading.After.LiveBytes - reading.Before.LiveBytes),
					static_cast<long long>(reading.After.OverheadBytes)
				);
				for (size_t index = 0; index < reading.Spans.size(); ++index) {
					const auto &[name, span] = reading.Spans[index];
					std::printf(
						"# source-common-empty-span call=%zu index=%zu parent=%u depth=%u inclusive_ms=%.6f "
						"self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						call + 1,
						index,
						span.Parent,
						span.Depth,
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
}
BENCH("CPU empty Frame annotation callbacks", 1) {
	static Profile profile(false);
	profile.Measure();
}
BENCH("CPU empty DisplayText annotation callbacks", 1) {
	static Profile profile(true);
	profile.Measure();
}
