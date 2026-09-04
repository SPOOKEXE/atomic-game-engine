#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _MSC_VER
#include <windows.h>
#endif

namespace engine::script {

	namespace {
		// Windows faults raised by a VM extension are structured exceptions, not
		// C++ exceptions. Keep the SEH block in this destructor-free helper so
		// MSVC can unwind the caller normally when an ordinary C++ exception
		// crosses the same boundary.
		bool RunInstanceIsolated(Runtime &runtime, const ecs::Entity instance, unsigned long &fault) {
#ifdef _MSC_VER
			__try {
				return runtime.RunInstance(instance);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				fault = static_cast<unsigned long>(GetExceptionCode());
				return false;
			}
#else
			(void)fault;
			return runtime.RunInstance(instance);
#endif
		}
	}

	void ScriptProfiler::SetEnabled(bool enabled) {
		Collecting = enabled;
		if (!Collecting) {
			Active.clear();
			Running.clear();
		}
	}

	ScriptProfiler::ActiveExecution *ScriptProfiler::FindActive(const void *thread) {
		const auto found =
			std::find_if(Active.begin(), Active.end(), [thread](const ActiveExecution &execution) {
				return execution.Thread == thread;
			});
		return found == Active.end() ? nullptr : &*found;
	}

	void ScriptProfiler::Begin(const void *thread, uint64_t nanoseconds, const void *parentThread) {
		if (!Collecting || thread == nullptr) {
			return;
		}

		if (ActiveExecution *existing = FindActive(thread); existing != nullptr) {
			existing->Leaf = UINT32_MAX;
			existing->LastNanoseconds = nanoseconds;
			std::erase(Running, thread);
			Running.push_back(thread);
			return;
		}
		uint32_t parent = UINT32_MAX;
		if (const ActiveExecution *caller = FindActive(parentThread); caller != nullptr) {
			parent = caller->Leaf;
		}
		Active.push_back(ActiveExecution{thread, parent, UINT32_MAX, nanoseconds});
		Running.push_back(thread);
	}

	uint32_t
	ScriptProfiler::FindOrAdd(uint32_t parent, std::string_view source, std::string_view function, int line) {
		const std::string_view namedSource = source.empty() ? "(native)" : source;
		const std::string_view namedFunction = function.empty() ? "(anonymous)" : function;
		for (size_t index = 0; index < Tree.size(); index++) {
			const ScriptProfileNode &node = Tree[index];
			if (node.Parent == parent && node.Line == line && node.Source == namedSource &&
				node.Function == namedFunction) {
				return static_cast<uint32_t>(index);
			}
		}

		if (Tree.size() >= MAXIMUM_NODES) {
			Dropped++;
			return UINT32_MAX;
		}

		Tree.push_back(
			ScriptProfileNode{
				.Parent = parent,
				.Source = std::string(namedSource),
				.Function = std::string(namedFunction),
				.Line = line,
				.Bindings = {},
			}
		);
		return static_cast<uint32_t>(Tree.size() - 1);
	}

	uint32_t ScriptProfiler::FindLeaf(uint32_t parent, std::span<const ScriptProfileFrame> stack) {
		for (size_t index = 0; index < stack.size(); index++) {
			const ScriptProfileFrame &frame = stack[index];
			const int line = index + 1 == stack.size() ? frame.Line : 0;
			parent = FindOrAdd(parent, frame.Source, frame.Function, line);
			if (parent == UINT32_MAX) {
				return UINT32_MAX;
			}
		}
		return parent;
	}

	void ScriptProfiler::Charge(ActiveExecution &execution, uint64_t nanoseconds) {
		if (execution.Leaf != UINT32_MAX && execution.Leaf < Tree.size() &&
			nanoseconds > execution.LastNanoseconds) {
			Tree[execution.Leaf].SelfNanoseconds += nanoseconds - execution.LastNanoseconds;
		}
		execution.LastNanoseconds = nanoseconds;
	}

	void ScriptProfiler::Sample(
		const void *thread, std::span<const ScriptProfileFrame> stack, uint64_t nanoseconds
	) {
		if (!Collecting || thread == nullptr || stack.empty()) {
			return;
		}

		if (FindActive(thread) == nullptr) {
			Begin(thread, nanoseconds);
		}
		ActiveExecution *execution = FindActive(thread);
		if (execution == nullptr) {
			return;
		}

		Charge(*execution, nanoseconds);
		const uint32_t leaf = FindLeaf(execution->Parent, stack);
		if (leaf == UINT32_MAX) {
			return;
		}
		ScriptProfileNode &node = Tree[leaf];
		if (execution->Leaf != leaf) {
			node.Calls++;
		}
		node.Samples++;
		execution->Leaf = leaf;
	}

	void ScriptProfiler::End(const void *thread, uint64_t nanoseconds, bool yielded) {
		ActiveExecution *execution = FindActive(thread);
		if (execution == nullptr) {
			return;
		}

		Charge(*execution, nanoseconds);
		if (yielded && execution->Leaf != UINT32_MAX && execution->Leaf < Tree.size()) {
			Tree[execution->Leaf].Yields++;
		}
		std::erase(Running, thread);
		if (yielded) {
			// The next resume belongs beneath the same call tree. Keep the
			// lightweight lineage record, but remove it from the active VM stack.
			return;
		}
		const auto found =
			std::find_if(Active.begin(), Active.end(), [thread](const ActiveExecution &active) {
				return active.Thread == thread;
			});
		if (found != Active.end()) {
			Active.erase(found);
		}
	}

	void ScriptProfiler::RecordAllocation(size_t bytes) {
		if (!Collecting || Running.empty()) {
			return;
		}
		if (const ActiveExecution *execution = FindActive(Running.back());
			execution != nullptr && execution->Leaf != UINT32_MAX && execution->Leaf < Tree.size()) {
			Tree[execution->Leaf].AllocatedBytes += bytes;
		}
	}

	void ScriptProfiler::RecordBinding(
		const void *thread, std::string_view name, uint64_t nanoseconds, bool yielded
	) {
		if (!Collecting || name.empty()) {
			return;
		}
		const ActiveExecution *execution = FindActive(thread);
		if (execution == nullptr || execution->Leaf == UINT32_MAX || execution->Leaf >= Tree.size()) {
			return;
		}

		std::vector<ScriptProfileNode::Binding> &bindings = Tree[execution->Leaf].Bindings;
		auto found =
			std::find_if(bindings.begin(), bindings.end(), [name](const ScriptProfileNode::Binding &binding) {
				return binding.Name == name;
			});
		if (found == bindings.end()) {
			bindings.push_back(
				ScriptProfileNode::Binding{std::string(name), 1, nanoseconds, yielded ? 1u : 0u}
			);
			return;
		}
		found->Calls++;
		found->Nanoseconds += nanoseconds;
		if (yielded) {
			found->Yields++;
		}
	}

	void ScriptProfiler::Clear() {
		Tree.clear();
		Dropped = 0;
		for (ActiveExecution &execution : Active) {
			execution.Leaf = UINT32_MAX;
		}
	}

	Language LanguageOf(std::string_view path) {
		const std::filesystem::path file(path);
		const std::string extension = file.extension().string();

		if (extension == ".js" || extension == ".mjs" || extension == ".ts") {
			return Language::JavaScript;
		}

		// Luau by default rather than by extension match, so a file with no
		// suffix runs somewhere rather than being refused for a reason that has
		// nothing to do with what is in it.
		return Language::Luau;
	}

	void Runtime::DeliverGuiEvents(std::span<const gui::GuiEvent> events) {
		// **Appended rather than assigned**, because a host may poll more than
		// one canvas between beats. The studio compiles and routes one
		// `gui::Router` per viewport panel - a panel *is* a canvas - so two
		// panels open is two calls here before a single heartbeat, and an
		// assignment would deliver whichever ran last and silently drop the
		// other.
		PendingGuiEvents.insert(PendingGuiEvents.end(), events.begin(), events.end());
	}

	void Runtime::DeliverSettingsMenuAction(core::Name action) {
		if (action.IsValid()) {
			PendingSettingsMenuActions.push_back(action);
		}
	}

	void Runtime::DeliverTeleportResult(TeleportResult result) {
		if (result.Id != 0) {
			PendingTeleportResults.push_back(std::move(result));
		}
	}

	bool Runtime::RunFile(const std::string &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			Error = "could not open " + path;
			return false;
		}

		std::ostringstream contents;
		contents << file.rdbuf();
		return Run(contents.str(), path);
	}

	size_t Runtime::RunWorldScripts() {
		Error.clear();

		const std::vector<ecs::Entity> scripts = ScriptsIn(Store, HostRoleValue.Server, HostRoleValue.Client);

		size_t ran = 0;
		std::string firstError;

		// **Rebuilt rather than accumulated.** This is what the scripts cost on
		// *this* run; adding to a previous run's figures would make the panel
		// report a total nobody asked about and hide a script that got slower.
		ScriptCosts.clear();
		ScriptCosts.reserve(scripts.size());

		for (const ecs::Entity instance : scripts) {
			// Recorded whether or not it is new: this call starts everything it
			// finds, and what the record is for is stopping `RunNewScripts` from
			// starting the same instance a second time.
			(void)RememberStarted(instance);

			const uint64_t before = StepsTaken();
			bool ok = false;
			unsigned long fault = 0;
			try {
				ok = RunInstanceIsolated(*this, instance, fault);
			} catch (const std::exception &failure) {
				Error = failure.what();
			} catch (...) {
				Error = "script raised an unknown host exception";
			}
			if (fault != 0) {
				Error = "script caused Windows exception " + std::to_string(fault);
			}
			const uint64_t after = StepsTaken();

			// Saturating, because the counter resets when a script blows its
			// budget - and a reset read as a delta would be an enormous figure
			// attributed to whichever script ran next.
			ScriptCosts.push_back(ScriptCost{instance, after >= before ? after - before : 0, ok});

			if (ok) {
				ran++;
				continue;
			}

			// A failed top-level script must not be considered again by a later
			// discovery pass. More importantly, keeping the failure on this row
			// isolates it from the rest of the world's scripts.
			Store.Set(instance, Disabled{});

			// **Every script runs even when one fails**, for the reason every
			// heartbeat connection does: a game where half the scripts silently
			// did not start is a bug report with nothing in it. Logged per
			// failure and reported once.
			ENGINE_ERROR("script '{}': {}", Store.InstanceNameOf(instance).Text(), Error);
			if (firstError.empty()) {
				firstError = Error;
			}
		}

		Error = firstError;
		return ran;
	}

	bool Runtime::RememberStarted(ecs::Entity instance) {
		const auto at = std::lower_bound(
			StartedScripts.begin(), StartedScripts.end(), instance, [](ecs::Entity left, ecs::Entity right) {
				return left.Id < right.Id;
			}
		);

		if (at != StartedScripts.end() && at->Id == instance.Id) {
			return false;
		}

		StartedScripts.insert(at, instance);
		return true;
	}

	size_t Runtime::RunNewScripts(std::span<const ecs::Entity> wanted) {
		Error.clear();

		size_t started = 0;
		std::string firstError;

		for (const ecs::Entity instance : wanted) {
			if (!RememberStarted(instance)) {
				continue;
			}

			bool ok = false;
			unsigned long fault = 0;
			try {
				ok = RunInstanceIsolated(*this, instance, fault);
			} catch (const std::exception &failure) {
				Error = failure.what();
			} catch (...) {
				Error = "script raised an unknown host exception";
			}
			if (fault != 0) {
				Error = "script caused Windows exception " + std::to_string(fault);
			}

			if (ok) {
				started++;
				continue;
			}

			Store.Set(instance, Disabled{});

			// Logged per failure and reported once, for the reason
			// `RunWorldScripts` gives: a world where half the scripts silently
			// did not start is a bug report with nothing in it.
			ENGINE_ERROR("script '{}': {}", Store.InstanceNameOf(instance).Text(), Error);
			if (firstError.empty()) {
				firstError = Error;
			}
		}

		Error = firstError;
		return started;
	}
}
