find_package(GTest REQUIRED)

add_executable(backend_config_tests config_test.cpp)
target_link_libraries(backend_config_tests PRIVATE eos_hardware eos_internal GTest::gtest GTest::gtest_main)
add_test(NAME backend_config_tests COMMAND backend_config_tests)

add_executable(socket_backend_tests socket_backend_test.cpp)
target_link_libraries(socket_backend_tests PRIVATE eos_backend_socket eos_internal GTest::gtest_main)
add_test(NAME socket_backend_tests COMMAND socket_backend_tests)
