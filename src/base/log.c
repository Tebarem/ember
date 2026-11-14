#include "base/base.h"

#include <stdarg.h>

void embi_log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_CUSTOM, SDL_LOG_PRIORITY_DEBUG, fmt, ap);
    va_end(ap);
}

void embi_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_CUSTOM, SDL_LOG_PRIORITY_INFO, fmt, ap);
    va_end(ap);
}

void embi_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_CUSTOM, SDL_LOG_PRIORITY_WARN, fmt, ap);
    va_end(ap);
}

void embi_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_CUSTOM, SDL_LOG_PRIORITY_ERROR, fmt, ap);
    va_end(ap);
}
