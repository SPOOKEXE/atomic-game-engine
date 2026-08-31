#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/replication/Listener.hpp>

#include <algorithm>
#include <imgui.h>
#include <optional>
#include <span>
#include <studio/Editor.hpp>
#include <studio/TeamCreate.hpp>
#include <utility>
#include <vector>

namespace studio {

	namespace {
		// The protocol two editors have to agree on to see each other.
		//
		// **Its own number, and not the replication one.** Two builds of the
		// editor that share a wire but differ in what a project *is* should not
		// appear joinable to each other, and the thing that changes when that
		// happens is the editor's release rather than a component encoding.
		constexpr uint32_t STUDIO_PROTOCOL = 1;

		// Hex when it is exactly that, words otherwise - `cdn::Stream` makes
		// the same choice for the same reason.
		std::optional<network::SessionKey> ReadSecret(std::string_view secret) {
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
		std::optional<network::SessionKey> key;
		if (!settings.Secret.empty()) {
			key = ReadSecret(settings.Secret);
			if (!key) {
				error = "the session key is neither 64 hex characters nor a passphrase";
				return false;
			}
		}

		// The stream first, because the port it binds is the port the
		// announcement has to carry - `settings.Port` of zero binds an
		// ephemeral one, and an advert naming zero sends every guest nowhere.
		engine::net::TransportSettings socket;
		std::unique_ptr<engine::net::Transport> nextSocket =
			engine::net::MakeUdpTransport(settings.Port, socket);
		if (nextSocket == nullptr) {
			error = "could not open a socket to host the session on";
			return false;
		}
		std::unique_ptr<EditStream> nextStream =
			EditStream::Host(*nextSocket, *Log, *Worlds, settings.PeerLimit, key ? &*key : nullptr);
		if (nextStream == nullptr) {
			error = "could not derive the private session identity";
			return false;
		}

		TeamCreateSettings offering = settings;
		offering.Port = nextSocket->Local().Port;

		if (!Open(offering, true, {}, error, std::move(key))) {
			return false;
		}

		// The stream borrows its transport, so replacement follows destruction
		// order as deliberately as Leave does.
		Stream.reset();
		Socket.reset();
		Socket = std::move(nextSocket);
		Stream = std::move(nextStream);
		return true;
	}

	bool TeamCreate::Join(
		const engine::net::Endpoint &at, double nowSeconds, std::string &error, std::string_view secret
	) {
		if (!at.IsValid()) {
			error = "that session has no address to join";
			return false;
		}

		// An ephemeral port: a guest is dialling out, and a well-known one
		// would be a port a second editor on the same machine could not take.
		engine::net::TransportSettings socket;
		std::unique_ptr<engine::net::Transport> nextSocket = engine::net::MakeUdpTransport(0, socket);
		if (nextSocket == nullptr) {
			error = "could not open a socket to join from";
			return false;
		}

		std::optional<network::SessionKey> key;
		if (!secret.empty()) {
			key = ReadSecret(secret);
			if (!key) {
				error = "the session key is neither 64 hex characters nor a passphrase";
				return false;
			}
		}

		std::unique_ptr<EditStream> nextStream =
			EditStream::Join(*nextSocket, at, nowSeconds, *Log, *Worlds, key ? &*key : nullptr);
		if (nextStream == nullptr) {
			error = "could not derive the private session identity";
			return false;
		}
		Stream.reset();
		Socket.reset();
		Socket = std::move(nextSocket);
		Stream = std::move(nextStream);
		return true;
	}

	void TeamCreate::PublishEdits(uint64_t waypoint, std::span<const Command> commands, double nowSeconds) {
		if (Stream != nullptr) {
			Stream->Publish(waypoint, commands, nowSeconds);
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
		std::string &error,
		std::optional<network::SessionKey> key
	) {
		if (announce && !key && !settings.Secret.empty()) {
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
			// **The default, said rather than assumed.** A team-create host is a
			// `replication::Listener` on its settings' default wire, which is
			// QUIC as of v0.19, so this is that value repeated where a peer can
			// read it before dialling.
			advert.Transports = engine::replication::ListenerSettings{}.Wire;
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
			error = "nothing could be announced - see the fault";
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
			if (Announcing) {
				SetCollaborators(static_cast<uint16_t>(Stream->Editors()));
			}
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

	// --- the menu and panel --------------------------------------------------

	void Editor::WatchForTeam() {
		if (Team == nullptr) {
			TeamStatus = "team create is not initialised";
			return;
		}

		std::vector<std::string> keys;
		if (TeamKeyField[0] != '\0') {
			keys.emplace_back(TeamKeyField);
		}
		if (Team->Watch(TeamPointField, keys)) {
			TeamStatus = keys.empty() ? "looking for public sessions" : "looking for invited sessions";
		} else {
			TeamStatus = "the invitation key could not be used";
		}
	}

	void Editor::HostTeam() {
		if (Team == nullptr) {
			TeamStatus = "team create is not initialised";
			return;
		}
		if (TeamPrivateField && TeamKeyField[0] == '\0') {
			TeamStatus = "a private session needs an invitation key or passphrase";
			return;
		}

		TeamCreateSettings offering;
		offering.Name = TeamNameField;
		offering.Project = GamePath.empty() ? std::string() : GamePath.stem().string();
		offering.Secret = TeamPrivateField ? std::string(TeamKeyField) : std::string();
		offering.RendezvousAddress = TeamPointField;
		offering.Port = static_cast<uint16_t>(std::clamp(TeamPortField, 0, 65535));
		offering.PeerLimit = static_cast<uint16_t>(std::clamp(TeamPeerLimitField, 0, 65535));

		std::string trouble;
		if (!Team->Host(offering, trouble)) {
			TeamStatus = trouble;
			Say("team create: " + trouble, engine::core::LogLevel::Error);
			return;
		}

		TeamStatus = "hosting on " + Team->Session().At.Text();
	}

	void Editor::JoinTeam(const engine::net::Endpoint &address, bool privateSession) {
		if (Team == nullptr) {
			TeamStatus = "team create is not initialised";
			return;
		}
		if (Team->Hosting()) {
			TeamStatus = "stop hosting before joining another session";
			return;
		}
		if (privateSession && TeamKeyField[0] == '\0') {
			TeamStatus = "a private session needs its invitation key";
			return;
		}

		std::string trouble;
		if (!Team->Join(
				address,
				engine::core::Clock::Seconds(),
				trouble,
				privateSession ? std::string_view(TeamKeyField) : std::string_view()
			)) {
			TeamStatus = trouble;
			Say("team create: " + trouble, engine::core::LogLevel::Error);
			return;
		}
		TeamStatus = "connecting to " + address.Text();
	}

	void Editor::LeaveTeam() {
		if (Team == nullptr) {
			return;
		}
		Team->Leave(engine::core::Clock::Seconds());
		TeamStatus = "left team create";
	}

	void Editor::DrawTeamCreateMenu() {
		if (ImGui::MenuItem("Open Team Create", nullptr, ShowTeamCreate)) {
			ShowTeamCreate = true;
		}
		ImGui::Separator();

		if (Team == nullptr) {
			ImGui::TextDisabled("Not initialised");
			return;
		}

		if (!Team->Watching()) {
			if (ImGui::MenuItem("Look for Editors")) {
				WatchForTeam();
			}
		} else {
			ImGui::TextDisabled(
				"%zu session%s found", Team->Peers().size(), Team->Peers().size() == 1 ? "" : "s"
			);
		}

		const bool joined = Team->Edits() != nullptr && !Team->Hosting();
		ImGui::BeginDisabled(joined);
		if (ImGui::MenuItem(Team->Hosting() ? "Stop Hosting" : "Host Session", nullptr, Team->Hosting())) {
			if (Team->Hosting()) {
				LeaveTeam();
			} else {
				HostTeam();
			}
		}
		ImGui::EndDisabled();

		if (!Team->Hosting() && (Team->Watching() || Team->Edits() != nullptr)) {
			if (ImGui::MenuItem(joined ? "Leave Session" : "Stop Looking")) {
				LeaveTeam();
			}
		}
	}

	void Editor::DrawTeamCreate() {
		if (!ShowTeamCreate) {
			return;
		}
		if (!ImGui::Begin("Team Create", &ShowTeamCreate)) {
			ImGui::End();
			return;
		}

		if (!Team->Watching()) {
			ImGui::TextUnformatted("Not looking for anybody yet.");
			ImGui::Spacing();
			if (ImGui::Button("Look for editors")) {
				WatchForTeam();
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
					// private session is a session of one - `SessionKey::Text`
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
		ImGui::Checkbox("Private session", &TeamPrivateField);
		ImGui::SameLine();
		ImGui::Checkbox("Show key", &TeamRevealKey);
		ImGui::InputTextWithHint(
			"Invitation key",
			"passphrase or 64 hex characters",
			TeamKeyField,
			sizeof(TeamKeyField),
			TeamRevealKey ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password
		);
		ImGui::TextDisabled("The key also unlocks private sessions you discover.");
		ImGui::InputTextWithHint(
			"Rendezvous", "host:port, for editors off this subnet", TeamPointField, sizeof(TeamPointField)
		);

		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::InputInt("Listen port", &TeamPortField, 0, 0)) {
			TeamPortField = std::clamp(TeamPortField, 0, 65535);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("0 chooses an available port automatically");
		}
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::InputInt("Editor limit", &TeamPeerLimitField, 1, 4)) {
			TeamPeerLimitField = std::clamp(TeamPeerLimitField, 0, 65535);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("0 allows any number of editors");
		}

		const bool joined = Team->Edits() != nullptr && !Team->Hosting();
		ImGui::BeginDisabled(joined);
		if (ImGui::Button(Team->Hosting() ? "Stop hosting" : "Host session")) {
			if (Team->Hosting()) {
				LeaveTeam();
			} else {
				HostTeam();
			}
		}
		ImGui::EndDisabled();
		if (!Team->Hosting() && (Team->Watching() || Team->Edits() != nullptr)) {
			ImGui::SameLine();
			if (ImGui::Button(joined ? "Leave session" : "Stop looking")) {
				LeaveTeam();
			}
		}

		if (!TeamStatus.empty()) {
			ImGui::TextDisabled("%s", TeamStatus.c_str());
		}

		ImGui::InputTextWithHint("Join address", "127.0.0.1:47600", TeamJoinField, sizeof(TeamJoinField));
		ImGui::Checkbox("Private address", &TeamJoinPrivateField);
		const std::optional<engine::net::Endpoint> manual = engine::net::Endpoint::Parse(TeamJoinField);
		ImGui::BeginDisabled(!manual.has_value() || Team->Hosting());
		if (ImGui::Button("Join address") && manual) {
			JoinTeam(*manual, TeamJoinPrivateField);
		}
		ImGui::EndDisabled();

		ImGui::Separator();

		const std::span<const network::Listing> peers = Team->Peers();
		if (peers.empty()) {
			ImGui::TextDisabled("No other editors seen.");
		} else if (ImGui::BeginTable(
					   "editors", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
				   )) {
			ImGui::TableSetupColumn("Editor");
			ImGui::TableSetupColumn("Project");
			ImGui::TableSetupColumn("Found");
			ImGui::TableSetupColumn("Editors");
			ImGui::TableSetupColumn("Action");
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
				if (row.Session.PeerLimit == 0) {
					ImGui::Text("%u", row.Session.Peers);
				} else {
					ImGui::Text("%u/%u", row.Session.Peers, row.Session.PeerLimit);
				}
				ImGui::TableNextColumn();
				const std::string rowId = row.Session.Session.Text();
				if (row.Joinable() && row.Dial().IsValid()) {
					ImGui::BeginDisabled(Team->Hosting());
					if (ImGui::SmallButton(("Join##" + rowId).c_str())) {
						JoinTeam(row.Dial(), row.Session.Admits == network::Access::Private);
					}
					ImGui::EndDisabled();
				} else if (row.Session.IsFull()) {
					ImGui::TextDisabled("full");
				} else if (row.Session.Admits == network::Access::Private) {
					if (TeamKeyField[0] == '\0') {
						ImGui::TextDisabled("key required");
					} else if (ImGui::SmallButton(("Unlock##" + rowId).c_str())) {
						WatchForTeam();
					}
				} else {
					ImGui::TextDisabled("no address");
				}
			}
			ImGui::EndTable();
		}

		ImGui::Separator();

		if (const EditStream *stream = Team->Edits()) {
			const EditCounters &edits = stream->Counters();

			// **Who is where, which is the half a lock is actually for.** The
			// refusal is the mechanism; seeing that somebody is working on a
			// model *before* you try is what stops the refusal happening.
			const std::span<const Lease> held = stream->Locks().Held();
			if (!held.empty() && ImGui::BeginTable("holds", 2, ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn("Editing");
				ImGui::TableSetupColumn("Who");
				ImGui::TableHeadersRow();

				for (const Lease &lease : held) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Describe(lease.Subject).c_str());
					ImGui::TableNextColumn();
					if (lease.Holder == stream->Self()) {
						ImGui::TextUnformatted("you");
					} else {
						ImGui::Text("editor %u", lease.Holder);
					}
				}
				ImGui::EndTable();
			}

			const std::span<const studio::Waiting> queue = stream->Locks().Queue();
			if (!queue.empty()) {
				ImGui::TextDisabled("%zu waiting behind them", queue.size());
			}

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
			if (stream->Backlog() > 0) {
				// **Waiting is not failing**, and a person watching a
				// colleague's screen not change wants to know which. An edit
				// held back has already happened here; what is waiting is the
				// message.
				ImGui::TextDisabled("%zu edit(s) waiting for a turn", stream->Backlog());
			}
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
			"never what somebody else did. Two people on one model take turns -\n"
			"whoever asked first goes first, and nobody's work is lost."
		);

		ImGui::End();
	}
}
