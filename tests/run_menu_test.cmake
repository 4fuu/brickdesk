# Runs the end-to-end menu shim test and compares its six screenshots.
#
# Required -D variables:
#   MENU_TEST, DEMO, STATE_APP, INPUTD, SHIM, CONFIG_DIR, ASSET_DIR,
#   OUT_DIR, BASELINE_DIR, IMAGEDIFF, PYTHON

foreach(required IN ITEMS MENU_TEST DEMO STATE_APP INPUTD SHIM CONFIG_DIR
                          ASSET_DIR OUT_DIR BASELINE_DIR IMAGEDIFF PYTHON)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "run_menu_test.cmake is missing -D${required}")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "SDL_VIDEODRIVER=offscreen"
          "SDL_AUDIODRIVER=dummy"
          "BRICKDESK_HEADLESS=1"
          "${MENU_TEST}"
          --demo "${DEMO}"
          --state-app "${STATE_APP}"
          --inputd "${INPUTD}"
          --shim "${SHIM}"
          --config "${CONFIG_DIR}"
          --assets "${ASSET_DIR}"
          --out-dir "${OUT_DIR}"
  RESULT_VARIABLE menu_result
  OUTPUT_VARIABLE menu_stdout
  ERROR_VARIABLE menu_stderr
)
message("${menu_stdout}")
if(menu_stderr)
  message("${menu_stderr}")
endif()
if(NOT menu_result EQUAL 0)
  message(FATAL_ERROR "menu test failed (exit ${menu_result})")
endif()

foreach(name IN ITEMS menu_open menu_focus menu_brightness
                     menu_device_readback menu_device_write_failed
                     menu_device_unavailable)
  set(actual "${OUT_DIR}/${name}.png")
  set(expected "${BASELINE_DIR}/${name}.png")
  if(NOT EXISTS "${actual}")
    message(FATAL_ERROR "menu test did not produce ${name}.png")
  endif()
  if(NOT EXISTS "${expected}")
    message(FATAL_ERROR "missing baseline ${expected} (run scripts/update-baselines.sh menu)")
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
      COMMAND "${CMAKE_COMMAND}" -E copy
              "${expected}" "${OUT_DIR}/${name}.expected.png"
    )
    message(FATAL_ERROR "screenshot ${name}.png does not match its baseline")
  endif()
endforeach()

message(STATUS "menu test: screenshots match baselines")
