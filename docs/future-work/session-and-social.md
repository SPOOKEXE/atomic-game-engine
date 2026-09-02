# Session and social system plan

## Status

This document is a future implementation plan. It describes systems that do not
yet exist as a trusted hosted service. Names below are proposed contracts, not
claims about current deployable infrastructure.

The first shipped version should be deliberately small: one control-plane
deployable with clear internal modules, one authoritative game server data
plane, and reusable engine libraries underneath both. Separate deployables are
earned later by measured load or a distinct failure boundary. Starting with a
fleet of tiny services would add network failure to boundaries that have not yet
settled.

## Goal

Provide a complete path from finding or requesting a game to joining it,
remaining connected, moving between places, communicating safely, and leaving
cleanly. The same contracts must support:

- direct local and private hosting with no central account provider;
- hosted public games with authenticated identities;
- public, private, and reserved server instances;
- lobbies, party joins, matchmaking, regional placement, and backfill;
- reconnects and cross-server teleports;
- world, team, party, direct, and system text channels;
- block, mute, report, and moderation enforcement;
- headless dedicated servers, clients, and deterministic Studio tests.

The engine remains useful without the hosted control plane. Direct address,
LAN discovery, rendezvous discovery, private session keys, and local Studio play
continue to work. Hosted features fail closed where identity or moderation is
required and fail with an explicit status rather than quietly becoming unsafe.

## Current foundation

The repository already contains useful lower-level pieces. They should be
extended at their existing seams rather than replaced.

### Discovery and reachability

`mono.network` provides:

- `Advert`, `SessionId`, `Directory`, and `Listing` as the common session
  discovery format;
- LAN beacons and bounded directory storage;
- `Presence` as the composition that owns discovery sockets;
- `RendezvousClient` and `RendezvousPoint` for NAT introduction;
- public and private session adverts, with private adverts authenticated by a
  `SessionKey`;
- protocol and transport hints for filtering incompatible listings.

The rendezvous point deliberately has no accounts, rankings, matchmaking, or
relay. It keeps a bounded, expiring mapping from session identity to endpoint
and advert. That remains a useful low-trust reachability service. It must not
quietly become the trusted session registry.

There is no relay fallback today. Symmetric NAT combinations can fail honestly.
A future relay is a separately metered data-plane product because it carries
game traffic and bandwidth cost. It is not hidden inside matchmaking.

### Transport, admission, and replication

`mono.engine/replication` already provides encrypted datagram and QUIC sessions,
stateless address cookies, server identity, optional client key proof, bounded
listeners, reliable messages, and authoritative replication. A listener can
apply an `AdmissionPolicy`, but the current opening exchange does not carry a
hosted account or matchmaking ticket.

Transport admission and game admission are already different limits:

- `replication::ListenerSettings::MaximumClients` caps connected transports;
- `Players.MaxPlayers` caps occupants admitted into a world.

That distinction remains. A future join reservation consumes neither limit
until the server accepts its encrypted join request. A reconnect grace slot is
accounted separately and may reserve one game occupant without holding an idle
transport.

### Players, teams, and characters

The scene layer has `Players`, `Player`, `PlayerIdentity`, `LocalPlayer`, teams,
spawn selection, starter containers, player signals, and character spawning.
Current identity is a stable numeric `UserId` plus display data supplied by the
host. It is not proof of a hosted account.

The character plan isolates characters from players. This plan therefore treats
possession, client ownership, player rosters, spawn policy, and respawn as
`Players` concerns. Session admission creates or resumes a `Player`; character
creation remains a later player-service decision. Bots and artificial
characters do not enter the session or social model unless a game deliberately
represents a bot as an occupant.

`Teams` is a gameplay service. Matchmaking may assign a team name or seed, but
the destination game decides how that maps to its `Team` instances. The control
plane never edits a world ECS store.

### Cross-world services and teleports

The world bus currently provides `MessagingService`, `CrossWorldService`,
`MemoryStoreService`, `DataStoreService`, and `TeleportService`. Current
teleporting addresses another world in the same universe process, copies a
bounded payload, removes the source player, and rebuilds the player at the
destination. `RegisterTeleportAdmission` allows a destination with no script
runtime to admit arrivals.

That is a good deterministic in-process path. It is not yet a cross-process
handoff protocol. A hosted teleport cannot destroy the source player before a
destination has reserved the arrival and issued a usable join ticket.

The current stores are gameplay services. They are not a session ledger,
account database, chat archive, or moderation audit log. Control-plane state
must use its own persistence boundary.

### Server and Studio

The dedicated server can host and announce a game session, enforce connection
caps, track player occupancy, publish Discord activity, and shut discovery down
in a defined order. A listening server owns one replicated world and UDP
endpoint. The server policy explicitly states that orchestration, matchmaking,
sharding, ledger, and persistence do not exist today.

Studio already has local play links, multi-world teleport tests, Team Create,
and network condition controls. Team Create is collaborative editing, not a
game lobby or hosted social service. Its session use can share low-level
networking but must not share gameplay admission state.

Discord integration is optional rich presence over a local Discord connection.
It can display party size and can receive join secrets when configured. Discord
is not an identity provider, account database, friend graph, moderation source,
or authority for joining a game.

## Non-negotiable boundaries

1. Engine libraries contain data models, codecs, deterministic state machines,
   validation, and client or server adapters. They do not open a control-plane
   database or assume a particular cloud provider.
2. The hosted control plane is a deployable product. It owns leases, tickets,
   queues, parties, social state, moderation state, and audit records.
3. The game server remains authoritative for its world. The control plane may
   authorize an occupant, but it does not simulate gameplay or mutate ECS rows.
4. The client never talks directly to a database. It uses authenticated service
   requests or the game server connection.
5. No pointer, ECS entity number, process-local `core::Name::Id`, or declaration
   ordinal crosses a world or service boundary.
6. A name that persists or crosses the wire is serialized as text. Existing
   wire-defined `SessionId` remains its specified 128-bit value and uses its
   canonical text form in service APIs and logs.
7. Identity, authorization, transport encryption, and session discovery are
   separate claims. Success in one does not imply success in another.
8. Every externally triggered table, payload, queue, subscription, and retry is
   bounded.
9. Every state-changing service request has an idempotency key. A timeout means
   the outcome is unknown until queried; it never means the caller should make
   a second unrelated operation.
10. Hosted social features may be absent. Their absence has an explicit status
    and never silently grants access.

## Product split

### Reusable engine libraries

Add a shared session library at the architecture tier approved by the target
graph. Its responsibilities are:

- strongly typed session, reservation, queue, party, join, reconnect, teleport,
  presence, and chat value objects;
- bounded codecs and protocol version negotiation;
- pure lifecycle and transition validation;
- ticket signature verification and key rotation metadata;
- retry and backoff helpers driven by caller-provided monotonic time;
- interfaces for a control-plane client, ticket verifier, and local test fake;
- script service descriptions shared by Luau and JavaScript bindings;
- metrics and structured status enums.

The shared library has no SQL client, HTTP server, cloud scheduler, UI toolkit,
Discord dependency, or world pointer. Existing `mono.network`,
`mono.engine/net`, and `mono.engine/replication` keep transport ownership.

Player-specific session application belongs beside the existing `Players`
service. It converts a validated join or reconnect into player lifecycle work.
The lower session library never creates an ECS player itself.

### Hosted control plane

Start with one deployable that contains these internal modules:

- session registry and server lease manager;
- placement and capacity manager;
- lobby, queue, and match coordinator;
- party and invitation manager;
- presence and social graph adapter;
- text routing and moderation coordinator;
- ticket issuer and key publisher;
- persistence and audit adapters;
- operator endpoints and metrics.

One deployable keeps atomic operations local while the data model is young. The
modules use explicit repository interfaces so measured load can split a module
later without changing game or script APIs. No engine module calls an internal
control-plane database adapter.

The control plane is not the packet relay. A future relay has its own regional
data-plane deployment and accepts short-lived relay grants from the control
plane.

### Game server data plane

The dedicated server:

- registers a leased instance and refreshes its capacity;
- verifies signed join tickets locally;
- admits, suspends, resumes, and removes players;
- reports occupancy and lifecycle changes;
- prepares and commits teleports;
- routes allowed chat operations through the moderation boundary;
- applies enforcement updates relevant to its occupants;
- continues simulation through short control-plane outages.

Admission must not perform a blocking database or control-plane request for
every connection. Tickets are signed and verified against a cached issuer key
set. Revocations and bans use bounded, refreshed snapshots plus pushed updates.
For high-risk actions, the server may require an online check with an explicit
timeout and refusal reason.

### Client and UI adapters

The client owns presentation and input. It receives immutable snapshots and
events from service adapters, then renders server browser, lobby, party, queue,
join, reconnect, chat, mute, report, and moderation views. The engine library
must not depend on the UI toolkit.

### Studio adapter

Studio uses an in-process deterministic fake of the control-plane interface for
default tests. It can optionally connect to a real development deployment when
the user asks. Local tests must not need public accounts or internet access.

## Identity and permissions

### Identity kinds

Support three explicit identity kinds:

- `HostedAccount`: an account proven by a trusted issuer;
- `LocalProfile`: a host-created identity for LAN, direct, or offline play;
- `Guest`: a short-lived identity scoped to one host or test run.

Each identity has a stable textual subject, display name, issuer name, and kind.
The numeric `Player.UserId` compatibility field is derived at the Players
adapter. It must not be used as cryptographic proof. Hosted code should key
persistent social records by issuer plus textual subject.

A client signing key proves possession of a key. It does not, by itself, prove
an account. Hosted account tokens bind the account subject to the client key or
to the authenticated service session that requested the join ticket.

### Roles and capabilities

Use named capabilities rather than numeric role ordinals. Initial capabilities
include:

- join public sessions;
- join a named reserved session;
- create and manage a party;
- invite members;
- send world, team, party, or direct text;
- mute, kick, or ban within a game scope;
- view reports and audit records;
- drain or close a server instance.

Roles are bundles assigned by policy. A game owner can define named roles, but
cannot grant a capability the hosted operator has withheld. Server scripts get
server-side capability checks through service bindings; clients only request an
action and receive a result.

### Join tickets

A join ticket is a signed, short-lived authorization. It contains bounded
fields:

- ticket id and idempotency key;
- issuer and signing key id;
- account or local identity subject;
- target universe, place, and session;
- party and match names when present;
- requested role and granted join capabilities;
- protocol and content build identity;
- issued, not-before, and expiry timestamps;
- one random nonce;
- reconnect or teleport transfer name when applicable.

The ticket contains no password, raw session key, database key, social graph,
or arbitrary script table. Sensitive transport reach data is returned beside
the ticket over the authenticated service connection.

Servers remember a bounded replay set until ticket expiry. A ticket accepted
for a new occupant cannot be accepted again. A reconnect ticket names the
existing suspended occupant and advances its connection generation.

### Admission sequence

Keep transport admission small. After encrypted transport setup and before any
world snapshot is published:

1. The client sends a bounded `JoinRequest` carrying the ticket, client build,
   requested transport features, and optional reconnect proof.
2. The server checks size, protocol, ticket signature, expiry, target, issuer,
   replay state, ban snapshot, capacity reservation, and client key binding.
3. The server atomically consumes the reservation and creates or resumes one
   player occupant.
4. The server replies with `JoinAccepted` or a structured `JoinRefused`.
5. Replication begins only after acceptance.

Refusal codes are stable names such as `SessionFull`, `SessionDraining`,
`TicketExpired`, `TicketReplayed`, `WrongBuild`, `Banned`, `IdentityRequired`,
and `ServerUnavailable`. User-facing text is localized by the client and is not
sent as the protocol decision.

Direct and offline sessions can issue local tickets from the host. This keeps
one post-handshake path without pretending the host is a central account
authority.

## Session records and lifecycle

### Session record

The control-plane registry owns a `SessionRecord` with:

- canonical session id;
- universe and place names;
- build, protocol, and content manifest identity;
- lifecycle state and monotonically increasing lease epoch;
- public, private, or reserved access class;
- region and placement pool;
- server identity key fingerprint;
- direct, rendezvous, and optional relay reach candidates;
- player capacity, occupied count, reserved count, and reconnect grace count;
- party, mode, and game-defined bounded labels used for matching;
- creation, ready, drain, lease expiry, and close timestamps;
- replacement session id when a handoff is underway.

The advert remains a small untrusted discovery hint. The registry record is a
trusted control-plane claim. Clients still authenticate the connected server
against the identity in the resolved join result.

### Lifecycle states

Use forward-only states:

`Starting -> Ready -> Draining -> Closed`

`Starting` and `Ready` may also move to `Failed`. A closed or failed session
never becomes ready again. Restarting a process creates a new `SessionId`, even
if a reserved server code continues to name the same logical reservation.

- `Starting`: placement exists but admission is not ready.
- `Ready`: heartbeats are current and new joins may be issued.
- `Draining`: no new ordinary joins; reconnect, party completion, and explicit
  handoff policy decide the limited exceptions.
- `Closed`: shutdown completed or the lease expired.
- `Failed`: startup failed or the host reported an unrecoverable fault.

Every server registration obtains a lease epoch. Heartbeats and lifecycle
updates carrying an old epoch are ignored and counted. This prevents a delayed
old process from making its replaced session look alive.

### Heartbeats and lease expiry

Heartbeats report occupancy, reserved capacity, reconnect grace, tick health,
build identity, and reach candidates. They are idempotent by session and epoch.
The registry expires a silent lease after a configured grace period and marks
the session closed. Rendezvous expiry remains separate and may happen earlier.

The server does not stop simulation merely because a heartbeat failed. It
enters degraded control-plane status, keeps admitted players, stops accepting
new hosted tickets after its bounded offline validation window, and retries with
jittered backoff.

### Public, private, and reserved instances

- A public instance may appear in the hosted browser and accept matchmaking.
- A private instance is unlisted and reached by a short-lived invitation or
  direct operator configuration.
- A reserved instance has a durable opaque access code naming a logical
  reservation. It may be replaced by a new physical session without changing
  the code.

Reserved access codes are high-entropy opaque text. The service stores a hash
or keyed digest, not the presented code. Codes are scoped by universe, carry
policy and expiry, and can be revoked or rotated. They are not the existing
`SessionKey`; one grants hosted join authorization and the other authenticates a
private discovery or rendezvous exchange.

## Placement, capacity, and regional choice

Placement consumes a request containing universe, place, build, access class,
region preferences, capacity, mode labels, and optional reservation name. It
returns an existing eligible session or asks the deployment adapter to start a
new server.

Region selection uses:

- server pool health and available capacity;
- client latency probes to known regional endpoints;
- party members' combined latency cost;
- operator region allowlists and data residency policy;
- measured queue wait and startup time.

Client-reported latency is a hint, not authority. The service bounds values and
may compare them with observed network paths. It does not infer precise physical
location when a coarse region decision is enough.

Capacity accounting distinguishes:

- connected transports;
- active player occupants;
- issued but unconsumed join reservations;
- reconnect grace reservations;
- party reservations;
- operator or moderator slots.

Reservations have short expiries and are consumed atomically. Backfill only
targets sessions that are ready, healthy, build-compatible, explicitly open to
backfill, and far enough from draining or round completion.

## Lobbies and matchmaking

### Lobby

A lobby is a pre-match coordination record, not a simulated world. It holds a
bounded member list, leader, universe, mode, build range, party references,
readiness, selected options, and lifecycle state. Games that need a fully
interactive lobby can use an ordinary game session instead.

Lobby mutations require an expected revision. A stale writer receives
`Conflict` plus the current revision. Ready checks are explicit and reset only
when a configured field changes.

### Queue ticket

A queue request creates one `MatchTicket` for either one player or one whole
party. The ticket includes:

- stable ticket name and idempotency key;
- universe, place, mode, build, and ruleset names;
- ordered regional preferences;
- party members and required party atomicity;
- optional skill band supplied by a trusted game adapter;
- team, role, and accessibility constraints;
- join-in-progress and backfill preferences;
- creation time, expansion step, and deadline.

The queue is partitioned first by hard compatibility fields. Soft constraints
expand on a documented schedule. Expansion is deterministic for equal inputs,
with stable textual ticket names as the final tie-break. Wall clock decides
eligibility windows, but it never changes an already formed match.

The initial matcher should use a bounded sorted scan, not a universal rules
language. Add game-specific matcher adapters only after two real games require
different logic.

### Match result

A match result names its members, target session or placement request, team
assignments, reservation expiry, and join ticket references. Formation and
capacity reservation commit together. If placement fails, tickets return to
the queue with their original age unless their deadline expired or a member
cancelled.

A player or party may hold at most one active queue ticket in a queue namespace.
Cancellation is idempotent. A late match result for a cancelled ticket is
discarded and releases its reservation.

### Backfill

The game server publishes a bounded `BackfillRequest` with open slots, team and
role needs, mode, round phase, and latest acceptable join time. The matcher can
fill all or part of it. The server revalidates round phase when tickets arrive;
a stale backfill never forces admission.

## Parties and invitations

A party is a social grouping that can enter lobbies, queues, and sessions
together. It is not a gameplay `Team` and does not imply chat permission outside
the party channel.

`PartyRecord` contains:

- party name and revision;
- leader identity;
- bounded member list with join order and role;
- privacy and invitation policy;
- current lobby, queue, match, and session names;
- creation and idle expiry time.

Party operations include create, invite, accept, decline, revoke, leave, kick,
transfer leader, set readiness, enqueue, cancel queue, and disband. Every
operation checks the expected party revision and an idempotency key.

Invitation tokens are short-lived, single-use, scoped to one recipient when the
recipient is known, and contain no raw party membership list. A shareable link
uses a separately rate-limited invite grant with maximum uses and expiry.

If a leader disconnects, leadership follows an explicit stable order after a
grace period. A queued party locks membership changes until the queue operation
is cancelled or completes. A game can opt into partial-party backfill, but the
default is atomic party placement.

## Joining, reconnecting, and handoff

### Join flow

The hosted client flow is:

1. Authenticate or establish an explicit local or guest identity.
2. Resolve a direct listing, invitation, reserved code, party result, or match
   result into a session and short-lived join ticket.
3. Select direct, rendezvous, or relay reach without changing session identity.
4. Connect and verify the server identity.
5. submit the encrypted `JoinRequest`.
6. Receive acceptance before the world snapshot.
7. Create `LocalPlayer`, install player-private data, and start replication.

The client exposes stage and progress events. Cancellation closes the current
attempt and releases any owned reservation. It never leaves a background join
attempt capable of replacing a later successful connection.

### Reconnect

On an ungraceful disconnect, the server may suspend the player for a bounded
grace period. Suspension:

- stops accepting client input immediately;
- applies the game's configured character policy, such as freeze, AI handoff,
  or normal simulation;
- retains the player occupant and private state if allowed;
- reserves one player slot;
- issues no social presence claim that the player is actively connected.

The client reconnects over a fresh transport. A short-lived reconnect token is
bound to identity, session, occupant name, client key, and connection
generation. Acceptance increments the generation, invalidates the old token,
and sends either a retained-state delta or a full authoritative snapshot.

If the original server is gone, reconnect becomes a replacement join. Only
state deliberately checkpointed by the game or transfer protocol can follow.
The system must not imply that arbitrary live ECS state survived a process
failure.

### Planned disconnect and drain

A planned client leave sends a best-effort leave reason, closes the session, and
does not reserve reconnect grace unless the game asks. A draining server stops
new matchmaking, publishes a deadline, and chooses one of:

- finish the current round and close;
- teleport occupants to a replacement;
- allow clients to return to a lobby;
- close with a structured maintenance reason.

Shutdown order keeps the existing rule: stop new work, withdraw discovery,
flush bounded lifecycle and audit records, close replication, close transports,
then stop jobs. A control-plane flush has a deadline and cannot hang shutdown.

## Teleport integration

### In-process path

Keep the existing deterministic same-universe bus teleport for local and Studio
worlds. It uses names and copied payloads, and the destination admits at the
barrier. Its script behavior remains compatible while the cross-process path is
added.

### Hosted cross-process path

Add an asynchronous prepare and commit protocol:

1. The source asks the control plane to prepare a transfer using a unique
   transfer name, target place or reserved code, player identity, build rules,
   and bounded teleport data.
2. Placement reserves capacity and returns a destination join ticket.
3. The source marks the player as transferring and stops new gameplay commands.
4. The client connects to the destination and presents the transfer ticket.
5. The destination consumes the ticket, creates the player, stores the teleport
   data, and acknowledges the transfer.
6. The source removes its player only after the destination commit, or after an
   explicit policy handles timeout and cancellation.

Every step is idempotent by transfer name. The destination stores a bounded
consumption record long enough to refuse replay. A client cannot alter the
target or payload after the source authority prepares it.

For compatibility, `TeleportService:Teleport` may start this operation and
raise only for immediate validation failure. Add `TeleportAsync` for a result
object or tuple with explicit statuses, cancellation, and progress. Add
reserved-server methods only after reserved access records exist.

Teleport data remains small, versioned, and serializable by the existing script
value codec. Large or durable state belongs in `DataStoreService` and is loaded
by stable identity at the destination. The transfer record is not a general
object store.

### Failure policy

- Failure before destination reservation leaves the source player active.
- Failure after reservation but before client departure releases the
  reservation and restores source input.
- Client disconnect during transfer preserves a bounded retry window.
- Destination commit wins over a late source cancellation.
- A transfer whose final state cannot be learned is queried by transfer name;
  it is never blindly repeated under a new name.

## Presence and friends seam

Presence is a privacy-filtered social projection, not transport discovery.
Initial states are `Offline`, `Online`, `InLobby`, `InGame`, and `InStudio`.
Details such as universe, place, party, and joinability are optional fields
filtered by relationship, user privacy, game policy, and moderation state.

The engine defines a `SocialGraphPort` with bounded methods for:

- querying relationship and block state;
- listing a page of friends or recent party members;
- subscribing to presence changes for an explicit bounded subject set;
- resolving whether a friend can be invited or joined.

No concrete friends provider is assumed. A hosted deployment can provide one;
offline mode returns `Unsupported`. Presence subscriptions are not global and
cannot be used to enumerate every account.

Discord activity consumes the local presence projection after privacy
filtering. A Discord join secret is an opaque, short-lived invitation resolved
by the session control plane. Discord user identity is never mapped to engine
account identity merely because Discord delivered the event.

## Text chat

### Channel model

Add `TextChatService` with named channel instances or handles. Initial channel
kinds are:

- world;
- team;
- party;
- direct conversation;
- system output.

Channel membership is authoritative and independently checked for every send.
Team membership comes from the game server's `Teams` adapter. Party membership
comes from the control plane. Direct conversations require policy and block
checks. System channels are write-only to trusted server or operator code.

A `TextMessage` contains a stable message name, channel name, sender identity
projection, server-assigned timestamp, normalized UTF-8 text, optional reply
reference, moderation outcome, and bounded game metadata. There is no arbitrary
HTML, script markup, executable rich text, or client-assigned sender.

### Send pipeline

1. The client or server submits text with a request id.
2. The nearest authority checks size, encoding, channel membership, capability,
   mute state, and rate limits.
3. Hosted chat applies block policy, spam controls, text filtering, and
   moderation rules.
4. The service assigns the message name and final display text.
5. Recipients receive the accepted message according to membership and block
   filters.
6. The sender receives an accepted, transformed, refused, or throttled result.

Games must never broadcast the raw client text while waiting for moderation.
If hosted moderation is required and unavailable, sending fails with
`ModerationUnavailable`. Local and private hosts may select an explicit local
policy and are shown as locally moderated.

### Text rules

- Validate UTF-8 before normalization.
- Cap bytes, Unicode scalar count, lines, mentions, links, and send rate.
- Normalize line endings and reject forbidden control characters.
- Preserve the original text only in restricted audit storage when policy and
  law permit it.
- Deliver filtered text appropriate to each recipient policy when required.
- Give every accepted message an immutable reference suitable for reporting.
- Treat edits as new moderated revisions and deletes as tombstones.

Voice chat, speech transcription, image messages, file transfer, and arbitrary
embedded media are not part of the first system.

## Block, mute, report, and moderation

### Block and mute

A block is an account-level relationship enforced by the hosted social and chat
services. It affects direct invitations, direct chat, join visibility, and
presence according to policy. It does not secretly alter gameplay collision or
game-specific combat rules.

A mute is a recipient preference. Local mute can immediately hide text without
a service round trip. Hosted mute synchronizes across devices. Server-enforced
channel mute is a moderation action and has scope, issuer, reason code, start,
and expiry.

### Reports

A report contains:

- reporter and subject identities;
- universe, place, session, match, channel, and message references as relevant;
- named reason category and optional bounded user note;
- client and server timestamps;
- immutable evidence references already held by trusted systems;
- request id and audit correlation name.

The client cannot upload an arbitrary server log and label it trusted evidence.
Reports are append-only submissions. Duplicate requests return the original
report name.

### Enforcement

Moderation actions use named scope and capability:

- warning;
- channel mute;
- session kick;
- universe ban;
- hosted-platform suspension;
- content or invitation restriction.

Every action names issuer, subject, reason code, scope, start, expiry, evidence
references, and appeal state. Game moderators cannot issue platform-wide
actions. Servers receive only the minimum active enforcement snapshot needed to
admit and operate their current occupants.

An appeal interface is part of the persistence model even if the first client
does not expose an appeal UI. Deleting moderation history on ordinary account
deletion follows legal retention policy rather than game script choice.

## Persistence, ledger, and audit

The control plane owns separate storage classes:

- durable identity references, party policy, blocks, moderation actions, and
  reserved access records;
- leased session, presence, queue, invitation, and capacity records with
  expiry;
- append-only audit events for privileged and security-relevant mutations;
- optional bounded chat history according to channel and retention policy.

Gameplay `DataStoreService` never doubles as one of these stores. `MemoryStore`
can back ephemeral hosted algorithms through an adapter, but scripts cannot
read control-plane queue or ticket records by guessing keys.

The ledger records state transitions that must be explainable, including ticket
issue and consumption, session lifecycle, capacity reservations, party leader
changes, matchmaking results, teleport commits, reports, and moderation
actions. It does not record every heartbeat or every presence refresh.

Every record carries schema version and stable text keys. Migrations run before
new writers are enabled. Readers tolerate the immediately previous schema
during rolling deployment. Destructive retention jobs use explicit cutoffs and
produce counts and audit records.

Secrets, raw access codes, account tokens, and private message bodies are never
written to ordinary logs. Audit access is capability-gated and itself audited.

## Script APIs

Bindings are generated once for Luau and JavaScript. Service surfaces are
capability-gated and identify which calls are server-only, client-readable, or
available offline.

### `Players`

Extend the player-facing service according to the separate character plan:

- active and inactive character roster;
- possession and swap requests;
- join, reconnect, suspension, and removal lifecycle signals;
- read-only hosted identity projection and session membership;
- server-only kick with a stable reason code.

Scripts never set authenticated identity fields or forge a session ticket.

### `SessionService`

Proposed server-facing surface:

- `GetSessionInfo()`;
- `SetJoinableAsync(joinable)`;
- `RequestBackfillAsync(options)`;
- `CancelBackfillAsync(requestName)`;
- `BeginDrainAsync(options)`;
- lifecycle and control-plane status signals.

Game scripts receive a bounded projection, not endpoints, signing keys, lease
epochs, or operator credentials.

### `MatchmakingService`

The ordinary client uses trusted UI remotes or a client-safe adapter. Server
scripts can create game-specific queue requests, cancel them, and read results.
Arbitrary client scripts cannot submit trusted skill values or choose another
player's identity.

### `PartyService`

Expose the local player's party projection, invitation events, and request
methods. Server scripts may inspect the party and match names delivered in a
validated join context. They cannot mutate membership without the service
policy.

### `TeleportService`

Preserve current methods and add:

- `TeleportAsync(placeName, players, options)`;
- `ReserveServerAsync(placeName, options)`;
- `TeleportToReservedServerAsync(accessCode, players, options)`;
- progress and result records with stable statuses;
- destination access to bounded teleport and match context.

Multi-player teleports reserve the group atomically by default. A game must opt
into partial transfer.

### `TextChatService`

Expose:

- channel discovery limited to channels the caller may see;
- message receive, update, and delete signals;
- `SendAsync`, `Mute`, `Unmute`, `BlockAsync`, `UnblockAsync`, and
  `ReportAsync` at their allowed host scope;
- server-only system messages and moderation requests;
- explicit status results for filtering, throttle, block, and outage.

The script API receives safe identity projections and filtered text. Raw account
tokens and unfiltered text never enter general game scripts.

## Client UI behavior

Provide toolkit-neutral view models for:

- server browser rows and reach status;
- lobby membership and readiness;
- party roster, invitations, and leader actions;
- queue state, elapsed time, cancellation, and match found;
- join stages, refusal reasons, and retry choices;
- reconnect countdown and destination handoff;
- chat tabs, unread counts, message status, mute, block, and report;
- privacy, parental, accessibility, and moderation notices.

UI actions emit commands carrying the snapshot revision they were based on.
Stale commands receive a refresh result. Optimistic presentation is allowed for
local mute and draft text, but membership, queue, ticket, and moderation state
is confirmed by authority before the UI presents success.

Do not use continuously repainting spinners. Progress changes on state or a
low-frequency timer. Dark mode follows project defaults: true black background,
white primary text, dense layout, and no decorative card chrome.

## Studio and local testing

Add a `LocalSessionControl` adapter with deterministic in-memory records. It
uses injected monotonic time and seeded identifiers so tests can advance lease,
queue, invite, reconnect, and moderation deadlines without sleeping.

Studio test controls should support:

- create public, private, and reserved local sessions;
- launch several clients and local dedicated servers;
- form parties and queue them;
- force capacity, backfill, drain, and replacement conditions;
- disconnect and reconnect selected clients;
- perform same-process and cross-process teleport simulations;
- inject latency, loss, duplicate replies, stale revisions, and service outage;
- choose local moderation outcomes without sending text externally;
- inspect lifecycle, reservations, tickets, and audit events.

The fake implements the same interface and state rules as the hosted client. It
does not fake transport encryption or replication when an integration test is
specifically about those layers.

Live hosted or multi-process Studio inspection is a final verification step.
Ask the user before running it. Headless deterministic tests remain the default.

## Failure, retry, and idempotency

### Request contract

Every mutation carries:

- operation name;
- caller identity and capability context;
- high-entropy request id;
- target record name;
- expected revision when editing existing state;
- deadline;
- trace correlation name.

The service stores a bounded deduplication result through the maximum retry
window. Repeating the request id returns the original outcome. Reusing it with a
different payload is refused and audited.

### Retry policy

Retry only operations documented as retryable. Use exponential backoff with
jitter and a maximum deadline. Honor server retry hints. Do not retry malformed,
unauthorized, expired, or conflict results without changing the relevant input.

All asynchronous client operations expose `Pending`, `Succeeded`, `Failed`, and
`Cancelled`. Cancellation is a request to stop; the caller still queries final
state when a commit may already have happened.

### Partial failure

- Registry outage stops new hosted joins after the offline ticket window but
  does not stop simulation.
- Matcher outage leaves queue tickets queryable and cancellable once service
  returns; clients do not create duplicates.
- Party outage blocks membership mutation but does not eject a party already in
  a game.
- Chat moderation outage follows the channel's fail-closed policy.
- Audit storage outage blocks privileged moderation mutations that cannot be
  recorded.
- Presence outage hides or marks presence stale. It does not imply offline and
  never removes a player from a game.

## Hostile input and abuse limits

Treat clients, discovered adverts, invitation links, and federated provider data
as hostile.

Enforce bounds at decode and again at policy boundaries:

- frame and field byte limits;
- list, page, party, lobby, queue, channel, and subscription limits;
- request rate and concurrent-operation limits by IP, identity, session, and
  device key where available;
- invite fan-out and acceptance limits;
- join ticket issue and failed admission limits;
- reconnect attempt and token rotation limits;
- chat byte, scalar, line, mention, link, and channel rates;
- report submission and evidence-reference limits;
- operator mutation and audit-read limits.

Use stateless or cheaply verifiable challenges before allocating per-stranger
state. Avoid reflecting larger replies to unauthenticated datagrams. Preserve
the current bounded directory and rendezvous behavior.

Admission never trusts a client-selected user id, party, team, match, role,
region, destination, or teleport payload. It cross-checks signed context and
server policy. A server validates every gameplay command against the currently
active connection generation and player possession.

Rate-limit keys are privacy-sensitive operational data with short retention.
They are not a shadow social graph. Security reviews cover ticket signing,
issuer key rotation, reservation replay, invitation leakage, moderation access,
and cross-tenant data separation before hosted launch.

## Observability and operations

### Correlation

Carry bounded correlation names through:

- queue ticket;
- match;
- placement request;
- session and lease epoch;
- party;
- join ticket and admission;
- reconnect generation;
- teleport transfer;
- chat message;
- report and moderation action.

Logs use stable status names and never dump secret-bearing objects. Dynamic log
levels can be scoped to session, account subject hash, or request name.

### Metrics

Record counters, gauges, and histograms for:

- session starts, readiness time, heartbeat age, drains, lease expiry, and
  failure reason;
- capacity by state, reservation age, stale occupancy, and backfill fill rate;
- queue depth, wait time, expansion step, cancellation, and match quality
  buckets;
- party mutation conflicts and invitation outcomes;
- join stage latency, refusal status, ticket replay, and connection route;
- reconnect success by elapsed disconnect time;
- teleport prepare, connect, commit, rollback, and orphan cleanup;
- chat moderation latency, transformed, refused, throttled, and delivery fanout;
- block, mute, report, and enforcement counts without exposing content;
- control-plane request latency, retry count, saturation, and dependency fault.

Metrics are read for diagnosis and capacity planning, never to steer simulation
within a tick. High-cardinality ids stay in traces and sampled logs, not metric
labels.

### Health and operator controls

Provide authenticated operator actions to inspect, drain, close, replace, and
quarantine sessions; inspect stuck reservations and transfers; disable a queue
or region; rotate ticket keys; and apply or revoke moderation action. Every
mutation requires a reason and creates an audit record.

## Save, wire, and replication policy

| Data | Durable | Control-plane wire | Game replication |
|---|---:|---:|---:|
| Session lease | Until expiry and audit retention | Yes | Read-only projection |
| Join reservation | Short-lived | Yes | No |
| Join ticket | Short-lived, deduplicated | Yes | No |
| Player hosted identity projection | Account lifetime | Yes | Scoped fields only |
| Party | Policy-dependent durable record | Yes | Join context only |
| Lobby and queue ticket | Short-lived | Yes | No |
| Match assignment | Audit retention | Yes | Join context only |
| Presence | Expiring | Yes | No |
| Reconnect token | Short-lived | Yes | No |
| Teleport transfer | Until settled plus audit retention | Yes | Destination context |
| Text message | Channel retention policy | Yes | Filtered recipients only |
| Block and moderation action | Durable policy record | Yes | Minimum enforcement view |
| Local mute | Local preference | Optional sync | No |

All service codecs are versioned and bounded. Game replication carries only the
fields needed by occupants of that world. Private player containers and social
state continue to use owner-scoped replication.

## Migration from current behavior

1. Preserve direct address, LAN, rendezvous, current private session keys, and
   local Studio joins as compatibility paths.
2. Introduce session value objects and statuses without changing current
   transport or player admission.
3. Add encrypted post-handshake `JoinRequest` with a local host issuer. Move
   world publication behind successful game admission.
4. Route existing dedicated server discovery through a session lifecycle
   adapter while keeping `Advert` as the discovery format.
5. Add the hosted registry and signed ticket issuer. Hosted mode opts in;
   offline mode keeps local tickets.
6. Move dedicated server occupant admission through the Players session adapter.
   Delete any second hosted admission path after parity tests pass.
7. Add reconnect suspension and connection generations.
8. Extend teleporting with prepare and commit. Keep same-process bus teleport as
   the fast local adapter.
9. Add parties, queues, placement, and backfill on the same reservation model.
10. Add text channels and moderation. Do not repurpose the server's existing
    diagnostic chatter resource as user chat.
11. Add privacy-filtered social presence and optional provider adapters.
12. Point Discord join secrets at invitation resolution without treating
    Discord identity as account proof.

Every migration step leaves one production path for each mode. Temporary
adapters are removed once all callers and tests use the new contract.

## Implementation phases and gates

### Phase 0: contracts and architecture gate

- Inventory all current join, player admission, disconnect, teleport,
  discovery, Studio play, and Discord join call paths.
- Decide module tiers and update the expected target graph with any new module.
- Define stable statuses, limits, versioning, and privacy classifications.
- Add pure lifecycle and codec tests before adding network clients.

Gate: architecture checks pass, codecs reject over-limit data, and no new
module sees an upward layer.

### Phase 1: local tickets and game admission

- Add `JoinRequest`, `JoinAccepted`, and `JoinRefused` on the encrypted link.
- Add local ticket issue and verification.
- Delay replication publication until game admission succeeds.
- Connect admission to one Players adapter and structured refusal output.

Gate: direct, LAN, rendezvous, QUIC, datagram fallback, full server, wrong build,
bad ticket, replay, and anonymous local joins pass headless integration tests.

### Phase 2: session lifecycle and registry

- Add server registration, lease epochs, heartbeats, capacity, drain, and close.
- Add the first control-plane deployable and persistence adapters.
- Add hosted ticket issue and offline verification key refresh.
- Add server browser resolution from registry records.

Gate: killed servers expire, stale epochs cannot revive them, ticket key rotation
works through rolling deployment, and direct hosting still works without the
service.

### Phase 3: reconnect and planned drain

- Add player suspension, reconnect tokens, connection generations, and resync.
- Add planned leave and server drain client views.
- Add replacement session links without claiming arbitrary state migration.

Gate: duplicate transports cannot drive one player, expired tokens release
capacity, and reconnect succeeds under the tested latency and loss budget.

### Phase 4: teleport prepare and commit

- Add destination reservation, transfer records, client handoff, commit, query,
  cancellation, and cleanup.
- Add asynchronous script bindings and Studio controls.
- Preserve current same-process teleport semantics.

Gate: crash and timeout injection at every step leaves at most one admitted
player and no permanent reservation leak.

### Phase 5: parties, lobbies, queues, and placement

- Add party revisions and invitations.
- Add lobbies and ready checks.
- Add bounded queue partitions, constraint expansion, placement, and match
  results.
- Add capacity reservation, regional choice, and backfill.

Gate: party atomicity, cancellation races, placement failure, stale capacity,
backfill expiry, and regional fallback pass deterministic and load tests.

### Phase 6: text chat and moderation

- Add channels, membership adapters, safe message model, rate limits, and
  moderation results.
- Add mute, block, report, enforcement, audit, and privacy retention.
- Add client view models and Studio local moderation.

Gate: raw text never bypasses required moderation, block policy holds across
all channel and invitation routes, and privileged actions always have audit
records.

### Phase 7: social presence and external seams

- Add bounded friend and relationship provider interfaces.
- Add privacy-filtered presence subscriptions and joinability.
- Resolve Discord join secrets through invitations.

Gate: provider outage reveals no extra presence, subscription tables stay
bounded, and no external provider identity is accepted as engine account proof.

### Phase 8: operational hardening

- Run regional soak, queue load, reconnect churn, chat burst, and rolling
  upgrade tests.
- Exercise key rotation, datastore migration, dependency outage, and disaster
  recovery.
- Establish service objectives and capacity alerts from measured results.
- Complete security and privacy review before public hosted launch.

Gate: measured limits are documented, overload is bounded, and recovery drills
produce complete audit and operator diagnostics.

## Test plan

### Unit and property tests

- every lifecycle transition, including refused backward transitions;
- ticket signature, expiry, target, key binding, and replay checks;
- bounded codec round trips and fuzzed malformed input;
- lease epoch and reservation accounting;
- lobby and party revision conflicts;
- deterministic queue ordering and expansion;
- reconnect generation replacement;
- teleport state machine idempotency;
- Unicode, normalization, and chat bound checks;
- capability, privacy, block, and moderation scope evaluation.

### Integration tests

- client, server, and local issuer over each supported transport;
- registry, server heartbeat, ticket issue, admission, and player creation;
- public, private, and reserved resolution;
- party to queue to placement to group join;
- reconnect before and after grace expiry;
- server drain and replacement join;
- same-process and cross-process teleports;
- team and party chat membership changes;
- moderation outage, report deduplication, and enforcement refresh;
- Discord invitation resolution without identity trust;
- mixed current and previous protocol versions during rolling update.

Use real process boundaries for the small curated end-to-end suite. Do not mock
the exact protocol under test. Use deterministic fakes for unrelated providers.

### Load and soak tests

- session heartbeat fan-in and lease expiry storms;
- browser and presence subscription churn;
- queue depth, party-size mix, and regional imbalance;
- capacity reservation contention and backfill bursts;
- reconnect storm after a network interruption;
- teleport batches during server drain;
- chat bursts, moderation latency, and hot channels;
- report and enforcement update bursts;
- issuer key rotation with old tickets still valid through expiry.

Every load claim names hardware, build preset, topology, message mix, duration,
and percentile. Profile release builds for shipped cost. Development builds may
be used for sanitizer and correctness runs but not performance claims.

### Security tests

- forged, expired, replayed, cross-session, and cross-universe tickets;
- stale lease owners and stale connection generations;
- invitation guessing and code reuse;
- malformed adverts, service frames, Unicode, and oversized lists;
- chat sender spoofing and raw-text bypass attempts;
- privilege escalation across game, universe, and platform scopes;
- audit tampering and secret leakage checks;
- rate-limit evasion across identity and endpoint dimensions.

## Profiling gates

Profile and report:

- admission verification CPU and allocation cost;
- server heartbeat and occupancy update cost;
- queue matching by queue depth and party-size mix;
- reservation contention and database round trips;
- chat moderation and fanout latency;
- client snapshot application and UI update cost;
- reconnect resync bytes and server tick impact;
- teleport prepare-to-commit latency;
- control-plane memory per active session, party, queue ticket, subscription,
  and chat channel.

No service request or database operation runs inside the fixed simulation tick
without an asynchronous boundary. Completed results enter a world at its normal
deterministic barrier. Profile spans carry correlation names, and heap or byte
counters are reported at the allocation or transfer boundary.

## Open decisions

These choices need deployment, legal, product, or measured workload evidence.
They do not block the local admission work in phases 0 and 1.

- Confirm the shared library and deployable target names and tiers against the
  architecture graph before adding either target.
- Choose the first hosted identity issuer and account recovery policy. Keep
  `LocalProfile` and `Guest` available regardless of that choice.
- Choose the control-plane request protocol and durable store after measuring
  the first lease, ticket, and queue workloads. The engine contracts must not
  expose either choice.
- Set reconnect grace, reservation expiry, drain deadlines, and offline ticket
  verification windows from play tests and operating targets.
- Decide whether a paid relay deployment is needed after measuring rendezvous
  failure rates by network type and region.
- Define each launch region, residency restriction, and placement pool before
  enabling hosted regional choice.
- Select a text filtering and moderation provider, supported languages,
  escalation policy, chat retention, evidence retention, and appeal process
  with legal review before hosted chat launches.
- Decide whether hosted direct messages ship in the first chat release. World,
  team, party, and system channels do not require a durable inbox.
- Define the compatibility mapping from hosted textual identity subjects to
  `Player.UserId` values. The mapping must be stable, collision-checked, and
  must never become authentication proof.
- Define game-specific matchmaking skill inputs only when a real game can name
  the trusted producer, update cadence, and acceptable match quality measure.

## Explicit non-goals

The first implementation does not include:

- a global account registration or payment product;
- Discord, Steam, console, or another platform as the canonical identity;
- voice chat, transcription, image chat, or file transfer;
- a hidden gameplay traffic relay inside rendezvous;
- migration of arbitrary live ECS state after server process loss;
- universal game-specific skill ranking or match rules language;
- peer-to-peer authority for public hosted games;
- end-to-end encrypted hosted text chat that bypasses required moderation;
- unbounded public presence search or account enumeration;
- gameplay inventory, economy, achievements, or commerce;
- anti-cheat as a substitute for authoritative simulation and validation;
- separate microservices for every control-plane module before load proves the
  need.

## Completion definition

This system is complete for its first hosted release when:

- direct and offline play still work without a central service;
- public, private, and reserved sessions have distinct tested access behavior;
- hosted joins use signed, replay-resistant tickets and structured refusal;
- server leases, drain, capacity, reservations, and replacement are observable;
- parties can queue atomically, matches reserve capacity, and backfill expires
  safely;
- reconnect cannot create two active drivers for one player;
- cross-process teleport survives retries without duplicating or losing an
  admitted player;
- chat membership, moderation, mute, block, report, and enforcement are tested
  across all supported channel kinds;
- Discord remains an optional presence and invitation surface, never identity;
- control-plane state has migrations, retention, audit, and operator recovery;
- hostile input and overload stay within documented bounds;
- headless, integration, load, sanitizer, architecture, binding, and format
  checks pass;
- live hosted verification has either been approved and completed or explicitly
  recorded as skipped by the user.
