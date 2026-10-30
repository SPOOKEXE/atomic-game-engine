// Compare one editor delete action with the pre-existing grouped-recording path.
// This is a proxy for batch snapshot cost, not a prior-revision comparison.
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/game/Game.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Universe.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <studio/Commands.hpp>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.bench.deleteselection")

namespace {
	using engine::core::HeapProfile;
	using engine::core::HeapTotals;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using Clock = std::chrono::steady_clock;

	struct Shape {
		const char *Name;
		size_t Roots;
		size_t Children;
	};

	struct Report {
		uint64_t Nanoseconds = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t AllocatedBlocks = 0;
		int64_t RetainedBytes = 0;
		int64_t PeakAboveEntry = 0;
		int64_t ProfilerOverheadBytes = 0;
		uint64_t DroppedScopes = 0;
		uint64_t ForeignFrees = 0;
		uint64_t DocumentHash = 14695981039346656037ull;
		size_t DocumentBytes = 0;
		size_t Commands = 0;
		size_t Publications = 0;
		bool PublishedInsideEntry = false;
	};

	void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}

	void Hash(Report &report, std::string_view text) {
		for (const unsigned char byte : text) {
			report.DocumentHash ^= byte;
			report.DocumentHash *= 1099511628211ull;
		}
		report.DocumentBytes += text.size();
	}

	Report Run(const Shape &shape, bool recording, bool memory) {
		studio::Editor editor;
		editor.Universe = std::make_unique<engine::world::Universe>();
		const auto world = editor.Universe->Create({.Name = engine::core::Name("delete-selection-bench")});
		Require(world.IsValid(), "could not create benchmark world");
		editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
		std::vector<Entity> roots;
		std::vector<std::string> names;
		std::vector<std::string> documents;
		roots.reserve(shape.Roots);
		names.reserve(shape.Roots);
		documents.reserve(shape.Roots);
		const auto part = engine::ecs::Classes::Find(engine::core::Name("Part"));
		Require(part.IsValid(), "Part class missing");
		editor.Universe->Enter(world, [&](Store &store) {
			for (size_t rootIndex = 0; rootIndex < shape.Roots; ++rootIndex) {
				names.push_back("Root" + std::to_string(rootIndex));
				const Entity root = store.CreateInstance(part, names.back());
				Require(root != engine::ecs::NULL_ENTITY, "could not create root");
				roots.push_back(root);
				for (size_t child = 0; child < shape.Children; ++child) {
					const Entity entity = store.CreateInstance(part, "Child" + std::to_string(child));
					Require(
						entity != engine::ecs::NULL_ENTITY && store.SetParent(entity, root),
						"could not create child"
					);
				}
				documents.push_back(engine::game::WriteInstanceDocument(store, root));
			}
		});

		Report report;
		studio::CommandLog::Watcher watcher;
		watcher.Committed = [&](uint64_t, std::span<const studio::Command> commands) {
			++report.Publications;
			report.Commands += commands.size();
			report.PublishedInsideEntry = editor.Universe->IsEntering();
			size_t index = 0;
			for (const auto &command : commands) {
				Require(command.Kind == studio::CommandKind::Destroy, "unexpected published command");
				Require(
					index < documents.size() && command.Document == documents[index],
					"published document differed"
				);
				Hash(report, command.Document);
				++index;
			}
		};
		editor.Commands->Watch(std::move(watcher));
		std::optional<std::string> recordingId;
		if (recording) {
			recordingId = editor.Commands->TryBeginRecording("benchmark", "benchmark");
			Require(recordingId.has_value(), "could not begin recording");
		}

		if (memory) HeapProfile::ResetTotals();
		const HeapTotals before = memory ? HeapProfile::Totals() : HeapTotals{};
		const auto started = Clock::now();
		editor.DeleteInstances(world, roots);
		if (recording) {
			Require(
				editor.Commands->FinishRecording(*recordingId, studio::FinishOperation::Commit),
				"could not commit recording"
			);
		}
		const auto ended = Clock::now();
		const HeapTotals after = memory ? HeapProfile::Totals() : HeapTotals{};
		report.Nanoseconds = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count()
		);
		if (memory) {
			report.AllocatedBytes = after.TotalBytes - before.TotalBytes;
			report.AllocatedBlocks = after.TotalBlocks - before.TotalBlocks;
			report.RetainedBytes = after.LiveBytes - before.LiveBytes;
			report.PeakAboveEntry = after.PeakBytes - before.LiveBytes;
			report.ProfilerOverheadBytes = after.OverheadBytes;
			report.DroppedScopes = after.DroppedScopes - before.DroppedScopes;
			report.ForeignFrees = after.ForeignFrees - before.ForeignFrees;
		}

		Require(
			report.Publications == 1 && report.Commands == shape.Roots && !report.PublishedInsideEntry,
			"delete publication differed"
		);
		editor.Universe->Enter(world, [&](Store &store) {
			for (const auto &name : names)
				Require(store.FindFirstRoot(name) == engine::ecs::NULL_ENTITY, "delete missed a root");
		});
		Require(editor.Commands->Undo(), "delete undo failed");
		Require(
			!editor.Commands->CanUndo() && editor.Commands->CanRedo(), "delete did not form one undo step"
		);
		editor.Universe->Enter(world, [&](Store &store) {
			for (size_t index = 0; index < names.size(); ++index) {
				const Entity root = store.FindFirstRoot(names[index]);
				Require(root != engine::ecs::NULL_ENTITY, "undo missed a root");
				Require(
					engine::game::WriteInstanceDocument(store, root) == documents[index],
					"undo changed the document"
				);
			}
		});
		Require(editor.Commands->Redo(), "delete redo failed");
		editor.Universe->Enter(world, [&](Store &store) {
			for (const auto &name : names)
				Require(store.FindFirstRoot(name) == engine::ecs::NULL_ENTITY, "redo missed a root");
		});
		Require(!editor.Commands->CanRedo(), "redo did not consume the group");
		editor.Commands->Watch({});
		return report;
	}

	void Print(const Shape &shape, bool recording, size_t trial, bool memory, const Report &report) {
		std::cout << "# delete-selection\tshape=" << shape.Name
				  << "\tmode=" << (recording ? "recording" : "batch") << "\ttrial=" << trial
				  << "\troots=" << shape.Roots << "\tchildren_per_root=" << shape.Children
				  << "\tdelete_ns=" << report.Nanoseconds << "\tcommands=" << report.Commands
				  << "\tpublications=" << report.Publications << "\tdocument_bytes=" << report.DocumentBytes
				  << "\tdocument_hash=" << report.DocumentHash << "\theap_enabled=" << memory;
		if (memory) {
			std::cout << "\tallocated_bytes=" << report.AllocatedBytes
					  << "\tallocated_blocks=" << report.AllocatedBlocks
					  << "\tretained_bytes=" << report.RetainedBytes
					  << "\tpeak_above_entry=" << report.PeakAboveEntry
					  << "\tprofiler_overhead_bytes=" << report.ProfilerOverheadBytes
					  << "\tdropped_scopes_delta=" << report.DroppedScopes
					  << "\tforeign_frees_delta=" << report.ForeignFrees;
		}
		std::cout << '\n';
	}
}

BENCH("Studio delete selection | batch and recording proxy", 1) {
	const char *requestedMemory = std::getenv("MONO_DELETE_BENCH_MEMORY");
	const bool memory = requestedMemory != nullptr && std::string_view(requestedMemory) == "1";
	Require(!memory || HeapProfile::IsCompiledIn(), "delete memory benchmark requires heap profiling");
	engine::scene::RegisterSceneClasses();
	studio::SetConfigRoot(engine::core::Paths::Base() / "studio-delete-benchmark-config");
	std::cout << "# delete-selection basis\tcontrol=existing_recording_proxy\tprior_revision=0"
				 "\theap=tracked_cpp_requested_payload\ttiming=delete_and_commit_only\tfixture_and_oracles="
				 "excluded\ttiming_qualified=0"
			  << "\theap_hooks_compiled=" << HeapProfile::IsCompiledIn() << '\n';
	for (const Shape shape :
		 {Shape{"single", 1, 0}, Shape{"large_subtree", 1, 4096}, Shape{"many_roots", 64, 32}}) {
		for (size_t trial = 0; trial < 3; ++trial) {
			const bool recordingFirst = trial % 2 != 0;
			const Report first = Run(shape, recordingFirst, memory);
			const Report second = Run(shape, !recordingFirst, memory);
			Require(
				first.DocumentHash == second.DocumentHash && first.DocumentBytes == second.DocumentBytes &&
					first.Commands == second.Commands,
				"batch and recording output differed"
			);
			Print(shape, recordingFirst, trial, memory, first);
			Print(shape, !recordingFirst, trial, memory, second);
		}
	}
}
