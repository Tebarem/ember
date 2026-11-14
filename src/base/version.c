#include <ember/ember.h>

EMB_API const char *emb_version_string(void)
{
    return EMB_VERSION_STRING;
}

EMB_API uint32_t emb_version(void)
{
    return ((uint32_t)EMB_VERSION_MAJOR << 16) | ((uint32_t)EMB_VERSION_MINOR << 8) |
           (uint32_t)EMB_VERSION_PATCH;
}
