#include <studio/Operators.hpp>
#include <studio/Widgets.hpp>

#include <algorithm>
#include <utility>

namespace studio {

	bool OperatorTable::Add(Operator op) {
		if (op.Id == Action::Count || !op.Poll || !op.Run) {
			return false;
		}

		if (Find(op.Id) != nullptr) {
			return false;
		}

		Registered.push_back(std::move(op));
		return true;
	}

	const Operator *OperatorTable::Find(Action id) const {
		for (const Operator &op : Registered) {
			if (op.Id == id) {
				return &op;
			}
		}
		return nullptr;
	}

	Availability OperatorTable::Available(Action id) const {
		const Operator *op = Find(id);
		if (op == nullptr) {
			return Availability::No("no such command");
		}
		return op->Poll();
	}

	bool OperatorTable::Run(Action id) const {
		const Operator *op = Find(id);
		if (op == nullptr || !op->Poll().Ready) {
			return false;
		}

		op->Run();
		return true;
	}

	std::vector<const Operator *> OperatorTable::Matching(std::string_view query) const {
		// Scored alongside the pointer rather than sorted by re-scoring in the
		// comparator: `FuzzyMatch` is not free and a comparator is called
		// O(n log n) times for n scores that do not change.
		std::vector<std::pair<int, const Operator *>> scored;
		scored.reserve(Registered.size());

		for (const Operator &op : Registered) {
			int score = 0;
			if (FuzzyMatch(query, op.Name, score)) {
				scored.emplace_back(score, &op);
			}
		}

		// **Stable, so an empty query keeps registration order.** That order is
		// the one the table is written in — file, then running, then editing,
		// then panels — which is the order somebody scans a command list in, and
		// a palette that reshuffled itself on every keystroke down to nothing
		// would lose it.
		std::stable_sort(scored.begin(), scored.end(), [](const auto &left, const auto &right) {
			return left.first > right.first;
		});

		std::vector<const Operator *> found;
		found.reserve(scored.size());
		for (const auto &[score, op] : scored) {
			found.push_back(op);
		}

		return found;
	}
}
