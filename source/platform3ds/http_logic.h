#ifndef HTTP_LOGIC_H
#define HTTP_LOGIC_H

/* Pure helpers for the audio download: no console headers, so the native
 * tests can exercise them on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* How many Location hops one request follows before giving up. */
#define HTTP_MAX_REDIRECTS 4

/* Passes over every CDN URL that may fail in a row, with no byte gained,
 * before the download gives up. With the delays below that is about 45 s:
 * enough for the console to wake from a closed lid and rejoin Wi-Fi. */
#define DOWNLOAD_MAX_FAILED_PASSES 8

/* Times one stall may ask storage-resolve for fresh CDN URLs. */
#define DOWNLOAD_MAX_REFRESHES 3

static inline bool http_status_is_redirect(int status)
{
	return status == 301 || status == 302 || status == 303 ||
	       status == 307 || status == 308;
}

/* Wait before the next pass after `failures` passes in a row came back
 * empty: 1 s, 2 s, 4 s, then 8 s each. */
static inline unsigned download_retry_delay_ms(int failures)
{
	if (failures <= 1)
		return 1000;
	if (failures >= 4)
		return 8000;
	return 1000u << (failures - 1);
}

/* Resolves a Location header value against the https URL that answered it,
 * into an absolute https URL. `location` may run on to the end of its header
 * line. Plain http is refused: there is no TLS-less transport here. */
static inline bool http_redirect_url(const char *from_url,
                                     const char *location,
                                     char *out, size_t outlen)
{
	if (!from_url || !location || !out || !outlen)
		return false;
	while (*location == ' ' || *location == '\t')
		location++;
	size_t loc_len = strcspn(location, "\r\n");
	while (loc_len && (location[loc_len - 1] == ' ' ||
	                   location[loc_len - 1] == '\t'))
		loc_len--;
	if (!loc_len)
		return false;

	const char *prefix;
	size_t prefix_len;
	if (loc_len > 8 && strncmp(location, "https://", 8) == 0) {
		prefix = "";
		prefix_len = 0;
	} else if (loc_len > 2 && location[0] == '/' && location[1] == '/') {
		prefix = "https:";
		prefix_len = 6;
	} else if (location[0] == '/') {
		if (strncmp(from_url, "https://", 8) != 0)
			return false;
		const char *slash = strchr(from_url + 8, '/');
		prefix = from_url;
		prefix_len = slash ? (size_t)(slash - from_url) : strlen(from_url);
		if (prefix_len <= 8)
			return false;
	} else {
		return false;
	}

	if (prefix_len + loc_len >= outlen)
		return false;
	memcpy(out, prefix, prefix_len);
	memcpy(out + prefix_len, location, loc_len);
	out[prefix_len + loc_len] = '\0';
	return true;
}

#endif
