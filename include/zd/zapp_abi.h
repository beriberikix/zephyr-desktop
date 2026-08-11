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
#define ZD_ABI_MINOR 3

/** Longest absolute path the desktop will hand back or accept. */
#define ZD_PATH_MAX 96

/** Longest zapp display name. */
#define ZD_ZAPP_NAME_MAX 24

/**
 * Longest single filename in a directory listing.
 *
 * Must exceed what the underlying filesystem can produce, or a listing would
 * silently hand back a truncated name that cannot be opened. FATFS with long
 * filenames yields 64 on this tree; fs_api.c BUILD_ASSERTs the relationship, so
 * a target configured for longer names fails to build rather than to work.
 */
#define ZD_NAME_MAX 80

/* Opaque handles. A zapp never sees an lv_obj_t, and never sees a struct
 * fs_file_t either -- every one of these is an index plus a generation counter
 * into a desktop-owned table, checked for liveness, kind and ownership on use.
 */
typedef struct zd_zapp_ctx *zd_zapp_ctx_t;
typedef struct zd_window *zd_window_t;
typedef struct zd_label *zd_label_t;
/** An open file. */
typedef struct zd_file *zd_file_t;
/**
 * An open directory being walked. NOT `enum zd_dir` below, which names a
 * location rather than a handle -- and note the tag is zd_dir_handle, because C
 * keeps struct and enum tags in one namespace and `struct zd_dir` would collide.
 */
typedef struct zd_dir_handle *zd_dir_t;

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

/* --- events --------------------------------------------------------------- */

enum zd_event_type {
	ZD_EV_WINDOW_SHOWN,
	/**
	 * Reserved. The MVP's window manager closes unconditionally and never
	 * sends this, so no zapp can yet refuse or defer a close. Defined now so
	 * the enum does not have to be renumbered when it starts being sent.
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
	ZD_EV_KEY,   /**< reserved; not delivered in the MVP */
};

struct zd_event {
	enum zd_event_type type;
	zd_window_t win;
	union {
		struct {
			int16_t x;
			int16_t y;
		} click;
		struct {
			uint32_t code;
		} key;
	};
};

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
