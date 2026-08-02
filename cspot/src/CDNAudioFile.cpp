#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <atomic>
#include <algorithm>
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"
#include "Crypto.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

namespace {
std::atomic<CDNAudioFile*> g_activeCdnAudioFile{nullptr};
std::mutex g_activeCdnAudioFileMutex;

size_t readHttpStreamYielding(std::iostream& stream, uint8_t* dst,
                              size_t bytes, std::atomic<bool>* abortFlag = nullptr) {
  size_t bytesReadTotal = 0;
  while (bytesReadTotal < bytes) {
    if (abortFlag && abortFlag->load()) {
      break;
    }
    auto chunkSize = std::min<size_t>(16384, bytes - bytesReadTotal);
    stream.read((char*)dst + bytesReadTotal, chunkSize);
    if (abortFlag && abortFlag->load()) {
      break;
    }
    auto bytesRead = stream.gcount();
    if (bytesRead <= 0) {
      break;
    }
    bytesReadTotal += bytesRead;
    if ((bytesReadTotal & 0x1FFFF) == 0) {
      BELL_SLEEP_MS(1);
    }
  }
  if (bytesReadTotal != bytes) {
    CSPOT_LOG(error, "HTTP stream short read: got=%u expected=%u",
              (unsigned)bytesReadTotal, (unsigned)bytes);
  }
  return bytesReadTotal;
}

bool responseOk(const std::unique_ptr<bell::HTTPClient::Response>& response) {
  return response && response->status() >= 200 && response->status() < 300;
}

uint32_t cdnNowMs() {
#ifdef ESP_PLATFORM
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
  return 0;
#endif
}
}  // namespace

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

CDNAudioFile::~CDNAudioFile() {
  // Remove this object from the cross-task interrupt registry before its
  // connections begin tearing down.
  unregisterActiveStream();
  abort();
  cancelPrefetch(true);
}

void CDNAudioFile::registerActiveStream() {
  std::lock_guard<std::mutex> lock(g_activeCdnAudioFileMutex);
  g_activeCdnAudioFile.store(this);
}

void CDNAudioFile::unregisterActiveStream() {
  std::lock_guard<std::mutex> lock(g_activeCdnAudioFileMutex);
  CDNAudioFile* expected = this;
  g_activeCdnAudioFile.compare_exchange_strong(expected, nullptr);
}

void CDNAudioFile::abort() {
  abortRequested = true;
  if (httpConnection) {
    httpConnection->abortNow();
  }
#ifdef ESP_PLATFORM
  {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    if (prefetchConnection) {
      prefetchConnection->abortNow();
    }
  }
#endif
}

void CDNAudioFile::abortActiveStream() {
  std::lock_guard<std::mutex> lock(g_activeCdnAudioFileMutex);
  CDNAudioFile* active = g_activeCdnAudioFile.load();
  if (active) {
    active->abort();
  }
}

void CDNAudioFile::interruptForSeek() {
  // Break only the current HTTP reads. Unlike abort(), this keeps the decoded
  // file object reusable so TrackPlayer can reopen Vorbis at the new offset.
  if (httpConnection) {
    httpConnection->abortNow();
  }
  cancelPrefetch(false);
}

void CDNAudioFile::interruptActiveStreamForSeek() {
  std::lock_guard<std::mutex> lock(g_activeCdnAudioFileMutex);
  CDNAudioFile* active = g_activeCdnAudioFile.load();
  if (active) {
    active->interruptForSeek();
  }
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  cancelPrefetch(false);
  this->enableRequestMargin = true;
  this->startupRangeComplete = false;
  this->position = newPos;
}

void CDNAudioFile::openStream() {
  abortRequested = false;
  registerActiveStream();
  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrl.c_str());

  // Open connection, read first 128 bytes
  this->httpConnection = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)});
  if (!responseOk(this->httpConnection)) {
    CSPOT_LOG(error, "CDN header request failed: status=%d",
              this->httpConnection ? this->httpConnection->status() : -1);
    this->totalFileSize = 0;
    return;
  }

  if (abortRequested ||
      readHttpStreamYielding(this->httpConnection->stream(), header.data(),
                             OPUS_HEADER_SIZE, &abortRequested) != OPUS_HEADER_SIZE) {
    this->totalFileSize = 0;
    return;
  }
  CSPOT_LOG(info, "Header bytes received");
  this->totalFileSize =
      this->httpConnection->totalLength() - SPOTIFY_OPUS_HEADER;
  if (this->totalFileSize == 0 ||
      this->httpConnection->totalLength() <= SPOTIFY_OPUS_HEADER) {
    CSPOT_LOG(error, "Invalid CDN total length: %u",
              (unsigned)this->httpConnection->totalLength());
    this->totalFileSize = 0;
    return;
  }

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer.resize(this->totalFileSize - footerStartLocation +
                      SPOTIFY_OPUS_HEADER);
  this->httpConnection = bell::HTTPClient::get(
      cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())});
  if (!responseOk(this->httpConnection)) {
    CSPOT_LOG(error, "CDN footer request failed: status=%d",
              this->httpConnection ? this->httpConnection->status() : -1);
    this->totalFileSize = 0;
    return;
  }

  if (abortRequested ||
      readHttpStreamYielding(this->httpConnection->stream(), footer.data(),
                             this->footer.size(), &abortRequested) != this->footer.size()) {
    this->totalFileSize = 0;
    return;
  }

  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  CSPOT_LOG(info, "Header and footer bytes received");
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;
  this->startupRangeComplete = false;
  cancelPrefetch(true);
}

size_t CDNAudioFile::fetchRangeSync(size_t requestPosition, size_t requestSize,
                                    uint8_t* buffer, const char* reason) {
  if (abortRequested || !buffer || requestSize == 0) {
    return 0;
  }

  this->httpConnection = bell::HTTPClient::get(
      cdnUrl, {bell::HTTPClient::RangeHeader::range(
                  requestPosition, requestPosition + requestSize - 1)});
  if (!responseOk(this->httpConnection)) {
    CSPOT_LOG(error, "CDN %s range request failed: status=%d",
              reason ? reason : "audio",
              this->httpConnection ? this->httpConnection->status() : -1);
    return 0;
  }

  size_t capacity = this->httpConnection->contentLength();
  if (capacity == 0 || capacity > HTTP_BUFFER_SIZE) {
    capacity = HTTP_BUFFER_SIZE;
  }

  uint32_t readStartMs = cdnNowMs();
  if (readHttpStreamYielding(this->httpConnection->stream(), buffer, capacity,
                             &abortRequested) != capacity) {
    return 0;
  }
  uint32_t readDtMs = cdnNowMs() - readStartMs;
  if (readDtMs >= 250) {
    CSPOT_LOG(error, "Slow CDN %s range read bytes=%u dt=%u pos=%u",
              reason ? reason : "audio", (unsigned)capacity,
              (unsigned)readDtMs, (unsigned)requestPosition);
  }
  return capacity;
}

void CDNAudioFile::startNextPrefetch() {
#if CSPOT_ENABLE_PREFETCH && defined(ESP_PLATFORM)
  if (abortRequested || this->totalFileSize == 0 || this->lastRequestCapacity == 0) {
    return;
  }

  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;
  size_t nextPosition = this->lastRequestPosition + this->lastRequestCapacity;
  if (nextPosition >= actualFileSize ||
      nextPosition >= actualFileSize - this->footer.size()) {
    return;
  }

#ifdef ESP_PLATFORM
  {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    if (prefetchInFlight) return;
    if (prefetchReady && prefetchRequestPosition == nextPosition) return;
    prefetchRequestPosition = nextPosition;
    prefetchCapacity = 0;
    prefetchReady = false;
    prefetchFailed = false;
    prefetchEncrypted = false;
    prefetchWasAdopted = false;
    prefetchInFlight = true;
  }

  CSPOT_LOG(info, "Prefetch started pos=%u bytes=%u",
            (unsigned)nextPosition, (unsigned)HTTP_BUFFER_SIZE);

  BaseType_t ok = xTaskCreatePinnedToCore(
      &CDNAudioFile::prefetchTaskEntry, "cdn_prefetch", 24 * 1024, this, 9,
      &prefetchTask, 0);
  if (ok != pdPASS) {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    prefetchInFlight = false;
    prefetchFailed = true;
    prefetchTask = nullptr;
    CSPOT_LOG(error, "Failed to start CDN prefetch task");
  }
#endif
#endif
}

bool CDNAudioFile::adoptPrefetch(size_t offsetPosition) {
#if CSPOT_ENABLE_PREFETCH && defined(ESP_PLATFORM)
  uint32_t waitStart = cdnNowMs();
  while (!abortRequested) {
    bool ready = false;
    bool failed = false;
    bool inFlight = false;
    size_t requestPosition = 0;
    size_t capacity = 0;
    {
      std::lock_guard<std::mutex> lock(prefetchMutex);
      ready = prefetchReady;
      failed = prefetchFailed;
      inFlight = prefetchInFlight;
      requestPosition = prefetchRequestPosition;
      capacity = prefetchCapacity;
    }

    if (ready && offsetPosition >= requestPosition &&
        offsetPosition < requestPosition + capacity) {
      CSPOT_LOG(info, "Prefetch hit pos=%u request=%u bytes=%u",
                (unsigned)offsetPosition, (unsigned)requestPosition,
                (unsigned)capacity);
      {
        std::lock_guard<std::mutex> lock(prefetchMutex);
        if (!prefetchReady || offsetPosition < prefetchRequestPosition ||
            offsetPosition >= prefetchRequestPosition + prefetchCapacity) {
          CSPOT_LOG(info, "Prefetch miss pos=%u request=%u bytes=%u",
                    (unsigned)offsetPosition, (unsigned)requestPosition,
                    (unsigned)capacity);
          return false;
        }
        if (prefetchEncrypted) {
          decrypt(prefetchBuffer.data(), prefetchCapacity, prefetchRequestPosition);
          prefetchEncrypted = false;
        }
        std::swap(httpBuffer, prefetchBuffer);
        lastRequestPosition = prefetchRequestPosition;
        lastRequestCapacity = prefetchCapacity;
        prefetchReady = false;
        prefetchCapacity = 0;
        startupRangeComplete = true;
        prefetchWasAdopted = true;
      }
      CSPOT_LOG(info, "Prefetch adopted pos=%u bytes=%u wait=%u",
                (unsigned)lastRequestPosition, (unsigned)lastRequestCapacity,
                (unsigned)(cdnNowMs() - waitStart));
      startNextPrefetch();
      return true;
    }

    if (!inFlight || failed ||
        (cdnNowMs() - waitStart) >= (uint32_t)HTTP_PREFETCH_WAIT_MS) {
      if (inFlight) {
        CSPOT_LOG(error, "CDN prefetch wait timed out pos=%u want=%u",
                  (unsigned)requestPosition, (unsigned)offsetPosition);
        cancelPrefetch(false);
      }
      CSPOT_LOG(info, "Prefetch miss pos=%u request=%u inFlight=%d failed=%d",
                (unsigned)offsetPosition, (unsigned)requestPosition,
                (int)inFlight, (int)failed);
      return false;
    }
    BELL_SLEEP_MS(5);
  }
  return false;
#else
  (void)offsetPosition;
  return false;
#endif
}

void CDNAudioFile::cancelPrefetch(bool waitForTask) {
#ifdef ESP_PLATFORM
  TaskHandle_t task = nullptr;
  {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    task = prefetchTask;
    prefetchReady = false;
    prefetchFailed = false;
    prefetchEncrypted = false;
    if (prefetchConnection) {
      prefetchConnection->abortNow();
    }
  }

  if (waitForTask && task && task != xTaskGetCurrentTaskHandle()) {
    uint32_t waitStart = cdnNowMs();
    while (prefetchTask && (cdnNowMs() - waitStart) < 6000) {
      BELL_SLEEP_MS(10);
    }
    if (prefetchTask) {
      CSPOT_LOG(error, "CDN prefetch task did not stop within cleanup window");
    }
  }
#else
  (void)waitForTask;
#endif
}

#ifdef ESP_PLATFORM
void CDNAudioFile::prefetchTaskEntry(void* arg) {
  static_cast<CDNAudioFile*>(arg)->runPrefetchTask();
  vTaskDelete(nullptr);
}

void CDNAudioFile::runPrefetchTask() {
  size_t requestPosition = 0;
  {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    requestPosition = prefetchRequestPosition;
  }

  if (!abortRequested) {
    auto response = bell::HTTPClient::get(
        cdnUrl, {bell::HTTPClient::RangeHeader::range(
                    requestPosition, requestPosition + HTTP_BUFFER_SIZE - 1)});
    {
      std::lock_guard<std::mutex> lock(prefetchMutex);
      prefetchConnection = std::move(response);
    }

    bool ok = false;
    size_t capacity = 0;
    {
      std::lock_guard<std::mutex> lock(prefetchMutex);
      ok = responseOk(prefetchConnection);
      if (ok) {
        capacity = prefetchConnection->contentLength();
        if (capacity == 0 || capacity > prefetchBuffer.size()) {
          capacity = prefetchBuffer.size();
        }
      }
    }

    if (ok && capacity > 0) {
      uint32_t readStartMs = cdnNowMs();
      bool readOk = false;
      bell::HTTPClient::Response* responsePtr = nullptr;
      {
        std::lock_guard<std::mutex> lock(prefetchMutex);
        responsePtr = prefetchConnection.get();
      }
      readOk = responsePtr &&
               readHttpStreamYielding(responsePtr->stream(), prefetchBuffer.data(),
                                      capacity, &abortRequested) == capacity;
      if (readOk && !abortRequested) {
        uint32_t readDtMs = cdnNowMs() - readStartMs;
        if (readDtMs >= 250) {
          CSPOT_LOG(error, "Slow CDN prefetch range read bytes=%u dt=%u pos=%u",
                    (unsigned)capacity, (unsigned)readDtMs,
                    (unsigned)requestPosition);
        }
        std::lock_guard<std::mutex> lock(prefetchMutex);
        prefetchCapacity = capacity;
        prefetchReady = true;
        prefetchEncrypted = true;
        prefetchFailed = false;
        CSPOT_LOG(info, "Prefetch completed pos=%u bytes=%u dt=%u",
                  (unsigned)requestPosition, (unsigned)capacity,
                  (unsigned)readDtMs);
      } else {
        std::lock_guard<std::mutex> lock(prefetchMutex);
        prefetchFailed = true;
      }
    } else {
      std::lock_guard<std::mutex> lock(prefetchMutex);
      prefetchFailed = true;
    }
  }

  bool adoptedWhileFinishing = false;
  {
    std::lock_guard<std::mutex> lock(prefetchMutex);
    prefetchConnection.reset();
    prefetchInFlight = false;
    prefetchTask = nullptr;
    adoptedWhileFinishing = prefetchWasAdopted;
  }

  // The decoder may adopt a completed buffer before this task has cleared its
  // in-flight flag. In that race, adoptPrefetch() cannot launch the following
  // range, so continue the pipeline here once cleanup is complete.
  if (adoptedWhileFinishing && !abortRequested) {
    startNextPrefetch();
  }
}
#endif

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  if (abortRequested) {
    return 0;
  }
  registerActiveStream();
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (this->totalFileSize == 0) {
    return 0;
  }

  if (position >= this->totalFileSize) {
    return 0;
  }

  if (position + bytes > this->totalFileSize) {
    bytes = this->totalFileSize - position;
  }

  // // Opus tries to read header, use prefetched data
  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  // // Opus tries to read footer, use prefetched data
  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      // Tries to read outside of bounds, truncate
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  // Data not in the headers. Make sense of whats going on.
  // Position in bounds :)
  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

#if CSPOT_ENABLE_PREFETCH && defined(ESP_PLATFORM)
    // Recover a missed launch race while there is still half a range available
    // for decoding. startNextPrefetch() is idempotent while work is active.
    if (offsetPosition + toRead >=
        lastRequestPosition + (lastRequestCapacity / 2)) {
      startNextPrefetch();
    }
#endif

#if CSPOT_ENABLE_PREFETCH && defined(ESP_PLATFORM)
    if (prefetchWasAdopted) {
      CSPOT_LOG(info, "Prefetch bytes served pos=%u bytes=%u",
                (unsigned)offsetPosition, (unsigned)toRead);
    }
#endif

    return toRead;
  } else {
    size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                        ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }

    size_t requestSize = this->startupRangeComplete ? HTTP_BUFFER_SIZE
                                                    : HTTP_START_BUFFER_SIZE;
    if (adoptPrefetch(offsetPosition)) {
      return readBytes(dst, bytes);
    }

    uint32_t waitStartMs = cdnNowMs();
    this->lastRequestCapacity =
        fetchRangeSync(requestPosition, requestSize, this->httpBuffer.data(), "audio");
    uint32_t waitDtMs = cdnNowMs() - waitStartMs;
    if (waitDtMs >= 100) {
      CSPOT_LOG(info, "CDN wait audio pos=%u req=%u dt=%u",
                (unsigned)requestPosition, (unsigned)requestSize,
                (unsigned)waitDtMs);
    }
    if (this->lastRequestCapacity == 0) {
      return 0;
    }
    this->lastRequestPosition = requestPosition;
    this->startupRangeComplete = true;
    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,

                  this->lastRequestPosition);
#if CSPOT_ENABLE_PREFETCH && defined(ESP_PLATFORM)
    prefetchWasAdopted = false;
#endif
    startNextPrefetch();

    return readBytes(dst, bytes);
  }

  return bytes;
}

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}
