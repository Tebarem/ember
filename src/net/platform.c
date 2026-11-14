/*
 * Mbed TLS platform hooks of module net (N1): time through SDL, and the single entropy entry point.
 */
#include "net_int.h"

#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/platform_util.h>
#include <time.h>

static mbedtls_time_t sdl_time(mbedtls_time_t *out)
{
    SDL_Time t = 0;
    (void)SDL_GetCurrentTime(&t);
    mbedtls_time_t s = (mbedtls_time_t)(t / SDL_NS_PER_SECOND);
    if (out != nullptr) {
        *out = s;
    }
    return s;
}

struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
    SDL_DateTime dt;
    SDL_Time ns = (SDL_Time)(*tt) * SDL_NS_PER_SECOND;
    if (!SDL_TimeToDateTime(ns, &dt, false)) {
        return nullptr;
    }
    memset(tm_buf, 0, sizeof *tm_buf);
    tm_buf->tm_year = dt.year - 1900;
    tm_buf->tm_mon = dt.month - 1;
    tm_buf->tm_mday = dt.day;
    tm_buf->tm_hour = dt.hour;
    tm_buf->tm_min = dt.minute;
    tm_buf->tm_sec = dt.second;
    tm_buf->tm_wday = dt.day_of_week;
    return tm_buf;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)SDL_GetTicks();
}

void embi_net_platform_init(void)
{
    (void)mbedtls_platform_set_time(sdl_time);
}

/*
 * The one entropy entry point of module net: fills buf with len cryptographically secure random bytes. For now it is
 * Mbed TLS's own OS-backed source (MBEDTLS_PSA_BUILTIN_GET_ENTROPY): BCryptGenRandom(system preferred RNG) on Windows,
 * the getrandom syscall (then /dev/urandom) on Linux, getentropy/arc4random on macOS and the BSDs. To use a secure
 * random function from SDL instead: in crypto_user_config.h replace MBEDTLS_PSA_BUILTIN_GET_ENTROPY with
 * MBEDTLS_PSA_DRIVER_GET_ENTROPY, define mbedtls_platform_get_entropy(flags, &bits, out, n) here as a call of this
 * function (setting bits to 8 * n), and replace the body below. Neither SDL_rand nor src/loader/random.c is secure
 * enough for this.
 */
bool embi_net_entropy(void *buf, size_t len)
{
    size_t bits = 0;
    return mbedtls_platform_get_entropy(0, &bits, buf, len) == 0 && bits >= 8 * len;
}
