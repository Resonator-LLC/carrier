/*  sha256.h — Self-contained SHA-256 (FIPS 180-4).
 *
 *  Public domain. No external dependencies.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <string.h>

typedef struct { uint32_t state[8]; uint64_t count; uint8_t buf[64]; } SHA256Ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define SHA256_CH(x,y,z)  (((x)&(y))^(~(x)&(z)))
#define SHA256_MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define SHA256_S0(x) (SHA256_ROR(x,2)^SHA256_ROR(x,13)^SHA256_ROR(x,22))
#define SHA256_S1(x) (SHA256_ROR(x,6)^SHA256_ROR(x,11)^SHA256_ROR(x,25))
#define SHA256_s0(x) (SHA256_ROR(x,7)^SHA256_ROR(x,18)^((x)>>3))
#define SHA256_s1(x) (SHA256_ROR(x,17)^SHA256_ROR(x,19)^((x)>>10))

static void sha256_transform(uint32_t s[8], const uint8_t blk[64])
{
    uint32_t W[64], a,b,c,d,e,f,g,h,T1,T2;
    int i;
    for (i = 0; i < 16; i++)
        W[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|
               ((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (i = 16; i < 64; i++)
        W[i] = SHA256_s1(W[i-2]) + W[i-7] + SHA256_s0(W[i-15]) + W[i-16];
    a=s[0];b=s[1];c=s[2];d=s[3];e=s[4];f=s[5];g=s[6];h=s[7];
    for (i = 0; i < 64; i++) {
        T1 = h + SHA256_S1(e) + SHA256_CH(e,f,g) + SHA256_K[i] + W[i];
        T2 = SHA256_S0(a) + SHA256_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+T1;d=c;c=b;b=a;a=T1+T2;
    }
    s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;
    s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}

static void sha256_init(SHA256Ctx *ctx)
{
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->count = 0;
    memset(ctx->buf, 0, sizeof(ctx->buf));
}

static void sha256_update(SHA256Ctx *ctx, const uint8_t *data, size_t len)
{
    size_t i, fill = (size_t)(ctx->count % 64);
    ctx->count += (uint64_t)len;
    if (fill && fill + len >= 64) {
        memcpy(ctx->buf + fill, data, 64 - fill);
        sha256_transform(ctx->state, ctx->buf);
        data += 64 - fill; len -= 64 - fill; fill = 0;
    }
    for (i = 0; i + 63 < len; i += 64)
        sha256_transform(ctx->state, data + i);
    if (len > i)
        memcpy(ctx->buf + fill, data + i, len - i);
}

static void sha256_final(SHA256Ctx *ctx, uint8_t out[32])
{
    int i;
    uint8_t pad[64];
    uint64_t bits = ctx->count * 8;
    size_t fill = (size_t)(ctx->count % 64);
    size_t pad_len = (fill < 56) ? (56 - fill) : (120 - fill);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    sha256_update(ctx, pad, pad_len);
    uint8_t len_bytes[8];
    for (i = 7; i >= 0; i--) { len_bytes[i] = (uint8_t)(bits & 0xff); bits >>= 8; }
    sha256_update(ctx, len_bytes, 8);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >>  8);
        out[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

#endif /* SHA256_H */
