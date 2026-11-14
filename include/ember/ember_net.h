#pragma once
/*
 * Ember networking: the embedder's controls over the runtime's network stack. A separate
 * header included by ember.h. C23 only.
 *
 * Nothing goes out to the network until the embedder allows it: a request to a host that no emb_runtime_net_allow
 * pattern matches is a network error (a TypeError for fetch, an error event for XHR, WebSocket and EventSource) and
 * logs one warning naming the host. There is no CORS: a request to an allowed host goes out like a native app's.
 * A handler registered for http or https (emb_runtime_set_resource_handler*) wins over the network.
 *
 * Certificate checks cannot be turned off: TLS 1.2 is the minimum, SNI and hostname verification are always on.
 *
 * Built with the CMake option EMBER_NET (ON by default). With EMBER_NET=OFF the functions below still exist and
 * return false (or do nothing): the runtime behaves as it would without the network stack.
 */

#include <ember/ember.h>

#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Adds an allowlist entry. Patterns (compared after URL host normalization: IDNA, lowercase, IPv6 in brackets):
 *   "host"           that host on any port            "host:port"   that host on that port
 *   "*.example.com"  any subdomain (not example.com itself), with an optional ":port"
 *   "*"              any host
 * IPv6 hosts are written in brackets ("[::1]:8080"). Returns false for a malformed pattern, a null argument, or when
 * the build has no network (EMBER_NET=OFF). Thread-safe.
 * The allowlist is the security boundary and it is checked on the host NAME, before DNS and for every redirect. Ember
 * does not filter the addresses a name resolves to: an allowlisted host may resolve to a private or loopback address
 * (DNS rebinding, split-horizon DNS), so allow only hosts whose DNS you trust. */
EMB_API bool emb_runtime_net_allow(emb_runtime *rt, const char *pattern);

/* Trusts another root certificate (PEM text, or one DER certificate), for a company CA or a test server. A PEM
 * buffer may hold several certificates. Returns false when nothing in the data parses as a certificate or the build
 * has no network. Takes effect for connections made after the call. */
EMB_API bool emb_runtime_net_add_root(emb_runtime *rt, const void *pem_or_der, size_t len);

/* Whether Mozilla's root list that Ember embeds is trusted (default: on). Roots added with emb_runtime_net_add_root
 * are trusted either way. Takes effect for connections made after the call. */
EMB_API void emb_runtime_net_use_bundled_roots(emb_runtime *rt, bool on);

/* The User-Agent header of network requests (default "Ember/<version>"). A copy is kept. */
EMB_API void emb_runtime_net_set_user_agent(emb_runtime *rt, const char *ua);

#ifdef __cplusplus
}
#endif
