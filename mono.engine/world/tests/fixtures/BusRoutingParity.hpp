#pragma once

#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Independent sender-string, sequence and full-message oracle. No private router
// state or production encoder is used, and no interned identifier is exported.
namespace bus_routing_fixture {
	using engine::core::Name;
	using engine::ecs::Store;
	using engine::world::BusBudget;
	using engine::world::BusKind;
	using engine::world::BusOperation;
	using engine::world::BusStatus;
	using engine::world::Delivery;
	using engine::world::Envelope;
	using engine::world::ExecutionMode;
	using engine::world::Postbox;
	using engine::world::RegisterMailboxTypes;
	using engine::world::Ticket;
	using engine::world::Universe;
	using engine::world::UniverseSettings;
	using engine::world::WorldId;
	using engine::world::WorldSettings;
	using engine::world::WorldStatus;
	inline void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}
	struct Canonical {
		uint64_t Hash = 14695981039346656037ULL;
		uint64_t Bytes = 0;
		uint64_t PayloadBytes = 0;
		std::vector<std::byte> Words;
		bool Export = false;
		void Byte(uint8_t value) {
			Hash = (Hash ^ value) * 1099511628211ULL;
			++Bytes;
			if (Export) Words.push_back(static_cast<std::byte>(value));
		}
		void Number(uint64_t value) {
			for (unsigned shift = 0; shift < 64; shift += 8)
				Byte(static_cast<uint8_t>(value >> shift));
		}
		void Text(std::string_view value) {
			Number(value.size());
			for (char byte : value)
				Byte(static_cast<uint8_t>(byte));
		}
		void Payload(std::span<const std::byte> bytes) {
			Number(bytes.size());
			PayloadBytes += bytes.size();
			for (std::byte byte : bytes)
				Byte(std::to_integer<uint8_t>(byte));
		}
		void Request(const Envelope &value) {
			Number(static_cast<uint8_t>(value.Bus));
			Number(static_cast<uint8_t>(value.Operation));
			Text(value.Key.Text());
			Text(value.From.Text());
			Text(value.Target.Text());
			Number(value.Sequence);
			Number(value.Reply.Value);
			Number(value.Version);
			Payload(value.Payload);
		}
		void Arrival(const Delivery &value) {
			Number(static_cast<uint8_t>(value.Bus));
			Text(value.Key.Text());
			Text(value.From.Text());
			Number(value.Reply.Value);
			Number(static_cast<uint8_t>(value.Status));
			Number(value.Version);
			Payload(value.Payload);
		}
		void Print(unsigned call, unsigned pass, const char *stage) const {
			if (!Export) return;
			std::printf(
				"# world-bus-canonical stage=%s call=%u pass=%u bytes=%" PRIu64 " hash=%016" PRIx64 " words=",
				stage,
				call,
				pass,
				Bytes,
				Hash
			);
			constexpr std::string_view digits = "0123456789abcdef";
			std::array<char, 4096> block{};
			size_t used = 0;
			for (std::byte byte : Words) {
				const auto value = std::to_integer<unsigned>(byte);
				block[used++] = digits[value >> 4];
				block[used++] = digits[value & 15];
				if (used == block.size()) {
					std::fwrite(block.data(), 1, used, stdout);
					used = 0;
				}
			}
			if (used) std::fwrite(block.data(), 1, used, stdout);
			std::putchar('\n');
		}
	};
	inline void CheckRequest(
		const Envelope &actual,
		Name sender,
		std::string_view topic,
		uint64_t sequence,
		std::span<const std::byte> payload
	) {
		Require(
			actual.Bus == BusKind::Messaging && actual.Operation == BusOperation::Publish &&
				actual.From == sender && actual.Key.Text() == topic && actual.Target == Name{} &&
				actual.Sequence == sequence && actual.Reply.Value == Ticket::NONE && actual.Version == 0 &&
				std::ranges::equal(actual.Payload, payload),
			"bus routing full envelope oracle failed"
		);
	}
	inline void CheckArrival(
		const Delivery &actual, Name sender, std::string_view topic, std::span<const std::byte> payload
	) {
		Require(
			actual.Bus == BusKind::Messaging && actual.From == sender && actual.Key.Text() == topic &&
				actual.Reply.Value == Ticket::NONE && actual.Status == BusStatus::Ok && actual.Version == 0 &&
				std::ranges::equal(actual.Payload, payload),
			"bus routing full delivery oracle failed"
		);
	}
	struct ChattyOracle {
		uint64_t InputHash = 0;
		std::array<Name, 50> Names{};
		std::array<WorldId, 50> Worlds{};
		explicit ChattyOracle(Universe &universe) {
			for (size_t index = 0; index < Names.size(); ++index)
				Names[index] = Name("bench.chat." + std::to_string(index));
			std::sort(Names.begin(), Names.end(), [](Name left, Name right) {
				return left.Text() < right.Text();
			});
			for (size_t index = 0; index < Names.size(); ++index) {
				Worlds[index] = universe.Find(Names[index]);
				Require(Worlds[index].IsValid(), "bus benchmark authored world missing");
			}
			InputHash = AuthoredHash(universe);
		}
		uint64_t AuthoredHash(const Universe &universe) const {
			Canonical input;
			input.Text(
				"serial chatty; subscribe tick<=1; publish every later tick; eight warmups; 50 ticks per call"
			);
			input.Text("chat");
			input.Text("bench.topic");
			input.Number(static_cast<uint8_t>(universe.Settings().Mode));
			input.Number(std::bit_cast<uint32_t>(1.0f / 60.0f));
			input.Number(2);
			input.Number(Names.size());
			for (size_t index = 0; index < Names.size(); ++index) {
				const auto settings = universe.SettingsOf(Worlds[index]);
				Require(
					settings.Name == Names[index] && settings.TickRate == 60 &&
						universe.Settings().Mode == ExecutionMode::WorldSerial,
					"bus benchmark authored settings changed"
				);
				input.Text(settings.Name.Text());
				input.Number(std::bit_cast<uint64_t>(settings.TickRate));
			}
			return input.Hash;
		}
		Canonical Verify(Universe &universe, uint64_t sequence, bool exportWords) const {
			Require(AuthoredHash(universe) == InputHash, "bus benchmark immutable authored hash changed");
			Canonical result;
			result.Export = exportWords;
			// Two setup ticks precede captures; subscription consumes sequence one.
			const auto stats = universe.Statistics();
			Require(
				stats.BusOperations == 50 && stats.Deliveries == 2450 &&
					stats.SimulationTicks == Names.size() * (sequence + 1) && stats.ActiveWorlds == 50 &&
					stats.Faulted == 0 && stats.Suspended == 0 && stats.Remote == 0,
				"bus benchmark full tick/count oracle failed"
			);
			result.Number(stats.BusOperations);
			result.Number(stats.Deliveries);
			result.Number(stats.SimulationTicks);
			result.Number(stats.ActiveWorlds);
			result.Number(stats.Suspended);
			result.Number(stats.Remote);
			result.Number(stats.Faulted);
			const auto traffic = universe.LastTraffic();
			Require(traffic.size() == Names.size(), "bus benchmark traffic cardinality failed");
			result.Number(traffic.size());
			for (size_t sender = 0; sender < Names.size(); ++sender) {
				CheckRequest(traffic[sender], Names[sender], "bench.topic", sequence, {});
				result.Request(traffic[sender]);
			}
			for (size_t receiver = 0; receiver < Worlds.size(); ++receiver) {
				result.Text(Names[receiver].Text());
				const auto status = universe.Enter(Worlds[receiver], [&](Store &store) {
					const auto arrived = Postbox(store).Deliveries();
					Require(arrived.size() == 49, "bus benchmark receiver cardinality failed");
					result.Number(arrived.size());
					size_t offset = 0;
					for (size_t sender = 0; sender < Names.size(); ++sender) {
						if (sender == receiver) continue;
						CheckArrival(arrived[offset], Names[sender], "bench.topic", {});
						result.Arrival(arrived[offset++]);
					}
				});
				Require(status == WorldStatus::Ok, "bus benchmark receiver unavailable");
			}
			return result;
		}
	};
	// Every fixture access proves the world was reachable; a skipped callback is
	// never accepted as an empty inbox or a successful refused request.
	template <class Body> inline void Enter(Universe &universe, WorldId world, Body &&body) {
		Require(
			universe.Enter(world, std::forward<Body>(body)) == WorldStatus::Ok,
			"bus oracle world access refused"
		);
	}
	struct PreflightResult {
		size_t Requests = 0;
		size_t Deliveries = 0;
		size_t PayloadBytes = 0;
	};
	inline PreflightResult Preflight() {
		RegisterMailboxTypes();
		UniverseSettings settings;
		settings.Mode = ExecutionMode::WorldSerial;
		Universe universe(settings);
		constexpr std::array<std::string_view, 4> authored = {
			"bus.oracle.20", "bus.oracle.3", "bus.oracle.11", "bus.oracle.2"
		};
		std::array<Name, 4> names{};
		std::array<WorldId, 4> worlds{};
		for (size_t index = 0; index < names.size(); ++index) {
			names[index] = Name(authored[index]);
			WorldSettings world;
			world.Name = names[index];
			world.TickRate = 60;
			worlds[index] = universe.Create(world);
		}
		universe.Tick(1.0f / 60.0f);
		for (WorldId world : worlds)
			Enter(universe, world, [](Store &store) {
				Require(Postbox(store).Subscribe("oracle.topic"), "oracle subscribe refused");
			});
		universe.Tick(1.0f / 60.0f);
		std::array<std::array<std::vector<std::byte>, 2>, 4> expected;
		for (size_t sender = 0; sender < worlds.size(); ++sender) {
			for (size_t ordinal = 0; ordinal < 2; ++ordinal) {
				auto &bytes = expected[sender][ordinal];
				bytes.resize(ordinal == 0 ? 0 : 37);
				for (size_t byte = 0; byte < bytes.size(); ++byte)
					bytes[byte] = static_cast<std::byte>((sender * 71 + ordinal * 29 + byte * 13) & 255);
				auto source = bytes;
				Enter(universe, worlds[sender], [&](Store &store) {
					Require(Postbox(store).Publish("oracle.topic", source), "oracle publish refused");
				});
				std::fill(source.begin(), source.end(), std::byte{0xff});
			}
		}
		universe.Tick(1.0f / 60.0f);
		std::array<size_t, 4> ordered = {0, 1, 2, 3};
		std::sort(ordered.begin(), ordered.end(), [&](size_t left, size_t right) {
			return names[left].Text() < names[right].Text();
		});
		PreflightResult result;
		auto verify = [&](const std::vector<Envelope> *replayed) {
			const auto traffic = universe.LastTraffic();
			Require(
				traffic.size() == 8 && universe.Statistics().BusOperations == 8 &&
					universe.Statistics().Deliveries == 24,
				"oracle complete routing counts failed"
			);
			for (size_t index = 0; index < traffic.size(); ++index) {
				const size_t sender =
					replayed
						? static_cast<size_t>(
							  std::find(names.begin(), names.end(), (*replayed)[index].From) - names.begin()
						  )
						: ordered[index / 2];
				const size_t ordinal =
					replayed ? static_cast<size_t>((*replayed)[index].Sequence - 2) : index % 2;
				Require(sender < 4 && ordinal < 2, "oracle replay reference invalid");
				CheckRequest(
					traffic[index], names[sender], "oracle.topic", ordinal + 2, expected[sender][ordinal]
				);
				++result.Requests;
				result.PayloadBytes += traffic[index].Payload.size();
			}
			for (size_t receiver = 0; receiver < worlds.size(); ++receiver) {
				Enter(universe, worlds[receiver], [&](Store &store) {
					const auto arrived = Postbox(store).Deliveries();
					Require(arrived.size() == 6, "oracle full inbox cardinality failed");
					size_t offset = 0;
					for (const auto &request : traffic) {
						if (request.From == names[receiver]) continue;
						CheckArrival(arrived[offset++], request.From, "oracle.topic", request.Payload);
						++result.Deliveries;
						result.PayloadBytes += request.Payload.size();
					}
				});
			}
		};
		verify(nullptr);
		// Recorded order deliberately differs from sender order. Reconstructed local
		// requests must be discarded, and the injected sequence must stay intact.
		std::vector<Envelope> replay(universe.LastTraffic().begin(), universe.LastTraffic().end());
		std::reverse(replay.begin(), replay.end());
		for (WorldId world : worlds)
			Enter(universe, world, [](Store &store) {
				Require(Postbox(store).Publish("oracle.topic"), "oracle duplicate setup refused");
			});
		universe.InjectTraffic(replay);
		universe.Tick(1.0f / 60.0f);
		verify(&replay);
		// An empty recording must not repeat the previous recorded traffic.
		universe.InjectTraffic({});
		universe.Tick(1.0f / 60.0f);
		Require(
			universe.LastTraffic().empty() && universe.Statistics().Deliveries == 0,
			"oracle empty replay repeated traffic"
		);
		Universe controls(settings);
		WorldSettings senderSettings;
		senderSettings.Name = Name("control.a");
		senderSettings.TickRate = 60;
		WorldSettings receiverSettings;
		receiverSettings.Name = Name("control.b");
		receiverSettings.TickRate = 60;
		const auto sender = controls.Create(senderSettings), receiver = controls.Create(receiverSettings);
		controls.Tick(1.0f / 60.0f);
		Enter(controls, receiver, [](Store &store) {
			Require(Postbox(store).Subscribe("control.topic"), "oracle control subscribe refused");
		});
		controls.Tick(1.0f / 60.0f);
		Enter(controls, receiver, [](Store &store) {
			Require(Postbox(store).Unsubscribe("control.topic"), "oracle control unsubscribe refused");
		});
		controls.Tick(1.0f / 60.0f);
		Enter(controls, sender, [](Store &store) {
			Require(Postbox(store).Publish("control.topic"), "oracle control publish refused");
		});
		controls.Tick(1.0f / 60.0f);
		Require(
			controls.Statistics().BusOperations == 1 && controls.Statistics().Deliveries == 0,
			"oracle unsubscribe retained recipient"
		);
		Enter(controls, receiver, [](Store &store) {
			Require(Postbox(store).Subscribe("control.topic"), "oracle resubscribe refused");
		});
		controls.Tick(1.0f / 60.0f);
		const std::array<std::byte, 3> budgetPayload = {std::byte{0}, std::byte{0x80}, std::byte{0xff}};
		Enter(controls, sender, [&](Store &store) {
			store.SetResource<BusBudget>(BusBudget{1, 0});
			Require(
				Postbox(store).Publish("control.topic", budgetPayload), "oracle admitted publish refused"
			);
			Require(
				!Postbox(store).Publish("control.topic", budgetPayload), "oracle over-budget publish admitted"
			);
		});
		controls.Tick(1.0f / 60.0f);
		Require(
			controls.Statistics().BusOperations == 1 && controls.Statistics().Deliveries == 1,
			"oracle refused publish altered traffic"
		);
		Enter(controls, receiver, [&](Store &store) {
			const auto arrived = Postbox(store).Deliveries();
			Require(arrived.size() == 1, "oracle budget receiver duplicated traffic");
			CheckArrival(arrived.front(), senderSettings.Name, "control.topic", budgetPayload);
		});
		Require(controls.Destroy(receiver) == WorldStatus::Ok, "oracle recipient removal refused");
		controls.Tick(1.0f / 60.0f);
		const auto replacement = controls.Create(receiverSettings);
		Require(replacement.IsValid(), "oracle recipient replacement refused");
		controls.Tick(1.0f / 60.0f);
		Enter(controls, sender, [](Store &store) {
			Require(Postbox(store).Publish("control.topic"), "oracle replacement publish refused");
		});
		controls.Tick(1.0f / 60.0f);
		Require(
			controls.Statistics().BusOperations == 1 && controls.Statistics().Deliveries == 0,
			"oracle replacement inherited subscription"
		);
		UniverseSettings federatedSettings = settings;
		federatedSettings.Federated = true;
		Universe federated(federatedSettings);
		const auto later = federated.Create(receiverSettings), earlier = federated.Create(senderSettings);
		federated.Tick(1.0f / 60.0f);
		for (auto world : {later, earlier})
			Enter(federated, world, [](Store &store) {
				Require(Postbox(store).Publish("control.topic"), "oracle federated publish refused");
			});
		federated.Tick(1.0f / 60.0f);
		Require(
			federated.LastTraffic().size() == 2 && federated.Statistics().Deliveries == 0,
			"oracle federated collection counts failed"
		);
		CheckRequest(federated.LastTraffic()[0], senderSettings.Name, "control.topic", 1, {});
		CheckRequest(federated.LastTraffic()[1], receiverSettings.Name, "control.topic", 1, {});
		Require(
			result.Requests == 16 && result.Deliveries == 48 && result.PayloadBytes == 1184,
			"bus oracle incomplete field traversal"
		);
		return result;
	}
}
