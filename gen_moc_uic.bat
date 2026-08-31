@echo off
set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%PATH%"

set "PROJ=D:\GarMap\qgis_plugins\cplusplus\map_quality_check_tools"
set "GEN=%PROJ%\GeneratedFiles\Release"
set "UIC=D:\LTZK\OSGeo4W_32815\apps\Qt5\bin\uic.exe"
set "MOC=D:\LTZK\OSGeo4W_32815\apps\Qt5\bin\moc.exe"

if not exist "%GEN%" mkdir "%GEN%"

echo === UIC generation ===
"%UIC%" -o "%GEN%\ui_vector_format_conversion.h" "%PROJ%\ui\vector_format_conversion.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_auto_quality_check.h" "%PROJ%\ui\auto_quality_check.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_data_restore.h" "%PROJ%\ui\data_restore.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_database_connection.h" "%PROJ%\ui\database_connection.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_data_management.h" "%PROJ%\ui\data_management.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_data_export.h" "%PROJ%\ui\data_export.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_param_config.h" "%PROJ%\ui\param_config.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_scale_selector_dialog.h" "%PROJ%\ui\scale_selector_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_clip_dialog.h" "%PROJ%\ui\clip_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_merge_dialog.h" "%PROJ%\ui\merge_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_format_conversion.h" "%PROJ%\ui\format_conversion.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_auto_edge_match.h" "%PROJ%\ui\auto_edge_match.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_db_config_dialog.h" "%PROJ%\ui_class\db_config_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_login_dialog.h" "%PROJ%\ui_class\login_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_product_storage_dialog.h" "%PROJ%\ui_class\product_storage_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_metadata_manager_dialog.h" "%PROJ%\ui_class\metadata_manager_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_access_control_dialog.h" "%PROJ%\ui_class\access_control_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_gdb_layer_selector_dialog.h" "%PROJ%\ui_class\gdb_layer_selector_dialog.ui"
if %errorlevel% neq 0 exit /b %errorlevel%
"%UIC%" -o "%GEN%\ui_mapdata_download.h" "%PROJ%\ui_class\mapdata_download.ui"
if %errorlevel% neq 0 exit /b %errorlevel%

echo === MOC generation ===
"%MOC%" "%PROJ%\map_quality_check_tools.h" -o "%GEN%\moc_map_quality_check_tools.cpp"
"%MOC%" "%PROJ%\ui_task\se_geojson2shp.h" -o "%GEN%\moc_se_geojson2shp.cpp"
"%MOC%" "%PROJ%\ui_class\se_vector_format_conversion.h" -o "%GEN%\moc_se_vector_format_conversion.cpp"
"%MOC%" "%PROJ%\ui_class\generalization_config_dialog.h" -o "%GEN%\moc_generalization_config_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\layer_type_select_dialog.h" -o "%GEN%\moc_layer_type_select_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\se_data_import.h" -o "%GEN%\moc_se_data_import.cpp"
"%MOC%" "%PROJ%\ui_class\se_metadata_viewer.h" -o "%GEN%\moc_se_metadata_viewer.cpp"
"%MOC%" "%PROJ%\ui_class\se_auto_quality_check.h" -o "%GEN%\moc_se_auto_quality_check.cpp"
"%MOC%" "%PROJ%\ui_class\se_layer_mapping_dialog.h" -o "%GEN%\moc_se_layer_mapping_dialog.cpp"
"%MOC%" "%PROJ%\map_check_backup_manager.h" -o "%GEN%\moc_map_check_backup_manager.cpp"
"%MOC%" "%PROJ%\ui_class\backup_strategy_edit_dialog.h" -o "%GEN%\moc_backup_strategy_edit_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\se_data_restore.h" -o "%GEN%\moc_se_data_restore.cpp"
"%MOC%" "%PROJ%\ui_class\se_database_connection.h" -o "%GEN%\moc_se_database_connection.cpp"
"%MOC%" "%PROJ%\ui_class\se_data_management.h" -o "%GEN%\moc_se_data_management.cpp"
"%MOC%" "%PROJ%\ui_class\se_data_export.h" -o "%GEN%\moc_se_data_export.cpp"
"%MOC%" "%PROJ%\ui_class\se_data_list_export.h" -o "%GEN%\moc_se_data_list_export.cpp"
"%MOC%" "%PROJ%\ui_class\map_tool_extent_picker.h" -o "%GEN%\moc_map_tool_extent_picker.cpp"
"%MOC%" "%PROJ%\ui_class\param_config_dialog.h" -o "%GEN%\moc_param_config_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\scale_selector_dialog.h" -o "%GEN%\moc_scale_selector_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\clip_dialog.h" -o "%GEN%\moc_clip_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\merge_dialog.h" -o "%GEN%\moc_merge_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\format_conversion_dialog.h" -o "%GEN%\moc_format_conversion_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\auto_edge_match_dialog.h" -o "%GEN%\moc_auto_edge_match_dialog.cpp"
"%MOC%" "%PROJ%\ui_task\se_clip_merge_task.h" -o "%GEN%\moc_se_clip_merge_task.cpp"
"%MOC%" "%PROJ%\ui_task\se_edge_match_task.h" -o "%GEN%\moc_se_edge_match_task.cpp"
"%MOC%" "%PROJ%\ui_task\se_format_convert_task.h" -o "%GEN%\moc_se_format_convert_task.cpp"
"%MOC%" "%PROJ%\ui_task\se_wuji_process_runner.h" -o "%GEN%\moc_se_wuji_process_runner.cpp"

"%MOC%" "%PROJ%\ui_class\db_config_dialog.h" -o "%GEN%\moc_db_config_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\login_dialog.h" -o "%GEN%\moc_login_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\product_storage_dialog.h" -o "%GEN%\moc_product_storage_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\metadata_manager_dialog.h" -o "%GEN%\moc_metadata_manager_dialog.cpp"
"%MOC%" "%PROJ%\ui_class\access_control_dialog.h" -o "%GEN%\moc_access_control_dialog.cpp"
"%MOC%" "%PROJ%\database\postgis_connector.h" -o "%GEN%\moc_postgis_connector.cpp"
"%MOC%" "%PROJ%\database\product_dao.h" -o "%GEN%\moc_product_dao.cpp"
"%MOC%" "%PROJ%\database\schema_manager.h" -o "%GEN%\moc_schema_manager.cpp"
"%MOC%" "%PROJ%\core\metadata_extractor.h" -o "%GEN%\moc_metadata_extractor.cpp"
"%MOC%" "%PROJ%\core\file_storage_manager.h" -o "%GEN%\moc_file_storage_manager.cpp"
"%MOC%" "%PROJ%\ui_class\gdb_layer_selector_dialog.h" -o "%GEN%\moc_gdb_layer_selector_dialog.cpp"
"%MOC%" "%PROJ%\core\calendar_helper.h" -o "%GEN%\moc_calendar_helper.cpp"
"%MOC%" "%PROJ%\core\data_importer.h" -o "%GEN%\moc_data_importer.cpp"
"%MOC%" "%PROJ%\ui_class\data_import_wizard.h" -o "%GEN%\moc_data_import_wizard.cpp"

echo === DONE ===
