#include "SpircHandler.h"

#include <cstdint>      // for uint8_t
#include <memory>       // for shared_ptr, make_unique, unique_ptr
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "BellLogger.h"      // for AbstractLogger
#include "CSpotContext.h"    // for Context::ConfigState, Context (ptr only)
#include "Logger.h"          // for CSPOT_LOG
#include "MercurySession.h"  // for MercurySession, MercurySession::Response
#include "NanoPBHelper.h"    // for pbDecode
#include "Packet.h"          // for cspot
#include "PlaybackState.h"   // for PlaybackState, PlaybackState::State
#include "TrackPlayer.h"     // for TrackPlayer
#include "TrackQueue.h"
#include "TrackReference.h"     // for TrackReference
#include "Utils.h"              // for stringHexToBytes
#include "pb_decode.h"          // for pb_release
#include "protobuf/spirc.pb.h"  // for Frame, State, Frame_fields, MessageTy...

using namespace cspot;

SpircHandler::SpircHandler(std::shared_ptr<cspot::Context> ctx) {
  this->playbackState = std::make_shared<PlaybackState>(ctx);
  this->trackQueue = std::make_shared<cspot::TrackQueue>(ctx, playbackState);

  auto EOFCallback = [this](uint32_t decodedMs, uint32_t durationMs,
                            size_t streamPosition, size_t streamSize) {
    constexpr uint32_t eofSlackMs = 1000;
    std::string currentTrackUri;
    int currentTrackIndex = playbackState->innerFrame.state.has_playing_track_index
                                ? playbackState->innerFrame.state.playing_track_index
                                : -1;
    if (currentTrackIndex >= 0 &&
        currentTrackIndex < (int)playbackState->remoteTracks.size()) {
      currentTrackUri = playbackState->remoteTracks[(size_t)currentTrackIndex].uri;
    }
    const bool streamAtEnd =
        streamSize > 0 && streamPosition + (4 * 1024) >= streamSize;
    const bool durationReached = durationMs > 0 && decodedMs >= durationMs;
    const bool nearEnd = durationMs > 0 && (durationMs - decodedMs) <= eofSlackMs;
    if (!streamAtEnd && !durationReached && !nearEnd) {
      CSPOT_LOG(error,
                "Rejected non-terminal EOF-driven NEXT decoded=%u duration=%u remaining=%u streamAtEnd=%d streamPos=%u size=%u trackUri=%s",
                (unsigned)decodedMs, (unsigned)durationMs,
                (unsigned)(durationMs > decodedMs ? durationMs - decodedMs : 0),
                streamAtEnd ? 1 : 0,
                (unsigned)streamPosition, (unsigned)streamSize,
                currentTrackUri.c_str());
      lastEofRejectMs = getCurrentTimestamp();
      lastEofRejectTrackUri = currentTrackUri;
      suppressNextPlaybackStart = true;
      trackPlayer->resetState(false);
      return;
    }
    if (durationMs == 0 && !streamAtEnd) {
      CSPOT_LOG(error,
                "Rejected unknown-duration EOF-driven NEXT decoded=%u streamPos=%u size=%u trackUri=%s",
                (unsigned)decodedMs, (unsigned)streamPosition,
                (unsigned)streamSize, currentTrackUri.c_str());
      lastEofRejectMs = getCurrentTimestamp();
      lastEofRejectTrackUri = currentTrackUri;
      suppressNextPlaybackStart = true;
      trackPlayer->resetState(false);
      return;
    }
    if (!streamAtEnd && durationMs > 0 && decodedMs + eofSlackMs < durationMs) {
      lastEofRejectMs = getCurrentTimestamp();
      lastEofRejectTrackUri = currentTrackUri;
      suppressNextPlaybackStart = true;
      CSPOT_LOG(error,
                "Rejected early EOF-driven NEXT decoded=%u duration=%u remaining=%u streamAtEnd=%d streamPos=%u size=%u trackUri=%s",
                (unsigned)decodedMs, (unsigned)durationMs,
                (unsigned)(durationMs - decodedMs), streamAtEnd ? 1 : 0,
                (unsigned)streamPosition, (unsigned)streamSize,
                currentTrackUri.c_str());
      trackPlayer->resetState(false);
      return;
    }
    if (trackQueue->isFinished()) {
      sendEvent(EventType::DEPLETED);
      return;
    }
    CSPOT_LOG(info,
              "EOF-driven NEXT accepted decoded=%u duration=%u streamPos=%u size=%u trackUri=%s",
              (unsigned)decodedMs, (unsigned)durationMs,
              (unsigned)streamPosition, (unsigned)streamSize,
              currentTrackUri.c_str());
    bool advanced = trackQueue->skipTrack(TrackQueue::SkipDirection::NEXT);
    CSPOT_LOG(info, "EOF queue advance decision accepted=%d advanced=%d",
              1, advanced ? 1 : 0);
    if (advanced) {
      sendEvent(EventType::NEXT);
    }
  };

  auto trackLoadedCallback = [this](std::shared_ptr<QueuedTrack> track,
                                    bool paused = false) {
    if (suppressPlaybackStartAfterEofReject(track)) {
      CSPOT_LOG(error,
                "Suppressed playback-start after rejected EOF trackUri=%s sameTrack=1 ageMs=%llu",
                track ? track->identifier.c_str() : "",
                lastEofRejectMs == 0 ? 0 : (getCurrentTimestamp() - lastEofRejectMs));
      return;
    }
    suppressNextPlaybackStart = false;
    CSPOT_LOG(info, "Track loaded callback -> playback-start trackUri=%s requestedPos=%u paused=%d",
              track ? track->identifier.c_str() : "",
              track ? (unsigned)track->requestedPosition : 0, paused ? 1 : 0);

    // Keep this as an internal bridge event so the PCM path can start
    // buffering. The public Spotify notify is sent only after real audio
    // reaches output in notifyAudioReachedPlayback().
    sendEvent(EventType::PLAYBACK_START, (int)track->requestedPosition);
  };

  this->ctx = ctx;
  this->trackPlayer = std::make_shared<TrackPlayer>(
      ctx, trackQueue, EOFCallback, trackLoadedCallback);

  // Subscribe to mercury on session ready
  ctx->session->setConnectedHandler([this]() { this->subscribeToMercury(); });
}

void SpircHandler::subscribeToMercury() {
  auto responseLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;

    sendCmd(MessageType_kMessageTypeHello);
    CSPOT_LOG(debug, "Sent kMessageTypeHello!");

    // Assign country code
    this->ctx->config.countryCode = this->ctx->session->getCountryCode();
  };
  auto subscriptionLambda = [this](MercurySession::Response& res) {
    if (res.fail)
      return;
    CSPOT_LOG(debug, "Received subscription response");

    this->handleFrame(res.parts[0]);
  };

  ctx->session->executeSubscription(
      MercurySession::RequestType::SUB,
      "hm://remote/user/" + ctx->config.username + "/", responseLambda,
      subscriptionLambda);
}

void SpircHandler::loadTrackFromURI(const std::string& uri) {}

void SpircHandler::notifyAudioEnded() {
  playbackState->updatePositionMs(0);
  notify();
  trackPlayer->resetState(true);
}

bool SpircHandler::suppressPlaybackStartAfterEofReject(
    const std::shared_ptr<QueuedTrack>& track) {
  if (!track) return false;
  if (!suppressNextPlaybackStart || lastEofRejectMs == 0 || lastEofRejectTrackUri.empty()) return false;
  unsigned long long now = getCurrentTimestamp();
  if (now < lastEofRejectMs || now - lastEofRejectMs > 3000) return false;
  return track->ref.uri == lastEofRejectTrackUri;
}

std::string SpircHandler::currentPlayingTrackUri() const {
  if (!playbackState || !playbackState->innerFrame.state.has_playing_track_index) {
    return {};
  }
  int idx = playbackState->innerFrame.state.playing_track_index;
  if (idx < 0 || idx >= (int)playbackState->remoteTracks.size()) {
    return {};
  }
  return playbackState->remoteTracks[(size_t)idx].uri;
}

void SpircHandler::notifyAudioReachedPlayback() {
  int offset = 0;

  // get HEAD track
  auto currentTrack = trackQueue->consumeTrack(nullptr, offset);
  if (currentTrack == nullptr) {
    return;
  }

  // A pending notify means the queue was advanced by skip/EOF.  The audio
  // output task is now actually playing that track, so publish its metadata.
  if (trackQueue->notifyPending) {
    trackQueue->notifyPending = false;
  }

  playbackState->updatePositionMs(currentTrack->requestedPosition);
  currentTrack->requestedPosition = 0;
  playbackState->setPlaybackState(PlaybackState::State::Playing);

  this->notify();

  sendEvent(EventType::TRACK_INFO, currentTrack->trackInfo);
  sendEvent(EventType::PLAY_PAUSE, false);
}

void SpircHandler::updatePositionMs(uint32_t position) {
  playbackState->updatePositionMs(position);
  notify();
}

void SpircHandler::disconnect() {
  this->trackQueue->stopTask();
  this->trackPlayer->stop();
  this->ctx->session->disconnect();
}

void SpircHandler::handleFrame(std::vector<uint8_t>& data) {
  // Decode received spirc frame
  playbackState->decodeRemoteFrame(data);

  switch (playbackState->remoteFrame.typ) {
    case MessageType_kMessageTypeNotify: {
      CSPOT_LOG(debug, "Notify frame");

      // Pause the playback if another player took control
      if (playbackState->isActive() &&
          playbackState->remoteFrame.device_state.is_active) {
        CSPOT_LOG(debug, "Another player took control, pausing playback");
        playbackState->setActive(false);

        this->trackPlayer->stop();
        sendEvent(EventType::DISC);
      }
      break;
    }
    case MessageType_kMessageTypeSeek: {
      this->trackPlayer->seekMs(playbackState->remoteFrame.position);

      playbackState->updatePositionMs(playbackState->remoteFrame.position);

      notify();

      sendEvent(EventType::SEEK, (int)playbackState->remoteFrame.position);
      break;
    }
    case MessageType_kMessageTypeVolume:
      noteVolumeActivity();
      playbackState->setVolume(playbackState->remoteFrame.volume);
      this->notify();
      sendEvent(EventType::VOLUME, (int)playbackState->remoteFrame.volume);
      break;
    case MessageType_kMessageTypePause:
      setPause(true);
      break;
    case MessageType_kMessageTypePlay:
      setPause(false);
      break;
    case MessageType_kMessageTypeNext:
      if (suppressRemoteSkipAfterVolume("NEXT")) {
        this->notify();
        break;
      }
      if (suppressUnexpectedRemoteNext()) {
        this->notify();
        break;
      }
      CSPOT_LOG(info, "Remote NEXT accepted pos=%u idx=%d volumeAgeMs=%llu",
                (unsigned)playbackState->innerFrame.state.position_ms,
                (int)playbackState->innerFrame.state.playing_track_index,
                lastVolumeActivityMs == 0 ? 0 : (getCurrentTimestamp() - lastVolumeActivityMs));
      if (nextSong()) {
        sendEvent(EventType::NEXT);
      }
      break;
    case MessageType_kMessageTypePrev:
      if (suppressRemoteSkipAfterVolume("PREV")) {
        this->notify();
        break;
      }
      CSPOT_LOG(info, "Remote PREV accepted pos=%u idx=%d volumeAgeMs=%llu",
                (unsigned)playbackState->innerFrame.state.position_ms,
                (int)playbackState->innerFrame.state.playing_track_index,
                lastVolumeActivityMs == 0 ? 0 : (getCurrentTimestamp() - lastVolumeActivityMs));
      if (previousSong()) {
        sendEvent(EventType::PREV);
      }
      break;
    case MessageType_kMessageTypeLoad: {
      this->trackPlayer->start();

      CSPOT_LOG(info, "Load frame tracks=%u remoteIdx=%d pos=%u localIdx=%d",
                (unsigned)playbackState->remoteTracks.size(),
                (int)playbackState->remoteFrame.state.playing_track_index,
                (unsigned)playbackState->remoteFrame.state.position_ms,
                (int)playbackState->innerFrame.state.playing_track_index);

      if (playbackState->remoteTracks.size() == 0) {
        CSPOT_LOG(info, "No tracks in frame, stopping playback");
        break;
      }

      bool wasActive = playbackState->isActive();
      int remoteIndex = playbackState->remoteFrame.state.playing_track_index;
      uint32_t requestedPosition = playbackState->remoteFrame.state.position_ms;
      bool sameTrackLoad =
          remoteIndex >= 0 &&
          remoteIndex < playbackState->remoteTracks.size() &&
          trackQueue->isCurrentTrack(playbackState->remoteTracks[remoteIndex]);
      if (wasActive && sameTrackLoad && requestedPosition > 0) {
        CSPOT_LOG(info,
                  "Same-track LOAD converted to in-place SEEK pos=%u idx=%d",
                  (unsigned)requestedPosition, remoteIndex);
        this->trackPlayer->seekMs(requestedPosition);
        playbackState->updatePositionMs(requestedPosition);
        playbackState->setPlaybackState(PlaybackState::State::Playing);
        this->notify();
        sendEvent(EventType::SEEK, (int)requestedPosition);
        break;
      }

      playbackState->setActive(true);

      playbackState->updatePositionMs(0);
      playbackState->setPlaybackState(PlaybackState::State::Playing);

      playbackState->syncWithRemote();

      // New-track LOAD frames must not perform expensive initial Vorbis seeks.
      // Same-track LOAD seeks are handled above as in-place SEEK operations.
      trackQueue->updateTracks(0, true);

      this->notify();

      // Stop the current track, if any
      trackPlayer->resetState();
      break;
    }
    case MessageType_kMessageTypeReplace: {
      CSPOT_LOG(info, "Replace frame tracks=%u remoteIdx=%d pos=%u localIdx=%d",
                (unsigned)playbackState->remoteTracks.size(),
                (int)playbackState->remoteFrame.state.playing_track_index,
                (unsigned)playbackState->remoteFrame.state.position_ms,
                (int)playbackState->innerFrame.state.playing_track_index);
      playbackState->syncWithRemote();

      // 1st track is the current one, but update the position
      bool cleared = trackQueue->updateTracks(
          playbackState->remoteFrame.state.position_ms +
              ctx->timeProvider->getSyncedTimestamp() -
              playbackState->innerFrame.state.position_measured_at,
          false);

      this->notify();

      // need to re-load all if streaming track is completed
      if (cleared) {
        sendEvent(EventType::FLUSH);
        trackPlayer->resetState();
      }
      break;
    }
    case MessageType_kMessageTypeShuffle: {
      CSPOT_LOG(debug, "Got shuffle frame");
      this->notify();
      break;
    }
    case MessageType_kMessageTypeRepeat: {
      CSPOT_LOG(debug, "Got repeat frame");
      this->notify();
      break;
    }
    default:
      break;
  }
}

void SpircHandler::setRemoteVolume(int volume) {
  noteVolumeActivity();
  playbackState->setVolume(volume);
  notify();
}

void SpircHandler::notify() {
  this->sendCmd(MessageType_kMessageTypeNotify);
}

bool SpircHandler::skipSong(TrackQueue::SkipDirection dir) {
  if (!acceptSkipCommand()) {
    CSPOT_LOG(info, "Skip command debounced");
    return false;
  }

  bool skipped = trackQueue->skipTrack(dir);
  CSPOT_LOG(info, "Manual skip command dir=%s result=%d",
            dir == TrackQueue::SkipDirection::NEXT ? "NEXT" : "PREV",
            skipped ? 1 : 0);

  // Reset track state
  trackPlayer->resetState(!skipped);

  // send NEXT or PREV event only when successful
  return skipped;
}

bool SpircHandler::nextSong() {
  return skipSong(TrackQueue::SkipDirection::NEXT);
}

bool SpircHandler::previousSong() {
  return skipSong(TrackQueue::SkipDirection::PREV);
}

bool SpircHandler::acceptSkipCommand() {
  unsigned long long now = getCurrentTimestamp();
  if (lastSkipCommandMs > 0 && now >= lastSkipCommandMs &&
      now - lastSkipCommandMs < 350) {
    return false;
  }
  lastSkipCommandMs = now;
  return true;
}

void SpircHandler::noteVolumeActivity() {
  lastVolumeActivityMs = getCurrentTimestamp();
}

bool SpircHandler::suppressRemoteSkipAfterVolume(const char* source) {
  unsigned long long now = getCurrentTimestamp();
  if (lastVolumeActivityMs == 0 || now < lastVolumeActivityMs ||
      now - lastVolumeActivityMs > 15000) {
    return false;
  }

  if (lastVolumeSkipSuppressedLogMs == 0 ||
      now < lastVolumeSkipSuppressedLogMs ||
      now - lastVolumeSkipSuppressedLogMs > 1000) {
    CSPOT_LOG(info, "Suppressed remote %s during volume activity", source);
    lastVolumeSkipSuppressedLogMs = now;
  }
  return true;
}

bool SpircHandler::suppressUnexpectedRemoteNext() {
  uint32_t positionMs =
      playbackState->innerFrame.state.has_position_ms
          ? playbackState->innerFrame.state.position_ms
          : 0;
  if (playbackState->innerFrame.state.has_position_measured_at &&
      playbackState->innerFrame.state.status == PlayStatus_kPlayStatusPlay) {
    uint32_t measuredAt =
        playbackState->innerFrame.state.position_measured_at;
    uint32_t nowSynced = ctx->timeProvider->getSyncedTimestamp();
    if (nowSynced >= measuredAt) {
      positionMs += nowSynced - measuredAt;
    }
  }

  if (positionMs < 15000) {
    lastRemoteNextGuardMs = 0;
    return false;
  }

  unsigned long long now = getCurrentTimestamp();
  if (lastRemoteNextGuardMs > 0 && now >= lastRemoteNextGuardMs &&
      now - lastRemoteNextGuardMs < 2500) {
    CSPOT_LOG(error,
              "Remote NEXT guard bypassed by repeat pos=%u localIdx=%d remoteIdx=%d",
              (unsigned)positionMs,
              (int)playbackState->innerFrame.state.playing_track_index,
              (int)playbackState->remoteFrame.state.playing_track_index);
    lastRemoteNextGuardMs = 0;
    return false;
  }

  lastRemoteNextGuardMs = now;
  CSPOT_LOG(error,
            "Suppressed suspicious remote NEXT pos=%u localIdx=%d remoteIdx=%d",
            (unsigned)positionMs,
            (int)playbackState->innerFrame.state.playing_track_index,
            (int)playbackState->remoteFrame.state.playing_track_index);
  return true;
}

std::shared_ptr<TrackPlayer> SpircHandler::getTrackPlayer() {
  return this->trackPlayer;
}

void SpircHandler::sendCmd(MessageType typ) {
  // Serialize current player state
  auto encodedFrame = playbackState->encodeCurrentFrame(typ);

  auto responseLambda = [=](MercurySession::Response& res) {
  };
  auto parts = MercurySession::DataParts({encodedFrame});
  ctx->session->execute(MercurySession::RequestType::SEND,
                        "hm://remote/user/" + ctx->config.username + "/",
                        responseLambda, parts);
}
void SpircHandler::setEventHandler(EventHandler handler) {
  this->eventHandler = handler;
}

void SpircHandler::setPause(bool isPaused) {
  if (isPaused) {
    CSPOT_LOG(debug, "External pause command");
    playbackState->setPlaybackState(PlaybackState::State::Paused);
  } else {
    CSPOT_LOG(debug, "External play command");

    playbackState->setPlaybackState(PlaybackState::State::Playing);
  }
  notify();
  sendEvent(EventType::PLAY_PAUSE, isPaused);
}

void SpircHandler::sendEvent(EventType type) {
  if (!eventHandler) return;
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = {};
  eventHandler(std::move(event));
}

void SpircHandler::sendEvent(EventType type, EventData data) {
  if (!eventHandler) return;
  auto event = std::make_unique<Event>();
  event->eventType = type;
  event->data = data;
  eventHandler(std::move(event));
}
