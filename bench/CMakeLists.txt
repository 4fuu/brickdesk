add_executable(bench_render bench_render.c)
target_link_libraries(bench_render PRIVATE brickdesk_core bd_inih brickdesk_warnings)
target_compile_definitions(bench_render PRIVATE
  BRICKDESK_BACKEND_NAME="${BRICKDESK_BACKEND}")

set(BENCH_HEADLESS_ENV
  "SDL_VIDEODRIVER=offscreen"
  "SDL_AUDIODRIVER=dummy"
  "BRICKDESK_HEADLESS=1"
)

if(BUILD_TESTING)
  add_test(NAME bench_smoke
    COMMAND bench_render --frames 8 --idle-ms 500 --scenario all)
  set_tests_properties(bench_smoke PROPERTIES
    ENVIRONMENT "${BENCH_HEADLESS_ENV}"
    TIMEOUT 180
    PASS_REGULAR_EXPRESSION "scenario=pan")

  set(BENCH_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/tests/artifacts")
  file(MAKE_DIRECTORY "${BENCH_ARTIFACT_DIR}")
  add_test(NAME bench_budget
    COMMAND bench_render
      --frames 20
      --idle-ms 3000
      --scenario all
      --budget "${CMAKE_CURRENT_SOURCE_DIR}/budgets.ini"
      --json "${BENCH_ARTIFACT_DIR}/bench.json")
  set_tests_properties(bench_budget PROPERTIES
    ENVIRONMENT "${BENCH_HEADLESS_ENV}"
    TIMEOUT 300
    PASS_REGULAR_EXPRESSION "budget: ok=1")
endif()
