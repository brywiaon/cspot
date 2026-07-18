#include "TrackPlayer.h"

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
  (ov_time_seek(file, (double)position / 1000))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, 0, 2, 1, section))
#else
#define VORBIS_SEEK(file, position) (ov_time_seek(file, position))
#define VORBIS_READ(file, buffer, bufferSize, section) \
  (ov_read(file, buffer, bufferSize, section))
#endif

static constexpr uint32_t INITIAL_HANDOFF_SEEK_BYPASS_MS = 0;
static constexpr uint32_t PCM_RATE_LOG_INTERVAL_MS = 1000;

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
    : bell::Task("cspot_player", 64 * 1024, 15, 1) {
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
  this->startPaused = paused;
  this->pendingReset = true;
  CDNAudioFile::abortActiveStream();

  std::scoped_lock lock(dataOutMutex);

  CSPOT_LOG(info, "Resetting state");
}

void TrackPlayer::seekMs(size_t ms) {
  CSPOT_LOG(info, "Seeking...");
  this->pendingSeekPositionMs = ms;
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
    }

    endOfQueueReached = false;

    // Wait 800ms. If next reset is requested in meantime, restart the queue.
    // Gets rid of excess actions during rapid queueing
      BELL_SLEEP_MS(5);

    if (pendingReset) {
      continue;
    }

    newTrack = trackQueue->consumeTrack(track, trackOffset);

    if (newTrack == nullptr) {
      if (trackOffset == -1) {
        // Reset required
        track = nullptr;
      }

      BELL_SLEEP_MS(10);
      continue;
    }

    track = newTrack;

    inFuture = trackOffset > 0;

    if (track->state != QueuedTrack::State::READY) {
      auto waitStartMs = cspotPlayerNowMs();
      track->loadedSemaphore->twait(12000);

      if (track->state != QueuedTrack::State::READY) {
        CSPOT_LOG(error,
                  "Track failed to load; holding current queue position state=%d wait=%u total=%u",
                  (int)track->state, (unsigned)(cspotPlayerNowMs() - waitStartMs),
                  (unsigned)(track->loadStartMs ? cspotPlayerNowMs() - track->loadStartMs : 0));
        track = nullptr;
        BELL_SLEEP_MS(500);
        continue;
      }
    }

    currentSongPlaying = true;
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
        BELL_SLEEP_MS(500);
        continue;
      }

      if (pendingReset || !currentSongPlaying) {
        currentTrackStream = nullptr;
        track->loading = false;
        continue;
      }

      if (trackOffset == 0 && pendingSeekPositionMs == 0) {
        this->trackLoaded(track, startPaused);
        startPaused = false;
      }

      if (pendingSeekPositionMs > 0) {
        track->requestedPosition = pendingSeekPositionMs;
        pendingSeekPositionMs = 0;
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

      while (!eof && currentSongPlaying) {
        // Execute seek if needed
        if (pendingSeekPositionMs > 0) {
          uint32_t seekPosition = pendingSeekPositionMs;

          // Reset the pending seek position
          pendingSeekPositionMs = 0;

          if (!vorbisSeekableOpen) {
            track->requestedPosition = seekPosition;
            retryCurrentTrack = true;
            retryCurrentTrackFast = true;
            currentSongPlaying = false;
            eof = false;
            break;
          }

          // Seek to the new position
          VORBIS_SEEK(&vorbisFile, seekPosition);
          decodedPlaybackMs = seekPosition;
        }

        uint32_t vorbisReadStartMs = trackPlayerNowMs();
        long ret = VORBIS_READ(&vorbisFile, (char*)&pcmBuffer[0],
                               pcmBuffer.size(), &currentSection);
        uint32_t vorbisReadDtMs = trackPlayerNowMs() - vorbisReadStartMs;
        decoderIdleMs += vorbisReadDtMs;

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
            auto toWrite = ret;

            while (!eof && currentSongPlaying && !pendingReset && toWrite > 0) {
              int written = 0;
              {
                std::scoped_lock dataOutLock(dataOutMutex);
                // If reset happened during playback, return
                if (!currentSongPlaying || pendingReset)
                  break;

                written = dataCallback(pcmBuffer.data() + (ret - toWrite),
                                       toWrite, track->identifier);
              }
              if (written == 0) {
                taskYIELD();
              }
              toWrite -= written;
            }
          }
          if (decodedBytesSinceYield >= (512 * 1024)) {
            decodedBytesSinceYield = 0;
          }
          if ((producedBytesSinceLog & ((512 * 1024) - 1)) == 0) {
            taskYIELD();
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

    if (retryCurrentTrack) {
      track = nullptr;
      BELL_SLEEP_MS(retryCurrentTrackFast ? 10 : 250);
      continue;
    }

    if (eof) {
      if (trackQueue->isFinished()) {
        endOfQueueReached = true;
      }

      this->eofCallback(decodedPlaybackMs, track->trackInfo.duration,
                        finalStreamPosition, finalStreamSize);
    }
  }
}

size_t TrackPlayer::_vorbisRead(void* ptr, size_t size, size_t nmemb) {
  if (this->currentTrackStream == nullptr) {
    return 0;
  }

  size_t requestedBytes = nmemb * size;
  uint32_t waitStartMs = trackPlayerNowMs();
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

    CSPOT_LOG(error,
              "CDN read returned no data before EOF, retrying pos=%u size=%u attempt=%d",
              (unsigned)streamPosition, (unsigned)streamSize, attempt + 1);
    BELL_SLEEP_MS(40);
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
  if (this->currentTrackStream == nullptr) {
    return 0;
  }
  switch (whence) {
    case 0:
      this->currentTrackStream->seek(offset);  // Spotify header offset
      break;
    case 1:
      this->currentTrackStream->seek(this->currentTrackStream->getPosition() +
                                     offset);
      break;
    case 2:
      this->currentTrackStream->seek(this->currentTrackStream->getSize() +
                                     offset);
      break;
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
