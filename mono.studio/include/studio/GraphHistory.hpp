#pragma once

// Undo and redo for an editor's ang graph, stored as the edit records the
// graph returns. Undo applies a record's inverse; the record that inverse
// produces is what redo applies, so nothing is snapshotted.

#include <engine/ang/Graph.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace studio {

	class GraphHistory {
	  public:
		// Oldest records are dropped past this many.
		static constexpr size_t LIMIT = 200;

		// Keeps one applied record as an undo step and forgets every redo step.
		void Record(engine::ang::EditRecord record) {
			if (record.Applied.empty()) return;
			Past.push_back(std::move(record));
			Future.clear();
			if (Past.size() > LIMIT) Past.erase(Past.begin());
		}

		// Folds `record` into the newest step: a drag of one slider or typing
		// into one field is one undo step. The step keeps its first inverse.
		void Amend(engine::ang::EditRecord record) {
			if (Past.empty()) {
				Record(std::move(record));
				return;
			}
			Past.back().Applied = std::move(record.Applied);
			Future.clear();
		}

		// Records several, oldest first, as a view's TakeEdits returns them.
		void Record(std::vector<engine::ang::EditRecord> records) {
			for (engine::ang::EditRecord &record : records)
				Record(std::move(record));
		}

		// False when there is nothing to undo, or the graph refused the inverse
		// (it was replaced underneath), which clears the history.
		bool Undo(engine::ang::Graph &graph) {
			return Step(graph, Past, Future);
		}
		bool Redo(engine::ang::Graph &graph) {
			return Step(graph, Future, Past);
		}

		bool CanUndo() const {
			return !Past.empty();
		}
		bool CanRedo() const {
			return !Future.empty();
		}
		void Clear() {
			Past.clear();
			Future.clear();
		}

	  private:
		bool Step(
			engine::ang::Graph &graph,
			std::vector<engine::ang::EditRecord> &from,
			std::vector<engine::ang::EditRecord> &to
		) {
			if (from.empty()) return false;
			engine::ang::EditResult result =
				graph.Apply(std::span<const engine::ang::Edit>(from.back().Inverse));
			from.pop_back();
			if (!result.Ok) {
				Clear();
				return false;
			}
			to.push_back(std::move(result.Record));
			return true;
		}

		std::vector<engine::ang::EditRecord> Past{};
		std::vector<engine::ang::EditRecord> Future{};
	};
}
