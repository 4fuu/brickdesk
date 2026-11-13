set(BD_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/tests/artifacts")
file(MAKE_DIRECTORY "${BD_ARTIFACT_DIR}")

set(BD_HEADLESS_ENV
  "SDL_VIDEODRIVER=offscreen"
  "SDL_AUDIODRIVER=dummy"
  "BRICKDESK_HEADLESS=1"
)

add_executable(bd_smoke_render smoke_render.c)
target_link_libraries(bd_smoke_render PRIVATE brickdesk_core brickdesk_warnings)

add_executable(bd_key_names_test key_names_test.c)
target_link_libraries(bd_key_names_test PRIVATE brickdesk_core brickdesk_warnings)

add_executable(bd_config_test config_test.c)
target_link_libraries(bd_config_test PRIVATE brickdesk_config brickdesk_warnings)

add_executable(bd_theme_test theme_test.c)
target_link_libraries(bd_theme_test PRIVATE brickdesk_core brickdesk_warnings)

add_executable(bd_dirty_regions dirty_regions.c)
target_link_libraries(bd_dirty_regions PRIVATE brickdesk_core brickdesk_warnings)

add_executable(bd_wall_scale wall_scale.c)
target_link_libraries(bd_wall_scale PRIVATE brickdesk_core brickdesk_warnings)

add_executable(bd_sysinfo_test sysinfo_test.c)
target_link_libraries(bd_sysinfo_test PRIVATE brickdesk_sysinfo brickdesk_warnings)

add_executable(bd_home_scale home_scale.c)
target_link_libraries(bd_home_scale PRIVATE brickdesk_home brickdesk_core brickdesk_config brickdesk_warnings)

add_executable(bd_home_capture home_capture.c)
target_link_libraries(bd_home_capture PRIVATE brickdesk_home brickdesk_core brickdesk_config brickdesk_warnings)

add_executable(bd_home_plugin_capture home_plugin_capture.c)
target_link_libraries(bd_home_plugin_capture PRIVATE
  brickdesk_shell
  brickdesk_plugin
  brickdesk_config
  brickdesk_warnings)
target_compile_definitions(bd_home_plugin_capture PRIVATE
  BRICKDESK_TEST_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config"
  BRICKDESK_TEST_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins")
add_dependencies(bd_home_plugin_capture bd_hello_plugin)

add_executable(bd_gfx_capture gfx_capture.c)
target_link_libraries(bd_gfx_capture PRIVATE brickdesk_core brickdesk_warnings)
target_compile_definitions(bd_gfx_capture PRIVATE
  BRICKDESK_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

add_executable(bd_hotkey_test hotkey_test.c)
target_link_libraries(bd_hotkey_test PRIVATE brickdesk_input brickdesk_config brickdesk_warnings)

add_executable(bd_perf_policy_test perf_policy_test.c)
target_link_libraries(bd_perf_policy_test PRIVATE brickdesk_perf brickdesk_warnings)
target_compile_definitions(bd_perf_policy_test PRIVATE
  BRICKDESK_PERF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_executable(bd_device_power_test device_power_test.c)
target_link_libraries(bd_device_power_test PRIVATE brickdesk_power brickdesk_warnings)
target_compile_definitions(bd_device_power_test PRIVATE
  BRICKDESK_POWER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/power")

add_executable(bd_device_battery_test device_battery_test.c)
target_link_libraries(bd_device_battery_test PRIVATE brickdesk_battery brickdesk_warnings)
target_compile_definitions(bd_device_battery_test PRIVATE
  BRICKDESK_BATTERY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/battery")

add_executable(bd_plugin_fixture plugin_fixture.c)
target_include_directories(bd_plugin_fixture PRIVATE
  "${PROJECT_SOURCE_DIR}/src/plugin")
target_link_libraries(bd_plugin_fixture PRIVATE brickdesk_plugin brickdesk_warnings)

add_executable(bd_plugin_capture plugin_capture.c)
target_link_libraries(bd_plugin_capture PRIVATE brickdesk_core brickdesk_plugin brickdesk_warnings)
target_compile_definitions(bd_plugin_capture PRIVATE
  BRICKDESK_TEST_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins")
add_dependencies(bd_plugin_capture bd_hello_plugin)

add_executable(bd_plugin_test plugin_test.c)
target_link_libraries(bd_plugin_test PRIVATE brickdesk_core brickdesk_plugin brickdesk_warnings)
target_compile_definitions(bd_plugin_test PRIVATE
  BRICKDESK_TEST_FIXTURE="$<TARGET_FILE:bd_plugin_fixture>"
  BRICKDESK_TEST_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins")
add_dependencies(bd_plugin_test bd_plugin_fixture bd_hello_plugin)

add_executable(bd_settings_test settings_test.c)
target_link_libraries(bd_settings_test PRIVATE brickdesk_settings brickdesk_warnings)
target_compile_definitions(bd_settings_test PRIVATE
  BRICKDESK_TEST_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config"
  BRICKDESK_TEST_FIXTURE="$<TARGET_FILE:bd_plugin_fixture>")
add_dependencies(bd_settings_test bd_plugin_fixture)

add_executable(bd_device_av_test device_av_test.c)
target_include_directories(bd_device_av_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/device")
target_link_libraries(bd_device_av_test PRIVATE brickdesk_device brickdesk_warnings)

add_executable(bd_device_feedback_test device_feedback_test.c)
target_include_directories(bd_device_feedback_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src/device")
target_link_libraries(bd_device_feedback_test PRIVATE brickdesk_feedback brickdesk_warnings)

add_executable(bd_settings_capture settings_capture.c)
target_link_libraries(bd_settings_capture PRIVATE brickdesk_settings brickdesk_warnings)
target_compile_definitions(bd_settings_capture PRIVATE
  BRICKDESK_TEST_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config"
  BRICKDESK_TEST_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins"
  BRICKDESK_TEST_FIXTURE="$<TARGET_FILE:bd_plugin_fixture>")
add_dependencies(bd_settings_capture bd_hello_plugin bd_plugin_fixture)

# Scripted key sequence used to record the review clip; not a CTest.
add_executable(bd_settings_demo settings_demo.c)
target_link_libraries(bd_settings_demo PRIVATE brickdesk_settings brickdesk_warnings)
target_compile_definitions(bd_settings_demo PRIVATE
  BRICKDESK_TEST_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config"
  BRICKDESK_TEST_FIXTURE="$<TARGET_FILE:bd_plugin_fixture>")
add_dependencies(bd_settings_demo bd_plugin_fixture)

if(BRICKDESK_BUILD_HARNESS)
  add_executable(bd_harness
    harness/main.c
    harness/scenario.c
  )
  target_include_directories(bd_harness PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/harness"
    "${PROJECT_SOURCE_DIR}/third_party/jsmn"
  )
  target_link_libraries(bd_harness PRIVATE brickdesk_shell brickdesk_config brickdesk_warnings)
endif()

if(CMAKE_CROSSCOMPILING)
  message(STATUS "Cross build: test executables are built but not registered with CTest")
  return()
endif()

add_test(NAME key_names COMMAND bd_key_names_test)
add_test(NAME sysinfo COMMAND bd_sysinfo_test)
add_test(NAME config COMMAND bd_config_test)
add_test(NAME hotkeys COMMAND bd_hotkey_test)
add_test(NAME perf_policy COMMAND bd_perf_policy_test)
add_test(NAME device_power COMMAND bd_device_power_test)
add_test(NAME device_battery COMMAND bd_device_battery_test)
set_tests_properties(device_battery PROPERTIES TIMEOUT 120)
add_test(NAME device_battery_cli
  COMMAND bd-battery --root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/battery/full")
set_tests_properties(device_battery_cli PROPERTIES
  PASS_REGULAR_EXPRESSION "battery.capacity=73"
  TIMEOUT 60)
add_test(NAME plugin COMMAND bd_plugin_test)
set_tests_properties(plugin PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 120)
add_test(NAME config_check
  COMMAND brickdesk-config check --config-dir "${PROJECT_SOURCE_DIR}/config")
add_test(NAME input_check
  COMMAND bd-input --check --config-dir "${PROJECT_SOURCE_DIR}/config")

add_test(NAME theme COMMAND bd_theme_test)

add_test(NAME dirty_regions COMMAND bd_dirty_regions)
set_tests_properties(dirty_regions PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 120)

add_test(NAME wall_scale COMMAND bd_wall_scale)
set_tests_properties(wall_scale PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 120)

add_test(NAME home_scale COMMAND bd_home_scale)
set_tests_properties(home_scale PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 300)

add_test(NAME settings COMMAND bd_settings_test)
set_tests_properties(settings PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 300)

add_test(NAME device_av COMMAND bd_device_av_test)
set_tests_properties(device_av PROPERTIES TIMEOUT 120)

add_test(NAME device_feedback COMMAND bd_device_feedback_test)
set_tests_properties(device_feedback PROPERTIES TIMEOUT 120)

add_test(NAME smoke_render
  COMMAND bd_smoke_render "${BD_ARTIFACT_DIR}/smoke.png")
set_tests_properties(smoke_render PROPERTIES ENVIRONMENT "${BD_HEADLESS_ENV}")

add_test(NAME version COMMAND brickdesk --version)
set_tests_properties(version PROPERTIES
  PASS_REGULAR_EXPRESSION "brickdesk [0-9]+[.][0-9]+[.][0-9]+ [(]host[)]")

add_test(NAME idle_loop COMMAND brickdesk --idle-check 500)
set_tests_properties(idle_loop PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  PASS_REGULAR_EXPRESSION "idle: duration_ms=500 presents=0 ")

add_test(NAME idle_loop_full COMMAND brickdesk --idle-check 3000)
set_tests_properties(idle_loop_full PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 60
  PASS_REGULAR_EXPRESSION "idle: duration_ms=3000 presents=0 ")

add_test(NAME config_dir_override
  COMMAND brickdesk --idle-check 100 --config-dir "${PROJECT_SOURCE_DIR}/config")
set_tests_properties(config_dir_override PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  PASS_REGULAR_EXPRESSION "idle: duration_ms=100 presents=0 ")

if(BRICKDESK_BUILD_HARNESS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import PIL, numpy"
    RESULT_VARIABLE BD_PYTHON_IMPORT_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(NOT BD_PYTHON_IMPORT_RESULT EQUAL 0)
    message(WARNING
      "Python modules Pillow and numpy are missing; install python3-pil and python3-numpy "
      "to run screenshot and image-diff tests")
  endif()

  set(BD_BASELINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/baselines/host")

  foreach(scenario_name IN ITEMS boot focus_move hint_hold)
    set(scenario_file "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/${scenario_name}.json")
    add_test(NAME "scenario.${scenario_name}"
      COMMAND "${CMAKE_COMMAND}"
        "-DHARNESS=$<TARGET_FILE:bd_harness>"
        "-DSCENARIO=${scenario_file}"
        "-DOUT_DIR=${BD_ARTIFACT_DIR}/${scenario_name}"
        "-DBASELINE_DIR=${BD_BASELINE_DIR}"
        "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
        "-DPYTHON=${Python3_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_scenario.cmake")
    set_tests_properties("scenario.${scenario_name}" PROPERTIES
      TIMEOUT 120
      FAIL_REGULAR_EXPRESSION "FAIL"
    )
  endforeach()

  add_test(NAME scenario.launch_menu
    COMMAND "${CMAKE_COMMAND}"
      "-DHARNESS=$<TARGET_FILE:bd_harness>"
      "-DSCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/scenarios/launch_menu.json"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/launch_menu"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      "-DCONFIG_DIR=${PROJECT_SOURCE_DIR}/config"
      "-DDEMO=$<TARGET_FILE:brickdesk-demo>"
      "-DSHIM=$<TARGET_FILE:brickdesk-menu>"
      "-DINPUTD=$<TARGET_FILE:brickdesk-inputd>"
      "-DREPLAY=${CMAKE_CURRENT_SOURCE_DIR}/scenarios/launch_menu.replay"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_scenario.cmake")
  set_tests_properties(scenario.launch_menu PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV};BRICKDESK_TEST_MEMORY_MB=132"
    TIMEOUT 180
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME determinism_boot
    COMMAND "${CMAKE_COMMAND}"
      "-DHARNESS=$<TARGET_FILE:bd_harness>"
      "-DSCENARIO=${CMAKE_CURRENT_SOURCE_DIR}/scenarios/boot.json"
      "-DOUT_ROOT=${BD_ARTIFACT_DIR}/determinism_boot"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/check_determinism.cmake")
  set_tests_properties(determinism_boot PROPERTIES TIMEOUT 120)

  add_test(NAME imagediff_unit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_imagediff.py"
            --imagediff "${PROJECT_SOURCE_DIR}/tools/imagediff.py")
  set_tests_properties(imagediff_unit PROPERTIES TIMEOUT 120)

  add_test(NAME plugin_capture
    COMMAND "${CMAKE_COMMAND}"
      "-DCAPTURE=$<TARGET_FILE:bd_plugin_capture>"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/plugin_capture"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_plugin_capture.cmake")
  set_tests_properties(plugin_capture PROPERTIES
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME gfx_capture
    COMMAND "${CMAKE_COMMAND}"
      "-DCAPTURE=$<TARGET_FILE:bd_gfx_capture>"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/gfx_capture"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gfx_capture.cmake")
  set_tests_properties(gfx_capture PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV}"
    TIMEOUT 180
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME home_capture
    COMMAND "${CMAKE_COMMAND}"
      "-DCAPTURE=$<TARGET_FILE:bd_home_capture>"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/home_capture"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gfx_capture.cmake")
  set_tests_properties(home_capture PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV}"
    TIMEOUT 300
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME home_plugin_capture
    COMMAND "${CMAKE_COMMAND}"
      "-DCAPTURE=$<TARGET_FILE:bd_home_plugin_capture>"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/home_plugin_capture"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gfx_capture.cmake")
  set_tests_properties(home_plugin_capture PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV}"
    TIMEOUT 180
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME settings_capture
    COMMAND "${CMAKE_COMMAND}"
      "-DCAPTURE=$<TARGET_FILE:bd_settings_capture>"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/settings_capture"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_gfx_capture.cmake")
  set_tests_properties(settings_capture PROPERTIES
    ENVIRONMENT "${BD_HEADLESS_ENV}"
    TIMEOUT 300
    FAIL_REGULAR_EXPRESSION "FAIL")

  add_test(NAME menu
    COMMAND "${CMAKE_COMMAND}"
      "-DMENU_TEST=$<TARGET_FILE:bd_menu_test>"
      "-DDEMO=$<TARGET_FILE:brickdesk-demo>"
      "-DSTATE_APP=$<TARGET_FILE:bd_menu_state_app>"
      "-DINPUTD=$<TARGET_FILE:brickdesk-inputd>"
      "-DSHIM=$<TARGET_FILE:brickdesk-menu>"
      "-DCONFIG_DIR=${PROJECT_SOURCE_DIR}/config"
      "-DASSET_DIR=${PROJECT_SOURCE_DIR}/assets"
      "-DOUT_DIR=${BD_ARTIFACT_DIR}/menu"
      "-DBASELINE_DIR=${BD_BASELINE_DIR}"
      "-DIMAGEDIFF=${PROJECT_SOURCE_DIR}/tools/imagediff.py"
      "-DPYTHON=${Python3_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_menu_test.cmake")
  set_tests_properties(menu PROPERTIES
    TIMEOUT 180
    FAIL_REGULAR_EXPRESSION "FAIL")
endif()

add_executable(bd_session_child session_child.c)
target_include_directories(bd_session_child PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/jsmn")
target_link_libraries(bd_session_child PRIVATE brickdesk_warnings)

add_executable(bd_session_test session_test.c)
target_link_libraries(bd_session_test PRIVATE brickdesk_session brickdesk_warnings)

add_executable(bd_shell_quota_test shell_quota_test.c)
target_link_libraries(bd_shell_quota_test PRIVATE
  brickdesk_shell
  brickdesk_config
  brickdesk_warnings)
target_compile_definitions(bd_shell_quota_test PRIVATE
  BRICKDESK_TEST_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config")

add_executable(bd_menu_state_app menu_state_app.c)
target_include_directories(bd_menu_state_app PRIVATE "${BRICKDESK_GLES2_INCLUDE_DIR}")
target_link_libraries(bd_menu_state_app PRIVATE
  ${BRICKDESK_SDL2_TARGET}
  ${BRICKDESK_GLES2_LIBRARY}
  ${CMAKE_DL_LIBS}
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>
  brickdesk_warnings)

add_executable(bd_menu_test menu_test.c)
target_link_libraries(bd_menu_test PRIVATE brickdesk_session brickdesk_warnings)

add_test(NAME session
  COMMAND bd_session_test $<TARGET_FILE:brickdesk-demo> $<TARGET_FILE:bd_session_child>)
set_tests_properties(session PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 180)

add_test(NAME shell_quota
  COMMAND bd_shell_quota_test $<TARGET_FILE:bd_session_child>)
set_tests_properties(shell_quota PROPERTIES
  ENVIRONMENT "${BD_HEADLESS_ENV}"
  TIMEOUT 180)
