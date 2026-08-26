# control - module invariants

L13, `shared`. The Model Context Protocol surface: being asked what this program
is doing, and told what to do about it.

`shared` rather than `client`, and it has to be. A headless server exposes its
worlds through this and links no client code at all, which is also why the
module builds an answer and never draws one.

---

## Two programs open one, and any program could

`mono.studio` and `mono.server` register `--mcp-port` and nothing else does.
`mono.client`, `mono.cdn` and the unified harness link this module or could, and
none of them offers the flag - `RUNNING.md` and `SECURITY.md` both say so, and
this file claimed five programs until v0.19. If a third one gains a surface, fix
this paragraph in the same commit.

**What differs between two surfaces is which rows are in the table**, and that is
the design rather than an accident: a server has no selection and a content
origin has no worlds, so neither declares a tool it cannot honour. A client is
told exactly what this program can do instead of discovering it by calling
something that fails. The same rule runs through the resource and prompt tables:
the `AGENTS.md` resources and the file-backed prompts are listed only when the
executable was staged into a checkout, and the handshake declares a capability
only when there is something behind it.

**A registry, not a switch**, which is the whole reason this is a module and not
a file in `mono.studio`. Adding a tool is one `Surface::Add` beside the thing it
exposes. `tools/list` and `tools/call` read the same table, so **a tool that is
callable is described and a tool that is described is callable** - there is no
second list to keep in step. `resources/*` and `prompts/*` are two more tables of
the same shape, added at v0.19.

Before this was a module the protocol lived in `mono.studio`, and adding it to a
second program would have meant a second JSON-RPC implementation. That is the
duplication the root `AGENTS.md` calls the most expensive kind.

---

## Six groups, one call

`Surface::AddStandardTools` installs every group, and a program adds its own rows
after it. The groups are separately available and no program has yet had a reason
to differ:

| Group | Rows | Answers out of |
|---|---|---|
| `AddUniverseTools` | `engine_info`, `world_list`, `world_tree`, `instance_get`, `instance_set`, `engine_components`, `component_list`, `entity_query`, `component_get`, `component_set`, `profile_frame` | the worlds |
| `AddArchitectureTools` | `layer_table`, `module_get`, `module_may_link` | `expected_graph.json`, compiled in |
| `AddScriptTools` | `class_list`, `class_get`, `script_check` | the class table, and `scriptcheck` |
| `AddDiagnosticTools` | `log_tail`, `log_level`, `metrics_read` | `core::Log` and `core::Metrics` |
| `AddBuildTools` | `test_run`, `test_result` | a spawned `testrunner` |
| `AddStandardResources` / `AddStandardPrompts` | the layer table, the module graph, the component catalogue, every `AGENTS.md`, the bindings; the `.claude/commands` files and an architecture-review pass | compiled in, or the checkout |

**`module_may_link` is the row worth defending.** It answers the question the
architecture check answers, out of the same file, and until v0.19 the only way to
ask it was to add the edge and watch a configure fail. `src/Architecture.cpp`
carries the one uncomfortable part: the two *rules* are spelled a third time
there, beside the CMake that owns each. The *data* is not copied at all, which is
the half that changes.

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

**`test_run` is what that rule looks like when it is enforced.** A full run is
171 seconds of suite time on this tree, so the tool spawns `testrunner`, returns a
handle and never waits; `test_result` polls it. `script_check` is the other side
of the same judgement and blocks on purpose: 111 ms measured for one file against
the whole declaration set, which is a hiccup rather than a stall, and it is
bounded by a cap on how much source it will take.

---

## What a tool may invoke

Two tools start a child process, and neither takes a command line from a client.

- `test_run` runs `<build>/tools/testrunner` with arguments this module
  assembles. It runs the suites and **does not build them**: a compile is minutes
  of the machine and the caller did not ask for one.
- `script_check` runs `<build>/tools/scriptcheck` over one file, and refuses a
  `path` that resolves outside the checkout.

There is no shell anywhere, no `just`, and no path a client can influence. That
matters more than it looks: the surface has no authentication, so a tool that
took a command line would turn "can reach loopback" into "can run anything",
which is a different boundary from the one `SECURITY.md` describes.

**Nothing here evaluates a script**, and the reason is the tick rather than a
security posture - this surface may already write a property and start a world.
A type check is a pure function of text with a bounded cost. An evaluation is a
Luau chunk with a `while true` in it, and there is no thread here to interrupt it
from: the surface would have handed a client the ability to hang the program with
four characters, and the program could not log why.

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

`DEFAULT_PORT` is 8738 and `DEFAULT_SERVER_PORT` is 8734, and neither is a
default in that sense: they are what `--mcp-port PORT` conventionally takes, and
the flag still has to be given. The number is named in three places a compiler
cannot see - `.mcp.json`, the `just mcp` recipe and this header - so
`mono.tools/mcpbridge/CMakeLists.txt` reads the declaration at configure time and
fails the build when any of them disagrees. Four copies had already gone wrong
twice before that check existed.

---

## asio is private and stays private

`Server.hpp` keeps asio behind a pimpl, so nothing that links this module
compiles a networking library. Putting an asio type in a public header would put
a socket vocabulary in front of `mono.server` and `mono.studio` alike, for no
gain - neither of them wants to see one.
