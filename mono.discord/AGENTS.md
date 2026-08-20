# discord - module invariants

What a person's Discord profile says while an Atomic program is running.
`shared` tier, above `Engine::core` and below every program. The studio, the
client, the server and the content origin all import it.

## Nothing here is called `Presence`

`network::Presence` already exists, and it means announcing on a subnet and
registering at a rendezvous point. The studio includes both headers. Two
`Presence` types in one translation unit is a trap for whoever reads it next,
so the connection here is `Link` and the payload is `Activity`.

## An activity is state, not an event

This is the property everything else leans on, and breaking it breaks three
other decisions at once.

An `Activity` says what is true **now**. There is no history, no ordering and
nothing to replay. So: a write that cannot go out is **dropped** rather than
queued, because the next pump re-states the same truth; the link stays quiet
when the new activity equals the last one sent; and a reconnect forgets what was
sent rather than trying to resume.

If something ever needs to tell Discord about a *thing that happened*, it does
not belong in this module. There is no queue here to put it in, and adding one
would take the guarantee away from the three places that already depend on it.

## Time is passed in, never read

`engine::net`'s rule, inherited through `mono.network`. Every backoff, throttle
and handshake deadline is an argument to `Link::Pump`, which is what lets the
suite exercise the whole retry ladder in microseconds without sleeping.

The one genuine wall clock this protocol needs is `timestamps.start`, and the
caller supplies it as `Activity::StartedUnixSeconds`. Reading it here would put
a non-deterministic input inside the one subsystem whose failures are hardest to
reproduce, and would do it for a field the caller already knows.

## Discord not running is not an error

It is the ordinary state of a headless origin, for months at a time. Every
connect failure logs at `TRACE`. A `WARN` there is a log file made of one
sentence repeated, and the real fault - a wrong application id - would be
invisible in it.

The one thing that *is* worth saying out loud is an `ERROR` frame before
`READY`, because that is almost always an application id somebody typed wrong,
and no amount of retrying fixes it.

## Everything from the socket is hostile

The socket is local, and that is not the same as trusted: anything on the
machine can bind `discord-ipc-0` and answer. So the length field is checked
against `MAXIMUM_FRAME_BYTES` **before** it is used to size anything, an opcode
outside the five is a corrupt frame rather than a cast, a payload that is not
JSON is ignored rather than half-read, and every field is checked for its type
before it is read.

## The Application ID is not a secret

It reads like one sitting in a preferences file, so it is written down here. An
Application ID ships inside every Discord game's binary and is visible to
anybody watching the socket; it names an application the way a package name
does. `studio/Config.hpp` forbids a **signing seed** in preferences, and that
rule is about a thing which authorises. This one does not.

## Off by default

Rich presence publishes the name of the file somebody has open to their entire
friends list. `Settings::Enabled` defaults off, an empty `ApplicationId` is
inert whatever else is set, and `HideNames` exists for the person who wants the
feature and not the file name.

## The socket search is a list, and the list is the feature

`SocketCandidates` tries four environment roots, ten socket numbers and seven
install layouts. The six layouts past the obvious one are Flatpak, Snap and
Vesktop, and they are how a large share of Linux users actually run Discord.
Cutting the list down would report "Discord is not running" to somebody looking
straight at it, with nothing in a log to say why.

The list is kept in step with `vionya/discord-rich-presence`, which is the
maintained implementation of this protocol.
