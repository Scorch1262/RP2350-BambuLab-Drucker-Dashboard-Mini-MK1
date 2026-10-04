/* Mutex-Typ fuer MBEDTLS_THREADING_ALT (Implementierung in tls.cpp des Dashboards) */
#ifndef DD_THREADING_ALT_H
#define DD_THREADING_ALT_H
typedef struct mbedtls_threading_mutex_t {
    void *h;
    char is_valid;
} mbedtls_threading_mutex_t;
#endif
