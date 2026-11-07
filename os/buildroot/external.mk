include $(sort $(wildcard $(BR2_EXTERNAL_OSCAIRN_PATH)/package/*/*.mk))

# Buildroot's libcurl names a CA bundle for OpenSSL and wolfSSL but not for Mbed TLS, and curl's
# configure does not look for one when cross-compiling, so every certificate would be refused.
ifeq ($(BR2_PACKAGE_LIBCURL_MBEDTLS),y)
LIBCURL_CONF_OPTS += --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt
endif
