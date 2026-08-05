#include "Signals.hpp"

#include <algorithm>

namespace engine::script {

	ConnectionId
	SignalTable::Connect(SignalKind kind, ecs::Entity subject, CallbackRef callback, core::Name property) {
		const uint64_t key = KeyOf(kind, subject);
		std::vector<Connection> &list = Lists[key];

		// First connection on this subject, so it joins the walk order. Recorded
		// once and never re-sorted: `EachSubject` promises the order instances
		// were first connected in, and a subject that disconnected and
		// reconnected keeping its old place is the less surprising of the two
		// answers — a script that toggles a listener does not expect its world
		// to start reporting changes in a different sequence.
		if (list.empty()) {
			SubjectOrder[static_cast<uint8_t>(kind)].push_back(subject);
		}

		const ConnectionId id = NextId++;
		list.push_back(Connection{id, callback, property, true, false});
		Owners.emplace(id, key);
		return id;
	}

	bool SignalTable::MarkOnce(ConnectionId id) {
		const auto owner = Owners.find(id);
		if (owner == Owners.end()) {
			return false;
		}

		const auto list = Lists.find(owner->second);
		if (list == Lists.end()) {
			return false;
		}

		for (Connection &connection : list->second) {
			if (connection.Id == id && connection.Live) {
				connection.Once = true;
				return true;
			}
		}
		return false;
	}

	bool SignalTable::Disconnect(ConnectionId id, CallbackRef &released) {
		const auto owner = Owners.find(id);
		if (owner == Owners.end()) {
			return false;
		}

		const auto list = Lists.find(owner->second);
		if (list == Lists.end()) {
			return false;
		}

		for (Connection &connection : list->second) {
			if (connection.Id != id || !connection.Live) {
				continue;
			}

			connection.Live = false;
			released = connection.Callback;
			Owners.erase(owner);

			// Only when nothing is walking the list. A fire in progress
			// compacts on its own way out, and doing it here would move the
			// vector under the loop — the exact failure the `Live` flag exists
			// to avoid.
			if (Firing == 0) {
				Compact(list->second);
			}
			return true;
		}
		return false;
	}

	bool SignalTable::Connected(ConnectionId id) const {
		const auto owner = Owners.find(id);
		if (owner == Owners.end()) {
			return false;
		}

		const auto list = Lists.find(owner->second);
		if (list == Lists.end()) {
			return false;
		}

		return std::any_of(list->second.begin(), list->second.end(), [id](const Connection &connection) {
			return connection.Id == id && connection.Live;
		});
	}

	void SignalTable::Fire(
		SignalKind kind, ecs::Entity subject, const std::function<void(const Connection &)> &body
	) {
		const auto list = Lists.find(KeyOf(kind, subject));
		if (list == Lists.end() || list->second.empty()) {
			return;
		}

		// Snapshotted before anything runs. A callback that connects another one
		// would otherwise extend its own iteration, and how far it got would
		// depend on where the vector happened to reallocate.
		const size_t count = list->second.size();
		Firing++;

		for (size_t index = 0; index < count; index++) {
			// Re-found each time: a callback may connect to this same signal,
			// and that can reallocate the vector the reference points into.
			// Holding one across the call was a use-after-free waiting for a
			// script that did the ordinary thing.
			const auto live = Lists.find(KeyOf(kind, subject));
			if (live == Lists.end() || index >= live->second.size()) {
				break;
			}

			const Connection connection = live->second[index];
			if (!connection.Live) {
				continue;
			}
			body(connection);
		}

		Firing--;
		if (Firing == 0) {
			// Re-found for the same reason, and after the counter drops so a
			// nested fire does not compact a list the outer one is walking.
			if (const auto live = Lists.find(KeyOf(kind, subject)); live != Lists.end()) {
				Compact(live->second);
			}
		}
	}

	void SignalTable::EachSubject(SignalKind kind, const std::function<void(ecs::Entity)> &body) const {
		const auto order = SubjectOrder.find(static_cast<uint8_t>(kind));
		if (order == SubjectOrder.end()) {
			return;
		}

		for (const ecs::Entity subject : order->second) {
			const auto list = Lists.find(KeyOf(kind, subject));
			if (list == Lists.end() || list->second.empty()) {
				continue;
			}
			body(subject);
		}
	}

	void SignalTable::DropSubject(ecs::Entity subject, std::vector<CallbackRef> &released) {
		for (const SignalKind kind :
			 {SignalKind::Heartbeat, SignalKind::Changed, SignalKind::PropertyChanged}) {
			const uint64_t key = KeyOf(kind, subject);
			const auto list = Lists.find(key);
			if (list == Lists.end()) {
				continue;
			}

			for (const Connection &connection : list->second) {
				if (connection.Live) {
					released.push_back(connection.Callback);
					Owners.erase(connection.Id);
				}
			}
			Lists.erase(list);

			// Out of the walk order too, or the vector grows by one entry per
			// destroyed instance for the rest of the world's life and
			// `EachSubject` hands out a dead handle. Nothing in the engine walks
			// subjects today — the `.Changed` fan-out is `ChangeQueue`,
			// subscribed per component with an entity filter — so this is a leak
			// first and a wrong answer only once something does.
			if (const auto order = SubjectOrder.find(static_cast<uint8_t>(kind));
				order != SubjectOrder.end()) {
				std::erase(order->second, subject);
			}
		}
	}

	void SignalTable::Clear(std::vector<CallbackRef> &released) {
		for (const auto &list : Lists) {
			for (const Connection &connection : list.second) {
				if (connection.Live) {
					released.push_back(connection.Callback);
				}
			}
		}

		Lists.clear();
		Owners.clear();
		SubjectOrder.clear();
		Firing = 0;
	}

	size_t SignalTable::Count(SignalKind kind, ecs::Entity subject) const {
		const auto list = Lists.find(KeyOf(kind, subject));
		if (list == Lists.end()) {
			return 0;
		}

		return static_cast<size_t>(
			std::count_if(list->second.begin(), list->second.end(), [](const Connection &connection) {
				return connection.Live;
			})
		);
	}

	void SignalTable::Compact(std::vector<Connection> &connections) {
		std::erase_if(connections, [](const Connection &connection) { return !connection.Live; });
	}
}
