#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine::script {

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
			const bool ok = RunInstance(instance);
			const uint64_t after = StepsTaken();

			// Saturating, because the counter resets when a script blows its
			// budget - and a reset read as a delta would be an enormous figure
			// attributed to whichever script ran next.
			ScriptCosts.push_back(ScriptCost{instance, after >= before ? after - before : 0, ok});

			if (ok) {
				ran++;
				continue;
			}

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

			if (RunInstance(instance)) {
				started++;
				continue;
			}

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
