#include "Session.h"

#include <algorithm>    // for find
#include <limits.h>     // for CHAR_BIT
#include <cstdint>      // for uint8_t
#include <functional>   // for __base
#include <memory>       // for shared_ptr, unique_ptr, make_unique
#include <random>       // for default_random_engine, independent_bi...
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "ApResolve.h"          // for ApResolve, cspot
#include "AuthChallenges.h"     // for AuthChallenges
#include "BellLogger.h"         // for AbstractLogger
#include "Logger.h"             // for CSPOT_LOG
#include "LoginBlob.h"          // for LoginBlob
#include "Packet.h"             // for Packet
#include "PlainConnection.h"    // for PlainConnection, timeoutCallback
#include "ShannonConnection.h"  // for ShannonConnection

#include "NanoPBHelper.h"  // for pbPutString, pbEncode, pbDecode
#include "pb_decode.h"
#include "protobuf/authentication.pb.h"

using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t>;

using namespace cspot;

Session::Session() {
  this->challenges = std::make_unique<cspot::AuthChallenges>();
}

Session::~Session() {}

void Session::connect(std::unique_ptr<cspot::PlainConnection> connection) {
  std::lock_guard<std::recursive_mutex> transportLock(transportMutex);
  this->shanConn = nullptr;
  if (!connection || !connection->isConnected()) {
    CSPOT_LOG(error, "Spotify AP connection unavailable");
    return;
  }

  this->conn = std::move(connection);
  conn->timeoutHandler = [this]() {
    return this->triggerTimeout();
  };
  auto helloPacket = this->conn->sendPrefixPacket(
      {0x00, 0x04}, this->challenges->prepareClientHello());
  if (helloPacket.empty() || !this->conn->isConnected()) {
    CSPOT_LOG(error, "Failed to send Spotify client hello");
    this->conn->close();
    return;
  }

  auto apResponse = this->conn->recvPacket();
  if (apResponse.empty() || !this->conn->isConnected()) {
    CSPOT_LOG(error, "Failed to receive Spotify AP hello");
    this->conn->close();
    return;
  }
  CSPOT_LOG(info, "Received APHello response");

  auto solvedHello = this->challenges->solveApHello(helloPacket, apResponse);

  if (conn->sendPrefixPacket({}, solvedHello).empty() ||
      !this->conn->isConnected()) {
    CSPOT_LOG(error, "Failed to send Spotify AP challenge response");
    this->conn->close();
    return;
  }
  CSPOT_LOG(debug, "Received shannon keys");

  // Generates the public and priv key
  this->shanConn = std::make_shared<ShannonConnection>();

  // Init shanno-encrypted connection
  this->shanConn->wrapConnection(this->conn, challenges->shanSendKey,
                                 challenges->shanRecvKey);
}

void Session::connectWithRandomAp() {
  static std::string lastSuccessfulAp;
  auto apResolver = std::make_unique<ApResolve>("");

  auto apAddresses = apResolver->fetchApAddresses();
  if (apAddresses.empty()) {
    CSPOT_LOG(error, "Spotify AP resolve failed; reconnect will retry");
    return;
  }

  // A previously healthy endpoint is worth trying first, but every address
  // returned by Spotify remains available as an immediate fallback. The old
  // behavior retried only ap_list[0], so one blocked port looked like a total
  // Spotify outage even when the resolver supplied working alternatives.
  auto cached = std::find(apAddresses.begin(), apAddresses.end(),
                          lastSuccessfulAp);
  if (cached != apAddresses.end() && cached != apAddresses.begin()) {
    std::rotate(apAddresses.begin(), cached, cached + 1);
  }

  CSPOT_LOG(info, "Spotify resolver supplied %u AP endpoints",
            static_cast<unsigned>(apAddresses.size()));
  for (size_t attempt = 0; attempt < apAddresses.size(); ++attempt) {
    const std::string& apAddr = apAddresses[attempt];
    auto conn = std::make_unique<cspot::PlainConnection>();
    conn->timeoutHandler = [this]() {
      return this->triggerTimeout();
    };

    CSPOT_LOG(info, "Connecting with Spotify AP <%s> endpoint %u/%u",
              apAddr.c_str(), static_cast<unsigned>(attempt + 1),
              static_cast<unsigned>(apAddresses.size()));
    conn->connect(apAddr);
    if (!conn->isConnected()) {
      continue;
    }

    this->connect(std::move(conn));
    if (shannonSnapshot()) {
      lastSuccessfulAp = apAddr;
      return;
    }
  }

  CSPOT_LOG(error, "Unable to establish Spotify AP session");
}

std::vector<uint8_t> Session::authenticate(std::shared_ptr<LoginBlob> blob) {
  // save auth blob for reconnection purposes
  authBlob = blob;
  auto activeShannon = shannonSnapshot();
  if (!activeShannon || !blob) {
    CSPOT_LOG(error, "Spotify authentication skipped; no AP session");
    return {};
  }

  // prepare authentication request proto
  auto data = challenges->prepareAuthPacket(blob->authData, blob->authType,
                                            deviceId, blob->username);

  // Send login request
  activeShannon->sendPacket(LOGIN_REQUEST_COMMAND, data);

  auto packet = activeShannon->recvPacket();
  switch (packet.command) {
    case AUTH_SUCCESSFUL_COMMAND: {
      APWelcome welcome = APWelcome_init_zero;
      CSPOT_LOG(debug, "Authorization successful");
      pbDecode(welcome, APWelcome_fields, packet.data);
      return std::vector<uint8_t>(welcome.reusable_auth_credentials.bytes,
                                  welcome.reusable_auth_credentials.bytes +
                                      welcome.reusable_auth_credentials.size);
      break;
    }
    case AUTH_DECLINED_COMMAND: {
      CSPOT_LOG(error, "Authorization declined");
      break;
    }
    default:
      CSPOT_LOG(error, "Unknown auth fail code %d", packet.command);
  }

  return std::vector<uint8_t>(0);
}

void Session::close() {
  auto activeConnection = connectionSnapshot();
  if (activeConnection) activeConnection->close();
}

std::shared_ptr<cspot::PlainConnection> Session::connectionSnapshot() const {
  std::lock_guard<std::recursive_mutex> transportLock(transportMutex);
  return conn;
}

std::shared_ptr<cspot::ShannonConnection> Session::shannonSnapshot() const {
  std::lock_guard<std::recursive_mutex> transportLock(transportMutex);
  return shanConn;
}

void Session::clearTransport() {
  std::lock_guard<std::recursive_mutex> transportLock(transportMutex);
  conn.reset();
  shanConn.reset();
}
