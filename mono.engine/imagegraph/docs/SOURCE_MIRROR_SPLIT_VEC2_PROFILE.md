# Source two-axis scalar animators

grug represents source `sep_axis` storage on catalogue Vec2, IVec2, Dimension and Range
inputs, including declared dynamic inputs. template-shaped names without
a declared dynamic input refuse storage. generic Vector is not a two-axis alias.

Polar Mirror keeps its local raw Path getter for `relative_dimension`,
`constant_dimension`, `position`, `center` and `scale`.
Each input owns its original X/Y scalar key lists. Shared property flags, end
mode and loop range stay on the existing input and animation track. Dormant
unsplit tuples and keys remain authored and survive saves unchanged.

retained axis storage carries the property's `Separated` flag. an overridden
instance chooses its own flag and borrows the original writer's key lists.
without an override, the getter delegates the choice through its instance chain.
a missing local flag means combined mode. the writer's flag does not prevent an
independent split getter from reading its stored axes. inactive scalar keys still
count toward bounds and survive projection, native saves and PXC edits.

only admitted getters choose whether timeline sampling needs a combined track.
a storage owner borrowed by a split getter does not activate its combined driver.
late getter admission adds previously unsampled ports while preserving earlier
samples under the same live byte budget.

unlinked raw axes are sampled before PCX expressions. linked producers retain
priority. consumer processing follows raw sampling; inactive combined drivers
are not executed while split axes supply the value. constant-unit IVec2 getters
round both components after raw sampling and PCX. saved nonconstant IVec2 units
remain unsupported and retain their source node instead of mapping it.

A static getter reads the first stored scalar key. An animated getter evaluates
a lone-key driver before considering the original writer's static multi-key
fast path. Otherwise the shared source key interpolation and driver controls
apply. Scalar axes are sampled before the Any-carried Path getter; X supplies
the Path ratio and Y is ignored by that getter. Its physical coordinates bypass
Reference units. Unlinked numeric axes still use the original Vec2 units.
Direct typed Path-to-Vec2 links remain rejected by source socket compatibility.

`setAnim` appends a sampled key, retimes the first key and refreshes the key map
without sorting. The native sampler preserves duplicate times and physical
storage order for this route. Its integer lookup reproduces the pinned
`updateKeyMap` overwrite order, including the final tail overriding earlier
intervals. Disabling replaces each axis with one frame-zero key after the writer
flag changes. Empty active scalar storage returns zero; an empty static getter
or enabling an empty scalar list returns an explicit unsupported diagnostic.

GroupReplay stores one owned axis overlay on the original writer. Instance
getters retain their independent flags and borrow that original overlay during
evaluation. Edits, projection, publication and rebinding retain both axes as one
atomic operation. They preserve the dormant unsplit storage; replacing that
ordinary animator leaves active axis edits intact. Active edits follow
`recalculateKeys` by sorting and removing equal-time records. The bounded native
profile keeps the first physical record for equal-time sorting; the pinned GML
does not establish the native GameMaker sort's tie order, so exact runner parity
for that tie remains unverified.

Native format 9 uses named `source_vec2_axis` X/Y blocks with the same full key
grammar as ordinary keys. an optional final 0/1 on each axis header selects the
animator; omission means active. X/Y modes must agree. complete authored
builtin-random captures use format 3 for active axes and format 4 when any axes
are inactive; format 4 stores the flag for every retained input. readers accept
formats 1 and 2 without axes and format 3 with active axes. PXC import retains the
physical scalar record identities separately for X and Y; inverse publication
preserves opaque key tails, unused extra axes, dormant tuples, links and units.
Empty saved PXC axes regain source constructor keys. Publishing a live empty
axis to PXC is refused because that save would reload a different value.

Admission uses the global 4096-key limit across ordinary keys and both axes of
all nodes. Clone bytes include required scalar key/name/driver storage; retained
bytes include vector and string capacities. Sampling admits its bounded index,
time and driver workspace before use. Group edits charge the old replay, new
replay, copied axis keys and sort workspace before allocation. Projection
preadmits the document clone, axis replacement and final combined key count.
Mode changes admit the old document/replay, separate old destination and new
key lists together. source `setAnim` and direct edits choose the selected input's
local separation flag even when its getter delegates to a base. aliased writes
retain the writer's separation flag and dormant key lists. paired mode changes
refresh delegated getter flags from their logical property owners, independently
of the shared writer. raw combined captures bypass property separation choices. Failed evaluation and authoring leave the published owners
unchanged. No provider pointer or borrowed key list survives the call.

Client frame scheduling includes authored separate-axis storage even when no
ordinary keys or explicit animated-input flags exist. Fixed-tick and live
sampling use the same existing CPU document route; repeated seeks reconstruct
the requested axes rather than retaining a sampled vector as authored state.
Missing animation flags preserve static values. Explicit source animation
enables sampled axis changes; both modes have literal client pixel fixtures.

the private selected-input sampler reads one declared Vec2, IVec2, Dimension or
Range getter with units disabled. its target processor, sibling inputs and
unread links remain asleep. required upstream getters and dynamically requested
PCX outputs still run. IVec2 rounds after PCX; Dimension and Range retain their
source pair resizing; Vec2 keeps long rows and pads short depth-two rows.
projection admits final row storage before copying and refuses image-array
shapes whose conversion is unrepresented.

the sampler's document-only route compiles a local plan under the caller's live
byte ledger and keeps it charged throughout sampling. that freshly compiled
immutable document needs no second compile inside evaluation. supplied plans
still undergo normal validation. compile or sampling refusal preserves the
prior result and its reservation. the caller admits its retained document and
prior result before the call.

source `self` and `node_values` read the host's retained input map. this sampler
requires an explicit observed map and matching expression owner when either is
used. it does not evaluate sibling getters to invent a fresh map. borrowed map,
prior result and replacement result share one byte ledger; refused evaluation
leaves the prior result and its reservation unchanged.

physical input moves rename stored axis ports and every scalar key target under
the replay's byte ledger. removing an original input retains its authored axes
even without a prior edit overlay. surviving overridden getters sample those
axes through the detached animator identity and its original track.
projection copies detached axes into each surviving alias, preserves its local
separation flag and clears copied source record identities. rebinding checks
the remapped scalar contents before retaining the detached replay.

source `getAnimators` initializes local scalar storage lazily from local
defaults. a group copy shares already-created axis arrays, but a copy made
before initialization can create independent arrays later. axis storage can
therefore have a different owner from the combined animator. group replay now retains that binding-time distinction independently from the
combined animator, including its original writer and immediate base. repeated
host binding preserves a cold alias after the base creates axes. new binding
copies the immediate base's current array rather than following combined owners.
a charged dependency memo lets descendants see parents newly bound in the same
transaction, regardless of request order.
input moves, detachment, projection and rebinding preserve that scalar identity.
scalar reads follow the delegated getter's captured axis identity; edits use
the selected local property's identity. warm scalar mode changes sample and
replace that independent owner, track and writer. changing a physical property's
mode updates scalar aliases independently from combined aliases. linked numeric
inputs bypass local axes; Mirror linked Paths still read local X as their ratio.
cold aliases refuse execution before source-backed retained initialization.
detached scalar mode changes remain unrepresented. retained initialization is still needed before separate/combine controls cover
those cases. repeated explicit source `setInstance` must
recopy the immediate base's current array even when its ID is unchanged; that
transition remains separate from host reconciliation and is unfinished.
source saves and restores local `def_val` separately from animator rows.
import retains finite two-element saved constructor pairs separately from
current socket values. native v9 text and capture envelope v5 preserve these
pairs; clone, replay and recording byte ledgers charge their owned storage.
mismatched saved lengths keep the catalogue constructor; matching unsupported
pairs retain an opaque source node. missing or empty scalar rows use the saved
constructor pair before the catalogue fallback. these records do not initialize
axis storage or warm an existing cold group binding.
current socket values cannot supply constructor provenance. separated reads lazily store
an axis array that later group binding can observe. native pure evaluation needs
a retained initialization transition or receipt to preserve that history.

the retained state must distinguish an uninitialized array from locally owned
axes and a shared axis identity. looking only at the final document cannot
recover whether group binding happened before or after axis creation.

separate/combine controls and missing shared-axis constructor storage are still unfinished.
this flag preserves
the source storage distinction needed by those controls. generic Vector, dynamic
HLSL property kinds and source constructors with unresolved defaults still need
separate source-backed handling.

This is a CPU reference profile. GPU acceptance and exact GameMaker runtime
comparison are separate checks. The existing source Wiggle driver needs its
unrepresented runner noise contract; fractional unordered key-map index
coercion, fractional source key-map storage, and native cubic keys without
represented tangents refuse execution rather than substitute values. Existing
source widget interpolation history is not captured; new authoring keys use the
same documented headless linear default as other source mode transitions.

The source scripts and hashes are recorded in
[source-mirror-split-vec2-sha256.json](source-mirror-split-vec2-sha256.json).
