
# DEFERRED

### [x] D00015

**Closed at v0.15 with all three parts built, and the third only after the invariant that forbade it was amended on purpose.** Three proposals for replication bandwidth, recorded together because they interact and separately because they were not equally ready. Written before any code, in the shape `v02v03v04.md` used: the open questions were the point rather than the plan, and each of the three closed by its own question being answered.

**(a) Lossy quantisation on the wire — DONE.** Wire version 4.

- **`scene::Transform` is 28 bytes and crosses in 10; `scene::Motion` is 24 and crosses in 12.** Position is three fixed-point axes and rotation is smallest-three — the largest component dropped, three sent at ten bits each, a two-bit index, exactly one 32-bit word. **Measured 25 entity values a datagram becoming 50**, against the real 1159-byte limit with `ChunkBytes` asked for above what can ever fit. The predicted 2.5x was optimistic by exactly the eight-byte entity handle, which does not shrink: the ceiling is 36/18 and the measurement lands on it. `just unified` at 64 entities is 6 messages and 4685 bytes a tick becoming 3 and 2612.
- **The largest message did not move, and that is the answer rather than a disappointment.** The packer fills to `ChunkBytes` whatever the stride is, so what changed is how many entities are in a datagram and not how big one is. At 2000 entities the largest message *rose* twelve bytes, because the budget is filled more completely.
- **The seam is a second pair of hooks on `ecs::TypeDescriptor`, not a codec over `Write`, and the difference was demonstrated rather than argued.** `Save` and `Load` are what a recording is made of. With the codec installed over `Write` instead, **`just determinism` and `just replay-check` both still passed** — they were comparing one lossy file against another. **That is a limit of both recipes worth knowing on its own**: they prove two runs agree, not that either is right. The mutation is killed by one case and by nothing else in the tree, which is why `TypeDescriptor::Wire` is its own slot and why `ecs/AGENTS.md` now carries the convention the build cannot check.
- **A wire form is installed by the registration that names the type**, which is what makes the two ends agree without either being told. The alternative considered — a table `replication` keeps by component name — makes agreement a discipline repeated in three programs and every test, and forgetting one is a receiver reading ten bytes as twenty-eight.
- **The snapshot path and the delta path were two places and are now one decision.** `BeginSnapshot` puts every value with a wire form *through* it before copying into the scratch store, so a joining client is given what the far side would have decoded. Without it a client's world depended on when it joined — which never shows as a failure and always shows as drift between two clients.
- **The grid is stated as the world's extent divided into steps, and the error is a bound in metres.** ±64 m in 32767 steps each way: 1.953 mm apart, **0.977 mm per axis anywhere in the world including both walls**, 1.69 mm on a 3D distance. Rotation is **0.0042 rad (0.24°)**, derived and then measured at 0.00408 over 400k orientations. Velocity is coarser on purpose and the justification is a test rather than a sentence: 3.9 mm/s over one 60 Hz tick is 65 µm, fifteen times under the position grid. 32767 of 32768 codes, so `+HalfExtent` is exactly representable — `Bounce` pins entities there, so the far wall is the common case and not an edge one.
- **Outside the extent an entity is clamped, on decode as well as on encode.** A clamped entity piles up against a wall somebody can see; a wrapped one is at the far side of the world and indistinguishable from a teleport the server meant. The decode clamp is not belt and braces: the encoder never emits -32768, so a trusting decoder would put a peer's entity outside the world this module states everything is inside. `WireCoversWorld` is the check for a world larger than the grid and it is a `static_assert` **where the world's size is authored**, because the encoder sees one component and not a world.
- **Both ends decode identically by construction, which is what (b) needs.** Every scale is a whole number over a power of two and a decode is one correctly-rounded division, so the value a client holds is one the server can predict bit for bit. Encoding is done in `double` — a float multiply near 32768 rounds to the wrong code and pushed the worst case 0.4% past half a step, which would have meant a bound with an apology in it.
- **Twelve mutations, twelve killed, one needed a new test.** Measuring the message-fit check against `sizeof` rather than the wire size survived the first sweep: it silently refuses a component that would have fitted, and nothing built a component large in a store and small on a wire.
- **Not done: the join snapshot is not itself smaller.** It carries the *decoded* value so that snapshot and delta deliver identical bytes, but it is still written by `Store::Save` at full width. Narrowing it means giving `Save` a lossy mode, which is the exact thing this design refuses — and the join is a one-off spread across ticks where the delta is every tick.

**(b) Group signatures — an audit layer, and the most interesting of the three. DONE.** Wire version 7, `replication/Audit.hpp`, `MessageKind::GroupSignatures` and `MessageKind::Disputed`.

- The server hashes groups of replicated state and sends the hashes; the client hashes its own copy and reports mismatches; the server sends the true state on a later tick. **Anti-entropy over the replicated world.**
- **It is complementary to deltas rather than a second way to do one job**, which is the question `docs/CODE_QUALITY.md` asks and the one this has to answer. Deltas are the fast path — what moved. Signatures are the audit — what disagrees. The audit is what makes the delta path's optimism safe, and it catches **generically** the whole class of bug this version chased one cause at a time: the lost creation, the stranded value, the stale forget, the tick that never completed.
- **The open question is answered per-client over a rotating slice, and the reason is that the other answer was not this entry's to give.** A cell hash is shareable only if a client sees a whole cell or none of it, which turns interest management from per-entity into per-cell — a real architectural change, and a larger one than a bandwidth entry authorises. The rotating slice is what bounds the per-client cost anyway: `AuditSettings` is `EntitiesPerGroup` 16, `GroupsPerAudit` 2, `EveryTicks` 8, so one audit is 32 entities in one datagram once every eight ticks and a world twice the size is swept half as often rather than costing twice as much. **The cadence argument was the strongest part of the proposal and it is what shipped.**
- **Membership is on the wire, and that single decision is what made the rest fall out.** The audit lists the entities it hashed rather than letting the receiver derive them from a group number, which lets the sender leave three sets out without the receiver knowing anything about them: anything still `Unconfirmed` (the delta path is already correcting it, so the server is merely ahead), anything the client *owns* (v0.13 ownership makes the client's copy the newer one), and anything carrying a `SuppressWhenTagged` tag (the far side derives that row, so the two ends are meant to disagree). It also means nothing about interest management, suppression or ownership had to cross.
- **What is hashed is the value a replica holds, on both ends, and assuming the quantiser is idempotent would have been wrong.** The authority puts its own value through the same encode-and-decode `BeginSnapshot` already puts a join through, so the two ends compute one expression over one buffer. Measured on the real smallest-three rotation: **1666 of 2 million** uniformly random orientations re-encode to different bytes, because the recovered component can come back below one of the three that were sent and the next encode drops a different one. That is a false mismatch reported for ever, every sweep, on one part in twelve hundred. `assets::HashTree` is the hash — tagged interiors, leaf count sealed into the root, which is what makes a *missing* entity a different digest rather than a matching prefix.
- **The rate limit is enforced by the server and none of it is read off the message**, which is what this entry called part of the security argument rather than a tuning knob. An answer must name the audit this server issued, on the tick it issued it, with labels that are groups this server hashed and strictly ascending so one cannot be named twice — and an audit may be answered once. The most a client claiming everything mismatches can buy is the repair of exactly the slice the server had already chosen to look at. An audit the link refused is struck off by `Unsent`, because a question that was never asked may not be answered.
- **The repair is the recovery walk and not a resend.** A disputed group puts its entities back into `Unconfirmed`, which is the same seeding an entity coming into view already gets — no second path, no new message, no structural churn. What it cannot reach is a client holding an entity the server has no record of sending; the bound on that is the one this module already had, `ResnapshotAfterTicks`, reached because a delta naming a row the client does not hold never lets `Applied` move. `Statistics::Disputed` says whether any of it is happening.
- **Off by default, on in `mono.server`, and that is the one thing that could not simply be the default.** `replication/AGENTS.md` says a quiet world sends nothing, and anti-entropy is precisely the thing that must speak on a world at rest — a value stranded on a still world is what no delta would ever report. Both cannot hold, so the host decides. `studio.playlink` and `engine.replication.stream` each pin the quiet-world property and are untouched.
- **Ten cases, ten mutations, every one red.** The two that matter are a replica deliberately diverged on a still world — corrupt a row on the client, and it comes back — and a client answering with every group label there is, which is refused and counted. The others pin the three exclusions, the round trip, the once-only answer and the refused audit; the mutation that removes the slice-range check does not merely fail, it reads past the recorded slice, which is what that check stands in front of.

**(c) The client dead-reckoning bodies from the quantised state — DONE, and the invariant was amended rather than read narrowly.** `replication/AGENTS.md`'s prediction section, `InterpolationSettings::ExtrapolateSeconds`, `SnapshotBuffer::DeadReckonSeconds`, `physics::Advanced` and `client::CollectReplicated`.

- **The amendment is the deliverable, and it is a distinction rather than an exception.** *Prediction is the local player and nothing else* still holds word for word about what it was written about: no entity but the nominated one is run ahead on the strength of inputs, nothing replays an input for a row it does not own, and `Store::CreatePredicted` is still the only identity a replica may mint. What was added beside it is that **predicting an input-driven agent is guessing at a human and dead-reckoning a body is evaluating a function the authority already sent** — `scene::Motion` *is* the derivative of the pose. The two now have separate rules and the file says which is which. Amended in the file, in the section, with the reasoning; not quietly narrowed.
- **`D00010` is unchanged and is now *scoped*, which is the reconciliation.** That entry decided a dry buffer stops rather than extrapolates because guessing forward is "a freeze plus a lie". Everything it decided still happens: the render clock stops at the newest sample, `Sample` holds the last pose the authority described, `Statistics::Stalls` counts it, and `engine.replication.snapshotbuffer`'s original case asserts it verbatim. What v0.15 adds sits **outside** the buffer and only on the rows carrying the one thing D00010 did not have — a velocity the server sent, which is a right answer to extrapolate *toward*. A player has none; a body with no `scene::Motion` has none; and for both of those the freeze stands, unchanged and for D00010's own reason. The lie D00010 refused was a guess with nothing behind it.
- **The bound this entry said nobody had measured, measured.** Per axis, worst case over the whole of both grids, `engine.scene.wire`: **0.977 mm at zero elapsed, 1.042 mm after one 60 Hz tick, 1.367 mm at 100 ms, 1.953 mm at 250 ms and 4.883 mm at one second** — a straight line of 3.906 mm per second of elapsed time on an intercept of 0.977 mm, which is the position grid's own error. On a 3D distance multiply by sqrt(3): 1.69 mm to 3.38 mm across the horizon. Every figure is within 0.003% of the analytic bound, so the growth is the quantisation and nothing else — the sum has to be taken in `double` to say that, because the same sum in `float` at 64 m from the origin exceeds it by an ulp, 3.5 um, which is a fact about float addition rather than about the grid.
- **The horizon is therefore derived and not chosen: a quarter of a second.** It is where the integrated velocity error equals the position error the integration started from — `WIRE_POSITION_HALF_EXTENT_METRES / WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND`, because both errors are half a step of their own grid and the step counts cancel, leaving 64 m over 256 m/s. Past it the guess is worse-conditioned than the last thing the authority actually said. It is authored in `scene::Wire.hpp` where both grids are and restated in `replication`, which links no simulation module and may not see them; `client.replicated` is the suite that fails if the two ever disagree, because rule 6 says a rule the build cannot check needs a test.
- **The correction is the guess *unwound*, which is neither a blend nor a snap.** The offset a caller adds is `velocity * seconds`, so easing `seconds` to zero eases the offset to zero — one number for the whole world instead of a per-entity pose blend, continuous by construction, and needing no second copy of any pose. It is given back at half real time, which is the fastest rate at which a corrected body still moves forward: at one it stands still and above one it moves backwards, and a body reversing is the one artefact more visible than the snap this replaces. A horizon's worth therefore takes half a second to unwind.
- **The collision answer is that nothing runs one, and the guess is bounded so that it does not have to.** No broad phase, no narrow phase, no solver, no gravity — a replica holds whichever colliders interest management let it see, so a swept stop would be right about the geometry that arrived and confidently wrong about the geometry that did not, and building an index to ask with is the per-tick pass over the whole replicated world that `mono.client/AGENTS.md` says this process does not do. Instead **a body is never carried further than its own smallest half-extent**, so the worst an unrun contact can cost is an overlap with something it was already touching. That also scales the feature the right way round without a second rule: a walking body gets the full quarter second and a twenty-metre-a-second one gets 25 ms, which is the case this entry was worried about.
- **Ownership decides the set, exactly as this entry said it must.** Extrapolate what nobody owns: a row carrying a `scene::NetworkOwner` is simulated authoritatively by its owner, so there is nothing arriving for a guess to be reconciled against and guessing as well would simulate it twice with one of the two wrong. The test is the presence of the component, not who it names.
- **Presentation only, and `(b)` is what turned that from a statement into an enforcement.** The guess reaches a `DrawInstance` and nothing else. It could not reach a component even if the interpolation rule did not already forbid it: the audit hashes what a replica *holds*, so a row overwritten by a guess would be reported as disagreeing with the authority on every sweep, for ever. The two parts of this entry meet there and neither had to be told about the other.
- **It cost a caller and *no* tick order, which is less than this entry predicted.** `CollectReplicated` is an existing `PreRender` system and no system, phase or registration was added. `physics::Advanced` is an `inline` function in a public header, so the linker pulls no new member for it: measured on `release` after the change, `IntegrateMotion.cpp.o` is still **out** of the client binary, and so are `BroadPhase`, `NarrowPhase`, `Solve` and `SyncBroadphase`. The reason it is a free function returning a pose rather than the private `IntegrateOne` being published is that a caller must be able to *place* a body without being handed a way to *step* one; `IntegrateMotion` is now written out of it, so a client cannot integrate arithmetic the server does not.
- **Thirteen mutations, thirteen red**, and two are worth naming. Dropping the guess to zero on the frame the stream returns — the snap the correction exists to avoid — is invisible to every counter and is caught only by a case that measures per-frame steps and asserts none of them is backwards. And deriving the horizon from the *angular* grid instead of the linear one still compiles, still produces a plausible number, and is caught by the two-constant agreement test rather than by anything about behaviour.

**Sequencing, and it fell out of the above rather than being chosen:** (a) was self-contained, needed no invariant changed, and is what made (b) sound. (b) had one open question — per-client against per-cell — and answered it per-client over a rotating slice, because the other answer was a change to interest management that a bandwidth entry does not authorise. (c) was what was left, and it needed exactly the two things this entry said it did: a rule rewritten, which was the repository owner's decision and was taken, and a bound nobody had measured, which is the table above. **Reopen trigger for (a): the first world whose delta does not fit at the current budget** — which the priority work made survivable rather than fatal, so it is a bandwidth question rather than a correctness one. **For (c): the first replicated body whose motion is not a constant velocity** — a projectile under gravity is dead-reckoned late by half g t squared rather than wrongly, which is a smaller lie than being underground and is why no acceleration is integrated, but a game built around arcs would want that measured rather than argued.

### [x] D00008

**Closed at v0.15 by deciding not to declare it, which is the answer this entry
had been giving for four versions without being allowed to stop.** The
single-player `ALLOW_TIER_ESCAPE` in `mono.client/CMakeLists.txt` stays written
out in a comment and undeclared. It is closed as decided rather than as done,
because there is nothing left to find out: the question was whether a
`client`-tier program would need to link a `server`-tier library, and the answer
arrived three separate times as no.

**The evidence is that the feature kept shipping without it.** v0.3 expected the
escape to be part of wiring the two programs together; `--connect` turned out to
talk to a server in another process over a UDP socket, which is precisely the
arrangement where the client links no server code. v0.7 landed the prerequisite
this entry named, `mono.engine/game`, and `mono.client --game FILE.agame` plays a
game file single-player under `HostRole::OfBoth`. That was declared with
`Engine::game` and not `Mono::server`, because playing a game file needs the
format and a VM rather than a hosted server. Twice the thing that looked like the
trigger was not it, and *hosting a server in your own process* stayed a narrower
idea than its description.

**The mechanism is no longer untried, and that is what makes closing this safe.**
`mono.unified_server_client` declares the escape because a diagnostic that runs
both halves of replication with `net` cut out genuinely needs both worlds in one
binary, and `mono.studio` declares it because an editor genuinely runs both
halves. Two users, neither of them this entry's, both visible in
`expected_graph.json`. So the argument for declaring a third pre-emptively is
gone: whoever needs it is copying a line with a working precedent rather than
writing the first one, and the tier check names the edge either way.

**Declaring it now would have been the actual mistake.** An escape with no user
is what the comment in `mono.client/CMakeLists.txt` says not to add, and it is
what somebody eventually reaches for to do something unrelated to single-player,
because it is already there and it already compiles.

**If it ever does arrive it is two lines and the comment names them**, and there
is a third option to weigh first that costs a process and needs no escape at all:
a single-player client that spawns `mono.server` as a child and connects over
loopback, buying the same crash isolation `parallel/process` already argues for.
The distinction to keep straight is that the escape is about linking rather than
about connecting.

### [x] D00031

**Closed at v0.15 by patching the editor's language server, which is the option
this entry declined three times.** `local face: Enum.NormalId` built and passed
from v0.14 and was underlined in the editor, because `mono.tools/scriptcheck`
registers `importedTypeBindings["Enum"]` on its own frontend and luau-lsp, reading
the same `engine.d.luau`, did not. The dotted spelling now resolves in both, hover
prints `Enum.NormalId`, and `just check` fails if either stops being true.

**The finding that made the decision, and it is the one the entry had been
guessing at.** There is no definitions-file spelling that closes this. Luau
resolves a prefixed type through `Scope::lookupImportedType("Enum", "NormalId")`,
and both `parseDeclaration` and `parseTypeAlias` reach `parseName`, a single
identifier — so a `declare` statement cannot produce a dotted name at all, and
`loadDefinitionFile` only ever writes a flat `exportedTypeBindings` entry. Every
write to `importedTypeBindings` in `Analysis/` is a `require` or a host reaching
into the frontend. **So the earlier bullets were right that the generator could
not fix this and wrong about why**: it is not that nobody had found the spelling,
it is that the grammar has none.

**The patch is thirteen lines and the hook was already there.**
`LSPPlatform::mutateRegisteredDefinitions` is a virtual with an empty body called
on every platform right after each definitions file loads, and luau-lsp's own
Roblox platform ends its override with `importedTypeBindings.emplace("Enum",
enumTypes)` from an API dump. The engine's copy sits in
`WorkspaceFolder::loadDefinitionFile` just after that hook — one hunk, pure
insertion, no header touched — because a platform of our own would have cost a
header edit and a second file for the same behaviour, and this is the shape that
conflicts least when upstream moves.

**The mechanism is a checked-in `.patch`, and that is the part worth carrying
forward.** `mono.vendor/AGENTS.md` said a patch goes upstream or into a fork;
neither fitted. There is nothing to send upstream — the loop keys on the `Enum_`
prefix this repository's generator emits and nobody else's — and a fork is a
remote to own, a push to make and a `main` to track forever for one hunk.
`mono.vendor/patches/luau-lsp-dotted-enum-types.patch` needs none of that: it is
visible in a normal diff, reproducible from a clean clone with `just luau-lsp`,
and it fails **loudly** at the moment upstream moves the code it edits, which is
the property the no-copied-files rule exists to protect. That file now carries the
argument and the three obligations that come with it.

**Hover was the open question and the answer was the one-line addition.** Without
it hover reads `local side: Enum_NormalId` — the flat name — because the type's
`name` is what `toString` prints and the binding alone does not change it.
Roblox's path sets `ctv->name = "Enum." + name` for exactly this reason, and the
patch does the same, measured both ways over a real `textDocument/hover`
round-trip rather than inferred. `scriptcheck` got the same line, so a diagnostic
from `just typecheck` and one from the editor name the same thing.

**Rule 6, which is why this is closed rather than fixed.** `just typecheck-editor`
runs `luau-lsp analyze` over every example through the language server an editor
actually runs, and it is in `just check`. **1.5 s**, so it went in rather than
beside; the cost that is not small is the first build of luau-lsp's own Luau —
11 minutes of CPU, 39 s wall on 24 cores, once, measured from a deleted build
tree and an unpatched clone. It catches both halves — a patch that stopped applying
and a spelling that stopped resolving — and it was proved by mutation in both
directions: reverse-applying the patch makes it report
`Portals-1-world.luau(410,39): Unknown type 'Enum.NormalId'` and exit 1, and a
deliberately broken patch makes `just luau-lsp` stop with the path named.

**Two numbers this entry stated were wrong.** It said 35 enums and then 36;
`scriptcheck` reports **38**, which is what the generator emits today. And the
entry was written about `Enum.Material`, which this engine does not have — the
example that exists, and the one an author tripped over, is `Enum.NormalId`.

**What is not closed, said plainly.** Under luau-lsp's `roblox` platform the
engine's `Enum_X` types are still mangled by Roblox's own loop, which strips four
characters and leaves `Enum._NormalId`. That costs nothing because `luau-lsp.json`
pins `platform.type` to `standard` and switching it was already the worst of the
three options here, but it is a real interaction rather than a theoretical one.

### [x] D00128

**Closed at v0.15 by moving the reader down rather than by vendoring one.**
There were three hand-written XML readers — `game::Xml` since v0.7, `Svg.cpp`'s
private scanner since v0.13, and `bake/src/Xml.hpp` extracted from it earlier in
this version — and rule 1 was the whole obstacle to there being one: `bake` is L9
and `game` is L10, so an importer naming `game::ParseXml` would have put `ecs`,
`world` and the save format underneath a foreign-format parser. The scanner is
`core/Xml.hpp` now, `bake/src/Xml.{hpp,cpp}` is deleted, and `game/Xml.cpp` lost
its recursive-descent parser and kept its document model and its writer.

**It landed at L1 and the entry's own candidate was one tier too high.** The test
applied was what the parser *depends on*, not what would be convenient: it opens
no file, links no vendor, allocates no global and names no other module's type,
so its height is the height of the standard library. `assets` was the obvious
candidate because `assets::ResizeImage` had made exactly this move — but
`ResizeImage` is arithmetic over an `assets::TextureData` and followed its own
dependency, and `game` does not link `assets` at all. Consolidating there would
have dragged content addressing, BLAKE3 and Crypto++ underneath the save format
to gain a string scanner. **The evidence for the placement is that
`expected_graph.json` did not change**: every caller already links `core`, so a
consolidation that needed no new edge is one that went to the right floor.

**The writer did not move, and that was decided rather than skipped.** It writes
this format's dialect — the declaration, tab indentation, `<x />` for an empty
element, a `]]>` split across two CDATA sections — has exactly one caller, and
can have no second one below L10 because nothing in `bake` writes XML. A writer
at L1 that only `game` calls would be an API nobody reaches for and a second
place to keep one format true. `EscapeXml` stayed with it for the same reason:
unescaping faces hostile input and belongs at the reader, escaping is the
writer's half.

**The two refusal policies are both in the shared header and neither is the
default.** `CheckEntityReferences` sweeps a whole document, which is right for
the SVG rasteriser because it never unescapes and so has no point at which a
reference would otherwise be met; `ReadContent` refuses at each point one is read
and exempts CDATA, which is right for `.rbxmx` because a real model in this
repository's corpus carries the Luau pattern `"[&;]"` inside a script and a sweep
refuses that file while naming an entity nobody wrote. **Collapsing them either
way round is the most likely way to get this wrong, so each direction has a case
that goes red**: "an rbxmx script's ampersand is source and not a reference" and
the CDATA half of "an svg's document type declaration is refused outright", both
proved red by making `ReadContent` unescape a CDATA section. The save format
takes `.rbxmx`'s route, for `.rbxmx`'s reason — a world's scripts are CDATA.

**What changed about the save format, stated rather than discovered later.**
Every document the v0.7 reader accepted is accepted and produces the same tree,
and `just determinism` and `just replay-check` are byte-identical either side.
Four differences, three of them narrowing and one of them not:

- **An element may carry at most `XmlLimits::MaximumAttributes`, which is 1024.**
  The writer emits nine at most, on `<Game>`. This was the one count a document
  stated that the reader had been taking on trust.
- **An element or attribute name must be an XML name**, and a `<` inside an
  attribute value is refused. Both were already true of the save reader and are
  now true of the two importers as well.
- **An entity reference must terminate within nine characters of its `&`**, so
  `&#000000233;` is refused where it used to expand. Every reference either
  format actually writes is shorter — `&#x10FFFF;` is the longest legal
  character reference and fits — and `EscapeXml` writes the five named entities
  and never a numeric one, so no file this engine has written contains one.
- **Some refusals report a different status**: `<!FOO` is `Refused` where it was
  `Malformed`, `</>` is `Malformed` where it was `Mismatched`, and a truncated
  numeric character reference is `Malformed` where it was `Truncated`. What a
  rejected document is *called* narrowed where the scanner's vocabulary is
  coarser than the old parser's. `Refused` is the one status that had to survive
  the mapping and does — it is the only one that means somebody tried something.
- **The one widening: text between the declaration and the root is now stepped
  over rather than refused.** `junk<Game/>` is still `Malformed` and so is a
  CDATA section before the root, both of which the loader checks; but
  `<?xml?>junk<Game/>` reads. The scanner steps over comments and the declaration
  inside one call and does not stop between them, so catching it needs either an
  option nobody else wants or the save reader knowing what a comment looks like —
  a second copy of the grammar, which is the thing this entry existed to remove.
  It is accepted because the run expands to nothing: no entity can be declared,
  so a reference in skipped text is inert, and a document still has to carry a
  `<Game format="N">` root to load. `game/tests/Xml.cpp` says so where somebody
  would otherwise read the gap as an oversight.

**The three attacks are tested at four places now rather than two**, because a
defence that holds in the scanner and is not reached by a caller is a defence
nobody has: `core/tests/Xml.cpp` drives the scanner, and the save format, the
model container and the rasteriser each still drive their own. Every case was
proved red by mutating the shipped source — the `<!` refusal removed, the
unknown-entity refusal removed, the code point bound removed, the attribute bound
removed, the name rule removed, and the scanner made recursive, which is the one
that turns 100,000 levels of nesting from a scan into a stack overflow.

### [x] D00117

**Closed at v0.15: a `TextBox` accumulates characters, and the three things this
entry named as remaining are the three that landed.** The trigger fired exactly
as written — somebody typed a character into a box and expected to see it.

- **`SDL_StartTextInput` is on the window, switched by whether
  `gui::FocusedTextBox` answers something.** Not left on: text input is what
  raises an on-screen keyboard and starts an input method's composition, so a
  client that asked once and never stopped would put a keyboard over the game on
  every platform that has one. Compared before it is called, for the reason
  `SDL_SetWindowRelativeMouseMode` beside it is.
- **`Translator::TypedText()` goes to `gui::Type` once a frame**, in
  `Client::Draw`'s interface block and *before* `Router::Update` — the
  characters were produced by a keyboard aimed at whatever held the focus when
  they arrived, so a press processed first would post them into the box the
  person is only now clicking on. The string never reached `scene::InputState`
  and did not have to: it crosses no snapshot, so the serialiser
  `Translate.hpp` refused to buy is still unbought.
- **The editing is `gui/Typing.hpp`**, which is `Type(store, Typing)` over the
  focused box: insert at the caret, replace a selection, Backspace one
  *character*, shift-arrow to extend, and Return that releases a single-line box
  and breaks a line in a `MultiLine` one. `src/Utf8.hpp` is the one place a
  character index and a byte offset cross, shared with `Focus`, which is what
  the entry meant by "the routine an editor would share".

**Three decisions the entry did not make and this closure did:**

- **The text lives in `Label::Text` and nowhere else.** No buffer on the router,
  none in the translator, no undo stack — rule 2, and the bug the second copy
  buys is a script writing `TextBox.Text` while somebody types into it.
- **Return releases rather than submits-and-keeps**, and a `MultiLine` box takes
  the line break instead. The release owes a `FocusReleased` the router cannot
  produce, because no press happened: `TypeResult::Released` says when, the
  caller builds the event, and `GuiEvent::Entered` carries Roblox's
  `enterPressed` — which was hard `false` under this entry's name and is now the
  difference between a field submitted and one abandoned.
- **A script setting `TextBox.Text` does not move the caret, and `Type` clamps
  before it reads one.** The property is a plain field the class table writes
  and there is no setter to hook, so a handler replacing the text with something
  shorter is ordinary — and every offset derived from the old caret would be
  past the end. The clamp is at the one reader that indexes by it rather than at
  the write.

**One thing this entry never said was missing, found while proving it:** nothing
in the shipped tree had ever called `gui::InstallGuiServices`, so no real world
had a `GuiService`, so `gui::Focus` refused every press and the whole focus half
was inert outside its own suite. `examples::LoadScene` now calls it beside
`scene::InstallServices`, which covers every `--script` world in the client and
the server. **The editor still does not**, and that is `mono.studio`'s to add.

**Still absent, and now for a reason about dispatch rather than about typing:**
`TextBox:CaptureFocus` and `:ReleaseFocus`. Every gui signal in this engine is
delivered by the host between frames — `Runtime::DeliverGuiEvents` over a list a
router or `Type` produced — and a script calling `CaptureFocus` would owe
`Focused` *inside the call that caused it*, which is a different dispatch rule
from every other signal here and belongs to whoever changes `script::Signals`
rather than to this entry. `UserInputService.TextBoxFocused` and its twin stay
absent for the pump crossing `script/src/UserInputService.cpp` records.

### [x] D00005

**Closed at v0.15 by the owner's decision, and the decision is the one this entry
had already argued for: no GitHub CI, ever, and a local gate instead.** The
reopen trigger was *the repository's owner asks for it*, and it fired in the
negative — what was asked for was the refusal made permanent, plus something
that makes `just preset=ci check` a recipe that actually gets run. There is no
`.github/workflows/ci.yml`, no `workflow_dispatch` stub, no action of any kind,
and `.github/` still holds nothing but `CODEOWNERS`. The paragraph below headed
*what this rules out* is not superseded; it is the closure.

**The entry's own lesson fired one more time on the way out, which is the useful
half of closing it.** It was reopened at v0.4 for claiming `just preset=ci check`
passed while the preset did not compile. Run again at v0.15 before anything was
touched, it did not compile again:

```
mono.cdn/src/Settings.cpp:125:75: error: possibly dangling reference to a temporary
                                        [-Werror=dangling-reference]
  125 |   for (const std::string &row : Flag("cdn.upstreams").Items()) {
```

`Items()` returns a `std::span` over the flag table's storage and not over the
temporary handle, so the reference was never dangling — but GCC cannot see that
through the call, `ci` makes it fatal, and the preset had therefore been broken
for however long it took somebody to think of typing `preset=ci`. Eleven
versions apart, the same warning class, for the same reason: nothing ran it.
`mono.client/src/Settings.cpp` had been reading the same flag kind through a
named span since the day it was written, so the fix was a line that already
existed six directories away. Everything else under `ci` was green — tests,
architecture, shaders, bindings, typecheck, determinism, replay.

**It was the third time and not the second, and the third is the one that
settles the argument.** `ROADMAP.md`'s v0.15 entry for the internal /
scripting-exposed split says the `ci` preset was already broken, lists what it
found, and finishes *"`just preset=ci build` exits 0 now"* — and
`mono.cdn/src/Settings.cpp` had carried this warning into the tree earlier the
same day. Both sentences cannot be true. Whatever the mechanism, a claim about a
preset written by somebody who had just run it was false within hours, which is
not a discipline problem and cannot be fixed by a firmer intention to remember.

**The gate is `.githooks/pre-push`, pointed at by `core.hooksPath`, installed by
`just install-hooks`, which `just setup` runs.** The hook is a file in the tree
rather than a thing in one clone's `.git/hooks`, because the latter is neither
cloned nor reviewable — which is the same objection this file has to a rule that
lives in somebody's memory. Only the pointer is per-clone local config, and a
fresh clone's first command installs it.

**It builds the `ci` preset and does not run the suites, and that is the whole
design rather than a shortcut.** The failure class is a warning going fatal,
which is a compile-time property; the suites are `just check`'s job. They also
cost two and a half minutes and are red in this tree whenever anybody's
half-finished module is, and a gate that fails for work that is not yours is a
gate that gets skipped every time — this entry recurring with extra steps.

**Measured rather than guessed, because a gate nobody tolerates gets disabled.**

| | |
|---|---|
| `just preset=ci build`, nothing changed | ~1 s |
| after a day of `dev`-preset drift | ~1 min |
| from an empty `.cache/build/ci/` | ~3 min, 1,998 targets |
| the whole of `just preset=ci check`, warm | ~3 min, of which `test-all` is 2.5 |

`ci` builds into a tree of its own, so a push after working in `dev` recompiles
what changed since the *last push* rather than nothing. That is the real cost and
it is why the hook is a build and not the chain.

**Proved rather than asserted, per rule 6.** An unused local was added to
`mono.tools/linecount/app/main.cpp`; `git push` refused with
`-Werror=unused-variable` named and the push abandoned, `git push --no-verify`
went through, and the same push succeeded in 1.4 s once the line was removed. A
deletion-only push skips the build in five milliseconds, since it compiles
nothing.

**Two honest limits, neither of which a workflow would have fixed and one of
which it would.** The hook builds the working tree rather than the commits being
pushed, so in a worktree with several people or agents in it, somebody else's
live warning refuses your push; that is correct — the tree is what is being
published — but it is not the same thing as gating a commit. And what this entry
always said was lost is still lost: **the second machine.**
`just check-server-is-headless` and `just check-cdn-is-bare` still prove the tier
split on a box that *has* a graphics stack, and a check that quietly depends on
something in this working tree still passes here forever and fails for the first
person who clones. Nothing local can close either. They are the price of the
decision, recorded rather than solved.

### [x] D00004

**Closed at v0.15 by the owner's decision, not by the trigger.** The reopen
trigger this entry finished with — *a program that calls `core::Random` and links
neither `net` nor `assets`* — never fired, and there is still not one. The owner
decided to do it anyway on the ground the entry itself had already narrowed to:
dependency hygiene at the lowest layer, plus one fewer row that
`THIRD_PARTY_NOTICES.md` owes to the module every other module links. **Not size.
The entry was right that it saves the shipped programs nothing, and the fresh
numbers below say so a third time.**

`engine::core::Random` is **SplitMix64's finaliser** now — Steele, Lea and Flood's
`mix64variant13` from *Fast Splittable Pseudorandom Number Generators* (OOPSLA
2014), spelled as Vigna's public-domain `splitmix64.c` spells it — over the packed
`(salt, index)` pair. Specified and citable, which was the only property SHA-256
was ever here for; three published constants instead of a hash compression. The
interface did not move, exactly as this entry predicted it would not have to.
`SecureWipe` went with it, since it was the only other call in `core` reaching
Crypto++ and `CryptoPP::SecureWipeBuffer` is a four-line volatile loop.

**The measurement, `release` preset, per program, either side of the swap.** Each
program relinked with `-Wl,-Map` and the map's *Archive member included to satisfy
reference by* section read directly, which is what reproduces this entry's
`43 of 173` exactly rather than approximately:

| program | `CryptoPP::` symbols | `libcryptopp.a` members | first cause in `core`? |
|---|---|---|---|
| `client` | 6,117 → **6,117** | 43 of 173 → **43 of 173** | none, before or after |
| `server` | 6,117 → **6,117** | 43 of 173 → **43 of 173** | none, before or after |
| `cdn`    | 5,712 → **5,712** | 37 of 173 → **37 of 173** | none, before or after |

Every first cause is first-party and none of them is `libengine_core.a`.
`client` and `server`: `libengine_net.a(Cipher.cpp.o)` five, `Handshake.cpp.o`
one, `libnetwork_lib.a(SessionKey.cpp.o)` three, `Advert.cpp.o` one. `cdn`:
`libengine_assets.a(Grant.cpp.o)` two, `Signature.cpp.o` one, and the same two
`network_lib` objects. **`cdn`'s zero is gone and this entry called why.** It said
the origin would stop being zero "for a reason that is not this entry's" and
predicted 36 members first-caused by `Grant.cpp.o` once `Origin` was wired into
`main`. It is wired, and the answer is 37 and `Grant.cpp.o`. One member out on a
prediction made two versions early.

**The program that does not exist, which is what the dependency was actually
worth.** A `main` calling only `Random::Float`, linking `core` and no other
first-party module, measured with `size`'s text column:

| | text | `CryptoPP::` symbols | members |
|---|---|---|---|
| SHA-256 | **1,552,030 B** | 5,647 | 36 of 173 |
| SplitMix64 | **1,732 B** | 0 | 0 |

That reproduces this entry's *1.55 MB and 5,647 against 1.6 KB and none* to the
byte on the first column and exactly on the other two — the one figure it recorded
as no longer reproducing, the 9,479, stayed gone. **Nothing drifted.**

**It also got about twenty-five times faster, which was never an argument for it
and is now a fact somebody will rely on.** `engine.core.bench.values` on the
`bench` preset: `Random::Float` 47 ns → **2 ns**, a three-salt position 146 ns →
**7 ns**, against 1 ns for a bare xorshift32 in the same run. `Random.hpp` used to
spend five paragraphs warning that it was a load-time budget and must not be
called per entity per frame; that warning is deleted rather than softened, because
the reason for it is gone.

**No caller relied on `Random` being unpredictable, and this was checked before
anything was changed rather than after.** Every security-sensitive site in the
tree already names `core::Random` in a comment saying it must *not* be used —
`net::Handshake`, `replication::Hello`, `assets::GrantKey`, `network::SessionKey`
— and each draws from the OS instead. `script`'s `HttpService:GenerateGUID` is the
one that looks like a counter-example and is not: it draws from `core::Random`
deliberately so that a recording replays, stamps the RFC 4122 version and variant
bits to make the *shape* a UUID, and states in its own comment that the value is
neither unpredictable nor unique across processes and must not be a session token.
It also has no first-party caller: nothing under `mono.*` calls it outside the
suites that test it.

**And the demonstration, because "SHA-256 was in there" invites the assumption
that something was lost.** `GenerateGUID`'s inputs are a per-runtime counter
starting at zero and an FNV-1a hash of the world's *name*, and a world's name is
not a secret. Twelve lines of Python knowing only the string `"scriptcall"` — the
name the parity suite gives its store — reproduce
`EA90FF9A-9A3E-498F-B626-EA711F6C37F4`, the exact literal that suite pinned under
SHA-256, and the three GUIDs after it. The old implementation was as guessable as
the new one; the hash never bought unpredictability because the thing being hashed
was public. So the swap is not a security regression: a deterministic generator
that was never a secret was replaced by a different deterministic generator that
is not one either.

**What it did break is what the v0.13 re-examination said it would: every seeded
stream moved.** `Random.new(seed)` in both script VMs draws through here, so the
sequence is part of what a saved world means. Three suites pinned values from the
old mixer and are red until the numbers are re-derived, all of them outside this
change's own module:

- `mono.engine/script/tests/ScriptCall.cpp` — two copies of the literal
  `EA90FF9A-9A3E-498F-B626-EA711F6C37F4`, which is
  `C03C1E18-C515-4449-AA47-612C9B59D267` now. **The two VMs do not disagree, and
  the failure reads as though they do.** `CHECK(luau == javascript)` passes with
  both sides showing the new string; only `CHECK(luau == probe.Expected)` fails,
  and Catch2 prints the probe's *name* — "GenerateGUID draws the same sequence in
  both" — as context on every assertion in the case, which is a sentence that
  looks like a diagnosis. The `GenerateGUID(0)` truthiness split still reads the
  same way in each language, and the three cases in `tests/HttpService.cpp` that
  assert the *properties* — the UUID shape, sixty-four draws with no repeat, and
  two runs of one world drawing the same sequence — all pass.
- `mono.engine/replication/tests/SnapshotBuffer.cpp` — `bufferedFrozen == 0`
  under `LossSettings::Seed = 0x5eed0010`, which is 6 now. The loss pattern is
  `core::Random::Float(arrivalNumber, seed)` and nothing else, so a different seed
  restores it; the two comparison figures in the same comment, 23 frozen at
  `DelayTicks = 0` and 4 at 1, were measured under SHA-256 and need re-measuring
  with it.
- `mono.unified_server_client/tests/Harness.cpp` — `DrawnX < ClientX`. The
  assertion has a latent assumption in it that this change happened to trip: the
  tracked entity's velocity comes from `Random::Range(index, 7u, -10.0f, 10.0f)`
  in the placeholder world and now points the other way, so "behind" is the other
  inequality. `Behind > 0.5` in the same case still passes, which is the same
  claim without the sign in it.

`just determinism` and `just replay-check` both pass and both would have passed
whatever was put behind the interface — they compare two runs of one binary, so
they are green on a generator that changed and agrees with itself. That is why
`core/tests/Random.cpp` pins actual values, anchored on the constant every
reference implementation of SplitMix64 prints for seed zero.

**`THIRD_PARTY_NOTICES.md` was checked and deliberately not edited.** Crypto++ is
still vendored, still linked by `net`, `assets`, `bake`, `network` and the test
runner, and still ships in all three programs. The root file's Crypto++ paragraph
already says `core::Random` "is no longer the cause", which was written when that
became true and is now true twice over.

### [x] D00104

**Closed at v0.15: `.rbxmx` is built, and with it every row of Rojo's file
table.** The entry's own summary was that an XML model needed a parser
`mono.vendor` does not carry, that the markup was the smaller half of the job,
and that when it arrived it would go in `bake` beside `.rbxm` and reuse every one
of `studio::RojoSync`'s mapping decisions. All three held. `bake::ReadRobloxModelXml`
hands back the same `RobloxModel` the binary reader does, `RojoSync.cpp` picks the
reader by extension and nothing else differs, and the editor changed in one line.

**The named reopen trigger was "an XML parser in `mono.vendor`", and the answer
is that there is still not one — deliberately.** That is the part worth keeping,
because the entry framed this as a vendor decision and the vendor decision went
the other way for a reason the entry could not have known. The engine already
had **two** hand-written XML readers: `game::Xml`, written at v0.7 for the save
format on the stated ground that the famous XML attacks are attacks on features a
save file does not need, and a private tag scanner inside `Svg.cpp` since v0.13.
So vendoring would not have removed a hand-written parser from the repository, it
would have added a library beside one. `Svg.cpp`'s copy became `bake/src/Xml.hpp`
and now serves both formats; `mono.vendor/AGENTS.md` carries the whole argument
under "Not adding one", and `D00128` is the consolidation rule 1 still prevents —
`game` is L10 and `bake` is L9.

**Three things the entry did not predict, and each cost a real mistake to find:**

- **A script's source is a `CDATA` section**, which is the XML half's version of
  the `ProtectedString` row the binary half warned about. A scanner that treats
  `<!` as a declaration refuses every file holding a script; one that treats it
  as markup mangles them. It was found by reading real files, which is the method
  the binary half's entry had already recommended.
- **A document-wide entity sweep is wrong for this format**, and that is not
  theoretical: a real `.rbxmx` in the corpus carries the Luau pattern `"[&;]"`
  inside a script, so a sweep that refuses undeclared references refuses that
  file while naming an entity nobody wrote. The sweep is right for SVG, which
  never unescapes; here the refusal belongs at each point a reference is read,
  with CDATA exempt because CDATA is text.
- **`Content` and `BinaryString` had to be read rather than refused.** They are
  separate elements in XML for what the binary container stores as one `String`,
  so refusing them would have made a `Decal` imported from `.rbxmx` lose a texture
  the same model keeps as `.rbxm` — a subset that looks smaller on paper and is a
  disagreement in practice. `bake/tests/RobloxModel.cpp` now reads one model
  written both ways and compares the trees field by field, which is what would
  have caught it.

**What stayed the same is the part the entry cared about most.** A referent is
still the shape of the tree and nothing else — in XML it is not even that, since
the nesting *is* the shape and the `referent` attribute is never read. An enum is
still refused, a `Ref` is still refused, and an unsupported type still costs its
property rather than its file.

### [x] D00120

**Closed at v0.15: `Player.Backpack` holds `Tool`s and the engine understands
one.** The entry's own summary was that the container was real, private to its
player on the wire, and filled by the spawn pipeline — and that *nothing in the
engine was a `Tool`*. There is a `Tool` class now, deriving from `Model`, and
equipping it is a reparent into a character `Model` exactly as it is in Roblox.

**The named reopen trigger was "a joint, or an attachment strong enough to carry
a handle", and the answer is neither: it is `scene::CharacterLimb`.** The entry
pointed at `scene::Attachment` as the first thing to look at, and it is the wrong
half — `ResolveAttachments` runs in `PreRender` and *resolves a frame* rather
than moving a part, and `Attachments.hpp` says in as many words that a caller
wanting a weld is asking for what that pass does not promise. What the engine
already had is the thing `Characters.hpp` chose over `Motor6D` for the whole
body: a limb is an anchored part carried along by a root at a fixed offset,
placed by `PoseCharacters` in one `CFrame` product. A held handle is one more
part in that formation, so a tool needed no constraint solver, no new pass and no
new component — it needed the row a forearm already has. The entry's premise that
"a tool that follows a hand is the same missing piece as a rig that does not fall
apart on a slope" was true of a *jointed* rig and not of this one.

**Three things came free and none of them had to be built.** The offsets
replicate, because `scene.CharacterLimb` replicates; the handle's per-tick
`Transform` stops crossing, because that component is already `replication`'s
suppressor for `scene.Transform`; and a client poses the handle from the root it
interpolated, because it already poses five limbs that way. `D00115` bought all
of it and this is its second consumer.

**What equipping is: the parent, and nothing beside it.** No `Equipped` flag and
no field naming the held tool — both would be a second copy of what the tree
already says, and the copy that goes stale the first time a script reparents one.
It is also what makes the wire rule fall out with no rule added:
`scene::PlayerOwning` answers the owner for anything under a `Player`, so a
stowed tool reaches exactly one client, and a held one sits under a `Model` in
`Workspace` and reaches everybody. `mono.server`'s interest predicate already
said both.

**Who may move one: the authority, through two doors that were already there.** A
client that can reparent its own tool can duplicate it — the write survives until
the next delta contradicts it, which presents as an inventory that works
sometimes. `ecs::Store::SetProperty` has refused every property write in a
replica since v0.3, which is where this engine answers "who owns a row" and which
is what refuses a `LocalScript`'s `tool.Parent = ...`; `EquipTool` and
`UnequipTool` make the same refusal on `Store::AdoptOnly` for the C++ door. That
is `scene::TakeDamage`'s pair, one class along, rather than a third statement.
`UpdateToolGrips` is deliberately on the other side of that line: it reparents
nothing, everything it writes is a function of where a tool already is, and it
therefore runs on a replica — which is what lets a client pose a handle whose
tool arrived over the wire.

**What happens on death and departure: nothing new, which is the point.** A
corpse keeps what it was holding — `Humanoid.Health` at zero leaves the body
where it fell, `D00121`'s rule — and `LoadCharacter` destroys that body with the
tool in it on the way to the next one, then refills `Backpack` from
`StarterGear`. That is the two-container rule `Services.hpp` already states,
unchanged: only `StarterGear` survives a death, equipped or stowed. A departing
player's `Backpack` goes with the `Player`, and their held tool goes with the
character `ReclaimOrphanedCharacters` collects. Neither case needed a line of
code, and asserting both is how that was established rather than assumed.

**One property, because one property has a reader.** `Tool.Grip` is composed onto
the grip point and is what places the handle. Roblox's `CanBeDropped`, `Enabled`,
`RequiresHandle`, `ToolTip` and `TextureId` are absent for the reason this entry
refused the whole class: there is no drop input, no activation channel and no
inventory interface, and a property nothing acts on is what "deliberately absent
rather than half-built" meant. `BackpackItem` is skipped for the same reason —
registering Roblox's abstract base would put an instantiable class that does
nothing into the insert palette.

**`Humanoid:EquipTool` is absent and is not the remaining half.** A class table
here carries properties and no methods, which is why `Sound.Playing` is a
property rather than `Play()`; and a Roblox script equips by writing
`tool.Parent = character` anyway, which is a declared property on `Instance` and
works. `PoseCharacters` calls `UpdateToolGrips`, so the world agrees with the
tree on every host that draws a character. The day classes carry methods, that
method sets a parent and nothing else changes.

**One bug fell out of building it, and it was older than this entry.**
`CharacterLimb` put its `core::CFrame` before its `ecs::Entity`, so the struct
carried a four-byte hole *and* four bytes of tail padding that `Reserved` had
been named to prevent — eight uninitialised bytes reaching every save file and
every wire delta, in a module whose `AGENTS.md` forbids exactly that. Found by
comparing a restored row against a recomputed one and watching two byte-identical
limbs disagree. The members are widest-first now and the struct is forty bytes
with no holes.

### [x] D00121

**Closed at v0.15: a character dies when `Humanoid.Health` reaches zero, and the
body stays where it fell until the respawn takes it.** The entry's whole subject
was that the trigger was missing — `scene::UpdateRespawns` measured
`Player.RespawnTime` from the tick a player was first seen with no model, which is
Roblox's delay attached to the wrong event. Both events schedule the same deadline
now, and the deadline is unchanged: `ceil(seconds / delta)` against the fixed tick,
so a respawn still lands on the same tick on every machine.

**Its reopen trigger fired in the same commit, which is why it is closed rather
than extended.** The trigger was "anything in the engine that damages a
character", and `Server::ApplyInputs` was the hit test with no consequence it
named. That path subtracts `SHOT_DAMAGE` now — a constant and never a roll, for
`FindSpawn`'s reason about picking a pad in tree order — so the health model has a
caller rather than being storage waiting for one.

**Who is allowed to write it: the authority, and it is one rule with two doors
rather than a new mechanism.** `ecs::Store::SetProperty` has refused every
property write in a replica since v0.3, which is where this engine already
answers "who owns a row", so `Humanoid.Health` is an ordinary writable property
and a client script asking for more is refused by name. `scene::TakeDamage` makes
the same refusal on `Store::AdoptOnly` for the C++ door, because the machine most
likely to want to write its own health is the one losing it — and a rule that only
covered scripts would be a rule a hit-resolution loop walks straight past. A
second authority flag on the descriptor was refused as the copy that drifts.

**`MaxHealth` has a reader inside the engine, which is the bar D00119 set.**
`Health` clamps against it and lowering it pulls the health down, so it is not a
number that exists only to be the denominator of somebody's bar. Both are written
conversions rather than `ClampedProperty` because each bound is the other field,
and `ClampedProperty` bakes its bounds in as template arguments so its setter can
stay captureless.

**What happens at zero goes through the lifetime machinery rather than beside
it.** `StepCharacters` skips a dead humanoid — it *replaces* horizontal velocity
every tick, so a corpse it still visited would walk on at `WalkSpeed` for the whole
delay — and `LoadCharacter` destroys the old body on its way to the new one, which
it has done since v0.14. Nothing new destroys anything.

**A death happens once without a flag on the row.** `TakeDamage` takes nothing
from a humanoid that is already dead, so two hits arriving in one tick are one
death, and `UpdateRespawns` schedules a deadline only for a player not already
carrying one.

**Two of the things the entry listed as dragged in were decided as absences.**
There is no `Humanoid.Died`: `scene` is L7 and a signal is L9, so a record here
would be a fact whose only consumer is a module that would have to be taught about
it — and `humanoid:GetPropertyChangedSignal("Health")` already delivers the
transition, once, through machinery that exists. And there is no `ForceField`:
`TakeDamage` is one door and a forcefield is one branch inside it, but the class
itself is a spawn-protection feature with a duration, a visual and a
`SpawnLocation.Duration` beside it, which is a second feature rather than the rest
of this one.

**`IsDead` is a function rather than `Health <= 0` written out three times.**
Three passes ask it, and it is spelled `!(Health > 0)` so that a NaN is dead: the
other way round a NaN compares false against everything and the character is
immortal, which is the one failure nothing downstream could explain.

### [x] D00113

**Closed at v0.15: there is one node graph, it is `mono.vendor/nodegraph`, and
the build now refuses a second.** The entry offered two ways out and named the
obstacle to the first — the template repository had no remote whose commits this
one could name. It has one, so that is the way it went.

**The direction is the half that was easy to get backwards.** The editor's copy
was the further-along of the two: folding a selection into a node whose interface
is derived from how it was wired, frames, a graph-owned template library, an
async evaluator with a worker pool, previews that belong to a wire rather than to
a node, a software surface rasteriser, a PNG writer and an inspector registry.
None of that is engine-specific and all of it went upstream. What stayed is what
only an engine has: a texture for a picture, a theme, an undo stack and a
dockable window. Sending the *template's* copy the other way would have been a
downgrade dressed as a merge.

**Two things had to be lifted out before the library could leave.** The canvas
and the inspector handlers called `engine::ui::Scaled`, `MutedColour`,
`AccentColour` and `WarningColour` at twenty-four sites; those became
`nodegraph::HostChrome()`, a process-wide table of four values, set by
`studio::ApplyNodeChrome` every frame the panel draws rather than once at
start-up — the interface scale and the palette are both settings somebody can
change while the panel is open. And `Graph::Hash` and `PictureKey` shared an
anonymous-namespace FNV mixer across what became two translation units, so it is
now a private header rather than a public one: a caller that could reach the
mixer could mint a key the cache would then honour, which is a stale picture
nobody can explain.

**The vendor is declared in `MonoVendor.cmake` although it ships a
CMakeLists**, which is not what the asio and imgui entries there are for.
Upstream builds its own copy of Dear ImGui, because the common case for a
template is a checkout with no build system beside it; adding that file here
would put a second set of every ImGui symbol on the link line and there is no
option that makes it consume an existing one. It also configures a test binary
and an SDL3 demo this repository does not want built.

**`VENDOR_PUBLIC` rather than `VENDOR`, and that is the widening this cost.**
The `Editor` holds the graph, the canvas and the evaluator by value and hands a
`nodegraph::PreviewImage` to its texture sink, so those types are in
`studio/Editor.hpp`. Hiding them would take a pimpl over the editor's whole
state. Nothing links `Mono::studio` but its own program and its own tests, which
is what makes that defensible rather than merely convenient.

**The suite moved with the code it tests**, which is `AGENTS.md`'s rule and not a
tidy-up: the cycle guard, the hash, folding, the save format, the async evaluator
and the canvas are 415 checks in `nodegraph_tests`, ported off Catch2 onto the
harness that repository already had. `mono.studio/tests/NodeGraph.cpp` is three
cases now, and every one of them is a seam neither repository can check alone —
the pixel order between `PreviewImage` and `assets::TextureData`, and the theme
reaching the library's chrome.

**The third implementation the entry warned about is what the build checks.**
`just check-one-node-graph` fails if any first-party file opens
`namespace nodegraph`. The render pipeline editor is still to be written and
`Engine::bakegraph`'s pipeline documents still have no panel; either could start
its own registry and its own canvas, and the first divergence would be in the
half nobody looks at.

### [x] D00102

**Closed at v0.15: a world carries its bake pipelines, and `server` still has no
JPEG decoder.** The entry was about a dependency and the dependency was settled
at v0.13 — `Engine::bakegraph` holds the node vocabulary and the document format
at `shared`, linking `Engine::core` and nothing else, while `bake` keeps every
importer and the evaluator. What was left was the wiring, and it is small
precisely because the split was the expensive half.

**What shipped.** `bake::PipelineSet` is the named collection a world holds —
several pipelines rather than one, for v0.11's "many node trees in one editor" —
with a set format that is the single-document format under `pipeline "name"`
lines, so the one is a strict substring of the other and there is one grammar for
a pipeline. `game` names `Engine::bakegraph`, `WriteWorldBody` emits
`<AssetPipelines>` beside `<Sources>` as one CDATA block, `ReadWorldBody` reads
it, and `RegisterGameClasses` registers the resource under `bake.PipelineSet`.
`expected_graph.json` carries the new edge into `game`, `client`, `server` and
`unified_server_client`; none of them names `bake`.

**Three details that were decided while doing it rather than before.**

- **The registration is in `game` rather than in the module that owns the type**,
  which inverts `scene::RegisterSceneComponents`' rule. `bakegraph` links `core`
  and nothing else, so `ecs::Components::Register` is out of its reach, and the
  alternative to that line is a link edge that undoes the split. Written down
  where the call is.
- **An unknown node kind refuses the pipelines and keeps the world, with a
  warning** — the render half's answer, and it matches for the render half's
  reason. The vocabulary is a closed list, so a kind from a newer build is
  indistinguishable from a typo; a world whose parts loaded and whose recipes did
  not is recoverable, and refusing the document loses a level over a bake chain
  somebody can re-author. The whole set goes rather than the readable half.
- **`FORMAT_VERSION` did not move.** A world with no pipelines writes no element,
  so existing content is written byte for byte as before, and an older build
  skips a child it does not recognise. A bump would make a readable file a
  refusal.

**Names sort by text and not by `core::Name::operator<`**, which orders by the
interning counter. First-seen order is a property of the process; a save file
that depended on it would stop being diffable and the round-trip test would stop
meaning anything. That is rule 4 arriving at a container rather than at a field.

**The correction this entry carried at v0.13 was that the block should wait for a
panel**, on the grounds that a save-format section with no producer and no
consumer is a feature that looks present and is not — `Editor::DrawAssetsPipeline`
went out with the render-pipeline revert and has not come back. It was written
anyway, because the half carrying the dependency risk is the wiring rather than
the widget: proved against a live round trip, a panel is later a UI change
against a format that already works, instead of a format change made beside one.
The panel itself is still the extended rendering pipeline's, in `ROADMAP.md`,
behind a prototype project.

### [x] D00115

**Replication filters by component and not by entity, so a character's limbs pay
wire for transforms that are overwritten the moment they land.** A character is
six parts and one of them moves: `scene::PoseCharacters` derives the other five
from the root, on whichever machine draws, precisely so that they cannot come
apart at speed. The offsets have to cross once. The *transforms* do not, and they
cross every tick anyway, because `replication::Authority::Replicate` names
`scene.Transform` for the whole world rather than for a set of rows.

**Measured shape rather than a measured number.** Five extra rows per character
per tick, each a ten-byte quantised `CFrame` — so about fifty bytes a character a
tick before framing, against roughly ten for the root alone. At thirty ticks and
twenty players that is on the order of thirty kilobytes a second of state the
receiver discards on arrival. Real, and not urgent: `replication::DistancePriority`
already bounds what a tick sends, so the cost lands as *other* rows arriving later
rather than as bandwidth nobody budgeted.

**Why it is not fixed by moving the limbs off `scene.`** They are `Part`
instances and authored content — they are in the save file, the explorer and the
properties panel — and a component prefix is how a module says what is shared.
Renaming to dodge the filter would make a character's parts a different kind of
thing from every other part.

**What it wants is a per-entity opt-out on the authority**, which is a change to
`Authority` rather than to `scene`: something like a `NotReplicated` tag the
delta pass skips rows for. That is a wire-format-adjacent decision and worth
making once, with a second consumer to check it against — a rag-doll, a particle
proxy, anything else derived on the receiver.

**Reopen trigger: a second thing derived on the receiver that also crosses, or a
bandwidth measurement that names limbs.**

**Closed at v0.15, and two of this entry's own assumptions were wrong.**

The first was that it needed a wire-format decision. It does not. A delta already
carries only the rows that changed, so a receiver cannot tell an omitted row from
an unchanged one — the filter is entirely on the sender and nothing about the
format moves. `Authority::SuppressWhenTagged(component, tag)` skips a slot's
delta rows for entities bearing `tag`, at the same point the interest check
already rejects a row, and before `offer` so a suppressed row consumes no
acknowledgement slot either.

The second was that it needed a new `NotReplicated` tag and a second consumer to
check that tag's shape against. It needed neither, because **the tag already
existed**: an entity carrying `scene.CharacterLimb` *is* an entity whose frame is
a product of its root and its rest offset, which is precisely the condition the
filter wants. A second thing derived on the receiver — a rag-doll, a particle
proxy — will want its own already-existing marker named the same way rather than
a shared opt-out invented ahead of it.

The tag reaches the authority as a **string**, which is what lets this cross the
tier boundary: `replication` is L12 and `scene` is L7, and `replication` neither
links it nor may. That is rule 4 doing the job it exists for, and the same way
`Replicate` has always named what it sends.

**Deltas only. The baseline still carries one copy**, and that is a property
rather than a gap: a newly admitted client holds the limb where the server last
put it, so its first frame is right before any derivation has run.

**Wired in the defaults table rather than at each host.** `server::Server` and
`studio::PlayLink` both build an authority by walking
`DefaultReplicatedComponents`, so the pairing lives on `ReplicatedComponent`
beside `Detection` — a filter applied at one of them would be a difference
between playing in the editor and playing for real, which is the hardest kind of
bug to see because both look correct alone.

### [x] D00114

**Closed at v0.15 by following assignments inside the buffer, and by making a
list say which kind it is.** The other option this entry offered — linking
`Luau.LanguageServer` — was refused on the grounds the entry itself gives: it
buys one of the two languages the feature was asked for, and it puts
`Luau::Frontend` behind a studio header. What follows is what was built and,
more usefully, what was deliberately not.

**What resolves is what the buffer writes down.** `Instance.new("Part")`,
`game:GetService("Lighting")` and the `OfClass`/`WhichIsA` pair carry the class
as a string literal, so reading one is reading rather than inferring. `:Clone()`
carries its receiver's class, because a clone of a `Part` is a `Part` whatever
else the file does. `local same = part` is one local under another name and is
followed. Only the **last** assignment before the caret counts: a local set from
`Instance.new` and then from something unreadable has stopped being a `Part`,
and keeping the older answer would be exactly the failure below.

**What is refused is `FindFirstChild` and `WaitForChild`.** Resolving them to
their receiver was the cheap version this entry described, and it is wrong: a
child of a `Model` is not a `Model`, so it would offer `Model`'s properties for
a `Part`. Those fall back to the union, and so does every other shape the reader
does not recognise — `.Parent`, a chain, a call with a call inside it, an
initializer spanning two lines.

**A guess presented as a fact was the thing to avoid, so neither list is now
presented without saying which it is.** A narrowed row's detail reads `bool on
Part`; a union row's reads `bool on some class`. The first claims "this class
has this" and the second says "one of these classes has this", which is the
distinction the entry said an author had no way to make. It lives in
`Completion::Detail` because that is the field the popup already draws, so it
arrives without the drawing code being told about it — and it labels the
**union** as well as the narrow, because a marker only on the narrowed rows is
one nobody can read who has never seen the other kind.

**Both languages, one rule.** Nothing followed here is Luau's: the two accessors
are read the same way, a trailing `;` is trimmed, and no declaration keyword is
looked at. `CompletionSources::Language` still decides methods against
properties and which keywords are offered, and nothing there changed.

The cases are in `mono.studio/tests/Complete.cpp`, including the two that must
**not** narrow: `FindFirstChild` on a `Model`, and a later assignment nobody can
read undoing an earlier one that could.

### [x] D00112

**Portals can be walked through; the frame you cross on still shows the wrong
side.** `NON-EUCLIDEAN.md` filed six rows of work at v0.14 and this entry
carried the last two. Traversal closed at v0.14; the seam did not.

**Traversal, which this entry said was blocked and now is not.** It was waiting
on a body to move — the character controller was `ROADMAP.md` v0.15 and arrived
early. `scene::CrossPortals` is the whole of it: the crossing is the segment
between `PreviousTransform` and `Transform`, tested for a change of side through
the pane's plane inside the pane's rectangle, and a body that crosses is
multiplied by `destination · half-turn · source⁻¹` — the *same* product
`AimSurfaceCameras` puts the camera through. The velocity is mapped by it too,
without which the body arrives aimed the way it was aimed in the frame it left
and walks out sideways.

**Two decisions inside it worth not relitigating.** The transform is derived
rather than read off `SurfaceLens::Mapping`, because that component is
presentation — fitted to the local eye, kept off the wire by
`replication::LocalToTheClient` — and a dedicated server never aims a surface at
all, so a traversal that read it would work in the editor and not on a server.
And a pane is crossed from **either** side, mapping through the crosser's own
face frame, for the same reason `AimSurfaceCameras` picks a side per *viewer*: a
hole is not a one-way door and both answers are right.

**The seam is what is left, and it is a rendering decision rather than a
feature.** A surface's texture is last frame's, which is what breaks the
dependency cycle between a surface and the scene it shows. On a mirror that is
invisible; on a hole somebody walks through, at 60 fps, on the frame they cross,
it is a visible seam. Removing it means rendering the portal chain inside the
frame, deepest first — the one property the surface pass trades away for being
cheap, and a change that belongs with the render-graph work `ROADMAP.md` files
behind a prototype project.

**Closed at v0.15, and the last row was much smaller than this entry says.**
The seam was that a surface samples the *other* surfaces from the textures they
held last frame. Removing it does not need a recursive pass with its own budget —
it needs the existing pass to run again: each bounce makes the previous one's
output the read side, and after `n` bounces a chain `n` deep is resolved inside
the frame, deepest first. The ping-pong pair each slot already carried is what
makes it safe. `Renderer::SetSurfaceBounces`, two by default, and `NON-EUCLIDEAN.md`
is the reading of CodeParade's demo that the work came out of.

**Part of what looked like the seam was not the seam, and is gone.** v0.15 found
that `EDGE_ON_MARGIN` — a mirror's fix for a camera that flips 180 degrees as a
viewer crosses its plane — was being applied to holes as well, so a portal drew
*nothing at all* within 0.3 studs of its own plane. That band is exactly where
somebody walking through spends the crossing. A linked portal has no
discontinuity to blank: the frame the viewer's side flips is the frame the
viewer is carried through the pane, and the two cancel. What is left of this
entry is the one-frame staleness above and nothing else. `NON-EUCLIDEAN.md` is the
reading of CodeParade's demo that turned it up.

**Two smaller things worth knowing before building on this.** Sixteen surfaces
per world, shared with mirrors, which is a reasonable limit for this kind of
level and not for a world dotted with holes. And physics has no per-region
filter, so two rooms genuinely occupying the same coordinates would share one
broadphase — `SurfaceCamera::TagFilter` already covers the rendering half.
Placing the regions apart and letting the portals lie avoids both, and is what
the demo does.

**What must not happen is a second renderer.** Anything beginning with a portal
pass of its own would be a copy of the surface pass under a different name, and
the two would drift on the first lighting change.

### [x] D00111

**An HTTP origin has no route that says what it holds.** v0.14 gave the assets
panel a tab per content source; a `Directory` source is listed by reading the
manifest in its `processed/` folder, and an `Http` one shows its address and a
sentence saying it serves by name. That sentence is true of the protocol rather
than of the panel: `delivery::AssetClient` fetches a manifest through the whole
priority list and reports what verified, and nothing asks *one* origin what it
has.

Two ways to close it, and the choice is the work:

- **A client per source.** `MakeAssetClient` over a one-entry `DeliverySettings`
  would answer honestly with no new protocol, at the cost of a client per tab
  with its own lifetime, its own pump and its own failure to report. Pumping
  those only while a tab is open is a state machine the panel does not have
  today.
- **A listing route on `cdn::Service`.** Cheaper to consume and a wider origin:
  anything an origin will enumerate is something an unauthenticated caller can
  enumerate, so it needs the ingest key's argument made again for reads.

**Closed at v0.15 with the listing route, and the key argument was made again
rather than reasoned around.**

`GET /catalogue` and `GET /catalogue/<cursor>` answer a page of the published
manifest — `asset <root> <kind> <bytes> <name>` a line, with the name last
because it is the only field with no bound on what is inside it. The cursor is a
decimal offset into the manifest's own name order, and every page carries the
publication root so a reader can tell a publish that swapped underneath it from a
list that changed. Paged because a manifest has no bound and a route that
serialised all of one would make a request's cost a property of somebody else's
content.

**It is off by default and admitted by `IngestSettings::Key`, which is the whole
of the security decision.** `ServiceSettings::Lists()` is "switched on, and a key
to admit it" — `IngestSettings::Accepts`'s both-or-neither rule applied to the
read side — so a half-configured origin reads as off. Disabled answers `404` as
though the route were not compiled in, for `IngestOf`'s reason: a `403` would
tell an unauthenticated caller that this build enumerates and only the key is
missing. There is deliberately no second secret and no second header; a key with
no `Inbox` is a read-only origin that enumerates, which is the useful shape.
`cdn --list-contents` refuses to start without `--ingest-key`.

**The panel says which kind of "cannot", and never guesses.** `studio::
OriginLister` is the seam: `ListingOutcome` has an entry for each of not asked,
no key here, unreachable, not offered, refused and unreadable, and `Describe`
gives each its own sentence. Entries are dropped with a failure — a half-listed
tab under a "could not list" note is the same confident wrongness as drawing the
live client's catalogue there.

Three assumptions worth writing down, because a reader will hit them:

- **`GET /manifest` was already open and already enumerates.** Anybody who can
  reach an origin can fetch the signed manifest and read every name out of it, so
  this route gates the *convenient* enumeration rather than closing a hole that
  was shut. It is still worth the flag — a text listing behind a key is a much
  smaller invitation than a parser nobody has to write — but "off by default"
  here does not mean an origin's names are secret. Making them secret is a
  separate decision about `/manifest`, and it is not this entry's.
- **The lister blocks, with a ceiling.** `MakeOriginLister` waits
  `MaximumPolls × PollMicroseconds` per page — a second by default — on the
  thread that called it. That is why `RefreshStoreContents` asks (panel opened,
  Refresh, import, publish: all somebody having asked) and `RebuildContentClients`
  deliberately does not, since it runs at start-up and on every Content-page save.
  The tab says "not asked yet" until a refresh. Growing a per-tab pump is exactly
  the state machine the rejected alternative wanted.
- **Enumeration is not authorisation.** A name in this listing is not fetchable
  by the caller: `/bundle/<root>` still needs a grant from the server, and the
  editor's own key is an operator secret rather than a player's. The listing says
  what an origin holds, not what anybody may have.

**What must not happen is the panel guessing.** Drawing the live client's
catalogue under a named origin's tab would attribute every name to whichever
origin the tab happened to be, and the first time two origins disagreed the
panel would be confidently wrong about where content came from.

### [x] D00110

**A library of default shaders needs a consumer, and there is not one.** v0.14
moved every built-in shader into `Engine::resources`, which is the half of that
roadmap item with a home to move to. The other half — "a variety of default
shaders" — would be GLSL that nothing loads: a shader reaches the GPU here only
by being named in `Renderer::Impl::CreatePipelines` or `InterfacePass`, and both
name a fixed set that matches the files one for one.

The two things that would give a variety somewhere to plug in are both filed
already and both above this in the stack:

- **`ShaderScript`.** Named in `render/ShaderCompiler.hpp` and in
  `render/AGENTS.md` as the reason a *runtime* compiler exists, and declared
  nowhere — there is no class, no property and no path from a world to a
  compile. Until there is, an author cannot select a shader by name at all.
- **The render graph as a node system**, which `ROADMAP.md` files under v0.??.
  That is where a pass gets to say which shader it wants, and where a permutation
  stops being a file somebody adds by hand.

**Adding the files first is the trap worth naming.** Six unlit/toon/water
fragments in `resources/shaders/` would compile, stage, pass every test and be
loaded by nothing — and each would then be a thing a later pipeline design has
to either adopt or explain. `resources/AGENTS.md` says the same rule for meshes
and textures: a default that can be generated should not be a file, and a
default nothing consumes should not exist yet.

**Closed at v0.15 by building the consumer first and the library second, which
is the order this entry insisted on and is the only part of it that survived
unchanged.** Two shaders were added — `unlit.frag` and `toon.frag` — and neither
is reachable except by name, because the name is what loads them.

**`ShaderScript` exists now**, and it is what the rest hangs off:
`scene::ShaderSource` holds GLSL and a revision, `Instance.new("ShaderScript")`
resolves, the explorer offers it and the properties panel edits its `Source`.
The revision moves on write and only on write — `scene::SetShaderSource` is the
one writer — which is what turns "has this changed" into an integer compare
rather than a hash of every shader in the world every frame.

**Fragment stage only, and that was not a simplification made to save work.** A
vertex shader has to agree with `GpuInstance`, which `render/AGENTS.md` says is
private and stays private, so an author able to supply one would be authoring
against a struct nobody promised to keep. The fragment stage's interface —
`opaque.frag`'s four samplers and three uniform buffers — is stated instead.

**The selection is `Material.Shader`**, a name on `MaterialRef`, carried onto the
part's `SurfaceAppearance` by `ResolveMaterials` in the same pass and by the same
argument as the texture maps, then onto `DrawInstance::Shader` and into the
renderer's slot arrays. Every hop of that chain already existed for `ColourMap`;
what is new is one more field at each.

**`render::ShaderLibrary` is the route, and its resolution order is the reason
the two default files are not the trap this entry named.** One name resolves
three ways: a `ShaderScript` in the world, compiled here and now; failing that a
built-in `glslc` produced during the build; failing that a diagnostic saying so.
`BuiltInShaderNames` is the list, so a `.frag` added to `resources/shaders/` and
not added to that list is a file nothing can name — which is a review question
rather than a thing somebody notices a release later.

**Two shaders and not six.** One proves a name resolves; two prove the
*selection* does, because a scene with an unlit sign and a toon character binds
two pipelines in one frame. A third would prove nothing further, and a
permutation added by hand is what the render graph exists to stop.

**Three things this entry asserted were wrong, and one of them mattered.**

- `Renderer::AddShader` did **not** already take SPIR-V bytes. It did not exist;
  the only occurrences of the name were the two comments that referred to it,
  including the `TODO(render-pipeline)` marker in `mono.studio/src/Network.cpp`.
  It exists now — it builds a fragment shader object and the opaque and
  transparent pipelines a variant is drawn through.
- "A shader reaches the GPU only by being named in `CreatePipelines` or
  `InterfacePass`" was true and is no longer the whole story: `DrawSlots` binds a
  variant per run, breaking a run where the shader name changes exactly as it
  already broke where the mesh or the seam plane did. A world where nothing
  selects a shader holds one invalid name throughout and pays one compare.
- The entry treated "the render graph as a node system" as a prerequisite. It is
  not one for *selection* — a name on a draw instance and a table of
  substitutions is enough. It remains the prerequisite for a **permutation**,
  which is a different thing and is still filed.

**What is deliberately not here, so the next person does not go looking.**

- **No variant for the shadow pass.** It writes depth and no colour, so a
  fragment shader there would cost a pass over the whole scene to produce
  nothing. `PipelineFamily::Other` is what says so in code.
- **No compute or vertex `ShaderScript`**, per the stage argument above.
- **Published shader *assets* still do not reach the renderer.** The
  `TODO(render-pipeline)` in `mono.studio/src/Network.cpp` is unchanged: what
  `AssetKind::Shader` delivers is a module a *node* would name, and which node
  is the graph's question. `AddShader` is now the call it would make.
- **No test of the pipeline itself**, because there is no device in a test.
  `render/AGENTS.md` is explicit about that, so what is covered is everything
  short of it: the class, the property, the resolve, the name resolution and the
  compiler — `mono.engine/scene/tests/Shaders.cpp` and
  `mono.engine/render/tests/ShaderLibrary.cpp`.

### [x] D00108

**Closed at v0.13 by `studio::EditStream`.** What follows is the entry as it
was written, kept because the reasoning is what the answer was built against.

The identity turned out to be an instance path rather than a new field in the
document format: two logs both issue `1` for their first instance, so an
`EditId` cannot cross, and a path is the one name two editors already share.
Ordering comes from the host relaying, which is also what keeps paths resolving
to the same instance everywhere. What the entry says about locking still holds
and is why there is none.

**Team create finds people and cannot yet let them edit together.**

`studio::TeamCreate` is the session layer and it is complete: an editor
announces itself at `Purpose::Studio`, sees the others on the subnet or through
a rendezvous point, and hands over a session id and a key to invite somebody
with. What it does not have is anywhere for two editors to put a change.

**Why the obvious answer is the wrong one.** `replication::Authority` already
orders changes to a world and streams them to clients, and pointing two editors
at it looks like a morning's work. It is not the same problem. An authority has
one writer and many readers; a shared document has many writers, and the
question it has to answer — what happens when two people move the same part in
the same beat — has no answer in a model built around a server that is right by
definition. Bolting one on would produce an editor where the last packet wins
and somebody's work disappears without a message.

**Why it is not fixed by locking.** Per-instance locks are the cheap version and
they fail in the ordinary case rather than the rare one: two people laying out
one model touch the same parts constantly, and a lock that has to be waited for
turns collaboration into taking turns.

**What closing it takes.** A change model with a total order that neither editor
owns, an undo stack per person that survives somebody else's edit landing in the
middle of it, and a policy for the conflicts the order does not resolve. That is
a design, not a patch, and it should be written down before any of it is typed.

Until then: the panel says "sessions only" in the window rather than in a
comment, because a person looking at a list of editors they cannot collaborate
with should be told why by the thing they are looking at.

### [CLOSED] D00105

**A plugin can change the world and cannot add a button, and the missing piece
is a channel rather than a function.**

**Closed at v0.12 by building the channel.** `script::HostSurface` is the seam —
one virtual taking a name and a `HostValue` list — and `script::HostValue` is a
value tree rather than `ScriptValue` widened, for the reason this entry
predicted: an instance handle means something inside one process and nothing on
a bus, so the two types stay apart. A Luau function passed as an argument
becomes a `HostCallback`, which is what makes a button's handler possible, and
`Runtime::Invoke` is the other direction. `mono.studio/src/PluginSurface.cpp` is
the editor's implementation and `engine.script.host` covers the crossing.

Two things the entry got right and one it did not. The value tree and the
callback were both needed, as predicted. What it called "the honest options" —
a polled queue or a registry-ref dispatch — turned out to be one option: the ref
lives in the module behind `Invoke`, so the host holds an id and polls nothing.

The original text follows.

`studio::PluginHost` runs a plugin as an ordinary `script::Runtime` against the
world an author is editing, so everything a game script can reach it can reach —
`Instance`, `workspace`, and since v0.12 `World`, the ECS underneath. The
selection needed no new surface at all: it is published as `studio.Selected`, a
described component, so a plugin queries for it and writes it back.

What that model cannot express is anything about the *editor* rather than about
the world: a toolbar button, a menu item, a docked panel, running a registered
command.

- **The obstacle is `script/AGENTS.md`'s first rule.** No `lua_State` appears in
  a public header, so the studio cannot install a global of its own into a
  plugin's VM the way `LuauEcs.cpp` installs `World` — that file is inside the
  module and `mono.studio` is not.
- **The shape that fits is a host-call seam, and the codec is already the hard
  half.** `script::ScriptValue` is the language-neutral tree both runtimes
  marshal through, with a deterministic encoding and a sorted map order; a
  `HostSurface` interface taking a name and a `ScriptValue` and answering one
  would give a host an arbitrary API without either side naming the other's
  types. What it costs is making `ScriptValue` public, which is a real widening
  of `script`'s surface and should be decided rather than slipped in.
- **A button also needs a callback going the other way**, which is the part the
  value tree does not answer: a handler lives in the plugin's VM and the press
  happens in the editor's frame. The honest options are a polled queue — the
  plugin asks "was I clicked" on its heartbeat — or a registry-ref dispatch
  inside the module, and the first is buildable today on top of the seam while
  the second is not.
- **Until then the absence is stated rather than discovered.**
  `studio/Plugins.hpp` says there is no toolbar API and why, which is the thing
  that keeps somebody from looking for one.

**Reopen trigger: the first plugin that wants to be invoked rather than to
run every frame.** A tool that aligns a selection is one — it should happen when
somebody asks, not sixty times a second.

### [CLOSED] D00047

**Readbacks: the viewer node's image, channel histograms, and an overdraw view.**

`PIPELINE_NODES.md` stage 8, and the three faults its §1.5 cannot reach without
a path off the GPU.

- **Faults 3, 4 and 9 all need the same thing.** Is this alpha channel blank; is
  this whole target uniform; how many times was this pixel shaded. Each is a
  reduction over a rendered target, and none is answerable from a declaration —
  `PipelineDiagnostics::UnusedAlpha` gets as close as a declaration can, which
  is "nothing is *arranged* to read it".
- **The `viewer` node is already in the catalogue** and does nothing, because
  showing what is on a wire means reading a target back and putting it
  somewhere. That is the same machinery, and it is the cheapest first user of it.
- SDL_GPU has the download path; what is missing is the fence discipline, because
  a readback that waits is a stall and a readback that does not is a frame late.
  A frame late is fine for a debug view and is worth saying out loud.
- **The arithmetic half is built** — `engine.render.readback`, with no device in
  it. `render::Histogram` answers faults 3 and 4 (`Constant`, `Blank`,
  `ImageHistogram::Uniform`), and `render::PendingReadback` is the fence policy:
  one download in flight, never stall, and report the age *from the request*
  rather than from the fence. Eight cases, four mutations checked red.
- What is left is the device half — a transfer buffer that outlives the frame and
  `SDL_QueryGPUFence` polled on a later one; the `viewer` node, which is one blit
  and the cheapest first user of all this; and overdraw, which is the odd one out
  because it needs a pass that *counts* rather than shades — additive blend into
  an `R8`, no depth write, its own pipeline and shader.

### [CLOSED] D00045

**Closed at v0.11: `NodeScope` shipped with three values.** The open question the
last bullet leaves — three scopes or four — was answered as it predicted.
`Frame`, `World` and `View` exist; `Surface` does not, because nothing schedules
a per-surface block and a fourth value would have been a word in the type with no
block to run in. `RenderGraph::Execute` runs the `World` band once per distinct
world and `Renderer::FrameRunner` prepares that world's first view before it,
which is what gave `World` something real to attach to.

**`Node::PerView` is gone**, not widened: the field is `Scope` and `Compile`'s
partition turns on a predicate rather than a boolean.

**`PIPELINE_NODES.md` stage 5's remaining half, as it was written:**

- A boolean says per-view or not. What a frame needs is **once per frame, once
  per world, once per view, once per surface** — and the multi-view seam already
  needs three of those. A shadow map is per world and is currently spelled "not
  per view", which is true and is not what it means.
- **`Compile`'s partition is the consumer.** Frame and World fall into the
  shared blocks, View and Surface into the per-view one, which maps exactly onto
  what the boolean does today — so this is a widening rather than a redesign.
- **The size estimate was wrong and is corrected here.** "85 references across
  18 files" counted `Band::PerView` and `CompiledGraph::PerView`, which are
  different symbols entirely. The real edit sites are around twenty-five: one
  field on `Node`, one on `Edit`, one on `NodeKindSpec`, `Compile`'s partition,
  and the designated initialisers in `StandardGraph` and the suites. The
  catalogue's table can keep a private `bool` in its own local row struct and map
  it at registration, so its forty-five rows do not change.
- **What actually blocks it is a design question, not the size.** Of the four
  scopes, `Frame` and `View` are what the boolean already says, and `World` has
  something real to attach to — `RenderGraph::Execute` runs the shared block once
  per distinct world. **`Surface` has nothing.** Nothing runs a per-surface
  block; the surface pass loops inside a per-view one. So a four-value enum would
  ship one value that is a word with no behaviour, which is the shape rule 6 is
  about.
- So the open question is whether this lands as **three** scopes now — `Frame`,
  `World`, `View` — with `Surface` waiting for a block to run in, or as four with
  one of them inert. Three is almost certainly right and it is not a call to make
  in the last few minutes of a session.

### [CLOSED] D00044

**`PropertyDescriptor::Writes` has no consumer. It is a declared constraint the
build does not check and nothing reads.**

**Filed on a false premise and closed with the real one.** `Writes` *is*
consumed: `mono.tools/bindings` emits it as the `writes` array of every property
row in `manifest.json`, a checked-in file both declaration files are generated
beside. The search that filed this covered `mono.engine`, `mono.client`,
`mono.server` and `mono.studio` and did not cover `mono.tools`.

So the field is not dead — it is *published*, which makes a wrong one worse
than the entry claimed rather than harmless. What was actually wrong:

- **Four read-only properties declared a write set.**
  `Attachment.WorldCFrame` and `WorldPosition`, `Humanoid.Grounded` and
  `MeshPart.TrianglesCount` each wrote `Writes = property.Reads` beside
  `Writable = false`. The manifest said so too, in the file, for two versions —
  telling every script author that setting them moves storage they cannot even
  be given a value for. `gui::ResolvedField` had the right shape the whole
  time; `scene` was the outlier.
- **Fixed** by declaring the empty set, and the manifest regenerated: four rows,
  no other drift.
- **And made unrepeatable.** `bindings` now walks the whole class table before
  it writes or compares, and refuses two contradictions: a read-only property
  that declares writes, and a writable one that declares none. It runs under
  `just check`, so a descriptor that drifts fails the build. That check is in
  the bindings tool rather than a suite because it is the only binary that
  registers `scene`, `script`, `effects`, `gui` and the services together.
- **An empty `reads` set is deliberately not a fault.** `Players.LocalPlayer`
  has one and is right to: it projects a world resource, not a component on the
  row. The consequence — such a property can never fire `.Changed` — is a
  limitation its own comment states, not a contradiction.

### [CLOSED] D00043

**A derived property whose getter walks to another entity cannot signal
`.Changed` when that entity moves.**

Closed by making the resolve pass report its own write, which is a smaller fix
than the entry expected and does not need a cross-entity dependency at all.

- **The real cause was one layer down and worse than filed.**
  `ResolveAttachments` wrote `WorldFrame` through the reference `Store::Each`
  hands out — a direct memory write, which the store does not report. So
  `Attachment.WorldCFrame` and `WorldPosition` could not fire `.Changed` for
  *any* reason, including the attachment's own `CFrame` being set. The entry
  read it as a limitation of per-entity delivery; delivery was never reached.
- **The fix**: the pass gathers what actually moved, then writes those rows
  through `Store::GetMutable` — the call that reports a write. `ChangeQueue`
  already subscribes to `Attachment` through `Reads`, so the signal now arrives
  on the attachment's own row, which is where the per-entity filter wants it.
- **Only the rows that moved**, which is the half that makes it safe to run
  every frame in two phases: reporting unconditionally would advance the world's
  change counter for ever and falsify `physics`'s static broadphase gate and
  `gui`'s compile gate. `engine.scene.attachments` asserts both directions —
  the parent moving signals, and a second pass over a still world signals
  nothing and leaves `ChangeVersion` where it was.

### [CLOSED] D00042

**`Scheduler` says registration order within a phase means nothing. Four systems
in `PreRender` depend on it.**

Closed by declaring the contract the scheduler already keeps, rather than
building a dependency sort for something insertion order expresses exactly.

- `Scheduler::RunPhases` walks its vector in insertion order, and every host in
  the repository relies on it: a pass that derives something is registered ahead
  of the pass that reads it, and `client::InstallPresentation` says so in its
  own comments. A rule the code breaks everywhere is not a rule, it is a trap —
  it tells a reader the ordering they can see is accidental.
- **The alternative was a declared dependency between systems**, which would be
  a second mechanism saying what registration already says, and would have to be
  written out at every one of those call sites to mean the same thing.
- The header now states both halves, and `ecs.scheduler` holds them: systems in
  one phase run in the order they were added, over two ticks so a scheduler that
  rebuilt its list could not pass; and a phase boundary still outranks
  registration, so the new sentence cannot be read as "registration order is
  *the* contract".

### [CLOSED] D00041

**The node canvas is a `gui` tree and the studio's panels are Dear ImGui. They
do not compose, and the mounting work has to answer this first.**

Closed by taking the second of the two options this entry laid out: the canvas
is drawn with an ImGui draw list, and `nodeview`'s `gui`-tree half is deleted.

- **The decision was forced by what "a full editor" needs, not by taste.** A
  read-only diagram could have gone either way. Adding a node, dragging a wire
  and refusing an incompatible drop all need per-frame input against per-frame
  geometry, and routing that through a retained tree rendered into an offscreen
  target — one per open editor — buys a widget set the panel does not otherwise
  use and costs a texture, a pass and a coordinate hop on every click.
- **What replaced it is testable in the same places.** `nodeview::Editor` holds
  the hit-test, the drop rule, the zoom-about-a-point arithmetic and the menu's
  search; `engine.nodeview.editor` asserts all of it. Only the drawing is in
  `mono.studio/src/Pipelines.cpp`, which is the part no test could have reached
  under either option.
- **Deleted rather than left dead**: `nodeview::Canvas`, `nodeview::Build`,
  `BuildAssets`, `CanvasStyle`, `Pick`, `PickAt` and `Click`, with their tests
  and the internal `Widgets.hpp`. Keeping a second, unused way to draw a node
  canvas is exactly the two-ways-to-do-one-job that `AGENTS.md` names as the
  most expensive debt in a monorepo, and it would have been the copy nobody
  noticed had rotted.
- **`CanvasState`'s pan survives** because the Assets Pipeline canvas is still a
  read-only diagram and still scrolls. It moves onto the same seam when that
  panel becomes an editor.

### [CLOSED] D00040

**`Node::PerView = false` is shared *per world*, and the graph has no idea what
a world is.**

**Closed at v0.11.** `Execute` takes one world identifier per view and runs the
shared block once per *distinct* world, with that world's views immediately
after it. `render::View::World` carries the number — an opaque `uint64_t` and
deliberately not a `world::WorldId`, since `render` is L12 and a world's
identifier is L4's; what the partition needs is only whether two views are of
the same world. The view-count overload is kept and means "every view in one
world", which is what a game and a single-panel editor both are.

Two views of one world run one shadow pass; two views of two worlds run two.
Views of one world need not be adjacent, because a studio's panels are in panel
order rather than world order. The grouping — a world's shared work, then that
world's views — is the load-bearing half: every world's shared block first would
have the second world's shadow pass overwrite the first's before the first's
views had sampled it.

Found while wiring `Renderer::Render` onto `graph::RenderGraph` — the §4.3
executor — and it is the thing that has to be settled before that work is worth
starting.

- **The shadow node is shared because every view of one world samples one map.**
  That is the claim v0.11 is built on and it is true, with the qualifier the
  graph does not carry: *of one world*. `Compile` partitions into shared and
  per-view and nothing in it names a world, so "shared" currently means "once
  per frame".
- **A frame may hold views of different worlds.** That is the roadmap line this
  version exists for, and the studio's second panel already *defaults* to a
  different world. Two such views need two shadow maps, fitted to two sets of
  bounds. Running one shared shadow pass for the frame would light one world's
  geometry with the other's sun fit.
- **It is not a bug today because nothing passes more than one view** (`D00038`),
  and a single-view frame has exactly one world in it. It becomes one on the
  first frame that does.
- **The renderer's shape says the same thing from the other side.** Everything
  the shadow pass needs — the union bound, the light matrix, the scene order —
  is derived inside the per-view setup from *that view's* draw list. Hoisting
  the pass without hoisting its inputs is not possible, and its inputs are only
  hoistable across views that share a draw list.
- **The likely answer is that a `View` names its world** — an opaque id the
  caller sets, not a pointer and not a `world::WorldId`, since `render` is L12
  and may not reach for one — and `Execute` runs the shared block once per
  distinct id rather than once per frame. That makes the partition mean what its
  comment already claims. The alternative is one compiled graph per world, which
  is cheaper to reason about and pays a compile per world rather than a
  partition per frame.
- **Promoted to a roadmap line at v0.11**, "the render pipeline is per world,
  not per process", because it is a change to what a pipeline *is* rather than a
  defect in one — `StandardGraph()` is a free function returning one frame for
  the whole process, and a universe holds several worlds.
- Until then `render/benchmarks/Frame.cpp` is the honest measure of what the
  partition is worth, and it says the shareable thing is the shadow *pass*
  rather than the shadow *fit*.

### [CLOSED] D00037

**`BENCH`'s iteration count means two different things in two files, and a row
does not say which.**

**Closed at v0.11.** The report carries the unit. `BenchCase` grew a
`BenchUnit` — `Call` when the body loops `Iterations` times, `Item` when it runs
once over `Iterations` things — and the line is now
`bench<TAB>suite<TAB>ns<TAB>spread<TAB>samples<TAB>iterations<TAB>unit<TAB>name`.
`graph`'s rows read `call` and `scene`'s read `item`, so two figures four orders
of magnitude apart are no longer silently comparable.

- **A second macro rather than a parameter.** `BENCH_PER_ITEM` declares the
  normalising form; `BENCH` keeps its signature and its meaning, so no existing
  benchmark changed except `scene/benchmarks/Ordering.cpp`, which was already
  using the divisor that way and now says so.
- **The unit goes before the name, not after it.** The name is free text
  flattened onto one line, so anything past it cannot be found by counting tabs.
- **The baseline migration this entry warned about cost nothing**, because
  `.cache/bench-baseline.tsv` is git-ignored — there was no committed baseline
  to migrate. Checked rather than assumed; the runner re-measured all 401 rows
  against the new format.

- `BenchMain::Sample` calls the body **once** and divides the elapsed time by the
  declared `Iterations`. So the count is a divisor the author promises the body
  honours, and `Bench.hpp` states that contract: *"Runs the body `Iterations`
  times."*
- **`graph/benchmarks/Cull.cpp` keeps the promise** — it writes
  `for (pass = 0; pass < 200000; pass++)` inside the body — and its rows are
  nanoseconds per call.
- **`scene/benchmarks/Ordering.cpp` does not, deliberately.** Its bodies run once
  and pass the *instance count* as the divisor, so its rows are nanoseconds per
  **instance**: *"One iteration is one instance, so every row divides into a
  per-instance cost."* That is a reasonable thing to want and the header says so.
- **Nothing in the report distinguishes them.** Both emit the same six tab-
  separated columns, so `OrderScene · 10k instances = 1` and `Cull 1000, all
  visible = 17207` sit in one output looking comparable and are off by four
  orders of magnitude from each other's unit.
- Found while writing `render/benchmarks/Frame.cpp`, which made the third
  mistake available: a body that runs once while declaring 200, reporting a
  frame at 739 ns. It was caught only because `graph`'s table gave a number to
  contradict it — 5000 instances cannot record in less time than one of them
  culls.
- **Not fixed here because the fix is a decision, not a patch.** Either
  `BenchCase` grows a unit field the report carries, or the per-instance
  normalisation is spelled differently — a `PER_ITEM` variant of the macro — so
  the divisor always means the same thing. `bench-accept` compares rows against a
  stored baseline, so whichever is chosen has to migrate the baseline in the same
  commit.

### [CLOSED] D00036

**307 public entities carried no comment, and nothing had ever been able to say so.**

- `just docs-check` runs two Doxygen passes. The first is the site, and it fails
  the recipe on malformed comments and dangling links; the second is the
  **coverage** pass, `EXTRACT_ALL = NO`, whose whole job is to report a public
  entity nobody documented. **The second had never completed**, because the first
  had been red for at least two versions and the recipe stops at it.
- **Found by fixing the thing in front of it**, which is `D00035`. The moment
  `warnings.txt` reached zero, `gaps.txt` reported **307** — and this is the
  third time this repository has recorded that cascade: `ROADMAP.md` v0.2 for
  `docs-check` itself, `D00005` for `just preset=ci check`, and now one level in.
- **All 307 were genuinely public**, checked rather than assumed: the coverage
  pass leaves `EXTRACT_PRIVATE` off, so none was a private detail that slipped
  in.
- **About a third were unattached rather than unwritten**, and one setting closed
  them. `ecs::AttributeValue` has fifteen payload fields that are one idea —
  a per-field comment could only ever have said "the `float` case" — and
  `engine::gui` declares nineteen `Describe` overloads under one paragraph
  explicitly about all nineteen. Doxygen attaches a comment to the declaration
  beneath it, so the other fourteen and eighteen counted as gaps.
  `DISTRIBUTE_GROUP_DOC = YES` plus `//@{` markers documents each family once.
  **Not a lowered bar**: the author still writes the comment and still marks the
  group by hand. What it stops is documentation written to satisfy a check.
- **The rest was writing, and the useful ones were where a name hides a trap** —
  `Delta::Baseline` and why a lost datagram is survivable, `Structure`'s three
  lists and why `Forgotten` is never merged with `Destroyed`,
  `Statistics::Refused` against `Deferred` (the link saying no against this
  module saying later, which this file already records people confusing twice),
  and `Answer::PublicKey` being repeated rather than remembered, which is what
  makes the challenge stateless.
- **Two wrong turns, both recorded rather than tidied away.** A filter rule was
  added so `//@{` was *not* promoted to `///@{`, on the reasoning that a
  delimiter is not prose — wrong for this pipeline, since the promoted form is
  the one Doxygen groups on. Worse, while that rule was in place it produced the
  measurement behind a confident claim in this entry that namespace-level
  overloads **cannot** be grouped. They can. **A tool change made mid-
  investigation invalidated the measurement being taken through it**, because the
  filter was both the instrument and the subject. The rule was reverted and left
  out, having no user.
- **It also turned up three more orphaned doc blocks** of the kind `D00035`
  found two of — a new member's comment inserted *inside* an existing one, so
  `RequestShownContent` and `PublishManifestNames` were undocumented while their
  prose sat on `FitPartsToMesh`. All three were added by the v0.10 mesh work.
- **`just docs-check` exits 0 and says "every public entity is documented".**
  It is now a check that can fail for a real reason, which it has not been able
  to do for two versions.

### [CLOSED] D00035

**`just docs-check` was red, and the thing it was red about was not the thing worth finding.**

- Found by running it rather than by reading it: the recipe fails when
  `warnings.txt` is non-empty, and it held **19 lines**. One was a genuinely
  broken link — `README.md` pointing at `docs/CPP_LINKER.md` after that file
  moved to `docs/retired/` — and the other eighteen were Doxygen's Markdown
  disagreeing with prose it was handed.
- **The link half was larger than the one warning showed.** The same move left
  **26 stale `docs/<name>.md` references across 20 files**, almost all in source
  comments where nothing checks them — `docs-check` only resolves links
  reachable from the documented surface. Two more dangling links were found by
  sweeping every local Markdown link directly, including one *inside*
  `docs/retired/v07v08.md` that broke by being moved beside the siblings it
  names.
- **The count moved the wrong way first, and that was the tell.** 19 to start;
  fixing the mainpage link took it to **26**, because the site pass had been
  stopping at that link and the seven it then reported had been invisible behind
  it — including two live source defects, where a new member's doc block had
  been inserted *inside* an existing one, leaving `bake::Graph::AddWrite` and
  `render::Renderer::TextureHandle` undocumented while their prose sat on the
  wrong function.
- **The eighteen had one cause, and it was not the one this entry first named.**
  `JAVADOC_AUTOBRIEF` ends the brief at the first sentence-ending stop and does
  not care that the stop is inside emphasis. This repository's house style is a
  bold *sentence* — `**Twenty-eight and not thirty-two.**` — so the split lands
  between the `**` and its partner: the brief ends holding an unclosed emphasis
  and the detail starts with a stranded closer. Doxygen reports it **against the
  following comment block**, which is why the warning never points at the
  comment that caused it.
- **The wrong answer was held for an afternoon and is recorded rather than
  quietly dropped.** The first minimal reproduction kept the stop inside the
  bold while dashes, quotes and line wrapping were varied around it — so every
  variant failed and *wrapping* took the blame. That conclusion was written into
  this entry as confirmed, with a measurement beside it (355 multi-line bolds
  across 145 headers) that made it look substantiated. **Emphasis spanning a
  line break is completely fine.** Moving the stop out fixes a bold spanning
  three lines; leaving it in breaks one that fits on half of one. The lesson is
  the old one: varying everything except the cause proves the cause is
  everything else.
- **Fixed in the filter, in one character.** `docgen::Promote` moves a trailing
  stop from inside the emphasis to outside it — `**Sentence.**` becomes
  `**Sentence**.` — which preserves the line count that every source link on the
  generated page depends on, keeps `JAVADOC_AUTOBRIEF`, and leaves the house
  style alone. An unpaired `**` leaves its block untouched, and markers inside a
  code span are code. Seven cases in `docgen/tests/Filter.cpp`.
- **The last one was in Markdown rather than in a comment, and pages are not
  filtered.** A code span containing apostrophes —
  `` `Unknown type 'Enum.Material'` `` — left an unmatched `</tt>`, reported
  against a line four bullets further down. It resisted isolation because the
  line, the pair and the section all rendered clean on their own; what found it
  was neutralising each of that line's sixteen code spans in turn.
- **`DOT_GRAPH_MAX_NODES` is 64 now**, up from Doxygen's default of 50, which
  `studio::Editor` passes with 55 collaborators — and passing it draws no graph
  at all and warns, so the default gave the one class whose relationships are
  hardest to hold in a head the one page with no picture.
- **Closed with `warnings.txt` at zero, and what that revealed is `D00036`.**
  The coverage pass behind it had never run to completion and reports 307
  documentation gaps. That is a separate entry because it is separate work.

### [CLOSED] D00034

**One asset baked and did not publish, and nothing in the pipeline would say which.**

- v0.10's store re-baked to **1974 raw, 1973 baked**. The gap was real, was never
  identified, and was only visible by *subtracting two numbers printed by two
  different tools* — which is why it survived a whole version.
- **Found, and it is not where this entry looked.** The entry assumed a baker
  refusing something and throwing the reason away. It was neither the baker nor
  the publisher: `cdn::ImportFile` accepted a **zero-byte file**. Found by
  diffing the two folders by hash, which left exactly one — `af1349b9…`, BLAKE3's
  empty-input digest — and then reading the content log, which named
  `blender-dragon/.venv/.lock`: a Python virtualenv lock file swept along by the
  folder import of a model.
- **The diagnosis this entry made was right and the location was wrong, which is
  worth keeping.** "The reporting is the actual defect, not the missing asset"
  held exactly — but the missing report was three stages upstream of where it
  was looked for. `cdn::Publish` *already* names what it skips, including an
  empty file; the trouble is that by the time it says so the file has been in
  `raw/` for good, and `raw/` is the folder the counts are taken from.
- **Closed by refusing it where refusing is free.** `ImportFile` now rejects an
  empty file and names it: it can never bake and never publish, so accepting one
  is guaranteed to produce a store whose totals disagree. Nothing is written and
  nothing is logged, because a refusal that still left the file behind would
  move the silence rather than remove it.
- **Not deleted from this repository's own store**, which still holds the
  zero-byte file: that is somebody's content directory and not this change's to
  edit. The counts there stay 1974/1973 until it is removed by hand.
- Pinned by a case that imports an empty file, requires the refusal, and then
  requires `raw/` and the log to be empty. Demonstrated by mutation.

### [CLOSED] D00033

**A mesh had no cached thumbnail, so a picker row showed a glyph until it was hovered.**

- `PaintPreview` rendered the *hovered* row into the studio's one preview slot —
  built-ins included — and every other row showed a letter. The rendering worked;
  what was missing was retention.
- **The blocker was in `render` rather than in `mono.studio`, and that reading
  held.** A cached thumbnail means keeping a scene target past the frame, and the
  renderer exposed no copy: the studio could ask for a slot to be drawn and could
  not ask for the result to be kept.
- **Closed at v0.10 by `Renderer::CaptureSceneTexture`**, a device-to-device blit
  from a slot into a new texture, published into `render::TextureTable` through a
  new `Adopt`. Nothing goes through the host: reading a picture back to the CPU
  only to upload it again would be a round trip across the bus for bytes that
  never needed to leave the device.
- **`Adopt` transfers ownership, and that is the whole contract.** The table
  releases the texture on `Drop`, on replacement and on `Shutdown` exactly as it
  does for one it uploaded — so every refusal in `CaptureSceneTexture` has to
  leave the caller still owning it, and a full table returns `false` *before*
  releasing whatever was under the name rather than after.
- **The drawn rectangle is copied, not the allocation.** A scene target is
  rounded up to 64-pixel blocks with hysteresis, so most of it is border the pass
  never wrote — copying it whole would keep a picture with an unwritten margin
  down two edges and force every consumer to carry `SceneTextureExtent` beside
  the handle, which is the coupling this ends.
- **It lands in the existing thumbnail cache rather than beside it**, which is
  what makes eviction work without knowing captures exist: `PumpThumbnails` drops
  the least recently drawn by calling `DropTexture` on exactly the name a capture
  publishes under. A second cache would have been a second thing to evict, and
  the one nobody wrote a policy for. Captured once per asset, not once per frame
  — the preview turns, and the frozen angle a thumbnail wants is any of them.
- **What is tested is every refusal and none of the success.** A capture needs a
  device; standing a fake in front of that would test the fake. Three cases pin
  the paths that decide whether a caller has just been handed a texture it now
  owns — no device, an invalid name, a slot never drawn into. The drawn path is
  verified by running the editor.
- The prediction in this entry's old reopen trigger was half right: the material
  preview did arrive and did want the same mechanism, but it shipped *before*
  this using the live slot, so the trigger never fired on its own.

### [CLOSED] D00032

**Deleting a `Material` instance left the texture it last resolved on the part.**

- `scene::ResolveMaterials` walks `MaterialRef` rows and writes the resolved texture into the parent's `SurfaceAppearance::ColourMap`. A part that no longer has a `Material` child has no row, so nothing visits it and the last resolved name stays — the part goes on drawing a texture nothing in the tree names any more.
- **The three states that do work are the ones that matter day to day**, which is why this shipped rather than blocking: no material at all leaves an authored `BasePart.ColorMap` alone; a material naming an asset resolves to it; and a material set back to `None` *clears* the part, because the pass writes even when it resolves to nothing. Only the deletion is stale, and only until something writes the field again.
- **The obvious fix is the wrong trade by two orders of magnitude.** Visiting every part every tick to ask whether it still has a material child is a child scan per drawable per tick, on the loop `client::CollectInstances` exists to keep flat, to correct an editor-time action.
- ~~**What would close it is a destruction hook**, which is the shape `ecs::Store::DestroyEntity` already carries for attributes.~~ **Closed at v0.10, and not that way — the hook would have been both harder and weaker.**
- **Harder, because the shape this entry pointed at is not a hook.** `DropAttributes` is a *hard-coded call* inside `DestroyEntity`, not a registration anything can add to, and `scene` sits above `ecs` — so closing this as described meant first inventing a general destruction hook in the storage layer, for one caller.
- **Weaker, because destruction is only one of the ways this goes wrong.** A `Material` **reparented** to a different part destroys nothing, and leaves the part it left holding the old texture for ever; so does a `MaterialRef` removed from a living instance, and a material moved under something with no `SurfaceAppearance`. A hook on the row leaving catches none of those three. This entry named the symptom it had seen and then wrote the fix for exactly that symptom, which is the failure worth keeping: **the trigger was "deleted", the defect was "stopped being written".**
- **What shipped is a difference between two passes.** `ResolveMaterials` records the parents it wrote in `MaterialCatalogue::Resolved`, and the next pass clears any parent in that record it did not write again. It is O(materials), not O(parts) — so the trade this entry rejected is not paid: nothing walks the drawables, and the pass already had every entity it needs in hand.
- **Three things had to be right and each is pinned.** The set holds **handles rather than indices**, because a destroyed entity's index is reused immediately and an index alone would clear an unrelated part built in its place — `Store::Get` checks the generation and answers null. `ReadMaterialCatalogues` clears the set on load, or a handle kept across a directory replacement names whatever now sits at that index. And a world that has never had a material still acquires **no resource at all**, which is what `ColourMapOf` being the non-creating reader exists to protect.
- **Demonstrated by mutation.** With the clear removed, the deletion case and the reparent case both fail; the third case — no catalogue on an untouched world — passes either way, because it pins a guarantee rather than catching this bug.

### [CLOSED] D00029

**The light count was spelled in C++ and in GLSL and nothing checked that the two agreed.**

- `render::MAX_SCENE_LIGHTS` is 16 and `MAX_LIGHTS` in `shaders/opaque.frag` is 16, and the only thing keeping them equal is that somebody wrote both. `AGENTS.md` rule 6 is explicit that a constraint the build does not check is documentation, and this one is.
- **A test that reads the shader back does not work, and it was tried.** What `Paths::Shaders` stages is SPIR-V — the constant is folded away by then — so a suite comparing the staged file against the C++ value has nothing to compare. Reading the repository's `shaders/` directory from a test binary would work and would make the test depend on the source tree being present beside it, which no other suite here does.
- **What a mismatch costs, which is why this is filed rather than shrugged at.** It is not a validation error and not a crash: `LightUniforms` is sized by the C++ constant and the shader indexes by its own, so a shader with a smaller cap silently ignores the tail of the set and one with a larger cap reads past the buffer. Both look like "that lamp does not work".
- ~~**The fix is one line and it is in the build rather than in either file.**~~ **Closed at v0.10, and the entry's own prescription was right except for where the number lives.** `mono_add_library` takes `SHADER_DEFINES`, `_mono_add_shaders` turns each into a `-D` on the glslc command, and `opaque.frag` guards its value with `#ifndef MAX_LIGHTS` so it still compiles by hand.
- **The number stayed in C++ rather than moving into the build, which is the one place this entry's plan was changed.** Putting it in CMake and defining it into both languages was the obvious reading of "one home in the build", and it fails on a detail: `Renderer.hpp` is included by things that do not link `render` — `mono.tools/linecount/tests/Report.cpp` is one — so a compile definition would either break them or need a fallback default, which is the second spelling again. Instead the configure **reads** `MAX_SCENE_LIGHTS` out of the header with a regex and feeds glslc. C++ stays where a reader expects the constant, and the shader keeps no literal of its own.
- **Demonstrated by mutation rather than asserted.** The header set to 8, reconfigured: the command line becomes `-DMAX_LIGHTS=8` and the staged SPIR-V changes hash. Reverted, and it changes back. **There is no test, deliberately** — the disagreement is now unrepresentable rather than detectable, and a suite could not have read the number anyway, which is what the first attempt at this discovered.
- **Two sharp edges, both handled where they bite.** The regex is matched at configure time, so `CMAKE_CONFIGURE_DEPENDS` names the header — without it, editing the constant recompiles the C++ and leaves the shaders on the value read at the last configure, which is this bug arriving by a different door. And a failed match is a `FATAL_ERROR` naming the file, not a silent skip, because a quiet fallback would restore exactly the drift being deleted.
- **Reopen trigger: the next time a shader needs a constant C++ also holds.** The mechanism now exists, so that is a one-line `SHADER_DEFINES` entry rather than a build change.

### [CLOSED] D00028

**`Enum.Material` is a type in Luau after all. This entry was wrong and is corrected rather than deleted.**

- **What it claimed:** that Luau *cannot* express a dotted type name for a global, that a definitions file's inability to declare one was the language's inability, and that the only way out was a generated `Enum.luau` module and a require-path resolver.
- **The first half was right and the conclusion did not follow.** A definitions file genuinely cannot declare one: `loadDefinitionFile` writes `exportedTypeBindings[name]` and nothing else, and there is no `declare` syntax for a dotted name. The probe that produced "Unknown type 'Enum.Material'" was real.
- **What was missed is where the resolution happens.** Luau parses `Enum.Material` in a type position as a reference with a *prefix*, and resolves it through `Scope::lookupImportedType("Enum", "Material")` — the `importedTypeBindings` map. `require` populates that map (`ConstraintGenerator.cpp:1512`), and so may a **host**. Roblox is not using definition-file syntax; it is registering that map. So is luau-lsp's Roblox platform.
- **Closed by doing the same thing.** `mono.tools/scriptcheck` walks the extern types the generator emitted under the `Enum_` prefix and aliases each under the `Enum` prefix, before `freeze`. 35 enums, and `local m: Enum.Material` typechecks. The examples that carried `Enum_Material` in an annotation now carry `Enum.Material`.
- **The declaration file still uses the flat names, and that is ordering rather than compromise.** The aliases are built *from* the types the file created, so they cannot exist while it is being loaded — emitting the dotted form there made the file fail to load before a single script was checked. Both spellings name the same `TypeFun`.
- **What is still open is the editor, and it is filed as D00031** rather than left inside a closed entry.
- **The lesson worth keeping: "the file cannot say it" is not "the language cannot do it".** The first probe answered the question that was asked and the wrong question was asked.

### [CLOSED] D00027

**The mirror flashed once per orbit, and it was a sign flip rather than a projection fault.**

- `scene::AimSurfaceCameras` sets `facing = distance >= 0 ? 1 : -1` and points the reflected camera along `unit * facing`. That is correct on either side of a pane — a face can be looked at from behind and the reflection belongs on that side — and it is discontinuous *at* the plane.
- **Measured, not inferred.** `scene/tests/SurfaceCameras.cpp` orbits the eye a full lap at 360 samples and records the worst single-step change. The field of view moves by 0.027 radians at worst; the camera's look vector moves by **exactly 2.0** at 1.588 radians, which is a 180° turn in one frame at precisely the plane crossing. Orbiting a pane centred on the origin crosses twice a lap.
- **Skipping the crossing was tried and does not work.** Returning early leaves the camera at its previous transform, so the flip lands a frame later instead of not happening; the discontinuity belongs to the sign, not to when it is evaluated. There is no continuous path between facing -Z and facing +Z.
- ~~**What would fix it.**~~ **Closed at v0.10, exactly as this entry predicted and for the reason it gave.** A pane seen edge-on subtends zero pixels, so its surface renders *nothing*: inside `EDGE_ON_MARGIN` of the plane the camera is left where it was and the pane is taken off its slot. The two orientations either side of the crossing are therefore never in consecutive **visible** frames, which is what removes the flash — the sign still flips, and nothing is shown while it does.
- **The measurement inverted.** Worst single-step turn over a 360-sample lap went from **exactly 2.0** — a 180 degree whip — to **exactly 0**, and the zero is not a tuned bound: within one side of the plane `facing` is constant, so the reflected camera's orientation does not change at all as the viewer orbits. Only its position does. Six samples of 360 come out blank, twice a lap.
- **The entry's note about the test was right and was worth writing down.** The bound asserted the bug (`<= 2.001`), so closing this failed it and forced the assertion to be rewritten in the same change — and rewriting it surfaced the real question, which is *what to measure*. Continuity is only asked of frames that draw; comparing across the blank band would be asserting continuity of a picture nobody was shown. A second assertion requires the band to actually be entered, because otherwise the test would pass by quietly no longer crossing the plane.
- **Two existing cases were pinning the degenerate arrangement without saying so, and only failing revealed it.** One put the viewer exactly in the glass and required a *clamped* frustum — a finite matrix for a view covering half a turn, which nobody can see. The other, "a rotated pane reflects along the way it actually faces", left the eye at +Z while the rotated normal was -X: the eye was level with the plane, the mirrored position was the eye itself, and its assertion passed for a reflection that was never computed. **A test that passes because nothing happened is the failure mode this closure actually found.**
- **What it does not cover, stated rather than left to be discovered.** The band is a distance and a viewer's motion is a speed, so somebody crossing the plane fast enough to step over the whole band between two frames still sees the flip. No width closes that for every speed.

### [CLOSED] D00026

**Closed in v0.10.** `bake` writes the runtime texture format from supported
source images, while the client and studio resolve image names through their
content tables and upload the resulting pixels. Missing or unresolved images
still use the visible fallback marker. Runtime code does not decode PNG files;
the parser remains in `bake`.

### [CLOSED] D00025

**`gui::Pick` tests an axis-aligned rectangle, so a rotated button clicks where it is not.**

- Split out of `D00023` rather than left inside it, because the two close in different modules: that one is a *backend* emitting rotated geometry, and this one is `gui`'s own hit test, which no backend can fix.
- `DrawCommand::Bounds` is the unrotated rectangle and `Rotation` sits beside it. `Pick` reads the first and ignores the second, so a rotated element draws in one place and answers a pointer in another.
- **A rotated button that draws in one place and clicks in another is a bug people file twice**, which is why it is written down separately from the drawing half.
- **Closed at v0.8, in the same change as `D00023`** — rotating the geometry without rotating the test would have made the mismatch visible rather than merely present.
- **The point is turned into the element's space, not the rectangle into the screen's.** A rotated rectangle is not a rectangle and testing one needs a polygon; rotating the *point* back by the same angle makes the test the axis-aligned one it already was, exactly.
- **The clip is deliberately still not rotated.** A scissor is axis-aligned on every backend there is, so an element rotated inside a clipped container is cut by an upright rectangle — which is what the painter does and what the hit test therefore has to agree with.

### [CLOSED] D00023

**A rotated `gui` element rotates its box and not its contents.**

- `ui::PaintGui` draws a rotated `Rectangle` or `Outline` as a convex quad and draws `Image` and `Text` upright at the rotated rectangle's centre. `Element::Rotation` and `Resolved::AbsoluteRotation` are correct and are carried on every command; what is missing is a backend that uses them for the other two kinds.
- **Why it stops there rather than being finished.** Rotating a glyph run means per-glyph quads, which imgui's `AddText` cannot emit — it walks the atlas and appends axis-aligned quads with no transform. Rotating a nine-slice means rotating each of the nine pieces individually with its own uv rectangle. Both are real work in the *backend*, and the backend that is going to matter is the batched quad pipeline `ROADMAP.md` schedules at L12, rather than this one.
- **The hit test rotates with neither.** `gui::Pick` tests `DrawCommand::Bounds` as an axis-aligned rectangle, so a rotated button is clickable on its unrotated box. Stated because a rotated button that draws in one place and clicks in another is a bug people file twice.
- **Closed at v0.8 by `render::InterfaceMesh`**, which is the pass this entry predicted: it emits its own vertices, so the rotation is applied to all four kinds in one place — `Push` turns each corner and every kind goes through `Push`.
- **The pivot is the element's centre, not each quad's**, which is the one thing about rotating text that is easy to get wrong and unmistakable when it is: a per-quad pivot spins every letter on the spot and leaves the run in a straight line. Pinned by a case that rotates a two-glyph label and asserts the run goes *down* rather than across.
- **`ui::PaintGui` is unchanged and still draws rotated contents upright**, which is now a difference between the two backends rather than a gap in both. It is the editor's, imgui cannot emit a transformed glyph run without a per-glyph path, and the backend that matters for a shipped game is the one that was fixed.
- The hit test moved with it — see `D00025`, closed in the same change.

### [CLOSED] D00022

**A `SurfaceGui` and a `BillboardGui` lay out against a canvas whose 3D half nothing supplies.**

- `gui::CanvasFor` gave a `SurfaceGui` its `CanvasSize` in pixels and ignored `SizingMode::PixelsPerStud`; it gave a `BillboardGui` the offset half of its `Size` and ignored the scale half. Everything under either laid out correctly *against that canvas* — what was missing was the number the canvas should have been.
- **Closed at v0.8 by `render::ResolveSpatialCanvases`**, which is the shape this entry predicted and the same split `AdornmentGeometry` already makes one dimension up: `gui` is L7 `shared` and links neither `scene` nor a camera, `render` links both, and the multiplication went to the module with both operands.
- **A component rather than `Surface::CanvasSize`, which is where the entry's own suggestion was wrong.** Writing the resolved pixels back into the authored field clobbers a saved property — toggle `Sizing` back to `FixedSize` and the canvas an author typed is gone, replaced by whatever the last pixels-per-stud frame computed. `gui::SpatialCanvas` is a derived component beside `Canvas` and `Resolved`: nothing authors it, nothing replicates it, and two hosts with different viewports are *supposed* to disagree about it.
- **Absence is the interface, and it does more work than the value.** A collector nothing can measure — a `SurfaceGui` on a `Folder`, a billboard in a world with no live camera, a headless test — simply has no component, and `CanvasFor` then uses the authored pixels, which is exactly the behaviour that was there before. So the feature is additive: no caller had to change to keep working, and the one that did not call it kept the old answer rather than a zero.
- **Stale ones are removed, which is the half that is cheap to skip and expensive to skip.** A resolved canvas nobody refreshes keeps working — it keeps the size of the last frame that could measure it — so a `SurfaceGui` switched back to `FixedSize`, or one whose adornee was deleted, would look right until somebody moved the part it is no longer attached to. Pinned by a case that resolves, changes the mode, and asserts the component is gone.
- **Both hosts call it, and the studio calls it per panel.** A viewport panel *is* a canvas with its own camera, so two panels looking at one world from two distances give one billboard two sizes — which is correct, and is the reason this is resolved at the draw site rather than once for the world.
- Nine cases in `render/tests/SpatialCanvas.cpp`, including one that runs the whole seam: `render` writes the component, `gui::Layout` reads it, and a frame inside a pixels-per-stud surface lands on the resolved rectangle. Neither module can assert that alone.

### [CLOSED] D00020

**`gui` text is a `core::Name`, and `core::Name` never releases a string.**

- `Label::Text`, `Picture::Image` and `Entry::PlaceholderText` all interned. That is what made each a `PropertyType::Name` — saved as text, sent as text, readable from both bindings, editable in the properties panel — with no new property type and no new wire form. `ecs::InstanceName` makes the same trade for the same reasons and still does.
- **The cost, stated rather than discovered.** Text that changes every frame interned a new string every frame, forever, plus the process-wide registry's mutex on every write. `label.Text = tostring(score)` at 60 Hz was an unbounded leak and a lock in the frame loop.
- **Closed at v0.8 by `ecs::PropertyType::String`**, which is exactly the shape this entry predicted and very nearly the size: the type, a case in `game::Values` at six sites, a widget in the properties panel, a case in each binding, two in the control surface and three in the manifest generator. Sixteen sites, and the compiler found every one of them — which is the dividend of `PropertyType` being a closed list that everything switches over exhaustively.
- **`Label::Text` and `Entry::PlaceholderText` are `std::string`; `Picture::Image` is still a `core::Name`, and that split is the actual decision.** An asset id is one of the bounded set of things a game shipped, so interning it buys an integer comparison everywhere downstream. A score is computed. The rule is short enough to apply without thinking about it: **a value the game picks from a set is a `Name`; a value the game computes is a `String`.** Both halves are pinned — one case asserts a thousand distinct labels move `core::Name::Count()` by zero, the other asserts a fresh image name still moves it.
- **The storage change cost nothing that was not already paid.** `Label` and `Entry` stop being trivially copyable, so they need written serialisers — which they already had, because a `core::Name`'s id is process-local and could never have been memcpy'd to a file either. `ecs::Column`'s non-trivial path has existed since v0.2 and this is its first user.
- **The one genuinely sharp edge, and it bit in a test before it could bite anywhere else.** Every binding reads a property into a shared `alignas(16) unsigned char bytes[...]` buffer and lets the descriptor fill it. A `String` getter *assigns* rather than filling bytes, and assigning a `std::string` into uninitialised storage is undefined behaviour — so all four callers take an explicit branch before the buffer, and the `PushValue`/`ToJs` switches refuse the type loudly rather than handling it. `gui/tests/Compile.cpp` walks *every* declared property and segfaulted on the first run, which is the failure arriving where it is cheap.
- **Nothing an author can see changed.** `engine.d.luau` and `engine.d.ts` are byte-identical across the change: both said `string` before and after, because whether the engine interns text is a storage concern and a type position should not leak one. A game file is unchanged too — `TypeTag` writes `string` for both and the schema check in `Game.cpp` now compares tags rather than enum members, so moving a property between the two is not a format break.
- **Measured, since it is a per-frame path.** `DrawCommand::Text` is owned as well, which makes a draw command non-trivial — but the compiled list is rebuilt only when the tree's hash moves, and `Compiled::Rebuild · 1k elements` went 136 → 139 ns per element, inside the run-to-run noise. The hash now folds every byte of a label rather than an interned id, which is the one real cost and is the one that has to be paid: a score going from 19 to 91 has to be seen to have changed.
- **`examples/Interface.luau` keeps its ten-hertz clock**, and its comment is unchanged because it never cited this: the throttle is about the compiled-list cache doing something measurable, and that reason survives the leak going away.

### [CLOSED] D00016

**A frame is described in two places as of v0.6 and nothing checks that the two agree.** Filed the moment the second description appeared, rather than after they disagreed.

**Closed at v0.11 by deleting the second description rather than checking it**, which is what the reopen trigger below said to wait for — and it went exactly the way that note warned it could not go otherwise.

- `render::PassOrder()` **reads its names out of `graph::StandardGraph()`**. It was a hand-written array of the same six; it is now a view of the one description. Drift is not possible rather than detectable, which is `AGENTS.md` rule 6 applied to the thing rule 6 was quoted about.
- **`graph::Pipeline` and `StandardPipeline` are deleted, along with their tests.** `RenderGraph` supersedes them entirely — the same declaration-ordered stages with the same read-before-write check, plus resources, the shared/per-view partition and an executor. Keeping the flat list beside it would have been the third description this entry explicitly forbids. "Delete the thing you replaced."
- **The comparison test is kept and repurposed.** It can no longer catch two lists disagreeing, because there is one; what it now checks is that the derivation lines up with the `Pass` enum, which is still written by hand. Mutation-verified: swapping `overlay` and `interface` in `StandardGraph` fails `tests/Passes.cpp` at both the count and the position.
- **The hole that remains is the one this entry always named.** A pass added by writing `SDL_BeginGPURenderPass` inline rather than entering through `PassRecorder` is still invisible to all of this. Closing *that* is the executor — `D00002`'s remainder — not another list.

- `Renderer::Render` submits a shadow pass, a surface pass, an opaque pass, a transparent pass and an overlay pass, in that order, from a function that knows all five by name. `graph::StandardPipeline` is the same five as data, and `Pipeline::Validate` checks the one property that goes wrong at this size — a stage reading a target no earlier stage wrote. **Neither knows the other exists.** `render` links `graph` for `Cull`, `Frustum` and `FitDirectionalLight`; it never reads a `Pipeline`.
- **This is D00002's "`render` must not grow a hand-rolled pass list" being overrun, and the overrun is not the interesting part.** The passes had nowhere else to live — the graph runtime describes and does not execute. What is worth filing is that the mitigation is a sentence: `mono.engine/render/AGENTS.md` now says *"if you add a pass here, add its stage there in the same change"*, which is **rule 6 out loud** — a rule the build does not check is documentation.
- **The check is built.** `render::Pass` and `render::PassOrder()` name the five stages in submission order; `mono.engine/render/tests/Passes.cpp` compares that list against `StandardPipeline`'s stage names, in order, with no device — the comparison is arithmetic over `core::Name`, so it runs anywhere. **Demonstrated by mutation rather than asserted**: a sixth stage added to `StandardPipeline` and nowhere else fails with *"Renderer submits 5 passes and StandardPipeline declares 6 stages"*. `just check` runs `test-all`, so it is every run and not only a changed-suite run.
- **The submission order is checked at runtime, because that half is the one a headless test cannot see.** `PassRecorder` walks `PassOrder` as the frame is built and refuses to go backwards — skips are the normal case and allowed, since every pass here is conditional and `Stage::Optional` already says so. It logs rather than aborts: a renderer that kills the process over its own bookkeeping is worse than the bug it found. `FrameResult::Passes` carries the bits out, which answers a question the draw-call count cannot — running `Mirrors-1-world` with and without `--stats` is 5 draw calls and 4, and only the bitmask says the missing one is the overlay pass rather than a geometry draw.
- **Two holes, named rather than implied.** A pass added by writing `SDL_BeginGPURenderPass` inline instead of entering through `PassRecorder` is still invisible — the convention narrowed from "remember to edit another module" to "enter passes through the recorder", which is greppable, but it is still a convention. And the smart-test signature is a hash over each suite's *header* closure, so a body-only edit to `StandardPipeline` re-runs nothing under bare `just test`; the renderer side is caught either way, because adding a `Pass` member edits `Renderer.hpp`, which is in the closure. **The asymmetry favours the direction this entry was written about.**
- **What is deliberately not done: running the frame from the list.** That is the render-node system, and a small version of it now would be the second executor `mono.engine/render/AGENTS.md` says is worse than one hand-rolled list. What is checked is the description, not the execution.
- **The trigger fired at v0.7, the check held, and the thing beside it did not — which is more useful than a clean pass would have been.** The studio needed its panels drawn inside the frame, so `Pass::Interface` is a real sixth pass, and it went into `render::PassOrder` *and* `graph::StandardPipeline` in the same change because the count-and-order comparison fails otherwise. That comparison is generic and it worked: **evidence rather than reassurance**, and the first time one of these has been demonstrated by a change somebody actually wanted instead of by a mutation.
- **What did not hold was the test next to it, and the distinction is the lesson.** `tests/Passes.cpp` also spells out each pass's *name* by hand, and `interface` was never added — so the one pass this entry's trigger is about was the one pass whose name nothing checked. The file's own header stayed at "five passes" too. **A generic assertion survived the change and both hand-maintained lists beside it rotted**, in the same file, written by the same argument. Fixed at v0.7, with the count deliberately removed from the prose rather than corrected: a number in a comment is the thing that goes stale, and this file exists to stop exactly that one level up.
- **What it did not do is close the entry, and the distinction matters more now than when it was filed.** Keeping two descriptions in step is not the same as having one. v0.8's `gui` rendering attaches a **seventh** pass at exactly this seam, and the cost of the convention is paid again per pass, forever, by whoever remembers.
- **Reopen trigger, now fired: a sixth pass shipped at v0.7 and v0.8 has started.** Both halves of the original trigger are met. **This should not be actioned on its own** — running the frame from the list is `D00002`, and a runtime *deletes* the second description rather than checking it, so doing this entry separately buys a third copy of a mitigation that is already a sentence in an `AGENTS.md` and a comparison in a test. Merge it into that work or leave it as the standing check; do not build a third thing here.

### [CLOSED] D00007

**Both halves are closed. The second one closed at v0.9 and nobody told this
entry, so it spent four versions asking for a file that was already in the
tree.** Found at v0.13 while walking the register against `D00109`, which is
about the same code from the other end.

**Lag compensation is built.** `replication::Rewind` is the "server-side history
buffer of past ticks" the entry below says this module deliberately does not
keep: `RewindSettings::HistoryTicks` frames of position, `Begin`/`Record` per
tick, `Sample` and `Each` at a fractional tick, `Oldest` for the bound.
`Server::ApplyInputs` uses it exactly as written — a client's input tick, less
the interpolation delay it renders behind and the half round trip its snapshot
took, through `Rewind::TickSeenBy`, then a hit test against *the history's*
candidates rather than the world's, because an entity destroyed since the tick
the client saw is still a legitimate thing to have shot at.

**The fairness policy the entry calls a game-design decision was made, and the
entry's own sentence is now the header comment.** `HistoryTicks = 32` — half a
second at sixty hertz, "because it covers an ordinary connection and not an
excuse". Somebody built this from this entry and did not come back to it.

**The reopen trigger fired too.** "The first hitscan weapon, which cannot be
built without it" — `examples::Shooting` is one: `Shot`, `MAXIMUM_SHOT_RANGE`,
`NearestHit`, and a server that recolours what was struck so every client sees
the server's verdict rather than the shooter's.

**What is left is not this entry's and is filed as `D00109`:** nothing ever
*sends* a shot. `examples::EncodeShot` has no caller outside its own suite, so
the whole rewound path decodes an input no client has produced. That is a
missing consumer, not a missing mechanism, and the mechanism is what this entry
was about.

**The lesson is the one this register keeps relearning, in a new place.**
`D00005` records a check whose claim aged into a false one; `D00004` records a
figure that drifted. This is the third shape: **an entry whose blocker was
removed by work that cited it.** A deferred item is only as good as the walk
over it, and the walk has to be against the tree rather than against the entry.

What it said before:

**The bandwidth half closed at v0.4. Lag compensation is untouched. They were filed together and should not have been — one had a trigger that could fire and the other has a trigger that cannot yet.**

- ~~Priority under a bandwidth cap.~~ **Closed, and the reopen trigger fired exactly as this entry wrote it.** `SendsOverBudget` came off zero in a real cross-process run: a 2000-entity world's tick was ~137 messages against a 64-packet budget, so 73 were dropped every tick with the tail chosen by position in a vector — the precise failure this entry predicted, found because the number it named as the signal was the number that moved. What shipped is what this entry asked for: a score per entity per client supplied by the game (this module carries named components and cannot know which one is a position, the same argument `SetInterest` already makes), **a rotation that outranks the score rather than being weighted against it**, and an explicit per-client answer with the reasoning in the header — the budget belongs to a link and there is one link per connection, so a per-server cap would have to be divided before it could be enforced, and that division *is* a per-client cap. The starvation bound is `StarvationTicks + ceil(n/k)` and is asserted by a test rather than argued for. Ordering costs nothing when there is no pressure: rows go out in dirty-bit order and are only re-packed by score if that did not fit.
- Worth keeping from the closure, because it was nearly missed: **the item was found by a bug, not by a measurement anybody set out to take.** The refusals were being blamed on load and on a wall-clock deadline for four separate investigations. The entry's own advice — "`ConnectionStats` already counts the refusals; read it before concluding a component is not replicating" — was right, and nobody read it. A counter that is not looked at is not a mitigation.
- **Still open: lag compensation** — rewinding the server to what a client saw when it fired. It needs a server-side history buffer of past ticks that `replication` deliberately does not keep, and a policy for how far back it will honour, which is a game-design decision about fairness rather than an engine one. **Reopen trigger: the first hitscan weapon**, which cannot be built without it. v0.4 brought the physics and the `Part` that trigger was implicitly waiting on, so the blocker is now the game rather than the engine.

*(The paragraph above appeared twice, once as a trailing bullet with the same
words. The duplicate is not reproduced.)*

### [CLOSED] D00006

**Closed in v0.9.** The stateless challenge prevents an unknown peer from
consuming a client slot, payloads use ChaCha20-Poly1305 with a monotone wire
counter, and the handshake is bound to a server identity with an Ed25519
signature. v0.10 added client identification and the server-side admission
policy. The default remains the weaker unauthenticated mode for compatibility;
`--identity-key` and `--server-key` opt into identity pinning.

### [CLOSED] D00002

**Closed at v0.11 by the executor landing.** `Renderer::Render` runs
`graph::Execute` over a compiled `graph::RenderGraph`; the six pass bodies are
handlers a `PassTable` finds by kind. `render::PassOrder` reads its names out of
the graph and the hand-written `Pass` enum beside it is deleted, so the frame is
described once. `Renderer::SetPipeline` takes an authored graph, which is the
line this entry's last bullet was really about — editing a pipeline changes a
frame now and not only a document.

**What did *not* close with it, recorded so nobody looks for it here:** the graph
still describes what runs and does not yet *allocate* what it runs over —
`Impl::TextureFor` maps the standard frame's resource names onto textures the
renderer owns, and a name it does not know gets an invalid answer rather than a
new target. Transient allocation, aliasing and pass culling — RDG's derived half,
`PIPELINE_NODES.md` §1.1 — are all still absent. That is the next entry somebody
should file rather than a bullet here.

**Three of the four bullets below were already false by the end of v0.6 and were corrected in place rather than left to be read.**

- The graph renderer of `RENDER_PIPELINE.md`. ~~The current `render` module is stage 0 of its twelve — one instanced opaque pass and one overlay pass, standing in for stage 1's skeleton.~~ **Five passes as of v0.6** — shadow, surface, opaque, transparent, overlay — over a frustum-culled draw list. Still stage 1's skeleton rather than the graph, and the thing that makes it a skeleton is unchanged: **the order is a function body**. More passes is not more architecture.
- ~~Its stage 2 needs `ecs::ChangeChannel` for per-node cache invalidation, and its §4.2 needs `ecs::Column`/`ComponentSet` to store nodes as rows. Both wait for v0.2, which is D00001.~~ **Both closed** — the storage rewrite at v0.2, chunking at v0.4. **Nothing in `ecs` blocks the graph any more**, which means this entry has been waiting on nobody but itself since v0.4.
- ~~The graph runtime itself is `mono.engine/graph/` at L9 and does not exist.~~ **It exists as of v0.6, at exactly the tier and layer this line named** — and what landed there is the *description*, not the runtime. `Frustum`, `Cull`, `Shadow`, and a `Pipeline` of `Stage` records that `Validate` checks for a stage reading what nothing wrote. What is absent is the whole of §4.2 and §12: no nodes, no handles, no capabilities, no cache, no compiler, no executor. `Renderer` calls `graph::Cull` and `graph::FitDirectionalLight` as **functions**; it does not run a graph. The directory existing is the least interesting half of this bullet closing.
- ~~`render` must not grow a hand-rolled pass list before it does.~~ **It grew one anyway, and that is an overrun rather than a revision.** v0.6's roadmap asked for shadows and a render-to-texture surface, the graph runtime executes nothing, so the passes had nowhere else to live. The part that is not defensible is that the two descriptions of a frame are now unchecked against each other — filed as **D00016**, which is where the argument about what to do belongs.
- §12.3's remaining additions to F5 — per-stage cache hit/miss, and the undemanded capability of a dead node — **unchanged, and now for the right reason**: they need the cache and the executor, not the directory. The tick rate on F3 and the tick/render split of §14 are done.
- **Reopen trigger, stated for the first time: v0.8's extended pipeline.** "Handle multiple worlds in parallel" is the first roadmap line that cannot be met by making the function longer — per-world pass sharing is exactly the decision `Stage::PerView` was written to record and that nothing yet reads, and HDR plus a G-buffer change what every later stage reads, which is the change a hand-rolled list makes by editing five call sites and a graph makes by editing a list.
