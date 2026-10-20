find_package(GTest REQUIRED)

add_executable(aplay_speaker_test
  aplay_speaker_test.cpp
)

target_link_libraries(aplay_speaker_test
  PRIVATE eos_internal eos_backend_aplay GTest::gtest_main
)

add_test(NAME aplay_speaker_test COMMAND aplay_speaker_test)
set_tests_properties(aplay_speaker_test PROPERTIES TIMEOUT 30)
