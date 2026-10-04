# Image cache authoring and retained source edits

The source callbacks for Image, Image Sequence and Image Animated cache their
live sprites. Image Animated Match Length uses the live sprite count, even
when an older cache is enabled. An empty live set leaves animation length
unchanged. Remove Cache disables cache_use and retains cache_data. These
facts are pinned to source revision b69eca232217360cf1502ef0223523d818606652.

The import retains the original source attri.cache_use/cache_data fields.
Native-generated layout metadata lives in the version 1 atomic_game_engine
sprite_cache envelope. Its data_hash binds the exact cache_data bytes. Unknown
source and envelope members stay intact. Disabled oversized text can remain
in raw source backing; an active cache that exceeds the native text carrier
stays opaque with a representation diagnostic rather than truncated data.

A pure source-image edit consumes an owned immutable frame observation with
exact authored node, authoring/input revisions and resolved controls. The host
producer attests that EncodedCache was encoded from those live frames. This
module does not open files, decode cache pixels or retain a provider. Cache,
Remove Cache and Match Length publish document and rebound GroupReplay owners
together. Old and candidate payloads/replay capacities are admitted before
clone; failure retains both previous outputs. History has its own host quota.

Match Length updates the total frame count and its native playback projection.
An explicit source endpoint beyond the new total remains independently saved
in SourceBounds, including end 12 when the live count becomes 2. Source intent
is not silently clamped. Checked PXC inverse preserves unknown records and
unchanged archive bytes. Engine layout annotations do not establish that an
official source application retains those unknown members after its own save.
