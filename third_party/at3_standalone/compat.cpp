#include <cstdarg>
#include <cstdio>

#include "compat.h"

void av_log(int level, const char *fmt, ...) {
	if (level > AV_LOG_WARNING)
		return;
	va_list vl;
	va_start(vl, fmt);
	std::fputs("ATRAC decoder: ", stderr);
	std::vfprintf(stderr, fmt, vl);
	va_end(vl);
}
