# Native audio and movie-decoding replacements

Status: resolved research for “Research native audio and movie-decoding
replacements.” No private files, assets, extracted names, or runtime output
were inspected.

## Decision

Use two separable host seams:

1. Decode CRI ADX into interleaved signed PCM behind a small, tested decoder
   interface. Feed PCM to XAudio2 source voices through a bounded queue and
   recycle buffers from `OnBufferEnd`. Keep guest-visible DirectSound state,
   3D parameters, loops, and lifecycle in the model; keep host mixing and
   output in the renderer.
2. Leave legacy Sofdec/SFD decoding unresolved until the exact container and
   codec variant is identified using public-safe static facts and synthetic
   fixtures. Then choose either a narrow clean-room demux/decode adapter for
   standard elementary streams or a licensed CRI runtime with explicit legacy
   support and redistribution permission.

The default Windows x64 audio backend is XAudio2 2.9. Microsoft positions
XAudio2 as DirectSound's replacement and provides dynamic buffers, source-rate
conversion, operation ordering, and completion callbacks. WASAPI remains a
lower-level fallback and would require the project to own more mixing and
device policy. See [XAudio2 introduction](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-introduction),
[XAudio2 key concepts](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-key-concepts),
and [Core Audio APIs](https://learn.microsoft.com/en-us/windows/win32/coreaudio/about-the-windows-core-audio-apis).

## Audio architecture

XAudio2 source voices map naturally to streamed PCM. Its buffer contract
supports play and loop ranges and requires audio data to remain valid until
completion. Use a single audio-thread command queue or ordered operation sets
so guest calls remain deterministic. Generated guest code must not call the
host API directly. See [`XAUDIO2_BUFFER`](https://learn.microsoft.com/en-us/windows/desktop/api/xaudio2/ns-xaudio2-xaudio2_buffer)
and [XAudio2 callbacks](https://learn.microsoft.com/en-us/windows/win32/xaudio2/callbacks).

WASAPI can render PCM through `IAudioClient` and `IAudioRenderClient`, but it
leaves voice scheduling, mixing, resampling, spatial behavior, and queue policy
to this project. Media Foundation's Source Reader is useful for decoded media
samples, but Microsoft does not recommend it where the application needs a
presentation clock. See [WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi)
and [Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/source-reader).

## ADX decoder choice

ADX is a CRI-specific ADPCM stream family rather than a Windows-native format.
The public vgmstream documentation covers multiple ADX header, loop, fixed,
exponential, and encrypted variants. This establishes that a pull decoder is
feasible, but not which subset this port needs. See the
[vgmstream format list](https://github.com/vgmstream/vgmstream/blob/master/doc/FORMATS.md)
and [usage notes](https://github.com/vgmstream/vgmstream/blob/master/doc/USAGE.md).

| Option | Assessment |
| --- | --- |
| Pinned libvgmstream | Best rapid compatibility candidate. Review the exact API, commit, BSD notice, transitive codecs, and binary custody before adoption. |
| Constrained FFmpeg build | Technically viable for ADX, but broader than needed. Record build flags and avoid configurations that alter GPL/LGPL or nonfree redistribution status. See the [FFmpeg license](https://github.com/FFmpeg/FFmpeg/blob/master/LICENSE.md). |
| Narrow project-owned decoder | Preferred long-term seam if a small required subset can be established using public-safe fixtures and independent implementation. |

Do not add a dependency yet. First define `decode_next_pcm()` around synthetic
fixtures that prove header parsing, channels, sample rate, block geometry,
sample counts, loops, and buffer completion.

## Sofdec/SFD boundary

The game's SFD files and the player API they need are mapped in
[sofdec-player-api.md](sofdec-player-api.md).

Current CRI material describes modern Sofdec products and `.usm` workflows; it
does not establish legacy Xbox SFD compatibility or grant redistribution rights
for an old runtime. See CRI's [Video Quick Start](https://www.criware.com/en/products/CRIWARE_Video_Quick_Start.pdf)
and [Sofdec Encoding Wizard](https://blog.criware.com/index.php/2024/12/23/the-sofdec-encoding-wizard/).

Do not treat successful MPEG decoding as proof of SFD support. The format gate
must identify the SFD version, tracks, video codec, ADX variant, timestamp and
interleave rules, and frame pixel format without publishing game material.
Media Foundation is not a solution unless a compatible source/demux component
is independently proven. FFmpeg is only a candidate if the payload uses
supported standard elementary streams. A CRI runtime is only a candidate with
written legacy-support and redistribution terms.

## Next gate

Create a public-safe format probe over synthetic byte arrays. It may report only
format version, channel count, sample rate, block geometry, loop metadata, and
packet timestamps. If that proves a conventional ADX subset, implement or pin a
decoder behind `decode_next_pcm()` and verify loop boundaries plus XAudio2
buffer recycling. Do not select a movie decoder until the exact SFD variant and
custody decision are closed.
