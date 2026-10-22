# Sofdec movie player API

Status: static map for step 1 of a native movie player. Read from the
supported XBE (SHA-256 `053d44e8…`) with bounded Ghidra queries. Addresses
are guest virtual addresses.

## Format

Every movie is a CRI Sofdec SFD file:

- an MPEG-1 program stream, with the `SofdecStream` header in private
  stream 2 (`0xBF`);
- MPEG-1 video on stream `0xE0`: 720x480 at 29.97 fps, 10–17 Mbit/s;
- standard CRI ADX audio on stream `0xC0`: 4-bit ADPCM, stereo, 48 kHz.

The game's movie table at `0x2C2448` has 21 entries. Four of the disc's SFD
files are 262 KB and were not probed further.

## Library

The game links CRI's MWSFD 2.26 player, its SFD 1.817 demuxer and its
MPV 1.813 MPEG decoder, all built in September 2002. The player is an object
whose first word points at a function table at `0x284168`.

## How the game drives it

| Game function | Role |
|---|---|
| `0x0B4E20` | Once at boot, from `0x0BB2A0`: `mwPlyInitSfdFx` with a 59.94 Hz display rate; reads the DirectSound speaker configuration. |
| `0x0B4EA0` | Start movie `EAX` (0–20), from `0x0A4E00`, `0x0CE6B0` and `0x0DEC10`. Creates two textures, sizes and creates the player, sets audio, starts the file, then shows the first frame. |
| `0x0BB310` | Main loop. While a movie plays it waits for vertical blank, ticks CRI server lane 4 (`0x193E40`), then takes, converts and releases one frame. |
| `0x0B52A0` | Draw, from `0x0A4E00`, `0x0CE480` and `0x06B520`. Reads the status each frame: 3 clears the playing flag, 4 stops drawing, 2 or more draws the current texture as a full-screen quad. |
| `0x0B53D0` | Stop, from `0x0A4710`, `0x0A4E00` and `0x0CE6B0`: stop, destroy, free the surround audio. |

`0x0B4CE0` creates two 720x480 linear A8R8G8B8 textures (pitch 2880) and
`0x0B4DA0` alternates between them: it locks the next one, points the
player's converter at it, converts the current frame and sizes the quad from
the frame's width and height.

### Calls the game makes

| Address | CRI function | Use |
|---|---|---|
| `0x199CB0` | `mwPlyInitSfdFx(iprm)` | Once at boot. |
| `0x198F80` | `mwPlyCalcWorkCprmSfd(cprm)` | Work buffer size for the creation parameters. |
| `0x199760` | `mwPlyCreateSofdec(cprm)` | Returns the handle. |
| `0x198550` | `mwPlyGetAdxtHn(h)` | Stereo path: the handle goes to `0x189210`, which sets DirectSound mix bins and ignores 0. |
| `0x1984E0` | `mwPlySetAudioSw(h, 0)` | Surround path only. |
| `0x198190` | `mwPlyGetCurFrm(h, frm)` | Fills the frame object; `frm[0] == 0` means no new frame. |
| `0x197EC0` | `mwPlyRelCurFrm(h)` | Returns the frame to the pool. |
| `0x1985F0` | `mwPlyFxSetOutBufSize` | Points the converter at the locked texture. |
| `0x1989F0` | `mwPlyFxCnvFrmARGB8888(h, frm, …)` | Converts the frame into it; this reaches the color conversion at `0x1A8A70`. |

Through the function table:

| Offset | Target | CRI function | Use |
|---|---|---|---|
| `+0x14` | `0x199680` | destroy | Stop path. |
| `+0x18` | `0x1A34A0` | `mwPlyStartFname(h, path)` | Start. |
| `+0x1C` | `0x1A3060` | `mwPlyStop(h)` | Stop path. |
| `+0x20` | `0x198320` | `mwPlyGetStat(h)` | Every drawn frame. |
| `+0x28` | `0x1A30B0` | `mwPlyPause(h, 0)` | Right after creation. |
| `+0x2C` | `0x198420` | `mwPlySetOutVol(h, vol)` | Stereo path, from the game's volume table. |

The game does not use the table's time, pan, output-volume query, memory or
stream-joint start entries.

Status values: 0 stopped, 1 preparing, 2 playing, 3 ended, 4 error.

### Structures

The initialization parameters at `0x5F2AF8` are the display rate as a float
(59.94), 1, 1 and 0. The last field selects how the decode server is
registered.

The creation parameters at `0x5F2BE0`:

| Offset | Value |
|---|---|
| `+0x00` | 1 |
| `+0x04` | maximum bit rate, 20,000,000 |
| `+0x08`, `+0x0C` | maximum width and height, 720 and 480 |
| `+0x10` | frame pool size, 5 |
| `+0x18`, `+0x1C` | work buffer address and size |

The frame object at `0x5F2BA0` is 16 dwords. The game reads `+0x00` (frame
buffer, nonzero when a frame is ready), `+0x08` width and `+0x0C` height,
and passes the whole object back to the converter. The player also fills the
picture structure (`+0x04`), picture type I/P/B/D as 1–4 (`+0x18`),
frame rate times 1000 (`+0x1C`), and the presentation time and time unit with
the frame numbers derived from them (`+0x20`–`+0x3C`).

### Audio paths

Stereo: the player decodes the movie's ADX track, the game routes it with
`0x189210` and sets its volume. Surround: when the speaker configuration has
bit 16 set and the movie index is below 4, the game turns the movie's audio
off and plays a separate six-channel ADX file through the ADX library instead.

### Decoding schedule

`mwPlyInitSfdFx` registers the player's servers with CRI's server lanes
(`0x193DD0` runs up to four servers per lane). The main loop ticks lane 4 once
per pass; a CRI worker thread does the rest of the decoding.

## Replacement seam

Replace the library at the calls above:

1. Register native replacements for the nine direct calls and the six table
   targets. Keep the table at `0x284168`: the native create returns a guest
   block whose first word points at it, so the game's indirect calls reach
   native code through normal dispatch. Decoder state stays on the host.
2. Return 0 from `mwPlyGetAdxtHn`; `0x189210` then does nothing. Send the
   decoded ADX audio to the host output with the volume from `+0x2C`.
3. Convert frames into the locked texture with the existing native color
   model, then retire the runtime's wrappers of `0x198320` and `0x1A8A70` in
   `cri_service_adapter.c`.
4. Leave the lane-4 tick alone; with no player servers registered it runs only
   the other CRI servers.

## Open questions

1. Where the color table passed to `0x1A8A70` comes from, and whether a
   native converter should reuse it.
2. The exact arguments of `mwPlyFxSetOutBufSize` and
   `mwPlyFxCnvFrmARGB8888`; the decompiler drops the register arguments the
   game passes.
3. Which speaker configuration the runtime's DirectSound reports, and so
   which audio path runs.
4. Frame pacing: the native player must release frames at 29.97 fps against
   the audio clock and report status 3 once the last frame is shown.
5. The packet timestamp and interleave rules of the SFD files, which the
   format gate in [audio-movie-replacements.md](../wayfinder/research/audio-movie-replacements.md)
   still requires.

## Video decoder

`movie_video.c` decodes the video with [pl_mpeg](https://github.com/phoboslab/pl_mpeg),
pinned and patched at configure time by `pl_mpeg_sofdec.cmake`. The SFD video
is MPEG-1 with two encoder extensions that stock pl_mpeg misreads:

- Intra DC is coded at 11-bit precision, as MPEG-2's `intra_dc_precision` 3:
  DC differentials up to size 11, a predictor reset of 1024 and no scaling.
  The precision is announced in TMPGEnc user data before the first picture
  (`IDCPREC`, an 8-digit length, then the value).
- Later sequence headers load new quantizer matrices, which stock pl_mpeg
  ignores after the first header.

With both fixes every SFD file on the disc decodes to within IDCT rounding of
ffmpeg: 48–60 dB luma PSNR on sampled frames. Slow, dark scenes drift to about
39 dB before the next I-frame. pl_mpeg withholds a stream's last picture, so
each movie ends one frame early.

Decoding the three largest movies took 3–4 ms per frame on a desktop PC and
18–23 ms in Eden on one core, against 33 ms per frame at 29.97 fps. The
slowest frames in Eden took 74–82 ms, so the player should decode ahead.

