# imagegraphio - module invariants

L10, `shared`. This adapter may see the L9 `bake` PXCX reader and L9
`imagegraph` document. Neither lower module may link back to it.

## The source archive is authoritative

Every import result keeps the checked PXCX archive, including its original
bytes. The native document is a projection of only source-backed facts. Do not
claim that writing the native document produces a Pixel Composer project, and
do not silently replace an unsupported foreign node with a plausible native
effect. Keep an opaque canvas node and a diagnostic instead.

## No files, runtime, or device state

The import accepts an in-memory parsed archive. This module does not open
paths, execute Pixel Composer, load plugins, render pixels, or depend on Studio,
client, server, or renderer. A host can choose where bytes come from. Tests may
read optional external fixtures, but production code never does.

## Semantic mappings require complete controls

Map a foreign node only when its serialized input indices, authored values,
animation state, and linked ports have a supported native meaning. A linked
control or unsupported output index demotes that node to opaque. Positional
foreign sockets stay positional on opaque nodes. A native socket name is used
only for a mapped node and source-backed index.

## Version and resource bounds

The semantic map is pinned to save version `121092` observed in the supplied
projects and the public source at commit `b69eca232217360cf1502ef0223523d818606652`.
Other versions remain structurally visible and opaque. The reader's 64 MiB
archive cap and the imagegraph node, link, output, dimension, and text caps
must be checked before building vectors or assigning native properties.
