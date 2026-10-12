#pragma once

#include <stdint.h>    // for uint32_t, uint8_t
#include <functional>  // for function
#include <memory>      // for shared_ptr, unique_ptr
#include <mutex>       // for recursive_mutex
#include <string>      // for string
#include <variant>     // for variant
#include <vector>      // for vector

#include "CDNAudioFile.h"  // for CDNTrackStream, CDNTrackStream::Track...
#include "TrackQueue.h"
#include "protobuf/spirc.pb.h"  // for MessageType

namespace cspot {
class TrackPlayer;
struct Context;

class SpircHandler {
 public:
  SpircHandler(std::shared_ptr<cspot::Context> ctx);
  ~SpircHandler();

  enum class EventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
    DEPLETED,
    FLUSH,
    PLAYBACK_START
  };

  struct PlaybackStartData {
    int positionMs = 0;
    uint64_t trackInstanceId = 0;
    std::string trackUri;
  };

  typedef std::variant<TrackInfo, PlaybackStartData, int, bool> EventData;

  struct Event {
    EventType eventType;
    EventData data;
  };

  typedef std::function<void(std::unique_ptr<Event>)> EventHandler;
  typedef std::function<uint32_t()> PositionProvider;

  void subscribeToMercury();
  std::shared_ptr<TrackPlayer> getTrackPlayer();

  void setEventHandler(EventHandler handler);
  void setPositionProvider(PositionProvider provider);

  void setPause(bool pause);

  bool previousSong();

  bool nextSong();

  void notifyAudioReachedPlayback();
  void notifyAudioEnded(uint32_t finalPositionMs);
  void updatePositionMs(uint32_t position);
  void setRemoteVolume(int volume);
  uint64_t loadTrackFromURI(const std::string& uri);
  std::shared_ptr<cspot::TrackQueue> getTrackQueue() { return trackQueue; }

  void disconnect();

 private:
  struct CallbackGate {
    explicit CallbackGate(SpircHandler* owner) : owner(owner) {}

    std::mutex mutex;
    SpircHandler* owner;
  };

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<CallbackGate> callbackGate;
  std::shared_ptr<cspot::TrackPlayer> trackPlayer;
  std::shared_ptr<cspot::TrackQueue> trackQueue;

  EventHandler eventHandler = nullptr;
  PositionProvider positionProvider = nullptr;

  std::shared_ptr<cspot::PlaybackState> playbackState;
  unsigned long long lastVolumeActivityMs = 0;
  unsigned long long lastEofRejectMs = 0;
  std::string lastEofRejectTrackUri;
  bool suppressNextPlaybackStart = false;
  std::recursive_mutex transitionMutex;

  void sendCmd(MessageType typ);

  void sendEvent(EventType type);
  void sendEvent(EventType type, EventData data);

  bool skipSong(TrackQueue::SkipDirection dir);
  void noteVolumeActivity();
  bool suppressPlaybackStartAfterEofReject(const std::shared_ptr<QueuedTrack>& track);
  std::string currentPlayingTrackUri() const;
  void handleTrackEof(uint32_t decodedMs, uint32_t durationMs,
                      size_t streamPosition, size_t streamSize);
  void handleTrackLoaded(std::shared_ptr<QueuedTrack> track, bool paused);
  void handleFrame(std::vector<uint8_t>& data);
  void notify();
};
}  // namespace cspot
