add_executable(multiroom_server_tests
    AudioFixtures.cpp
    HttpTestServer.cpp
    TestMain.cpp
    TestSupport.cpp
    test_audio_format.cpp
    test_audio_server.cpp
    test_block_pool.cpp
    test_client_clock_model.cpp
    test_client_output.cpp
    test_client_registry.cpp
    test_client_session.cpp
    test_client_sync.cpp
    test_clock.cpp
    test_commandline.cpp
    test_control_api.cpp
    test_control_server.cpp
    test_decoder.cpp
    test_discovery.cpp
    test_equalizer.cpp
    test_error.cpp
    test_fileutil.cpp
    test_integration.cpp
    test_ids.cpp
    test_logger.cpp
    test_matp_roundtrip.cpp
    test_media_fetch.cpp
    test_media_origin.cpp
    test_media_source.cpp
    test_output_clock.cpp
    test_output_planner.cpp
    test_packetizer.cpp
    test_pcm_store.cpp
    test_processor.cpp
    test_real_resamplers.cpp
    test_resampler.cpp
    test_resampler_registry.cpp
    test_server_config.cpp
    test_source_timeline.cpp
    test_start_scheduler.cpp
    test_system.cpp
    test_units.cpp
    test_zone.cpp
    test_zone_manager.cpp
)

target_link_libraries(multiroom_server_tests PRIVATE multiroom_server_core)
target_include_directories(multiroom_server_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Headers that live next to their source rather than in include/ are reached
# the same way the code under test reaches them: by adding src/ for the test
# binary only.
target_include_directories(multiroom_server_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

multiroom_set_warnings(multiroom_server_tests)
multiroom_copy_ffmpeg_runtime(multiroom_server_tests)

add_test(NAME multiroom_server_tests COMMAND multiroom_server_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND MULTIROOM_FFMPEG_ROOT)
    set_tests_properties(multiroom_server_tests PROPERTIES
        ENVIRONMENT "LD_LIBRARY_PATH=${MULTIROOM_FFMPEG_ROOT}/lib:${CMAKE_BINARY_DIR}/third_party")
endif()
