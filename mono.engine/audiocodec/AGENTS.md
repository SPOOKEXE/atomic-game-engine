# audiocodec - module invariants

L8, shared. Pure sample buffers and bounded audio decoding. Existing public
headers retain the `engine/audio/` paths and `engine::audio` namespace so device
and host callers use one decoder and one sample representation.

## Bytes in, owned samples out

This module depends only on core and the private minimp3 decoder. It never opens
files, acquires devices, reads world state, schedules playback, starts threads,
or depends on the client audio mixer. Hosts decide where encoded bytes come from.

Reject malformed lengths before reading or allocating. WAV input bytes and MP3
expanded sample counts have explicit limits. Unsupported formats are refusals,
not guessed conversions. A decoded sample buffer owns its values.

## Preserve decoder behavior

WAV truncation is refused. MP3 may retain complete independently decoded frames
before a truncated tail. Keep their existing fixtures and suite identifiers when
moving ownership. Headers expose no vendor types. Device callback and sample
clock rules remain in the higher audio module.
