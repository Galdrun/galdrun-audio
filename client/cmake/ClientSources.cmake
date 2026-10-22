# The client sources every platform compiles, relative to the client root.
# Shared by MULTIROOM_CORE_SOURCES (../CMakeLists.txt) and the ESP32 component
# (../esp32/main/CMakeLists.txt), which add their own platform files; neither
# list globs.
set(MULTIROOM_CLIENT_COMMON_SOURCES
    src/app/ClientCommandHandler.cpp
    src/app/CommandLine.cpp
    src/app/DeviceSelection.cpp
    src/app/InputPump.cpp
    src/app/InputSettings.cpp
    src/app/LoopTiming.cpp
    src/app/SessionHooks.cpp
    src/app/StreamPlayback.cpp
    src/app/TestTone.cpp
    src/audio/AudioBackend.cpp
    src/audio/AudioDeviceCache.cpp
    src/audio/AudioDeviceInfo.cpp
    src/audio/AudioEngine.cpp
    src/audio/AudioRingBuffer.cpp
    src/audio/DriftEstimator.cpp
    src/audio/FormatProbe.cpp
    src/audio/HardwareClock.cpp
    src/audio/HardwareVolume.cpp
    src/audio/IAudioBackend.cpp
    src/audio/PlaybackSupervisor.cpp
    src/audio/RingBufferSource.cpp
    src/audio/TimelineSource.cpp
    src/audio/ToneGenerator.cpp
    src/audio/VolumeControl.cpp
    src/config/ClientConfig.cpp
    src/control/ClientStatus.cpp
    src/control/CommandQueue.cpp
    src/control/ControlHttp.cpp
    src/control/ControlProtocol.cpp
    src/control/ControlServer.cpp
    src/input/InputBackend.cpp
    src/input/InputController.cpp
    src/matp/ClientIdentity.cpp
    src/matp/AudioStream.cpp
    src/matp/TimelineMap.cpp
    src/matp/ClockModel.cpp
    src/matp/Session.cpp
    src/net/ServerConnection.cpp
    src/net/Socket.cpp
    src/util/FileUtil.cpp
    src/util/Logger.cpp
)
