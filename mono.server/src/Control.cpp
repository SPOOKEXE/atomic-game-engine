// What a dedicated server can be asked about itself.
//
// **The editor had five tools of its own and the server had none**, which left
// the program most likely to be misbehaving unattended answering only the shared
// table: how many worlds it holds and what is in them. Nothing said whether
// anybody was connected, whether the socket was even bound, or whether the
// datagrams arriving were players or somebody knocking.
//
// **Beside the thing it exposes**, which is `mono.engine/control`'s stated
// reason for being a registry rather than a switch: a tool is one `Surface::Add`
// next to the state it reads. `mono.studio/src/Control.cpp` is the same file for
// the editor, and neither program's vocabulary leaks into the other's.
//
// Everything here runs on the tick thread, inside `Server::Pump`, and reads
// counters that are already there. None of it walks a world.

#include <engine/core/Log.hpp>
#include <engine/replication/Listener.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <server/Server.hpp>

namespace server {

	using engine::control::Tool;
	using nlohmann::json;

	void Server::RegisterControlTools() {
		Server *host = this;

		ControlSurface.Add(
			Tool{
				"engine_info",
				"This server's own state: how many scenes it holds, whether the game socket is bound "
				"and where, how many clients are on it, and what it is set to do. Call it first. Use "
				"host_link for the admission counters and host_players for who is here.",
				[] { return json{{"type", "object"}}; },
				[host](const json &, std::string &) {
					json worlds = json::array();
					for (const engine::world::WorldId id : host->Worlds().Worlds()) {
						worlds.push_back(std::string(host->Worlds().NameOf(id).Text()));
					}

					const engine::replication::Listener *clients = host->Replication.get();
					const engine::net::Endpoint bound = host->ListeningOn();

					return json{
						{"game",
						 json{
							 {"path", host->Settings.GamePath},
							 {"contentMode", Describe(host->Settings.ContentDelivery)},
						 }},
						{"server",
						 json{
							 {"tickRate", host->Settings.TickRate},
							 {"unpaced", host->Settings.Unpaced},
							 {"listening", clients != nullptr},
							 {"endpoint", clients == nullptr ? std::string() : bound.Text()},
							 {"clients", clients == nullptr ? size_t(0) : clients->Count()},
							 {"maximumClients", host->Settings.MaximumClients},
							 {"advertised", host->Settings.Advertise},
						 }},
						{"universe", json{{"worlds", std::move(worlds)}, {"count", host->Worlds().Count()}}},
						{"control",
						 json{
							 {"port", host->ControlServer.Port()},
							 {"served", host->ControlServer.Served()},
						 }},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"host_link",
				"What the game socket has done since this server started. `admitted` and `dropped` "
				"are players; `turned` is somebody refused because the server was full and `rejected` "
				"is the game's own admission policy declining one, which are different problems with "
				"different fixes. `challenged` rising while `admitted` stays flat is somebody "
				"knocking rather than a server in trouble, because a challenge costs no state.",
				[] { return json{{"type", "object"}}; },
				[host](const json &, std::string &failure) -> json {
					const engine::replication::Listener *clients = host->Replication.get();
					if (clients == nullptr) {
						failure = "this server is not listening. It was started without --listen, so "
								  "there is no game socket and no clients.";
						return nullptr;
					}

					const engine::replication::Listener::Statistics &counts = clients->Stats();
					return json{
						{"endpoint", host->ListeningOn().Text()},
						{"connected", clients->Count()},
						{"maximumClients", host->Settings.MaximumClients},
						{"admitted", counts.Admitted},
						{"dropped", counts.Dropped},
						{"turned", counts.Turned},
						{"rejected", counts.Rejected},
						{"challenged", counts.Challenged},
						{"refused", counts.Refused},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"host_players",
				"Who is in this game: one row per client that has a Player instance, with the entity "
				"id that Player is and the link's round-trip estimate in milliseconds. A client "
				"connected but not yet spawned has no row here and is still counted by host_link - "
				"the two numbers differing is the ordinary state during a join, not a fault.",
				[] { return json{{"type", "object"}}; },
				[host](const json &, std::string &failure) -> json {
					const engine::replication::Listener *clients = host->Replication.get();
					if (clients == nullptr) {
						failure = "this server is not listening. It was started without --listen, so "
								  "there is no game socket and no clients.";
						return nullptr;
					}

					json players = json::array();
					for (const auto &[slot, occupant] : host->Players) {
						const engine::replication::ClientId id{slot, occupant.Generation};
						players.push_back(
							json{
								{"client", slot},
								{"generation", occupant.Generation},
								{"player", occupant.Instance.Id},
								{"roundTripMilliseconds", clients->RoundTripMilliseconds(id)},
							}
						);
					}

					return json{
						{"players", std::move(players)},
						{"connected", clients->Count()},
					};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"admission_list",
				"Lists the client public keys allowed to start new sessions. `restricted` false means "
				"identity is optional and every client is admitted; true with no keys denies every new "
				"client.",
				[] { return json{{"type", "object"}}; },
				[host](const json &, std::string &) {
					json keys = json::array();
					for (const engine::assets::PublicKey &key : host->AdmittedClientKeys) {
						keys.push_back(key.ToHex());
					}
					return json{{"restricted", host->AdmissionRestricted}, {"keys", std::move(keys)}};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"admission_allow",
				"Allows a client public key to start future sessions and enables restricted admission. The "
				"client keeps the matching secret and proves possession by signing its live session.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 json{
							 {"key", json{{"type", "string"}, {"description", "64 lowercase hex characters"}}}
						 }},
						{"required", json::array({"key"})},
					};
				},
				[host](const json &arguments, std::string &failure) -> json {
					if (!arguments.contains("key") || !arguments["key"].is_string()) {
						failure = "key must be a 64-character lowercase hexadecimal client public key.";
						return nullptr;
					}
					const std::optional<engine::assets::PublicKey> key =
						engine::assets::PublicKey::FromHex(arguments["key"].get<std::string>());
					if (!key.has_value()) {
						failure = "key must be a 64-character lowercase hexadecimal client public key.";
						return nullptr;
					}
					const bool existed =
						std::find(host->AdmittedClientKeys.begin(), host->AdmittedClientKeys.end(), *key) !=
						host->AdmittedClientKeys.end();
					if (!existed) {
						host->AdmittedClientKeys.push_back(*key);
					}
					host->AdmissionRestricted = true;
					if (host->Replication != nullptr) {
						host->Replication->RequireClientIdentity(true);
					}
					return json{{"key", key->ToHex()}, {"added", !existed}, {"restricted", true}};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"admission_revoke",
				"Revokes a client public key for future sessions. Existing sessions are not disconnected. "
				"Revoking the final key leaves admission restricted and denies every new client.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 json{
							 {"key", json{{"type", "string"}, {"description", "64 lowercase hex characters"}}}
						 }},
						{"required", json::array({"key"})},
					};
				},
				[host](const json &arguments, std::string &failure) -> json {
					if (!arguments.contains("key") || !arguments["key"].is_string()) {
						failure = "key must be a 64-character lowercase hexadecimal client public key.";
						return nullptr;
					}
					const std::optional<engine::assets::PublicKey> key =
						engine::assets::PublicKey::FromHex(arguments["key"].get<std::string>());
					if (!key.has_value()) {
						failure = "key must be a 64-character lowercase hexadecimal client public key.";
						return nullptr;
					}
					const auto found =
						std::find(host->AdmittedClientKeys.begin(), host->AdmittedClientKeys.end(), *key);
					const bool removed = found != host->AdmittedClientKeys.end();
					if (removed) {
						host->AdmittedClientKeys.erase(found);
					}
					host->AdmissionRestricted = true;
					if (host->Replication != nullptr) {
						host->Replication->RequireClientIdentity(true);
					}
					return json{{"key", key->ToHex()}, {"removed", removed}, {"restricted", true}};
				},
			}
		);

		ControlSurface.Add(
			Tool{
				"admission_open",
				"Disables the client-key whitelist for future sessions. This is an explicit operation "
				"because "
				"an empty restricted whitelist means deny everyone, not open the server.",
				[] { return json{{"type", "object"}}; },
				[host](const json &, std::string &) {
					host->AdmissionRestricted = false;
					if (host->Replication != nullptr) {
						host->Replication->RequireClientIdentity(false);
					}
					return json{{"restricted", false}};
				},
			}
		);
	}
}
