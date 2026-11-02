# PLANK uses only the prepared, pinned Host FFmpeg tree. Package preparation
# verifies its source and selected patches before configuring this project.
include_guard(GLOBAL)

if(NOT FFMPEG_PREPARED_BINARIES)
    message(FATAL_ERROR "Set FFMPEG_PREPARED_BINARIES to the pinned Host FFmpeg install; see the PLANK build runbook")
endif()
set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PREPARED_BINARIES}/include")
if(NOT EXISTS "${FFMPEG_INCLUDE_DIRS}/libavcodec/avcodec.h")
    message(FATAL_ERROR "Prepared Host FFmpeg headers are missing")
endif()

# Keep static library order: avcodec depends on swscale/avutil and x264.
set(FFMPEG_LIBRARIES)
foreach(library avcodec swscale avutil cbs x264)
    set(archive "${FFMPEG_PREPARED_BINARIES}/lib/lib${library}.a")
    if(NOT EXISTS "${archive}")
        message(FATAL_ERROR "Required prepared Host library is missing: ${archive}")
    endif()
    list(APPEND FFMPEG_LIBRARIES "${archive}")
endforeach()
list(APPEND FFMPEG_LIBRARIES m pthread dl)
