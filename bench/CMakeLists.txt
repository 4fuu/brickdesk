add_executable(bench_render bench_render.c)
target_link_libraries(bench_render PRIVATE brickdesk_core brickdesk_warnings)

if(BUILD_TESTING)
  add_test(NAME bench_smoke COMMAND bench_render --frames 8 --scenario all)
  set_tests_properties(bench_smoke PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV}"
    TIMEOUT 180
    PASS_REGULAR_EXPRESSION "scenario=pan")
endif()
