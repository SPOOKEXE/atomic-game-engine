#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/world/TickExchange.hpp>

#include <algorithm>
#include <mutex>

namespace engine::world {
	namespace {
		constexpr size_t MAXIMUM_NAME = 256;
		constexpr size_t MAXIMUM_CHANNELS = 16;
		struct OpenChannel {
			std::string Name;
			uint64_t Incarnation = 0;
		};
		struct Endpoints {
			std::vector<OpenChannel> Open;
		};
		std::mutex ChannelLock;
		std::vector<TickExchangeChannel> Channels;
		bool Text(std::string_view text) {
			return !text.empty() && text.size() <= MAXIMUM_NAME && text.find('\0') == std::string_view::npos;
		}
		TickExchangeChannel Channel(std::string_view name) {
			const std::lock_guard held(ChannelLock);
			const auto found = std::find_if(Channels.begin(), Channels.end(), [&](const auto &value) {
				return value.Name == name;
			});
			return found == Channels.end() ? TickExchangeChannel{} : *found;
		}
		std::string ReadText(core::ByteReader &reader) {
			const size_t size = reader.ReadUInt32();
			if (size > MAXIMUM_NAME || size > reader.Remaining()) {
				reader.Fail();
				return {};
			}
			std::string result(size, '\0');
			reader.ReadRaw(result.data(), size);
			if (!Text(result)) reader.Fail();
			return result;
		}
		void WriteEndpoints(core::ByteWriter &writer, const void *source, size_t count) {
			for (size_t index = 0; index < count; ++index) {
				const auto &value = static_cast<const Endpoints *>(source)[index];
				writer.WriteUInt32(static_cast<uint32_t>(value.Open.size()));
				for (const auto &open : value.Open) {
					writer.WriteString(open.Name);
					writer.WriteUInt64(open.Incarnation);
				}
			}
		}
		void ReadEndpoints(core::ByteReader &reader, void *destination, size_t count) {
			for (size_t index = 0; index < count; ++index) {
				Endpoints value;
				const size_t size = reader.ReadUInt32();
				if (size > MAXIMUM_CHANNELS) {
					reader.Fail();
					return;
				}
				for (size_t at = 0; at < size && !reader.Failed(); ++at) {
					OpenChannel open{ReadText(reader), reader.ReadUInt64()};
					if (!open.Incarnation || (!value.Open.empty() && value.Open.back().Name >= open.Name)) {
						reader.Fail();
						return;
					}
					value.Open.push_back(std::move(open));
				}
				if (reader.Failed()) return;
				static_cast<Endpoints *>(destination)[index] = std::move(value);
			}
		}
		const OpenChannel *Opened(const ecs::Store &store, std::string_view name) {
			const auto *endpoints = store.Resource<Endpoints>();
			if (!endpoints) return nullptr;
			const auto found =
				std::find_if(endpoints->Open.begin(), endpoints->Open.end(), [&](const auto &value) {
					return value.Name == name;
				});
			return found == endpoints->Open.end() ? nullptr : &*found;
		}
		bool Valid(const TickExchangeStamp &stamp) {
			return Text(stamp.SourceWorld) && Text(stamp.DestinationWorld) && Text(stamp.Channel) &&
				   stamp.SourceIncarnation && stamp.SourceTick && stamp.Sequence;
		}
		void WriteStamp(core::ByteWriter &writer, const TickExchangeStamp &stamp) {
			writer.WriteString(stamp.SourceWorld);
			writer.WriteString(stamp.DestinationWorld);
			writer.WriteString(stamp.Channel);
			writer.WriteUInt64(stamp.SourceIncarnation);
			writer.WriteUInt64(stamp.SourceTick);
			writer.WriteUInt64(stamp.Sequence);
		}
		TickExchangeStamp ReadStamp(core::ByteReader &reader) {
			TickExchangeStamp stamp{
				ReadText(reader),
				ReadText(reader),
				ReadText(reader),
				reader.ReadUInt64(),
				reader.ReadUInt64(),
				reader.ReadUInt64()
			};
			if (!Valid(stamp)) reader.Fail();
			return stamp;
		}
		std::vector<std::byte> ReadPayload(core::ByteReader &reader) {
			const size_t size = reader.ReadUInt32();
			if (size > MAXIMUM_TICK_EXCHANGE_BYTES || size > reader.Remaining()) {
				reader.Fail();
				return {};
			}
			std::vector<std::byte> result(size);
			reader.ReadRaw(result.data(), size);
			return result;
		}
		void WritePayload(core::ByteWriter &writer, std::span<const std::byte> bytes) {
			writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
			writer.WriteRaw(bytes.data(), bytes.size());
		}
	}

	bool RegisterTickExchangeChannel(const TickExchangeChannel &channel) {
		if (!Text(channel.Name) || !channel.Collect || !channel.Serve || !channel.Apply) return false;
		const std::lock_guard held(ChannelLock);
		const auto found = std::find_if(Channels.begin(), Channels.end(), [&](const auto &value) {
			return value.Name == channel.Name;
		});
		if (found != Channels.end())
			return found->Collect == channel.Collect && found->Serve == channel.Serve &&
				   found->Apply == channel.Apply;
		Channels.push_back(channel);
		return true;
	}
	void RegisterTickExchangeComponents() {
		ecs::Components::Register<Endpoints>("world.TickExchangeEndpoints", WriteEndpoints, ReadEndpoints);
	}
	bool OpenTickExchange(ecs::Store &store, std::string_view channel, uint64_t incarnation) {
		RegisterTickExchangeComponents();
		if (store.AdoptOnly() || !incarnation || !Channel(channel).Collect) return false;
		if (const auto *open = Opened(store, channel)) return open->Incarnation == incarnation;
		Endpoints value;
		if (const auto *old = store.Resource<Endpoints>()) value = *old;
		if (value.Open.size() == MAXIMUM_CHANNELS) return false;
		value.Open.push_back({std::string(channel), incarnation});
		std::sort(value.Open.begin(), value.Open.end(), [](const auto &a, const auto &b) {
			return a.Name < b.Name;
		});
		store.SetResource(value);
		return true;
	}
	bool HasTickExchanges(const ecs::Store &store) {
		if (store.AdoptOnly()) return false;
		const auto *value = store.Resource<Endpoints>();
		return value && !value->Open.empty();
	}
	bool
	CollectTickExchanges(ecs::Store &store, std::string_view world, std::vector<TickExchangeRequest> &out) {
		ENGINE_PROFILE("collect tick exchanges");
		if (!Text(world)) return false;
		if (store.AdoptOnly()) {
			out.clear();
			return true;
		}
		const auto *endpoints = store.Resource<Endpoints>();
		if (!endpoints) {
			out.clear();
			return true;
		}
		const auto open = endpoints->Open;
		std::vector<TickExchangeRequest> requests;
		size_t bytes = 0;
		for (const auto &binding : open) {
			const auto channel = Channel(binding.Name);
			if (!channel.Collect) return false;
			std::vector<TickExchangeRequest> produced;
			channel.Collect(store, produced);
			if (produced.size() > MAXIMUM_TICK_EXCHANGE_MESSAGES - requests.size()) return false;
			uint64_t sequence = 0;
			for (auto &request : produced) {
				request.Stamp.SourceWorld = world;
				request.Stamp.Channel = binding.Name;
				request.Stamp.SourceIncarnation = binding.Incarnation;
				request.Stamp.SourceTick = store.Time().Tick;
				request.Stamp.Sequence = ++sequence;
				core::ByteWriter encoded;
				if (!WriteTickExchange(encoded, request) ||
					encoded.Size() > MAXIMUM_TICK_EXCHANGE_BYTES - bytes)
					return false;
				bytes += encoded.Size();
				requests.push_back(std::move(request));
			}
		}
		core::Metrics::Count("world.tick_exchange.request_bytes", static_cast<double>(bytes));
		core::Metrics::Count("world.tick_exchange.requests", static_cast<double>(requests.size()));
		out = std::move(requests);
		return true;
	}
	TickExchangeReply ServeTickExchange(ecs::Store &store, const TickExchangeRequest &request) {
		ENGINE_PROFILE("serve tick exchange");
		TickExchangeReply reply;
		reply.Stamp = request.Stamp;
		const auto *open = Opened(store, request.Stamp.Channel);
		const auto channel = Channel(request.Stamp.Channel);
		if (store.AdoptOnly() || !open || !channel.Serve || !Valid(request.Stamp) ||
			request.Payload.size() > MAXIMUM_TICK_EXCHANGE_BYTES)
			return reply;
		reply.DestinationIncarnation = open->Incarnation;
		reply.DestinationTick = store.Time().Tick;
		reply.Status = channel.Serve(store, request, reply.Payload);
		if (reply.Payload.size() > MAXIMUM_TICK_EXCHANGE_BYTES) {
			reply.Payload.clear();
			reply.Status = TickExchangeStatus::Overflow;
		}
		core::ByteWriter encoded;
		if (WriteTickExchange(encoded, reply))
			core::Metrics::Count("world.tick_exchange.reply_bytes", static_cast<double>(encoded.Size()));
		core::Metrics::Count("world.tick_exchange.replies", 1);
		return reply;
	}
	bool ApplyTickExchanges(ecs::Store &store, std::span<const TickExchangeReply> replies) {
		ENGINE_PROFILE("apply tick exchanges");
		if (store.AdoptOnly()) return replies.empty();
		const auto *endpoints = store.Resource<Endpoints>();
		if (!endpoints) return replies.empty();
		const auto open = endpoints->Open;
		for (const auto &reply : replies) {
			const auto *binding = Opened(store, reply.Stamp.Channel);
			if (!binding || reply.Stamp.SourceIncarnation != binding->Incarnation ||
				reply.Stamp.SourceTick != store.Time().Tick)
				return false;
		}
		for (const auto &binding : open) {
			const auto channel = Channel(binding.Name);
			if (!channel.Apply) return false;
			std::vector<TickExchangeReply> selected;
			for (const auto &reply : replies)
				if (reply.Stamp.Channel == binding.Name) selected.push_back(reply);
			channel.Apply(store, selected);
		}
		return true;
	}
	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeRequest &request) {
		if (!Valid(request.Stamp) || request.Payload.size() > MAXIMUM_TICK_EXCHANGE_BYTES) return false;
		core::ByteWriter encoded;
		encoded.WriteUInt8(1);
		WriteStamp(encoded, request.Stamp);
		WritePayload(encoded, request.Payload);
		writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
		return true;
	}
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeRequest &out) {
		if (reader.ReadUInt8() != 1) {
			reader.Fail();
			return false;
		}
		TickExchangeRequest request{ReadStamp(reader), ReadPayload(reader)};
		if (reader.Failed()) return false;
		out = std::move(request);
		return true;
	}
	bool WriteTickExchange(core::ByteWriter &writer, const TickExchangeReply &reply) {
		if (!Valid(reply.Stamp) || reply.Payload.size() > MAXIMUM_TICK_EXCHANGE_BYTES ||
			static_cast<unsigned>(reply.Status) > static_cast<unsigned>(TickExchangeStatus::Cancelled))
			return false;
		core::ByteWriter encoded;
		encoded.WriteUInt8(2);
		WriteStamp(encoded, reply.Stamp);
		encoded.WriteUInt64(reply.DestinationIncarnation);
		encoded.WriteUInt64(reply.DestinationTick);
		encoded.WriteUInt8(static_cast<uint8_t>(reply.Status));
		WritePayload(encoded, reply.Payload);
		writer.WriteRaw(encoded.Bytes().data(), encoded.Size());
		return true;
	}
	bool ReadTickExchange(core::ByteReader &reader, TickExchangeReply &out) {
		if (reader.ReadUInt8() != 2) {
			reader.Fail();
			return false;
		}
		TickExchangeReply reply;
		reply.Stamp = ReadStamp(reader);
		reply.DestinationIncarnation = reader.ReadUInt64();
		reply.DestinationTick = reader.ReadUInt64();
		reply.Status = static_cast<TickExchangeStatus>(reader.ReadUInt8());
		reply.Payload = ReadPayload(reader);
		if (static_cast<unsigned>(reply.Status) > static_cast<unsigned>(TickExchangeStatus::Cancelled))
			reader.Fail();
		if (reader.Failed()) return false;
		out = std::move(reply);
		return true;
	}
}
