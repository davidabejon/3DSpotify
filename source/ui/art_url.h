/* Cover-art URLs: only Spotify's image CDN is fetched. Shared by the
 * artwork (top screen) and the result thumbnails. Pure C, no 3DS headers,
 * tested in tests/native_logic_test.c. */
#ifndef ART_URL_H
#define ART_URL_H

#include <stdbool.h>
#include <string.h>

#define ART_HOST "i.scdn.co"
#define ART_URL_PREFIX "https://" ART_HOST "/"

/* True for "https://i.scdn.co/<path>" with a non-empty path of printable
 * ASCII and no backslash or fragment: nothing that could reach another
 * host or smuggle bytes into the request line. */
static inline bool art_url_allowed(const char *url)
{
	const size_t prefix_len = sizeof(ART_URL_PREFIX) - 1;
	if (!url || strncmp(url, ART_URL_PREFIX, prefix_len) != 0 ||
	    url[prefix_len] == '\0')
		return false;
	for (const unsigned char *p = (const unsigned char *)url + prefix_len;
	     *p; p++)
		if (*p <= 0x20 || *p > 0x7e || *p == '\\' || *p == '#')
			return false;
	return true;
}

/* The request path ("/image/...") of an allowed URL, else NULL. */
static inline const char *art_url_path(const char *url)
{
	if (!art_url_allowed(url))
		return NULL;
	return url + sizeof(ART_URL_PREFIX) - 2;
}

#endif
