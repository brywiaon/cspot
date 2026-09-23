idf_component_register(
    SRCS
        "idf_component_stub.cpp"
        "cspot/bell/main/audio-sinks/esp/es8311.c"
    INCLUDE_DIRS
        "cspot/include"
        "cspot/bell/main/io/include"
        "cspot/bell/main/utilities/include"
        "cspot/bell/main/audio-sinks/include"
        "cspot/bell/main/audio-sinks/include/esp"
        "cspot/bell/main/audio-codec/include"
        "cspot/bell/main/audio-dsp/include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_CODEC_AAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3 ON CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS OFF CACHE BOOL "" FORCE)
set(BELL_SINK_ALSA OFF)
set(BELL_SINK_PORTAUDIO OFF)

add_subdirectory(cspot)
target_link_libraries(${COMPONENT_LIB} INTERFACE cspot)

