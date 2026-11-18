/* Just enough JSON for what the services answer.
 *
 * Two families. The first (json_str, json_int, json_array_next) looks a key
 * up at any depth of a NUL-terminated text: enough for the flat objects of
 * the OAuth stage and the shallow lists of context-resolve, and no weight
 * beyond that. The second (json_member and the rest) works on bounded slices
 * and only one level at a time, for deep answers such as pathfinder's, where
 * the same key ("name") turns up at several depths of one item.
 */
#ifndef JSON3DS_H
#define JSON3DS_H

#include <stdbool.h>
#include <stddef.h>

/* Copies the string value of `key` into `out`, resolving \" \\ \/ \n \r \t
 * and \uXXXX to UTF-8 (a surrogate pair to one 4-byte character, a lone
 * surrogate to '?'). Returns false when the key is absent, its value is not
 * a string, or it does not fit. */
bool json_str(const char *json, const char *key, char *out, size_t outlen);

/* The same decoding for a value slice that starts at its opening quote. */
bool json_slice_str(const char *v, size_t len, char *out, size_t outlen);

/* The value of `key` among the members of the object `json` (len bytes,
 * leading whitespace allowed), as a slice: keys nested deeper are not
 * looked at. Strings are skipped quote-aware, so braces and escaped quotes
 * inside them are not structure. False when absent or malformed. */
bool json_member(const char *json, size_t len, const char *key,
                 const char **out, size_t *outlen);

/* json_member down a dotted path ("data.searchV2.artists"). */
bool json_path(const char *json, size_t len, const char *path,
               const char **out, size_t *outlen);

/* Walks the values of the array slice `arr`, one per call. `cursor` starts
 * at 0. False at the end, or when the array is cut short. */
bool json_item_next(const char *arr, size_t len, size_t *cursor,
                    const char **out, size_t *outlen);

/* A string or an integer at a dotted path of an object slice. */
bool json_member_str(const char *json, size_t len, const char *path,
                     char *out, size_t outlen);
bool json_member_int(const char *json, size_t len, const char *path,
                     long *out);

/* Reads an integer value. Returns false when absent or not a number. */
bool json_int(const char *json, const char *key, long *out);

/* Walks the objects of the array at `key`, one call per object.
 *
 * `cursor` starts at 0 and is carried between calls. Each object is handed back
 * as a slice, which json_str and json_int can then be applied to -- and since
 * those search at any depth, keys nested inside the object (a track's
 * "metadata" block, say) are reachable without a second pass. Returns false
 * when the array ends.
 *
 * Brace counting is string-aware: a brace inside a value, or an escaped quote,
 * must not be mistaken for structure. */
bool json_array_next(const char *json, const char *key, size_t *cursor,
                     const char **obj, size_t *objlen);

#endif
