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
key lists together. Failed evaluation and authoring leave the published owners
unchanged. No provider pointer or borrowed key list survives the call.

Client frame scheduling includes authored separate-axis storage even when no
ordinary keys or explicit animated-input flags exist. Fixed-tick and live
sampling use the same existing CPU document route; repeated seeks reconstruct
the requested axes rather than retaining a sampled vector as authored state.
Missing animation flags preserve static values. Explicit source animation
enables sampled axis changes; both modes have literal client pixel fixtures.

mixed-mode instance authoring and separate/combine controls are still unfinished.
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
