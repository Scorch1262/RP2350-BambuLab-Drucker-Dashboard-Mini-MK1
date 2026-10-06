/*
 * Schlanke mbedTLS-Konfiguration fuer das Drucker-Dashboard (RP2350).
 *
 * Nur TLS-CLIENT (Bambu-MQTT Port 8883, FTPS Port 990, A1-Kamera
 * Port 6000, RTSPS Port 322, optional OctoPrint-HTTPS/MQTT-TLS).
 * Zertifikate werden - wie im Python-Original (ssl._create_unverified_context)
 * - NICHT geprueft, da die Drucker selbstsignierte Zertifikate verwenden.
 *
 * Ersetzt die Original-mbedtls_config.h aus mbedTLS 3.6.x.
 */
#ifndef DD_MBEDTLS_CONFIG_H
#define DD_MBEDTLS_CONFIG_H

/* ---- System ---- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT      /* mbedtls_hardware_poll() in tls.cpp */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_HAVE_TIME                 /* fuer TLS-1.3-Tickets/Session-Handling */
#define MBEDTLS_PLATFORM_MS_TIME_ALT      /* mbedtls_ms_time() in tls.cpp */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT             /* Mutexe in tls.cpp (PSA-Krypto ist global) */

/* ---- Kryptografie ---- */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_PADDING_PKCS7
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_GENPRIME
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE 4
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 0
#define MBEDTLS_MPI_WINDOW_SIZE 3

/* PSA-Krypto (von TLS 1.3 in mbedTLS 3.6 vorausgesetzt) */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/* ---- X.509 (Serverzertifikat parsen, nicht pruefen) ---- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* ---- TLS ---- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE   /* von TLS 1.3 vorausgesetzt */
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_RENEGOTIATION
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
/* DHE-RSA bewusst aus: 2048-Bit-Exponentiation dauert auf dem RP2350 ~1 s je Schritt */

#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif /* DD_MBEDTLS_CONFIG_H */
