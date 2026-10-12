#include "TrackPlayer.h"

#include <algorithm>
#include <cinttypes>
#include <mutex>        // for mutex, scoped_lock
#include <string>       // for string
#include <type_traits>  // for remove_extent_t
#include <vector>       // for vector, vector<>::value_type

#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

#include "BellLogger.h"        // for AbstractLogger
#include "BellUtils.h"         // for BELL_SLEEP_MS
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "TrackQueue.h"        // for CDNTrackStream, CDNTrackStream::TrackInfo
#include "WrappedSemaphore.h"  // for WrappedSemaphore

#ifdef BELL_VORBIS_FLOAT
#define VORBIS_SEEK(file, position) \
  (ov_time_seek_page(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek_page(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#endif

static constexpr uint32_t INITIAL_HANDOFF_SEEK_BYPASS_MS = 0;
static constexpr uint32_t PCM_RATE_LOG_INTERVAL_MS = 3000;
static constexpr size_t PCM_PRODUCER_YIELD_BYTES = 64 * 1024;
static constexpr TickType_t PCM_PRODUCER_COOPERATIVE_DELAY_TICKS =
    pdMS_TO_TICKS(4);

namespace cspot {
struct Context;
struct TrackReference;
}  // namespace cspot

using namespace cspot;

#ifdef ESP_PLATFORM
static uint32_t cspotPlayerNowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
#else
static uint32_t cspotPlayerNowMs() {
  return 0;
}
#endif

static size_t vorbisReadCb(void* ptr, size_t size, size_t nmemb,
                           TrackPlayer* self) {
  return self->_vorbisRead(ptr, size, nmemb);
}

static int vorbisCloseCb(TrackPlayer* self) {
  return self->_vorbisClose();
}

static int vorbisSeekCb(TrackPlayer* self, int64_t offset, int whence) {

  return self->_vorbisSeek(offset, whence);
}

static int vorbisNoSeekCb(TrackPlayer*, int64_t, int) {
  return -1;
}

static long vorbisTellCb(TrackPlayer* self) {
  return self->_vorbisTell();
}

static long vorbisNoTellCb(TrackPlayer*) {
  return -1;
}

namespace {
uint32_t trackPlayerNowMs() {
#ifdef ESP_PLATFORM
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
#else
  return 0;
#endif
}
}  // namespace

TrackPlayer::TrackPlayer(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::TrackQueue> trackQueue,
                         EOFCallback eof, TrackLoadedCallback trackLoaded)
    // Vorbis decoding is throughput work, not a realtime owner. Keeping it
    // scheduler-balanced and below the PCM consumer prevents a long Spotify
    // seek from starving either core's idle/watchdog service.
    : bell::Task("cspot_player", 64 * 1024, 16, tskNO_AFFINITY) {
  this->ctx = ctx;
  this->eofCallback = eof;
  this->trackLoaded = trackLoaded;
  this->trackQueue = trackQueue;
  this->playbackSemaphore = std::make_unique<bell::WrappedSemaphore>(5);

  // Initialize vorbis callbacks
  vorbisFile = {};
  vorbisCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisTellCb,
  };
  vorbisStreamCallbacks = {
      (decltype(ov_callbacks::read_func))&vorbisReadCb,
      (decltype(ov_callbacks::seek_func))&vorbisNoSeekCb,
      (decltype(ov_callbacks::close_func))&vorbisCloseCb,
      (decltype(ov_callbacks::tell_func))&vorbisNoTellCb,
  };
}

TrackPlayer::~TrackPlayer() {
  isRunning = false;
  CDNAudioFile::abortActiveStream();
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::start() {
  if (!isRunning) {
    isRunning = true;
    startTask();
  }
}

void TrackPlayer::stop() {
  isRunning = false;
  CDNAudioFile::abortActiveStream();
  resetState();
  std::scoped_lock lock(runningMutex);
}

void TrackPlayer::resetState(bool paused) {
  // Mark for reset
  this->currentSongPlaying = false;
  this->pendingSeekPositionMs = 0;
  this->pendingSeekRequested = false;
  this->startPaused = paused;
  this->pendingReset = true;
  CDNAudioFile::abortActiveStream();

  std::scoped_lock lock(dataOutMutex);

  CSPOT_LOG(info, "Resetting state");
}

bool TrackPlayer::beginQueueReplacement(bool paused, uint32_t timeoutMs) {
  // Queue publication and decoder teardown are one transaction. Merely
  // setting pendingReset is not sufficient: the decoder may otherwise consume
  // the old queue before its publisher installs the replacement.
  queueReplacementInProgress.store(true, std::memory_order_release);
  decoderQuiesced.store(false, std::memory_order_release);
  resetState(paused);
  trackQueue->playableSemaphore->give();

  const uint32_t startedAt = cspotPlayerNowMs();
  while (isRunning.load(std::memory_order_acquire) &&
         !decoderQuiesced.load(std::memory_order_acquire)) {
    if (cspotPlayerNowMs() - startedAt >= timeoutMs) {
      CSPOT_LOG(error,
                "Decoder did not quiesce for queue replacement within %u ms",
                (unsigned)timeoutMs);
      queueReplacementInProgress.store(false, std::memory_order_release);
      trackQueue->playableSemaphore->give();
      return false;
    }
    BELL_SLEEP_MS(2);
  }
  return true;
}

void TrackPlayer::endQueueReplacement() {
  queueReplacementInProgress.store(false, std::memory_order_release);
  trackQueue->playableSemaphore->give();
}

void TrackPlayer::seekMs(size_t ms) {
  CSPOT_LOG(info, "Seek requested pos=%u", (unsigned)ms);
  this->pendingSeekPositionMs.store(ms, std::memory_order_release);
  this->pendingSeekRequested.store(true, std::memory_order_release);
  CDNAudioFile::interruptActiveStreamForSeek();
}

void TrackPlayer::runTask() {
  std::scoped_lock lock(runningMutex);

  std::shared_ptr<QueuedTrack> track, newTrack = nullptr;

  int trackOffset = 0;
  bool eof = false;
  bool endOfQueueReached = false;
  uint32_t lastDecodeRateLogMs = trackPlayerNowMs();
  uint64_t producedBytesSinceLog = 0;
  uint32_t decoderIdleMs = 0;

  while (isRunning) {
    bool retryCurrentTrack = false;
    bool retryCurrentTrackFast = false;

    if (queueReplacementInProgress.load(std::memory_order_acquire)) {
      currentSongPlaying.store(false, std::memory_order_release);
      track = nullptr;
      decoderQuiesced.store(true, std::memory_order_release);
      BELL_SLEEP_MS(2);
      continue;
    }

    // Ensure we even have any tracks to play
    if (!this->trackQueue->hasTracks() ||
        (!pendingReset && endOfQueueReached && trackQueue->isFinished())) {
      this->trackQueue->playableSemaphore->twait(300);
      continue;
    }

    // Last track was interrupted, reset to default
    if (pendingReset) {
      track = nullptr;
      pendingReset = false;
      inFuture = false;
      decoderQuiesced.store(true, std::memory_order_release);
    }

    endOfQueueReached = false;

    if (pendingReset) {
      continue;
    }

    newTrack = trackQueue->consumeTrack(track, trackOffset);

    if (queueReplacementInProgress.load(std::memory_order_acquire)) {
      track = nullptr;
      decoderQuiesced.store(true, std::memory_order_release);
      continue;
    }

    if (newTrack == nullptr) {
      if (trackOffset == -1) {
        // Reset required
        track = nullptr;
      }

      BELL_SLEEP_MS(2);
      continue;
    }

    track = newTrack;

    inFuture = trackOffset > 0;

    if (track->state != QueuedTrack::State::READY) {
      auto waitStartMs = cspotPlayerNowMs();
      while (track->state != QueuedTrack::State::READY &&
             !pendingReset.load(std::memory_order_acquire) &&
             !queueReplacementInProgress.load(std::memory_order_acquire) &&
             cspotPlayerNowMs() - waitStartMs < 12000) {
        track->loadedSemaphore->twait(50);
      }

      if (pendingReset.load(std::memory_order_acquire) ||
          queueReplacementInProgress.load(std::memory_order_acquire)) {
        track = nullptr;
        continue;
      }

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error,
                  "Track failed to load; holding current queue position state=%d reason=%s uri=%s wait=%u total=%u",
                  (int)track->state,
                  track->failureReason.empty() ? "unspecified" : track->failureReason.c_str(),
                  track->ref.uri.c_str(),
                  (unsigned)(cspotPlayerNowMs() - waitStartMs),
                  (unsigned)(track->loadStartMs ? cspotPlayerNowMs() - track->loadStartMs : 0));
        track = nullptr;
        BELL_SLEEP_MS(100);
        continue;
      }
    }

    currentSongPlaying = true;
    decoderQuiesced.store(false, std::memory_order_release);
    uint32_t decodedPlaybackMs = track->requestedPosition;
    size_t finalStreamPosition = 0;
    size_t finalStreamSize = 0;
    size_t decodedBytesSinceYield = 0;

    {
      std::scoped_lock lock(playbackMutex);

      currentTrackStream = track->getAudioFile();

      // Open the stream
      for (int openAttempt = 0; openAttempt < 3; openAttempt++) {
        currentTrackStream->openStream();
        if (currentTrackStream->getSize() > 0 || pendingReset ||
            queueReplacementInProgress.load(std::memory_order_acquire) ||
            !currentSongPlaying) {
          break;
        }
        CSPOT_LOG(error, "Track stream open failed, retrying attempt=%d",
                  openAttempt + 1);
        BELL_SLEEP_MS(350);
      }
      if (currentTrackStream->getSize() == 0) {
        CSPOT_LOG(error, "Track stream open failed; holding current queue position");
        currentTrackStream = nullptr;
        currentSongPlaying = false;
        track->loading = false;
        track = nullptr;
        BELL_SLEEP_MS(100);
        continue;
      }

      if (pendingReset ||
          queueReplacementInProgress.load(std::memory_order_acquire) ||
          !currentSongPlaying) {
        currentTrackStream = nullptr;
        track->loading = false;
        continue;
      }

      if (trackOffset == 0 && !pendingSeekRequested.load(std::memory_order_acquire)) {
        this->trackLoaded(track, startPaused);
        startPaused = false;
      }

      if (pendingSeekRequested.exchange(false, std::memory_order_acq_rel)) {
        track->requestedPosition = pendingSeekPositionMs.load(std::memory_order_acquire);
      }

      bool vorbisSeekableOpen = track->requestedPosition > 0;
      auto openStartMs = cspotPlayerNowMs();
      int32_t r = ov_open_callbacks(this, &vorbisFile, NULL, 0,
                                    vorbisSeekableOpen ? vorbisCallbacks
                                                       : vorbisStreamCallbacks);
      CSPOT_LOG(info, "Vorbis open mode=%s result=%d dt=%u total=%u",
                vorbisSeekableOpen ? "seek" : "stream", (int)r,
                (unsigned)(cspotPlayerNowMs() - openStartMs),
                (unsigned)(track->loadStartMs ? cspotPlayerNowMs() - track->loadStartMs : 0));
      (void)openStartMs;
      if (r < 0) {
        CSPOT_LOG(error, "Vorbis open failed result=%d; retrying current track", (int)r);
        retryCurrentTrack = true;
        retryCurrentTrackFast = true;
        currentSongPlaying = false;
        currentTrackStream = nullptr;
        track->loading = false;
        continue;
      }

      if (track->requestedPosition > 0 &&
          track->requestedPosition <= INITIAL_HANDOFF_SEEK_BYPASS_MS) {
        track->requestedPosition = 0;
        decodedPlaybackMs = 0;
      } else if (track->requestedPosition > 0) {
        auto seekStartMs = cspotPlayerNowMs();
        VORBIS_SEEK(&vorbisFile, track->requestedPosition);
        CSPOT_LOG(error, "Track initial seek pos=%u dt=%u total=%u",
                  (unsigned)track->requestedPosition,
                  (unsigned)(cspotPlayerNowMs() - seekStartMs),
                  (unsigned)(track->loadStartMs ? cspotPlayerNowMs() - track->loadStartMs : 0));
      }

      eof = false;
      track->loading = true;
      decodedPlaybackMs = track->requestedPosition;

      while (!eof && currentSongPlaying &&
             !queueReplacementInProgress.load(std::memory_order_acquire)) {
        // Execute seek if needed
        if (pendingSeekRequested.exchange(false, std::memory_order_acq_rel)) {
          uint32_t seekPosition =
              (uint32_t)pendingSeekPositionMs.load(std::memory_order_acquire);

          if (!vorbisSeekableOpen) {
            // Normal playback opens in streaming mode for fast startup. On
            // the first seek, reopen the existing cached CDN file with seek
            // callbacks instead of reloading the whole queue entry.
            ov_clear(&vorbisFile);
            currentTrackStream->seek(0);
            auto reopenStartedAt = cspotPlayerNowMs();
            int32_t reopenResult =
                ov_open_callbacks(this, &vorbisFile, NULL, 0, vorbisCallbacks);
            CSPOT_LOG(info, "Seekable reopen result=%d dt=%u pos=%u",
                      (int)reopenResult,
                      (unsigned)(cspotPlayerNowMs() - reopenStartedAt),
                      (unsigned)seekPosition);
            (void)reopenStartedAt;
            if (reopenResult < 0) {
              vorbisFile = {};
              track->requestedPosition = seekPosition;
              retryCurrentTrack = true;
              retryCurrentTrackFast = true;
              currentSongPlaying = false;
              eof = false;
              break;
            }
            vorbisSeekableOpen = true;
          }

          track->requestedPosition = seekPosition;

          // Seek to the new position
          VORBIS_SEEK(&vorbisFile, seekPosition);
          decodedPlaybackMs = seekPosition;
        }

        uint32_t vorbisReadStartMs = trackPlayerNowMs();
        long ret = VORBIS_READ(&vorbisFile, (char*)&pcmBuffer[0],
                               pcmBuffer.size(), &currentSection);
        uint32_t vorbisReadDtMs = trackPlayerNowMs() - vorbisReadStartMs;
        decoderIdleMs += vorbisReadDtMs;

        // A seek may have aborted the range read above. Handle the latest
        // target at the top of the loop instead of treating that abort as EOF.
        if (pendingSeekRequested.load(std::memory_order_acquire)) {
          continue;
        }

        if (ret == 0) {
          size_t streamPosition = currentTrackStream ? currentTrackStream->getPosition() : 0;
          size_t streamSize = currentTrackStream ? currentTrackStream->getSize() : 0;
          uint32_t trackDuration = track->trackInfo.duration;
          if (streamSize > 0 && streamPosition + (256 * 1024) < streamSize) {
            CSPOT_LOG(error,
                      "False EOF before end of Spotify stream, retrying track pos=%u size=%u",
                      (unsigned)streamPosition, (unsigned)streamSize);
            retryCurrentTrack = true;
            currentSongPlaying = false;
            eof = false;
            break;
          }
          constexpr uint32_t eofDecodeSlackMs = 1000;
          constexpr uint32_t eofAcceptWindowMs = 1000;
          uint32_t remainingPlaybackMs =
              trackDuration > decodedPlaybackMs ? trackDuration - decodedPlaybackMs : 0;
          bool streamIsTerminal = streamSize > 0 && streamPosition + (256 * 1024) >= streamSize;
          bool durationLooksComplete = trackDuration > 0 &&
                                      decodedPlaybackMs + eofDecodeSlackMs >= trackDuration;
          if (streamIsTerminal || remainingPlaybackMs <= eofAcceptWindowMs || durationLooksComplete) {
            CSPOT_LOG(info,
                      "EOF terminal/near-end, treating as normal completion decoded=%u duration=%u remaining=%u streamPos=%u size=%u",
                      (unsigned)decodedPlaybackMs, (unsigned)trackDuration,
                      (unsigned)remainingPlaybackMs,
                      (unsigned)streamPosition, (unsigned)streamSize);
            eof = true;
          } else if (trackDuration > 0 && decodedPlaybackMs + eofDecodeSlackMs < trackDuration) {
            CSPOT_LOG(error,
                      "Guarded early EOF, fast retrying current track decoded=%u duration=%u remaining=%u streamPos=%u size=%u",
                      (unsigned)decodedPlaybackMs, (unsigned)trackDuration,
                      (unsigned)(trackDuration - decodedPlaybackMs),
                      (unsigned)streamPosition, (unsigned)streamSize);
            track->requestedPosition =
                decodedPlaybackMs > 1000 ? decodedPlaybackMs - 500 : decodedPlaybackMs;
            retryCurrentTrack = true;
            retryCurrentTrackFast = true;
            currentSongPlaying = false;
            eof = false;
            break;
          }
          CSPOT_LOG(info, "EOF");
          // and done :)
          eof = true;
        } else if (ret < 0) {
          CSPOT_LOG(error, "An error has occured in the stream %d", ret);
          retryCurrentTrack = true;
          currentSongPlaying = false;
        } else {
          producedBytesSinceLog += (uint64_t)ret;
          decodedPlaybackMs +=
              (uint32_t)(((uint64_t)ret * 1000ULL) / (44100ULL * 2ULL * 2ULL));
          decodedBytesSinceYield += (size_t)ret;
          if (this->dataCallback != nullptr) {
            size_t toWrite = (size_t)ret;

            while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {
              size_t written = 0;
              {
                std::scoped_lock dataOutLock(dataOutMutex);
                // If reset happened during playback, return
                if (!currentSongPlaying || pendingReset)
                  break;

                size_t offset = (size_t)ret - (size_t)toWrite;
                written = dataCallback(pcmBuffer.data() + offset, toWrite,
                                       track->identifier);
              }
              if (written > toWrite) {
                CSPOT_LOG(error,
                          "PCM callback contract violation written=%u requested=%u",
                          (unsigned)written, (unsigned)toWrite);
                written = toWrite;
              }
              if (written == 0) {
                vTaskDelay(1);
              }
              toWrite -= written;
            }
          }
          if (decodedBytesSinceYield >= PCM_PRODUCER_YIELD_BYTES) {
            decodedBytesSinceYield = 0;
            // A bare yield only selects peers at the same priority. Block for
            // a bounded interval so the idle/watchdog and hosted networking
            // tasks are guaranteed an execution window on sustained decode.
            vTaskDelay(std::max<TickType_t>(1,
                PCM_PRODUCER_COOPERATIVE_DELAY_TICKS));
          }
        }

        uint32_t nowMs = trackPlayerNowMs();
        if (nowMs != 0 && (lastDecodeRateLogMs == 0 ||
                           (long)(nowMs - lastDecodeRateLogMs) >=
                               (long)PCM_RATE_LOG_INTERVAL_MS)) {
          uint32_t elapsedMs = lastDecodeRateLogMs == 0
                                   ? PCM_RATE_LOG_INTERVAL_MS
                                   : (nowMs - lastDecodeRateLogMs);
          uint32_t producedPerSec = elapsedMs > 0
                                        ? (uint32_t)((producedBytesSinceLog * 1000ULL) / elapsedMs)
                                        : 0;
          (void)producedPerSec;
          CSPOT_LOG(info,
                    "PCM decode rate produced=%uB/s idle=%ums last_read_dt=%ums",
                    (unsigned)producedPerSec, (unsigned)decoderIdleMs,
                    (unsigned)vorbisReadDtMs);
          producedBytesSinceLog = 0;
          decoderIdleMs = 0;
          lastDecodeRateLogMs = nowMs;
        }
      }
      ov_clear(&vorbisFile);

      CSPOT_LOG(info, "Playing done");

      // always move back to LOADING (ensure proper seeking after last track has been loaded)
      finalStreamPosition = currentTrackStream ? currentTrackStream->getPosition() : 0;
      finalStreamSize = currentTrackStream ? currentTrackStream->getSize() : 0;
      currentTrackStream = nullptr;
      track->loading = false;
    }

    if (queueReplacementInProgress.load(std::memory_order_acquire)) {
      track = nullptr;
      decoderQuiesced.store(true, std::memory_order_release);
      continue;
    }

    if (retryCurrentTrack) {
      track = nullptr;
      BELL_SLEEP_MS(retryCurrentTrackFast ? 10 : 250);
      continue;
    }

    if (eof) {
      // Decoding runs ahead of the physical output. Do not advance Spotify's
      // queue until the speaker has rendered the PCM already handed off.
      if (playbackDrainCallback != nullptr) {
        playbackDrainCallback();
      }
      if (trackQueue->isFinished()) {
        endOfQueueReached = true;
      }

      this->eofCallback(decodedPlaybackMs, track->trackInfo.duration,
                        finalStreamPosition, finalStreamSize);
    }
  }
}

size_t TrackPlayer::_vorbisRead(void* ptr, size_t size, size_t nmemb) {
  // libvorbis can continue probing after a queue replacement has retired the
  // current decoder. Terminate that probe immediately instead of allowing an
  // obsolete CDN seek/read to delay the newly selected track.
  if (pendingReset.load(std::memory_order_acquire) ||
      !currentSongPlaying.load(std::memory_order_acquire) ||
      this->currentTrackStream == nullptr || ptr == nullptr || size == 0 ||
      nmemb > SIZE_MAX / size) {
    return 0;
  }

  size_t requestedBytes = nmemb * size;
  uint32_t waitStartMs = trackPlayerNowMs();
  static unsigned long lastNoDataRetryLogMs = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    size_t bytesRead =
        this->currentTrackStream->readBytes((uint8_t*)ptr, requestedBytes);
    if (bytesRead > 0) {
      uint32_t waitDtMs = trackPlayerNowMs() - waitStartMs;
      if (waitDtMs >= 50) {
        CSPOT_LOG(info, "CDN read wait bytes=%u dt=%u attempts=%d",
                  (unsigned)bytesRead, (unsigned)waitDtMs, attempt + 1);
      }
      return bytesRead;
    }

    size_t streamPosition = this->currentTrackStream->getPosition();
    size_t streamSize = this->currentTrackStream->getSize();
    if (streamSize == 0 || streamPosition >= streamSize) {
      return 0;
    }

    unsigned long nowMs = trackPlayerNowMs();
    if (lastNoDataRetryLogMs == 0 || nowMs < lastNoDataRetryLogMs ||
        nowMs - lastNoDataRetryLogMs >= 3000UL) {
      CSPOT_LOG(info,
                "CDN read returned no data before EOF, retrying pos=%u size=%u attempt=%d",
                (unsigned)streamPosition, (unsigned)streamSize, attempt + 1);
      lastNoDataRetryLogMs = nowMs;
    }
      BELL_SLEEP_MS(1);
  }

  uint32_t waitDtMs = trackPlayerNowMs() - waitStartMs;
  if (waitDtMs >= 50) {
    CSPOT_LOG(info, "CDN read wait bytes=0 dt=%u attempts=4",
              (unsigned)waitDtMs);
  }
  return 0;
}

size_t TrackPlayer::_vorbisClose() {
  return 0;
}

int TrackPlayer::_vorbisSeek(int64_t offset, int whence) {
  // A reset means the Ogg instance and its stream no longer own playback.
  // Returning failure makes ov_time_seek_page unwind rather than performing
  // further range probes against a track that has already been superseded.
  if (pendingReset.load(std::memory_order_acquire) ||
      !currentSongPlaying.load(std::memory_order_acquire)) {
    return -1;
  }
  if (this->currentTrackStream == nullptr) {
    return -1;
  }
  const size_t streamSize = this->currentTrackStream->getSize();
  const auto addSignedOffset = [streamSize](size_t base, int64_t delta) {
    base = std::min(base, streamSize);
    if (delta >= 0) {
      const uint64_t increase = (uint64_t)delta;
      return increase > streamSize - base ? streamSize
                                          : base + (size_t)increase;
    }
    const uint64_t decrease = (uint64_t)(-(delta + 1)) + 1;
    return decrease > base ? (size_t)0 : base - (size_t)decrease;
  };

  switch (whence) {
    case 0:
      this->currentTrackStream->seek(
          offset > 0 ? std::min((size_t)offset, streamSize) : 0);
      break;
    case 1: {
      this->currentTrackStream->seek(addSignedOffset(
          this->currentTrackStream->getPosition(), offset));
      break;
    }
    case 2: {
      this->currentTrackStream->seek(addSignedOffset(streamSize, offset));
      break;
    }
    default:
      return -1;
  }

  return 0;
}

long TrackPlayer::_vorbisTell() {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  return this->currentTrackStream->getPosition();
}

void TrackPlayer::setDataCallback(DataCallback callback) {
  this->dataCallback = callback;
}

void TrackPlayer::setPlaybackDrainCallback(PlaybackDrainCallback callback) {
  this->playbackDrainCallback = callback;
}
