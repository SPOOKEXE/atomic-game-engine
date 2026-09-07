#include <engine/world/TickExchangeHost.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine::world {
	namespace {
		constexpr uint32_t CONTROL_MAGIC = 0x58454854;
		bool Valid(TickExchangeOperation operation, uint64_t frame) {
			return frame != 0 && operation <= TickExchangeOperation::Cancel;
		}
		template <class Message>
		bool WriteBatch(core::ByteWriter &writer, const std::vector<Message> &messages) {
			if (messages.size() > MAXIMUM_TICK_EXCHANGE_MESSAGES) return false;
			writer.WriteUInt32(static_cast<uint32_t>(messages.size()));
			for (const auto &message : messages)
				if (!WriteTickExchange(writer, message) || writer.Size() > MAXIMUM_TICK_EXCHANGE_BYTES)
					return false;
			return writer.Size() <= MAXIMUM_TICK_EXCHANGE_BYTES;
		}
		template <class Message> bool ReadBatch(core::ByteReader &reader, std::vector<Message> &messages) {
			const auto count = reader.ReadUInt32();
			if (reader.Failed() || count > MAXIMUM_TICK_EXCHANGE_MESSAGES) return false;
			messages.resize(count);
			for (auto &message : messages)
				if (!ReadTickExchange(reader, message)) return false;
			return true;
		}
		void Prefix(
			core::ByteWriter &writer,
			uint8_t direction,
			TickExchangeOperation operation,
			uint64_t frame,
			uint32_t round
		) {
			writer.WriteUInt32(CONTROL_MAGIC);
			writer.WriteUInt8(direction);
			writer.WriteUInt8(static_cast<uint8_t>(operation));
			writer.WriteUInt64(frame);
			writer.WriteUInt32(round);
		}
		bool Prefix(
			core::ByteReader &reader,
			uint8_t direction,
			TickExchangeOperation &operation,
			uint64_t &frame,
			uint32_t &round
		) {
			if (reader.Remaining() > MAXIMUM_TICK_EXCHANGE_BYTES || reader.ReadUInt32() != CONTROL_MAGIC ||
				reader.ReadUInt8() != direction)
				return false;
			operation = static_cast<TickExchangeOperation>(reader.ReadUInt8());
			frame = reader.ReadUInt64();
			round = reader.ReadUInt32();
			return !reader.Failed() && Valid(operation, frame);
		}
		bool Valid(const TickExchangeCommand &command) {
			return Valid(command.Operation, command.Frame) && command.Round <= MAXIMUM_HOST_EXCHANGE_ROUNDS &&
				   std::isfinite(command.FrameSeconds) && command.FrameSeconds >= 0 &&
				   (command.Operation == TickExchangeOperation::Begin ? command.Round == 0
																	  : command.FrameSeconds == 0) &&
				   (command.Operation == TickExchangeOperation::Serve || command.Requests.empty()) &&
				   (command.Operation == TickExchangeOperation::Apply || command.Replies.empty());
		}
		bool Valid(const TickExchangeResult &result) {
			return Valid(result.Operation, result.Frame) && result.Round <= MAXIMUM_HOST_EXCHANGE_ROUNDS &&
				   result.Rounds <= MAXIMUM_HOST_EXCHANGE_ROUNDS &&
				   (result.Operation == TickExchangeOperation::Begin || result.Rounds == 0) &&
				   (result.Success ||
					(result.Requests.empty() && result.Replies.empty() && result.Rounds == 0)) &&
				   (result.Operation == TickExchangeOperation::Collect || result.Requests.empty()) &&
				   (result.Operation == TickExchangeOperation::Serve || result.Replies.empty());
		}
	}
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeCommand &command) {
		if (!Valid(command)) return false;
		core::ByteWriter encoded;
		Prefix(encoded, 0, command.Operation, command.Frame, command.Round);
		encoded.WriteFloat(command.FrameSeconds);
		if (!WriteBatch(encoded, command.Requests) || !WriteBatch(encoded, command.Replies)) return false;
		writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
		return true;
	}
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeCommand &command) {
		TickExchangeCommand read;
		if (!Prefix(reader, 0, read.Operation, read.Frame, read.Round)) return false;
		read.FrameSeconds = reader.ReadFloat();
		if (!ReadBatch(reader, read.Requests) || !ReadBatch(reader, read.Replies) || !reader.AtEnd() ||
			!Valid(read))
			return false;
		command = std::move(read);
		return true;
	}
	bool WriteTickExchangeControl(core::ByteWriter &writer, const TickExchangeResult &result) {
		if (!Valid(result)) return false;
		core::ByteWriter encoded;
		Prefix(encoded, 1, result.Operation, result.Frame, result.Round);
		encoded.WriteUInt8(result.Success ? 1 : 0);
		encoded.WriteUInt32(result.Rounds);
		if (!WriteBatch(encoded, result.Requests) || !WriteBatch(encoded, result.Replies)) return false;
		writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
		return true;
	}
	bool ReadTickExchangeControl(core::ByteReader &reader, TickExchangeResult &result) {
		TickExchangeResult read;
		if (!Prefix(reader, 1, read.Operation, read.Frame, read.Round)) return false;
		const auto success = reader.ReadUInt8();
		read.Success = success == 1;
		read.Rounds = reader.ReadUInt32();
		if (success > 1 || !ReadBatch(reader, read.Requests) || !ReadBatch(reader, read.Replies) ||
			!reader.AtEnd() || !Valid(read))
			return false;
		result = std::move(read);
		return true;
	}
	TickExchangeHost::TickExchangeHost(Universe &universe) : Worlds(universe) {}
	TickExchangeHost::~TickExchangeHost() {
		Disconnect();
	}
	void TickExchangeHost::Disconnect() {
		if (Stage != Phase::Idle) Worlds.CancelTickExchangeFrame();
		Stage = Phase::Idle;
		ActiveFrame = 0;
		LastCommand.clear();
		LastResult = {};
	}
	TickExchangeResult TickExchangeHost::Handle(const TickExchangeCommand &command) {
		TickExchangeResult result;
		result.Operation = command.Operation;
		result.Frame = command.Frame;
		result.Round = command.Round;
		core::ByteWriter encoded;
		if (!WriteTickExchangeControl(encoded, command)) return result;
		if (std::equal(
				encoded.Bytes().begin(), encoded.Bytes().end(), LastCommand.begin(), LastCommand.end()
			))
			return LastResult;
		if (command.Operation == TickExchangeOperation::Begin) {
			if (Stage != Phase::Idle || command.Frame <= LastFrame || Worlds.TickExchangeFrameOpen())
				return result;
			LastFrame = command.Frame;
			const int rounds = Worlds.BeginTickExchangeFrame(command.FrameSeconds);
			if (rounds < 0) return result;
			if (static_cast<uint32_t>(rounds) > MAXIMUM_HOST_EXCHANGE_ROUNDS) {
				Worlds.CancelTickExchangeFrame();
				return result;
			}
			ActiveFrame = command.Frame;
			NextRound = 0;
			Stage = Phase::Between;
			result.Rounds = static_cast<uint32_t>(rounds);
			result.Success = true;
		} else {
			if (Stage == Phase::Idle || command.Frame != ActiveFrame ||
				(command.Operation != TickExchangeOperation::Cancel && command.Round != NextRound))
				return result;
			switch (command.Operation) {
			case TickExchangeOperation::Collect:
				if (Stage != Phase::Between || NextRound >= MAXIMUM_HOST_EXCHANGE_ROUNDS) return result;
				result.Success =
					Worlds.BeginTickExchangeRound() && Worlds.CollectTickExchangeRequests(result.Requests);
				if (result.Success) Stage = Phase::Collected;
				break;
			case TickExchangeOperation::Serve:
				if (Stage != Phase::Collected) return result;
				result.Success = Worlds.ServeTickExchangeRequests(command.Requests, result.Replies);
				if (result.Success) Stage = Phase::Served;
				break;
			case TickExchangeOperation::Apply:
				if (Stage != Phase::Served || NextRound == std::numeric_limits<uint32_t>::max())
					return result;
				result.Success =
					Worlds.ApplyTickExchangeReplies(command.Replies) && Worlds.FinishTickExchangeRound();
				if (result.Success) {
					++NextRound;
					Stage = Phase::Between;
				}
				break;
			case TickExchangeOperation::End:
				if (Stage != Phase::Between) return result;
				result.Success = Worlds.EndTickExchangeFrame();
				if (result.Success) {
					Stage = Phase::Idle;
					ActiveFrame = 0;
				}
				break;
			case TickExchangeOperation::Cancel:
				Worlds.CancelTickExchangeFrame();
				Stage = Phase::Idle;
				ActiveFrame = 0;
				result.Success = true;
				break;
			case TickExchangeOperation::Begin:
				break;
			}
		}
		if (!result.Success) {
			result.Requests.clear();
			result.Replies.clear();
		}
		LastCommand.assign(encoded.Bytes().begin(), encoded.Bytes().end());
		LastResult = result;
		return result;
	}
}
