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
| `saved` | `Store::Save` writes the definition and tags beside the rows, and so does a `.agame` save from Studio. A process that never declared the component can load either. |

A `replicated` component also carries its definition in snapshots, for the
same reason. Untagged script components are still in every snapshot, but a
process that loads one must declare them first.

## In a game file

`SaveGame`, `WriteWorldDocument` and the instance documents Studio copies and
moves write each `saved` component on the instance that carries it:

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
- Untagged script components are not written. Instance attributes are always
  written, one `<Attribute>` each.

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
- `engine.scripthost.ecs`: the post-seal refusal read from Luau, and the string
  cap in Luau and JavaScript.
