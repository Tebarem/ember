#pragma once
/*
 * Mbed TLS threading layer over SDL (MBEDTLS_THREADING_ALT, see crypto_user_config.h): the types the library asks
 * for. The functions are installed once by embi_net_tls_global_init (tls.c).
 */
#include <SDL3/SDL_mutex.h>

typedef SDL_Mutex *mbedtls_platform_mutex_t;
typedef SDL_Condition *mbedtls_platform_condition_variable_t;
