# Prefab and package system plan

## Goal

Build reusable prefab assets and versioned packages without creating another
scene graph, another ECS, another asset store, or another world serializer.
A prefab is an authored instance subtree with stable internal member keys. A
prefab instance is that subtree materialised into the existing ECS plus a sparse
set of local changes. A package is the named, versioned unit that owns prefabs,
scripts, assets, and dependencies.

The first complete path must support:

- creating a prefab from an existing instance subtree
- placing, duplicating, saving, loading, and replicating prefab instances
- editing an instance through sparse property, addition, removal, and nesting
  overrides
- publishing immutable package versions through the existing asset manifest
- pinning exact package content for a universe
- updating instances with a reviewable three-way merge
- resolving nested prefabs and variants without cycles
- undoing or sharing each Studio operation through the existing command stream

The simple case stays simple. An author can turn a model into a local prefab,
place it several times, edit one property, and see one override. Package
publishing, dependency constraints, and conflict review appear only when the
author uses them.

## Existing foundation

This work extends existing ownership cuts rather than introducing parallel
ones.

| Existing piece | Current role | Required extension |
|---|---|---|
| `ecs::Store` and registered classes | Own live instances, hierarchy, components, and properties | Remain the only live prefab storage |
| `game::WriteInstanceDocument` and `ReadInstanceDocument` | Round-trip an instance subtree with document-local references | Share their item, property, source, and reference codec with prefab documents |
| `.agame`, `.auniverse`, and `.aworld` XML | Save universes and worlds | Save prefab source references and sparse instance state beside normal items |
| `game::WriteWorldBody` and `ReadWorldBody` | Save and load classes, properties, scripts, and asset pipelines | Materialise prefab instances through one validated load stage |
| `script::SourceCache` | Owns world script source by stable source name | Mount package script sources under package-qualified names |
| `assets::Manifest` | Maps stable asset names to content roots and delivery bundles | Carry package manifests and prefab documents as asset kinds |
| `assets::ContentHash` | Verifies immutable content and enables deduplication | Identify resolved package bytes, never replace author-facing package names |
| `delivery::AssetClient` | Fetches verified content from directories, HTTP, or relay sources | Fetch locked package and prefab assets through the same client |
| `cdn::Publish` | Builds deterministic signed manifests and content stores | Publish package output as ordinary manifest assets |
| `scene` asset references | Name meshes, textures, materials, animations, and sounds | Resolve package-qualified logical names through the locked package graph |
| `Instance:Clone()` and `Store::CloneInstance` | Clone a live subtree | Preserve or detach prefab provenance according to an explicit clone mode |
| Studio Explorer duplicate and paste | Round-trip through the shared instance document | Preserve sparse prefab state rather than baking inherited members |
| `CommandLog` | Owns Studio undo and redo | Group a prefab edit, apply, detach, or update into one waypoint |
| `EditStream` | Orders and replicates committed Studio edits | Carry prefab operations as bounded edit records |
| runtime replication | Replicates ordinary ECS entities and component changes | Replicate materialised prefab results with no prefab-specific world mirror |

Current instance documents number items only within one document. That is safe
for a temporary reference map, but it is not stable enough for overrides or
package updates. Prefab members therefore need persistent textual keys that do
not change when an item is renamed, moved, or written in a different order.

## Non-negotiable design rules

1. The ECS owns every live and saved prefab fact. A package cache may hold
   immutable source documents, but it may not mirror live instance state.
2. The existing game instance codec remains the one class, property, source,
   and reference codec. Prefab files add an identity and override layer around
   it rather than copying it.
3. A name crosses a file, process, wire, or manifest boundary. ECS entity ids,
   class declaration order, component ids, and interned `Name::Id()` values do
   not.
4. Package names, export names, prefab member keys, and dependency names are
   stable strings. `core::Name` may intern them after validation.
5. A content hash proves which immutable bytes were selected. It is not the
   package's author-facing identity and cannot silently stand in for one.
6. Published package versions are immutable. Changing content requires a new
   version. Re-publishing different bytes under the same name and version is
   refused.
7. Runtime loads an exact lock. It never resolves "latest" while starting a
   game or after a network connection succeeds.
8. A prefab update is prepared and validated away from the live subtree, then
   committed at a world mutation barrier. Failure leaves the previous subtree
   and lock intact.
9. Overrides are sparse intent. The save file does not duplicate every
   inherited property and child.
10. Nested prefab and package dependency cycles are rejected with the complete
    named cycle in the error.
11. Studio uses `CommandLog`, `EditStream`, and existing instance selection.
    There is no prefab-only undo stack, selection model, or collaborative
    protocol.
12. Runtime replication sends the materialised ECS result. It does not ask each
    client to fetch a package and independently reconstruct authoritative
    gameplay state.
13. Importing, diffing, or updating a package never executes its scripts.
14. File and network parsers validate into bounded descriptions before any
    instance is created or any source is mounted.

## Terms and ownership

### Prefab definition

A `PrefabDefinition` is immutable source data identified by:

- package name
- prefab export name
- package version
- resolved package content root
- prefab document content root

It contains one root item, its descendants, stable member keys, local
references, nested prefab declarations, and default property values. The
definition is not a live world and does not tick.

### Prefab instance

A `PrefabInstance` is a normal live instance subtree rooted at one ECS entity.
Its root carries prefab source identity and its sparse override set. Each
materialised inherited member carries derived provenance linking it to the root
and its definition member key. Local additions carry local keys. All gameplay,
rendering, physics, scripting, hierarchy queries, and replication continue to
see ordinary instances.

### Package

A package is a named set of exported prefab definitions and declared content.
It may contain:

- prefab documents
- variant documents
- ModuleScripts and other script sources
- meshes, textures, materials, sounds, animations, and shaders
- data files accepted by the asset pipeline
- direct package dependencies

A package is not a new script VM, world, ECS store, or delivery protocol.

### Variant

A variant is one named base prefab plus a sparse definition patch. It uses the
same override operations as an instance but publishes them as reusable source.
Variants use single inheritance. A chain may be nested, but multiple base
prefabs are not merged implicitly.

## Module placement and dependency direction

The system crosses existing layers, so ownership must follow the current stack:

- `assets` owns package identity records, content roots, and immutable package
  manifest parsing.
- `game` owns prefab document encoding, world save integration, dependency lock
  encoding, and validated materialisation orchestration.
- `scene` owns prefab provenance components and package-qualified scene content
  references where those are world facts.
- `script` owns `PrefabService` and package module resolution. The Luau and
  JavaScript adapters bind the same service operations.
- `delivery` fetches locked content through the existing asset client.
- `replication` sees only the materialised components it already knows how to
  replicate.
- Studio owns authoring, diff, conflict review, update commands, and local
  package workspace views.
- `assetc` and the CDN publisher bake and publish package content without
  depending on Studio.

If a lower layer needs the result of higher-layer orchestration, pass a checked
description downward. Do not add a dependency edge upward. Any new module must
be placed in `expected_graph.json`; however, the first implementation should
prefer focused types in the existing owners over a new cross-cutting module.

## Identity model

### Package and export names

`PackageName` is a globally stable string chosen by the publisher. A reverse
domain form such as `org.atomic.examples.environment` avoids registry-wide
numeric ids without requiring one central allocator. Names are compared after
one documented normalisation pass. Case folding, Unicode normalisation, and
separator rules are identical in the publisher and every reader.

Exports have stable names within a package:

```text
org.atomic.examples.environment:prefabs/OakTree
org.atomic.examples.environment:variants/WinterOak
org.atomic.examples.environment:scripts/Growth
org.atomic.examples.environment:textures/Bark
```

The qualified text is what files, lock entries, and script APIs carry. Runtime
may intern it, but never serialises the interned number.

### Stable member keys

Each definition member receives one `MemberKey` when it first joins a prefab.
The key is opaque text, stored in the prefab document, and retained across:

- display-name changes
- sibling reorder
- reparenting within the same definition
- property changes
- package versions

A generated form such as `member.01K4...` is suitable. The exact generator is
less important than these rules:

- the key is stored, never recomputed from display name or hierarchy path
- copied source members receive new keys unless the operation explicitly moves
  them
- keys are unique within one definition
- local additions use a separate `local.` key namespace
- publishing rejects duplicate, missing, malformed, or reused keys

An author normally sees display names. Studio exposes the member key only in
diagnostics, diffs, and conflict details.

### Occurrence paths

Nested prefab definitions can instantiate the same child prefab more than
once. A target inside nesting is therefore addressed by an `OccurrencePath`:

```text
member.ChildHouse/member.FrontDoor/member.Handle
```

Every segment is a stable member key. It is not a display hierarchy path.
Resolution walks one validated nested definition boundary at a time. Paths have
a hard depth and byte limit.

### Package versions and locks

Published versions use semantic version strings. A dependency declaration may
state a compatible range during authoring, but a universe lock records:

- exact package name
- exact version string
- package manifest content root
- publisher identity or signing key identity
- exact roots for all transitive dependencies

The lock is sorted by package name and byte-stable. Runtime accepts only the
locked graph. Studio may calculate a new candidate lock, but does not replace
the active lock until the user accepts the update and every dependency has been
fetched and verified.

Local workspace packages use the same package name and exports but resolve to a
labelled `workspace` revision with a content root. They do not masquerade as a
published semantic version. Publishing requires an explicit version and a
clean, reproducible bake.

## Package format

### Source workspace

A local package workspace is a directory below the universe's declared package
root. It contains one package descriptor plus source assets. Descriptor paths
are relative, canonical, and unable to escape the workspace. Symbolic links,
absolute paths, parent traversal, device paths, and platform-specific aliases
are rejected at the boundary.

The source descriptor declares:

- package name and proposed version
- publisher identity
- exported prefab and variant names
- declared scripts and assets
- direct dependency names and compatible ranges
- package capabilities needed for authoring or runtime

Discovery order never affects output. The bake sorts all descriptor entries by
their stable names before hashing or writing.

### Published package manifest

The baked package manifest is a canonical, versioned document carried as an
ordinary asset. It maps each qualified export name to:

- asset kind
- immutable content root
- uncompressed byte length
- declared direct dependencies
- optional source metadata that is safe to publish

The existing CDN manifest remains the delivery index. Package manifests do not
add a second chunk store, signature scheme, bundle format, or fetch client.
They are assets inside the signed content graph. The publisher groups package
manifests with their small definition and script assets where measured startup
behaviour benefits from it.

Add explicit `AssetKind::Package` and `AssetKind::Prefab` values rather than
deriving kind from a filename in downstream readers. Variant documents use the
prefab kind with a declared base.

### Prefab document

The prefab document is XML and uses the existing `XmlDocument`, `XmlWriter`,
`XmlLimits`, property tags, class registry, source cache codec, and instance
item codec. The shared codec gains an identity policy:

- world and clipboard documents may keep document-local numeric item ids for
  temporary references
- prefab definitions require textual member keys
- override targets require occurrence paths

Conceptually:

```xml
<Prefab format="1"
        package="org.atomic.examples.environment"
        name="prefabs/OakTree">
  <Sources>...</Sources>
  <Item class="Model" key="member.Root" name="OakTree">
    <Item class="MeshPart" key="member.Trunk" name="Trunk">
      <Property name="MeshId" type="string">textures/TrunkMesh</Property>
    </Item>
  </Item>
</Prefab>
```

The final spelling is fixed by format tests before shipping. The important cut
is that `Item` and `Property` are encoded and decoded by the same functions as
world instances. A prefab reader is not permitted to grow its own property
switch.

Unknown classes and incompatible property types follow an explicit package
load policy. Authoring may preserve a validated opaque source node for review,
but runtime refuses a required unknown class instead of silently producing a
different prefab.

## Live ECS representation

### Root state

The root carries a `scene::PrefabInstance` component with:

- source package name
- prefab or variant export name
- resolved package version
- resolved definition content root
- instance key used by saved cross-boundary references
- update policy
- sparse override records
- local additions and removal tombstones
- last applied source revision

The component is authoritative instance state. A private Studio map may index
it for display, but cannot own another copy.

Override arrays need an explicit game codec because the current generic
property set has no arbitrary typed record list. This is an extension hook in
the shared game serializer, like world sources and asset pipelines, not a
second serializer. The records must still be stored in an ECS component and
must have one parser and writer shared by world, clipboard, undo, and tests.

### Member provenance

Each materialised member carries compact `scene::PrefabMember` provenance:

- root entity reference, valid only in the current world
- definition member key or local addition key
- occurrence path index or interned path
- inherited, local, removed-placeholder, or detached status

This component is derived from the root source plus overrides and can be
rebuilt on load. It is not an external identity and its entity reference never
crosses a world boundary. The root's textual `InstanceKey` and the member's
textual occurrence path are used when a save or package edit needs stable
identity.

### Materialisation cache

Parsed immutable definitions may be cached by content root. The cache contains
validated descriptions, not live entities. It is bounded by bytes, supports
concurrent readers, and invalidates only when a local workspace revision gets
a different root. Published content never mutates in place.

Materialisation reads the cached description and creates normal ECS rows in a
detached staging subtree. It must not retain pointers into cache storage after
commit.

## Instantiation

Instantiation is one deterministic operation:

1. Resolve the qualified prefab name through the active exact package lock.
2. Fetch and verify the package manifest and prefab content root.
3. Parse to a bounded checked definition without touching the world.
4. Resolve nested definitions and validate both dependency graphs.
5. Allocate a detached staging subtree in stable definition order.
6. Create every class and default component through existing class factories.
7. Apply definition properties and resolve internal references by member key.
8. Apply variant patches, then instance overrides.
9. Mount required script source names without running them.
10. Parent the completed root at the mutation barrier and publish ordinary
    create and component changes.

Any failure before step 10 destroys the detached staging subtree. A failure in
the final parent operation leaves the old parent and selection unchanged.

The optional parent and transform are explicit call arguments. Applying the
placement transform affects the root through the existing transform property;
it does not rewrite authored child transforms.

### Clone, duplicate, paste, and detach

Cloning a prefab instance defaults to preserving its source and sparse
overrides while generating a new root `InstanceKey` and new local-addition
keys. Inherited definition member keys remain the same because they are scoped
to the new root occurrence.

Studio offers a separate `Detach from Prefab` operation. Detach removes source
and provenance state while retaining the materialised normal subtree. It is
one undoable command and does not modify the source package.

Copy and paste serialise the prefab root identity and sparse state through the
shared instance document. If the destination universe lacks a compatible
locked package, paste reports the missing package and offers an explicit
dependency import. It never silently bakes the source into unrelated local
instances.

## Sparse override model

An override is addressed by occurrence path, operation kind, and stable
property or child name. Records are sorted canonically before save and hash.

### Property overrides

A property override stores:

- target occurrence path
- stable property name
- declared property type tag
- encoded value or stable reference target
- source definition revision at first override, for update review

The encoded value uses the shared property codec. Setting a property to the
current inherited value removes the override. Resetting explicitly removes the
record and re-applies the inherited value.

### Local additions

A local addition is a complete instance subtree attached below an inherited or
local member. Its root receives a `local.` member key scoped to the prefab
instance. The subtree uses the shared instance document codec and may contain
ordinary local references.

If an added subtree contains another prefab instance, that nested instance
keeps its own package source and sparse state. It is not flattened into the
parent override.

### Removals

Removing an inherited member writes a tombstone for its occurrence path. The
materialised member is removed, but the tombstone remains so a package update
does not recreate it. Removing a local addition deletes its addition record and
needs no tombstone.

A tombstone covers the inherited member's subtree. Individual descendant
tombstones under an already removed ancestor are normalised away.

### Reparenting and rename

Renaming is an override of the existing stable `Name` property. It does not
change a member key.

Reparenting an inherited member is a structural override containing target
member and new parent occurrence paths. It is allowed only inside the same
prefab root. Moving an inherited member outside the root requires detach or a
local copy, because a sparse source link cannot own half a subtree elsewhere.

Sibling display order is not an identity. If ordered children become a scene
fact later, order is a named structural property and receives its own override
rather than borrowing serialization order.

### Override normalisation

Before commit or save:

- duplicate property overrides collapse to the last accepted edit
- overrides equal to inherited values disappear
- descendant edits beneath a tombstoned ancestor are refused or removed after
  explicit confirmation
- local additions with missing parents are conflicts, not new roots
- structural cycles are rejected
- records are sorted by occurrence path, operation kind, and property name

Normalisation runs once per committed edit or load, not once per frame.

## Nested prefabs

A definition may declare a nested prefab at one member key. The declaration
contains a qualified prefab export and the package dependency that supplies it.
The nested root is one occurrence boundary, so several placements of the same
definition remain distinct.

Nested resolution builds two graphs:

- package dependency graph by package name and locked version
- prefab expansion graph by qualified prefab export

Both are checked before materialisation. A repeated package in different
branches is fine when the lock resolves it to one exact version. A back-edge is
a cycle and is refused with the full path.

Initial hard limits:

- 32 nested prefab levels
- 16 variant base levels
- 256 direct package dependencies
- 65,536 materialised members in one prefab instance

These are security caps, not performance promises. Studio warns at lower
measured authoring thresholds.

## Variants

A variant document names exactly one base prefab or variant and stores a sparse
patch using the same record schema as an instance override. Applying a variant
has this order:

1. base prefab defaults
2. each variant patch from oldest base to leaf
3. instance overrides

Variant patches may change properties, add local definition members, remove
base members, and nest other prefabs. Published variant additions receive
stable `member.` keys, not `local.` keys, because they become definition
members.

Studio can create a variant from a prefab instance by promoting selected
overrides. Promotion shows exactly which overrides move into the variant and
which remain on the instance. It is one command and never mutates a published
package version.

## Dependencies and cycles

### Declared dependencies

Every cross-package reference requires a direct declared dependency. Transitive
visibility is not enough. This keeps package cuts reviewable and prevents a
package from breaking when an unrelated dependency stops re-exporting a name.

The package compiler verifies:

- every qualified external name has a direct dependency
- dependency ranges have at least one selected version in the candidate lock
- one package name resolves to one exact version in a universe
- no package or prefab expansion cycle exists
- every referenced export exists and has the expected asset kind
- no dependency is present but unused, unless explicitly retained for script
  dynamic lookup

Dynamic lookup uses a declared bounded export prefix. It does not turn the
whole package registry into an undeclared runtime dependency.

### Lock calculation

Studio resolves a candidate graph outside the live universe. Version choice is
deterministic: constraints, available signed versions, publisher trust, and a
documented stable tie-break determine one answer. The accepted exact lock is
saved with the universe.

The runtime loader does not contain the solver. It verifies and consumes the
exact lock. Servers and clients therefore agree on package bytes without
repeating a registry query.

## References across prefab boundaries

Reference properties become normal `ecs::Entity` references after
materialisation. Their source encoding depends on the target:

- same definition: target member key
- nested definition: target occurrence path
- local addition: local member key
- another prefab instance in the same saved document: target root's
  document-local item id plus its textual occurrence path
- package asset or script: qualified package export name

The existing world writer may continue to use document-local numeric ids for
ordinary references whose source and target are both written items. When the
target is an inherited prefab member omitted from the sparse save, the writer
uses a `prefab-ref` form containing the target root and textual occurrence
path. The shared reference codec owns both forms.

References never persist a live entity generation, provenance path index,
class id, or content-cache pointer. A missing target is diagnosed with the
qualified package, root instance key, and occurrence path. Optional reference
properties may resolve to null. Required definition references make
materialisation fail.

An update maps old and new members by stable key before external references are
swapped. A removed target with an outside reference is a blocking conflict
unless the property is nullable and the user explicitly accepts clearing it.

## Scripts and modules

Package scripts reuse `ModuleScript`, `Script`, `LocalScript`, the existing
source containers, and the same Luau and JavaScript VMs.

Published source is mounted under a deterministic qualified source name:

```text
package/org.atomic.examples.environment/1.4.2/scripts/Growth.luau
```

The package manifest binds that logical name to verified bytes. Materialised
script instances point to the qualified source name in `SourceCache`. The
loader does not write package bytes to arbitrary filesystem paths.

Module resolution rules are explicit:

- a relative require stays inside the current package
- a qualified require needs a declared direct package dependency
- Luau and JavaScript use the same package name and lock resolution
- a cross-VM module boundary uses the engine's explicit cross-language bridge
  when that feature exists, never an implicit package conversion
- source import, package diff, and update do not execute modules

Editing inherited published source creates a local script-source override or
opens an editable local package checkout. It cannot modify the cached published
bytes. Publishing a local source change creates a new package version.

Package scripts receive no extra filesystem, network, microphone, camera, or
Studio authority merely because they came from a package. Existing game and
plugin permission checks remain the authority.

## Asset references, bake, and delivery

Prefab properties name package exports, not raw content roots or local source
paths. The package bake resolves each export through existing asset pipelines,
then records its immutable content root and kind in the package manifest.

The bake must be reproducible:

- source walk and descriptor entries are sorted
- tool and pipeline versions that affect bytes are inputs to the result
- referenced textures, materials, meshes, animations, sounds, and shaders are
  declared and validated
- identical input and tool versions produce identical package and asset roots
- no source file outside the package workspace is read

The existing CDN publisher chunks, bundles, signs, and stores output. Package
affinity may inform bundle grouping, but package boundaries do not force one
large download. Small prefab documents and scripts may arrive together while
large textures and meshes retain useful streaming and cancellation units.

Local Studio preview may mount a verified workspace bake immediately, following
the current on-demand asset path. Status must distinguish workspace-only bytes
from published bytes available to clients.

## Save and load

### Universe data

The universe saves:

- package source configuration
- exact package lock
- publisher trust decisions required by that lock
- local workspace substitutions used only for authoring

World files save each prefab root's source identity and sparse state. Inherited
materialised descendants are not written as full ordinary items. Local
additions are written once inside the override payload.

### Load order

1. Parse and validate the universe and package lock.
2. Verify package names, versions, roots, signatures, and dependency graph.
3. Fetch or locate required package and prefab assets.
4. Parse definitions to checked immutable descriptions.
5. Parse world ordinary items and prefab root records without running scripts.
6. Materialise prefab staging subtrees and resolve all reference forms.
7. Commit complete roots into their saved hierarchy.
8. Mount script sources and start scripts through the normal game startup path.

If required package content is missing, corrupt, untrusted, or incompatible,
runtime load fails with a stable error. Studio may open a recovery view that
preserves the raw validated prefab records, but it must not claim a partial
materialisation is the authored world.

### Save stability

Saving an unchanged universe twice produces byte-identical package locks,
prefab source records, and override ordering. Opening and saving does not create
override records. Materialisation order, hash table order, and ECS entity ids
cannot affect output.

## Runtime replication

The authoritative host materialises prefabs before or at a fixed mutation
barrier. Runtime replication observes the resulting ordinary entities and
components. Clients do not need package authoring metadata to simulate the
replicated world.

This has several consequences:

- a prefab placement replicates as normal entity creation
- an update replicates as the corresponding bounded create, write, reparent,
  and destroy changes
- interest management can stream part of a large prefab like any other scene
  subtree
- ownership and anti-cheat rules remain entity and component rules
- package bytes are not trusted as authoritative state from a client

`PrefabInstance` and `PrefabMember` provenance are not replicated to gameplay
clients by default. Team Create and Studio edit streaming carry authoring
operations through `EditStream`. A game that deliberately exposes prefab
identity to scripts may replicate a small read-only source-name component, but
that is an explicit schema choice.

Large prefab updates must respect replication budgets. The server may stage
delivery across clients after one atomic authoritative commit, as normal
replication already does. It may not expose half the update to simulation over
several ticks.

## Script API

Add one `PrefabService` backed by the same service surface for Luau and
JavaScript.

Initial runtime API:

```text
PrefabService:Instantiate(prefabName, parent?, transform?) -> Instance
PrefabService:IsPrefabInstance(instance) -> boolean
PrefabService:GetSource(instance) -> PrefabSource?
PrefabService:GetMemberKey(instance) -> string?
PrefabService:GetRoot(instance) -> Instance?
PrefabService:Detach(instance) -> boolean
```

`Instantiate` is authority-only where world mutation is authority-only. It
returns only after the subtree is complete or fails without creating a visible
root. Async content fetching uses the scripting runtime's existing yield or
future convention once available; it does not block a world tick on network or
disk.

Runtime scripts may change normal properties and hierarchy through existing
APIs. A write to an inherited member becomes runtime state, not automatically
an authored Studio override. Persisting that write requires an explicit game
save system. Studio authoring routes edits through `PrefabService`'s editor
adapter so sparse overrides and undo records are updated.

Studio and trusted plugin additions:

```text
PrefabService:CreateFromInstance(root, packageName, exportName)
PrefabService:Apply(instance)
PrefabService:Revert(instance, target?, property?)
PrefabService:Detach(instance)
PrefabService:CreateVariant(instance, packageName, exportName)
PrefabService:GetOverrides(instance)
PrefabService:Update(instance, candidateVersion, resolutions?)
PackageService:GetLockedVersion(packageName)
PackageService:CheckForUpdates(packageName?)
PackageService:ResolveCandidate(changes)
PackageService:ApplyCandidate(lock)
PackageService:Publish(packageName, version)
```

Trusted authoring operations still validate paths, package trust, limits, and
world authority. Plugin bindings do not gain direct access to cache files,
signing keys, or internal content roots beyond read-only diagnostics.

## Studio authoring

### Creating and editing prefabs

Explorer adds focused actions:

- Create Prefab
- Create Variant
- Open Prefab Source
- Apply Selected Overrides
- Revert Selected Overrides
- Detach from Prefab
- Select Prefab Root
- Find Source Member
- Check for Package Update

Creating a prefab opens an isolated authoring world backed by the same ECS and
instance codec. It is not a custom tree model. Saving the authoring world writes
the local prefab definition, assigns keys to new members, validates references,
and creates a new workspace content root.

The Explorer decorates inherited, overridden, locally added, removed, and
conflicted members with restrained icons or text colour. The hierarchy remains
the real instance hierarchy. Hidden inherited members can be revealed without
recreating them.

### Properties and override view

The Properties dock shows whether each value is inherited, overridden, or
locally added. It provides reset and source-value actions on the property row.
The dedicated Prefab Overrides view groups records by member and operation and
supports filtering by conflict, local addition, removal, and shadowed upstream
change.

No panel recomputes a full definition diff every frame. A revision on the root,
active definition, selection, and candidate update invalidates cached rows.
Closed or hidden panels do no diff or preview work.

### Package view

The Package view shows:

- local and published packages
- active exact lock
- dependency and reverse-dependency graphs
- exported prefabs, variants, scripts, and assets
- workspace changes against the last published version
- available signed versions
- fetch, bake, and publish progress
- trust, signature, and conflict status

Package update checks are asynchronous and cancellable. The UI reports which
manifest, definition, or asset is being fetched and the byte totals known at
that stage. Network or bake work never freezes the Studio frame.

### Undo, redo, and Team Create

Every user action records one named `CommandLog` waypoint. Creating a prefab,
applying overrides, reverting a subtree, detaching, changing a lock, or applying
an update can touch many entities but remains one undoable operation.

`EditStream` carries bounded semantic prefab records or the ordinary commands
they produce. The host orders and validates them. A package or prefab source
being edited has a named lock scope based on package and export names, not a
local path or entity number.

An update arriving while local edits are pending is rebased through the same
command replay rule as other Team Create edits. If the rebase changes conflict
classification, the update pauses for review rather than selecting a winner on
different machines.

## Update and conflict handling

### Explicit update policy

Published package instances stay on the exact locked version until an author
accepts a new candidate lock. Opening a project, connecting to a server, or
seeing that a newer version exists does not mutate the world.

Local workspace packages may support an opt-in live preview. Even then, a
workspace root change creates a candidate update and applies at a Studio
mutation barrier. It never modifies a running authoritative game without an
explicit runtime hot-update policy.

### Three-way comparison

An update compares:

- base: the exact definition revision last applied
- upstream: the candidate new definition
- local: the instance's sparse override and local additions

Members match by stable key. Properties match by stable property name and type.
The comparison classifies:

- upstream-only addition, removal, move, rename, or property change
- local override that still applies cleanly
- upstream change hidden by a local property override
- target removed or class changed beneath a local override
- incompatible property type change
- local addition whose parent disappeared
- cross-boundary reference whose target disappeared
- nested dependency or variant base change

An upstream property change beneath a valid local override is a shadowed
change, not automatically a blocking conflict. Studio shows it because the
result would change if the override is later reset.

Blocking conflicts require one recorded resolution:

- keep local intent against a selected replacement target
- accept upstream and remove the local override
- preserve removed content as a detached local addition
- clear an optional broken reference
- cancel the whole update

There is no automatic fuzzy match by display name. It can suggest candidates
for the user, but only stable keys or an explicit resolution commit identity.

### Atomic apply and rollback

The updater builds `PrefabApplyPlan` from validated old definition, candidate
definition, overrides, and resolutions. The plan contains all creates, writes,
reference remaps, reparents, and destroys in deterministic order.

Before touching the live root it:

- verifies every target still has the expected source revision
- builds the candidate subtree detached
- resolves every internal and external reference
- checks limits and class/property compatibility
- records the reversible Studio command payload
- reserves required bounded storage where practical

At the world mutation barrier it swaps the completed subtree and remaps outside
references by stable occurrence path. Old entities are destroyed only after
all required references point to the candidate. If final commit cannot finish,
the recorded inverse restores the old parent, references, and root state before
the barrier ends. The active package lock changes only after every affected
root commits.

Runtime readers therefore observe either the old complete version or the new
complete version. They never observe a half-applied hierarchy.

## Hostile content and limits

Game files, package manifests, prefab documents, CDN content, and collaborative
edit payloads are untrusted.

Every parser follows parse, validate, then build. Initial named hard caps cover:

- package descriptor and prefab document bytes
- XML depth, elements, attributes, and text bytes
- packages and direct dependencies per universe
- exports and source files per package
- members per definition and materialised instance
- nested prefab and variant depth
- override, tombstone, and local addition counts
- occurrence path segments and bytes
- script source bytes
- asset declared and uncompressed bytes
- total decompressed package bytes
- conflicts and resolutions in one update
- edit-stream payload bytes and records

Exact values live in public constants beside the parser and are exercised at
limit and limit-plus-one. The first implementation starts with the nested and
member limits stated above, measures real projects, then fixes the remaining
caps before enabling remote packages.

Security checks include:

- canonical path containment after filesystem resolution
- duplicate names and keys after normalisation
- signature and content-root verification before parse
- decompression ratio and total-output bounds
- integer overflow in byte and element counts
- reference paths that escape a prefab root
- dependency and nesting cycles
- unknown required classes, properties, and asset kinds
- scripts that request undeclared sources or package dependencies
- package name confusion through case or Unicode variants
- local workspace substitution excluded from release export unless explicitly
  baked and locked

Errors identify stable package and export names. They do not leak cache paths,
tokens, signing keys, private origin addresses, or arbitrary source contents.

Add fuzz targets for package manifest, prefab document, override set, lock, and
cross-boundary reference parsing. Fuzzers stop at checked descriptions and do
not execute scripts or initialise a renderer.

## Diagnostics and profiling

Use `ENGINE_PROFILE_CAT` around meaningful package, parse, diff, materialise,
and apply units. Worker parsing and asset bake work report completed durations
through the established async profiling path.

Report at least:

- package manifests and bytes fetched
- verified content cache hits and misses
- definitions parsed and cache hit rate
- members materialised
- property and structural overrides applied
- update members compared
- clean, shadowed, and conflicting changes
- staging bytes and peak temporary bytes
- apply creates, writes, reparents, destroys, and reference remaps
- package bake input and output bytes
- publish deduplication and bundle bytes
- refused documents and the bounded reason category

Profile release builds with small, medium, and limit-near prefab suites. Record
startup, placement, duplicate, save, load, diff, update, and detach costs.
Compare cached and cold content paths separately.

Parallel work is limited to immutable stages such as hashing, asset baking,
fetching independent content, and parsing separate definition documents. ECS
creation and commit stay with the world owner. Measure serial and parallel
crossovers before adding jobs, and never allow work to complete on a later tick
when gameplay determinism requires it in the current one.

## Migration from ordinary subtrees

Migration is additive and keeps the project loadable after every phase.

1. Extract the current instance item, property, source, and reference codec into
   shared internal functions without changing world output.
2. Add stable member keys and prefab definition round-trip tests.
3. Add local prefab workspaces and materialisation without world persistence.
4. Add `PrefabInstance` root state and sparse world save/load.
5. Route Explorer duplicate, copy, paste, detach, and undo through the shared
   prefab state.
6. Add variants and nested prefab validation.
7. Add package descriptors, exact universe locks, and local dependency resolve.
8. Add CDN package publish and verified delivery.
9. Add update diff, conflict review, atomic apply, and rollback.
10. Add script bindings and Team Create records.
11. Remove any temporary expanded-save or prefab-specific property codec used
    during bring-up.

`Create Prefab` converts selected ordinary subtrees by assigning stable keys and
writing a local definition. Existing ordinary instances remain valid forever.
There is no bulk automatic conversion on project open.

If an early development build saved fully expanded prefab descendants, provide
one bounded migration reader that recognises that explicit format version,
reconstructs sparse state, and writes only the canonical current form. Delete
the compatibility reader when the repository's supported format window allows
it.

## Implementation phases and gates

### Phase 1: shared format primitives

- extract one instance codec used by world, clipboard, and prefab documents
- define package, export, member key, occurrence path, and lock types
- add canonical validation and named limits
- round-trip definitions without materialising them

Gate: existing `.agame`, `.auniverse`, `.aworld`, clipboard, and instance
round-trip fixtures remain byte-identical where no format extension is used.

### Phase 2: local prefab core

- create prefab definitions from ordinary subtrees
- materialise local definitions into the ECS
- store root and member provenance
- preserve provenance across clone, duplicate, copy, and paste
- detach to ordinary instances

Gate: local prefabs survive save, load, duplicate, and detach with references
and scripts intact.

### Phase 3: sparse editing

- property overrides and reset
- local additions and inherited tombstones
- structural overrides and normalisation
- Properties and Explorer decoration
- one-waypoint undo and Team Create edit records

Gate: every supported Studio edit changes only the expected sparse record and
materialised ECS rows.

### Phase 4: nesting and variants

- occurrence paths
- nested prefab dependency validation
- single-base variants
- cycle reports and depth caps
- cross-boundary references

Gate: repeated nesting, local overrides inside nested instances, variants, and
cycle refusal pass format and integration suites.

### Phase 5: packages and delivery

- local package descriptor and workspace
- dependency constraints and exact universe lock
- reproducible package bake
- package and prefab asset kinds
- signed CDN publication and delivery
- qualified script and asset resolution

Gate: a clean machine starts a locked project from the published content store
without access to the source workspace.

### Phase 6: updates and conflicts

- three-way stable-key diff
- candidate lock review
- conflict records and explicit resolutions
- detached staging and atomic commit
- rollback and outside-reference remap

Gate: injected failures at every prepare and commit boundary leave the old
world and lock valid, and successful updates are one undoable operation.

### Phase 7: runtime and script completion

- `PrefabService` and package query APIs in both script languages
- authority and permission checks
- normal replication coverage for placements and hot updates
- async fetch progress and cancellation
- profiling, limits, fuzzing, and release documentation

Gate: server, client, Studio, and headless builds pass with no source workspace
or renderer required by package core tests.

## Focused test plan

### Format tests

- prefab definition empty, one member, deep hierarchy, and maximum members
- stable member keys survive rename, reorder, and reparent
- duplicate, missing, malformed, and overlong keys are refused
- every existing property type round-trips through the shared codec
- internal, nested, local, and external prefab references round-trip
- canonical output is byte-identical across repeated writes
- source and asset names retain package qualification
- unknown format versions and incompatible property tags fail clearly
- lock entries sort by package name and preserve exact roots
- limit and limit-plus-one for every parser cap

### Materialisation tests

- defaults create the same classes and properties as the source definition
- class factories and default values run exactly once
- references resolve after all targets exist
- missing required content leaves no visible root
- transform placement changes only the root
- clone preserves sparse source but creates independent instance and local keys
- detach retains materialised content and removes provenance
- scripts are mounted but do not run during import or diff

### Override tests

- setting one property writes one override
- setting it back to inherited removes the override
- adding and removing local subtrees normalises correctly
- inherited removal persists across reload and update
- rename does not change member identity
- illegal cross-root reparent is refused
- a tombstoned ancestor removes redundant descendant edits
- override ordering is independent of edit order

### Nesting and variant tests

- same nested prefab appears twice with distinct occurrence paths
- overrides target the correct repeated occurrence
- package dependency and prefab expansion cycles show their complete name path
- depth and member expansion caps stop before allocation growth
- variant patch order is base to leaf to instance
- variant addition keys remain stable in the next published version
- multiple base inheritance is refused

### Package tests

- identical source and tool versions produce identical roots
- file discovery order cannot change output
- path escape, symlink escape, and case-confused duplicate names are refused
- undeclared direct dependencies are refused
- one package name cannot resolve to two versions in one lock
- published name and version cannot accept different bytes
- cold and cached delivery produce identical verified definitions
- corrupt chunks, manifest roots, signatures, and compressed lengths fail before
  parse
- local workspace substitutions are excluded from release export by default

### Update tests

- clean upstream property, addition, removal, move, and rename
- valid local override shadows an upstream value change
- removed local target, changed class, and changed property type conflict
- removed reference target blocks or clears only through recorded resolution
- local addition survives when its parent survives
- local addition with removed parent conflicts
- nested dependency and variant base updates compare correctly
- cancel changes neither subtree nor lock
- successful apply changes both once and undoes once
- injected allocation, reference, parent, and command failure rolls back
- outside references remap by stable occurrence path

### Studio and collaboration tests

- Explorer and Properties report inherited, override, local, removed, and
  conflicted state
- hidden panels perform no definition diff work
- create, apply, revert, detach, and update each produce one waypoint
- undo and redo restore exact sparse state and selection
- Team Create orders simultaneous edits and converges after optimistic replay
- stale source revisions force re-review rather than divergent apply
- progress and cancellation keep Studio responsive during fetch and bake

### Runtime and replication tests

- authoritative placement replicates the complete ordinary subtree
- clients do not need authoring provenance to receive gameplay state
- interest loss and regain preserves the same materialised values
- runtime property edits replicate normally
- a hot update commits in one server tick and streams within normal budgets
- client package or entity claims cannot bypass server authority
- headless server loads locked packages without Studio or GPU dependencies

### Fuzz and soak tests

- package manifest parser
- prefab XML parser
- override and lock parser
- cross-boundary reference parser
- dependency and occurrence-path graph validation
- repeated place, update, detach, destroy cycles with flat live memory
- large package cache eviction while active instances remain valid
- repeated failed updates leave entity count, references, and lock unchanged

Avoid broad tests that merely open an empty project. Each suite owns one public
format or behaviour and fails when that contract breaks.

## Non-goals for the first complete system

- a public package marketplace, billing, reviews, or discovery ranking
- arbitrary package install scripts or native binary extensions
- multiple-inheritance variants
- fuzzy automatic conflict resolution by display name
- editing immutable published package bytes in place
- runtime resolution of newest compatible versions
- client-side reconstruction of authoritative worlds from package content
- a second scene serializer, ECS, hierarchy, script VM, asset store, CDN, or
  replication protocol
- source-control replacement or a general distributed version-control system
- per-frame prefab diffing or live package polling in shipped games
- silently flattening missing packages into ordinary instances
- loading unbounded or unsigned remote content

## Open decisions

These choices need small prototypes or compatibility data before their final
spelling is frozen. They do not change the ownership cuts above.

1. Choose the canonical `MemberKey` generator and text alphabet. The key must
   be stable, compact, collision-resistant, easy to validate, and unrelated to
   display names or entity ids.
2. Freeze package-name case and Unicode normalisation rules before the first
   published package. Publisher, Studio, lock parser, and runtime must share one
   implementation and one set of fixtures.
3. Decide whether prefab documents use their own file extension in local
   workspaces or remain named XML assets. The published asset kind and bytes
   are unchanged either way.
4. Measure real projects before freezing hard byte, export, override, and cache
   caps. Security caps must be constants and cannot be replaced by warnings.
5. Decide whether the root `InstanceKey` should become a general saved scene
   instance key. Keep it prefab-scoped unless another completed system needs
   stable scene identity, because a universal id adds storage and migration to
   every instance.
6. Confirm whether read-only prefab source identity is useful to ordinary game
   clients. Keep authoring provenance unreplicated by default until a concrete
   gameplay use justifies the bytes.
7. Decide whether a universe may lock two major versions of one package under
   explicit aliases. The first implementation resolves one package name to one
   exact version because it keeps module and asset lookup unambiguous.
8. Define the runtime hot-update policy separately from Studio updates. The
   core apply path can support a mutation barrier, but shipped games should not
   gain remote package polling or automatic mutation by accident.
9. Measure whether package affinity should influence CDN bundle grouping. Keep
   the existing bundle policy until startup traces show a repeatable benefit.

## Completion definition

The system is complete when an author can create a local prefab, place and
override it, nest it, derive a variant, publish its package, lock it into a
universe, load it on a clean server and client, update it with conflicts, undo
the update, and detach an instance while all of these remain true:

- one ECS owns live state
- one instance and property codec owns scene data
- one asset manifest and delivery path owns package bytes
- one Studio command stream owns undo and collaboration
- stable textual names and member keys own cross-boundary identity
- save output is canonical and sparse
- runtime replication carries ordinary materialised state
- hostile input is bounded before construction
- failed fetches, parses, and applies leave the previous world usable
- focused tests, fuzz targets, architecture checks, both headless presets, and
  release-profile gates pass
