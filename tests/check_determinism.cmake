# Runs one scenario three times and verifies the screenshots are pixel-identical.
#
# Required -D variables: HARNESS, SCENARIO, OUT_ROOT, IMAGEDIFF, PYTHON

if(NOT DEFINED HARNESS OR NOT DEFINED SCENARIO OR NOT DEFINED OUT_ROOT OR
   NOT DEFINED IMAGEDIFF OR NOT DEFINED PYTHON)
  message(FATAL_ERROR "check_determinism.cmake is missing required -D arguments")
endif()

file(REMOVE_RECURSE "${OUT_ROOT}")
file(MAKE_DIRECTORY "${OUT_ROOT}")

foreach(run RANGE 1 3)
  set(run_dir "${OUT_ROOT}/run${run}")
  file(MAKE_DIRECTORY "${run_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "SDL_VIDEODRIVER=offscreen"
            "SDL_AUDIODRIVER=dummy"
            "BRICKDESK_HEADLESS=1"
            "${HARNESS}" --scenario "${SCENARIO}" --out-dir "${run_dir}"
    RESULT_VARIABLE harness_result
    OUTPUT_VARIABLE harness_stdout
    ERROR_VARIABLE harness_stderr
  )
  if(NOT harness_result EQUAL 0)
    message("${harness_stdout}")
    message("${harness_stderr}")
    message(FATAL_ERROR "determinism run ${run} failed (exit ${harness_result})")
  endif()
endforeach()

file(GLOB reference_shots "${OUT_ROOT}/run1/*.png")
list(LENGTH reference_shots reference_count)
if(reference_count EQUAL 0)
  message(FATAL_ERROR "determinism scenario produced no screenshots")
endif()

foreach(reference IN LISTS reference_shots)
  get_filename_component(filename "${reference}" NAME)
  foreach(run RANGE 2 3)
    set(candidate "${OUT_ROOT}/run${run}/${filename}")
    if(NOT EXISTS "${candidate}")
      message(FATAL_ERROR "run${run} did not produce ${filename}")
    endif()
    execute_process(
      COMMAND "${PYTHON}" "${IMAGEDIFF}"
              --expected "${reference}"
              --actual "${candidate}"
              --diff "${OUT_ROOT}/run${run}/${filename}.diff.png"
              --mode exact
      RESULT_VARIABLE compare_result
      OUTPUT_VARIABLE compare_stdout
      ERROR_VARIABLE compare_stderr
    )
    if(NOT compare_result EQUAL 0)
      message("${compare_stdout}")
      message("${compare_stderr}")
      message(FATAL_ERROR "${filename} differs between run1 and run${run}")
    endif()
  endforeach()
endforeach()

message(STATUS "determinism: 3 runs identical (${reference_count} screenshot(s))")
