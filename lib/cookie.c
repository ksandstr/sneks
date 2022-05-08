#include <stdint.h>
#include <stdbool.h>
#include <ccan/siphash/siphash.h>
#include <l4/types.h>
#include <sneks/cookie.h>

struct cookie_info {
	uint64_t time;
	int consumer;
	uint64_t object;
} __attribute__((packed));

uint32_t gen_cookie(const struct cookie_key *key, L4_Clock_t now, unsigned object, int pid) {
	struct cookie_info raw = { .time = now.raw & ~(L4_Word64_t)COOKIE_PERIOD_US, .consumer = pid, .object = object };
	return siphash_2_4(&raw, sizeof raw, key->key);
}

bool validate_cookie(uint32_t cookie, const struct cookie_key *key, L4_Clock_t now, unsigned object, int pid) {
	L4_Clock_t next = { .raw = now.raw + COOKIE_PERIOD_US + 1 };
	return gen_cookie(key, now, object, pid) == cookie || gen_cookie(key, next, object, pid) == cookie;
}
