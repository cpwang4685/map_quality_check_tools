cmake_minimum_required(VERSION 3.16)
project(map_quality_check_tools VERSION 3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)
# AUTOUIC 会把手写的 ui_class/ui_fit_helper.h（25 处 include）误判为 uic 生成物，
# 因无 fit_helper.ui 而报错。故关闭 AUTOUIC，改用 qt5_wrap_ui 显式生成（见下）。
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

# ================================================================
# 平台检测
# ================================================================
if(UNIX AND NOT APPLE)
    add_definitions(-DOS_FAMILY_LINUX -DOS_FAMILY_UNIX)
    set(LINUX_BUILD TRUE)
elseif(WIN32)
    add_definitions(-DOS_FAMILY_WINDOWS)
    set(LINUX_BUILD FALSE)
endif()

# ================================================================
# NMO SDK 开关
#   Windows：vcxproj 构建（DELAYLOAD），CMake 分支也全量编译 NMO
#   Linux：默认 SE_NMO_NO_SDK（功能置灰、include 剔除）；
#          NMO SDK .so 就位后加 -DSE_NMO_SDK_ENABLED=ON 翻转
# ================================================================
option(SE_NMO_SDK_ENABLED "链接 NMO SDK（需 include/ + lib/libMapBatchProcessing.so、libIO.so）" OFF)
set(NMO_SDK_HOME "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/nmo_sdk"
    CACHE PATH "NMO SDK 根目录（含 include/ lib/）")

if(WIN32)
    set(SE_NMO_SDK_ENABLED ON)
elseif(SE_NMO_SDK_ENABLED)
    add_definitions(-DSE_NMO_SDK_ENABLED)
    find_library(NMO_MAPBATCH_LIBRARY MapBatchProcessing PATHS "${NMO_SDK_HOME}/lib" NO_DEFAULT_PATH)
    find_library(NMO_IO_LIBRARY IO PATHS "${NMO_SDK_HOME}/lib" NO_DEFAULT_PATH)
else()
    add_definitions(-DSE_NMO_NO_SDK)
endif()

# ================================================================
# QGIS SDK（麒麟）
# ================================================================
set(QGIS_SDK_DIR "/qgis/kylin_v10_sp1_qgis_32815_release"
    CACHE PATH "QGIS SDK install prefix (Kylin)")
if(NOT EXISTS "${QGIS_SDK_DIR}/include/qgis/qgis.h")
    message(FATAL_ERROR "QGIS_SDK_DIR=${QGIS_SDK_DIR} is invalid: include/qgis/qgis.h not found. Pass -DQGIS_SDK_DIR=<path> if the SDK is elsewhere.")
endif()
find_library(QGIS_CORE_LIBRARY qgis_core PATHS "${QGIS_SDK_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(QGIS_GUI_LIBRARY  qgis_gui  PATHS "${QGIS_SDK_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
set(QGIS_INCLUDE_DIR "${QGIS_SDK_DIR}/include")

# ================================================================
# Qt5（系统 Qt 5.12.12）
#   含 Svg：QGIS SDK 的 qgsrubberband.h include <QSvgRenderer>（QtSvg 模块），
#   插件经 clip_dialog → map_extent_tool.h → qgsrubberband.h 间接依赖，缺了编不过。
# ================================================================
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Xml Network Sql Svg)

# ================================================================
# 系统地理库
# ================================================================
find_package(GDAL REQUIRED)
# PostgreSQL 客户端库（libpq）——插件只用 libpq-fe.h + libpq。
# 不用 find_package(PostgreSQL)：CMake 3.16 的 FindPostgreSQL 强求服务器头
# (PostgreSQL_TYPE_INCLUDE_DIR，来自 postgresql-server-dev-* 包)，插件用不到。
find_library(POSTGRESQL_LIBRARY pq)
find_path(POSTGRESQL_INCLUDE_DIR libpq-fe.h PATH_SUFFIXES postgresql)
if(NOT POSTGRESQL_LIBRARY OR NOT POSTGRESQL_INCLUDE_DIR)
    message(FATAL_ERROR "libpq 未找到：需要 libpq-dev 包（libpq-fe.h 在 /usr/include/postgresql，libpq.so 在系统库路径）。")
endif()
find_library(GEOS_C_LIBRARY geos_c REQUIRED)   # geo_algorithm_h/vector/cse_generalization.cpp 用 GEOS C API

# ================================================================
# 共享源码目录 geo_algorithm_h（插件目录上三级：D:\GarMap\geo_algorithm_h）
#   默认相对路径，麒麟上整树拷贝时位置无关；也可 -DGEO_ALGORITHM_H_DIR=<path>
# ================================================================
set(GEO_ALGORITHM_H_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../geo_algorithm_h"
    CACHE PATH "geo_algorithm_h 目录（含 commontype/ vector/ 等）")
if(NOT EXISTS "${GEO_ALGORITHM_H_DIR}/commontype/se_commondef.h")
    message(WARNING "geo_algorithm_h 未找到（当前值 ${GEO_ALGORITHM_H_DIR}），编译会失败。可用 -DGEO_ALGORITHM_H_DIR=<path> 指定。")
endif()

# ================================================================
# spdlog 1.15.0（header-only，含 bundled fmt）
#   优先用插件打包目录（../../.. 指 D:\GarMap\extern\spdlog-1.x），缺则退系统
# ================================================================
set(SPDLOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../extern/spdlog-1.x/include"
    CACHE PATH "spdlog include 目录（含 spdlog/spdlog.h）")
if(NOT EXISTS "${SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
    # 打包目录缺失，退回系统安装的 spdlog（find_path 不会覆盖已存在的 CACHE 变量，用 FORCE）
    find_path(_SPDLOG_SYSTEM_INCLUDE_DIR spdlog/spdlog.h DOC "spdlog include dir (system)")
    if(_SPDLOG_SYSTEM_INCLUDE_DIR)
        set(SPDLOG_INCLUDE_DIR "${_SPDLOG_SYSTEM_INCLUDE_DIR}"
            CACHE PATH "spdlog include 目录（含 spdlog/spdlog.h）" FORCE)
    endif()
endif()
if(NOT EXISTS "${SPDLOG_INCLUDE_DIR}/spdlog/spdlog.h")
    message(FATAL_ERROR "spdlog 头未找到，可用 -DSPDLOG_INCLUDE_DIR=<path> 指定。")
endif()

# ================================================================
# 源文件清单（与 map_quality_check_tools.vcxproj 的 ClCompile 一致，去掉
# GeneratedFiles 的 moc_* / ui_*，由 AUTOMOC/AUTOUIC 生成）
# ================================================================
set(PLUGIN_SOURCES
    # 插件入口
    map_quality_check_tools.cpp

    # 顶层业务
    map_check_backup_manager.cpp

    # 核心工具
    core/calendar_helper.cpp
    core/data_importer.cpp
    core/file_storage_manager.cpp
    core/metadata_extractor.cpp

    # 数据库层（libpq）
    database/postgis_connector.cpp
    database/product_dao.cpp
    database/product_metadata.cpp
    database/schema_manager.cpp

    # UI 对话框（ui_class/）
    ui_class/access_control_dialog.cpp
    ui_class/auto_edge_match_dialog.cpp
    ui_class/backup_strategy_edit_dialog.cpp
    ui_class/clip_dialog.cpp
    ui_class/data_import_wizard.cpp
    ui_class/db_config_dialog.cpp
    ui_class/format_conversion_dialog.cpp
    ui_class/gdb_layer_selector_dialog.cpp
    ui_class/generalization_config_dialog.cpp
    ui_class/layer_type_select_dialog.cpp
    ui_class/login_dialog.cpp
    ui_class/map_tool_extent_picker.cpp
    ui_class/mapdata_download.cpp
    ui_class/merge_dialog.cpp
    ui_class/metadata_manager_dialog.cpp
    ui_class/param_config_dialog.cpp
    ui_class/product_storage_dialog.cpp
    ui_class/scale_selector_dialog.cpp
    ui_class/se_auto_quality_check.cpp
    ui_class/se_data_export.cpp
    ui_class/se_data_import.cpp
    ui_class/se_data_list_export.cpp
    ui_class/se_data_management.cpp
    ui_class/se_data_restore.cpp
    ui_class/se_database_connection.cpp
    ui_class/se_db_manager.cpp
    ui_class/se_layer_mapping_dialog.cpp
    ui_class/se_metadata_viewer.cpp
    ui_class/se_mission453_check.cpp
    ui_class/se_mission454_check.cpp
    ui_class/se_mission455_check.cpp
    ui_class/se_mission456_check.cpp
    ui_class/se_mission457_check.cpp
    ui_class/se_mission458_check.cpp
    ui_class/se_mission459_check.cpp
    ui_class/se_vector_format_conversion.cpp

    # 后台任务（ui_task/）
    ui_task/se_clip_merge_task.cpp
    ui_task/se_edge_match_task.cpp
    ui_task/se_format_convert_task.cpp
    ui_task/se_geojson2shp.cpp
    ui_task/se_nmo_sdk_bridge.cpp
    ui_task/se_wuji_process_runner.cpp
    ui_task/se_wuji_xml_generator.cpp

    # 跨目录共享源码（geo_algorithm_h）
    ${GEO_ALGORITHM_H_DIR}/vector/cse_generalization.cpp
)

# 头文件（AUTOMOC 需要；仅列插件自身目录，GeneratedFiles 的 ui_*.h 由 AUTOUIC 重建）
file(GLOB PLUGIN_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/database/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_class/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_task/*.h
)

# UI 文件（19 个；uic 生成 ui_<basename>.h）
set(PLUGIN_UIS
    ui/auto_edge_match.ui
    ui/auto_quality_check.ui
    ui/clip_dialog.ui
    ui/data_export.ui
    ui/data_management.ui
    ui/data_restore.ui
    ui/database_connection.ui
    ui/format_conversion.ui
    ui/merge_dialog.ui
    ui/param_config.ui
    ui/scale_selector_dialog.ui
    ui/vector_format_conversion.ui
    ui_class/access_control_dialog.ui
    ui_class/db_config_dialog.ui
    ui_class/gdb_layer_selector_dialog.ui
    ui_class/login_dialog.ui
    ui_class/mapdata_download.ui
    ui_class/metadata_manager_dialog.ui
    ui_class/product_storage_dialog.ui
)

# 资源文件
set(PLUGIN_RCCS
    map_quality_check_tools.qrc
)

# AUTOUIC 关闭后，显式运行 uic 生成全部 ui_*.h（19 个 .ui basename 无重名，平铺进 build 目录）
qt5_wrap_ui(PLUGIN_UI_HEADERS ${PLUGIN_UIS})

# ================================================================
# 构建目标
# ================================================================
add_library(${PROJECT_NAME} SHARED
    ${PLUGIN_SOURCES}
    ${PLUGIN_HEADERS}
    ${PLUGIN_UI_HEADERS}
    ${PLUGIN_UIS}
    ${PLUGIN_RCCS}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "plugin_map_quality_check_tools"
    PREFIX ""                                    # 产出 plugin_map_quality_check_tools.so（LTZK resolvePluginDllPath 有 .so 回退）
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    BUILD_RPATH   "${QGIS_SDK_DIR}/lib"
    INSTALL_RPATH "${QGIS_SDK_DIR}/lib"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/database
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_class
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_task
    ${QGIS_INCLUDE_DIR}
    ${QGIS_INCLUDE_DIR}/qgis          # 麒麟 SDK 头在 include/qgis/ 子目录
    ${GEO_ALGORITHM_H_DIR}
    ${GDAL_INCLUDE_DIRS}
    ${POSTGRESQL_INCLUDE_DIR}
    ${SPDLOG_INCLUDE_DIR}
    # qt5_wrap_ui 生成物目录（ui_*.h 平铺在 build 目录）
    ${CMAKE_CURRENT_BINARY_DIR}
)

# NMO SDK include（仅 SDK 可用时加入；SE_NMO_NO_SDK 下相关 include 已被守卫剔除）
if(WIN32 OR SE_NMO_SDK_ENABLED)
    target_include_directories(${PROJECT_NAME} PRIVATE "${NMO_SDK_HOME}/include")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${QGIS_CORE_LIBRARY}
    ${QGIS_GUI_LIBRARY}
    ${GDAL_LIBRARIES}
    ${POSTGRESQL_LIBRARY}
    ${GEOS_C_LIBRARY}
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Xml Qt5::Network Qt5::Sql Qt5::Svg
)

# NMO SDK 库（Linux 分支）
if(SE_NMO_SDK_ENABLED AND NOT WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${NMO_MAPBATCH_LIBRARY} ${NMO_IO_LIBRARY})
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()

# ================================================================
# 安装：插件本体进 plugins/（与 /opt/ltzk/plugins 布局一致）
# ================================================================
install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION plugins)

message(STATUS "map_quality_check_tools 插件构建已配置")
message(STATUS "  输出: ${CMAKE_BINARY_DIR}/plugins/plugin_map_quality_check_tools.so")
message(STATUS "  QGIS SDK: ${QGIS_SDK_DIR}")
message(STATUS "  NMO: ${SE_NMO_SDK_ENABLED} (OFF=功能置灰; ON=链接 ${NMO_SDK_HOME})")
