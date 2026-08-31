"""Add new file entries to vcxproj.filters."""
with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj.filters', 'r', encoding='utf-8') as f:
    content = f.read()

# Add new MOC ClCompile entries
moc_entries = [
    'moc_map_check_backup_manager.cpp',
    'moc_backup_strategy_edit_dialog.cpp',
    'moc_se_data_restore.cpp',
    'moc_se_database_connection.cpp',
    'moc_se_data_management.cpp',
    'moc_se_data_export.cpp',
    'moc_map_tool_extent_picker.cpp',
]
moc_xml = ''.join(f'\n    <ClCompile Include="GeneratedFiles\\Release\\{m}">\n      <Filter>Generated Files</Filter>\n    </ClCompile>' for m in moc_entries)
# Insert after the last moc in the ClCompile section
old = '<ClCompile Include="..\\..\\..\\geo_algorithm_h\\vector\\cse_generalization.cpp" />'
content = content.replace(old, old + moc_xml)

# Add new source ClCompile entries
src_entries = [
    ('map_check_backup_manager.cpp', 'Source Files'),
    ('ui_class\\se_data_restore.cpp', 'ui_class'),
    ('ui_class\\se_database_connection.cpp', 'ui_class'),
    ('ui_class\\se_data_management.cpp', 'ui_class'),
    ('ui_class\\se_data_export.cpp', 'ui_class'),
    ('ui_class\\map_tool_extent_picker.cpp', 'ui_class'),
    ('ui_class\\backup_strategy_edit_dialog.cpp', 'ui_class'),
]
src_xml = '\n'.join(f'    <ClCompile Include="{s}">\n      <Filter>{f}</Filter>\n    </ClCompile>' for s, f in src_entries)
content = content.replace(old + moc_xml, old + moc_xml + '\n' + src_xml)

# Add new MOC CustomBuild entries
moc_cb_entries = [
    'map_check_backup_manager.h',
    'ui_class\\backup_strategy_edit_dialog.h',
    'ui_class\\se_data_restore.h',
    'ui_class\\se_database_connection.h',
    'ui_class\\se_data_management.h',
    'ui_class\\se_data_export.h',
    'ui_class\\map_tool_extent_picker.h',
]
cb_xml = ''.join(f'\n    <CustomBuild Include="{h}" />' for h in moc_cb_entries)
# Insert before the closing of the CustomBuild section (last existing CustomBuild is auto_quality_check.ui)
old_cb = '<CustomBuild Include="ui\\auto_quality_check.ui">'
content = content.replace(old_cb, cb_xml + '\n    ' + old_cb)

# Add UIC CustomBuild entries for new .ui files
uic_entries = [
    'ui\\data_restore.ui',
    'ui\\database_connection.ui',
    'ui\\data_management.ui',
    'ui\\data_export.ui',
]
uic_xml = ''.join(f'\n    <CustomBuild Include="{u}">\n      <Filter>ui</Filter>\n    </CustomBuild>' for u in uic_entries)
# Insert after the last UI CustomBuild (auto_quality_check.ui)
old_ui = '<CustomBuild Include="ui\\auto_quality_check.ui">\n      <Filter>ui</Filter>\n    </CustomBuild>'
content = content.replace(old_ui, old_ui + uic_xml)

# Add ClInclude entries for new generated UI headers
cli_entries = [
    'ui_data_restore.h',
    'ui_database_connection.h',
    'ui_data_management.h',
    'ui_data_export.h',
]
cli_xml = ''.join(f'\n    <ClInclude Include="GeneratedFiles\\Release\\{h}">\n      <Filter>Generated Files</Filter>\n    </ClInclude>' for h in cli_entries)
old_cli = '<ClInclude Include="GeneratedFiles\\Release\\ui_auto_quality_check.h">'
content = content.replace(old_cli, cli_xml + '\n    ' + old_cli)

with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj.filters', 'w', encoding='utf-8') as f:
    f.write(content)
print("filters updated successfully")
