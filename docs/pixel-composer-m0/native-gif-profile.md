# Native GIF encoding profile

`bake::WriteGif` accepts borrowed equal-size RGBA8 frames and nonzero centisecond delays. It emits independent full frames, binary alpha (below 128 transparent), per-frame palettes and an explicit loop count. Zero repeats indefinitely.

Quality 0 through 3 retains 2, 4, 6 or 8 channel bits before deterministic weighted median palette reduction to 255 opaque colours. Quality 3 preserves exact colours when a frame has at most 255 distinct opaque colours. GIF cannot preserve partial alpha.

The encoder caps frames at 4096 and total pixels at 16 million. The caller's byte limit bounds output plus encoder-owned peak workspace. Invalid input or allocation failure preserves the previous output. The codec performs no filesystem or process operations.

Validation: the dev bake suite passes 48 assertions in three cases. An independent Pillow decode also verified the 401-pixel, two-frame fixture, transparency disposal, 30/70 ms delays, multiple LZW clear intervals and sub-blocks.

This is a native CPU profile. GameMaker's opaque quantizer, fractional-delay narrowing and black-canvas compositing remain executable parity gates. The source `gif_open` third argument is a clear colour, not a loop count.
