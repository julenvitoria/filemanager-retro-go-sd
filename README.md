# File Manager — file explorer for Game & Watch (Retro-Go SD)

**English** | [Español](README.es.md)

GWHB homebrew for the Retro-Go SD 2.x firmware (`external_cores` branch) to manage the SD card
from the console itself: copy, move, rename, delete, view and edit files.

## Features

- **Browse** the whole SD card, folders first, in alphabetical order, with sizes.
- **Copy, cut (move) and paste** files and **whole folders**, with everything inside.
- **Rename**, **delete** (with confirmation), **create folders** and **create text files**.
- **Multi-select**: actions apply to everything selected.
- **View** text files, hex dumps and **images**: JPEG, PNG, GIF (animated too) and BMP (also the
  firmware's screenshots).
- **Play videos**: AVI with Motion-JPEG video and PCM sound (convert yours with one ffmpeg command).
- **Play music**: MP3 (also MP2) and WAV, going on with the next file of the folder.
- **Edit text** (`.txt`, `.cfg`, `.ini`…) with an on-screen keyboard.
- **Hex-edit** any file, with a hex keypad.
- **Information** about files and folders, and about the **SD card** (capacity, used, free).

The interface is in Spanish or English depending on the firmware's language (any other language
uses English).

## Installation

Copy `FileManager.bin` to `/homebrews/` on the SD card. It shows up in the **Homebrew** tab as
"File Manager".

---

# Manual

## 1. The file list

The main screen shows the current folder: the path at the top, the contents in the middle
(folders in yellow with a trailing `/`, files with their size), and at the bottom the cursor
position, how many items are selected (`*3`) and what is in the clipboard (`[2 to copy]`).

| Button | Action |
|---|---|
| Up / down | Move through the list |
| Left / right | Previous / next page |
| A | Enter the folder, or open the file (see below which viewer it uses) |
| B | Go up to the parent folder |
| TIME | Select / deselect the item (and move to the next one) |
| GAME | Actions menu |
| PAUSE/SET | Retro-Go menu (brightness, volume, exit, power off) |
| PAUSE/SET + D-pad | Brightness / volume shortcuts, with their on-screen indicator |

Pressing **A** on a file opens:
- `.jpg`, `.png`, `.gif`, `.bmp` → image viewer; also the `.img` cover thumbnails of Retro-Go
  (`/covers/...`), recognised by their content (other `.img` files, such as disk images, open in hex);
- `.avi` → video player (`.mp4`, `.mkv`, `.mov`… show how to convert them);
- `.mp3`, `.mp2`, `.wav` → audio player (`.ogg`, `.flac`, `.m4a`… show how to convert them);
- `.txt`, `.cfg`, `.ini`, `.log`, `.json`, `.xml`, `.csv`, `.md`, `.c`, `.h`… → text viewer;
- anything else → hex viewer.

## 2. The actions menu (GAME)

The options depend on where the cursor is and whether something is selected or in the clipboard:

| Option | What it does |
|---|---|
| Open | Same as pressing A |
| View as text | Opens the file in the text viewer, whatever its extension |
| Edit text | Opens the text editor (section 5) |
| View / edit in hex | Opens the hex viewer (section 6) |
| Copy / Cut (move) | Puts the item under the cursor, or all selected items, in the clipboard |
| Paste here (n) | Copies or moves the clipboard contents here |
| Rename | Changes the name with the name keyboard (section 4) |
| Delete | Deletes the item or the selected ones, folders with everything inside. Asks first, with "No" as the default |
| New folder | Creates a folder in the current folder |
| New text file | Creates an empty file and opens it in the editor |
| Information | Size and date; for folders, how many files and subfolders it has and their total size |
| Select all / Deselect all | Selects or deselects everything in the folder |
| Clear clipboard | Forgets what was copied or cut |
| SD card | Total capacity, used and free space (needs a recent firmware, see "Firmware") |
| Exit | Back to the Retro-Go menu |

## 3. Copy, move and paste

1. Select what you want with **TIME** (or just put the cursor on it if it is a single item).
2. **GAME → Copy** or **Cut (move)**.
3. Go to the destination folder and **GAME → Paste here**.

- A progress bar is shown, and **B cancels**. A file is never left half-copied.
- If something with the same name already exists, it asks: **overwrite**, **skip** or **cancel**.
- Pasting a copy into the same folder creates `name (2).ext`.
- It will not copy a folder into itself.
- Moving is instant with the newer firmware. With the older one it copies and then deletes the
  original.

## 4. The name keyboard (rename, new folder, new file)

A full-screen keyboard with the name at the top:

| Button | Action |
|---|---|
| D-pad | Move between keys |
| A | Type the key |
| B | Delete the last character |
| TIME | Switch between lowercase, uppercase and symbols |
| GAME | Accept |
| PAUSE | Cancel |

The bottom row also has **Space**, **Del**, **abc/ABC**, **OK** and **Cancel**. Names with
characters the card does not allow (`\ / : * ? " < > |`) are rejected.

## 5. The text editor

Open it with **GAME → Edit text**, with **A** inside the text viewer, or by creating a new file.
The text is at the top; the title shows the file name and the cursor's line:column (with `*` if
there are unsaved changes). A full keyboard is docked at the bottom.

The editor has two modes, switched with **TIME**:

| Button | Keyboard shown | Keyboard hidden (cursor mode) |
|---|---|---|
| D-pad | Choose a key | Move the cursor through the text |
| A | Press the chosen key | Show the keyboard again |
| B | Delete the previous character | Delete the previous character |
| TIME | Hide the keyboard (more lines visible) | Show the keyboard |
| GAME | Editor menu | Editor menu |

**Special keys:** Bksp (backspace), Del (deletes the character under the cursor), Enter (new
line), Tab, Space, arrows `< > ^ v`, Home/End (start/end of line) and PgUp/PgDn.
**Shift**: one press capitalises only the next key; two presses lock it (it shows `SHIFT`); three
release it.

**Editor menu (GAME):** Save, Save and exit, or Exit without saving (asks for confirmation). With
no changes it only offers Exit.

Details:
- Lines are not wrapped: when they are wider than the screen, the view scrolls sideways.
- Windows line endings (CRLF) and accented characters in the file are kept. On screen accents are
  shown without the accent because the small font does not have them, but they are saved intact.
- Saving first writes a temporary copy and then replaces the original, so a failure never leaves
  a half-written file. With the older firmware (no fast rename) it writes over the file directly.
- Maximum size for editing: 160 KB.

## 6. The hex viewer and editor

Each line shows the offset (in grey), 8 bytes in hex and the same 8 bytes as text on the right.

**Viewing:** D-pad up/down to scroll, left/right to change page, **A** to edit and **B** to leave.

**Editing:** an orange cursor appears, with the digit about to be typed in red. A hex keypad is
docked at the bottom:

```
 0 1 2 3 4 5 6 7  -1 +1
 8 9 A B C D E F  Bksp
 < > ^ v  PgUp PgDn  Text
```

| Button | Keyboard shown | Keyboard hidden (cursor mode) |
|---|---|---|
| D-pad | Choose a key | Move the cursor (left/right one digit at a time) |
| A | Press the chosen key | Show the keyboard again |
| B | Step back one digit and restore its original value | Same |
| TIME | Hide the keyboard | Show the keyboard |
| GAME | Menu: Save, Save and exit, Exit without saving | Same |

- **0-9 and A-F** write the digit at the cursor and move on to the next one, like a PC hex
  editor. To write `FF 00 12`, just press F, F, 0, 0, 1, 2.
- **-1 / +1** decrease or increase the byte under the cursor (to nudge a value).
- **Text** moves the cursor to the text column on the right and shows the full keyboard: every
  key you press is written as one byte. There, the **Hex** key goes back to the hex column. Del
  restores the byte under the cursor to its original value.

Safety:
- Changed bytes are shown in blue and the title has a `*` until they are saved.
- The file is not touched until you choose Save, and only the changed bytes are overwritten:
  nothing is ever inserted or removed, so the file always keeps its size.
- Up to 2048 changed bytes before saving.

## 7. The image viewer

Shows **JPEG**, **PNG**, **GIF** and **BMP** pictures of any size, fitted to the screen (small ones
are enlarged by a whole factor so they stay sharp).

| Button | Action |
|---|---|
| Left / right | Previous / next picture in the folder |
| A, B, GAME, TIME | Back to the list |

- JPEG: baseline (the usual kind, from cameras and phones). Pictures wider than 2560 px are
  decoded at 1/8 size, which is fast and still more than the screen needs. **Progressive JPEGs are
  not supported** (a message says so).
- PNG: every colour type and bit depth, transparency (shown over black) and interlaced files.
- GIF: animations play with their own timing.
- BMP: 8, 16, 24 and 32 bits, such as the screenshots the firmware takes with PAUSE + GAME.
- Retro-Go covers (`.img`, JPEG inside), so the thumbnails in `/covers` can be checked.
- Big files show "Loading..." for a moment.

## 8. The video player

Plays **AVI files with Motion-JPEG video (at most 320x240) and PCM sound**. Every frame is decoded
by the console's hardware JPEG decoder, and the picture follows the sound. The file name shows at
the top for the first 3 seconds.

| Button | Action |
|---|---|
| A | Pause / resume (shows time and a progress bar) |
| Left / right | Back / forward 10 seconds |
| Down / up | Back / forward 1 minute |
| B | Stop |
| PAUSE/SET | Retro-Go menu (volume, brightness…); the video is silenced meanwhile |
| PAUSE/SET + D-pad | Volume / brightness, with the indicator over the video |

Videos from phones or the internet (MP4, MKV, MOV, WebM… with H.264, H.265, VP9…) can't be
decoded by the console: opening one shows a message. Convert them on a PC with
[ffmpeg](https://ffmpeg.org/) (Windows, macOS and Linux):

```
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,scale=trunc(iw/2)*2:trunc(ih/2)*2,fps=15" -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 22050 -ac 1 output.avi
```

The `-pix_fmt yuvj420p` part matters: the console's JPEG decoder only handles colour 4:2:0 frames
(4:2:2, 4:4:4 or greyscale MJPEG get a message). This makes a 320x240 (or smaller, keeping the
shape) video at 15 frames per second with mono
22 kHz sound. `-q:v` sets the quality (2 = best and biggest, 10 = smallest); `fps=15` can be
raised if the card reads fast enough. A 4-minute clip takes about 30-40 MB. Audio in other PCM
rates is converted while playing; compressed audio (MP3, AAC…) plays without sound.

## 9. The audio player

Plays **MP3** (MPEG-1/2/2.5 layer 3, CBR or VBR; also layer 1 and 2, `.mp2`) and **WAV** (PCM 8,
16, 24 and 32 bits, 32-bit float, A-law and µ-law), mono or stereo, at any sample rate. The
console has one speaker, so stereo is mixed to mono.

The screen shows the waveform being played, the time and length with a progress bar, and the
format. For the first 3 seconds (and while paused) a bar at the top shows the song's title and
artist from its ID3 tags, or the file name.

| Button | Action |
|---|---|
| A | Pause / resume |
| Left / right | Back / forward 10 seconds |
| Down / up | Back / forward 1 minute |
| TIME | Next audio file of the folder |
| GAME | Previous audio file |
| B | Stop (back to the list) |
| PAUSE/SET | Retro-Go menu; the sound pauses meanwhile |
| PAUSE/SET + D-pad | Volume / brightness, with the indicator |

When a file ends (or you skip past its end) the next audio file of the folder plays, so a folder
works as an album. Other formats (OGG, Opus, FLAC, M4A/AAC, WMA…) and compressed WAV (ADPCM…)
show a message, as do files that hold no audio (such as a failed download that only kept the
tags and cover) and other formats renamed to `.mp3`; convert them with [ffmpeg](https://ffmpeg.org/):

```
ffmpeg -i input.flac -c:a libmp3lame -q:a 2 output.mp3
```

## 10. Tips
- To edit an emulator's configuration or a `.cfg`, open it with **GAME → Edit text**.
- Before touching a save file, make a copy (Copy → Paste here in the same folder creates
  `name (2).ext`) and hex-edit the original.
- If a text file does not open in the text viewer because of its extension, use
  **GAME → View as text**.

---

## Firmware

Works with any Retro-Go SD 2.x firmware (ABI v2, 908 bytes). The official firmware includes two
extra functions for it since commit
[d2b3868](https://github.com/sylverb/game-and-watch-retro-go-sd/commit/d2b3868c9c7980a0d48c28465234f12902528fdd)
("firmware ABI: expose rg_storage_rename and rg_storage_free_space", `external_cores` branch).
With a firmware from that commit on there are a few improvements:

| | Older firmware | Firmware with d2b3868 or later |
|---|---|---|
| Rename / move | Copy and delete (slow with big files) | Instant |
| Saving in the text editor | Directly over the file | Temporary copy + replace |
| SD card (capacity / free) | Not available | Yes |

The program detects the firmware by itself. It reads those two functions past the 908 bytes of
the ABI, only if the firmware's `size` field includes them (`src/fsops.c`).

## Building

With the Arm GNU Toolchain 15.2.rel1 (the same one retro-go-sd uses):

    make GCC_PATH=/path/to/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin

Or with Docker: `make docker`. The result is `build/FileManager.bin`.

To try the Spanish interface in the emulator without changing the firmware's language:
`make CORE_C_DEFS="-DPROJECT_KIND_HOMEBREW=1 -DFM_TEST_ES"`.

`sdk/` is the Retro-Go SD cores/homebrews SDK (ABI v2, 908 bytes, on purpose: so the program also
starts on firmware without the new functions).

## Code

- `src/main.c`: list, menus, clipboard, actions, input.
- `src/fsops.c`: paths and recursive operations (copy, move, delete, count) with progress.
- `src/osk.c`: name keyboard (full screen).
- `src/kbd.c`: keyboard docked at the bottom (text, hex, and the hex editor's text column).
- `src/editor.c`: text editor.
- `src/viewer.c`: text and hex viewers (with the hex editor).
- `src/image.c`: image viewer (JPEG, PNG, GIF, BMP) with a streaming scaler.
- `src/video.c`: AVI/MJPEG video player.
- `src/audio.c`: MP3/WAV audio player.
- `src/third_party/`: [picojpeg](https://github.com/richgel999/picojpeg) (public domain) and the
  inflate part of [miniz](https://github.com/richgel999/miniz) (MIT), for JPEG and PNG;
  [minimp3](https://github.com/lieff/minimp3) (CC0) for MP3.

## Status

Tested in the gwemu emulator, with the `external_cores` firmware with and without the change:

- browsing;
- text and hex viewers;
- image viewer: big (1/8) and normal JPEG, grayscale JPEG, progressive JPEG message, PNG
  (small enlarged, RGBA, palette, 16-bit, interlaced), animated GIF, BMP, browsing with
  left/right;
- video player: playback with sound, pause, seeking both ways, end of file, a video without
  sound, stereo 24 kHz sound (resampled), and the messages for MPEG-4, too-big and MP4 files;
- audio player: MP3 CBR/VBR/mono/22-48 kHz, MP2, WAV 8/16/24-bit, float, A-law, µ-law (all
  checked sample by sample against ffmpeg), ID3v1/v2 titles, seeking, pause, next/previous and
  end of file, the Retro-Go menu and the volume indicator, and the ADPCM/OGG messages;
- copying folders with subfolders: byte-identical result;
- moving (instant, and by copy + delete);
- renaming with the keyboard;
- creating and deleting folders;
- multi-select;
- pasting into the same folder;
- information, SD card and the PAUSE menu;
- hex editor: typing digits with the keypad, typing text in the text column, saving and
  discarding;
- text editor: typing, deleting, moving, Shift, saving (CRLF and UTF-8 intact), exiting without
  saving and creating a new file.

On the console, the file features (browsing, copying, moving, renaming, deleting…) have been
tested. The text and hex editors and the image and video viewers, so far, only in the emulator.

## License

MIT (see `LICENSE`). The headers in `sdk/include/` keep their original licenses; picojpeg is
public domain, miniz is MIT and minimp3 is CC0 (`src/third_party/`).
