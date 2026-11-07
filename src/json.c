/* json.c - read values out of a yyjson document by JSON pointer. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "json.h"

void
json_str(yyjson_val *root, const char *ptr, char *dst, size_t size)
{
	const char *s = yyjson_get_str(yyjson_ptr_get(root, ptr));

	snprintf(dst, size, "%s", s != NULL ? s : "");
}

/* 128-bit counters arrive as reals and are clamped. */
int64_t
json_int(yyjson_val *root, const char *ptr, int64_t def)
{
	yyjson_val *v = yyjson_ptr_get(root, ptr);
	const char *s;
	char *end;
	long long n;
	double d;

	if (yyjson_is_uint(v))
		return yyjson_get_uint(v) > INT64_MAX ? INT64_MAX :
		    (int64_t)yyjson_get_uint(v);
	if (yyjson_is_sint(v))
		return yyjson_get_sint(v);
	if (yyjson_is_real(v)) {
		d = yyjson_get_real(v);
		return d >= 9.2e18 ? INT64_MAX : d < 0 ? def : (int64_t)d;
	}
	if ((s = yyjson_get_str(v)) != NULL) {
		errno = 0;
		n = strtoll(s, &end, 10);
		if (errno == 0 && end != s && *end == '\0')
			return n;
	}
	return def;
}

int8_t
json_bool(yyjson_val *root, const char *ptr)
{
	yyjson_val *v = yyjson_ptr_get(root, ptr);

	return yyjson_is_bool(v) ? (int8_t)yyjson_get_bool(v) : -1;
}
