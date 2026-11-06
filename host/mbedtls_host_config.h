/* Mbed TLS settings for the Listenica host (appended to Mbed TLS's default config).
 * The debug module stamps every message with its source file's full path at build time, which
 * would publish the build machine's folders (and user name) inside the released binaries. The host
 * never enables Mbed TLS debug output, so it is compiled out. */
#undef MBEDTLS_DEBUG_C
