/* json.h - read values out of a yyjson document by JSON pointer. */
#ifndef NASCHECK_JSON_H
#define NASCHECK_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "yyjson.h"

/*
 * Missing or mistyped values yield the default ("" for strings). Integers
 * are also read from numeric strings, as zpool status -j prints them.
 */
void	json_str(yyjson_val *root, const char *ptr, char *dst, size_t size);
int64_t	json_int(yyjson_val *root, const char *ptr, int64_t def);
int8_t	json_bool(yyjson_val *root, const char *ptr);	/* 1, 0, -1 */

#endif
