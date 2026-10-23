#ifndef NP_CRYPTO_H
#define NP_CRYPTO_H
#include <stddef.h>

void md5(const void *data, size_t n, unsigned char out[16]);
void sha256(const void *data, size_t n, unsigned char out[32]);
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, unsigned char out[32]);
/* Base64 URL 安全字母表，无填充；out 容量 >= 4*n/3+1 */
void b64url(const unsigned char *in, size_t n, char *out);

#endif
