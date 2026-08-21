# control - module invariants

L13, `shared`. The Model Context Protocol surface: being asked what this program
is doing, and told what to do about it.

`shared` rather than `client`, and it has to be. A headless server exposes its
worlds through this and links no client code at all, which is also why the
module builds an answer and never draws one.

---

## One protocol, five programs, one table

The editor, the server, the client, the unified harness and the content origin
all answer the same handshake and the same `tools/list`. **What differs is which
rows are in the table**, and that is the design rather than an accident: a server
has no selection and a content origin has no worlds, so neither declares a tool
it cannot honour. A client is told exactly what this program can do instead of
discovering it by calling something that fails.

**A registry, not a switch**, which is the whole reason this is a module and not
a file in `mono.studio`. Adding a tool is one `Surface::Add` beside the thing it
exposes. `tools/list` and `tools/call` read the same table, so **a tool that is
callable is described and a tool that is described is callable** - there is no
second list to keep in step.

Before this was a module the protocol lived in `mono.studio`, and adding it to a
second program would have meant a second JSON-RPC implementation. That is the
duplication the root `AGENTS.md` calls the most expensive kind.

---

## Everything runs on the thread that calls `Answer`

`Server::Pump` does its socket work on the caller's thread, and there is no
socket thread. That is not a simplification to be optimised away later: it is
what makes a tool allowed to touch a world at all, because `Universe::Enter`
aborts on a foreign thread rather than racing.

**Two things follow.**

- **A tool is expected to be quick.** It runs inside a frame or a tick. A tool
  that walks every entity in every world is a stall the program cannot see the
  cause of, which is why `world_tree` is depth- and count-limited and says so in
  its own description.
- **Do not add a thread here.** A tool that needs to do slow work should start
  it and return a handle, not block the tick. Nothing in this module may make
  the answer arrive on a thread the world does not belong to.

---

## The description is the documentation, and it is written for a stranger

A `Tool::Description` is the only documentation a client ever gets. It is read
by a model that has never seen this engine, so it carries the vocabulary as well
as the behaviour: that a world is a scene, that the universe is the game, that
stopping restores a snapshot.

**Write it for someone who does not know the words.** "Lists worlds" is useless.
The existing rows are the model to copy, and `engine_info` ends with "Call it
first" for a reason.

**Refusing is a tool error, not a protocol error.** Set `failure` and return
null. MCP draws that distinction so a model can read the reason and try again;
returning a protocol error instead ends the conversation over something the
caller could have fixed.

---

## The three component tools are three different questions

Easy to conflate, and a client that picks the wrong one gets a confidently empty
answer.

| Tool | Answers | Source |
|---|---|---|
| `engine_components` | what storage this **engine** has: every registered type, its size, whether it is a tag, whether a save can carry it, whether replication has a compact form | `ecs::Components`, per process |
| `component_list` | what a **game** declared for itself, with its fields and how many entities carry each | `ecs::Schemas`, per world |
| `instance_get` | what one **instance** currently holds, as properties | the class tree, per entity |

`engine_components` takes no world, and that is deliberate: the component table
is per-process and sealed before any world ticks. Accepting a world argument
would imply the answer could differ between two, which is the belief
`Components::Seal` exists to prevent.

`docs/ECS_COMPONENTS.md` is `engine_components` plus a written purpose per row.
The purpose cannot be served from here because it lives in a checked-in file and
not in the process.

---

## Nothing links this by default, and the port is never open by default

Each program opts in by constructing a `Surface`, and each opens its port only
when told to. `SECURITY.md` carries why that is a decision rather than a
default, and `Server` binds only to `127.0.0.1` and accepts one client.

**Do not add a default port, a default bind address or an auto-start.** A
control surface that is open because nobody turned it off is a remote code
execution path wearing a developer tool's name.

---

## asio is private and stays private

`Server.hpp` keeps asio behind a pimpl, so nothing that links this module
compiles a networking library. Putting an asio type in a public header would put
a socket vocabulary in front of `mono.server` and `mono.studio` alike, for no
gain - neither of them wants to see one.
