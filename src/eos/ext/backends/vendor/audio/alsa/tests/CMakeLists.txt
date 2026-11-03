find_package(GTest REQUIRED)

add_executable(alsa_mic_test
  alsa_mic_test.cpp
)

target_include_directories(alsa_mic_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(alsa_mic_test
  PRIVATE eos_internal eos_backend_alsa GTest::gtest_main
)

add_test(NAME alsa_mic_test COMMAND alsa_mic_test)
set_tests_properties(alsa_mic_test PROPERTIES TIMEOUT 30)
