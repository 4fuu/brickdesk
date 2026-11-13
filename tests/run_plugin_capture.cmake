# Runs plugin desktop and settings on/off captures against their baselines.
# Required: CAPTURE, OUT_DIR, BASELINE_DIR, IMAGEDIFF, PYTHON

if(NOT DEFINED CAPTURE OR NOT DEFINED OUT_DIR OR NOT DEFINED BASELINE_DIR OR
   NOT DEFINED IMAGEDIFF OR NOT DEFINED PYTHON)
  message(FATAL_ERROR "run_plugin_capture.cmake is missing required -D arguments")
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
  message(FATAL_ERROR "plugin capture failed (exit ${capture_result})")
endif()

foreach(name IN ITEMS plugin_hello plugin_settings plugin_settings_off)
  set(actual "${OUT_DIR}/${name}.png")
  set(expected "${BASELINE_DIR}/${name}.png")
  if(NOT EXISTS "${actual}")
    message(FATAL_ERROR "plugin capture produced no ${name}.png")
  endif()
  if(NOT EXISTS "${expected}")
    message(FATAL_ERROR "missing baseline ${expected} (run scripts/update-baselines.sh plugin_hello plugin_settings plugin_settings_off)")
  endif()

  set(diff "${OUT_DIR}/${name}.diff.png")
  execute_process(
    COMMAND "${PYTHON}" "${IMAGEDIFF}"
            --expected "${expected}"
            --actual "${actual}"
            --diff "${diff}"
            --mode tolerance
            --max-delta 1
            --max-diff-ratio 0.001
            --min-ssim 0.9999
            --max-cluster 4
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
      COMMAND "${CMAKE_COMMAND}" -E copy "${expected}" "${OUT_DIR}/${name}.expected.png")
    message(FATAL_ERROR "${name}.png does not match its baseline")
  endif()
endforeach()

message(STATUS "plugin capture: plugin screenshots match baselines")
