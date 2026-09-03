cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK environment variable is not set. Run: source env.sh")
endif()

set(VITASDK $ENV{VITASDK})

# 必须先加载工具链（设置交叉编译器），再调用 project()
include(${VITASDK}/share/vita.toolchain.cmake)
include(${VITASDK}/share/vita.cmake)

project(psvsend VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(VITA_TITLEID "PSVSEND01")
set(VITA_TITLE  "PSVSend")

add_executable(${PROJECT_NAME}
    src/main.c
    src/api.c
    src/config.c
    src/dlog.c
    src/discovery.c
    src/http.c
    src/json_util.c
    src/net.c
    src/ui/ui_main.c
    src/ui/pages.c
    src/ui/widgets.c
    src/ui/input.c
    src/ui/theme.c
)
target_link_libraries(${PROJECT_NAME}
    vita2d freetype png jpeg z m
    SceAppMgr_stub SceCommonDialog_stub SceCtrl_stub SceDisplay_stub
    SceGxm_stub SceIofilemgr_stub SceKernelThreadMgr_stub
    SceNet_stub SceNetCtl_stub
    ScePgf_stub ScePvf_stub SceSysmodule_stub SceTouch_stub
)

vita_create_self(${PROJECT_NAME}.elf ${PROJECT_NAME})

# 手动打包 VPK（vita_create_vpk 的 FILE 参数会经 separate_arguments 拆分，
# 破坏含空格/中文的路径，故改为 add_custom_command 直接调用工具）
set(_sfo  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}_param.sfo)
set(_vpk  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk)
set(_pkgsce ${CMAKE_CURRENT_BINARY_DIR}/sce_sys)
set(_elf  ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.elf)
add_custom_command(OUTPUT ${_vpk}
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys ${_pkgsce}
  COMMAND ${VITA_MKSFOEX} -s TITLE_ID=${VITA_TITLEID} -s APP_VER=01.00
          ${VITA_TITLE} ${_sfo}
  COMMAND ${VITA_PACK_VPK} -s ${_sfo} -b ${_elf}
          -a ${_pkgsce}/icon0.png=sce_sys/icon0.png
          -a ${_pkgsce}/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png
          -a ${_pkgsce}/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png
          -a ${_pkgsce}/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml
          ${_vpk}
  DEPENDS ${_elf}
          ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png
          ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png
          ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png
          ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml
  COMMENT "Building vpk ${PROJECT_NAME}"
  VERBATIM
)
add_custom_target(vpk ALL DEPENDS ${_vpk})
