# Image cache actions

Image, Image Array and Image Sequence inspectors provide Cache live images and
Remove cache. Image Sequence also provides Match live image count. Each action
uses the existing export-intent owner to freeze document/input revisions,
project frame, selected endpoints and source controls before preparing results.
Cancellation or stale revisions discard the result. Accepted document edits use
one history transaction and rebind the replay owner before publication.

Cache and Match read live originals under explicit file grants even when an
older saved cache is enabled. Remove requires controls only and disables use
while retaining the saved cache text and its layout metadata. Matching an empty
live sequence leaves length unchanged. Matching a nonempty sequence preserves
explicit source endpoints, including an end beyond the new project length.

Saved-cache playback needs no original file grant. Native cache writes name an
RGBA8 top-down layout and bind it to the exact cache-text hash. Foreign caches
require an explicit matching layout observation. The inspector exposes four
named channel/orientation choices and allows revocation. Changed cache text or
conflicting layout facts refuse playback instead of guessing pixel storage.

PXC edits preserve unknown node, input and cache-envelope fields. Native saves,
checked PXC reloads, shared export pixels and actual assetc subprocess checks
cover this workflow. Six headless ImGui cases activate the real inspector
controls and exercise intent admission, cancellation, stale results, history
refusal, undo/redo and source-range reset. Live Studio remains unverified.

These image-source sprite caches are distinct from Cache and Cache Array frame
journals. Group loading, selected Clear and fractional frame-cache playback
remain separate implementation and verification work.
