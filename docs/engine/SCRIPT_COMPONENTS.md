# Script components

A script declares a component with `World:DefineComponent` and tags it with
`World:SetComponentTags`. The component is a real ECS component: C++ systems,
queries and snapshots see it the same as an engine component.

```luau
World:DefineComponent("anatomy.Body", {
	Genome = "string",
	Revision = "int32",
	Status = "string",
})
World:SetComponentTags("anatomy.Body", { "replicated", "saved" })
workspace.Model:SetComponent("anatomy.Body", { Genome = share, Revision = 3 })
```

## When to declare

Declare at any time. A plugin, a game's startup script and a script that runs
mid-session all work the same way.

The component table seals after startup, but the seal applies only to C++
types. A script component's dense id never leaves the process: saves, join
snapshots and the replication wire carry its name, and each world iterates its
own tables in the order it created them. So a late id cannot change what
another process or another world sees.

Declaring the same name again with the same fields agrees and returns `false`.
Declaring it with different fields raises an error that `pcall` can read:

```luau
local ok, message = pcall(function()
	World:DefineComponent("anatomy.Body", { Genome = "number" })
end)
-- ok == false, message names the component and says the fields differ
```

A client may not create a new definition. It learns definitions from the
server, or it declares one identically.

## Tags with behaviour

| Tag | Effect |
|---|---|
| `replicated` | The server shares the component with every client. The server writes and clients receive. A client that joins later gets the current values in its join snapshot. |
| `saved` | No extra effect. Every script component is saved, see below. Kept so older scripts still run. |

Every script component, tagged or not, carries its definition and tags in
`Store::Save` snapshots, join snapshots and `.agame` saves. A process that
never declared it can load any of them. This includes components a plugin
declared: a world saved while a plugin's component is on an instance reloads
with that component, whether the plugin has loaded yet or not.

## In a game file

`SaveGame`, `WriteWorldDocument` and the instance documents Studio copies and
moves write each script component on the instance that carries it:

```xml
<ComponentDefinitions>
  <Definition name="anatomy.Body" tags="replicated,saved"><Bytes encoding="hex">...</Bytes></Definition>
</ComponentDefinitions>
<Item class="Model" name="AnatomySubject" id="4">
  <Component name="anatomy.Body">
    <Field name="AnatomyGenome" type="string">...</Field>
  </Component>
  <Attribute name="Label" type="string">torso</Attribute>
</Item>
```

- Each definition goes once per document, before the instances. The loader
  registers it (legal after the seal) before it reads a row, so a game whose
  plugin has not loaded yet still gets its components back.
- A definition that disagrees with one this process already holds is refused
  with a log line naming the component. Every instance still loads, without
  that component.
- A field uses the property text form. A `reference` field is written only
  when its target is in the same document.
- Instance attributes are always written, one `<Attribute>` each.

## Engine components

`GetComponent`, `SetComponent` and `RemoveComponent` also take a component the
engine declared, such as `scene.Visual`. An engine component is a C++ struct
with no field list, so its fields are the scriptable properties that read it.
A write goes through those properties, so there is still one write path.

```luau
local visual = part:GetComponent("scene.Visual") -- { Transparency = 0, Color = ..., ... }
part:SetComponent("scene.Visual", { Transparency = 0.5 })
entity:SetComponent("scene.Visual") -- adds it at its defaults
```

- An instance answers from its own class. A classless entity from
  `World:CreateEntity` answers from every class that declares a property over
  that component.
- A missing component is added at its defaults before the values are written.
- `ecs.Hierarchy`, `ecs.InstanceClass`, `ecs.InstanceName`,
  `ecs.InstanceProjection` and `ecs.ClientLocal` are refused. They make an
  entity an instance and change only through the instance surface.

## Iterating

```luau
World:ForEach("anatomy.Body", "scene.Visual", function(entity, body, visual)
	entity:SetComponent("anatomy.Body", { Revision = body.Revision + 1 })
end)

local total = 0
World:ForEachParallel("anatomy.Body", function(entity, body)
	total += body.Revision -- reads only
end)
```

`ForEach` calls the function once per entity that has every named component.
Each value is what `GetComponent` returns. The matches are collected first. An
entity that an earlier call destroyed, or that lost a named component, is
skipped. So the function can write.

`ForEachParallel` is the read-only form. In its function, each of these raises
an error:

- `SetComponent`, `RemoveComponent`, `World:CreateEntity` and the component
  metadata calls
- a property assignment and `Instance.new`
- a method or host call whose name does not start with `Get`, `Find`, `Has`,
  `Is`, `Count` or `Equals`

The order of the calls is not promised. One Luau VM runs on one thread, so the
function runs on the script thread today. Because writes are refused, the
engine can move the calls to worker threads later and no script changes.

JavaScript has the same calls: `World.ForEach(...)` and
`World.ForEachParallel(...)`.

## How a client learns a definition

1. The server tags a component `replicated`. `Authority::DiscoverSchemas`
   starts to replicate it.
2. The server sends the definition by name, as field names, types, packing
   and enum sets. It never sends the dense id.
3. The client registers the definition. This is legal after the seal.
4. Rows arrive in the join snapshot and in deltas, by name.

If the client already holds a different definition under that name, it refuses
the definition message and the join snapshot. The client does not join, and
nothing reads rows through the wrong layout.

## Limits

| Limit | Value |
|---|---|
| Bytes in one `string` field value | 65536 (`Schemas::MAXIMUM_STRING_BYTES`) |
| Fields in one component | 256 |
| Tags on one component | 256 |
| Bytes in one encoded definition | 65536 |

A longer string is refused by `SetComponent` in Luau and JavaScript, by
`Schemas::WriteField`, and by the reader of a save or a delta. A 22 KiB base64
string fits.

## Tests

- `engine.ecs.schema`: late registration after the seal, the string cap, a
  `saved` component reloaded in a fresh sealed process, and a reload refused
  over a different declaration.
- `engine.replication.dynamicschemas`: a joined client and a late joiner get the
  same values, including a 22 KiB string; the late joiner's stream replays in a
  fresh sealed process; a client with a different declaration refuses to join.
- `engine.scripthost.ecs`: the post-seal refusal read from Luau, the string
  cap in Luau and JavaScript, engine components read and written through
  their properties, `ForEach` writes, and every write refused in
  `ForEachParallel`.
- `engine.game.roundtrip`: an untagged script component survives a save and
  load.
