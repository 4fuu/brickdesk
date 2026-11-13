# Runs one scenario and compares every produced screenshot with its baseline.
#
# Required -D variables:
#   HARNESS, SCENARIO, OUT_DIR, BASELINE_DIR, IMAGEDIFF, PYTHON

if(NOT DEFINED HARNESS OR NOT DEFINED SCENARIO OR NOT DEFINED OUT_DIR OR
   NOT DEFINED BASELINE_DIR OR NOT DEFINED IMAGEDIFF OR NOT DEFINED PYTHON)
  message(FATAL_ERROR "run_scenario.cmake is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

set(harness_command "${HARNESS}" --scenario "${SCENARIO}" --out-dir "${OUT_DIR}")
if(DEFINED CONFIG_DIR AND CONFIG_DIR)
  list(APPEND harness_command --config-dir "${CONFIG_DIR}")
endif()
if(DEFINED DEMO AND DEMO)
  list(APPEND harness_command --demo "${DEMO}")
endif()
if(DEFINED SHIM AND SHIM)
  list(APPEND harness_command --shim "${SHIM}")
endif()
if(DEFINED INPUTD AND INPUTD)
  list(APPEND harness_command --inputd "${INPUTD}")
endif()
if(DEFINED REPLAY AND REPLAY)
  list(APPEND harness_command --replay "${REPLAY}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "SDL_VIDEODRIVER=offscreen"
          "SDL_AUDIODRIVER=dummy"
          "BRICKDESK_HEADLESS=1"
          ${harness_command}
  RESULT_VARIABLE harness_result
  OUTPUT_VARIABLE harness_stdout
  ERROR_VARIABLE harness_stderr
)
message("${harness_stdout}")
if(harness_stderr)
  message("${harness_stderr}")
endif()
if(NOT harness_result EQUAL 0)
  message(FATAL_ERROR "harness failed for ${SCENARIO} (exit ${harness_result})")
endif()

file(GLOB shots "${OUT_DIR}/*.png")
list(LENGTH shots shot_count)
if(shot_count EQUAL 0)
  message(FATAL_ERROR "scenario ${SCENARIO} produced no screenshots")
endif()

foreach(actual IN LISTS shots)
  get_filename_component(filename "${actual}" NAME)
  get_filename_component(stem "${actual}" NAME_WE)
  set(expected "${BASELINE_DIR}/${filename}")

  if(NOT EXISTS "${expected}")
    message(FATAL_ERROR "missing baseline ${expected} (run scripts/update-baselines.sh)")
  endif()

  # Mesa versions differ by low-bit rounding in SDF/text, blur and scaled
  # cover output.  Mesa 22 (orb) and Mesa 25 (Ubuntu 24.04 CI) can put that
  # drift on whole text lines across the wall and on the launch card, so allow
  # a small fraction of <=3-LSB pixels and a wide cluster; a real layout shift
  # or colour change still fails.
  set(diff "${OUT_DIR}/${stem}.diff.png")
  execute_process(
    COMMAND "${PYTHON}" "${IMAGEDIFF}"
            --expected "${expected}"
            --actual "${actual}"
            --diff "${diff}"
            --mode tolerance
            --max-delta 3
            --max-diff-ratio 0.02
            --min-ssim 0.9999
            --max-cluster 1024
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_stdout
    ERROR_VARIABLE compare_stderr
  )
  message("${compare_stdout}")
  if(compare_stderr)
    message("${compare_stderr}")
  endif()

  if(NOT compare_result EQUAL 0)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy
              "${expected}" "${OUT_DIR}/${stem}.expected.png"
    )
    message(FATAL_ERROR "screenshot ${filename} does not match its baseline")
  endif()
endforeach()

message(STATUS "scenario ${SCENARIO}: ${shot_count} screenshot(s) match baselines")
