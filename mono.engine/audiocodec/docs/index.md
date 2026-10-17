# Audio codecs

Pure WAV and MP3 decoding from bounded byte spans, with owned interleaved float
samples. Public `engine/audio/Sample.hpp`, `Wav.hpp` and `Mp3.hpp` are exported by
this shared module. The client audio module owns mixing and device playback.
