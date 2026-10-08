# Source group lifecycle

The native projection keeps source archive identity separate from executable
group state. A base `Node_Collection` projects as a group with its own local
children and sockets. Its path remains host-owned source data; imagegraph does
not open it. A Collection instance binds attributes and ordered parent sockets
to its base without replacing its local children. When an enclosing Group
contains a missing nested Collection, reconciliation creates a shallow group
instance, with no synthetic executor and no cloned children. A Group instance
instead reconciles its child nodes and nested groups against the base, cloning
missing members and their local animator data while retaining instance-local
overrides. `InstanceBase` records that relationship. Collection instances do
not receive inferred instance callbacks.

`parent_value` is the per-port alias for a `pc.group_input` boundary. Each
alias belongs to a particular control node and socket, and carries its own
`SourceParentInputBase` owner. That owner supplies the source parent input's
value and animation when the child alias inherits it; ordinary inputs continue
to use node `InstanceBase` and per-input override rules. Alias lookup and replay
must stay keyed by node and port, since one node can have multiple boundary
ports and nested Collection instances can inherit different owners. The
authored child record remains the serialized source of the alias.

For a routed group boundary, ordinary numeric `from_tag` values keep the
source output-index fallback interpretation, including tags such as `-1`,
`-5`, and positive indices. Reserved `-2`, `-3`, and `-4` select trigger or
metadata outputs in the source getter and are not ordinary boundary fallback
tags; native projection rejects those selectors where it cannot represent the
route. This is specific to the `from_tag` field. A `-4` value elsewhere can be
an authored empty-surface marker and must not be treated as a routing tag.
Import preserves the original archive records for save projection.

`RenderActive` controls automatic group rendering. A cold disabled group does
not schedule its children or downstream consumers. A forced update can render
a pure disabled group, but a non-pure group still waits for normal scheduling.
Disabling an already warm group can leave held outputs available during a
partial update; a full readiness reset clears scheduling readiness while the
old held output remains in that session. The flag is serialized with the
document, but session outputs and readiness are process state: after save and
reopen, a fresh session starts cold and a disabled group has no warm output to
reuse. Re-enabling it allows automatic evaluation again. Partial updates accept
explicit affected-node IDs and propagate their work downstream; unchanged
ready nodes keep their held outputs. `PureFunction` permits forced execution only after an explicit purity refresh
classifies the group as pure. Load, topology, membership, animation-mode,
pure-function and interface changes refresh that classification. Ordinary
value changes and render callbacks retain the cached classification.

Frame activity is separate from cached purity. Source constructors seed known
activity; native kernels and validated host captures retain the last observed
callback flag. A missing observation stays unknown rather than being guessed
static. Loaded opaque host nodes require supported observations before a
refresh can classify them. Group Input parent values belong to the enclosing
Collection, so their animation does not become a child Group control.

Held image arrays carry an explicit payload marker. Ordinary heterogeneous
arrays retain their raw value channel, even when produced by an array node.
New dynamic sockets on a held Array Split receive its source constructor zero
for representable Any or Scalar ports; unsupported narrow sockets refuse
explicitly. Reconciliation retains existing outputs and rejects changes
atomically when type or memory bounds cannot be preserved.

Authored junctions appear as typed canvas views within their owning group.
The views use the durable junction IDs and never become engine nodes on save.
Save validates group crossings before publishing, so a child producer cannot
bypass an output interface. Junction deletion requires a coherent interface
edit. The views do not establish group-border sockets or live visual behavior.

Tunnel lookup samples sender registry selectors from prior completed outputs.
Receiver callbacks read their current name getter and retain previous payloads
on a miss. Sender payload getters can read held values without a callback;
callback domains remain local to the sender whose callback ran. Typed image
arrays keep their provenance through frozen groups. Native demand evaluation
does not establish the source renderer's exact cold scheduling order.

These contracts are supported by native projection, reconciliation, replay,
and group-render tests. Joined batch44 passed both builds, all 3,606 core
cases, 360 source IO cases, asset cooking and 33 offscreen Vulkan cases.
Batch43 validated 26 workload profiles, including retained group, tunnel and
captured common-socket phases; its benchmark binary is unchanged in batch44. The broad run
retains 21 listener-startup failures under the sandbox. These checks do not
establish licensed-runtime parity or live Studio behavior. Reserved trigger
and metadata routes still require ordered source project-step state; they
cannot be inferred from readiness, callback success or current canvas position.
