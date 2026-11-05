#include "search_path.h"

#include <stdio.h>

bool sp_search_path(const char *query, char *path, size_t path_size)
{
	static const char hex[] = "0123456789ABCDEF";
	char encoded[384];
	size_t n = 0;

	if (!query || !query[0] || !path || path_size == 0)
		return false;

	for (const unsigned char *p = (const unsigned char *)query; *p; p++) {
		unsigned char c = *p;
		if (c == ' ') {
			if (n + 1 >= sizeof(encoded))
				return false;
			encoded[n++] = '+';
		} else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		           c == '.' || c == '~') {
			if (n + 1 >= sizeof(encoded))
				return false;
			encoded[n++] = (char)c;
		} else {
			if (n + 3 >= sizeof(encoded))
				return false;
			encoded[n++] = '%';
			encoded[n++] = hex[c >> 4];
			encoded[n++] = hex[c & 0x0F];
		}
	}
	encoded[n] = '\0';

	int length = snprintf(path, path_size,
	                      "/context-resolve/v1/spotify:search:%s", encoded);
	return length >= 0 && (size_t)length < path_size;
}
