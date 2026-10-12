#include "MercurySession.h"

#include <string.h>     // for memcpy
#include <memory>       // for shared_ptr
#include <mutex>        // for scoped_lock
#include <stdexcept>    // for runtime_error
#include <type_traits>  // for remove_extent_t, __underlying_type_impl<>:...
#include <utility>      // for pair
#ifndef _WIN32
#include <arpa/inet.h>  // for htons, ntohs, htonl, ntohl
#endif
#include "BellLogger.h"         // for AbstractLogger
#include "BellTask.h"           // for Task
#include "BellUtils.h"          // for BELL_SLEEP_MS
#include "Logger.h"             // for CSPOT_LOG
#include "NanoPBHelper.h"       // for pbPutString, pbDecode, pbEncode
#include "PlainConnection.h"    // for PlainConnection
#include "ShannonConnection.h"  // for ShannonConnection
#include "TimeProvider.h"       // for TimeProvider
#include "Utils.h"              // for extract, pack, hton64

using namespace cspot;

MercurySession::MercurySession(std::shared_ptr<TimeProvider> timeProvider)
    : bell::Task("mercury_dispatcher", 8 * 1024, 7, 1) {
  this->timeProvider = timeProvider;
}

MercurySession::~MercurySession() {
  std::scoped_lock lock(this->isRunningMutex);
}

void MercurySession::runTask() {
  isRunning = true;
  std::scoped_lock lock(this->isRunningMutex);

  this->executeEstabilishedCallback = true;
  while (isRunning) {
    auto activeConnection = connectionSnapshot();
    auto activeShannon = shannonSnapshot();
    if (!activeShannon || !activeConnection || !activeConnection->isConnected()) {
      failAllPending();
      reconnect();
      continue;
    }

    cspot::Packet packet = {};
    try {
      packet = activeShannon->recvPacket();
      activeConnection = connectionSnapshot();
      if (!activeConnection || !activeConnection->isConnected() ||
          packet.command == 0) {
        failAllPending();
        reconnect();
        continue;
      }

      CSPOT_LOG(debug, "Received packet, command: %d", packet.command);

      if (static_cast<RequestType>(packet.command) == RequestType::PING) {
        timeProvider->syncWithPingPacket(packet.data);

        this->lastPingTimestamp = timeProvider->getSyncedTimestamp();
        std::lock_guard<std::mutex> sendLock(requestSendMutex);
        activeShannon->sendPacket(0x49, packet.data);
      } else {
        this->packetQueue.push(packet);
      }
    } catch (const std::runtime_error& e) {
      CSPOT_LOG(error, "Error while receiving packet: %s", e.what());
      failAllPending();

      if (!isRunning)
        return;

      reconnect();
      continue;
    }
  }
}

void MercurySession::reconnect() {
  isReconnecting = true;

  // A network outage may last for hours. Retrying recursively grows this
  // task's stack once per attempt and eventually kills Spotify Connect.
  while (isRunning) {
    try {
      // Reconnect is an exclusive transport transaction. Requests wait rather
      // than racing a replaced Shannon cipher/socket generation.
      std::lock_guard<std::mutex> transportTransaction(requestSendMutex);
      clearTransport();

      this->connectWithRandomAp();
      auto token = this->authenticate(this->authBlob);
      auto activeConnection = connectionSnapshot();
      if (activeConnection && activeConnection->isConnected() &&
          shannonSnapshot() && !token.empty()) {
        CSPOT_LOG(info, "Reconnection successful");
        BELL_SLEEP_MS(100);
        lastPingTimestamp = timeProvider->getSyncedTimestamp();
        isReconnecting = false;
        this->executeEstabilishedCallback = true;
        return;
      }
      CSPOT_LOG(error, "Spotify reconnect did not establish a session");
    } catch (...) {
      CSPOT_LOG(error, "Cannot reconnect, will retry in 5s");
    }

    for (int waitedMs = 0; isRunning && waitedMs < 5000; waitedMs += 100) {
      BELL_SLEEP_MS(100);
    }
  }
  isReconnecting = false;
}

void MercurySession::setConnectedHandler(
    ConnectionEstabilishedCallback callback) {
  std::lock_guard<std::mutex> callbackLock(connectionCallbackMutex);
  this->connectionReadyCallback = std::move(callback);
}

bool MercurySession::triggerTimeout() {
  if (!isRunning)
    return true;
  auto currentTimestamp = timeProvider->getSyncedTimestamp();

  if (currentTimestamp - this->lastPingTimestamp > PING_TIMEOUT_MS) {
    CSPOT_LOG(debug, "Reconnection required, no ping received");
    return true;
  }

  return false;
}

void MercurySession::unregister(uint64_t sequenceId) {
  std::lock_guard<std::mutex> callbackLock(callbackMutex);
  auto callback = this->callbacks.find(sequenceId);

  if (callback != this->callbacks.end()) {
    this->callbacks.erase(callback);
  }
}

void MercurySession::unregisterAudioKey(uint32_t sequenceId) {
  std::lock_guard<std::mutex> callbackLock(callbackMutex);
  auto callback = this->audioKeyCallbacks.find(sequenceId);

  if (callback != this->audioKeyCallbacks.end()) {
    this->audioKeyCallbacks.erase(callback);
  }
}

void MercurySession::disconnect() {
  CSPOT_LOG(info, "Disconnecting mercury session");
  this->isRunning = false;
  // Reconnect deliberately clears conn before opening another AP. A source
  // handoff can request disconnect in that window, so never dereference the
  // transiently empty connection. Keep a snapshot alive through close().
  {
    std::lock_guard<std::mutex> transportTransaction(requestSendMutex);
    auto activeConnection = connectionSnapshot();
    if (activeConnection) {
      activeConnection->close();
    }
  }
  std::scoped_lock lock(this->isRunningMutex);
}

std::string MercurySession::getCountryCode() {
  return this->countryCode;
}

void MercurySession::handlePacket() {
  Packet packet = {};

  this->packetQueue.wtpop(packet, 200);

  if (executeEstabilishedCallback.exchange(false)) {
    ConnectionEstabilishedCallback callback;
    {
      std::lock_guard<std::mutex> callbackLock(connectionCallbackMutex);
      callback = connectionReadyCallback;
    }
    if (callback) callback();
  }

  switch (static_cast<RequestType>(packet.command)) {
    case RequestType::COUNTRY_CODE_RESPONSE: {
      if (packet.data.size() < 2) {
        CSPOT_LOG(error, "Rejected truncated country-code packet");
        break;
      }
      this->countryCode = std::string();
      this->countryCode.resize(2);
      memcpy(this->countryCode.data(), packet.data.data(), 2);
      CSPOT_LOG(debug, "Received country code %s", this->countryCode.c_str());
      break;
    }
    case RequestType::AUDIO_KEY_FAILURE_RESPONSE:
    case RequestType::AUDIO_KEY_SUCCESS_RESPONSE: {
      if (packet.data.size() < sizeof(uint32_t)) {
        CSPOT_LOG(error, "Rejected truncated audio-key packet");
        break;
      }
      // this->lastRequestTimestamp = -1;

      // First four bytes mark the sequence id
      auto seqId = ntohl(extract<uint32_t>(packet.data, 0));

      AudioKeyCallback callback;
      {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        auto found = audioKeyCallbacks.find(seqId);
        if (found != audioKeyCallbacks.end()) {
          callback = std::move(found->second);
          audioKeyCallbacks.erase(found);
        }
      }
      if (callback) {
        const bool success = static_cast<RequestType>(packet.command) ==
                             RequestType::AUDIO_KEY_SUCCESS_RESPONSE;
        callback(success, packet.data);
      }

      break;
    }
    case RequestType::SEND:
    case RequestType::SUB:
    case RequestType::UNSUB: {
      CSPOT_LOG(debug, "Received mercury packet");

      auto response = this->decodeResponse(packet.data);
      ResponseCallback callback;
      {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        auto found = callbacks.find(response.sequenceId);
        if (found != callbacks.end()) {
          callback = std::move(found->second);
          callbacks.erase(found);
        }
      }
      if (callback) callback(response);
      break;
    }
    case RequestType::SUBRES: {
      auto response = decodeResponse(packet.data);

      auto uri = std::string(response.mercuryHeader.uri);
      ResponseCallback subscription;
      {
        std::lock_guard<std::mutex> callbackLock(callbackMutex);
        auto found = subscriptions.find(uri);
        if (found != subscriptions.end()) subscription = found->second;
      }
      if (subscription) subscription(response);
      break;
    }
    default:
      break;
  }
}

void MercurySession::failAllPending() {
  Response response = {};
  response.fail = true;

  std::unordered_map<uint64_t, ResponseCallback> pendingCallbacks;
  std::unordered_map<std::string, ResponseCallback> pendingSubscriptions;
  std::unordered_map<uint32_t, AudioKeyCallback> pendingAudioKeys;
  {
    std::lock_guard<std::mutex> callbackLock(callbackMutex);
    pendingCallbacks.swap(callbacks);
    pendingSubscriptions.swap(subscriptions);
    pendingAudioKeys.swap(audioKeyCallbacks);
  }

  for (auto& it : pendingCallbacks) {
    it.second(response);
  }
  for (auto& it : pendingSubscriptions) {
    it.second(response);
  }
  const std::vector<uint8_t> noAudioKey;
  for (auto& it : pendingAudioKeys) {
    it.second(false, noAudioKey);
  }
}

MercurySession::Response MercurySession::decodeResponse(
    const std::vector<uint8_t>& data) {
  Response response = {};
  response.parts = {};
  response.fail = true;

  constexpr size_t mercuryPrefixSize = 15;
  if (data.size() < mercuryPrefixSize) {
    CSPOT_LOG(error, "Rejected truncated mercury response size=%u",
              (unsigned)data.size());
    return response;
  }

  auto sequenceLength = ntohs(extract<uint16_t>(data, 0));
  if (sequenceLength != sizeof(uint64_t)) {
    CSPOT_LOG(error, "Rejected mercury sequence length=%u",
              (unsigned)sequenceLength);
    return response;
  }
  response.sequenceId = hton64(extract<uint64_t>(data, 2));

  auto partsNumber = ntohs(extract<uint16_t>(data, 11));

  auto headerSize = ntohs(extract<uint16_t>(data, 13));
  if ((size_t)headerSize > data.size() - mercuryPrefixSize) {
    CSPOT_LOG(error, "Rejected mercury header size=%u packet=%u",
              (unsigned)headerSize, (unsigned)data.size());
    return response;
  }
  auto headerBytes =
      std::vector<uint8_t>(data.begin() + 15, data.begin() + 15 + headerSize);

  auto pos = 15 + headerSize;
  while (pos < data.size()) {
    if (data.size() - pos < sizeof(uint16_t)) {
      CSPOT_LOG(error, "Rejected truncated mercury part header");
      return response;
    }
    auto partSize = ntohs(extract<uint16_t>(data, pos));
    pos += sizeof(uint16_t);
    if ((size_t)partSize > data.size() - pos) {
      CSPOT_LOG(error, "Rejected mercury part size=%u remaining=%u",
                (unsigned)partSize, (unsigned)(data.size() - pos));
      return response;
    }

    response.parts.push_back(std::vector<uint8_t>(
        data.begin() + pos, data.begin() + pos + partSize));
    pos += partSize;
  }

  if (partsNumber != response.parts.size() + 1) {
    CSPOT_LOG(error, "Rejected mercury part count declared=%u actual=%u",
              (unsigned)partsNumber, (unsigned)(response.parts.size() + 1));
    return response;
  }

  pbDecode(response.mercuryHeader, Header_fields, headerBytes);
  response.fail = false;

  return response;
}

uint64_t MercurySession::executeSubscription(RequestType method,
                                             const std::string& uri,
                                             ResponseCallback callback,
                                             ResponseCallback subscription,
                                             DataParts& payload) {
  std::lock_guard<std::mutex> requestLock(requestSendMutex);
  CSPOT_LOG(debug, "Executing Mercury Request, type %s",
            RequestTypeMap[method].c_str());

  // Encode header
  pbPutString(uri, tempMercuryHeader.uri);
  pbPutString(RequestTypeMap[method], tempMercuryHeader.method);

  tempMercuryHeader.has_method = true;
  tempMercuryHeader.has_uri = true;

  // GET and SEND are actually the same. Therefore the override
  // The difference between them is only in header's method
  if (method == RequestType::GET) {
    method = RequestType::SEND;
  }

  auto headerBytes = pbEncode(Header_fields, &tempMercuryHeader);
  const uint64_t requestSequence = sequenceId++;
  {
    std::lock_guard<std::mutex> callbackLock(callbackMutex);
    if (method == RequestType::SUB && subscription) {
      subscriptions[uri] = std::move(subscription);
    }
    if (callback) callbacks[requestSequence] = std::move(callback);
  }

  // Structure: [Sequence size] [SequenceId] [0x1] [Payloads number]
  // [Header size] [Header] [Payloads (size + data)]

  // Pack sequenceId
  auto sequenceIdBytes = pack<uint64_t>(hton64(requestSequence));
  auto sequenceSizeBytes = pack<uint16_t>(htons(sequenceIdBytes.size()));

  sequenceIdBytes.insert(sequenceIdBytes.begin(), sequenceSizeBytes.begin(),
                         sequenceSizeBytes.end());
  sequenceIdBytes.push_back(0x01);

  auto payloadNum = pack<uint16_t>(htons(payload.size() + 1));
  sequenceIdBytes.insert(sequenceIdBytes.end(), payloadNum.begin(),
                         payloadNum.end());

  auto headerSizePayload = pack<uint16_t>(htons(headerBytes.size()));
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                         headerSizePayload.end());
  sequenceIdBytes.insert(sequenceIdBytes.end(), headerBytes.begin(),
                         headerBytes.end());

  // Encode all the payload parts
  for (int x = 0; x < payload.size(); x++) {
    headerSizePayload = pack<uint16_t>(htons(payload[x].size()));
    sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                           headerSizePayload.end());
    sequenceIdBytes.insert(sequenceIdBytes.end(), payload[x].begin(),
                           payload[x].end());
  }

  try {
    auto activeShannon = shannonSnapshot();
    if (!activeShannon) throw std::runtime_error("Spotify transport unavailable");
    activeShannon->sendPacket(
        static_cast<std::underlying_type<RequestType>::type>(method),
        sequenceIdBytes);
  } catch (...) {
    unregister(requestSequence);
  }

  return requestSequence;
}

uint32_t MercurySession::requestAudioKey(const std::vector<uint8_t>& trackId,
                                         const std::vector<uint8_t>& fileId,
                                         AudioKeyCallback audioCallback) {
  std::lock_guard<std::mutex> requestLock(requestSendMutex);
  auto buffer = fileId;
  const uint32_t requestSequence = audioKeySequence++;

  // Store callback
  {
    std::lock_guard<std::mutex> callbackLock(callbackMutex);
    if (audioCallback) audioKeyCallbacks[requestSequence] = std::move(audioCallback);
  }

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  buffer.insert(buffer.end(), trackId.begin(), trackId.end());
  auto audioKeySequenceBuffer = pack<uint32_t>(htonl(requestSequence));
  buffer.insert(buffer.end(), audioKeySequenceBuffer.begin(),
                audioKeySequenceBuffer.end());
  auto suffix = std::vector<uint8_t>({0x00, 0x00});
  buffer.insert(buffer.end(), suffix.begin(), suffix.end());

  // Used for broken connection detection
  // this->lastRequestTimestamp = timeProvider->getSyncedTimestamp();
  try {
    auto activeShannon = shannonSnapshot();
    if (!activeShannon) throw std::runtime_error("Spotify transport unavailable");
    activeShannon->sendPacket(
        static_cast<uint8_t>(RequestType::AUDIO_KEY_REQUEST_COMMAND), buffer);
  } catch (...) {
    unregisterAudioKey(requestSequence);
  }
  return requestSequence;
}
