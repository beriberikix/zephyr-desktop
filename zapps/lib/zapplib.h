/*
 * zapplib — the libc a zapp does not get.
 *
 * CONFIG_LLEXT_EXPORT_DEFAULT_GROUPS=n leaves the desktop exporting exactly one
 * symbol, so an extension has no strlen, no snprintf and no memcpy. Every zapp
 * has therefore carried its own copy of the same four helpers, with the same
 * comment explaining the same GCC trap. This is that copy, made once.
 *
 * It is deliberately not a Zephyr library and links no Zephyr code: it is
 * compiled into each extension alongside the zapp's own sources, which is only
 * possible at all since the ARM targets moved to LLEXT_TYPE_ELF_RELOCATABLE.
 * A zapp that wants to stay a single translation unit -- hello does -- simply
 * does not list it.
 *
 * The rule every function here exists to obey: nothing may emit a call to a
 * libc symbol. That is easy to break by accident. A plain `for` loop writing
 * zeroes becomes a memset call at -O2, and a struct assignment becomes one
 * without any loop being written at all. Both build cleanly and fail at load
 * with an undefined-symbol error a long way from the cause, so after touching
 * a zapp run `nm -u build/<name>.llext` and expect at most zd_get_host_api.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZAPPS_LIB_ZAPPLIB_H_
#define ZAPPS_LIB_ZAPPLIB_H_

#include <stdbool.h> /* compiler-provided; no libc is linked in */
#include <stdint.h>

/**
 * @brief Zero a block.
 *
 * Writes through a volatile pointer on purpose. Given an ordinary loop -- or a
 * compound-literal assignment, which is how this started life in notes.c --
 * GCC recognises the pattern and emits a call to memset, which a zapp may not
 * import. Volatile is what stops the optimiser being clever enough to break the
 * build at runtime.
 */
void z_zero(void *dst, uint32_t len);

/** Copy @p len bytes. Not overlap-safe; see z_move(). */
void z_copy(void *dst, const void *src, uint32_t len);

/** Copy @p len bytes, correctly when the ranges overlap. */
void z_move(void *dst, const void *src, uint32_t len);

/** @return the length of @p s, excluding the terminator. */
uint32_t z_len(const char *s);

/**
 * @brief A pseudo-random number, from a seed the caller keeps.
 *
 * xorshift32: three shifts and three xors, no table, no libc, and no hidden
 * global -- the state is the caller's, which matters here for the same reason
 * everything else per-instance does. Two copies of one zapp share .bss, so a
 * file-scope generator would mean two Minesweeper boards drawing from one
 * sequence and, on the same tick, laying identical mines.
 *
 * Not for anything that has to be unguessable. A zero seed is replaced with a
 * constant rather than sticking at zero forever, which is xorshift's one trap.
 */
uint32_t z_rand32(uint32_t *state);

/** @return a value in [0, @p n). 0 if @p n is 0. */
uint32_t z_rand_below(uint32_t *state, uint32_t n);

/** @return true if the two strings are equal. */
bool z_eq(const char *a, const char *b);

/** @return true if @p s ends with @p suffix. */
bool z_ends_with(const char *s, const char *suffix);

/**
 * @brief Copy @p src into a @p cap byte buffer, truncating rather than
 *        overflowing, and always terminating.
 *
 * @return the number of bytes written, excluding the terminator.
 */
uint32_t z_strcpy(char *dst, uint32_t cap, const char *src);

/**
 * @brief Append @p src at offset @p at in a @p cap byte buffer.
 *
 * @return the new length, or 0 if the result would not fit -- which the callers
 *         chain on, so a single overflow anywhere aborts the whole expression.
 */
uint32_t z_append(char *dst, uint32_t at, uint32_t cap, const char *src);

/** Append @p value in decimal. @return the new length, or 0 if it would not fit. */
uint32_t z_append_u32(char *dst, uint32_t at, uint32_t cap, uint32_t value);

/** Append @p value in decimal, zero-padded to @p digits. Same return. */
uint32_t z_append_pad(char *dst, uint32_t at, uint32_t cap, uint32_t value,
		      uint32_t digits);

/**
 * @brief The last path component of @p path.
 *
 * Returns a pointer into @p path, not a copy. A path ending in a separator
 * yields the empty string, which is the caller's problem to notice.
 */
const char *z_basename(const char *path);

/**
 * @brief Truncate @p path at its last separator, in place.
 *
 * How a zapp goes up a directory. It cannot append ".." instead: the desktop's
 * shim rejects that as a component on purpose, so that the shim and the
 * filesystem can never disagree about what a path means.
 *
 * @return false, leaving @p path alone, if there is nothing above it.
 */
bool z_parent(char *path);

/**
 * @brief Build "<dir>/<name>" in @p out.
 *
 * @return false if the result would not fit, in which case @p out holds an
 *         empty string rather than a truncated path -- half a path is far more
 *         dangerous than none, because it still opens something.
 */
bool z_path_join(char *out, uint32_t cap, const char *dir, const char *name);

#endif /* ZAPPS_LIB_ZAPPLIB_H_ */
