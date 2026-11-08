// Native unit tests for portable HTTP Basic / Bearer authentication (esp32/web/http_auth.c, TASK-01).
#include <stdio.h>
#include <string.h>

#include "http_auth.h"

static int failures, checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

int main(void)
{
    // unprovisioned (both empty / NULL): open for initial setup
    CHECK(http_auth_secret(NULL, NULL) == NULL);
    CHECK(http_auth_secret("", "") == NULL);
    CHECK(http_auth_ok(NULL, "", ""));
    CHECK(http_auth_ok("", NULL, NULL));

    // admin_pass takes precedence over token; falls back to token when admin_pass is empty
    CHECK(!strcmp(http_auth_secret("adm1", "tok1"), "adm1"));
    CHECK(!strcmp(http_auth_secret("", "tok1"), "tok1"));
    CHECK(!strcmp(http_auth_secret(NULL, "tok1"), "tok1"));

    // Bearer scheme: case-insensitive scheme, exact secret match, rejects missing/prefix/suffix
    CHECK(!http_auth_ok(NULL, "s3cr3t", "tok"));
    CHECK(!http_auth_ok("", "s3cr3t", "tok"));
    CHECK(http_auth_ok("Bearer s3cr3t", "s3cr3t", "tok"));
    CHECK(http_auth_ok("  bearer   s3cr3t \r\n", "s3cr3t", "tok"));
    CHECK(!http_auth_ok("Bearer tok", "s3cr3t", "tok"));      // token rejected when admin_pass is set
    CHECK(http_auth_ok("Bearer tok", "", "tok"));             // token accepted when admin_pass is empty
    CHECK(!http_auth_ok("Bearer s3cr3", "s3cr3t", "tok"));    // truncated
    CHECK(!http_auth_ok("Bearer s3cr3t!", "s3cr3t", "tok"));  // extra char
    CHECK(!http_auth_ok("Bearer ", "s3cr3t", "tok"));

    // Basic scheme: "admin:s3cr3t" -> "YWRtaW46czNjcjN0"; ":s3cr3t" -> "OnMzY3IzdA=="
    CHECK(http_auth_ok("Basic YWRtaW46czNjcjN0", "s3cr3t", ""));
    CHECK(http_auth_ok("BASIC OnMzY3IzdA==", "s3cr3t", ""));
    // password containing ':' ("user:a:b:c" -> "dXNlcjphOmI6Yw==")
    CHECK(http_auth_ok("Basic dXNlcjphOmI6Yw==", "a:b:c", ""));
    // wrong password ("admin:wrong" -> "YWRtaW46d3Jvbmc=")
    CHECK(!http_auth_ok("Basic YWRtaW46d3Jvbmc=", "s3cr3t", ""));
    // missing colon ("adminonly" -> "YWRtaW5vbmx5")
    CHECK(!http_auth_ok("Basic YWRtaW5vbmx5", "adminonly", ""));
    // malformed base64 & invalid padding
    CHECK(!http_auth_ok("Basic abc", "s3cr3t", ""));
    CHECK(!http_auth_ok("Basic YWR=taW4", "s3cr3t", ""));
    CHECK(!http_auth_ok("Digest realm=foo", "s3cr3t", ""));

    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
