#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/world/Driver.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace engine::world {
	bool Driver::TickHosts(float frameSeconds, double now) {
		ENGINE_PROFILE("driver tick exchange");
		if (!std::isfinite(Settings_.TickExchangeSeconds) || Settings_.TickExchangeSeconds <= 0 ||
			Settings_.TickExchangeSeconds > 60 || ExchangeFrame == std::numeric_limits<uint64_t>::max())
			return false;
		struct Participant {
			core::Name Host;
			TickExchangeCommand Command;
			TickExchangeResult Result;
			std::vector<TickExchangeReply> ApplyReplies;
			std::vector<std::pair<size_t, size_t>> ReplyTargets;
			bool Waiting = false;
		};
		std::vector<Participant> participants(1);
		for (const auto &host : Supervisor_.Hosts()) {
			// Retired worlds answer Unavailable through the directory below. Their
			// failure must not freeze unrelated surviving worlds indefinitely.
			if (host.State == HostState::Failed) {
				if (host.Linked) Supervisor_.CloseLink(host.Name);
				continue;
			}
			if (!host.Linked || !host.Ready || host.State != HostState::Running) return false;
			participants.emplace_back();
			participants.back().Host = host.Name;
		}
		const auto started = std::chrono::steady_clock::now();
		const auto deadline = started + std::chrono::duration<double>(Settings_.TickExchangeSeconds);
		const uint64_t frame = ++ExchangeFrame;
		uint32_t round = 0;
		const auto awaitReplies = [&](auto until, bool requireSuccess) {
			for (;;) {
				const double elapsed =
					std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
				Stats.FramesReceived += Supervisor_.Pump(now + elapsed);
				bool waiting = false;
				for (size_t at = 1; at < participants.size(); ++at) {
					auto &participant = participants[at];
					if (!participant.Waiting) continue;
					if (auto result = Supervisor_.TakeTickExchange(participant.Host)) {
						participant.Result = std::move(*result);
						participant.Waiting = false;
						if (requireSuccess && !participant.Result.Success) return false;
					} else {
						if (!Supervisor_.StatusOf(participant.Host).Linked) return false;
						waiting = true;
					}
				}
				if (!waiting) return true;
				if (std::chrono::steady_clock::now() >= until) return false;
				ENGINE_PROFILE_CAT("host phase wait", core::ProfileCategory::Idle);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		};
		const auto phase = [&](TickExchangeOperation operation) {
			for (auto &participant : participants) {
				participant.Command.Operation = operation;
				participant.Command.Frame = frame;
				participant.Command.Round = round;
				participant.Command.FrameSeconds =
					operation == TickExchangeOperation::Begin ? frameSeconds : 0;
			}
			for (size_t at = 1; at < participants.size(); ++at) {
				auto &participant = participants[at];
				if (!Supervisor_.SendTickExchange(participant.Host, participant.Command)) return false;
				participant.Waiting = true;
			}
			participants[0].Result = LocalExchange.Handle(participants[0].Command);
			return participants[0].Result.Success && awaitReplies(deadline, true);
		};
		const auto cancel = [&] {
			LocalExchange.Disconnect();
			for (size_t at = 1; at < participants.size(); ++at) {
				auto &participant = participants[at];
				TickExchangeCommand command;
				command.Operation = TickExchangeOperation::Cancel;
				command.Frame = frame;
				command.Round = round;
				participant.Waiting = Supervisor_.SendTickExchange(participant.Host, command);
				if (!participant.Waiting) Supervisor_.CloseLink(participant.Host);
			}
			// An unacknowledged cancellation cannot leak a paused frame into the
			// next driver tick. Closing the link makes the host cancel on disconnect.
			(void)awaitReplies(std::chrono::steady_clock::now() + std::chrono::milliseconds(100), false);
			for (size_t at = 1; at < participants.size(); ++at)
				if (participants[at].Waiting) Supervisor_.CloseLink(participants[at].Host);
			ENGINE_WARN("driver cancelled host exchange frame {} round {}", frame, round);
			return false;
		};
		if (!phase(TickExchangeOperation::Begin)) return cancel();
		uint32_t rounds = 0;
		for (const auto &participant : participants)
			rounds = std::max(rounds, participant.Result.Rounds);
		for (; round < rounds; ++round) {
			if (!phase(TickExchangeOperation::Collect)) return cancel();
			for (size_t source = 0; source < participants.size(); ++source) {
				auto &origin = participants[source];
				for (auto &request : origin.Result.Requests) {
					const size_t replyIndex = origin.ApplyReplies.size();
					TickExchangeReply unavailable;
					unavailable.Stamp = request.Stamp;
					origin.ApplyReplies.push_back(std::move(unavailable));
					const auto destination = Universe_.Find(core::Name(request.Stamp.DestinationWorld));
					if (!destination.IsValid()) continue;
					const auto host = Universe_.HostOf(destination);
					const auto target =
						std::find_if(participants.begin(), participants.end(), [host](const auto &p) {
							return p.Host == host;
						});
					if (target == participants.end()) continue;
					if (target->Command.Requests.size() >= MAXIMUM_TICK_EXCHANGE_MESSAGES) return cancel();
					target->ReplyTargets.emplace_back(source, replyIndex);
					target->Command.Requests.push_back(std::move(request));
				}
			}
			if (!phase(TickExchangeOperation::Serve)) return cancel();
			for (auto &destination : participants) {
				if (destination.Result.Replies.size() != destination.ReplyTargets.size()) return cancel();
				for (size_t at = 0; at < destination.ReplyTargets.size(); ++at) {
					const auto [source, reply] = destination.ReplyTargets[at];
					participants[source].ApplyReplies[reply] = std::move(destination.Result.Replies[at]);
				}
				destination.Command.Requests.clear();
				destination.ReplyTargets.clear();
			}
			for (auto &participant : participants) {
				participant.Command.Replies = std::move(participant.ApplyReplies);
				participant.ApplyReplies.clear();
			}
			if (!phase(TickExchangeOperation::Apply)) return cancel();
			for (auto &participant : participants)
				participant.Command.Replies.clear();
		}
		if (!phase(TickExchangeOperation::End)) return cancel();
		return true;
	}
}
