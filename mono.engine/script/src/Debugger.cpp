#include <engine/script/Debugger.hpp>

#include <algorithm>

namespace engine::script {

	namespace {
		// Whether a chunk name ends with what somebody typed.
		//
		// **A suffix rather than an exact compare.** Luau reports a chunk by the
		// name it was loaded under, which here is the asset-relative path; a
		// person setting a breakpoint thinks in file names and types
		// `enemy.luau` rather than `scripts/ai/enemy.luau`. Matching on the tail
		// makes both work and keeps the stored form the one the VM will report.
		bool SourceMatches(std::string_view reported, std::string_view wanted) {
			if (wanted.empty()) {
				return false;
			}
			if (wanted.size() > reported.size()) {
				return false;
			}
			return reported.compare(reported.size() - wanted.size(), wanted.size(), wanted) == 0;
		}
	}

	void Debugger::Add(std::string source, int line, BreakAction action) {
		for (Breakpoint &point : Points) {
			if (point.Line == line && point.Source == source) {
				// Replaced in place, so the hit count and the position in the
				// list survive somebody changing what it does.
				point.Action = action;
				point.Enabled = true;
				return;
			}
		}

		Breakpoint point;
		point.Source = std::move(source);
		point.Line = line;
		point.Action = action;
		Points.push_back(std::move(point));
	}

	bool Debugger::Remove(std::string_view source, int line) {
		const auto found = std::find_if(Points.begin(), Points.end(), [&](const Breakpoint &point) {
			return point.Line == line && point.Source == source;
		});
		if (found == Points.end()) {
			return false;
		}

		Points.erase(found);
		return true;
	}

	bool Debugger::Enable(std::string_view source, int line, bool enabled) {
		for (Breakpoint &point : Points) {
			if (point.Line == line && point.Source == source) {
				point.Enabled = enabled;
				return true;
			}
		}
		return false;
	}

	void Debugger::Clear() {
		Points.clear();
	}

	void Debugger::Adopt(const Debugger &other) {
		for (const Breakpoint &point : other.Points) {
			// Through `Add` rather than by copying the vector, so the
			// replace-in-place rule holds and a hit count from the other side
			// cannot arrive attached to a run that never produced it.
			Add(point.Source, point.Line, point.Action);
			Enable(point.Source, point.Line, point.Enabled);
		}
	}

	bool Debugger::Armed() const {
		return std::any_of(Points.begin(), Points.end(), [](const Breakpoint &point) {
			return point.Enabled;
		});
	}

	Breakpoint *Debugger::Match(std::string_view source, int line) {
		for (Breakpoint &point : Points) {
			if (point.Enabled && point.Line == line && SourceMatches(source, point.Source)) {
				return &point;
			}
		}
		return nullptr;
	}

	void Debugger::Record(DebugHit hit) {
		// **The oldest goes, not the newest.** A breakpoint in a loop produces
		// hits faster than anybody reads them, and keeping the first sixty-four
		// would mean the panel filled up in the first tick and then showed
		// nothing that happened afterwards — which is the opposite of what
		// somebody watching a loop wants.
		if (Caught.size() >= MAXIMUM_HITS) {
			Caught.erase(Caught.begin());
		}
		Caught.push_back(std::move(hit));
	}
}
