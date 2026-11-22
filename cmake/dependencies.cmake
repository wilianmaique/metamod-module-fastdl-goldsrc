include(FetchContent)
# Only the headers are used. Do not build upstream examples or tests.
FetchContent_Declare(boost_headers
  URL https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.gz
  URL_HASH SHA256=c4a3b310ddd2472416e091067166b0713be97c63f38c212c484ada022fd296ce
  SOURCE_SUBDIR headers-only)
FetchContent_Declare(metamod
  URL https://codeload.github.com/rehlds/Metamod-R/tar.gz/56590c5c70fa95775051888b7fe0a4e285856ebb
  URL_HASH SHA256=483ff83c501924a8295a700a30df4dd0426968b226062cff0c3195057b152fef
  SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(boost_headers metamod)
set(sdk "${metamod_SOURCE_DIR}/metamod/extra/example/include")
set(sdk_include_dirs "${sdk}/metamod")
foreach(dir common engine dlls pm_shared public game_shared)
  list(APPEND sdk_include_dirs "${sdk}/hlsdk/${dir}")
endforeach()
