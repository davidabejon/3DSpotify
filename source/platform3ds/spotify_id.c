#include "spotify_id.h"

#include <stdio.h>
#include <string.h>

static const char B62[] =
	"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

/* Long division of the 16-byte number by 62, one digit per pass, least
 * significant first. */
void sp_id_from_gid(const uint8_t gid[16], char out[SP_ID_LEN + 1])
{
	uint8_t n[16];
	memcpy(n, gid, 16);
	for (int i = SP_ID_LEN - 1; i >= 0; i--) {
		unsigned rem = 0;
		for (int j = 0; j < 16; j++) {
			unsigned cur = (rem << 8) | n[j];
			n[j] = (uint8_t)(cur / 62);
			rem = cur % 62;
		}
		out[i] = B62[rem];
	}
	out[SP_ID_LEN] = '\0';
}

bool sp_gid_from_id(const char *id, uint8_t gid[16])
{
	memset(gid, 0, 16);
	if (!id || strlen(id) != SP_ID_LEN)
		return false;
	for (int i = 0; i < SP_ID_LEN; i++) {
		const char *d = strchr(B62, id[i]);
		if (!d)
			goto bad;
		unsigned carry = (unsigned)(d - B62);
		for (int j = 15; j >= 0; j--) {
			unsigned cur = gid[j] * 62u + carry;
			gid[j] = (uint8_t)cur;
			carry = cur >> 8;
		}
		if (carry)
			goto bad;       /* more than 128 bits */
	}
	return true;
bad:
	memset(gid, 0, 16);
	return false;
}

bool sp_uri_from_gid(sp_kind kind, const uint8_t gid[16], char *out, size_t len)
{
	static const char *const NAMES[] = { "track", "album", "artist" };
	if (!out || len == 0)
		return false;
	out[0] = '\0';
	if ((unsigned)kind >= sizeof(NAMES) / sizeof(NAMES[0]))
		return false;
	char id[SP_ID_LEN + 1];
	sp_id_from_gid(gid, id);
	int n = snprintf(out, len, "spotify:%s:%s", NAMES[kind], id);
	if (n < 0 || (size_t)n >= len) {
		out[0] = '\0';
		return false;
	}
	return true;
}
