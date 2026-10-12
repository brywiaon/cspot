#include "TrackQueue.h"
#include <pb_decode.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "AccessKeyFetcher.h"
#include "BellTask.h"
#include "CDNAudioFile.h"
#include "CheckedPBDecode.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "Utils.h"
#include "WrappedSemaphore.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "protobuf/metadata.pb.h"

using namespace cspot;

namespace {
std::atomic<uint64_t> s_nextTrackInstanceId{1};

#ifdef ESP_PLATFORM
uint32_t cspotNowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
#else
uint32_t cspotNowMs() {
  return 0;
}
#endif

#ifdef ESP_PLATFORM
void cspotQueueYield(bool forceDelay = false) {
  static uint32_t yieldCounter = 0;
  yieldCounter++;
  if (forceDelay || (yieldCounter & 0x07) == 0) {
    vTaskDelay(1);
  } else {
    taskYIELD();
  }
}
#else
void cspotQueueYield(bool forceDelay = false) {
  (void)forceDelay;
}
#endif

void clearQueuedTracksCooperative(std::deque<std::shared_ptr<QueuedTrack>>& queue) {
  while (!queue.empty()) {
    queue.pop_front();
    cspotQueueYield(queue.empty());
  }
}

std::string spotifyBodyPreview(std::string_view body) {
  auto previewSize = std::min<size_t>(body.size(), 160);
  std::string preview(body.substr(0, previewSize));
  for (auto& character : preview) {
    unsigned char value = static_cast<unsigned char>(character);
    if (!std::isprint(value)) {
      character = '.';
    }
  }
  return preview;
}

#ifndef BELL_ONLY_CJSON
bool readSpotifyCdnUrl(const nlohmann::json& jsonResult, std::string& cdnUrl) {
  if (jsonResult.contains("cdnurl")) {
    const auto& cdnUrlValue = jsonResult["cdnurl"];
    if (cdnUrlValue.is_array() && !cdnUrlValue.empty() &&
        cdnUrlValue[0].is_string()) {
      cdnUrl = cdnUrlValue[0].get<std::string>();
      return true;
    }
    if (cdnUrlValue.is_string()) {
      cdnUrl = cdnUrlValue.get<std::string>();
      return true;
    }
  }

  for (const auto* key : {"cdnurls", "urls"}) {
    if (!jsonResult.contains(key)) {
      continue;
    }
    const auto& urlsValue = jsonResult[key];
    if (urlsValue.is_array() && !urlsValue.empty() && urlsValue[0].is_string()) {
      cdnUrl = urlsValue[0].get<std::string>();
      return true;
    }
  }

  return false;
}
#endif

bool readSpotifyCdnUrlFromBinary(std::string_view body, std::string& cdnUrl) {
  constexpr std::string_view urlPrefix = "https://";
  auto start = body.find(urlPrefix);
  if (start == std::string_view::npos) {
    return false;
  }

  auto end = start;
  while (end < body.size()) {
    unsigned char value = static_cast<unsigned char>(body[end]);
    if (value <= 0x20 || value == '"' || value == '\'' || value == '<' ||
        value == '>' || value == '\\') {
      break;
    }
    end++;
  }

  if (end <= start + urlPrefix.size()) {
    return false;
  }

  cdnUrl = std::string(body.substr(start, end - start));
  return true;
}
}  // namespace

namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  if (countryList == nullptr || country == nullptr || country[0] == '\0' ||
      country[1] == '\0') {
    return false;
  }
  uint16_t countryList_length = strlen(countryList);
  for (uint16_t x = 0; x + 1 < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  if (restrictions == nullptr || count <= 0) {
    return false;
  }
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  if (!pbTrack) return;
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = pbTrack->name != nullptr ? std::string(pbTrack->name) : std::string();

  if (pbTrack->artist_count > 0) {
    // Handle artist data
    artist = pbTrack->artist[0].name != nullptr
                 ? std::string(pbTrack->artist[0].name)
                 : std::string();
  }

  if (pbTrack->has_album) {
    // Handle album data
    album = pbTrack->album.name != nullptr ? std::string(pbTrack->album.name)
                                           : std::string();

    if (pbTrack->album.has_cover_group &&
        pbTrack->album.cover_group.image_count > 0) {
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  if (!pbEpisode) return;
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = pbEpisode->name != nullptr ? std::string(pbEpisode->name)
                                    : std::string();

  if (pbEpisode->covers != nullptr && pbEpisode->covers->image_count > 0) {
    // Handle episode info
    auto imageId = pbArrayToVector(pbEpisode->covers->image[0].file_id);
    imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
  }

  number = pbEpisode->has_number ? pbEpisode->number : 0;
  discNumber = 0;
  duration = pbEpisode->duration;
}

QueuedTrack::QueuedTrack(TrackReference& ref,
                         std::shared_ptr<cspot::Context> ctx,
                         uint32_t requestedPosition)
    : instanceId(s_nextTrackInstanceId.fetch_add(1, std::memory_order_relaxed)),
      requestedPosition(requestedPosition),
      ctx(ctx) {
  this->ref = ref;

  loadedSemaphore = std::make_shared<bell::WrappedSemaphore>();
  state = State::QUEUED;
}

QueuedTrack::~QueuedTrack() {
  state = State::FAILED;
  loadedSemaphore->give();

  if (pendingMercuryRequest != 0) {
    ctx->session->unregister(pendingMercuryRequest);
  }

  if (pendingAudioKeyRequest != 0) {
    ctx->session->unregisterAudioKey(pendingAudioKeyRequest);
  }

  pb_release(Track_fields, &metadataTrack);
  pb_release(Episode_fields, &metadataEpisode);
}

std::shared_ptr<cspot::CDNAudioFile> QueuedTrack::getAudioFile() {
  if (state != State::READY) {
    return nullptr;
  }

  return std::make_shared<cspot::CDNAudioFile>(cdnUrl, audioKey);
}

void QueuedTrack::stepParseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int alternativeCount, filesCount = 0;
  bool canPlay = false;
  AudioFile* selectedFiles = nullptr;

  const char* countryCode = ctx->config.countryCode.c_str();

  if (ref.type == TrackReference::Type::TRACK) {
    if (!pbTrack) {
      failureReason = "missing track metadata";
      state = State::FAILED;
      loadedSemaphore->give();
      return;
    }
    CSPOT_LOG(info, "Track name: %s", pbTrack->name ? pbTrack->name : "");
    CSPOT_LOG(info, "Track duration: %d", pbTrack->duration);

    CSPOT_LOG(debug, "trackInfo.restriction.size() = %d",
              pbTrack->restriction_count);

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      trackInfo.loadPbTrack(pbTrack, trackId);
    }
  } else {
    if (!pbEpisode) {
      failureReason = "missing episode metadata";
      state = State::FAILED;
      loadedSemaphore->give();
      return;
    }
    // Handle episodes
    CSPOT_LOG(info, "Episode name: %s",
              pbEpisode->name ? pbEpisode->name : "");
    CSPOT_LOG(info, "Episode duration: %d", pbEpisode->duration);

    CSPOT_LOG(debug, "episodeInfo.restriction.size() = %d",
              pbEpisode->restriction_count);

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      trackInfo.loadPbEpisode(pbEpisode, trackId);
    }
  }

  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
    if (selectedFiles[x].format == ctx->config.audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
    }
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
    failureReason = "no compatible audio file";
    CSPOT_LOG(error, "File not available for playback uri=%s format=%d files=%d",
              ref.uri.c_str(), (int)ctx->config.audioFormat, filesCount);

    // no alternatives for song
    state = State::FAILED;
    loadedSemaphore->give();
    return;
  }

  // Assign track identifier
  identifier = bytesToHexString(fileId);

  state = State::KEY_REQUIRED;
}

void QueuedTrack::stepLoadAudioFile(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  // Request audio key
  std::weak_ptr<QueuedTrack> weakSelf = weak_from_this();
  this->pendingAudioKeyRequest = ctx->session->requestAudioKey(
      trackId, fileId,
      [weakSelf, &trackListMutex, updateSemaphore](
          bool success, const std::vector<uint8_t>& audioKey) {
        auto self = weakSelf.lock();
        if (!self) return;
        std::scoped_lock lock(trackListMutex);

        if (success && audioKey.size() >= 4) {
          CSPOT_LOG(info, "Got audio key");
          self->audioKey =
              std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());

          self->state = State::CDN_REQUIRED;
        } else {
          CSPOT_LOG(error, "Failed to get audio key");
          self->failureReason = "audio key request failed";
          self->state = State::FAILED;
          self->loadedSemaphore->give();
        }
        updateSemaphore->give();
      });

  state = State::PENDING_KEY;
}

void QueuedTrack::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    // Wait for access key
    return;
  }

    // Request CDN URL
    CSPOT_LOG(info, "Received access key, fetching CDN URL...");

  try {
    auto countryCode = ctx->config.countryCode.empty() ? "US" : ctx->config.countryCode;
    auto salt = static_cast<unsigned>(
        ctx->timeProvider->getSyncedTimestamp() ^ fileId[0] ^ fileId[fileId.size() - 1]);

    std::string requestUrl = string_format(
        "https://gae2-spclient.spotify.com/storage-resolve/files/audio/"
        "interactive/%s?product=0&country=%s&salt=%u",
        bytesToHexString(fileId).c_str(), countryCode.c_str(), salt);

    auto req = bell::HTTPClient::get(
        requestUrl,
        {bell::HTTPClient::ValueHeader({"Authorization", "Bearer " + accessKey}),
         bell::HTTPClient::ValueHeader({"Accept-Encoding", "identity"})});

    // Wait for response
    std::string_view result = req->body();

#ifdef BELL_ONLY_CJSON
    cJSON* jsonResult = cJSON_Parse(result.data());
    cdnUrl = cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0)
                 ->valuestring;
    cJSON_Delete(jsonResult);
#else
    auto jsonResult = nlohmann::json::parse(result, nullptr, false);
    if ((jsonResult.is_discarded() || !readSpotifyCdnUrl(jsonResult, cdnUrl)) &&
        !readSpotifyCdnUrlFromBinary(result, cdnUrl)) {
      auto contentEncoding = std::string(req->header("content-encoding"));
      auto transferEncoding = std::string(req->header("transfer-encoding"));
      auto contentType = std::string(req->header("content-type"));
      CSPOT_LOG(error,
                "Invalid CDN URL response: status=%d len=%u type=%s encoding=%s "
                "transfer=%s preview=%s",
                req->status(), static_cast<unsigned>(result.size()),
                contentType.c_str(), contentEncoding.c_str(),
                transferEncoding.c_str(), spotifyBodyPreview(result).c_str());
      state = State::FAILED;
      failureReason = "invalid CDN URL response";
      loadedSemaphore->give();
      return;
    }
#endif

    CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    state = State::READY;
    loadedSemaphore->give();
  } catch (...) {
    CSPOT_LOG(error, "Cannot fetch CDN URL");
    failureReason = "CDN URL request failed";
    state = State::FAILED;
    loadedSemaphore->give();
  }
}

void QueuedTrack::expire() {
  if (state != State::QUEUED) {
    failureReason = "queue entry expired";
    state = State::FAILED;
    loadedSemaphore->give();
  }
}

void QueuedTrack::stepLoadMetadata(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {

  // Prepare request ID
  std::string requestUrl = string_format(
      "hm://metadata/3/%s/%s",
      ref.type == TrackReference::Type::TRACK ? "track" : "episode",
      bytesToHexString(ref.gid).c_str());

  std::weak_ptr<QueuedTrack> weakSelf = weak_from_this();
  auto responseHandler = [weakSelf, &trackListMutex,
                          updateSemaphore](MercurySession::Response& res) {
    auto self = weakSelf.lock();
    if (!self) return;
    std::scoped_lock lock(trackListMutex);

    if (res.parts.size() == 0) {
      // Invalid metadata, cannot proceed
      self->failureReason = "empty metadata response";
      self->state = State::FAILED;
      updateSemaphore->give();
      self->loadedSemaphore->give();
      return;
    }

    // Parse the metadata
    if (self->ref.type == TrackReference::Type::TRACK) {
      pb_release(Track_fields, &self->metadataTrack);
      self->metadataTrack = Track_init_zero;
      if (!pbDecodeChecked(self->metadataTrack, Track_fields, res.parts[0])) {
        self->failureReason = "track metadata decode failed";
        self->state = State::FAILED;
        updateSemaphore->give();
        self->loadedSemaphore->give();
        return;
      }
    } else {
      pb_release(Episode_fields, &self->metadataEpisode);
      self->metadataEpisode = Episode_init_zero;
      if (!pbDecodeChecked(self->metadataEpisode, Episode_fields,
                           res.parts[0])) {
        self->failureReason = "episode metadata decode failed";
        self->state = State::FAILED;
        updateSemaphore->give();
        self->loadedSemaphore->give();
        return;
      }
    }

    // Parse received metadata
    self->stepParseMetadata(&self->metadataTrack, &self->metadataEpisode);

    updateSemaphore->give();
  };
  // Execute the request
  pendingMercuryRequest = ctx->session->execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  state = State::PENDING_META;
}

TrackQueue::TrackQueue(std::shared_ptr<cspot::Context> ctx,
                       std::shared_ptr<cspot::PlaybackState> state)
    : bell::Task("CSpotTrackQueue", 1024 * 32, 5, 0),
      playbackState(state),
      ctx(ctx) {
  accessKeyFetcher = std::make_shared<cspot::AccessKeyFetcher>(ctx);
  processSemaphore = std::make_shared<bell::WrappedSemaphore>();
  playableSemaphore = std::make_shared<bell::WrappedSemaphore>();

  // Assign encode callback to track list
  playbackState->innerFrame.state.track.funcs.encode =
      &TrackReference::pbEncodeTrackList;
  playbackState->innerFrame.state.track.arg = &currentTracks;
  // Start the task
  startTask();
};

TrackQueue::~TrackQueue() {
  stopTask();

  std::scoped_lock lock(tracksMutex);

}

TrackInfo TrackQueue::getTrackInfo(std::string_view identifier) {
  std::scoped_lock lock(tracksMutex);
  for (auto& track : preloadedTracks) {
    if (track->identifier == identifier)
      return track->trackInfo;
  }
  return TrackInfo{};
}

void TrackQueue::runTask() {
  isRunning = true;

  std::scoped_lock lock(runningMutex);

  std::deque<std::shared_ptr<QueuedTrack>> trackQueue;

  while (isRunning) {
    processSemaphore->twait(100);

    {
      std::scoped_lock lock(tracksMutex);
      // Snapshot the queue and index together. Skip/update may replace both
      // from another task, so neither may be observed outside this lock.
      if (currentTracksIndex < 0) {
        continue;
      }
      clearQueuedTracksCooperative(trackQueue);
      trackQueue = preloadedTracks;
    }

    if (trackQueue.empty()) {
      continue;
    }

    // Make sure we have the newest access key only when local playback needs it.
    accessKey = accessKeyFetcher->getAccessKey();

    for (auto& track : trackQueue) {
      if (track) {
        this->processTrack(track);
        cspotQueueYield(false);
      }
    }
    clearQueuedTracksCooperative(trackQueue);
  }
}

void TrackQueue::stopTask() {
  if (isRunning) {
    isRunning = false;
    processSemaphore->give();
    std::scoped_lock lock(runningMutex);
  }
}

std::shared_ptr<QueuedTrack> TrackQueue::consumeTrack(
    std::shared_ptr<QueuedTrack> prevTrack, int& offset) {
  std::scoped_lock lock(tracksMutex);

  if (currentTracksIndex == -1 || currentTracksIndex >= currentTracks.size()) {
    return nullptr;
  }

  // No previous track, return head
  if (prevTrack == nullptr) {
    offset = 0;

    if (preloadedTracks.empty()) {
      queueNextTrack(0);
    }

    return preloadedTracks.empty() ? nullptr : preloadedTracks[0];
  }

  // if (currentTracksIndex + preloadedTracks.size() >= currentTracks.size()) {
  //   offset = -1;

  //   // Last track in queue
  //   return nullptr;
  // }

  auto prevTrackIter =
      std::find(preloadedTracks.begin(), preloadedTracks.end(), prevTrack);

  if (prevTrackIter != preloadedTracks.end()) {
    // Get offset of next track
    offset = prevTrackIter - preloadedTracks.begin() + 1;
  } else {
    offset = 0;
  }

  if (offset >= preloadedTracks.size()) {
    // Last track in preloaded queue
    return nullptr;
  }

  // Return the current track
  return preloadedTracks[offset];
}

void TrackQueue::processTrack(std::shared_ptr<QueuedTrack> track) {
  if (track && track->loadStartMs == 0) {
    track->loadStartMs = cspotNowMs();
  }

  switch (track->state) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED:
      track->stepLoadCDNUrl(accessKey);

      if (track->state == QueuedTrack::State::READY) {
        std::scoped_lock lock(tracksMutex);
        const bool stillQueued =
            std::find(preloadedTracks.begin(), preloadedTracks.end(), track) !=
            preloadedTracks.end();
        if (stillQueued && preloadedTracks.size() < MAX_TRACKS_PRELOAD) {
          // Queue a new track to preload
          queueNextTrack(preloadedTracks.size());
        }
      }
      break;
    default:
      // Do not perform any action
      break;
  }
}

bool TrackQueue::queueNextTrack(int offset, uint32_t positionMs) {
  const int requestedRefIndex = offset + currentTracksIndex;

  if (requestedRefIndex < 0 || requestedRefIndex >= currentTracks.size()) {
    return false;
  }

  // in case we re-queue current track, make sure position is updated (0)
  if (offset == 0 && preloadedTracks.size() &&
      preloadedTracks[0]->ref == currentTracks[currentTracksIndex]) {
    preloadedTracks.pop_front();
  }

  if (offset <= 0) {
    preloadedTracks.push_front(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  } else {
    preloadedTracks.push_back(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  }

  processSemaphore->give();

  return true;
}

bool TrackQueue::skipTrack(SkipDirection dir, bool expectNotify) {
  bool skipped = true;
  std::deque<std::shared_ptr<QueuedTrack>> retiredTracks;
  std::unique_lock<std::mutex> lock(tracksMutex);
  int16_t oldIndex = currentTracksIndex;

  if (dir == SkipDirection::PREV) {
    uint64_t position =
        !playbackState->innerFrame.state.has_position_ms
            ? 0
            : playbackState->innerFrame.state.position_ms +
                  ctx->timeProvider->getSyncedTimestamp() -
                  playbackState->innerFrame.state.position_measured_at;

    if (currentTracksIndex > 0 && position < 3000) {
      queueNextTrack(-1);

      if (preloadedTracks.size() > MAX_TRACKS_PRELOAD) {
        retiredTracks.push_back(std::move(preloadedTracks.back()));
        preloadedTracks.pop_back();
      }

      currentTracksIndex--;
    } else {
      queueNextTrack(0);
    }
  } else {
    if (currentTracks.size() > currentTracksIndex + 1) {
      if (!preloadedTracks.empty()) {
        retiredTracks.push_back(std::move(preloadedTracks.front()));
        preloadedTracks.pop_front();
      }

      if (!queueNextTrack(preloadedTracks.size() + 1)) {
        CSPOT_LOG(info, "Failed to queue next track");
      }

      currentTracksIndex++;
    } else {
      skipped = false;
    }
  }

  if (skipped) {
    // Update frame data
    playbackState->innerFrame.state.playing_track_index = currentTracksIndex;
    CSPOT_LOG(info, "TrackQueue skip dir=%s oldIdx=%d newIdx=%d total=%u expectNotify=%d",
              dir == SkipDirection::NEXT ? "NEXT" : "PREV",
              (int)oldIndex, (int)currentTracksIndex,
              (unsigned)currentTracks.size(), expectNotify ? 1 : 0);

    if (expectNotify) {
      // Reset position to zero
      notifyPending = true;
    }
  } else {
    CSPOT_LOG(error, "TrackQueue skip rejected dir=%s idx=%d total=%u",
              dir == SkipDirection::NEXT ? "NEXT" : "PREV",
              (int)currentTracksIndex, (unsigned)currentTracks.size());
  }

  lock.unlock();
  clearQueuedTracksCooperative(retiredTracks);
  return skipped;
}

bool TrackQueue::hasTracks() {
  std::scoped_lock lock(tracksMutex);

  return currentTracks.size() > 0;
}

uint64_t TrackQueue::currentTrackInstanceId() {
  std::scoped_lock lock(tracksMutex);
  return preloadedTracks.empty() ? 0 : preloadedTracks.front()->instanceId;
}

bool TrackQueue::isFinished() {
  std::scoped_lock lock(tracksMutex);
  return currentTracksIndex >= currentTracks.size() - 1;
}

bool TrackQueue::isCurrentTrack(const TrackReference& ref) {
  std::scoped_lock lock(tracksMutex);
  if (currentTracksIndex < 0 || currentTracksIndex >= currentTracks.size()) {
    return false;
  }
  return currentTracks[currentTracksIndex] == ref;
}

bool TrackQueue::updateTracks(uint32_t requestedPosition, bool initial) {
  std::deque<std::shared_ptr<QueuedTrack>> retiredTracks;
  std::unique_lock<std::mutex> lock(tracksMutex);
  bool cleared = true;
  int16_t oldIndex = currentTracksIndex;

  // Copy requested track list
  currentTracks = playbackState->remoteTracks;
  currentTracksIndex = playbackState->innerFrame.state.playing_track_index;
  CSPOT_LOG(info,
            "TrackQueue updateTracks initial=%d oldIdx=%d newIdx=%d total=%u requestedPos=%u",
            initial ? 1 : 0, (int)oldIndex, (int)currentTracksIndex,
            (unsigned)currentTracks.size(), (unsigned)requestedPosition);

  if (initial) {
    // Detach stale work immediately; cancellation/destruction happens after
    // releasing the control-path mutex.
    retiredTracks.swap(preloadedTracks);

    if (currentTracksIndex < currentTracks.size()) {
      // Push a song on the preloaded queue
      queueNextTrack(0, requestedPosition);
    }

    // We already updated track meta, mark it
    notifyPending = true;

    playableSemaphore->give();
  } else if (!preloadedTracks.empty() && preloadedTracks[0]->loading) {
    // try to not re-load track if we are still loading it

    // Keep the active loader, but retire speculative tracks outside the lock.
    while (preloadedTracks.size() > 1) {
      retiredTracks.push_back(std::move(preloadedTracks.back()));
      preloadedTracks.pop_back();
    }

    CSPOT_LOG(info, "Keeping current track %d", currentTracksIndex);

    cleared = false;
  } else {
    retiredTracks.swap(preloadedTracks);

    // Push a song on the preloaded queue
    CSPOT_LOG(info, "Re-loading current track");
    queueNextTrack(0, requestedPosition);
  }

  lock.unlock();
  clearQueuedTracksCooperative(retiredTracks);
  return cleared;
}
