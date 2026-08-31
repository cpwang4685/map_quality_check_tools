"""Fix line 28 (0-indexed) in vcxproj to add PATH before uic.exe"""
path_line = 'set "PATH=D:\\LTZK\\OSGeo4W_32815\\bin;D:\\LTZK\\OSGeo4W_32815\\apps\\Qt5\\bin;%PATH%"\r\n'

with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj', 'rb') as f:
    lines = f.readlines()

lines[28] = path_line.encode('ascii')

with open(r'D:/GarMap/qgis_plugins/cplusplus/map_quality_check_tools/map_quality_check_tools.vcxproj', 'wb') as f:
    f.writelines(lines)
print('Line 29 fixed')
