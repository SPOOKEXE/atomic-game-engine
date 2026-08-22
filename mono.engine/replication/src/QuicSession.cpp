#include <engine/core/Profiling.hpp>
#include <engine/replication/QuicSession.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace engine::replication {

	namespace {
		// The exporter label the identity binding is derived under.
		//
		// **The version is inside the string**, for the reason
		// `net/src/Handshake.cpp` puts it inside its salt: a v2 binding is a
		// different label and therefore a different value, so a v1 peer and a v2
		// peer fail to agree rather than half-agreeing.
		constexpr std::string_view BINDING_LABEL = "atomic-replication-identity-v1";

		// The most delay a caller can ask for. One minute, so a mistaken property
		// cannot retain an unbounded queue - `Session::SetSimulatedLatency`'s
		// cap, kept the same so the two wires behave the same under it.
		constexpr double MAXIMUM_LATENCY_SECONDS = 60.0;
	}

	QuicRoute QuicRouteFor(MessageKind kind) {
		// `docs/CODE_ARCH.md` §10's table, read literally. A stream each for the
		// things whose loss is visible as an absence, and datagrams for the one
		// whose loss is covered by the next one arriving.
		switch (kind) {
		case MessageKind::SnapshotChunk:
			// **The one that pays for the whole exercise.** A join snapshot is
			// megabytes and it must not stall a door opening.
			return {0, true};
		case MessageKind::Structure:
			return {1, true};
		case MessageKind::Input:
			return {2, true};
		case MessageKind::Applied:
			return {3, true};
		case MessageKind::User:
			return {4, true};
		case MessageKind::Identify:
			return {5, true};

		case MessageKind::Delta:
			return {8, false};
		case MessageKind::GroupSignatures:
			return {9, false};
		case MessageKind::Disputed:
			return {10, false};
		}
		return {1, true};
	}

	QuicSession::QuicSession(
		std::unique_ptr<net::quic::Connection> connection, const QuicSessionSettings &settings
	)
		: Connection_(std::move(connection)), Settings(settings), Remaining(settings.BytesPerTick) {
		SetSimulatedLatency(settings.SimulatedLatencyMilliseconds);
	}

	QuicSession::~QuicSession() = default;

	std::unique_ptr<QuicSession> QuicSession::Connect(
		net::Transport &transport,
		const net::Endpoint &peer,
		double nowSeconds,
		const QuicSessionSettings &settings
	) {
		std::unique_ptr<net::quic::Connection> connection =
			net::quic::Connection::Connect(transport, peer, nowSeconds, settings.Connection);
		if (connection == nullptr) {
			return nullptr;
		}
		return std::unique_ptr<QuicSession>(new QuicSession(std::move(connection), settings));
	}

	std::unique_ptr<QuicSession> QuicSession::Accept(
		net::Transport &transport,
		const net::Endpoint &peer,
		std::span<const std::byte> datagram,
		double nowSeconds,
		const QuicSessionSettings &settings
	) {
		std::unique_ptr<net::quic::Connection> connection =
			net::quic::Connection::Accept(transport, peer, datagram, nowSeconds, settings.Connection);
		if (connection == nullptr) {
			return nullptr;
		}
		return std::unique_ptr<QuicSession>(new QuicSession(std::move(connection), settings));
	}

	const net::Endpoint &QuicSession::Peer() const {
		return Connection_->Peer();
	}

	bool QuicSession::Send(std::span<const std::byte> message, double nowSeconds) {
		ENGINE_PROFILE_CAT("QuicSession::Send", core::ProfileCategory::Network);

		if (!Carrying()) {
			return false;
		}

		// **The ceiling is enforced here, not above.** A limiter in userland runs
		// after the payload has been built, which is the half that costs, and
		// `net/AGENTS.md` says so about `Link::Reserve` for the same reason.
		const auto size = static_cast<uint32_t>(std::min<size_t>(message.size(), Remaining + 1));
		if (size > Remaining) {
			return false;
		}

		const std::optional<MessageKind> kind = PeekMessageKind(message);
		if (!kind.has_value()) {
			// A message this module cannot name is one it cannot route, and
			// guessing a channel would put it on a stream whose ordering means
			// something else. Refused at the call site that made it.
			return false;
		}
		const QuicRoute route = QuicRouteFor(*kind);

		if (SimulatedLatencySeconds > 0.0) {
			// Held before the connection rather than after it. See the note on
			// `Waiting` in the header: a packet framed now and released later
			// carries an acknowledgement that has gone stale.
			Waiting.push_back({nowSeconds + SimulatedLatencySeconds, {message.begin(), message.end()}});
			Remaining -= size;
			return true;
		}

		const bool taken = route.Reliable ? Connection_->Send(route.Channel, message, nowSeconds)
										  : Connection_->SendUnreliable(route.Channel, message, nowSeconds);
		if (taken) {
			Remaining -= size;
		}
		return taken;
	}

	size_t QuicSession::Flush(double nowSeconds) {
		ENGINE_PROFILE_CAT("QuicSession::Flush", core::ProfileCategory::Network);

		while (!Waiting.empty() && Waiting.front().ReadyAtSeconds <= nowSeconds) {
			const Delayed &next = Waiting.front();
			const std::optional<MessageKind> held = PeekMessageKind(next.Bytes);
			const QuicRoute route = QuicRouteFor(held.value_or(MessageKind::Structure));
			if (route.Reliable) {
				Connection_->Send(route.Channel, next.Bytes, nowSeconds);
			} else {
				Connection_->SendUnreliable(route.Channel, next.Bytes, nowSeconds);
			}
			Waiting.pop_front();
		}

		return Connection_->Flush(nowSeconds);
	}

	bool QuicSession::Receive(std::span<const std::byte> datagram, double nowSeconds) {
		const bool taken = Connection_->Receive(datagram, nowSeconds);
		Take(nowSeconds);
		return taken;
	}

	void QuicSession::Take(double nowSeconds) {
		(void)nowSeconds;
		for (const net::quic::Arrival &arrival : Connection_->Inbound()) {
			Arrived.push_back(arrival.Bytes);
		}
		Connection_->ClearInbound();
	}

	void QuicSession::ClearInbound() {
		Arrived.clear();
	}

	void QuicSession::Advance(double nowSeconds) {
		// **Reset at the barrier with everything else per-tick.** Resetting
		// anywhere else lets a connection spend two ticks' worth inside one -
		// `net/AGENTS.md` on budgets, which survives the change of transport
		// because it was never a fact about the framing.
		Remaining = Settings.BytesPerTick;

		// The transport's own timers are driven off the tick rather than off a
		// thread, so a session that is not otherwise sending still has to be
		// turned over or nothing retransmits and nothing times out.
		Connection_->Flush(nowSeconds);
		Take(nowSeconds);
	}

	bool QuicSession::Carrying() const {
		return Connection_->State() == net::quic::ConnectionState::Established;
	}

	bool QuicSession::Live() const {
		return Connection_->State() != net::quic::ConnectionState::Closed;
	}

	void QuicSession::Disconnect(double nowSeconds) {
		Connection_->Close(nowSeconds);
	}

	float QuicSession::RoundTripMilliseconds() const {
		return static_cast<float>(Connection_->Stats().RoundTripMilliseconds);
	}

	size_t QuicSession::SendAllowanceBytes() const {
		// The smaller of what the operator will pay for and what the path will
		// take. Reporting only the first would have an authority build a tick's
		// worth of world for a link that cannot carry it; reporting only the
		// second would ignore the ceiling that exists precisely because a path's
		// capacity is not the question a bill is a function of.
		const uint64_t window = Connection_->Stats().CongestionWindow;
		const auto ceiling = static_cast<uint64_t>(Remaining);
		if (window == 0) {
			return static_cast<size_t>(ceiling);
		}
		return static_cast<size_t>(std::min(ceiling, window));
	}

	void QuicSession::SetSimulatedLatency(double milliseconds) {
		if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
			SimulatedLatencySeconds = 0.0;
			return;
		}
		SimulatedLatencySeconds = std::min(milliseconds / 1000.0, MAXIMUM_LATENCY_SECONDS);
	}

	double QuicSession::SimulatedLatency() const {
		return SimulatedLatencySeconds * 1000.0;
	}

	bool QuicSession::Binding(std::span<std::byte> out) const {
		return Connection_->Export(BINDING_LABEL, out);
	}

	bool QuicSession::Refused() const {
		return Connection_->Refused();
	}

	net::quic::Connection::Statistics QuicSession::Stats() const {
		return Connection_->Stats();
	}
}
