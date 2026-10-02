# imagegraphexport module invariants

L11, shared. This is the process-local filesystem and codec boundary shared by
Studio and command-line programs. It depends on core, assets, parallel, imagegraph,
bake and imagegraphio. It must not depend on product libraries, tools, renderer,
GPU APIs, scripting runtimes, or device state.

## Explicit ownership

The host supplies the immutable document, compiled plan, decoded images,
recorded observations, provider, exact output directory and encoder executables.
Graph-authored paths cannot widen these grants. External processes use literal
argv, bounded deadlines and staged byte limits. Never interpret graph text as
an ambient shell command or resolve a codec through PATH.

## Publication and replay

Bound frame counts, dimensions, decoded buffers and output bytes before growth.
One animation range shares one replay owner across all frames. Publish complete
outputs from exclusive staging directories. A failed export must preserve the
previous output. Retained debug frames remain inside the granted destination,
with their exact directory returned to the caller.

## Native profile honesty

The CPU GIF profile has documented deterministic quantization and alpha rules.
It is not proof of byte or palette equality with an unpublished GameMaker
encoder. Source parity gates remain explicit until native observations exist.
