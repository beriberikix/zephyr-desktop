/*
 * zephyr-desktop — the app ABI.
 *
 * This header is the contract between the desktop and every extension. It
 * deliberately includes no Zephyr and no LVGL headers, only <stdint.h> and
 * <stddef.h>. That is what keeps LVGL's ABI from silently becoming ours, and
 * what would let an app be linked statically behind the same contract if a
 * non-llext-capable target ever mattered again.
 *
 * Versioning rule: abi_major must match exactly; app.abi_minor <= host.abi_minor
 * is accepted. The host vtable only ever grows by appending, and struct_size
 * lets an older app bind safely against a newer host. Anything else is a major
 * bump.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZD_APP_ABI_H_
#define ZD_APP_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZD_ABI_MAJOR 0
#define ZD_ABI_MINOR 2

/** Longest absolute path the desktop will hand back or accept. */
#define ZD_PATH_MAX 96

/** Longest app display name. */
#define ZD_APP_NAME_MAX 24

/* Opaque handles. An app never sees an lv_obj_t. */
typedef struct zd_app_ctx *zd_app_ctx_t;
typedef struct zd_window *zd_window_t;
typedef struct zd_label *zd_label_t;

struct zd_rect {
	int16_t x;
	int16_t y;
	int16_t w;
	int16_t h;
};

/**
 * Well-known directories, resolved per session.
 *
 * Apps never build absolute paths themselves: the filesystem root differs
 * between targets (FATFS wants a "/RAM:" volume prefix under QEMU, an SD volume
 * on hardware) and the home directory belongs to the session, not the app.
 */
enum zd_dir {
	ZD_DIR_HOME,        /**< read-write */
	ZD_DIR_SYSTEM_ZAPPS, /**< read-only */
	ZD_DIR_USER_ZAPPS,   /**< read-write */
	ZD_DIR_TMP,         /**< read-write */
};

/* --- events --------------------------------------------------------------- */

enum zd_event_type {
	ZD_EV_WINDOW_SHOWN,
	/**
	 * Reserved. The MVP's window manager closes unconditionally and never
	 * sends this, so no app can yet refuse or defer a close. Defined now so
	 * the enum does not have to be renumbered when it starts being sent.
	 */
	ZD_EV_WINDOW_CLOSE_REQUEST,
	ZD_EV_WINDOW_FOCUS,
	ZD_EV_WINDOW_BLUR,
	ZD_EV_CLICK, /**< content-area click, window-relative */
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
 * The desktop's side of the contract, handed to an app at init.
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
	zd_window_t (*window_create)(zd_app_ctx_t ctx, const struct zd_window_desc *desc);
	void (*window_close)(zd_app_ctx_t ctx, zd_window_t win);
	int (*window_set_title)(zd_app_ctx_t ctx, zd_window_t win, const char *title);
	int (*window_set_geometry)(zd_app_ctx_t ctx, zd_window_t win,
				   const struct zd_rect *geom);
	int (*window_get_geometry)(zd_app_ctx_t ctx, zd_window_t win, struct zd_rect *out);

	/* content -- deliberately tiny, grown one widget at a time on demand */
	zd_label_t (*label_create)(zd_app_ctx_t ctx, zd_window_t win, const char *text,
				   int16_t x, int16_t y);
	int (*label_set_text)(zd_app_ctx_t ctx, zd_label_t label, const char *text);

	/* filesystem -- always ctx-scoped, never raw paths */
	int (*path_resolve)(zd_app_ctx_t ctx, enum zd_dir dir, char *out, uint32_t out_len);

	/* misc */
	void (*log)(zd_app_ctx_t ctx, int level, const char *msg);
	int64_t (*uptime_ms)(void);

	/**
	 * Raw lv_obj_t of the window's content area.
	 *
	 * NULL unless the app's manifest carries ZD_APP_FLAG_TRUSTED. The slot
	 * exists from the first version so the escape hatch is a documented,
	 * version-gated part of the contract rather than something bolted on
	 * under pressure later. Using it welds the app to LVGL's ABI.
	 */
	void *(*unsafe_lvgl_content)(zd_app_ctx_t ctx, zd_window_t win);

	/* --- ABI 0.2 ------------------------------------------------------ */

	/**
	 * Per-instance storage. Added in 0.2, and not a convenience.
	 *
	 * llext refcounts extensions by name: launching the same app twice loads
	 * the image ONCE and hands both instances the same code, the same .data
	 * and the same .bss. A file-scope variable in an app is therefore shared
	 * across every instance of that app, and the second instance will
	 * quietly stamp on the first one's state.
	 *
	 * Anything an app needs one copy of per instance goes here. Small values
	 * can be stuffed in the pointer itself; larger state needs an allocation
	 * the app owns and frees in fini().
	 */
	void (*set_user_data)(zd_app_ctx_t ctx, void *data);
	void *(*get_user_data)(zd_app_ctx_t ctx);
};

/* --- the app's side ------------------------------------------------------- */

#define ZD_APP_MAGIC 0x5A444150u /* 'ZDAP' */

#define ZD_APP_FLAG_TRUSTED      (1u << 0) /**< may use unsafe_lvgl_content */
#define ZD_APP_FLAG_SINGLETON    (1u << 1) /**< at most one instance */
#define ZD_APP_FLAG_WANTS_THREAD (1u << 2) /**< reserved; see below */

/**
 * What an app exports, via LL_EXTENSION_SYMBOL(zd_app_manifest).
 *
 * Ordering guarantee: no event is delivered to event() until init() has
 * returned. Creating a window focuses it, so without this an app would be told
 * it has focus before it had recorded the handle it was just handed, and could
 * not tell its own window from another instance's.
 *
 * ZD_APP_FLAG_WANTS_THREAD is defined but not honoured in the MVP. Apps are
 * callback-driven on the desktop thread, because hello world needs no thread
 * and thread-per-app is where the ABI gets genuinely hard: locking discipline,
 * priorities, and teardown of a thread that may be blocked. When it arrives the
 * desktop will use llext_bootstrap() with a per-instance stack, and this
 * structure will not change.
 */
struct zd_app_manifest {
	uint32_t magic;
	uint16_t abi_major;
	uint16_t abi_minor;
	uint32_t flags;
	const char *name;
	const char *icon; /**< NULL in the MVP */

	int (*init)(zd_app_ctx_t ctx, const struct zd_host_api *api);
	void (*event)(zd_app_ctx_t ctx, const struct zd_event *ev);
	void (*fini)(zd_app_ctx_t ctx);
};

/** Symbol name the loader looks up in the extension's export table. */
#define ZD_APP_MANIFEST_SYM "zd_app_manifest"

/** Symbol name the extension links against to reach the desktop. */
#define ZD_HOST_API_SYM "zd_get_host_api"

/** Implemented by the desktop, resolved by the loader. */
const struct zd_host_api *zd_get_host_api(void);

#ifdef __cplusplus
}
#endif

#endif /* ZD_APP_ABI_H_ */
