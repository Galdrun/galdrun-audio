add_executable(multiroom_tests
    TestMain.cpp
    TestSupport.cpp
    test_audio_engine.cpp
    test_client_command_handler.cpp
    test_command_queue.cpp
    test_commandline.cpp
    test_config.cpp
    test_control_protocol.cpp
    test_control_server.cpp
    test_device_cache.cpp
    test_es8388_startup.cpp
    ../src/audio/esp32/Es8388.cpp
    test_discovery_beacon.cpp
    test_drift_estimator.cpp
    test_fileutil.cpp
    test_hardware_volume.cpp
    test_input.cpp
    test_json.cpp
    test_logger.cpp
    test_timeline_map.cpp
    test_matp_audio_packet.cpp
    test_matp_audio_stream.cpp
    test_matp_end_to_end.cpp
    test_matp_clock_model.cpp
    test_matp_protocol.cpp
    test_matp_session.cpp
    test_playback_supervisor.cpp
    test_ring_buffer.cpp
    test_server_connection.cpp
    test_stream_playback.cpp
    test_text_util.cpp
    test_timeline_source.cpp
    test_volume_control.cpp
)

target_link_libraries(multiroom_tests PRIVATE multiroom_core)
target_include_directories(multiroom_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The socket wrapper lives in src/ rather than in include/, so the loopback
# tests reach it the same way the backend tests would: by adding src/ for
# themselves only.
target_include_directories(multiroom_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_include_directories(multiroom_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/esp32_stubs")

multiroom_set_warnings(multiroom_tests)

add_test(NAME multiroom_tests COMMAND multiroom_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(multiroom_tests PRIVATE test_network_settings.cpp test_cpu_settings.cpp test_ssh_settings.cpp)
    add_executable(input_discovery_tests test_input_discovery.cpp)
    target_include_directories(input_discovery_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(input_discovery_tests PRIVATE multiroom_core)
    foreach(function fopen opendir readdir open close ioctl)
        target_link_options(input_discovery_tests PRIVATE "-Wl,--wrap=${function}")
    endforeach()
    multiroom_set_warnings(input_discovery_tests)
    add_test(NAME input_discovery_tests COMMAND input_discovery_tests)
    add_executable(alsa_snapshot_tests test_alsa_snapshot.cpp)
    target_include_directories(alsa_snapshot_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(alsa_snapshot_tests PRIVATE multiroom_core)
    foreach(function snd_pcm_wait snd_pcm_writei snd_pcm_status
                     snd_pcm_status_get_delay snd_pcm_status_get_driver_htstamp
                     pthread_setschedparam)
        target_link_options(alsa_snapshot_tests PRIVATE "-Wl,--wrap=${function}")
    endforeach()
    multiroom_set_warnings(alsa_snapshot_tests)
    add_test(NAME alsa_snapshot_tests COMMAND alsa_snapshot_tests)
    set_tests_properties(alsa_snapshot_tests PROPERTIES TIMEOUT 10)
    add_executable(alsa_shutdown_tests test_alsa_shutdown.cpp)
    target_include_directories(alsa_shutdown_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_link_libraries(alsa_shutdown_tests PRIVATE multiroom_core)
    foreach(function snd_pcm_wait snd_pcm_writei snd_pcm_drain snd_pcm_drop)
        target_link_options(alsa_shutdown_tests PRIVATE "-Wl,--wrap=${function}")
    endforeach()
    multiroom_set_warnings(alsa_shutdown_tests)
    add_test(NAME alsa_shutdown_tests COMMAND alsa_shutdown_tests)
    set_tests_properties(alsa_shutdown_tests PROPERTIES TIMEOUT 10)
    add_executable(alsa_rescan_tests test_alsa_rescan.cpp)
    target_link_libraries(alsa_rescan_tests PRIVATE multiroom_core)
    foreach(function snd_card_next snd_ctl_open snd_ctl_close snd_ctl_card_info
                     snd_ctl_card_info_get_id snd_ctl_card_info_get_name
                     snd_ctl_card_info_get_longname snd_ctl_pcm_next_device
                     snd_ctl_pcm_info snd_pcm_info_get_name snd_pcm_info_get_id snd_pcm_open snd_mixer_attach)
        target_link_options(alsa_rescan_tests PRIVATE "-Wl,--wrap=${function}")
    endforeach()
    multiroom_set_warnings(alsa_rescan_tests)
    add_test(NAME alsa_rescan_tests COMMAND alsa_rescan_tests)
    set_tests_properties(alsa_rescan_tests PROPERTIES TIMEOUT 10)
endif()
