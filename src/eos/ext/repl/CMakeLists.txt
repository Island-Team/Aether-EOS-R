# Northbound REPL client (eos-repl) — standalone HTTP/SSE client for the
# external adapter. It never connects to eosd directly.
option(EOS_REPL "Build the northbound REPL client (eos-repl)" ON)

if(EOS_REPL)
  find_package(Threads REQUIRED)

  # Pure logic: input parsing and output rendering, no sockets — the tests
  # link only this.
  add_library(eos_repl_core
    auto_base.cpp
    catalog.cpp
    command.cpp
    completion.cpp
    format.cpp
    history.cpp
    json_input.cpp
    options.cpp
  )
  target_include_directories(eos_repl_core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(eos_repl_core
    PUBLIC nlohmann_json::nlohmann_json eos_lib
    PRIVATE tomlplusplus::tomlplusplus
  )

  # The client itself: HTTP/SSE transport plus the prompt loop.
  add_executable(eos_repl
    session.cpp
    terminal.cpp
    main.cpp
  )
  set_target_properties(eos_repl PROPERTIES OUTPUT_NAME eos-repl)
  target_include_directories(eos_repl
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(eos_repl
    PRIVATE eos_repl_core eos_lib Boost::boost replxx::replxx Threads::Threads
  )

  if(ISLAND_BUILD_TESTS)
    add_subdirectory(tests)
  endif()
endif()
