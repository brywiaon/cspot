#include "PlainConnection.h"

#ifndef _WIN32
#include <fcntl.h>  // for fcntl, O_NONBLOCK
#include <netdb.h>  // for addrinfo, freeaddrinfo, getaddrinfo
#include <netinet/in.h>   // for IPPROTO_IP, IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/select.h>   // for select, fd_set
#include <sys/errno.h>    // for EAGAIN, EINTR, ETIMEDOUT, errno
#include <sys/socket.h>   // for setsockopt, connect, recv, send, shutdown
#include <sys/time.h>     // for timeval
#include <cstring>        // for memset
#else
#include <ws2tcpip.h>
#endif
#include "BellLogger.h"  // for AbstractLogger
#include "Logger.h"      // for CSPOT_LOG
#include "Packet.h"      // for cspot
#include "Utils.h"       // for extract, pack

using namespace cspot;

static int getErrno();

namespace {
constexpr int SPOTIFY_AP_CONNECT_TIMEOUT_MS = 3000;

bool setSocketBlocking(int socketFd, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0 : 1;
  return ioctlsocket(socketFd, FIONBIO, &mode) == 0;
#else
  int flags = fcntl(socketFd, F_GETFL, 0);
  if (flags < 0) return false;
  if (blocking) flags &= ~O_NONBLOCK;
  else flags |= O_NONBLOCK;
  return fcntl(socketFd, F_SETFL, flags) == 0;
#endif
}

bool connectInProgress(int errorCode) {
#ifdef _WIN32
  return errorCode == WSAEWOULDBLOCK || errorCode == WSAEINPROGRESS ||
         errorCode == WSAEALREADY;
#else
  return errorCode == EINPROGRESS || errorCode == EWOULDBLOCK ||
         errorCode == EAGAIN || errorCode == EALREADY;
#endif
}

bool waitForConnectedSocket(int socketFd, int timeoutMs, int& socketError) {
  fd_set writeSet;
  fd_set errorSet;
  FD_ZERO(&writeSet);
  FD_ZERO(&errorSet);
  FD_SET(socketFd, &writeSet);
  FD_SET(socketFd, &errorSet);
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  int ready = select(socketFd + 1, nullptr, &writeSet, &errorSet, &timeout);
  if (ready <= 0) {
    socketError = ready == 0 ? ETIMEDOUT : getErrno();
    return false;
  }
#ifdef _WIN32
  int errorLength = sizeof(socketError);
#else
  socklen_t errorLength = sizeof(socketError);
#endif
  socketError = 0;
  if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&socketError), &errorLength) != 0) {
    socketError = getErrno();
    return false;
  }
  return socketError == 0 && FD_ISSET(socketFd, &writeSet);
}
}  // namespace

static int getErrno() {
#ifdef _WIN32
  int code = WSAGetLastError();
  if (code == WSAETIMEDOUT)
    return ETIMEDOUT;
  if (code == WSAEINTR)
    return EINTR;
  return code;
#else
  return errno;
#endif
}

PlainConnection::PlainConnection() {
  this->apSock = -1;
};

PlainConnection::~PlainConnection() {
  this->close();
};

void PlainConnection::connect(const std::string& apAddress) {
  struct addrinfo h, *airoot, *ai;
  const size_t separator = apAddress.rfind(":");
  if (separator == std::string::npos || separator == 0 ||
      separator + 1 >= apAddress.size()) {
    CSPOT_LOG(error, "Invalid Spotify AP address <%s>", apAddress.c_str());
    apSock = -1;
    return;
  }
  std::string hostname = apAddress.substr(0, separator);
  std::string portStr = apAddress.substr(separator + 1);
  memset(&h, 0, sizeof(h));
  h.ai_family = AF_INET;
  h.ai_socktype = SOCK_STREAM;
  h.ai_protocol = IPPROTO_TCP;

  // Lookup host
  int resolveError = getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot);
  if (resolveError != 0) {
    CSPOT_LOG(error, "Spotify AP DNS failed host=%s error=%d",
              hostname.c_str(), resolveError);
    apSock = -1;
    return;
  }

  // find the right ai, connect to server
  int lastError = 0;
  for (ai = airoot; ai; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
      continue;

    this->apSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (this->apSock < 0) {
      lastError = getErrno();
      continue;
    }

    setSocketBlocking(this->apSock, false);
    int connectResult = ::connect(this->apSock, (struct sockaddr*)ai->ai_addr,
                                  ai->ai_addrlen);
    int connectError = connectResult == 0 ? 0 : getErrno();
    bool connected = connectResult == 0;
    if (!connected && connectInProgress(connectError)) {
      connected = waitForConnectedSocket(this->apSock,
                                         SPOTIFY_AP_CONNECT_TIMEOUT_MS,
                                         connectError);
    }
    setSocketBlocking(this->apSock, true);
    if (connected) {
#ifdef _WIN32
      uint32_t tv = 3000;
#else
      struct timeval tv;
      tv.tv_sec = 3;
      tv.tv_usec = 0;
#endif
      setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                 sizeof tv);
      setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv,
                 sizeof tv);

      int flag = 1;
      setsockopt(this->apSock, /* socket affected */
                 IPPROTO_TCP,  /* set option at TCP level */
                 TCP_NODELAY,  /* name of option */
                 (char*)&flag, /* the cast is historical cruft */
                 sizeof(int)); /* length of option value */
      break;
    }
    lastError = connectError;

#ifdef _WIN32
    closesocket(this->apSock);
#else
    ::close(this->apSock);
#endif
    apSock = -1;
  }

  freeaddrinfo(airoot);
  if (apSock >= 0) {
    CSPOT_LOG(info, "Connected to Spotify AP <%s>", apAddress.c_str());
  } else {
    CSPOT_LOG(error, "Can't connect to Spotify AP <%s> error=%d timeout=%dms",
              apAddress.c_str(), lastError, SPOTIFY_AP_CONNECT_TIMEOUT_MS);
  }
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  if (!isConnected()) {
    return {};
  }

  // Read packet size
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  if (!isConnected()) {
    return {};
  }

  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));
  if (packetSize < 4 || packetSize > 1024 * 1024) {
    CSPOT_LOG(error, "Invalid plain packet size %u", packetSize);
    close();
    return {};
  }

  packetBuffer.resize(packetSize, 0);

  // Read actual data
  readBlock(packetBuffer.data() + 4, packetSize - 4);
  if (!isConnected()) {
    return {};
  }

  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data) {
  // Calculate full packet length
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

  // Packet structure [PREFIX] + [SIZE] +  [DATA]
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

  // Actually write it to the server
  if (writeBlock(sizeRaw) != sizeRaw.size()) {
    return {};
  }

  return sizeRaw;
}

void PlainConnection::readBlock(const uint8_t* dst, size_t size) {
  unsigned int idx = 0;
  ssize_t n;
  int retries = 0;

  while (idx < size) {
    if (!isConnected()) {
      return;
    }
  READ:
    if ((n = recv(this->apSock, (char*)&dst[idx], size - idx, 0)) <= 0) {
      switch (getErrno()) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            CSPOT_LOG(error, "Connection lost, will need to reconnect...");
            close();
            return;
          }
          goto READ;
        case EINTR:
          goto READ;
        default:
          if (retries++ > 4) {
            close();
            return;
          }
          goto READ;
      }
    }
    idx += n;
  }
}

size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  unsigned int idx = 0;
  ssize_t n;

  int retries = 0;

  while (idx < data.size()) {
    if (!isConnected()) {
      return idx;
    }
  WRITE:
    if ((n = send(this->apSock, (char*)&data[idx],
                  data.size() - idx < 64 ? data.size() - idx : 64, 0)) <= 0) {
      switch (getErrno()) {
        case EAGAIN:
        case ETIMEDOUT:
          if (timeoutHandler()) {
            close();
            return idx;
          }
          goto WRITE;
        case EINTR:
          goto WRITE;
        default:
          if (retries++ > 4) {
            close();
            return idx;
          }
          goto WRITE;
      }
    }
    idx += n;
  }

  return data.size();
}

void PlainConnection::close() {
  if (this->apSock < 0)
    return;

  CSPOT_LOG(info, "Closing socket...");
  shutdown(this->apSock, SHUT_RDWR);
#ifdef _WIN32
  closesocket(this->apSock);
#else
  ::close(this->apSock);
#endif
  this->apSock = -1;
}
