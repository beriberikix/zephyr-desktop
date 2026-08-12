/*
 * zapplib — see zapplib.h for why this exists at all.
 *
 * Everything here is written to survive an optimiser that would rather call
 * libc. Byte-at-a-time loops through volatile pointers are not an accident and
 * not a performance oversight; they are the only way to be sure the compiler
 * does not helpfully replace them with memset or memcpy.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <lib/zapplib.h>

void z_zero(void *dst, uint32_t len)
{
	volatile unsigned char *byte = dst;

	while (len-- > 0u) {
		*byte++ = 0u;
	}
}

void z_copy(void *dst, const void *src, uint32_t len)
{
	volatile unsigned char *to = dst;
	const unsigned char *from = src;

	while (len-- > 0u) {
		*to++ = *from++;
	}
}

void z_move(void *dst, const void *src, uint32_t len)
{
	volatile unsigned char *to = dst;
	const unsigned char *from = src;

	/* Back to front when the destination is above the source and they
	 * overlap. Editing a text buffer in place -- deleting a run and closing
	 * the gap -- is exactly this case, and getting it wrong corrupts the
	 * document rather than crashing, which is worse.
	 */
	if (to > (volatile unsigned char *)from) {
		to += len;
		from += len;
		while (len-- > 0u) {
			*--to = *--from;
		}
		return;
	}

	while (len-- > 0u) {
		*to++ = *from++;
	}
}

uint32_t z_len(const char *s)
{
	const char *p = s;

	while (*p != '\0') {
		p++;
	}

	return (uint32_t)(p - s);
}

bool z_eq(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}

	return *a == *b;
}

bool z_ends_with(const char *s, const char *suffix)
{
	uint32_t slen = z_len(s);
	uint32_t xlen = z_len(suffix);

	return xlen <= slen && z_eq(s + slen - xlen, suffix);
}

uint32_t z_strcpy(char *dst, uint32_t cap, const char *src)
{
	uint32_t at = 0;

	if (cap == 0u) {
		return 0;
	}

	while (src[at] != '\0' && at + 1u < cap) {
		dst[at] = src[at];
		at++;
	}

	dst[at] = '\0';
	return at;
}

uint32_t z_append(char *dst, uint32_t at, uint32_t cap, const char *src)
{
	while (*src != '\0') {
		if (at + 1u >= cap) {
			return 0;
		}
		dst[at++] = *src++;
	}

	dst[at] = '\0';
	return at;
}

uint32_t z_append_u32(char *dst, uint32_t at, uint32_t cap, uint32_t value)
{
	return z_append_pad(dst, at, cap, value, 1);
}

uint32_t z_append_pad(char *dst, uint32_t at, uint32_t cap, uint32_t value,
		      uint32_t digits)
{
	char buf[10];
	uint32_t n = 0;

	do {
		buf[n++] = (char)('0' + (value % 10u));
		value /= 10u;
	} while (value != 0u && n < sizeof(buf));

	while (n < digits && n < sizeof(buf)) {
		buf[n++] = '0';
	}

	while (n > 0u) {
		if (at + 1u >= cap) {
			return 0;
		}
		dst[at++] = buf[--n];
	}

	dst[at] = '\0';
	return at;
}

const char *z_basename(const char *path)
{
	const char *last = path;

	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/') {
			last = p + 1;
		}
	}

	return last;
}

bool z_path_join(char *out, uint32_t cap, const char *dir, const char *name)
{
	uint32_t at;

	if (cap == 0u) {
		return false;
	}

	out[0] = '\0';

	at = z_append(out, 0, cap, dir);
	at = at != 0u ? z_append(out, at, cap, "/") : 0u;
	at = at != 0u ? z_append(out, at, cap, name) : 0u;

	if (at == 0u) {
		out[0] = '\0';
		return false;
	}

	return true;
}
