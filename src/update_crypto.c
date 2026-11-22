#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wchar.h>

/* Recovery must not load a bcrypt.dll from the portable or recovery directory. */
typedef struct {
    HMODULE module;
#define API(member) __typeof__(&member) member
    API(BCryptOpenAlgorithmProvider);
    API(BCryptCloseAlgorithmProvider);
    API(BCryptCreateHash);
    API(BCryptDestroyHash);
    API(BCryptHashData);
    API(BCryptFinishHash);
    API(BCryptImportKeyPair);
    API(BCryptDestroyKey);
    API(BCryptVerifySignature);
#undef API
} CryptoApi;

static int system_crypto(CryptoApi *api) {
    wchar_t path[MAX_PATH];
    static const wchar_t name[] = L"\\bcrypt.dll";
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH || n + sizeof name / sizeof *name > MAX_PATH) return 0;
    memcpy(path + n, name, sizeof name);
    api->module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api->module) return 0;
#define LOAD(member) do { \
    api->member = (__typeof__(api->member))GetProcAddress(api->module, #member); \
    if (!api->member) { FreeLibrary(api->module); return 0; } \
} while (0)
    LOAD(BCryptOpenAlgorithmProvider);
    LOAD(BCryptCloseAlgorithmProvider);
    LOAD(BCryptCreateHash);
    LOAD(BCryptDestroyHash);
    LOAD(BCryptHashData);
    LOAD(BCryptFinishHash);
    LOAD(BCryptImportKeyPair);
    LOAD(BCryptDestroyKey);
    LOAD(BCryptVerifySignature);
#undef LOAD
    return 1;
}

int sg_update_sha256(const unsigned char *data, uint32_t len, unsigned char *hash) {
    CryptoApi api;
    if (!system_crypto(&api)) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = api.BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
             api.BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0 &&
             api.BCryptHashData(h, (PUCHAR)data, len, 0) == 0 &&
             api.BCryptFinishHash(h, hash, 32, 0) == 0;
    if (h) api.BCryptDestroyHash(h);
    if (alg) api.BCryptCloseAlgorithmProvider(alg, 0);
    FreeLibrary(api.module);
    return ok;
}

int sg_update_p256(const unsigned char *key, const unsigned char *hash,
                   const unsigned char *signature) {
    struct {
        BCRYPT_ECCKEY_BLOB header;
        unsigned char points[64];
    } blob = {{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32}, {0}};
    memcpy(blob.points, key, sizeof blob.points);
    CryptoApi api;
    if (!system_crypto(&api)) return 0;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE pub = NULL;
    int ok = api.BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) == 0 &&
             api.BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &pub,
                                     (PUCHAR)&blob, sizeof blob, 0) == 0 &&
             api.BCryptVerifySignature(pub, NULL, (PUCHAR)hash, 32, (PUCHAR)signature, 64, 0) == 0;
    if (pub) api.BCryptDestroyKey(pub);
    if (alg) api.BCryptCloseAlgorithmProvider(alg, 0);
    FreeLibrary(api.module);
    return ok;
}
#else
/* OpenSSL is only used by POSIX development builds/tests, not shipped. */
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/params.h>

int sg_update_sha256(const unsigned char *data, uint32_t len, unsigned char *hash) {
    unsigned int size = 0;
    return EVP_Digest(data, len, hash, &size, EVP_sha256(), NULL) == 1 && size == 32;
}

int sg_update_p256(const unsigned char *key, const unsigned char *hash,
                   const unsigned char *signature) {
    int ok = 0;
    EVP_PKEY *pub = NULL;
    EVP_PKEY_CTX *verify = NULL;
    EVP_PKEY_CTX *import = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(signature, 32, NULL), *s = BN_bin2bn(signature + 32, 32, NULL);
    char curve[] = "prime256v1";
    unsigned char encoded[65] = {4}, der[72], *output = der;
    memcpy(encoded + 1, key, 64);
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, curve, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, encoded, sizeof encoded),
        OSSL_PARAM_construct_end()
    };

    if (!sig || !r || !s || ECDSA_SIG_set0(sig, r, s) != 1) goto done;
    r = s = NULL; /* Now owned by sig. */
    int der_len = i2d_ECDSA_SIG(sig, &output);
    if (der_len <= 0 || !import || EVP_PKEY_fromdata_init(import) != 1 ||
        EVP_PKEY_fromdata(import, &pub, EVP_PKEY_PUBLIC_KEY, params) != 1) goto done;
    verify = EVP_PKEY_CTX_new(pub, NULL);
    ok = verify && EVP_PKEY_verify_init(verify) == 1 &&
         EVP_PKEY_CTX_set_signature_md(verify, EVP_sha256()) == 1 &&
         EVP_PKEY_verify(verify, der, (size_t)der_len, hash, 32) == 1;
done:
    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(sig);
    EVP_PKEY_CTX_free(verify);
    EVP_PKEY_free(pub);
    EVP_PKEY_CTX_free(import);
    return ok;
}
#endif
