cmake_minimum_required(VERSION 3.16)
project(sdrconnect_classifier VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(HFC_BUILD_TOOLS "Build the command line tools" ON)
option(HFC_BUILD_TESTS "Build the test suite" ON)

if(MSVC)
  # Static runtime: the tools run without a VC++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

find_package(Threads REQUIRED)

# --- Third-party code (third_party/README.md) and the decoders copied from
# the SDRuno plugin repositories (decoders/README.md). Their warnings are
# theirs: compiled without the strict warning flags above.
add_library(ft8lib STATIC
  third_party/ft8_lib/ft8/constants.c
  third_party/ft8_lib/ft8/crc.c
  third_party/ft8_lib/ft8/decode.c
  third_party/ft8_lib/ft8/encode.c
  third_party/ft8_lib/ft8/ldpc.c
  third_party/ft8_lib/ft8/message.c
  third_party/ft8_lib/ft8/text.c
  third_party/ft8_lib/common/monitor.c
  third_party/ft8_lib/fft/kiss_fft.c
  third_party/ft8_lib/fft/kiss_fftr.c
)
target_include_directories(ft8lib PUBLIC third_party/ft8_lib)
set_target_properties(ft8lib PROPERTIES COMPILE_OPTIONS "")
if(NOT WIN32)
  target_link_libraries(ft8lib PUBLIC m)
endif()

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
set_target_properties(sqlite3 PROPERTIES COMPILE_OPTIONS "")
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
target_link_libraries(sqlite3 PUBLIC Threads::Threads)

file(GLOB NAVTEX_SOURCES decoders/navtex/src/*.cpp)
add_library(navtex_core STATIC ${NAVTEX_SOURCES})
target_include_directories(navtex_core PUBLIC decoders/navtex/include)
set_target_properties(navtex_core PROPERTIES COMPILE_OPTIONS "")
file(GLOB MORSE_SOURCES decoders/morse/src/*.cpp)
add_library(morse_core STATIC ${MORSE_SOURCES})
target_include_directories(morse_core PUBLIC decoders/morse/include)
set_target_properties(morse_core PROPERTIES COMPILE_OPTIONS "")
add_library(pskdsp STATIC
  decoders/psk/PskDecoder.cpp
  decoders/psk/PskModulator.cpp
  decoders/psk/Spectrum.cpp
  decoders/psk/TextDecoder.cpp
  decoders/psk/Varicode.cpp
)
target_include_directories(pskdsp PUBLIC decoders/psk)
set_target_properties(pskdsp PROPERTIES COMPILE_OPTIONS "")

# The mode catalogue (catalogue/modes.json), embedded for the matcher.
set(HFC_CATALOGUE_CPP ${CMAKE_CURRENT_BINARY_DIR}/catalogue_data.cpp)
add_custom_command(
  OUTPUT ${HFC_CATALOGUE_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/catalogue/modes.json -DOUTPUT=${HFC_CATALOGUE_CPP}
          -DFUNCTION=hfc::catalogueJson -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS catalogue/modes.json cmake/embed.cmake
  COMMENT "Embedding the mode catalogue")

# The learned family model (catalogue/family_model.txt), embedded.
set(HFC_FAMILY_MODEL_CPP ${CMAKE_CURRENT_BINARY_DIR}/family_model_data.cpp)
add_custom_command(
  OUTPUT ${HFC_FAMILY_MODEL_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/catalogue/family_model.txt -DOUTPUT=${HFC_FAMILY_MODEL_CPP}
          -DFUNCTION=hfc::familyModelText -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS catalogue/family_model.txt cmake/embed.cmake
  COMMENT "Embedding the family model")

# EiBi's schedule (catalogue/eibi), embedded: the built-in frequency database.
set(HFC_EIBI_CPP ${CMAKE_CURRENT_BINARY_DIR}/eibi_data.cpp)
add_custom_command(
  OUTPUT ${HFC_EIBI_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/catalogue/eibi/sked-b26.csv -DOUTPUT=${HFC_EIBI_CPP}
          -DFUNCTION=hfc::eibiScheduleText -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS catalogue/eibi/sked-b26.csv cmake/embed.cmake
  COMMENT "Embedding the EiBi schedule")

# Portable core: no dependencies beyond the C++17 standard library and the
# platform's sockets.
add_library(hfc_core STATIC
  ${HFC_CATALOGUE_CPP}
  ${HFC_FAMILY_MODEL_CPP}
  core/src/family_model.cpp
  ${HFC_EIBI_CPP}
  core/src/catalogue.cpp
  core/src/frequency_list.cpp
  core/src/dsc.cpp
  core/src/hfdl.cpp
  core/src/fax.cpp
  core/src/sstv.cpp
  core/src/timesig.cpp
  core/src/wspr.cpp
  core/src/drm.cpp
  core/src/http_get.cpp
  core/src/akula.cpp
  core/src/constant_weight.cpp
  core/src/channelizer.cpp
  core/src/client.cpp
  core/src/codec.cpp
  core/src/decoder_host.cpp
  core/src/detector.cpp
  core/src/dsp.cpp core/src/features.cpp
  core/src/file.cpp
  core/src/ftx_decoder.cpp
  core/src/iq_clock.cpp
  core/src/json.cpp
  core/src/net.cpp
  core/src/pipeline.cpp
  core/src/probe.cpp
  core/src/zip.cpp
  core/src/worker_pool.cpp
  core/src/sdrconnect.cpp
  core/src/sigmf.cpp
  core/src/store.cpp
  core/src/web_server.cpp
  core/src/websocket.cpp
)
target_include_directories(hfc_core PUBLIC core/include)
target_link_libraries(hfc_core PUBLIC Threads::Threads ft8lib sqlite3 navtex_core morse_core pskdsp)
if(WIN32)
  target_link_libraries(hfc_core PUBLIC ws2_32 shell32)
endif()
target_compile_definitions(hfc_core PUBLIC HFC_VERSION="${PROJECT_VERSION}")

# A stand-in for SDRconnect's WebSocket server (tests, development).
add_library(hfc_testing STATIC testing/fake_sdrconnect.cpp testing/iq_scene.cpp testing/demo_scene.cpp)
target_include_directories(hfc_testing PUBLIC testing)
target_link_libraries(hfc_testing PUBLIC hfc_core)

# The application: the processing for each receiver, the log and the web
# UI (app/ui/index.html, embedded in the program).
set(HFC_UI_CPP ${CMAKE_CURRENT_BINARY_DIR}/ui_page.cpp)
add_custom_command(
  OUTPUT ${HFC_UI_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/app/ui/index.html -DOUTPUT=${HFC_UI_CPP}
          -DFUNCTION=hfc::uiPage -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS app/ui/index.html cmake/embed.cmake
  COMMENT "Embedding the web UI")
set(HFC_ICON_CPP ${CMAKE_CURRENT_BINARY_DIR}/ui_icon.cpp)
add_custom_command(
  OUTPUT ${HFC_ICON_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/app/resources/hf_classifier.png -DOUTPUT=${HFC_ICON_CPP}
          -DFUNCTION=hfc::uiIcon -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS app/resources/hf_classifier.png cmake/embed.cmake
  COMMENT "Embedding the UI's icon")
set(HFC_TASKS_CPP ${CMAKE_CURRENT_BINARY_DIR}/recording_tasks.cpp)
add_custom_command(
  OUTPUT ${HFC_TASKS_CPP}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/catalogue/recording_tasks.json -DOUTPUT=${HFC_TASKS_CPP}
          -DFUNCTION=hfc::recordingTasksText -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS catalogue/recording_tasks.json cmake/embed.cmake
  COMMENT "Embedding the recording tasks")
add_library(hfc_app STATIC app/classifier_app.cpp app/platform.cpp app/diagnostics.cpp app/log_capture.cpp app/runner.cpp app/sdrconnect_source.cpp
            app/settings.cpp ${HFC_UI_CPP} ${HFC_ICON_CPP} ${HFC_TASKS_CPP})
target_include_directories(hfc_app PUBLIC app)
target_link_libraries(hfc_app PUBLIC hfc_core hfc_testing)  # hfc_testing: the demo scene
if(WIN32)
  target_link_libraries(hfc_app PUBLIC version)  # file versions, for the diagnostic report
endif()

if(HFC_BUILD_TOOLS)
  add_executable(hf_classifier app/main.cpp)
  target_link_libraries(hf_classifier PRIVATE hfc_app)
  if(WIN32)
    # Icon and version information, and the program for everyday use:
    # HFClassifier.exe, without a console, in the notification area.
    set(HFC_ICON ${CMAKE_CURRENT_SOURCE_DIR}/app/resources/hf_classifier.ico)
    file(TO_CMAKE_PATH "${HFC_ICON}" HFC_ICON)
    configure_file(app/resources/hf_classifier.rc.in ${CMAKE_CURRENT_BINARY_DIR}/hf_classifier.rc @ONLY)
    target_sources(hf_classifier PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/hf_classifier.rc)
    target_link_libraries(hf_classifier PRIVATE shell32)
    add_executable(HFClassifier WIN32 app/tray_main.cpp ${CMAKE_CURRENT_BINARY_DIR}/hf_classifier.rc)
    target_link_libraries(HFClassifier PRIVATE hfc_app shell32 user32)
    if(MINGW)
      target_link_options(HFClassifier PRIVATE -municode)
    endif()
  endif()
  add_executable(sdrc_probe tools/sdrc_probe.cpp)
  target_link_libraries(sdrc_probe PRIVATE hfc_core)
  add_executable(hfc_features tools/hfc_features.cpp)
  target_link_libraries(hfc_features PRIVATE hfc_core)
  add_executable(sdrc_record tools/sdrc_record.cpp)
  target_link_libraries(sdrc_record PRIVATE hfc_core)
  add_executable(make_test_scene tools/make_test_scene.cpp)
  target_link_libraries(make_test_scene PRIVATE hfc_testing)
  add_executable(make_busy_scene tools/make_busy_scene.cpp)
  target_link_libraries(make_busy_scene PRIVATE hfc_testing)
  add_executable(fake_sdrconnect tools/fake_sdrconnect.cpp)
  target_link_libraries(fake_sdrconnect PRIVATE hfc_testing)
  if(WIN32)
    # The installer (installer/setup.nsi), with NSIS:
    #   cmake --build build --config Release --target installer
    set(HFC_PF86 "ProgramFiles(x86)")
    find_program(HFC_MAKENSIS makensis PATHS "$ENV{${HFC_PF86}}/NSIS" "$ENV{ProgramFiles}/NSIS")
    if(HFC_MAKENSIS)
      add_custom_target(
        installer
        # (UTF-8: setup.nsi has a byte order mark too; without one, makensis
        # reads the system's ANSI code page and the Bulgarian texts break.)
        COMMAND ${HFC_MAKENSIS} -V2 -WX -INPUTCHARSET UTF8 -DVERSION=${PROJECT_VERSION} "-DBINDIR=$<TARGET_FILE_DIR:HFClassifier>"
                "-DREADME=${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                "-DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/HFClassifier_Setup_${PROJECT_VERSION}.exe"
                "${CMAKE_CURRENT_SOURCE_DIR}/installer/setup.nsi"
        DEPENDS HFClassifier hf_classifier sdrc_probe sdrc_record
        COMMENT "Building the installer"
        VERBATIM)
    endif()
  endif()
endif()

if(HFC_BUILD_TESTS)
  enable_testing()
  add_executable(hfc_tests tests/test_main.cpp tests/test_dsp.cpp tests/test_pipeline.cpp tests/test_store.cpp tests/test_app.cpp
                 tests/test_run.cpp tests/test_features.cpp
                 tests/test_catalogue.cpp tests/test_frequency_list.cpp tests/test_dsc.cpp tests/test_hfdl.cpp tests/test_fax.cpp tests/test_sstv.cpp tests/test_timesig.cpp tests/test_wspr.cpp tests/test_drm.cpp tests/test_policy.cpp tests/test_akula.cpp tests/test_constant_weight.cpp tests/test_family_model.cpp tests/test_diagnostics.cpp)
  target_link_libraries(hfc_tests PRIVATE hfc_testing hfc_app)
  if(TARGET fake_sdrconnect)
    # Started by the tests in place of SDRconnect_headless.
    add_dependencies(hfc_tests fake_sdrconnect)
    target_compile_definitions(hfc_tests PRIVATE HFC_FAKE_SDRCONNECT="$<TARGET_FILE:fake_sdrconnect>")
  endif()
  add_test(NAME hfc_tests COMMAND hfc_tests)
endif()
