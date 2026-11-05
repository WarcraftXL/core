# SPDX-License-Identifier: GPL-3.0-or-later
# ctest: Windows executables, run under wine from Linux (CMAKE_CROSSCOMPILING_EMULATOR).

# The test extensions never go into the client folder.
set(CLIENT_PATH "")

# Built for SDK 1.3 by Export, with the manifest wxl_add_extension writes.
wxl_add_extension(wxl-test-current
    SOURCES extensions/Current.cpp
    VERSION 7.0.1
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/current")

# Hand-written WXL_Query: one claiming a newer minor, one of another API version.
wxl_add_extension(wxl-test-newer
    SOURCES extensions/Newer.cpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/newer")
wxl_add_extension(wxl-test-old-api
    SOURCES extensions/OldApi.cpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/old-api")

add_executable(wxl-loader-test
    LoaderTest.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Loader.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Manifest.cpp")
target_include_directories(wxl-loader-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-loader-test PRIVATE wxl_common WarcraftXL::sdk)
target_compile_definitions(wxl-loader-test PRIVATE ${WXL_DEFS})
add_dependencies(wxl-loader-test wxl-test-current wxl-test-newer wxl-test-old-api)

add_test(NAME loader
         COMMAND wxl-loader-test "${CMAKE_CURRENT_BINARY_DIR}/built" "${CMAKE_CURRENT_BINARY_DIR}/scratch")

# --- fault isolation: extensions that fault, throw or decline, through the real bus and chains ---
wxl_add_extension(wxl-test-faulty
    SOURCES extensions/Faulty.cpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/wxl-test-faulty")
wxl_add_extension(wxl-test-throws
    SOURCES extensions/Throws.cpp
    LINK WarcraftXL::game
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/wxl-test-throws")
wxl_add_extension(wxl-test-declines
    SOURCES extensions/Declines.cpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/wxl-test-declines")
wxl_add_extension(wxl-test-resident
    SOURCES extensions/Resident.cpp
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/built/wxl-test-resident")

add_executable(wxl-guard-test
    GuardTest.cpp
    "${PROJECT_SOURCE_DIR}/src/engine/events/Event.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/hook/Hook.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Loader.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Json.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Manifest.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/Owners.cpp")
target_include_directories(wxl-guard-test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/deps/minhook/include")
target_link_libraries(wxl-guard-test PRIVATE wxl_common WarcraftXL::sdk minhook)
target_compile_definitions(wxl-guard-test PRIVATE ${WXL_DEFS})
add_dependencies(wxl-guard-test wxl-test-faulty wxl-test-throws wxl-test-declines wxl-test-resident)
add_test(NAME guard COMMAND wxl-guard-test "${CMAKE_CURRENT_BINARY_DIR}/built")

# --- the objects table over a fake object manager ----------------------------------------------
add_executable(wxl-objects-test ObjectsTest.cpp "${PROJECT_SOURCE_DIR}/src/runtime/objects/Objects.cpp")
target_include_directories(wxl-objects-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-objects-test PRIVATE WarcraftXL::sdk)
target_compile_definitions(wxl-objects-test PRIVATE ${WXL_DEFS})
add_test(NAME objects COMMAND wxl-objects-test)

# --- the event bus ------------------------------------------------------------------------------
add_executable(wxl-bus-test
    BusTest.cpp
    "${PROJECT_SOURCE_DIR}/src/engine/events/Event.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/Owners.cpp")
target_include_directories(wxl-bus-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-bus-test PRIVATE wxl_common WarcraftXL::sdk)
target_compile_definitions(wxl-bus-test PRIVATE ${WXL_DEFS})
add_test(NAME bus COMMAND wxl-bus-test)

# --- patches: checked, refused across owners, given back on disable -------------------------------
add_executable(wxl-patch-test
    PatchTest.cpp
    "${PROJECT_SOURCE_DIR}/src/engine/patch/Patch.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/Owners.cpp")
target_include_directories(wxl-patch-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-patch-test PRIVATE wxl_common WarcraftXL::sdk)
target_compile_definitions(wxl-patch-test PRIVATE ${WXL_DEFS})
add_test(NAME patch COMMAND wxl-patch-test)

# --- the settings store -------------------------------------------------------------------------
add_executable(wxl-settings-test
    SettingsTest.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Command.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Ini.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Store.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Value.cpp")
target_include_directories(wxl-settings-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-settings-test PRIVATE WarcraftXL::sdk)
target_compile_definitions(wxl-settings-test PRIVATE ${WXL_DEFS})
add_test(NAME settings COMMAND wxl-settings-test)

# --- storage: names, the redirect/provider/transform order, the in-memory file handle ------------
add_executable(wxl-storage-test
    StorageTest.cpp
    "${PROJECT_SOURCE_DIR}/src/engine/storage/ArchiveName.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/storage/Providers.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/storage/SyntheticFile.cpp")
target_include_directories(wxl-storage-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(wxl-storage-test PRIVATE WarcraftXL::sdk)
target_compile_definitions(wxl-storage-test PRIVATE ${WXL_DEFS})
add_test(NAME storage COMMAND wxl-storage-test)

# --- bench: dispatch through the bus, hook chains, interceptors, signals, the settings walk; no allocation ---
add_executable(wxl-bench
    Bench.cpp
    "${PROJECT_SOURCE_DIR}/src/engine/events/Event.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/hook/Hook.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/patch/Patch.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/HookPoints.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/Owners.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Api.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/extensions/Services.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Ini.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Store.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/settings/Value.cpp")
target_include_directories(wxl-bench PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/deps/minhook/include")
target_link_libraries(wxl-bench PRIVATE wxl_common WarcraftXL::sdk minhook)
target_compile_definitions(wxl-bench PRIVATE ${WXL_DEFS})
add_test(NAME bench COMMAND wxl-bench)

# The hook-point table is generated by the SDK from the atlas; with a working copy of the SDK, it
# must be what the generator gives.
find_program(WXL_LUAJIT luajit)
if(WXL_SDK_DIR AND WXL_LUAJIT AND EXISTS "${WXL_SDK_DIR}/tools/offsets/gen.lua")
    set(WXL_ATLAS_DIR "${WXL_SDK_DIR}/../atlas" CACHE PATH "The atlas checkout the SDK generates from")
    add_test(NAME hookpoints-generated
             COMMAND ${WXL_LUAJIT} "${WXL_SDK_DIR}/tools/offsets/gen.lua" --atlas "${WXL_ATLAS_DIR}"
                     --core "${PROJECT_SOURCE_DIR}" --check)
endif()

# --- the seams: lifetimes, damage, death, names and chat over fake hook points and object manager ---
add_executable(wxl-seams-test
    seams/Harness.cpp
    seams/LifetimeCheck.cpp
    seams/ThunkCheck.cpp
    seams/DamageCheck.cpp
    seams/UnitAndChatCheck.cpp
    seams/WorldCheck.cpp
    "${PROJECT_SOURCE_DIR}/src/client/CGObject_C/Lifetime.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CGObject_C/LifetimeThunks.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CGUnit_C/Damage.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CGUnit_C/DamageArgs.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CGUnit_C/DeathAndName.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CGChat/Chat.cpp"
    "${PROJECT_SOURCE_DIR}/src/client/CWorldScene/World.cpp"
    "${PROJECT_SOURCE_DIR}/src/engine/events/Event.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/Owners.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/objects/Objects.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/objects/Lifetime.cpp")
target_include_directories(wxl-seams-test PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
target_link_libraries(wxl-seams-test PRIVATE wxl_common WarcraftXL::game)
target_compile_definitions(wxl-seams-test PRIVATE ${WXL_DEFS})
add_test(NAME seams COMMAND wxl-seams-test)
