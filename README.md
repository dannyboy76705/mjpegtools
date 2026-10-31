# mjpegsuite

Two small programs for keeping video as a folder of JPEG pictures and getting it back as a seekable `.mkv`.

* **mjpegstore** turns a progressive y4m stream into JPEG files, lists a set for an archiver, and writes a clip as a Matroska (MJPEG) stream or file.
* **mjpegmount** mounts a set read-only (FUSE) and shows every clip as a real `.mkv` file, so a player or editor can open it and seek without anything being exported first.

The pictures are the point. Every frame is made the same way (same quantization, same standard Huffman tables, a restart marker after every block of 16x16 pixels), so a part of the picture that does not change from one frame to the next comes out as the same bytes, aligned the same way. A compressor that looks back over the set (zstd, xz) can then match it. JPEG is lossy: the quality is in `specs.txt`.

## Build

Needs a C compiler, make, libjpeg (development package), and libfuse3 with pkg-config for `mjpegmount`.

```
apt install build-essential libjpeg-dev libfuse3-dev pkg-config     # Debian / Ubuntu
make
make install PREFIX=/usr/local
```

`make install` also installs the man pages (for example `man mjpegstore`).

## A set

A set is a folder with a `specs.txt` and one subfolder per clip:

```
SET/specs.txt            resolution, sampling, chromasiting, framerate, bitdepth, aspect, quality, range
SET/ep1/b00000/frame-0000001.jpg    pictures, 5000 per bucket folder
SET/ep1/b00000/frame-0000002.jpg
...
SET/ep2/...
```

`specs.txt` is created from the stream header on the first ingest. If it exists, later ingests must match it. `mjpegstore -c SET` checks it (and creates it if missing), and `-c -E` edits it, showing each value with its valid choices and writing nothing until you confirm.

### The settings in `specs.txt`

One `key=value` per line; blank lines and lines starting with `#` are ignored.

| key | values | notes |
|---|---|---|
| `resolution` | `WIDTHxHEIGHT`, each side 1 to 65535 | `resolution=720x480` |
| `sampling` | `420`, `422`, `444` | chroma subsampling: 4:2:0 (half width and height), 4:2:2 (half width), 4:4:4 (full) |
| `chromasiting` | `jpeg`, `mpeg2`, `paldv` | 4:2:0 only. `jpeg` is centred (JPEG, MPEG-1, most web video; also written `center`, `mpeg1`), `mpeg2` is left-aligned (most DVD sources; also `left`), `paldv` is PAL DV (also `topleft`) |
| `framerate` | a fraction `N:D`, such as `24000:1001` | reduced for you (`48000:2002` becomes `24000:1001`). Common: `24000:1001` (23.976, film and most anime), `24:1`, `25:1` (PAL), `30000:1001` (29.97, NTSC), `30:1`, `50:1`, `60000:1001`, `60:1`. Decimals like `29.97` are refused: use the exact fraction |
| `aspect` | a pixel aspect ratio `N:M` | `1:1` square pixels; `10:11` NTSC 4:3; `40:33` NTSC 16:9; `12:11` PAL 4:3; `16:11` PAL 16:9; any other ratio works |
| `range` | `limited` or `full` | `limited` is 16 to 235, normal video (also `tv`, `broadcast`, `mpeg`); `full` is 0 to 255 (also `pc`, `jpeg`) |
| `bitdepth` | `8` | the only value today; see the TODO below |
| `quality` | 1 to 100 | JPEG quality, default 90. Keep it the same for every clip of a set |

Most of these come from the stream header on the first ingest, and `mjpegstore -c -E SET` shows the choices for each one when you edit them. A stream whose settings differ from an existing `specs.txt` is refused.

Only **progressive** footage is stored. An interlaced stream is refused: deinterlace first (to 60p, ideally with a motion-aware deinterlacer) and feed progressive frames.

## Use

```
mjpegstore -i ep1.y4m SET/ep1                 # y4m file (or - for stdin) -> JPEG pictures
ffmpeg -hide_banner -loglevel quiet -nostdin -i ep3.mkv -vf scale=720:480 -f yuv4mpegpipe - | mjpegstore -i - -p SET/ep3
                                              # any video ffmpeg can read, scaled, straight in (stdin)
mjpegstore -s -f ep1.mkv SET/ep1              # write a clip as Matroska
mjpegstore -x ep1.mkv SET/ep1                 # same, asks before overwriting
mjpegstore SET/ep1 SET/ep2 > list.txt         # the files, in natural order, for an archiver
mjpegmount SET /mnt/set                       # clips appear as /mnt/set/ep1.mkv, ...
fusermount3 -u /mnt/set
```

Every command prints its options with `-h`.

In the `ffmpeg` line, the options before `-i` are yours to choose: `-hide_banner` and `-loglevel quiet` keep ffmpeg's banner and its own progress text off the terminal, so only `mjpegstore`'s output shows, and `-nostdin` stops ffmpeg from reading keystrokes from the terminal while it feeds the pipe. `-p` goes on the `mjpegstore` side because a stream has no known length, so there is no progress bar; `-p` shows a running count of frames read, so a slow upstream doesn't look like a hang. (A y4m file has a progress bar already.) Feed it progressive frames, as above: `mjpegstore` refuses an interlaced stream.

### Ingest options

* `-j N` JPEG quality for a new `specs.txt` (default 90). Ignored when `specs.txt` exists, so every frame in a set matches.
* `-R N` a restart marker after every N MCUs (an MCU is 16x16 pixels for 4:2:0). Default 1, the smallest pieces, which gave the smallest archives; 0 means one marker per row of blocks. Use the same `-R` for every clip of a set: it is not kept in `specs.txt`.
* `-y` accept the settings detected from the stream header without asking.
* `-t N` worker threads (default: number of CPUs).
* `-p` with `-i -`, show a running frame count; `-q` quiet.

### The list

`mjpegstore DIR...` prints the set's files in natural order (numbers compare as numbers), with no hashing and no reading beyond the directory scan. The list begins with the set's `specs.txt`, so an archive built from it carries the file `-s` and `mjpegmount` need. A folder that holds only subfolders (several clips at once) is refused rather than walked as one pile: name or glob the clips. `-f FILE` writes the list to a file and never appends; it asks before overwriting.

## The whole flow

```
ffmpeg -hide_banner -loglevel quiet -nostdin -i ep1.mkv -vf scale=720:480 -f yuv4mpegpipe - | mjpegstore -i - -p SET/ep1
                                               # or: mjpegstore -i ep1.y4m SET/ep1 (a y4m file)
mjpegstore SET/ep1 > list.txt                  # natural order
sortof-palscore SET/ep1 -o list.txt            # optional: scene order (see sortof-suite)
zfrag pack -T list.txt -o set.tar.zfrag        # or: tar -T list.txt -cf - | zstd -9 > set.tar.zst
```

`zfrag mount` shows an `mjpegstore` set inside an archive as one `.mkv` per clip under `virtual/` and the archive's own files under `native/`, with no second mount on top. See the zfrag README.

## mjpegmount

```
mjpegmount [fuse options] SETDIR MOUNTPOINT
mjpegmount -f SETDIR MOUNTPOINT            # foreground, errors on the terminal
fusermount3 -u MOUNTPOINT
```

Each clip appears as `CLIP.mkv`, the exact byte stream `mjpegstore -s` writes, except for the writing-application name in the header (`mjpegmount` instead of `mjpegstore`). The JPEG files go into the Matroska blocks untouched, and every size in the file follows from the sizes of the picture files. When a clip is first touched its files are listed and their sizes added up, which fixes every offset in the virtual file. A read at any offset then maps onto a few header bytes and the picture file(s) behind them. Nothing is decoded, re-encoded or scanned, and only the pictures a read touches are opened.

The picture files can live anywhere: plain disk, or another mount. A valid `specs.txt` is required in the set folder; the mount never writes it.

## What the settings cost and save

These come from tests on anime footage while the tools were built:

* Whole frames beat separate fields by about 16.5% in the compressed archive.
* Quality 90 is about 35% smaller than quality 95.
* A restart after every MCU (`-R 1`) is 17 to 21% smaller than one per row.
* Putting the pictures in a similarity order changes the compressed size by about 0.1% on real footage. It matters for exact duplicates (see `sortof-palscore -D`), where it saves a great deal, and for how an archive plays when mounted.

## TODO

* **10-bit support.** Today only 8-bit streams are stored. 10-bit is planned for the Debian forky stable release, whose libjpeg-turbo (3.1.3) has the higher-bit-depth JPEG support it needs.
* **lossless support** same thing. waiting on forky. will implement as 1 row per restart marker which will save a bit on scrolling footage as the line moves but is usually the same 

## Questions and answers

**Why make this? Don't other solutions exist?**
There are solutions that focus on small files or on high-quality masters, but I found none that kept each frame as a picture and let me deduplicate it against a pool of pictures from other clips. I wanted a video pool, and also the ability to inspect and use a single frame in other projects with ease, without a lossless base and its massive storage requirements. I also wanted it to play on low to moderate hardware at 480p to 720p without a GPU (think AviSynth, ffmpeg encoding).

**Why JPEG?**
It is universal, established, and is not going anywhere. The pictures can be viewed in any viewer, and they are easy to stream or package into a Matroska container. At high quality settings it is suitable for home archiving.

**Why not MPEG-2 intra?**
As a stream you cannot deduplicate it later, and as individual files you cannot easily view them. JPEG also supports restart markers, which help later compression; MPEG-2 does not.

**Why not MPEG-2, MPEG-4 and other video codecs?**
I wanted something easy on resources, deduplicatable, and high quality on every frame. MPEG-2 with P and B frames starts to drift and does not allow deduplication. AVC, HEVC and AV1 are fantastic, but they need a lot of CPU to decode, they do not go easily into one picture per frame, and even then they do not deduplicate easily.

**Why not FFV1?**
I tried it. Even in frame mode it was bigger in some cases (mainly anime) than zstd on a yuv4mpeg stream. Put into a frame-per-file layout it still did not deduplicate, because of how its Huffman tables work.

**Why not yuv4mpeg?**
This project did start that way. For home use, 13 GB per 20-minute episode adds up fast. MJPEG at quality 90 to 95 is visually lossless enough that I find it suitable for home archiving.

**Why no interlace support?**
I tried it with one field per file. The file size and the extra logic needed to make it work were a burden, and it would be limited to CPU decoding forever. Progressive MJPEG at least supports GPU decoding on some platforms.

## Thank you

* **[Debian](https://www.debian.org)**: for a solid distribution that does not move, which makes supporting a version for years instead of days possible.
* **The [libfuse](https://github.com/libfuse/libfuse) team**: for making virtual file serving for encoders possible.
* **The [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) team**: for a good encoder that exposes all the features of the spec, most notably the restart markers.
* **The [Matroska](https://github.com/Matroska-Org) team**: for a good container that makes muxing on the fly easy.

## License

MIT. Copyright (c) 2026 Daniel Lee Witzel. See `LICENSE`.

### Third-party libraries

`mjpegstore` and `mjpegmount` link to libjpeg (libjpeg-turbo: IJG, BSD-3-Clause and zlib licenses), and `mjpegmount` also to libfuse3 (LGPL-2.1). Nothing of them is included: the programs use the copies installed on the system.

If you package or distribute binaries: with dynamic linking to the system libraries (as a Debian package does) nothing more is needed beyond shipping the `LICENSE` file. If you link a library statically or bundle it, you must also include that library's own license text, and for libfuse (LGPL) let users replace it with their own build.
