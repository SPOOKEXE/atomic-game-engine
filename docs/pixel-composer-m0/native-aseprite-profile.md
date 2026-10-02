# Native Aseprite byte profile

The byte-only reader owns decoded frames, palette entries, layer metadata, tags, linked cels, embedded tilesets and the source-shaped inspection tree. The inspection JSON keeps ordered frame/chunk fields and bounded binary buffers. Rendering selects a frame or named layer subtree, with optional cel crop and opacity.

Counts, frame/chunk boundaries, pixel sizes, inflation and retained bytes are checked before publication. External tilesets require a host dependency and are refused by this byte-only API. Failure preserves the prior document or image.

The committed fixtures are independently authored indexed, linked, tagged and tilemap images. Upstream sprite assets used during exploratory verification are not included. The dev bake suite passes 146 cases and 15,714 assertions, including exact raw pixel byte counts and frame-boundary checks.

This validates the native codec. Source node controls, imported project edits and licensed executable comparison have separate integration gates.
