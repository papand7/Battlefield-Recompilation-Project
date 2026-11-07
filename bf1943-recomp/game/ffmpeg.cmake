# FFmpeg (LGPL-2.1, thirdparty/ffmpeg/COPYING.LGPLv2.1): only libavutil and the
# libavcodec parts behind Xenia's XMA path (xmaframes decoder, from the
# has207/FFmpeg xmaframes branch). Config headers from Xenia's ffmpeg-xenia.
set(FFMPEG_DIR ${ROOT}/thirdparty/ffmpeg)
include(${FFMPEG_DIR}/sources.cmake)
add_library(bf1943_ffmpeg STATIC ${FFMPEG_SOURCES})
target_compile_definitions(bf1943_ffmpeg PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
target_include_directories(bf1943_ffmpeg PRIVATE
  ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR} ${FFMPEG_DIR}/libavcodec
  ${FFMPEG_DIR}/compat/atomics/win32)
target_include_directories(bf1943_ffmpeg INTERFACE ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR})
# -Wno-incompatible-pointer-types: Xenia's component lists use the public types.
target_compile_options(bf1943_ffmpeg PRIVATE -w -Wno-incompatible-pointer-types)
target_link_libraries(bf1943_ffmpeg PUBLIC bcrypt)
