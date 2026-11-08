// HTTP authentication for the relay's mutating endpoints (POST /setup, /ota, /contact/*, /debug/*):
//   - Active secret: admin_pass when non-empty, otherwise token when non-empty; if both are empty (unprovisioned
//     first-time setup), requests are permitted so initial setup can set credentials.
//   - Accepted Authorization header schemes (ASCII case-insensitive):
//       Authorization: Bearer <secret>
//       Authorization: Basic <base64(<any-user>:<secret>)>
//   - Secret comparison runs in constant time over the configured secret length.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Active secret (admin_pass if non-empty, else token if non-empty), or NULL when neither is configured.
const char *http_auth_secret(const char *admin_pass, const char *token);

// True if auth_hdr satisfies the active secret, or if neither admin_pass nor token is configured.
bool http_auth_ok(const char *auth_hdr, const char *admin_pass, const char *token);

#ifdef __cplusplus
}
#endif
