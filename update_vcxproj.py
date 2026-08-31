"""Add new build entries to vcxproj for data export/backup restore integration."""
import re

with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj', 'r', encoding='utf-8') as f:
    content = f.read()

PROJ = r'D:\GarMap\qgis_plugins\cplusplus\map_quality_check_tools'
UIC_EXE = r'D:\LTZK\OSGeo4W_32815\apps\Qt5\bin\uic.exe'
MOC_EXE = r'D:\LTZK\OSGeo4W_32815\apps\Qt5\bin\moc.exe'

# ---- 1. Add UIC CustomBuild entries for 4 new .ui files ----
# Insert after the last </CustomBuild> of existing UI entries (auto_quality_check.ui)
uic_entries = r'''
    <CustomBuild Include="ui\data_restore.ui">
      <FileType>Document</FileType>
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{uic}" -o  {proj}\GeneratedFiles\Release\ui_data_restore.h   {proj}\ui\data_restore.ui
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">UIC ui_data_restore.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\Release\ui_data_restore.h;%%(Outputs)</Outputs>
      <LinkObjects Condition="'$(Configuration)|$(Platform)'=='Release|x64'">false</LinkObjects>
    </CustomBuild>
    <CustomBuild Include="ui\database_connection.ui">
      <FileType>Document</FileType>
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{uic}" -o  {proj}\GeneratedFiles\Release\ui_database_connection.h   {proj}\ui\database_connection.ui
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">UIC ui_database_connection.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\Release\ui_database_connection.h;%%(Outputs)</Outputs>
      <LinkObjects Condition="'$(Configuration)|$(Platform)'=='Release|x64'">false</LinkObjects>
    </CustomBuild>
    <CustomBuild Include="ui\data_management.ui">
      <FileType>Document</FileType>
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{uic}" -o  {proj}\GeneratedFiles\Release\ui_data_management.h   {proj}\ui\data_management.ui
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">UIC ui_data_management.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\Release\ui_data_management.h;%%(Outputs)</Outputs>
      <LinkObjects Condition="'$(Configuration)|$(Platform)'=='Release|x64'">false</LinkObjects>
    </CustomBuild>
    <CustomBuild Include="ui\data_export.ui">
      <FileType>Document</FileType>
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{uic}" -o  {proj}\GeneratedFiles\Release\ui_data_export.h   {proj}\ui\data_export.ui
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">UIC ui_data_export.h</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\Release\ui_data_export.h;%%(Outputs)</Outputs>
      <LinkObjects Condition="'$(Configuration)|$(Platform)'=='Release|x64'">false</LinkObjects>
    </CustomBuild>
'''.format(uic=UIC_EXE, proj=PROJ)

# Insert after the last UIC CustomBuild block (auto_quality_check.ui closing tag)
insert_marker = '</CustomBuild>\n  </ItemGroup>\n  <ItemGroup>\n    <!-- ====== 已有源文件 ====== -->'
uic_entries_stripped = uic_entries.strip()
content = content.replace(insert_marker, '</CustomBuild>' + uic_entries_stripped + '\n  </ItemGroup>\n  <ItemGroup>\n    <!-- ====== 已有源文件 ====== -->')

# ---- 2. Add new MOC ClCompile entries ----
moc_clcompile = r'''
    <ClCompile Include="GeneratedFiles\Release\moc_map_check_backup_manager.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_backup_strategy_edit_dialog.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_se_data_restore.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_se_database_connection.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_se_data_management.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_se_data_export.cpp" />
    <ClCompile Include="GeneratedFiles\Release\moc_map_tool_extent_picker.cpp" />
'''.rstrip()

# Insert after existing moc ClCompile entries
# Find the last moc ClCompile entry and insert after it
old_moc_compile = '<ClCompile Include="GeneratedFiles\\Release\\moc_se_auto_quality_check.cpp" />'
content = content.replace(old_moc_compile, old_moc_compile + moc_clcompile)

# ---- 3. Add new source ClCompile entries ----
src_clcompile = r'''
    <!-- ====== 数据导出 & 备份恢复 ====== -->
    <ClCompile Include="map_check_backup_manager.cpp" />
    <ClCompile Include="ui_class\se_data_restore.cpp" />
    <ClCompile Include="ui_class\se_database_connection.cpp" />
    <ClCompile Include="ui_class\se_data_management.cpp" />
    <ClCompile Include="ui_class\se_data_export.cpp" />
    <ClCompile Include="ui_class\map_tool_extent_picker.cpp" />
    <ClCompile Include="ui_class\backup_strategy_edit_dialog.cpp" />
'''.rstrip()

# Insert after existing source ClCompile entries
# Find the last source ClCompile before the geo_algorithm one
old_src = '<ClCompile Include="ui_class\\se_db_manager.cpp" />'
content = content.replace(old_src, old_src + src_clcompile)

# ---- 4. Add MOC CustomBuild entries for new Q_OBJECT headers ----
moc_cb = r'''
    <CustomBuild Include="map_check_backup_manager.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\backup_strategy_edit_dialog.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\se_data_restore.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\se_database_connection.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\se_data_management.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\se_data_export.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
    <CustomBuild Include="ui_class\map_tool_extent_picker.h">
      <Command Condition="'$(Configuration)|$(Platform)'=='Release|x64'">setlocal
	set "PATH=D:\LTZK\OSGeo4W_32815\bin;D:\LTZK\OSGeo4W_32815\apps\Qt5\bin;%%PATH%%"
	"{moc}"  "%%(FullPath)"  -o  "{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp"
	if %%errorlevel%% neq 0 goto :cmEnd
	:cmEnd
	endlocal &amp; call :cmErrorLevel %%errorlevel%% &amp; goto :cmDone
	:cmErrorLevel
	exit /b %%1
	:cmDone
	if %%errorlevel%% neq 0 goto :VCEnd</Command>
      <Message Condition="'$(Configuration)|$(Platform)'=='Release|x64'">Moc%%27ing %%(FullPath)</Message>
      <Outputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">{proj}\GeneratedFiles\$(ConfigurationName)\moc_%%(Filename).cpp</Outputs>
      <AdditionalInputs Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(QTDIR_OSGEO4W)\bin\moc.exe;%%(FullPath);%%(AdditionalInputs)</AdditionalInputs>
    </CustomBuild>
'''.format(moc=MOC_EXE, proj=PROJ)

# Insert after the last MOC CustomBuild (se_auto_quality_check.h)
old_moc_cb = '</CustomBuild>\n  </ItemGroup>\n  <ItemGroup>\n    <CustomBuild Include="ui_class\\se_vector_format_conversion.h">'
content = content.replace(old_moc_cb, '</CustomBuild>' + moc_cb.strip() + '\n  </ItemGroup>\n  <ItemGroup>\n    <CustomBuild Include="ui_class\\se_vector_format_conversion.h">')

# ---- 5. Add ClInclude entries for generated UI headers ----
cli_entries = r'''
    <ClInclude Include="GeneratedFiles\Release\ui_data_restore.h" />
    <ClInclude Include="GeneratedFiles\Release\ui_database_connection.h" />
    <ClInclude Include="GeneratedFiles\Release\ui_data_management.h" />
    <ClInclude Include="GeneratedFiles\Release\ui_data_export.h" />
'''.rstrip()

old_cli = '<ClInclude Include="GeneratedFiles\\Release\\ui_auto_quality_check.h" />'
content = content.replace(old_cli, old_cli + cli_entries)

with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj', 'w', encoding='utf-8') as f:
    f.write(content)
print("vcxproj updated successfully")
