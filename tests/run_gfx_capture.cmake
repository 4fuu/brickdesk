# Runs the primitive capture program and compares every PNG with its baseline.
# Required: CAPTURE, OUT_DIR, BASELINE_DIR, IMAGEDIFF, PYTHON

if(NOT DEFINED CAPTURE OR NOT DEFINED OUT_DIR OR NOT DEFINED BASELINE_DIR OR
   NOT DEFINED IMAGEDIFF OR NOT DEFINED PYTHON)
  message(FATAL_ERROR "run_gfx_capture.cmake is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "SDL_VIDEODRIVER=offscreen"
          "SDL_AUDIODRIVER=dummy"
          "BRICKDESK_HEADLESS=1"
          "${CAPTURE}" "${OUT_DIR}"
  RESULT_VARIABLE capture_result
  OUTPUT_VARIABLE capture_stdout
  ERROR_VARIABLE capture_stderr
)
message("${capture_stdout}")
if(capture_stderr)
  message("${capture_stderr}")
endif()
if(NOT capture_result EQUAL 0)
  message(FATAL_ERROR "gfx capture failed (exit ${capture_result})")
endif()

file(GLOB shots "${OUT_DIR}/*.png")
list(LENGTH shots shot_count)
if(shot_count EQUAL 0)
  message(FATAL_ERROR "gfx capture produced no screenshots")
endif()

foreach(actual IN LISTS shots)
  get_filename_component(filename "${actual}" NAME)
  get_filename_component(stem "${actual}" NAME_WE)
  set(expected "${BASELINE_DIR}/${filename}")
  if(NOT EXISTS "${expected}")
    message(FATAL_ERROR "missing baseline ${expected} (run scripts/update-baselines.sh)")
  endif()

  # Blur sampling differs by 1 LSB across Mesa versions; primitives, text and
  # tile captures still compare at max-delta 1 with a tight SSIM floor.
  set(diff "${OUT_DIR}/${stem}.diff.png")
  execute_process(
    COMMAND "${PYTHON}" "${IMAGEDIFF}"
            --expected "${expected}"
            --actual "${actual}"
            --diff "${diff}"
            --mode tolerance
            --max-delta 1
            --max-diff-ratio 0.10
            --min-ssim 0.9995
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

message(STATUS "gfx capture: ${shot_count} screenshot(s) match baselines")
