/*
 * zephyr-desktop — the zapp ABI.
 *
 * This header is the contract between the desktop and every extension. It
 * deliberately includes no Zephyr and no LVGL headers, only <stdint.h> and
 * <stddef.h>. That is what keeps LVGL's ABI from silently becoming ours, and
 * what would let a zapp be linked statically behind the same contract if a
 * non-llext-capable target ever mattered again.
 *
 * Versioning rule: abi_major must match exactly; zapp.abi_minor <= host.abi_minor
 * is accepted. The host vtable only ever grows by appending, and struct_size
 * lets an older zapp bind safely against a newer host. Anything else is a major
 * bump.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_ZAPP_ABI_H_
#define ZD_ZAPP_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZD_ABI_MAJOR 0
#define ZD_ABI_MINOR 6

/**
 * Longest absolute path the desktop will hand back or accept.
 *
 * A TUNING KNOB, NOT AN ABI MEMBER, and the difference is worth stating because
 * this is the second time the number has moved. It appears in no struct in this
 * header -- only here -- and every host call that writes a path into a caller's
 * buffer is told that buffer's size (path_resolve, dialog_get_path,
 * get_launch_arg) and promises -ENOSPC rather than half a path. So raising it
 * moves no offset and breaks no layout: the worst that happens to a zapp built
 * against the older value is -ENOSPC for a path that could not have existed
 * before the change, which is the failure it already handles.
 *
 * Keep it that way. The moment something in this header writes a path into a
 * fixed array a zapp declares, the knob becomes a member and can never move
 * again -- which is exactly what happened to ZD_NAME_MAX below.
 *
 * Raised from 96 in 0.6, because a file browser is the first thing that walks
 * directories: "/RAM:/home/user" plus a 64-byte FATFS long name is already 80,
 * so 96 could not hold two levels.
 */
#define ZD_PATH_MAX 192

/** Longest zapp display name. */
#define ZD_ZAPP_NAME_MAX 24

/**
 * Longest window title, terminator included.
 *
 * Part of the ABI since 0.5, because a zapp building a title -- "*name -
 * Notepad" -- needs somewhere to build it, and the alternative was for it to
 * guess. Longer titles are truncated, not refused.
 */
#define ZD_TITLE_MAX 32

/**
 * Longest single filename in a directory listing.
 *
 * Must exceed what the underlying filesystem can produce, or a listing would
 * silently hand back a truncated name that cannot be opened. FATFS with long
 * filenames yields 64 on this tree; fs_api.c BUILD_ASSERTs the relationship, so
 * a target configured for longer names fails to build rather than to work.
 *
 * FROZEN, unlike ZD_PATH_MAX above, and this is the worked example of why that
 * one is careful. This value is the array bound inside struct zd_dirent, which
 * the host fills through a pointer the zapp supplied -- and fs_readdir has no
 * length parameter to check it against. A zapp built against 0.5 has an 88-byte
 * dirent on its stack; a host built with a larger value writes past the end of
 * it. That is a stack smash, it happens on a correctly-versioned zapp, and no
 * version gate can catch it, because the sizes never meet. Raising this is a
 * major bump. Do not.
 */
#define ZD_NAME_MAX 80

/* Opaque handles. A zapp never sees an lv_obj_t, and never sees a struct
 * fs_file_t either -- every one of these is an index plus a generation counter
 * into a desktop-owned table, checked for liveness, kind and ownership on use.
 */
typedef struct zd_zapp_ctx *zd_zapp_ctx_t;
typedef struct zd_window *zd_window_t;
typedef struct zd_label *zd_label_t;
/** An editable multi-line text field. Added in 0.5. */
typedef struct zd_text *zd_text_t;
/** A menu bar, or one drop-down within it. Added in 0.5. */
typedef struct zd_menu *zd_menu_t;
/** A scrolling column of selectable single-line rows. Added in 0.6. */
typedef struct zd_list *zd_list_t;
/** An open file. */
typedef struct zd_file *zd_file_t;
/**
 * An open directory being walked. NOT `enum zd_dir` below, which names a
 * location rather than a handle -- and note the tag is zd_dir_handle, because C
 * keeps struct and enum tags in one namespace and `struct zd_dir` would collide.
 */
typedef struct zd_dir_handle *zd_dir_t;

/** Wall-clock time as the desktop understands it. See clock_now(). */
struct zd_time {
	uint8_t hour;   /**< 0..23 */
	uint8_t minute;
	uint8_t second;
};

struct zd_rect {
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
};

/**
 * Well-known directories, resolved per session.
 *
 * Zapps never build absolute paths themselves: the filesystem root differs
 * between targets (FATFS wants a "/RAM:" volume prefix under QEMU, an SD volume
 * on hardware) and the home directory belongs to the session, not the zapp.
 *
 * This names a *location*. The handle to a directory you are walking is
 * `zd_dir_t`, declared above.
 */
enum zd_dir {
	ZD_DIR_HOME,        /**< read-write */
	ZD_DIR_SYSTEM_ZAPPS, /**< read-only */
	ZD_DIR_USER_ZAPPS,   /**< read-write */
	ZD_DIR_TMP,         /**< read-write */
};

/* --- storage --------------------------------------------------------------- */

/*
 * Open flags in the project's own namespace rather than Zephyr's FS_O_*. This
 * header may not include a Zephyr header -- that is what keeps a zapp buildable
 * out of tree against nothing but the ABI -- so the desktop maps these.
 */
#define ZD_O_READ   (1u << 0)
#define ZD_O_WRITE  (1u << 1)
#define ZD_O_CREATE (1u << 2) /**< create if absent; needs ZD_O_WRITE */
#define ZD_O_APPEND (1u << 3) /**< seek to end after opening */
#define ZD_O_TRUNC  (1u << 4) /**< discard existing contents */

enum zd_dirent_type {
	ZD_DIRENT_FILE,
	ZD_DIRENT_DIR,
};

struct zd_dirent {
	char name[ZD_NAME_MAX]; /**< the entry's name, not a path */
	uint32_t size;          /**< bytes; 0 for a directory */
	uint8_t type;           /**< enum zd_dirent_type */
};

/** Whence values for fs_seek, matching <stdio.h> so they read as expected. */
#define ZD_SEEK_SET 0
#define ZD_SEEK_CUR 1
#define ZD_SEEK_END 2

/* --- keys ------------------------------------------------------------------ */

/*
 * Key codes, in the desktop's own namespace.
 *
 * Not Linux input codes and not LVGL's LV_KEY_*, for the same reason ZD_O_READ
 * is not FS_O_READ: this header may include no Zephyr and no LVGL header, and a
 * zapp built out of tree against nothing but the ABI must still compile. The
 * desktop maps whatever its input sources produce onto these.
 *
 * APPEND ONLY, like the event enum -- a zapp compares against the numbers it
 * was compiled with. Spelled as #defines with explicit values rather than an
 * enum so that is impossible to miss.
 *
 * Anything that produces a character arrives as ZD_KEY_CHAR with the codepoint
 * in ev->key.unicode. Everything else is a named key with unicode == 0. Enter,
 * Tab and Backspace are named rather than characters because an editor treats
 * them as commands far more often than as text.
 */
#define ZD_KEY_CHAR      1  /**< see ev->key.unicode */
#define ZD_KEY_BACKSPACE 2
#define ZD_KEY_TAB       3
#define ZD_KEY_ENTER     4
#define ZD_KEY_ESCAPE    5
#define ZD_KEY_DELETE    6
#define ZD_KEY_LEFT      7
#define ZD_KEY_RIGHT     8
#define ZD_KEY_UP        9
#define ZD_KEY_DOWN      10
#define ZD_KEY_HOME      11
#define ZD_KEY_END       12
#define ZD_KEY_PAGE_UP   13
#define ZD_KEY_PAGE_DOWN 14
#define ZD_KEY_INSERT    15
#define ZD_KEY_F1        16 /**< F1..F12 are consecutive; see ZD_KEY_F() */
#define ZD_KEY_F12       27

/** F1 is ZD_KEY_F(1). Out-of-range @p n is the caller's problem. */
#define ZD_KEY_F(n) (ZD_KEY_F1 + (n) - 1)

#define ZD_MOD_SHIFT (1u << 0)
#define ZD_MOD_CTRL  (1u << 1)
#define ZD_MOD_ALT   (1u << 2)

/* --- text widgets ----------------------------------------------------------- */

/** Not editable and never takes the caret. */
#define ZD_TEXT_READONLY (1u << 0)
/** One line: Enter is not inserted, and the field never wraps. */
#define ZD_TEXT_ONE_LINE (1u << 1)

/* --- dialogs ---------------------------------------------------------------- */

/** Which buttons a confirm dialog shows. */
#define ZD_DLG_OK_CANCEL     0u
#define ZD_DLG_YES_NO_CANCEL 1u

/** What a file dialog is for. */
#define ZD_DLG_OPEN 0u
#define ZD_DLG_SAVE 1u

/*
 * What came back, in ev->dialog.result.
 *
 * OK and YES are the same value on purpose: they are the same answer -- the
 * affirmative button, whatever it was labelled -- and a zapp that opened an
 * OK/Cancel dialog should not have to remember which name to compare against.
 */
#define ZD_DLG_CANCEL 0
#define ZD_DLG_OK     1
#define ZD_DLG_YES    1
#define ZD_DLG_NO     2

/* --- events --------------------------------------------------------------- */

/*
 * These values are part of the binary contract: a zapp built against an older
 * minor version compares against the numbers it was compiled with. New members
 * are therefore APPENDED AT THE END, never inserted next to the events they are
 * conceptually related to. Grouping ZD_EV_RESIZED with the other window events
 * would have renumbered ZD_EV_CLICK and silently broken every 0.3 zapp.
 */
enum zd_event_type {
	ZD_EV_WINDOW_SHOWN,
	/**
	 * The user asked for this window to close. Delivered since 0.4.
	 *
	 * Close what you must, flush what you must, then call window_close().
	 * This is an ask, not a veto: if you have not closed the window within
	 * CONFIG_ZD_CLOSE_GRACE_MS, or the user hits the close box a second
	 * time, the desktop closes it for you. Do not treat the grace period as
	 * a place to wait for anything slower than a file sync.
	 */
	ZD_EV_WINDOW_CLOSE_REQUEST,
	ZD_EV_WINDOW_FOCUS,
	ZD_EV_WINDOW_BLUR,
	/**
	 * A click in the window's content area, with coordinates relative to
	 * that area's top-left -- the same origin label_create() uses, so a zapp
	 * never has to know the chrome's dimensions. Clicks on the titlebar or
	 * the close box are the desktop's business and are not delivered.
	 */
	ZD_EV_CLICK,
	/**
	 * A key was pressed in this window. Delivered since 0.5; reserved and
	 * silent before that.
	 *
	 * Presses only -- releases are not delivered, because nothing an
	 * application does needs them and they double the traffic through a
	 * dispatch path that runs on the desktop thread. Auto-repeat, when it
	 * arrives, will look like more presses.
	 *
	 * Which keys reach you depends on what has the caret. If the window has
	 * a focused text widget, ordinary typing goes into it and never appears
	 * here; a key held with CTRL always comes here instead, so accelerators
	 * work while the user is typing. A window with no text widget gets
	 * everything.
	 */
	ZD_EV_KEY,

	/* --- ABI 0.4 ------------------------------------------------------ */

	/**
	 * The content area has a new size, carried in ev->resize.
	 *
	 * Sent when the user lets go of the resize grip, not while they drag it,
	 * and once for any window_set_geometry() that changed the size. The
	 * cadence is deliberate: a zapp callback may do filesystem I/O, and on a
	 * board where storage borrows the display's pin one of those per pointer
	 * sample would be indefensible. The content area itself tracks the
	 * pointer live; only your own widgets lag until the button comes up.
	 */
	ZD_EV_RESIZED,
	/**
	 * The window was minimised or restored. The pair to FOCUS/BLUR, and the
	 * cue to stop doing work whose result nobody can see. A minimised window
	 * is still yours: its handle stays valid and it keeps its place in the
	 * stacking order.
	 */
	ZD_EV_MINIMIZED,
	ZD_EV_RESTORED,

	/* --- ABI 0.5 ------------------------------------------------------ */

	/**
	 * The USER changed a text widget's contents, named in ev->text.text.
	 *
	 * Not sent for edits the zapp itself made through text_set_text(),
	 * text_insert(), text_paste() and the rest -- you already know about
	 * those, and being re-entered from inside your own host call is a trap
	 * rather than a service. This event means exactly what a dirty flag
	 * wants it to mean.
	 */
	ZD_EV_TEXT_CHANGED,
	/**
	 * A menu item was chosen; ev->menu.id is the value you gave it.
	 *
	 * Delivered after the drop-down has been dismissed, so closing your own
	 * window from here is safe -- and is exactly what File -> Exit should
	 * do.
	 */
	ZD_EV_MENU,
	/**
	 * A dialog you asked for has been answered: ev->dialog.id is the value
	 * you passed, ev->dialog.result is one of ZD_DLG_OK / _YES / _NO /
	 * _CANCEL.
	 *
	 * For a file dialog, call dialog_get_path() to find out which file --
	 * a path does not fit in an event and does not belong in one.
	 *
	 * Delivered after the dialog has come down, so closing your own window
	 * from here is safe.
	 */
	ZD_EV_DIALOG,

	/* --- ABI 0.6 ------------------------------------------------------ */

	/**
	 * A list's selection moved -- by click, or by an arrow key while that
	 * list had the keyboard within its window. ev->list names the list, the
	 * row and the id you gave that row; index is -1 when the selection was
	 * cleared.
	 *
	 * CHEAP BY CONTRACT. This fires on every arrow key, so do not do
	 * filesystem I/O in it. Whatever you want to show about the selected
	 * thing, you had while you were filling the list -- fs_readdir handed
	 * you the size and the type -- so cache it then. On a board where
	 * storage borrows the display's pin, a stat per keystroke would stop
	 * the screen drawing while the user held Down. Same reasoning as
	 * ZD_EV_RESIZED arriving on release rather than per pointer sample.
	 */
	ZD_EV_LIST_SELECT,
	/**
	 * A row was double-clicked, or Enter was pressed on it.
	 *
	 * Delivered after the click has been fully dispatched, so rebuilding
	 * the list from in here -- which is exactly what entering a directory
	 * means -- is safe. list_clear() and list_add_item() write a model that
	 * the desktop turns back into rows one loop iteration later.
	 */
	ZD_EV_LIST_ACTIVATE,
	/**
	 * Somebody launched you again while you were already running, and you
	 * set ZD_ZAPP_FLAG_SINGLETON so no second instance was made.
	 *
	 * Call get_launch_arg() for what they wanted; ev->win is one of your
	 * windows, already raised and focused. A path does not fit in an event,
	 * for the same reason dialog_get_path() exists.
	 */
	ZD_EV_LAUNCH_ARG,
};

struct zd_event {
	enum zd_event_type type;
	zd_window_t win;
	union {
		struct {
			int16_t x;
			int16_t y;
		} click;
		/**
		 * ZD_EV_KEY. @a code is a ZD_KEY_* value; @a unicode is the
		 * codepoint when @a code is ZD_KEY_CHAR and 0 otherwise; @a
		 * mods is a mask of ZD_MOD_*.
		 *
		 * Widened in 0.5 from a lone `code`, which is why the two new
		 * fields are after it rather than in a tidier order.
		 */
		struct {
			uint32_t code;
			uint32_t unicode;
			uint16_t mods;
		} key;
		/** New content-area size for ZD_EV_RESIZED. Added in 0.4. */
		struct {
			int16_t w;
			int16_t h;
		} resize;
		/** ZD_EV_TEXT_CHANGED. Added in 0.5. */
		struct {
			zd_text_t text;
		} text;
		/** ZD_EV_MENU. Added in 0.5. */
		struct {
			uint16_t id;
		} menu;
		/** ZD_EV_DIALOG. Added in 0.5. */
		struct {
			uint16_t id;
			int16_t result;
		} dialog;
		/** ZD_EV_LIST_SELECT and ZD_EV_LIST_ACTIVATE. Added in 0.6. */
		struct {
			zd_list_t list;
			int16_t index; /**< row, or -1 for "nothing selected" */
			uint16_t id;   /**< the id you gave that row */
		} list;
	};
};

/*
 * What actually has to hold for an older zapp to keep working.
 *
 * The union may grow -- a wider member makes struct zd_event bigger, and that
 * is harmless, because a zapp only ever reads the event through a pointer the
 * desktop handed it and only ever reads members it was compiled to know about.
 * What may NOT change is where anything already there lives. These assertions
 * are the rule; sizeof deliberately is not asserted, because pinning it would
 * forbid exactly the growth that is safe.
 */
_Static_assert(offsetof(struct zd_event, type) == 0, "zd_event.type moved");
_Static_assert(offsetof(struct zd_event, win) == sizeof(void *),
	       "zd_event.win moved");
_Static_assert(offsetof(struct zd_event, click) == 2 * sizeof(void *),
	       "the zd_event union moved");
_Static_assert(offsetof(struct zd_event, click) == offsetof(struct zd_event, key) &&
		       offsetof(struct zd_event, click) ==
			       offsetof(struct zd_event, resize),
	       "a zd_event union member is not at the union's offset");
/* The 0.6 member holds a pointer, so it is the first one whose alignment could
 * have moved the union. It does not: `win` is already a pointer, so the union
 * was pointer-aligned before this arrived. Asserted rather than reasoned about.
 */
_Static_assert(offsetof(struct zd_event, click) == offsetof(struct zd_event, list),
	       "the zd_event union moved when the list member was added");

struct zd_window_desc {
	const char *title;
	struct zd_rect geom; /**< w or h of 0 means "desktop picks and cascades" */
	uint32_t flags;
};

/* --- the host API --------------------------------------------------------- */

/**
 * The desktop's side of the contract, handed to a zapp at init.
 *
 * A vtable rather than a pile of exported functions: it keeps the export
 * surface at one symbol, gives the ABI a version gate, and gives every call a
 * natural place to hang the per-instance permission check.
 *
 * Grows by appending only. Check struct_size before using a field added after
 * the minor version you were built against.
 */
struct zd_host_api {
	uint16_t abi_major;
	uint16_t abi_minor;
	uint32_t struct_size;

	/* windows */
	zd_window_t (*window_create)(zd_zapp_ctx_t ctx, const struct zd_window_desc *desc);
	void (*window_close)(zd_zapp_ctx_t ctx, zd_window_t win);
	int (*window_set_title)(zd_zapp_ctx_t ctx, zd_window_t win, const char *title);
	/**
	 * Move and resize. A w or h of 0 leaves that dimension alone; anything
	 * below the desktop's minimum window size is raised to it, so check the
	 * result with window_get_geometry() rather than assuming you got what
	 * you asked for. A size change delivers ZD_EV_RESIZED before returning.
	 */
	int (*window_set_geometry)(zd_zapp_ctx_t ctx, zd_window_t win,
				   const struct zd_rect *geom);
	int (*window_get_geometry)(zd_zapp_ctx_t ctx, zd_window_t win, struct zd_rect *out);

	/* content -- deliberately tiny, grown one widget at a time on demand */
	zd_label_t (*label_create)(zd_zapp_ctx_t ctx, zd_window_t win, const char *text,
				   int16_t x, int16_t y);
	int (*label_set_text)(zd_zapp_ctx_t ctx, zd_label_t label, const char *text);

	/* filesystem -- always ctx-scoped, never raw paths */
	int (*path_resolve)(zd_zapp_ctx_t ctx, enum zd_dir dir, char *out, uint32_t out_len);

	/* misc */
	void (*log)(zd_zapp_ctx_t ctx, int level, const char *msg);
	int64_t (*uptime_ms)(void);

	/**
	 * Raw lv_obj_t of the window's content area.
	 *
	 * NULL unless the zapp's manifest carries ZD_ZAPP_FLAG_TRUSTED. The slot
	 * exists from the first version so the escape hatch is a documented,
	 * version-gated part of the contract rather than something bolted on
	 * under pressure later. Using it welds the zapp to LVGL's ABI.
	 */
	void *(*unsafe_lvgl_content)(zd_zapp_ctx_t ctx, zd_window_t win);

	/* --- ABI 0.2 ------------------------------------------------------ */

	/**
	 * Per-instance storage. Added in 0.2, and not a convenience.
	 *
	 * llext refcounts extensions by name: launching the same zapp twice loads
	 * the image ONCE and hands both instances the same code, the same .data
	 * and the same .bss. A file-scope variable in a zapp is therefore shared
	 * across every instance of that zapp, and the second instance will
	 * quietly stamp on the first one's state.
	 *
	 * Anything a zapp needs one copy of per instance goes here. Small values
	 * can be stuffed in the pointer itself; larger state needs an allocation
	 * the zapp owns and frees in fini().
	 */
	void (*set_user_data)(zd_zapp_ctx_t ctx, void *data);
	void *(*get_user_data)(zd_zapp_ctx_t ctx);

	/* --- ABI 0.3: storage ---------------------------------------------- */

	/*
	 * Paths are absolute and are checked on every call against the session's
	 * permitted roots: "..", relative paths, anything outside a root, and any
	 * write to a read-only root are refused. Build them from path_resolve(),
	 * never by hand.
	 *
	 * Be clear about what that is worth. Without an MMU a loaded extension is
	 * trusted code in the kernel address space, so this is a contract, not a
	 * security boundary. What it buys is that it is the only *linkable* route.
	 *
	 * READS AND WRITES MAY BE SHORT. A single call moves at most
	 * CONFIG_ZD_FS_IO_CHUNK bytes and may move fewer; loop until you have what
	 * you asked for or the call returns 0. This is deliberate. Zapps run as
	 * callbacks on the desktop thread, so the whole UI is stopped for the
	 * duration of a transfer -- and on a board where storage and the display
	 * share a pin, the screen physically cannot be drawn while one is in
	 * flight. Bounding a call bounds the stall.
	 *
	 * Every open handle is closed for you when your instance is unloaded, but
	 * quotas are small: close what you finish with.
	 */
	int (*fs_open)(zd_zapp_ctx_t ctx, const char *path, uint32_t flags, zd_file_t *out);
	/** @return bytes read, 0 at end of file, or a negative errno. */
	int (*fs_read)(zd_zapp_ctx_t ctx, zd_file_t file, void *buf, uint32_t len);
	/** @return bytes written, or a negative errno. */
	int (*fs_write)(zd_zapp_ctx_t ctx, zd_file_t file, const void *buf, uint32_t len);
	int (*fs_seek)(zd_zapp_ctx_t ctx, zd_file_t file, int32_t offset, int whence);
	/** @return the current offset, or a negative errno. */
	int (*fs_tell)(zd_zapp_ctx_t ctx, zd_file_t file);
	/** Flush to the medium. Not implied by anything else. */
	int (*fs_sync)(zd_zapp_ctx_t ctx, zd_file_t file);
	void (*fs_close)(zd_zapp_ctx_t ctx, zd_file_t file);

	int (*fs_opendir)(zd_zapp_ctx_t ctx, const char *path, zd_dir_t *out);
	/** @return 0 on an entry, -ENOENT at the end of the directory. */
	int (*fs_readdir)(zd_zapp_ctx_t ctx, zd_dir_t dir, struct zd_dirent *out);
	void (*fs_closedir)(zd_zapp_ctx_t ctx, zd_dir_t dir);

	/** Fills name/size/type for one path. @return 0 or a negative errno. */
	int (*fs_stat)(zd_zapp_ctx_t ctx, const char *path, struct zd_dirent *out);
	int (*fs_mkdir)(zd_zapp_ctx_t ctx, const char *path);
	int (*fs_unlink)(zd_zapp_ctx_t ctx, const char *path);
	/** Both paths are scoped and both must be writable. */
	int (*fs_rename)(zd_zapp_ctx_t ctx, const char *from, const char *to);

	/* --- ABI 0.4: window state ----------------------------------------- */

	/*
	 * Minimising is not closing. The window keeps its handle, its contents
	 * and its place in the stacking order; it is simply unmapped, so it
	 * draws nowhere and cannot be clicked. Restoring puts it back where it
	 * was in the stack and raises it to the front.
	 *
	 * The user can do both from the chrome and from the taskbar without
	 * asking the zapp. These exist so a zapp can do it to itself, and so the
	 * state it is told about in ZD_EV_MINIMIZED is one it can also set.
	 */
	int (*window_minimize)(zd_zapp_ctx_t ctx, zd_window_t win);
	int (*window_restore)(zd_zapp_ctx_t ctx, zd_window_t win);

	/* --- ABI 0.5: editable text ---------------------------------------- */

	/*
	 * A real text field: caret, word wrap, scrolling, click to position,
	 * drag to select. label_create() is for showing a string; this is for
	 * editing one, and it is what makes a text editor possible without the
	 * zapp reimplementing an editor.
	 *
	 * POSITIONS ARE BYTE OFFSETS, everywhere -- cursor, selection, the
	 * offset into text_get_text(). Not character indices. Bytes are what a
	 * zapp has when it reads a file, and converting is the desktop's job.
	 *
	 * A widget belongs to the window it was created in and dies with it.
	 * Destroying the window destroys the widget; the handle then stops
	 * resolving, as every stale handle does.
	 */
	zd_text_t (*text_create)(zd_zapp_ctx_t ctx, zd_window_t win,
				 const struct zd_rect *geom, uint32_t flags);
	void (*text_destroy)(zd_zapp_ctx_t ctx, zd_text_t text);

	int (*text_set_text)(zd_zapp_ctx_t ctx, zd_text_t text, const char *s);
	/**
	 * Copy out from byte offset @p from.
	 *
	 * SHORT BY CONTRACT, exactly like fs_read and for the same reason: a
	 * document outgrows any single call worth making on the desktop thread.
	 * Loop until it returns 0.
	 *
	 * @return bytes written to @p buf excluding the terminator, 0 at the
	 *         end of the text, or a negative errno.
	 */
	int (*text_get_text)(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t from, char *buf,
			     uint32_t len);
	/** @return length in bytes, excluding the terminator. */
	int (*text_get_length)(zd_zapp_ctx_t ctx, zd_text_t text);
	/**
	 * @return the most bytes this widget will hold.
	 *
	 * Ask, do not assume: the bound is the desktop's, it differs between
	 * boards, and a zapp that guessed would either refuse files it could
	 * have opened or accept ones it will silently truncate. "This file is
	 * too large for Notepad" is only honest if it is checked.
	 */
	int (*text_get_capacity)(zd_zapp_ctx_t ctx, zd_text_t text);
	/** Insert at the caret, replacing the selection if there is one. */
	int (*text_insert)(zd_zapp_ctx_t ctx, zd_text_t text, const char *s);
	int (*text_set_geometry)(zd_zapp_ctx_t ctx, zd_text_t text,
				 const struct zd_rect *geom);

	int (*text_set_cursor)(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t pos);
	/** @return the caret's byte offset, or a negative errno. */
	int (*text_get_cursor)(zd_zapp_ctx_t ctx, zd_text_t text);
	/** @return 1 with @p from and @p to filled, 0 if nothing is selected. */
	int (*text_get_selection)(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t *from,
				  uint32_t *to);
	/** Select a byte range. from >= to clears the selection. */
	int (*text_select)(zd_zapp_ctx_t ctx, zd_text_t text, uint32_t from, uint32_t to);
	/** @return 1 if something was deleted, 0 if nothing was selected. */
	int (*text_delete_selection)(zd_zapp_ctx_t ctx, zd_text_t text);

	/* --- ABI 0.5: the clipboard ---------------------------------------- */

	/*
	 * One desktop-wide buffer of bytes, outliving the zapp that filled it.
	 * This is the first thing in the ABI that is a property of the desktop
	 * rather than of your instance, which is exactly what makes pasting
	 * into another application work.
	 *
	 * No ownership negotiation and no formats. X11's selection protocol is
	 * what taking those seriously looks like, and none of it earns its keep
	 * here.
	 */
	/** Truncates at the desktop's limit. @return bytes stored. */
	int (*clipboard_set)(zd_zapp_ctx_t ctx, const char *text, uint32_t len);
	/** Short by contract, from byte offset @p from. @return bytes copied. */
	int (*clipboard_get)(zd_zapp_ctx_t ctx, uint32_t from, char *buf, uint32_t len);
	int (*clipboard_length)(zd_zapp_ctx_t ctx);

	/**
	 * The three verbs against a text widget.
	 *
	 * Provided rather than left to each zapp because the empty case of each
	 * is where a reimplementation goes wrong, and because two applications
	 * ought to agree about what Ctrl+V does.
	 *
	 * @return bytes moved, 0 for a no-op -- nothing selected, or an empty
	 *         clipboard -- or a negative errno. A cut copies first and only
	 *         deletes if the copy succeeded.
	 */
	int (*text_cut)(zd_zapp_ctx_t ctx, zd_text_t text);
	int (*text_copy)(zd_zapp_ctx_t ctx, zd_text_t text);
	int (*text_paste)(zd_zapp_ctx_t ctx, zd_text_t text);

	/* --- ABI 0.5: menus ------------------------------------------------ */

	/*
	 * The desktop draws the menu; you say what is in it and are told which
	 * item was chosen. A menu is chrome -- it has to match the palette, it
	 * has to behave the same in every application, and on a touch panel it
	 * has to be sized by rules a zapp has no way to know.
	 *
	 * The bar lives inside your window, above your content area, and the
	 * content area shrinks to make room. Your coordinates do not move: they
	 * were always relative to the content area, which is now shorter. Adding
	 * a bar therefore delivers ZD_EV_RESIZED, so widgets laid out before it
	 * can be fixed up.
	 *
	 * Command ids are yours; the desktop only hands them back.
	 */
	zd_menu_t (*menubar_create)(zd_zapp_ctx_t ctx, zd_window_t win);
	zd_menu_t (*menu_add_submenu)(zd_zapp_ctx_t ctx, zd_menu_t bar, const char *label);
	int (*menu_add_item)(zd_zapp_ctx_t ctx, zd_menu_t menu, const char *label,
			     uint16_t id);
	int (*menu_add_separator)(zd_zapp_ctx_t ctx, zd_menu_t menu);
	/** A disabled item is drawn greyed and cannot be chosen. */
	int (*menu_set_item_enabled)(zd_zapp_ctx_t ctx, zd_menu_t menu, uint16_t id,
				     bool enabled);

	/* --- ABI 0.5: window content size ---------------------------------- */

	/**
	 * The usable area inside a window, in the coordinates your widgets use.
	 *
	 * window_get_geometry() reports the OUTER rectangle, which includes the
	 * chrome -- and a zapp is deliberately not told how thick the chrome is,
	 * because that is the desktop's business and changes with the target's
	 * touch slop. Before 0.5 the only way to learn the content size was to
	 * wait for the first ZD_EV_RESIZED and guess until then; notes.c still
	 * carries the comment. This is the answer.
	 */
	int (*window_get_content_size)(zd_zapp_ctx_t ctx, zd_window_t win, int16_t *w,
				       int16_t *h);

	/* --- ABI 0.5: dialogs ---------------------------------------------- */

	/*
	 * "The text in the Untitled file has changed" and "open which file?"
	 * are not application problems. Every zapp that edits anything needs
	 * both, they should look the same everywhere, and the file picker has
	 * to walk the filesystem through the session's permissions -- which is
	 * the desktop's business and not yours.
	 *
	 * BOTH ARE ASYNCHRONOUS. They return as soon as the dialog is on
	 * screen, and the answer arrives later as ZD_EV_DIALOG carrying the id
	 * you passed. There is no modal loop: the desktop thread runs LVGL, so
	 * blocking it to wait for a click would stop the thing being clicked
	 * from drawing.
	 *
	 * Only one dialog exists at a time, desktop-wide -- asking while one is
	 * up returns -EBUSY. It is system modal rather than application modal,
	 * which is a simplification and the opposite of what Win95 did.
	 *
	 * @return 0 once it is up, or a negative errno.
	 */
	int (*dialog_confirm)(zd_zapp_ctx_t ctx, const char *title, const char *msg,
			      uint32_t buttons, uint16_t id);
	/** @param dir which well-known directory to list; there is no navigation. */
	int (*dialog_file)(zd_zapp_ctx_t ctx, const char *title, enum zd_dir dir,
			   uint32_t mode, uint16_t id);
	/**
	 * The path a file dialog produced, valid from ZD_EV_DIALOG until the
	 * next dialog. Empty if the answer was ZD_DLG_CANCEL.
	 *
	 * @return its length, or -ENOSPC if @p len is too small -- never a
	 *         truncated path, because half a path still opens something.
	 */
	int (*dialog_get_path)(zd_zapp_ctx_t ctx, char *buf, uint32_t len);

	/* --- ABI 0.5: the desktop clock ------------------------------------ */

	/**
	 * @brief What time the desktop thinks it is.
	 *
	 * The same answer the taskbar's clock shows, deliberately: two
	 * independent guesses disagreeing on one screen is worse than one
	 * guess. And on the targets so far it IS a guess -- there is no RTC on
	 * qemu_cortex_a53, so this counts up from a fixed start rather than
	 * reporting 00:00 since boot. A board with an RTC makes every caller
	 * correct at once without any of them changing.
	 *
	 * uptime_ms() remains the right call for measuring an interval. This
	 * one is for showing a person a time.
	 */
	int (*clock_now)(zd_zapp_ctx_t ctx, struct zd_time *out);

	/* --- ABI 0.5: declining a close ------------------------------------ */

	/**
	 * @brief Answer ZD_EV_WINDOW_CLOSE_REQUEST with "no".
	 *
	 * The window stays and the outstanding request is dropped, so the
	 * user's next click on the close box starts the conversation over --
	 * you will be asked again, and can ask them again.
	 *
	 * This is what makes the Cancel button on a "save changes?" box mean
	 * anything. Without it, declining and saying nothing are the same
	 * thing: the grace period expires and the desktop takes the window.
	 *
	 * The rule 0.4 stated has therefore been sharpened rather than
	 * abandoned. A zapp cannot keep a window by IGNORING the request --
	 * silence still loses it after CONFIG_ZD_CLOSE_GRACE_MS. It can keep it
	 * by answering, which is the bargain every desktop makes. Do not use
	 * this to be unclosable; there is no end-task here yet to save the user
	 * from you.
	 */
	void (*window_close_cancel)(zd_zapp_ctx_t ctx, zd_window_t win);

	/* --- ABI 0.6: list widgets ------------------------------------------ */

	/*
	 * A scrolling column of single-line rows, one of which may be selected.
	 *
	 * label_create() shows a string and text_create() edits one; this is
	 * for choosing between many, and it is what makes a file browser
	 * possible without the zapp drawing rows it cannot draw -- a zapp never
	 * sees an lv_obj_t, so before this there was no way to put a directory
	 * on screen at all.
	 *
	 * THE LIST IS A MODEL, NOT A PICTURE, and this is the one thing to
	 * understand before using it. list_clear() and list_add_item() change
	 * what the list *is*, immediately: list_get_count() and
	 * list_get_selected() answer from the model the instant you call them,
	 * and list_get_item_text() reads back what you put in. Only the rows on
	 * screen lag, by at most one turn of the desktop loop.
	 *
	 * That is what makes it safe to empty and refill a list from inside
	 * ZD_EV_LIST_ACTIVATE -- which is precisely what "the user opened this
	 * directory" means. It is also the rule the window manager already
	 * applies to stacking order: the model is the truth and what LVGL holds
	 * is a projection of it, re-applied from the loop.
	 *
	 * Row ids are yours; the desktop only hands them back. A list belongs
	 * to the window it was created in and dies with it.
	 */
	zd_list_t (*list_create)(zd_zapp_ctx_t ctx, zd_window_t win,
				 const struct zd_rect *geom, uint32_t flags);
	void (*list_destroy)(zd_zapp_ctx_t ctx, zd_list_t list);
	int (*list_set_geometry)(zd_zapp_ctx_t ctx, zd_list_t list,
				 const struct zd_rect *geom);

	/**
	 * Empty it.
	 *
	 * The selection is cleared, not preserved. Row 3 of the new contents is
	 * not the thing row 3 used to be, and carrying the index across is how
	 * you delete the wrong file.
	 */
	int (*list_clear)(zd_zapp_ctx_t ctx, zd_list_t list);
	/** @return the new row's index, or -ENOSPC if the list is full. */
	int (*list_add_item)(zd_zapp_ctx_t ctx, zd_list_t list, const char *text,
			     uint16_t id);
	int (*list_get_count)(zd_zapp_ctx_t ctx, zd_list_t list);
	/**
	 * @return the most rows this list will hold.
	 *
	 * Ask, do not assume -- the bound is the desktop's and differs between
	 * boards, exactly as it does for text_get_capacity(). A zapp that
	 * guessed would either refuse to show a directory it could have shown
	 * or quietly stop listing partway down it.
	 */
	int (*list_get_capacity)(zd_zapp_ctx_t ctx, zd_list_t list);
	/** @return the selected row, or -1 if nothing is selected. */
	int (*list_get_selected)(zd_zapp_ctx_t ctx, zd_list_t list);
	/** Select a row and scroll it into view. -1 clears the selection. */
	int (*list_set_selected)(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index);
	/** @return the id you gave row @p index, or a negative errno. */
	int (*list_get_item_id)(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index);
	/**
	 * Copy a row's text back out.
	 *
	 * NOT short by contract, unlike text_get_text() and fs_read(): a row is
	 * one line, and half a filename still names a file, just the wrong one.
	 * Returns -ENOSPC rather than truncating, like dialog_get_path().
	 *
	 * Ask rather than keeping your own copy of every row. The desktop is
	 * already holding the string; a second listing alongside it is memory
	 * spent to avoid a memcpy.
	 *
	 * @return the length written excluding the terminator, or -errno.
	 */
	int (*list_get_item_text)(zd_zapp_ctx_t ctx, zd_list_t list, int32_t index,
				  char *buf, uint32_t len);

	/* --- ABI 0.6: labels can be moved and destroyed --------------------- */

	/*
	 * 0.5 could create a label and then neither move it nor get rid of it,
	 * and every one it created held a handle for the life of the window.
	 * That survived only because nothing had a label whose position
	 * depended on anything -- a status line along the bottom of a resizable
	 * window is the first, and it has to follow ZD_EV_RESIZED.
	 */
	int (*label_set_pos)(zd_zapp_ctx_t ctx, zd_label_t label, int16_t x, int16_t y);
	void (*label_destroy)(zd_zapp_ctx_t ctx, zd_label_t label);
};

/* --- the zapp's side ------------------------------------------------------- */

#define ZD_ZAPP_MAGIC 0x5A445A50u /* 'ZDZP' */

#define ZD_ZAPP_FLAG_TRUSTED      (1u << 0) /**< may use unsafe_lvgl_content */
#define ZD_ZAPP_FLAG_SINGLETON    (1u << 1) /**< at most one instance */
#define ZD_ZAPP_FLAG_WANTS_THREAD (1u << 2) /**< reserved; see below */

/**
 * What a zapp exports, via LL_EXTENSION_SYMBOL(zd_zapp_manifest).
 *
 * Ordering guarantee: no event is delivered to event() until init() has
 * returned. Creating a window focuses it, so without this a zapp would be told
 * it has focus before it had recorded the handle it was just handed, and could
 * not tell its own window from another instance's.
 *
 * ZD_ZAPP_FLAG_WANTS_THREAD is defined but not honoured in the MVP. Zapps are
 * callback-driven on the desktop thread, because hello world needs no thread
 * and thread-per-zapp is where the ABI gets genuinely hard: locking discipline,
 * priorities, and teardown of a thread that may be blocked. When it arrives the
 * desktop will use llext_bootstrap() with a per-instance stack, and this
 * structure will not change.
 */
struct zd_zapp_manifest {
	uint32_t magic;
	uint16_t abi_major;
	uint16_t abi_minor;
	uint32_t flags;
	const char *name;
	const char *icon; /**< NULL in the MVP */

	int (*init)(zd_zapp_ctx_t ctx, const struct zd_host_api *api);
	void (*event)(zd_zapp_ctx_t ctx, const struct zd_event *ev);
	void (*fini)(zd_zapp_ctx_t ctx);
};

/** Symbol name the loader looks up in the extension's export table. */
#define ZD_ZAPP_MANIFEST_SYM "zd_zapp_manifest"

/** Symbol name the extension links against to reach the desktop. */
#define ZD_HOST_API_SYM "zd_get_host_api"

/** Implemented by the desktop, resolved by the loader. */
const struct zd_host_api *zd_get_host_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ZD_ZAPP_ABI_H_ */
