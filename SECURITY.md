# Security

## Reporting

Do not open a public issue for anything in the threat model below. Report it
privately through GitHub's security advisory form on this repository.

Include what you did, what happened, and — if you have one — the smallest input
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
  server, and per VM — there is no trusted-server shortcut and no
  trusted-language shortcut

Out of scope: bugs that need a hostile local user who already has code
execution, and anything that requires the developer to deliberately disable a
sandbox on their own machine.

---

## How the code is meant to defend itself

These are the rules a fix should restore, not just work around:

**Parsing is separated from building.** A reader validates bytes and produces a
description with every size, index and reference already checked. Something
else builds objects from the description, and may assume its input is sound.
Fusing the two is the standard way this class of parser gets exploited — a
half-built object graph holding a length the parser has not finished checking.

**A parser is small enough to read.** It lives behind a module boundary, its
dependencies are visible in one `CMakeLists.txt`, and its public surface is a
handful of functions. Reading is what catches the bug where a length field is
trusted before it is bounded.

**Everything untrusted has a fuzz target.** Corpora live beside the targets. A
crash found by fuzzing is a release blocker, not a bug report.

**Capability, not trust.** Nothing is safe because of where it ran.

---

## Current status

v0.1. None of the untrusted paths above exist yet — there is no game file
format, no network layer and no script VM, so there is nothing on those paths
to attack. The rules are written down now because retrofitting them onto code
that already exists is the expensive version.

What does exist and is worth reporting: a crash or memory error in the shader
loading path, the SPIR-V staging, or the command-line handling.
