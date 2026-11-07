find_package(GTest REQUIRED)

add_executable(simulator_world_test world_test.cpp)
target_link_libraries(simulator_world_test PRIVATE eos_simulator_world)
add_test(NAME simulator_world_test COMMAND simulator_world_test)

add_executable(demo_raster_map_test raster_map_test.cpp)
target_link_libraries(demo_raster_map_test PRIVATE eos_simulator_world)
add_test(NAME demo_raster_map_test COMMAND demo_raster_map_test)

add_executable(demo_scene_test scene_test.cpp)
target_link_libraries(demo_scene_test PRIVATE eos_simulator_world)
add_test(NAME demo_scene_test COMMAND demo_scene_test)

add_executable(demo_source_media_test source_media_test.cpp)
target_link_libraries(demo_source_media_test PRIVATE eos_simulator_world)
add_test(NAME demo_source_media_test COMMAND demo_source_media_test)
set_tests_properties(demo_source_media_test PROPERTIES ENVIRONMENT "GST_PLUGIN_FEATURE_RANK=nvv4l2decoder:0")

add_executable(simulator_system_camera_source_test system_camera_source_test.cpp)
target_link_libraries(simulator_system_camera_source_test
  PRIVATE eos_simulator_world GTest::gtest GTest::gtest_main
)
add_test(NAME simulator_system_camera_source_test COMMAND simulator_system_camera_source_test)

add_executable(demo_simulation_config_test config_test.cpp)
target_link_libraries(demo_simulation_config_test
  PRIVATE eos_simulator_service GTest::gtest GTest::gtest_main
)
target_compile_definitions(demo_simulation_config_test PRIVATE EOS_SOURCE_ETC="${ISLAND_SOURCE_DIR}/etc/eos")
add_test(NAME demo_simulation_config_test COMMAND demo_simulation_config_test)

add_executable(simulator_storage_test storage_test.cpp)
target_link_libraries(simulator_storage_test PRIVATE eos_simulator_service GTest::gtest GTest::gtest_main)
add_test(NAME simulator_storage_test COMMAND simulator_storage_test)

add_executable(simulator_web_server_test web_server_test.cpp)
target_link_libraries(simulator_web_server_test
  PRIVATE eos_simulator_service GTest::gtest GTest::gtest_main
)
add_test(NAME simulator_web_server_test COMMAND simulator_web_server_test)

add_executable(simulator_ipc_test ipc_test.cpp)
target_link_libraries(simulator_ipc_test
  PRIVATE eos_simulator_ipc_client eos_simulator_ipc_server
          GTest::gtest GTest::gtest_main
)
add_test(NAME simulator_ipc_test COMMAND simulator_ipc_test)
