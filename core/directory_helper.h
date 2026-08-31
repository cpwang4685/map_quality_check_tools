#ifndef DIRECTORY_HELPER_H
#define DIRECTORY_HELPER_H

#include <QSet>
#include <QString>
#include <functional>
#include "database/product_dao.h"
#include "database/product_metadata.h"

/**
 * @brief 地图成果固定目录树辅助工具
 *
 * 固定目录树结构（根节点为数据库名，展示用，不建库）：
 *   地图成果（根）
 *   ├─ 制图成果
 *   │   ├─ AI
 *   │   ├─ PDF
 *   │   └─ 其它
 *   ├─ 制图要素
 *   │   ├─ 影像
 *   │   ├─ 晕渲
 *   │   └─ 要素集
 *   └─ 制图资料
 *       ├─ 文档资料
 *       ├─ 表格资料
 *       └─ 其它
 *
 * 该结构持久化到 product_directory 表中（制图成果/制图要素/制图资料 为根级，
 * 即 parent_id = 0），子节点挂在其下。数据导入时按文件格式自动归类挂载。
 */

namespace DirectoryHelper
{

/** @brief 固定第一级目录名称 */
inline const QString kDrawingResults = QStringLiteral("制图成果");   // 制图成果
inline const QString kDrawingElements = QStringLiteral("制图要素");  // 制图要素
inline const QString kDrawingMaterials = QStringLiteral("制图资料"); // 制图资料

/** @brief 制图成果下的第二级 */
inline const QString kResultAI     = QStringLiteral("AI");
inline const QString kResultPDF    = QStringLiteral("PDF");
inline const QString kResultOther  = QStringLiteral("其它");

/** @brief 制图要素下的第二级 */
inline const QString kElementImage    = QStringLiteral("影像");
inline const QString kElementShading  = QStringLiteral("晕渲");
inline const QString kElementFeature  = QStringLiteral("要素集");

/** @brief 制图资料下的第二级 */
inline const QString kMaterialDoc     = QStringLiteral("文档资料");
inline const QString kMaterialTable   = QStringLiteral("表格资料");
inline const QString kMaterialOther   = QStringLiteral("其它");

/**
 * @brief 栅格数据子类别（影像 / 晕渲）
 */
enum class RasterSubcategory
{
	Image,    // 影像
	Shading   // 晕渲
};

/**
 * @brief 固定目录节点 ID 集合
 */
struct FixedDirectoryIds
{
	int drawingResults = -1;  // 制图成果
	int resultAI       = -1;
	int resultPDF      = -1;
	int resultOther    = -1;
	int drawingElements = -1; // 制图要素
	int elementImage   = -1;
	int elementShading = -1;
	int elementFeature = -1;  // 要素集
	int drawingMaterials = -1;// 制图资料
	int materialDoc    = -1;
	int materialTable  = -1;
	int materialOther  = -1;

	bool valid() const { return drawingResults > 0 && drawingElements > 0 && drawingMaterials > 0; }
};

/**
 * @brief 幂等创建固定目录树，返回各节点 ID
 * @param dao 数据访问对象
 */
inline FixedDirectoryIds ensureFixedDirectories(ProductDAO& dao)
{
	FixedDirectoryIds ids;

	// 第一级
	ids.drawingResults  = dao.findOrCreateDirectory(kDrawingResults, 0);
	ids.drawingElements = dao.findOrCreateDirectory(kDrawingElements, 0);
	ids.drawingMaterials= dao.findOrCreateDirectory(kDrawingMaterials, 0);

	// 制图成果 → 第二级
	ids.resultAI    = dao.findOrCreateDirectory(kResultAI, ids.drawingResults);
	ids.resultPDF   = dao.findOrCreateDirectory(kResultPDF, ids.drawingResults);
	ids.resultOther = dao.findOrCreateDirectory(kResultOther, ids.drawingResults);

	// 制图要素 → 第二级
	ids.elementImage   = dao.findOrCreateDirectory(kElementImage, ids.drawingElements);
	ids.elementShading = dao.findOrCreateDirectory(kElementShading, ids.drawingElements);
	ids.elementFeature = dao.findOrCreateDirectory(kElementFeature, ids.drawingElements);

	// 制图资料 → 第二级
	ids.materialDoc    = dao.findOrCreateDirectory(kMaterialDoc, ids.drawingMaterials);
	ids.materialTable  = dao.findOrCreateDirectory(kMaterialTable, ids.drawingMaterials);
	ids.materialOther  = dao.findOrCreateDirectory(kMaterialOther, ids.drawingMaterials);

	return ids;
}

/**
 * @brief 根据文件格式解析所属的固定目录节点
 * @param ext 文件扩展名（小写，不含点）
 * @param isRaster 是否为栅格数据
 * @param rasterSub 栅格子类别（影像/晕渲）
 * @param ids 固定目录节点 ID
 * @return 目标目录节点 ID（-1 表示解析失败）
 */
inline int resolveDirectoryIdByExt(const QString& ext, bool isRaster,
	RasterSubcategory rasterSub, const FixedDirectoryIds& ids)
{
	QString e = ext.toLower();

	// ── 栅格数据：按子类别挂到影像 / 晕渲 ──
	if (isRaster)
		return (rasterSub == RasterSubcategory::Shading) ? ids.elementShading : ids.elementImage;

	// ── 制图成果 ──
	if (e == "ai")
		return ids.resultAI;
	if (e == "pdf")
		return ids.resultPDF;
	// 其它制图格式（cdr / dwg / dxf 等）
	if (e == "cdr" || e == "dwg" || e == "dxf")
		return ids.resultOther;

	// ── 制图资料 ──
	if (e == "doc" || e == "docx")
		return ids.materialDoc;
	if (e == "xls" || e == "xlsx")
		return ids.materialTable;

	// ── 其它格式统一挂到制图资料的"其它"节点 ──
	return ids.materialOther;
}

/**
 * @brief 递归收集指定目录及其所有子目录的 ID（含自身）
 * @param dao 数据访问对象
 * @param dirId 起始目录 ID
 * @param out 输出的目录 ID 集合
 */
inline void collectDirectoryIds(ProductDAO& dao, int dirId, QSet<int>& out)
{
	if (dirId <= 0) return;
	out.insert(dirId);
	const auto children = dao.getChildDirectories(dirId);
	for (const auto& child : children)
		collectDirectoryIds(dao, child.id, out);
}

/**
 * @brief 在指定固定节点下创建/获取一个以名称命名的子节点
 * @param dao 数据访问对象
 * @param parentDirId 固定节点 ID
 * @param name 子节点名称（如矢量文件夹名、文件名、gdb 名）
 * @param nodeType 节点类型（0=普通目录，1=图层节点）
 * @return 子节点目录 ID
 */
inline int createNamedChildDirectory(ProductDAO& dao, int parentDirId, const QString& name, int nodeType = 0)
{
	if (parentDirId <= 0) return parentDirId;
	QString trimmed = name.trimmed();
	if (trimmed.isEmpty()) return parentDirId;
	return dao.findOrCreateDirectory(trimmed, parentDirId, nodeType);
}

/**
 * @brief 清理本次导入中新建但未成功挂载任何产品的空目录节点
 *
 * 仅在目录下既没有产品、也没有子目录时才删除（避免误删有历史产品的目录）。
 * 返回清理的数量。
 *
 * @param dao 数据访问对象
 * @param usedDirs 本次导入动态使用的命名目录 id 集合（Vector 分支创建命名节点时记录）
 * @param populatedDirs 本次导入成功挂载产品的目录 id 集合（insertProduct 成功时记录）
 * @param logCallback 可选日志回调（QString 为日志内容）
 */
inline int cleanupEmptyCreatedDirs(ProductDAO& dao, const QSet<int>& usedDirs,
	const QSet<int>& populatedDirs,
	const std::function<void(const QString&)>& logCallback = nullptr)
{
	int removed = 0;
	for (int dirId : usedDirs)
	{
		if (dirId <= 0) continue;
		// 本次有产品挂载 → 保留
		if (populatedDirs.contains(dirId)) continue;
		// 该目录下仍存在历史产品 → 保留
		if (!dao.getProductsByDirectory(dirId).isEmpty()) continue;
		// 该目录存在子目录 → 保留（子目录可能有产品）
		if (!dao.getChildDirectories(dirId).isEmpty()) continue;

		if (dao.deleteDirectory(dirId))
		{
			++removed;
			if (logCallback)
				logCallback(QString::fromUtf8("  🗑 入库失败，已清理空目录节点"));
		}
	}
	return removed;
}

} // namespace DirectoryHelper

#endif // DIRECTORY_HELPER_H
