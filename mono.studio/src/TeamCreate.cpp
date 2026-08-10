#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>

#include <imgui.h>
#include <optional>
#include <studio/Editor.hpp>
#include <studio/TeamCreate.hpp>
#include <utility>

namespace studio {

	namespace {
		// The protocol two editors have to agree on to see each other.
		//
		// **Its own number, and not the replication one.** Two builds of the
		// editor that share a wire but differ in what a project *is* should not
		// appear joinable to each other, and the thing that changes when that
		// happens is the editor's release rather than a component encoding.
		constexpr uint32_t STUDIO_PROTOCOL = 1;

		// Hex when it is exactly that, words otherwise — `cdn::Stream` makes
		// the same choice for the same reason.
		std::optional<network::SessionKey> ReadSecret(const std::string &secret) {
			if (secret.empty()) {
				return std::nullopt;
			}
			std::optional<network::SessionKey> key = network::SessionKey::FromText(secret);
			if (!key) {
				key = network::SessionKey::FromPassphrase(secret);
			}
			return key;
		}
	}

	TeamCreate::TeamCreate(CommandLog &log, engine::world::Universe &universe)
		: Log(&log), Worlds(&universe) {}

	TeamCreate::~TeamCreate() = default;

	bool TeamCreate::Host(const TeamCreateSettings &settings, std::string &error) {
		// The stream first, because the port it binds is the port the
		// announcement has to carry — `settings.Port` of zero binds an
		// ephemeral one, and an advert naming zero sends every guest nowhere.
		engine::net::TransportSettings socket;
		Socket = engine::net::MakeUdpTransport(settings.Port, socket);
		if (Socket == nullptr) {
			error = "could not open a socket to host the session on";
			return false;
		}
		Stream = EditStream::Host(*Socket, *Log, *Worlds);

		TeamCreateSettings offering = settings;
		offering.Port = Socket->Local().Port;

		if (!Open(offering, true, {}, error)) {
			Stream.reset();
			Socket.reset();
			return false;
		}
		return true;
	}

	bool TeamCreate::Join(const engine::net::Endpoint &at, double nowSeconds, std::string &error) {
		if (!at.IsValid()) {
			error = "that session has no address to join";
			return false;
		}

		// An ephemeral port: a guest is dialling out, and a well-known one
		// would be a port a second editor on the same machine could not take.
		engine::net::TransportSettings socket;
		Socket = engine::net::MakeUdpTransport(0, socket);
		if (Socket == nullptr) {
			error = "could not open a socket to join from";
			return false;
		}

		Stream = EditStream::Join(*Socket, at, nowSeconds, *Log, *Worlds);
		return true;
	}

	void TeamCreate::PublishEdits(std::span<const Command> commands, double nowSeconds) {
		if (Stream != nullptr) {
			Stream->Publish(commands, nowSeconds);
		}
	}

	bool TeamCreate::Watch(const std::string &rendezvousAddress, std::span<const std::string> secrets) {
		TeamCreateSettings watching;
		watching.RendezvousAddress = rendezvousAddress;

		std::string ignored;
		const bool opened = Open(watching, false, secrets, ignored);
		if (!opened) {
			ENGINE_WARN("team create: {}", ignored);
		}
		return opened;
	}

	bool TeamCreate::Open(
		const TeamCreateSettings &settings,
		bool announce,
		std::span<const std::string> secrets,
		std::string &error
	) {
		std::optional<network::SessionKey> key;
		if (announce && !settings.Secret.empty()) {
			key = ReadSecret(settings.Secret);
			if (!key) {
				error = "the session key is neither 64 hex characters nor a passphrase";
				return false;
			}
		}

		network::Advert advert;
		if (announce) {
			advert.Session = network::SessionId::Draw();
			if (!advert.Session.IsValid()) {
				error = "no entropy for a session id, so this editor cannot be announced";
				return false;
			}
			advert.Use = network::Purpose::Studio;
			advert.Admits = key ? network::Access::Private : network::Access::Public;
			advert.Protocol = STUDIO_PROTOCOL;
			// The wildcard with the port a peer would connect to, exactly as a
			// game server announces: the listing at the other end resolves the
			// address against where the datagram came from.
			advert.At = engine::net::Endpoint::FromIPv4({0, 0, 0, 0}, settings.Port);
			advert.Name = settings.Name.empty() ? std::string("atomic studio") : settings.Name;
			advert.Detail = settings.Project;
			advert.Peers = 1;
			advert.PeerLimit = settings.PeerLimit;
		}

		network::PresenceSettings presence;
		presence.Announce = announce;
		// An editor always watches. Hosting without seeing the others is an
		// editor that cannot tell whether anybody accepted.
		presence.Discover = true;
		presence.RendezvousAddress = settings.RendezvousAddress;
		presence.Protocol = STUDIO_PROTOCOL;
		presence.Use = network::Purpose::Studio;

		// Kept before the key is moved into the presence, because this is what
		// somebody copies out of the panel and sends to the people they are
		// inviting.
		KeyText = key ? key->Text() : std::string();

		Presence_ = network::Presence::Open(presence, advert, std::move(key));
		Announcement = Presence_->Advertised();
		Announcing = announce && Presence_->Announcing();

		for (const std::string &secret : secrets) {
			std::optional<network::SessionKey> held = ReadSecret(secret);
			if (held) {
				Presence_->Seen().Trust(std::move(*held));
			} else {
				// Named rather than dropped: a key that was meant to unlock a
				// session and did not is the reason somebody cannot see it.
				ENGINE_WARN("team create: a held key is neither 64 hex characters nor a passphrase");
			}
		}

		if (announce && !Announcing) {
			error = "nothing could be announced — see the fault";
			// Not a failure to open. The rendezvous half may still be running,
			// and an editor that refused to watch because it could not
			// broadcast would be refusing over the wrong half.
		}
		return true;
	}

	void TeamCreate::Leave(double nowSeconds) {
		// The stream before the socket it borrows. A stream outliving its
		// transport is a dangling reference in a destructor, which is the least
		// debuggable place for one.
		Stream.reset();
		Socket.reset();

		if (Presence_ != nullptr) {
			Presence_->Withdraw(nowSeconds);
			Presence_.reset();
		}
		Announcing = false;
		Announcement = {};
		KeyText.clear();
	}

	void TeamCreate::Pump(double nowSeconds) {
		if (Presence_ != nullptr) {
			Presence_->Pump(nowSeconds);
		}
		if (Stream != nullptr) {
			Stream->Pump(nowSeconds);
		}
	}

	void TeamCreate::SetCollaborators(uint16_t count) {
		if (Presence_ == nullptr || !Announcing || count == Announcement.Peers) {
			return;
		}
		Announcement.Peers = count;
		Presence_->SetAdvert(Announcement);
	}

	std::span<const network::Listing> TeamCreate::Peers() const {
		if (Presence_ == nullptr) {
			return {};
		}
		return Presence_->Seen().Listings();
	}

	network::PresenceFault TeamCreate::Fault() const {
		return Presence_ == nullptr ? network::PresenceFault::None : Presence_->Fault();
	}

	// --- the panel -----------------------------------------------------------
	//
	// **Deliberately small, and it says what it does not do.** The session layer
	// is what v0.13 asked for and what exists; the shared document is not here,
	// and a panel that implied otherwise would be the "half-added" outcome
	// AGENTS.md calls worse than not starting. So the line at the bottom of this
	// window is part of the feature rather than an apology for it.

	void Editor::DrawTeamCreate() {
		if (!ShowTeamCreate) {
			return;
		}
		if (!ImGui::Begin("Team Create", &ShowTeamCreate)) {
			ImGui::End();
			return;
		}

		const double now = engine::core::Clock::Seconds();

		if (!Team->Watching()) {
			ImGui::TextUnformatted("Not looking for anybody yet.");
			ImGui::Spacing();
			if (ImGui::Button("Look for editors")) {
				// Watching costs one socket and announces nothing, which is
				// what somebody opening this panel to look wants.
				Team->Watch(TeamPointField, {});
			}
		} else {
			if (Team->Hosting()) {
				ImGui::Text("Hosting \"%s\"", Team->Session().Name.c_str());
				ImGui::TextUnformatted(Team->Session().Session.Text().c_str());
				if (ImGui::SmallButton("Copy session id")) {
					ImGui::SetClipboardText(Team->Session().Session.Text().c_str());
				}
				if (!Team->Invitation().empty()) {
					ImGui::SameLine();
					// The key is the invitation. It has to be copyable or a
					// private session is a session of one — `SessionKey::Text`
					// carries that argument in full.
					if (ImGui::SmallButton("Copy key")) {
						ImGui::SetClipboardText(Team->Invitation().c_str());
					}
				}
			} else {
				ImGui::TextUnformatted("Watching. Nothing is being announced about this editor.");
			}

			if (Team->Fault() != network::PresenceFault::None) {
				ImGui::TextColored(
					ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "discovery: %s", network::Describe(Team->Fault())
				);
			}
		}

		ImGui::Separator();

		ImGui::InputTextWithHint("Name", "what to call this session", TeamNameField, sizeof(TeamNameField));
		ImGui::InputTextWithHint(
			"Key",
			"a passphrase, or blank to invite the whole subnet",
			TeamKeyField,
			sizeof(TeamKeyField),
			ImGuiInputTextFlags_Password
		);
		ImGui::InputTextWithHint(
			"Rendezvous", "host:port, for editors off this subnet", TeamPointField, sizeof(TeamPointField)
		);

		if (ImGui::Button(Team->Hosting() ? "Stop hosting" : "Host")) {
			if (Team->Hosting()) {
				Team->Leave(now);
			} else {
				TeamCreateSettings offering;
				offering.Name = TeamNameField;
				offering.Project = GamePath.empty() ? std::string() : GamePath.stem().string();
				offering.Secret = TeamKeyField;
				offering.RendezvousAddress = TeamPointField;
				// The port a peer would connect to is the editor's hosted
				// server, which only exists while Play is running. Zero when it
				// is not, and an announcement carrying zero is one nothing can
				// act on — so it is reported rather than hidden.
				offering.Port = 0;

				std::string trouble;
				if (!Team->Host(offering, trouble)) {
					Say("team create: " + trouble, engine::core::LogLevel::Error);
				}
			}
		}

		ImGui::Separator();

		const std::span<const network::Listing> peers = Team->Peers();
		if (peers.empty()) {
			ImGui::TextDisabled("No other editors seen.");
		} else if (ImGui::BeginTable(
					   "editors", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
				   )) {
			ImGui::TableSetupColumn("Editor");
			ImGui::TableSetupColumn("Project");
			ImGui::TableSetupColumn("Found");
			ImGui::TableSetupColumn("Address");
			ImGui::TableHeadersRow();

			for (const network::Listing &row : peers) {
				if (row.Session.Session == Team->Session().Session) {
					// This editor, listed back by the rendezvous point. Useful
					// to know and not useful to look at.
					continue;
				}
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.Session.Name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.Session.Detail.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(network::Describe(row.Via));
				ImGui::TableNextColumn();
				if (row.Joinable()) {
					ImGui::TextUnformatted(row.Dial().Text().c_str());
				} else {
					ImGui::TextDisabled(
						"%s", row.Session.Admits == network::Access::Private ? "locked" : "full"
					);
				}
			}
			ImGui::EndTable();
		}

		ImGui::Separator();

		if (const EditStream *stream = Team->Edits()) {
			const EditCounters &edits = stream->Counters();
			ImGui::Text(
				"%s · %zu editor%s",
				stream->Connected() ? "connected" : "connecting",
				stream->Editors(),
				stream->Editors() == 1 ? "" : "s"
			);
			ImGui::TextDisabled(
				"sent %llu · received %llu · applied %llu",
				static_cast<unsigned long long>(edits.Sent),
				static_cast<unsigned long long>(edits.Received),
				static_cast<unsigned long long>(edits.Applied)
			);
			if (edits.Undelivered > 0 || edits.Malformed > 0) {
				// **Both are worth seeing and they are different problems.**
				// Undelivered is this end's link refusing; malformed is the
				// other end sending something this build cannot read.
				ImGui::TextColored(
					ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
					"undelivered %llu · malformed %llu",
					static_cast<unsigned long long>(edits.Undelivered),
					static_cast<unsigned long long>(edits.Malformed)
				);
			}
		} else {
			ImGui::TextDisabled("Host or join to share edits.");
		}

		ImGui::TextDisabled(
			"Edits replicate; undo does not. Ctrl+Z reverses what you did,\n"
			"never what somebody else did."
		);

		ImGui::End();
	}
}
