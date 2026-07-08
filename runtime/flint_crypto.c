#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 * CRC32
 * ================================================================ */

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_ready = 1;
}

int64_t flint_crc32(const char* data, int64_t len) {
    if (!data || len <= 0) return 0;
    if (!crc32_table_ready) crc32_build_table();
    uint32_t crc = 0xFFFFFFFF;
    for (int64_t i = 0; i < len; i++) {
        uint8_t byte = (uint8_t)data[i];
        crc = crc32_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return (int64_t)(crc ^ 0xFFFFFFFF);
}

int64_t flint_crc32_str(const char* s) {
    if (!s) return 0;
    return flint_crc32(s, (int64_t)strlen(s));
}

/* ================================================================
 * MD5 (RFC 1321)
 * ================================================================ */

static const uint32_t md5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

static const uint32_t md5_s[64] = {
     7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
     5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
     4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
     6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

#define MD5_LEFTROTATE(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i*4] |
               ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) |
               ((uint32_t)block[i*4+3] << 24);
    }
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + MD5_LEFTROTATE(a + f + md5_K[i] + x[g], md5_s[i]);
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_hash(const uint8_t* input, uint64_t len, uint8_t digest[16]) {
    uint32_t state[4];
    state[0] = 0x67452301;
    state[1] = 0xefcdab89;
    state[2] = 0x98badcfe;
    state[3] = 0x10325476;

    uint64_t bitlen = len * 8;
    uint64_t full_blocks = len / 64;
    for (uint64_t i = 0; i < full_blocks; i++) {
        md5_transform(state, input + i * 64);
    }

    uint8_t tail[128];
    uint64_t remaining = len % 64;
    memcpy(tail, input + full_blocks * 64, remaining);
    tail[remaining] = 0x80;
    uint64_t padlen;
    if (remaining < 56) {
        padlen = 56 - remaining - 1;
        memset(tail + remaining + 1, 0, padlen);
        padlen = remaining + 1 + padlen;
    } else {
        padlen = 64 - remaining - 1 + 56;
        memset(tail + remaining + 1, 0, 64 - remaining - 1);
        md5_transform(state, tail);
        memset(tail, 0, 56);
        padlen = 56;
    }
    for (int i = 0; i < 8; i++) {
        tail[padlen + i] = (uint8_t)((bitlen >> (i * 8)) & 0xFF);
    }
    md5_transform(state, tail);
    if (padlen + 8 > 64) {
        md5_transform(state, tail + 64);
    }

    for (int i = 0; i < 4; i++) {
        digest[i*4]   = (uint8_t)( state[i]        & 0xFF);
        digest[i*4+1] = (uint8_t)((state[i] >> 8)  & 0xFF);
        digest[i*4+2] = (uint8_t)((state[i] >> 16) & 0xFF);
        digest[i*4+3] = (uint8_t)((state[i] >> 24) & 0xFF);
    }
}

static char* md5_hex(const uint8_t digest[16]) {
    char* out = (char*)malloc(33);
    if (!out) return NULL;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = hex[(digest[i] >> 4) & 0xF];
        out[i*2+1] = hex[ digest[i]        & 0xF];
    }
    out[32] = '\0';
    return out;
}

char* flint_md5(const char* data, int64_t len) {
    if (!data || len == 0) {
        const char* empty = "";
        uint8_t digest[16];
        md5_hash((const uint8_t*)empty, 0, digest);
        return md5_hex(digest);
    }
    uint64_t ulen;
    if (len < 0) {
        ulen = strlen(data);
    } else {
        ulen = (uint64_t)len;
    }
    uint8_t digest[16];
    md5_hash((const uint8_t*)data, ulen, digest);
    return md5_hex(digest);
}

char* flint_md5_str(const char* s) {
    return flint_md5(s, -1);
}

/* ================================================================
 * SHA-256 (FIPS 180-4)
 * ================================================================ */

static const uint32_t sha256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_sig0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_sig1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  |
               ((uint32_t)block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = SHA256_sig1(w[i-2]) + w[i-7] + SHA256_sig0(w[i-15]) + w[i-16];
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_SIG1(e) + SHA256_CH(e, f, g) + sha256_K[i] + w[i];
        uint32_t t2 = SHA256_SIG0(a) + SHA256_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_hash(const uint8_t* input, uint64_t len, uint8_t digest[32]) {
    uint32_t state[8];
    state[0] = 0x6a09e667;
    state[1] = 0xbb67ae85;
    state[2] = 0x3c6ef372;
    state[3] = 0xa54ff53a;
    state[4] = 0x510e527f;
    state[5] = 0x9b05688c;
    state[6] = 0x1f83d9ab;
    state[7] = 0x5be0cd19;

    uint64_t bitlen = len * 8;
    uint64_t full_blocks = len / 64;
    for (uint64_t i = 0; i < full_blocks; i++) {
        sha256_transform(state, input + i * 64);
    }

    uint8_t tail[128];
    uint64_t remaining = len % 64;
    memcpy(tail, input + full_blocks * 64, remaining);
    tail[remaining] = 0x80;
    uint64_t padlen;
    if (remaining < 56) {
        padlen = 56 - remaining - 1;
        memset(tail + remaining + 1, 0, padlen);
        padlen = remaining + 1 + padlen;
    } else {
        padlen = 64 - remaining - 1 + 56;
        memset(tail + remaining + 1, 0, 64 - remaining - 1);
        sha256_transform(state, tail);
        memset(tail, 0, 56);
        padlen = 56;
    }
    for (int i = 0; i < 8; i++) {
        tail[padlen + i] = (uint8_t)((bitlen >> (56 - i * 8)) & 0xFF);
    }
    sha256_transform(state, tail);
    if (padlen + 8 > 64) {
        sha256_transform(state, tail + 64);
    }

    for (int i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)((state[i] >> 24) & 0xFF);
        digest[i*4+1] = (uint8_t)((state[i] >> 16) & 0xFF);
        digest[i*4+2] = (uint8_t)((state[i] >> 8)  & 0xFF);
        digest[i*4+3] = (uint8_t)( state[i]        & 0xFF);
    }
}

static char* sha256_hex(const uint8_t digest[32]) {
    char* out = (char*)malloc(65);
    if (!out) return NULL;
    const char* hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hex[(digest[i] >> 4) & 0xF];
        out[i*2+1] = hex[ digest[i]        & 0xF];
    }
    out[64] = '\0';
    return out;
}

char* flint_sha256(const char* data, int64_t len) {
    if (!data || len == 0) {
        const char* empty = "";
        uint8_t digest[32];
        sha256_hash((const uint8_t*)empty, 0, digest);
        return sha256_hex(digest);
    }
    uint64_t ulen;
    if (len < 0) {
        ulen = strlen(data);
    } else {
        ulen = (uint64_t)len;
    }
    uint8_t digest[32];
    sha256_hash((const uint8_t*)data, ulen, digest);
    return sha256_hex(digest);
}

char* flint_sha256_str(const char* s) {
    return flint_sha256(s, -1);
}
