# Native audio

The Windows runtime sends game PCM and decoded Xbox ADPCM to XAudio2.
Submitted buffers own their samples; guest memory can be reused immediately.
Playback tracks play/loop regions, frequency, volume, seek, stop, replacement,
and release. CRI status waits service the existing registered audio worker.

Set `RECOMP_AUDIO_GAIN=1` for full host gain, or a finite value from 0 to 1.
The default is muted. An unavailable output device leaves guest timing active.
The backend uses each source's sample rate; it does not force 22 kHz output.

A pump thread feeds XAudio2 from guest buffers on its own, so frame hitches,
loading, and window drags delay only the game's refills, not audio it already
wrote. Each new or starved voice starts behind 50 ms of silence. XAudio2
consumes some rates slightly off nominal (22050 Hz runs 0.23% fast without its
resampler, 0.05% slow with it), so each voice trims its pitch by at most 1% to
hold that cushion. The exit line `[audio-output] summary` reports
`dropped_buffers`, `underruns`, and `engine_glitches`; nonzero values point
to audible gaps.

Before regenerating the game, apply the FSUBP source correction documented in
[`tools/xboxrecomp-patches`](../tools/xboxrecomp-patches/README.md).
It fixes an incorrect x87 destination that corrupted ADX filter coefficients.
Generated source and game data remain private.

Issue [#15](https://github.com/NoRain211/doaxbv-re/issues/15) was accepted after
repeated natural title/menu, activity, pause, and return flows with audible
music and effects. The user confirmed synchronization. General game performance
stalls remain separate work; physical device unplug/reconnect is unproven.

The CMake checks `recomp-runtime-xbox-adpcm`, `recomp-runtime-audio-output`,
and `recomp-runtime-four-cases` cover codec samples and validation, backend
buffer lifetime/error handling, and adapter playback controls respectively.

## Custom soundtracks

Players drop MP3, WAV, or FLAC files into `private\UserMusic` in the release
folder, which the launcher's **Music folder** button opens, and pick them in
the game's own custom soundtrack screen, such as the Radio Station playlist.
`RunGame.cmd` passes that folder to the runner as `RECOMP_USER_MUSIC`; a
runner started without it uses UserMusic beside the imported XBE. The runner
creates the folder at startup if it is missing and scans it once, so added or
removed files appear after a restart.

Decoding uses Windows Media Foundation, so macOS and Linux builds have an empty
catalog; a portable decoder is open work.

The game stays in control: it enumerates the soundtrack, chooses the song,
starts and stops it, and mixes it through CRI and DirectSound. The runtime
replaces only the Xbox services underneath, the XAPI soundtrack catalog and
the WMA decoder object, and decodes with Windows Media Foundation.
[Custom soundtracks](custom-soundtracks.md) describes the design, the guest
contract, and the checks.

- **Catalog.** One soundtrack named UserMusic holds up to 500 songs, sorted
  case-insensitively. Names are the filename stem, cut to 31 UTF-16 units. A
  song's ID is a hash of its filename, so adding files keeps existing IDs and
  renaming a file changes its ID. Files that fail a trial decode are skipped
  with a [custom-music] skipped line, and the startup line
  [custom-music] catalog tracks=N scan_ms=M reports the result.
- **Formats.** PCM WAV, float WAV, FLAC, and MP3 pass the automated decoder
  checks. Other WAV encodings depend on the codecs installed in Windows.
  Output is 44.1 kHz 16-bit stereo, so high-resolution sources are resampled.
  Originals are opened read-only and no audio cache is written.
- **Requirements.** The feature needs a program generated from the current
  recipe, because the recipe routes the game's direct soundtrack calls through
  manual dispatch. Media Foundation is delay-loaded: on Windows N editions
  without the Media Feature Pack, the runner starts with custom music disabled
  and logs [custom-music] disabled.

An automated agent run reached the playlist and logged a decoder opening for
each format. Natural selection, audible playback, playlist play-through, next
track, end-of-track advance, seeking within a song where the game offers it,
volume, stop, and return to the game's own music still need a user play test
of a named build.

## Radio shuffle

The game has no random play: its music player keeps each playlist's next
position one past the playing song, adds one after every song, and wraps at
the end. Each playlist entry also carries a location mask, and the player
skips songs not allowed where the player is.

With the launcher's **Shuffle music** checkbox, or `RECOMP_MUSIC_SHUFFLE=1`
for a runner started directly, the runtime wraps the two player functions
that start songs (`0x000B8F70`, which starts the playlist song, and
`0x000B9790`, the per-frame update; EAX is the player). Each runs
unchanged. When it leaves the playlist's next position at the game's
sequential step, the wrapper replaces it with a random entry allowed at the
current location, never the playing song, and logs `[music-shuffle] next=N
of M`. The recipe routes the direct calls to both functions through manual
dispatch. The host random source is used, so the game's own `rand`
sequence is unchanged. Saved playlists and their order are unchanged.

"Previous" still steps back one entry from the shuffled next song. The goal
is an option in the game's own PC settings screen (#77, #22) that replaces
the launcher checkbox.

| Player field | Meaning |
|--------------|---------|
| `+0x658` | Active playlist |
| `+0x6E0` | Current location, 0-7 (signed byte) |

| Playlist field | Meaning |
|----------------|---------|
| `+0x000` | Entry count, at most 100 |
| `+0x644` | Pointer to the entry array, 16 bytes per entry; entry `+6` is the 16-bit location mask, `+0xC` the song ID |
| `+0x64C` | Next position |
| `+0x650` | Playing position |
