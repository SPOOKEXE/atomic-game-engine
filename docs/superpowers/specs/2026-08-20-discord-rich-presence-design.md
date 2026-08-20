# Discord rich presence

Roadmap v0.17: *discord rich presence under preferences to set it up as a tab
"Discord Presence"*.

What a person's Discord profile says while an Atomic program is running, and
the tab in Preferences that decides it. Four programs report: the studio says
what is being edited, the client what is being played, the server what it is
hosting, and the origin that it is serving.

## Decisions, and what they replaced

**We speak the IPC protocol ourselves rather than linking Discord's Social
SDK.** The SDK the roadmap links is a prebuilt closed-source shared library
downloaded from the developer portal after accepting terms. It is not on any
package index and cannot be a submodule, so it would be either a committed
binary blob or a manual download step that hard-breaks the build for anybody
who skipped it. Every other entry in `mono.vendor/` is a submodule. It also
carries friends, lobbies and voice, none of which is wanted. Underneath it is
a small local socket protocol that has been stable since 2017 and that every
third-party library speaks. Writing it is roughly six hundred lines, adds no
dependency, builds offline, and is testable without Discord installed, which a
binary SDK is not.

The archived `discord/discord-rpc` is Discord's own MIT implementation of the
same protocol and would build offline, but it was archived in 2021 and is
fifteen hundred lines wrapping what is written here in six hundred.

**The Application ID is supplied rather than baked.** Presence reports as a
Discord application, and that application's name is the bold first line a
person's friends see - so shipping one would put this project's name on
somebody else's game. The default is `-1`, which is not a snowflake and never
will be. That is a placeholder rather than an empty string so an unconfigured
install can say "no application id" instead of handshaking its way to Discord's
`Invalid Client ID`, and `IsConfigured` refuses it exactly as it refuses an
empty field.

An Application ID is not a credential and the **client secret is neither needed
nor accepted** anywhere here. The secret exists for OAuth2 authorization-code
exchange; this protocol never authenticates, and the whole of what identifies
the caller is `client_id` in the handshake.

**It is a top-level member, not an engine module.** `mono.network/CMakeLists.txt`
states the rule: a member is top-level rather than an engine module when
several programs import it and it is not a layer of the engine. That is exactly
this. `shared` tier is forced anyway, because `MONO_TIER_ALLOWS_server` lists
only `shared` and the server links it.

**Nothing here is called `Presence`.** `network::Presence` already exists and
means announcing on a subnet and registering at a rendezvous point. Two
`Presence` types in one studio translation unit is a trap. The connection here
is `discord::Link`.

**Off by default.** Rich presence publishes the name of the file somebody has
open to their whole friends list.

## Module layout

```
mono.discord/
  AGENTS.md
  CMakeLists.txt
  docs/
  include/discord/
    Activity.hpp      the payload value type and its limits
    Channel.hpp       the byte pipe interface, and the two ways to get one
    Frame.hpp         the [opcode][length][json] codec
    Link.hpp          the state machine
    Settings.hpp      what is configured, and the flag table
  src/
    Activity.cpp
    Channel.cpp       MemoryChannel, and the path search
    Frame.cpp
    Link.cpp
    Settings.cpp
    platform/posix/Socket.cpp
    platform/windows/Socket.cpp
  tests/
    Frame.cpp
    Link.cpp
    Templates.cpp
    Sockets.cpp       POSIX only
```

`mono_add_library(discord TIER shared NAMESPACE Mono DEPS Engine::core VENDOR
Vendor::json)`. `Vendor::json` is `VENDOR` rather than `VENDOR_PUBLIC`: no
public header here names a `nlohmann::json`, because `Activity` is a struct of
strings and the JSON exists only inside `Link.cpp`.

Not `Engine::net`. That module is UDP between two machines. This is a unix
domain socket to a desktop application on the same box, and the two share no
type.

`engine::parallel::SocketChannel` has the right shape and is deliberately not
reused. It builds socketpairs for process-per-world hosts and exposes no
"connect to this path", and widening its public header to add one would turn a
job-workers module into an IPC-endpoints module. The hundred lines of
non-blocking socket setup are written again rather than shared, and that is the
cheaper of the two debts.

## The protocol

Confirmed against `discord/discord-rpc`, not from memory.

**Frame.** `[uint32 opcode][uint32 length][length bytes of JSON]`,
little-endian, whole frame at most 64 KiB.

| Opcode | Meaning |
|---|---|
| 0 | Handshake |
| 1 | Frame |
| 2 | Close |
| 3 | Ping |
| 4 | Pong |

**Where the socket is.** `discord-ipc-0` through `discord-ipc-9`, tried in
order, first one that connects wins.

- POSIX: in the first directory named by `XDG_RUNTIME_DIR`, `TMPDIR`, `TMP` or
  `TEMP`, plus `/tmp` unconditionally last. Under each of those, seven install
  layouts: the plain one, `app/com.discordapp.Discord/`,
  `app/dev.vencord.Vesktop/`, `.flatpak/com.discordapp.Discord/xdg-run/`,
  `.flatpak/dev.vencord.Vesktop/xdg-run/`, `snap.discord-canary/` and
  `snap.discord/`. The six past the first are Flatpak, Snap and Vesktop, which
  is how a large share of Linux users actually run Discord; a search that omits
  them reports "Discord is not running" to somebody looking straight at it. The
  list is `vionya/discord-rich-presence`'s, which is the maintained
  implementation of this protocol. Socket number is the outer loop and layout
  the inner one, because Discord hands the lowest free number to whichever
  client started first.
- POSIX, under a Snap confinement: `XDG_RUNTIME_DIR` points at the snap's own
  directory rather than the one Discord's socket is in, so when `SNAP` is set
  that one root is walked up a level. Every other root is taken as it stands.
- Windows: `\\.\pipe\discord-ipc-N`.
- macOS uses the POSIX path, where `TMPDIR` is the per-user temporary
  directory.

**Handshake**, opcode 0:

```json
{"v": 1, "client_id": "<application id>"}
```

Discord answers with an opcode 1 frame whose `evt` is `READY`. Anything else,
including a `Close`, returns the link to backoff.

**Update**, opcode 1:

```json
{"cmd": "SET_ACTIVITY",
 "nonce": "<counter>",
 "args": {"pid": <process id>,
          "activity": {"details": "...",
                       "state": "...",
                       "timestamps": {"start": <unix seconds>},
                       "assets": {"large_image": "...", "large_text": "..."},
                       "party": {"id": "...", "size": [n, capacity]},
                       "secrets": {"join": "..."},
                       "buttons": [{"label": "...", "url": "..."}]}}}
```

**Field limits**, encoded as named constants and enforced by truncation:

| Field | Limit |
|---|---|
| `details`, `state` | 128 characters |
| `large_text` | 128 characters |
| `large_image` | 256 characters |
| button `label` | 32 characters |
| button `url` | 512 characters |
| buttons | 2 |

Truncation is UTF-8 safe. Cutting a multibyte character in half produces a
frame Discord rejects, and the failure is silent, so this is a test rather than
a comment.

**Rate.** Discord accepts roughly five updates per twenty seconds. `Link` sends
only when the filled activity differs from the last one sent *and* not more
often than `MINIMUM_UPDATE_SECONDS`. Both conditions, not either: the diff
alone lets a token that changes every frame, such as a part count, hammer the
socket.

## The failure model

**Discord not running is not an error.** Every connect failure logs at `DEBUG`.
A headless origin in a datacentre fails this on every retry for the life of the
process, and a `WARN` there is a log nobody can read.

**Backoff** starts at one second and doubles to sixty. It resets on `READY`,
not on connect, because a socket that accepts and then refuses the handshake is
a failure that should back off rather than spin.

**Nothing blocks.** The socket is non-blocking. A `Send` that would block drops
the frame and returns `Full`. That is safe here specifically because an
activity is level-triggered state rather than a stream of events, so the next
pump re-sends the current truth. A studio frame must never stall because
Discord stopped draining a pipe.

**Time is passed in.** `Link::Pump(double nowSeconds)`, and every backoff and
throttle deadline is an argument, following the rule `mono.network/AGENTS.md`
inherits from `engine::net`. Tests state timeouts instead of sleeping for them.

The single real clock this needs is `timestamps.start`, which must be a unix
epoch second rather than the monotonic seconds the pump takes. The caller fills
`Activity::StartedUnixSeconds`, so the module reads no clock at all.

## Public interfaces

```cpp
namespace discord {

    // One button on a presence card. Discord shows at most two, and shows
    // neither of them back to the person whose presence it is.
    struct Button {
        std::string Label;
        std::string Url;
    };

    // What Discord is asked to show. Already filled and already clamped:
    // templates are resolved before an Activity exists.
    struct Activity {
        std::string Details;
        std::string State;
        std::string LargeImage;
        std::string LargeText;

        // Zero for no timer. A unix epoch second, which is why it is supplied
        // rather than read.
        int64_t StartedUnixSeconds = 0;

        // Zero size means no party block.
        std::string PartyId;
        uint32_t PartySize = 0;
        uint32_t PartyCapacity = 0;

        // Empty unless join secrets are on. See "Joining".
        std::string JoinSecret;

        std::vector<Button> Buttons;

        bool operator==(const Activity &) const = default;
    };

    // Why a frame did not go out.
    enum class ChannelStatus : uint8_t { Ok, Empty, Full, Closed, TooLarge };

    // A byte pipe. Two implementations: the local socket, and the in-memory
    // one the suite drives.
    class Channel {
      public:
        virtual ~Channel() = default;
        virtual ChannelStatus Send(std::span<const std::byte> bytes) = 0;
        virtual ChannelStatus Receive(std::vector<std::byte> &into) = 0;
        virtual bool Open() const = 0;
        virtual void Close() = 0;
    };

    // Connects to the first Discord socket that answers.
    //
    // @param override An exact path to use instead of searching, for tests
    //                 and for an unusual install. Empty searches.
    // @return The channel, or nullptr when nothing answered.
    std::unique_ptr<Channel> ConnectLocal(std::string_view override = {});

    // What a program has to say about itself, as tokens a template names.
    using Facts = std::vector<std::pair<std::string, std::string>>;

    // Substitutes {token} and clamps to `limit` characters on a UTF-8
    // boundary. An unknown token resolves to nothing.
    std::string Fill(std::string_view pattern, const Facts &facts, size_t limit);

    // What was configured. Built from flags, or from the studio's
    // preferences, and the studio's tab edits one of these.
    struct Settings {
        bool Enabled = false;
        std::string ApplicationId;
        std::string Details;
        std::string State;
        std::string LargeImage;
        std::string LargeText;
        std::string ButtonLabel;
        std::string ButtonUrl;
        bool ShowElapsed = true;
        bool HideNames = false;
        bool JoinSecrets = false;
    };

    bool DeclareFlags();
    Settings SettingsFromFlags(const Settings &defaults);

    // Where a link has got to. Reported so the tab can say it.
    enum class LinkState : uint8_t { Off, Waiting, Handshaking, Ready };

    // The connection, the handshake, the throttle and the retry.
    class Link {
      public:
        explicit Link(Settings settings);
        ~Link();

        Link(const Link &) = delete;
        Link &operator=(const Link &) = delete;

        // Replaces what is reported. Cheap, and safe to call every frame:
        // nothing is sent until it differs and the throttle allows it.
        void SetActivity(const Activity &activity);

        // Connects, handshakes, retries, drains and sends what is due.
        void Pump(double nowSeconds);

        LinkState State() const;

        // What went wrong last, for the tab. Empty when nothing has.
        const std::string &Trouble() const;

        // Called with a join secret when one arrives. Only ever called with
        // `Settings::JoinSecrets` on.
        void OnJoin(std::function<void(std::string)> handler);
    };
}
```

## Configuration

`discord::DeclareFlags()` registers one table, so config-file and command-line
support arrives in every program for free, exactly as `cdn.*` and `client.*`
do.

| Flag | Default | Meaning |
|---|---|---|
| `discord.enabled` | `false` | Whether to report at all |
| `discord.app-id` | `-1` | The Discord Application ID. `-1` or empty is inert |
| `discord.details` | per program | The first line's template |
| `discord.state` | per program | The second line's template |
| `discord.large-image` | `atomic` | An asset key uploaded to your application |
| `discord.large-text` | per program | The tooltip on that image |
| `discord.button-label` | per program | Empty for no button |
| `discord.button-url` | per program | https only |
| `discord.show-elapsed` | `true` | The "01:23 elapsed" timer |
| `discord.hide-names` | `false` | Substitute a generic word for place and world names |
| `discord.join-secrets` | `false` | See "Joining" |

`SettingsFromFlags` returns its `defaults` argument untouched when
`Flags::Has("discord.enabled")` is false, following
`engine::parallel::ApplyFlags`: a program that never declared the table does
not use it, and a dead flag reading `false` would be right for the wrong
reason.

**The studio does not declare this table.** It is the one program with a
Preferences page, and `studio/Config.hpp` argues that an editor persists what
somebody configured in a document it owns. A `--flag discord.enabled=false` that
the tab then contradicted would be worse than no switch, so the studio reads and
writes `preferences.json` only, and the client, the server and the origin read
the flags only.

**The Application ID is not a secret, and the module's `AGENTS.md` says so.**
It ships inside every Discord game's binary and is visible to anyone watching
the socket. It is a name. Without that note it reads as a credential sitting in
a preferences file, which `studio/Config.hpp` is explicit about not doing.

**The studio persists it instead.** `Preferences` gains a `Discord` member of
type `discord::Settings`, written to and read from `preferences.json` under a
`"discord"` object. The per-program wording is set on `Prefs.Discord` before
`Preferences::Load` runs, which works because `Load` leaves alone anything the
document does not mention.

## Templates and tokens

The program builds `Facts`; the module substitutes and clamps. Tokens per
program:

| Program | Tokens |
|---|---|
| studio | `place`, `world`, `instances`, `worlds`, `selection` |
| client | `game`, `world`, `server`, `worlds` |
| server | `game`, `players`, `capacity`, `worlds`, `port` |
| cdn | `groups`, `manifests`, `bytes`, `port` |

`hide-names` replaces `place`, `world` and `game` with `a project`, `a world`
and `a game` before substitution, so it is one rule in one place rather than a
branch at each call site. It deliberately does not cover the client's `server`
token: an address is only in a card because somebody typed it into a template,
and the template is the switch for it.

Defaults:

| Program | details | state | button |
|---|---|---|---|
| studio | `Editing {place}` | `{instances} instances in {world}` | none |
| client | `Playing {game}` | `In {world}` | none |
| server | `Hosting {game}` | `{players} of {capacity} players` | `Join this server`, no link |
| cdn | `Hosting an Atomic origin` | `{groups} groups served` | none |

The server's button ships with a label and no link, on purpose. Nothing in this
repository knows an address a stranger could click - that needs a port forward,
a domain and a joining flow the engine has not got - so setting
`discord.button-url` alone produces a working button, and setting neither
produces no button rather than a broken one.

The server is also the one program that fills the `party` block, because it is
the one that has a party. Discord draws "3 of 8" from it, which is the fact
somebody deciding whether to join actually wants.

`large-text` defaults to `Atomic Game Engine` everywhere. The small image and
its tooltip are not modelled: the protocol carries them, nothing here would set
them, and a field no caller fills is a field that goes stale.

An empty template means the line is omitted rather than sent empty.

## The tab

`Editor::DrawDiscordSettings()` in `mono.studio/src/Settings.cpp`, added to the
tab bar at `DrawSettings` as `"Discord Presence"`, after `Compute` and before
`Keybinds`.

Contents, in order:

1. **A state line.** `off`, `waiting for Discord`, `connecting` or
   `reporting as <application name>`, plus `Trouble()` when it is not empty.
   This is the first thing somebody looks at when it is not working.
2. **Enabled**, and the **Application ID** field, with a line under it saying
   where to get one and that it is not a secret. Everything below is disabled
   while the id is empty.
3. **The two templates**, each with the token list for the studio shown beside
   it as dimmed text.
4. **A live preview** drawn as Discord lays it out: image, bold application
   name, details, state, elapsed. This is most of the value of the tab, because
   Discord does not render a person's own buttons or card back to them, so
   without a preview the only way to see the result is to ask a friend.
5. **Show elapsed** and **Hide names**, the latter with a line saying what it
   is for.
6. **Button label and URL**, with the note that Discord shows buttons to other
   people and not to you.

Everything writes `Preferences` and saves on change, as the other pages do.

## Wiring

| Program | Owner | Pumped at |
|---|---|---|
| studio | `Editor` | beside `PumpContent` in `Editor.cpp`'s frame loop |
| client | `Client` | beside `PumpSounds` in `Client.cpp` |
| server | `Server` | beside `Discovery->Pump` in `Server::Run` |
| cdn | `main` | in the serve loop beside `serving->Pump(NowSeconds())` |

Each owner holds a `std::unique_ptr<discord::Link>`, null when unconfigured,
and a small private function that builds this program's `Facts`. Facts are
rebuilt every pump; they are a handful of short strings and the throttle means
nothing is sent unless they changed.

`mono.tools/architecture/expected_graph.json` gains a `discord` module row
(`tier: shared`, `links: ["core"]`) and the four program rows gain `discord`.
The server row stays headless and the cdn row still configures without a Vulkan
SDK, because the module links neither.

## Joining

Built, tested and switched off, per `discord.join-secrets`.

With it on, `Link` sends `SUBSCRIBE` for `ACTIVITY_JOIN` after `READY`,
includes `secrets.join` in the activity, and hands a received secret to the
`OnJoin` handler.

**With it off, `Link` strips any `Activity::JoinSecret` it was given.** The
gate is in the module rather than in the four callers, and a test covers it. The
first draft did not have this and the failure it allows is invisible to whoever
hits it: a caller fills a secret while the feature is off, Discord draws a
native join button, a friend clicks it and nothing happens, and no log line
anywhere mentions any of it. The `party` block is not gated, because "2 of 8" is
worth showing whether or not anybody can act on it.

It stays off because the secret arrives as a string the *joining* program has
to act on, and Discord delivers it by launching the game through a registered
URI scheme. An `atomic://` handler registered with the OS on three platforms is
its own piece of work and belongs on its own roadmap line. Until then a session
is joined through the URL button, which needs nothing.

## Testing

`mono.discord/tests/`, all of it running on CI with no Discord installed and no
account.

**`Frame.cpp`.** Round-trip. A declared length past 64 KiB is refused rather
than allocated. A truncated header is held, not consumed. A frame split across
two reads reassembles. That last one is where hand-rolled protocol code usually
breaks, because a local socket almost always delivers small frames whole and
then one day does not.

**`Link.cpp`**, against `MemoryChannel`. The handshake bytes are exactly right,
opcode and length included. `SET_ACTIVITY` goes out on `READY`. An identical
activity is not sent again. A changed one is not sent before the throttle
interval and is sent after it. A `Close` mid-session returns to backoff. Backoff
grows, caps at sixty, and resets on `READY` rather than on connect. With join
secrets off, no `SUBSCRIBE` is sent and no `secrets` key appears; with them on,
both do, and a delivered `ACTIVITY_JOIN` reaches the handler.

**`Templates.cpp`.** An unknown token resolves to nothing. A fact with no token
is ignored. Truncation lands on a UTF-8 boundary. An empty template omits the
line. `HideNames` substitutes before rather than after.

**`Flags.cpp`.** A program that never declared the table keeps its own
settings; a declared but unset table leaves every default alone; what somebody
typed wins per-row while untouched rows keep the program's wording; `false` over
a `true` default takes, which a "did somebody set this" implemented as "is the
value non-empty" would silently lose; and nothing is configured without both the
switch and a real id.

**`Sockets.cpp`**, POSIX only. All seven install layouts appear in the search
and `discord-ipc-10` does not; a lower socket number under any layout is tried
before a higher one under the plainest; a real listening socket is found at
`discord-ipc-3` when 0 through 2 are absent; an absent socket is a null return
rather than a throw; and a freshly connected channel answers `Empty` rather than
blocking, which is the property the whole no-thread design rests on.

**What was verified outside the suite.** A Python fake speaking the server half
over a real unix socket, against the real `server` and `studio` binaries. Both
connected, handshook, and sent one correct `SET_ACTIVITY`; the server's carried
its button and the studio's read its templates out of `preferences.json`. Ten
seconds of running produced exactly one update each, which is the throttle and
the diff both working.

What that still cannot prove is that *Discord* accepts the JSON. That needs the
Application ID and a running client, and is the one manual step. Until the id
arrives the default is a named empty constant and the feature is inert, which
is the correct behaviour for an unconfigured install regardless.

## Out of scope

- Friends, lobbies, voice and everything else the Social SDK carries.
- OAuth and account linking.
- The `atomic://` URI handler, and therefore a working Ask-to-Join.
- Presence for `mono.unified_server_client`, which is a test harness.
