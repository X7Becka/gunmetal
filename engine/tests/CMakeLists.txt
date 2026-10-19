add_executable(gm_tests test_main.cpp)
target_link_libraries(gm_tests PRIVATE gm_engine)

add_test(NAME core COMMAND gm_tests)

add_test(NAME cli_version COMMAND gunmetal --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "gunmetal 0.1.0")

add_test(NAME cli_headless COMMAND gunmetal --headless --frames 2)
set_tests_properties(cli_headless PROPERTIES PASS_REGULAR_EXPRESSION "frames=2")

add_test(NAME cli_dump COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
set_tests_properties(cli_dump PROPERTIES PASS_REGULAR_EXPRESSION "size: [0-9]+ bytes")

add_test(NAME cli_dump_missing COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/missing.bin)
set_tests_properties(cli_dump_missing PROPERTIES WILL_FAIL TRUE)
