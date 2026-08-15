# Security

## Reporting

Do not open a public issue for anything in the threat model below. Report it
privately through GitHub's security advisory form on this repository.

Include what you did, what happened, and - if you have one - the smallest input
that reproduces it. You will get an acknowledgement; if a fix takes a while,
you will get an explanation of why rather than silence.

---

## The threat model

This engine is built to be downloaded once and then pointed at game files from
anywhere. Two boundaries follow from that, and both are hostile:

**A server loads a game file from its operator.** For a self-hosted or
multi-tenant host, that operator is not the person who wrote the engine.

**A client loads a game file from a server.** Anybody can run a server, so this
is untrusted content arriving over a network into a binary on a player's
machine. This is the one that matters most: a crash here is somebody else's
content running on someone's computer.

Everything reached from those two paths is in scope:

- the game file reader
- cooked asset containers
- the session descriptor and the replication decoder
- the runtime shader compiler's input
- the script sandbox and its capability model, on both the client and the
  server, and per VM - there is no trusted-server shortcut and no
  trusted-language shortcut

Out of scope: bugs that need a hostile local user who already has code
execution, and anything that requires the developer to deliberately disable a
sandbox on their own machine.

---

## The control surface is a third boundary, and it is opt-in for that reason

**`--mcp-port` opens a socket that can read and write a running program's
worlds.** It answers Model Context Protocol, which is what lets a language model
or a script watch the engine and drive it: list scenes, read and write
properties, start and stop a world, read the log, read the profile. The control
surface is currently exposed by `server` and `studio`; `client`, the unified
harness and `cdn` do not register this option. It is a development surface and
it is deliberately powerful.

It has **no authentication of any kind**. It does not need any, because of the
three properties below - and it would need a great deal if any of them were
relaxed.

- **It binds `127.0.0.1` and nothing else.** Not `0.0.0.0`, not a configurable
  address. The acceptor is constructed with the loopback address spelled out in
  `mono.engine/control/src/Server.cpp`; there is no flag that widens it, and
  adding one would be adding a remote-control surface for a program that runs
  scripts and writes files.
- **It is off unless a flag asks for it.** Every program defaults
  `ControlPort` to -1 and opens nothing. A port that opened itself because the
  program started would be one nobody chose, on a machine where something else
  may be listening for it.
- **Anything that can reach loopback on that machine can drive it.** That is the
  honest statement of the boundary: this is a single-user development tool, and
  on a shared or multi-tenant machine it should not be opened at all. It belongs
  on a developer's own workstation, beside a debugger, and it is exactly as
  dangerous as one.

**Never enable it on a production host.** A dedicated server with `--mcp-port`
open is a server whose worlds can be rewritten by any local process. There is no
configuration that makes it safe to run in front of players; the flag exists so
that a person building a game can see and steer the engine while they build it.

The ports are conventional rather than enforced - any free port works, and the
defaults only exist so the two supported programs on one machine do not collide:

| Program | Port |
|---|---|
| `server` | 8734 |
| `studio` | 8738 |

## How the code is meant to defend itself

These are the rules a fix should restore, not just work around:

**Parsing is separated from building.** A reader validates bytes and produces a
description with every size, index and reference already checked. Something
else builds objects from the description, and may assume its input is sound.
Fusing the two is the standard way this class of parser gets exploited - a
half-built object graph holding a length the parser has not finished checking.

**A parser is small enough to read.** It lives behind a module boundary, its
dependencies are visible in one `CMakeLists.txt`, and its public surface is a
handful of functions. Reading is what catches the bug where a length field is
trusted before it is bounded.

**Every new untrusted parser needs a fuzz target.** Corpora belong beside the
targets. The current tree has negative parser tests, but it does not yet carry
first-party fuzz executables or corpora; do not describe those tests as fuzzing.

**Capability, not trust.** Nothing is safe because of where it ran.

---

## Current status

The current tree contains all of the major paths above: the XML game-file
reader, baked asset readers, the replication and HTTP network readers, runtime
shader compilation, Luau and QuickJS runtimes, and the loopback control surface.
They are active attack surfaces and must be treated as hostile input paths.

The parser suites cover malformed game files, assets and network messages with
negative cases. Dedicated first-party fuzz targets and corpora are still open,
so a crash or memory error in any parser remains a release blocker. Crashes or
memory errors in shader loading, SPIR-V staging, command-line handling or the
control surface are also in scope.
