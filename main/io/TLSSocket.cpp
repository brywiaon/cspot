#include "TLSSocket.h"

#include <mbedtls/net_sockets.h>  // for mbedtls_net_connect, mbedtls_net_free
#include <mbedtls/ssl.h>          // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <psa/crypto.h>
#include <cstring>                // for strlen, NULL

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellUtils.h"
#include "X509Bundle.h"  // for shouldVerify, attach

/**
 * Platform TLSSocket implementation for the mbedtls
 */
bell::TLSSocket::TLSSocket() {
  this->isClosed = false;
  mbedtls_net_init(&server_fd);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);

  if (bell::X509Bundle::shouldVerify()) {
    bell::X509Bundle::attach(&conf);
  }
  psa_crypto_init();
}

void bell::TLSSocket::open(const std::string& hostUrl, uint16_t port) {
  int ret;
  if ((ret = mbedtls_net_connect(&server_fd, hostUrl.c_str(),
                                 std::to_string(port).c_str(),
                                 MBEDTLS_NET_PROTO_TCP)) != 0) {
    BELL_LOG(error, "http_tls", "failed! connect returned %d\n", ret);
    close();
    return;
  }

  if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {

    BELL_LOG(error, "http_tls", "failed! config returned %d\n", ret);
    close();
    return;
  }

  // Only verify if the X509 bundle is present
  if (bell::X509Bundle::shouldVerify()) {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
    BELL_LOG(error, "http_tls", "failed! setup returned %d\n", ret);
    close();
    return;
  }

  if ((ret = mbedtls_ssl_set_hostname(&ssl, hostUrl.c_str())) != 0) {
    BELL_LOG(error, "http_tls", "failed! hostname returned %d\n", ret);
    close();
    return;
  }
  mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send,
                      mbedtls_net_recv, mbedtls_net_recv_timeout);

  while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      BELL_LOG(error, "http_tls", "failed! config returned %d\n", ret);
      close();
      return;
    }
    BELL_SLEEP_MS(1);
  }
}

size_t bell::TLSSocket::read(uint8_t* buf, size_t len) {
  int ret = 0;
  uint32_t waitedMs = 0;
  do {
    ret = mbedtls_ssl_read(&ssl, buf, len);
    if (ret > 0) {
      return (size_t)ret;
    }
    if (ret == 0) {
      return 0;
    }
    if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE &&
        ret != MBEDTLS_ERR_SSL_TIMEOUT) {
      return 0;
    }
    BELL_SLEEP_MS(5);
    waitedMs += 5;
  } while (waitedMs < 5000);
  return 0;
}

size_t bell::TLSSocket::write(uint8_t* buf, size_t len) {
  int ret = mbedtls_ssl_write(&ssl, buf, len);
  return ret > 0 ? (size_t)ret : 0;
}

size_t bell::TLSSocket::poll() {
  return mbedtls_ssl_get_bytes_avail(&ssl);
}
bool bell::TLSSocket::isOpen() {
  return !isClosed;
}

void bell::TLSSocket::close() {
  mbedtls_net_free(&server_fd);
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&conf);
  this->isClosed = true;
}
