# map_quality_check_tools — 测绘成果质量检查与地图综合工具（QGIS 插件）

基于 **QGIS 3.28 C++ 二次开发**的测绘数据处理插件，运行于自主可控的 LTZK 桌面平台（QGIS 二次开发平台）。
**支持 Windows x64 与 银河麒麟 V10（x86_64）双平台**，是典型的国产化替代 GIS 工程。

> 插件以 `QLibrary` 动态加载方式集成进平台，平台负责地图画布、图层管理、数据库连接等基础能力，
> 插件专注业务功能：质量检查、地图综合、合并接边、格式转换、数据管理等。

---

## 功能特性

### 🕵️ 成果质量检查
- 多类自动质检任务（450+ 系列检查项：悬挂点、伪节点、重叠、拓扑一致性等）
- 基于 XML 规则配置的检查项管理，可按线/面要素类型定制
- 检查结果可视化定位与一键修复辅助

### 🗺️ 地图综合（缩编）
- 基于 NMO 地理处理 SDK 的面溶解、线连接等综合算子
- 面/线按几何类型**自动分流**：面→溶解（Mission 327）、线→连接（Mission 175）
- 分组/连接字段自动推荐（跨文件字段交集 + 编码自动判定 + 最优字段排序）
- 地理坐标自动投影（EPSG:3857）执行后转回原坐标系，保证距离计算精度

### 🔗 合并 / 接边
- 多幅面要素溶解合并、多幅线要素断线智能连接（接边）
- 结点拟合容差、连接模式（同层/跨层）等高级参数自适应
- 多文件输出自动合并为单文件

### 🔄 格式转换
- SHP / GPKG / GDB / MDB 等矢量格式互转，单文件与**批量转换**模式
- 编码处理（GBK/UTF-8 自动识别归一化），保证中文属性不乱码
- 图形化源数据树形选择（盘符懒加载、多选勾选）

### 💾 数据管理
- PostgreSQL/PostGIS 数据导入导出（含 psql 管道、批量入库）
- 定时/手动备份、过期清理、恢复（支持恢复到数据库或文件系统）
- 元数据管理、成果管理、图层映射配置

### 🏗️ 平台集成
- 登录鉴权、用户权限控制、参数配置、地图下载等平台能力

---

## 技术栈

| 类别 | 选型 |
|---|---|
| 语言 | C++17 |
| GUI / 平台 | Qt 5.12（麒麟）/ Qt 5.15（Windows）、QGIS 3.28 C++ API |
| 空间数据 | GDAL/OGR、SpatiaLite |
| 数据库 | PostgreSQL / PostGIS |
| 综合算子 | NMO 地理处理 SDK（可选依赖，缺失时功能自动置灰） |
| JSON | nlohmann/json（单头库） |
| 构建 | CMake（跨平台）/ MSBuild + Qt VS Tools（Windows） |

## 跨平台设计

同一份源码通过 `Q_OS_WIN` / `SE_NMO_NO_SDK` 等宏守卫同时产出 Windows 与麒麟版本：

- **麒麟**：系统 Qt 5.12 + QGIS SDK 3.28.15，CMake 构建，链接系统 GDAL 3.0 / PROJ 6 / GEOS 3.8
- **NMO SDK 守卫**：麒麟暂缺 SDK 时用 `SE_NMO_NO_SDK` 宏剔除 NMO 头与调用，相关功能置灰提示，SDK 就绪后翻转宏开关即可启用
- 模板/资源路径统一走运行时探测，适配不同部署布局

## 构建

### Windows（MSVC）
1. 安装 QGIS 3.28 开发环境（OSGeo4W）与 Qt 5.15
2. VS 打开 `map_quality_check_tools.sln`，编译 Release|x64
3. 产物：`plugin_map_quality_check_tools.dll`（uic/moc 由 vcxproj 自动生成）

### Linux / 麒麟 V10（CMake）
```bash
mkdir build && cd build
cmake .. -DQGIS_SDK_DIR=/path/to/qgis-3.28.15 \
         -DSE_NMO_SDK_ENABLED=OFF   # NMO SDK 就绪后置 ON
make
```

---

## 目录结构

```
├── ui_class/       # 业务对话框（质检/综合/合并/接边/格式转换/数据管理…）
├── ui_task/        # 后台任务与 SDK 桥接（NMO bridge、格式转换 task、XML 模板生成…）
├── ui/             # Qt Designer 界面文件 (.ui)
├── database/       # PostgreSQL 数据访问层（DAO / schema）
├── core/           # 核心工具（数据导入等）
├── config/         # 检查规则、缩编管线等 XML 配置
├── images/         # 图标资源
├── xml/            # 合并/接边任务模板
├── docs/           # 使用说明文档
├── CMakeLists.txt  # 跨平台构建（含 NMO 守卫分支）
└── *.vcxproj / *.sln  # Windows 构建工程
```

## License

（请在此填写你的开源许可证，例如 MIT / Apache-2.0）
