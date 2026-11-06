cmake_minimum_required(VERSION 3.26)
project(Tori VERSION 0.1.0 LANGUAGES CXX RC)

if (NOT MSVC)
    message(FATAL_ERROR "Xen requires the MSVC toolchain")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

foreach (OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
endforeach ()

option(TORI_BUILD_TESTS "Build the ToriCore unit tests" ON)

# Builds against a local XenUI checkout instead of fetching it, while XenUI changes are still unpushed:
#   cmake --preset debug -DTORI_XENUI_DIR=C:/path/to/XenUI
set(TORI_XENUI_DIR "" CACHE PATH "Local XenUI checkout to build against instead of fetching one")
if (TORI_XENUI_DIR)
    set(FETCHCONTENT_SOURCE_DIR_XENUI "${TORI_XENUI_DIR}" CACHE PATH "" FORCE)
else ()
    unset(FETCHCONTENT_SOURCE_DIR_XENUI CACHE)
endif ()

include(FetchContent)
FetchContent_Declare(XenUI
        GIT_REPOSITORY https://github.com/jakerieger/XenUI.git
        GIT_TAG master
)
set(XENUI_BUILD_TESTS OFF)
set(XENUI_BUILD_SHOWCASE OFF)
set(XENUI_BUILD_EXAMPLES OFF)
FetchContent_MakeAvailable(XenUI)

# BitTorrent engine, as ToriCore.dll. Must not depend on XenUI so it can be tested headless.
# The version, in project() above, as macros for both C++ (Core/Version.hpp) and the resource compiler
configure_file(Resource/ToriVersion.h.in "${CMAKE_BINARY_DIR}/Generated/ToriVersion.h" @ONLY)

add_library(ToriCore SHARED
        Resource/ToriCore.rc
        Source/Core/Bencode.cpp
        Source/Core/Bitfield.cpp
        Source/Core/Encoding.cpp
        Source/Core/FileIO.cpp
        Source/Core/Format.cpp
        Source/Core/InfoHash.cpp
        Source/Core/PeerId.cpp
        Source/Core/Random.cpp
        Source/Core/RateMeter.cpp
        Source/Core/Sha1.cpp
        Source/Core/TorrentTypes.cpp
        Source/Core/Utf8.cpp
        Source/Core/Win32.cpp
        Source/Dht/DhtNode.cpp
        Source/Dht/Krpc.cpp
        Source/Dht/RoutingTable.cpp
        Source/Engine/Choker.cpp
        Source/Engine/PiecePicker.cpp
        Source/Engine/ResumeData.cpp
        Source/Engine/Session.cpp
        Source/Engine/Torrent.cpp
        Source/Meta/MagnetUri.cpp
        Source/Meta/Metainfo.cpp
        Source/Meta/TorrentCreator.cpp
        Source/Net/Endpoint.cpp
        Source/Net/Http.cpp
        Source/Net/Socket.cpp
        Source/Peer/Extensions.cpp
        Source/Peer/Messages.cpp
        Source/Peer/PeerConnection.cpp
        Source/Storage/FileStorage.cpp
        Source/Storage/PieceVerifier.cpp
        Source/Tracker/Announcer.cpp
        Source/Tracker/HttpTracker.cpp
        Source/Tracker/UdpTracker.cpp
)
target_include_directories(ToriCore PUBLIC Source "${CMAKE_BINARY_DIR}/Generated")
target_compile_definitions(ToriCore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
# C++ only: the resource compiler shares the targets' options and doesn't know these
target_compile_options(ToriCore
        PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
target_link_libraries(ToriCore PUBLIC ws2_32 bcrypt winhttp shell32)
# Every function is exported, so the app, ToriCli and the tests all use the same engine code
set_target_properties(ToriCore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(Tori WIN32
        Resource/Tori.rc
        Source/main.cpp
        Source/UI/Actions.cpp
        Source/UI/CrashLog.cpp
        Source/UI/Details.cpp
        Source/UI/Dialogs.cpp
        Source/UI/Instance.cpp
        Source/UI/Menus.cpp
        Source/UI/Model.cpp
        Source/UI/Root.cpp
        Source/UI/StatusBar.cpp
        Source/UI/TorrentList.cpp
        Source/UI/Updates.cpp
)
target_include_directories(Tori PRIVATE Resource)
target_link_options(Tori PRIVATE /ENTRY:mainCRTStartup)
target_link_libraries(Tori PRIVATE ToriCore XenUI::XenUI shell32 ole32 dbghelp)

add_custom_command(TARGET Tori POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:Tori> $<TARGET_FILE_DIR:Tori>
        COMMAND_EXPAND_LISTS
)

# Headless driver for checking real-world downloads
add_executable(ToriCli Tools/ToriCli.cpp)
target_link_libraries(ToriCli PRIVATE ToriCore shell32)

if (TORI_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(doctest
            GIT_REPOSITORY https://github.com/doctest/doctest.git
            GIT_TAG v2.4.11
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR header-only-no-cmake
    )
    FetchContent_MakeAvailable(doctest)

    add_executable(ToriTests
            Tests/Main.cpp
            Tests/BencodeTests.cpp
            Tests/ChokerTests.cpp
            Tests/CreatorTests.cpp
            Tests/DhtTests.cpp
            Tests/EncodingTests.cpp
            Tests/EngineTests.cpp
            Tests/ExtensionTests.cpp
            Tests/FormatTests.cpp
            Tests/MagnetTests.cpp
            Tests/MetainfoTests.cpp
            Tests/PeerTests.cpp
            Tests/ResumeTests.cpp
            Tests/StorageTests.cpp
            Tests/TestUtil.hpp
            Tests/TrackerTests.cpp
    )
    target_include_directories(ToriTests SYSTEM PRIVATE "${doctest_SOURCE_DIR}")
    # doctest's own std forward declarations can't stringify string_view on MSVC
    target_compile_definitions(ToriTests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
    target_link_libraries(ToriTests PRIVATE ToriCore)
    add_test(NAME ToriTests COMMAND ToriTests)
endif ()
