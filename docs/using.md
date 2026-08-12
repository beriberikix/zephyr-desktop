# Using the desktop

Assumes you have it booted — if not, the [README](../README.md) is six commands.
This is what to do with it once the window is open.

## The desktop

A patterned background, and a taskbar along the bottom with four things on it:

| | |
|---|---|
| **Start** | the launcher. Lists every zapp found on the filesystem. |
| *(middle)* | one button per open window; click to raise and focus it, click again to minimise. |
| **abc** | shows and hides the on-screen keyboard. |
| **10:18** | the clock. See the warning below — it is not the real time. |

Clicking the background defocuses everything. The taskbar is always on top of
every window; you cannot lose it behind one.

## Windows

- **Move** — drag the titlebar.
- **Resize** — drag the grip in the bottom-right corner. That corner only;
  there is no edge-dragging and no maximise.
- **Minimise** — the `_` box, or the window's taskbar button. It keeps its place
  in the stacking order, so restoring it puts it back where it was.
- **Close** — the `X` box.
- **Raise** — click anywhere in the window.

A zapp is allowed to *decline* a close — that is how "save changes?" works. If
one does, it stays open and puts a dialog up. If a zapp stops answering
altogether, the desktop closes it anyway after a couple of seconds.

## What is in the Start menu

Six entries, and one of them is supposed to fail.

### hello
A window and a line of text, and that is all. It exists to be the smallest
possible zapp — about 3 KB of `.llext`, one source file, no library. Open it
twice: the two windows are two instances of the same loaded image.

### notes
Appends a line to `~/notes.txt` every time you click in it, and shows the file
back when you next launch it. Small, but it is the whole chain — a real
extension, found on a real filesystem, writing through the permission shim to
real storage, and still there after a reboot.

### notepad
A text editor. Typing, selection, cut/copy/paste, and File / Edit menus. Open a
file with `Ctrl+O`, save with `Ctrl+S`. Try closing it with unsaved changes: the
"save changes?" box is the close-decline path being exercised.

### files
A file browser, and a **singleton** — launch it twice and the running one comes
to the front rather than a second opening. Double-click a folder to enter it,
`..` to go up. It will not go above the directory it started in. You can make a
folder and delete things. Double-clicking a `.txt` opens it in Notepad, which is
one zapp launching another with an argument; the `.txt` → Notepad mapping is
hardcoded in the browser and says so in its own source, because there is no
association registry.

### mines
Minesweeper. Three difficulties in the Game menu, and the board fits itself to
your screen rather than assuming a size — on a small panel you get a smaller
board with the same mine density, not a scrolling one.

**Tap to uncover, and *hold* to flag.** There is one mouse button and no
keyboard modifier in this desktop, so a long press is the only place the second
verb could go. It is also completely invisible, which is why there is a
Help → How to Play. The first click of a game is never a mine — mines are laid
after you click.

### badabi
**This one is meant to fail.** It declares an ABI major version the desktop does
not support, and exists so that the version gate is exercised in the field
rather than only in a test nobody runs. Clicking it should log a refusal and
open nothing. If it ever opens a window, the gate is broken.

## Typing

On QEMU you have a real keyboard and it just works. The **abc** button raises an
on-screen keyboard for boards that have no keys — it feeds the same funnel a
real keyboard does, so nothing downstream can tell the two apart.

On a touch board the keyboard also raises itself when you tap into a text field
(`CONFIG_ZD_OSK_AUTO`, on by default wherever the touch slop says there is a
finger). It costs about 128 px of screen while up, which is why it is a toggle
and not a permanent strip.

## Things that will look like bugs and are not

- **The clock is made up.** On a board with no RTC, `clock_now()` reports
  uptime dressed as a time of day. There is no date anywhere in the ABI. This is
  why Notepad has no Insert Time/Date: a fake time in the corner of a taskbar is
  a nicety, and the same number written into a document you then save is a small
  lie with a long life.
- **The file browser does not notice files created behind its back.** Nothing in
  this desktop notifies anything of anything. Navigate away and back.
- **`badabi` failing to launch** is the ABI gate working. See above.
- **A zapp refusing to close** is a zapp doing what it is allowed to do.

## Driving it without a hand

Everything above can be scripted, which is how it gets tested:

```sh
python3 tools/qemu-drive.py -d build \
    'wait:3' 'click:20,258' 'expect:discovered 6 zapp(s)'
```

`click:`, `press:`/`release:` (for hold-to-flag), `drag:`, `type:`, `key:` and
`expect:` run in order; the exit status is non-zero if an `expect:` never
matched. `tools/qemu-drive.py --help` has the full table. When the question is
about pixels rather than log lines, `tools/shot.py` screenshots the framebuffer.
