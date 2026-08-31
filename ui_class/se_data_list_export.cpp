/*--------------SE---------------*/
#include "se_data_list_export.h"

/*--------------QT---------------*/
#include <QFileDialog>
// 【2026-08-24】与数据库连接配置 UI 共享连接需要读取 QSettings
#include <QSettings>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QApplication>
#include <QProgressDialog>
#include <QTimer>
#include <QPointer>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QComboBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QMenu>
#include <QAction>
#include <QGroupBox>
#include <QGridLayout>
#include <QCheckBox>
#include <QListWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedWidget>
#include <QWidget>
#include <QTextEdit>
#include <QStyle>
#include <QSet>
#include <QEventLoop>
#include <cmath>
#include <algorithm>

/*--------------GDAL/OGR---------------*/
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>
#include <cpl_string.h>

/*--------------QGIS---------------*/
#include "se_data_management.h"
#include <qgisinterface.h>
#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsmapmouseevent.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfields.h>
#include <qgsfield.h>
#include <qgsrectangle.h>
#include <qgspointxy.h>
#include <qgswkbtypes.h>
#include <qgsproject.h>
#include <qgsmessagelog.h>
#include <qgsmaplayer.h>
#include <qgsdatasourceuri.h>
#include <qgsproviderregistry.h>
#include <qgsogrprovidermetadata.h>
#include <qgsprovidersublayerdetails.h>
#include <qgscoordinatetransform.h>
#include <qgsunittypes.h>

/* 数据库连接对话框 */
#include "se_database_connection.h"

/* 数据库访问 */
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

// ============================================================
// CRS 转换辅助（加载居中 / 导出范围过滤共用）
// ============================================================
// 取画布当前显示 CRS（destinationCrs）；canvas 为空返回无效 CRS
static QgsCoordinateReferenceSystem seCanvasDestCrs(QgsMapCanvas* canvas)
{
	if (!canvas) return QgsCoordinateReferenceSystem();
	return canvas->mapSettings().destinationCrs();
}

// 把矩形从 from 转换到 to：任一无效或两 CRS 相同则原样返回；转换抛异常也原样返回，
// 保证导出/缩放流程绝不因 CRS 问题中断
static QgsRectangle seXformRect(const QgsRectangle& rect,
	const QgsCoordinateReferenceSystem& from, const QgsCoordinateReferenceSystem& to)
{
	if (!rect.isNull() && from.isValid() && to.isValid() && from != to)
	{
		QgsCoordinateTransform tr(from, to, QgsProject::instance());
		if (tr.isValid())
		{
			try { return tr.transformBoundingBox(rect); }
			catch (...) { }
		}
	}
	return rect;
}

// 用户选的输出路径若带数据扩展名则视为"该文件所在目录"，否则本身当作目录。
// 用于多数据源（目录/GDB 整体）导出时把输出定位到用户意图的文件夹
static QString outputDirFromPick(const QString& pick)
{
	QString lower = pick.toLower();
	if (lower.endsWith(".shp") || lower.endsWith(".tif") || lower.endsWith(".tiff")
		|| lower.endsWith(".gpkg") || lower.endsWith(".gdb") || lower.endsWith(".img"))
		return QFileInfo(pick).absolutePath();
	return pick;
}

// ============================================================
// 纸张尺寸辅助（主区裁切导出"计算"按钮 + 纸张尺寸解析共用）
// ============================================================
// ISO 216 A 系列纸张（宽=长边、高=短边，mm）
struct SeIsoPaper { const char* name; double w; double h; };
static const SeIsoPaper kSeIsoPapers[] = {
	{ "A0", 1189, 841 },
	{ "A1", 841, 594 },
	{ "A2", 594, 420 },
	{ "A3", 420, 297 },
	{ "A4", 297, 210 },
};

// 解析纸张规格文本："A0" / "A0：893.0*654.0" / "A1：677.0*496.0"。
// 带组合文本时只取纸张系列前缀，后缀为展示用的绘图尺寸。
// 返回 true 且填充 w/h（宽=长边、高=短边）；"自定义"或未知文本返回 false
static bool seParsePaperSpec(const QString& text, double& w, double& h)
{
	QString sz = text.trimmed();
	int sep = sz.indexOf(QStringLiteral("："));
	if (sep < 0) sep = sz.indexOf(QStringLiteral(":"));
	if (sep >= 0) sz = sz.left(sep).trimmed();
	const QString upper = sz.toUpper();
	for (const SeIsoPaper& p : kSeIsoPapers)
		if (upper == QLatin1String(p.name)) { w = p.w; h = p.h; return true; }
	return false;
}

// ============================================================
// 手动绘制地图工具（真正的 QgsMapTool 子类）
// 说明：不使用 QgsGeometry 值对象，避免触发虚表符号；直接跟踪坐标点计算外包矩形
// ============================================================
// 【2026-08-25 临时调试】绘制流程阶段探针：写临时日志文件定位闪退点（定位后删除）
static void dbgRangeLog(const QString& tag)
{
	QFile f(QDir::tempPath() + "/rangeexport_dbg.log");
	if (f.open(QIODevice::Append | QIODevice::Text))
	{
		QByteArray line = (QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + "  " + tag).toUtf8();
		f.write(line + "\n");
		f.close();
	}
}

// 【2026-08-25 临时调试】dump canvas 及 viewport 的 children 地址（只读地址不反引用），
// 用于和崩溃 receiver 地址比对，确认悬空 child 是否来自 canvas 子树（定位后删除）
static void dbgDumpCanvasChildren(QgsMapCanvas* canvas)
{
	if (!canvas) return;
	{
		QString s = "canvas=" + QString::number((quintptr)canvas, 16) + " children:";
		const QObjectList& kids = canvas->children();
		for (const QObject* k : kids)
			s += " " + QString::number((quintptr)k, 16);
		dbgRangeLog(s);
	}
	if (QWidget* vp = canvas->viewport())
	{
		QString s = "canvas viewport=" + QString::number((quintptr)vp, 16) + " children:";
		const QObjectList& kids = vp->children();
		for (const QObject* k : kids)
			s += " " + QString::number((quintptr)k, 16);
		dbgRangeLog(s);
	}
}

#ifdef _WIN32
// 【2026-08-25 临时调试】崩溃调用栈记录：段错误（0xc0000005）不是 C++ 异常，catch(...) 拦不住，
// 只能装 UnhandledExceptionFilter，在崩溃点用 StackWalk64 从异常上下文回溯真实调用栈，
// 以确定 isWidgetType() 是被哪个信号/事件激活调用、sender 是谁（定位后删除）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

namespace
{
	// 读指针前先查页可读性，避免过滤器内二次崩溃（无 __try/对象展开，兼容 /EHsc）
	static quintptr safeReadPtr(quintptr addr)
	{
		if (!addr) return 0;
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
			&& (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
			&& !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return *(quintptr*)addr;
		return 0;
	}

	LONG WINAPI rangeExportCrashFilter(_EXCEPTION_POINTERS* ep)
	{
		QFile f(QDir::tempPath() + "/rangeexport_dbg.log");
		if (f.open(QIODevice::Append | QIODevice::Text))
		{
			QByteArray line;
			line += QDateTime::currentDateTime().toString("HH:mm:ss.zzz").toUtf8();
			line += "  CRASH code=0x" + QByteArray::number((unsigned)ep->ExceptionRecord->ExceptionCode, 16);
			line += " pc=" + QByteArray::number((quintptr)ep->ExceptionRecord->ExceptionAddress, 16);
			if (ep->ExceptionRecord->ExceptionCode == 0xc0000005 && ep->ExceptionRecord->NumberParameters >= 2)
				line += " readfault=" + QByteArray::number((quintptr)ep->ExceptionRecord->ExceptionInformation[1], 16);
			line += "\n";
			f.write(line);

			// isWidgetType() 在崩溃瞬间尚未改写 RCX：RCX = receiver(this)，读其 vtable 可定类
			if (ep->ContextRecord)
			{
#ifdef _M_X64
				quintptr recv = (quintptr)ep->ContextRecord->Rcx;
#else
				quintptr recv = (quintptr)ep->ContextRecord->Ecx;
#endif
				quintptr vtable = safeReadPtr(recv);
				QByteArray rl = "  receiver=" + QByteArray::number(recv, 16);
				if (vtable)
				{
					HMODULE mod = nullptr;
					if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						(LPCWSTR)vtable, &mod))
					{
						WCHAR wname[MAX_PATH];
						DWORD n = GetModuleFileNameW(mod, wname, MAX_PATH);
						rl += " vtable=" + QString::fromWCharArray(wname, n).section('\\', -1).toUtf8()
							+ "+0x" + QByteArray::number(vtable - (quintptr)mod, 16);
					}
					else
					{
						rl += " vtable=" + QByteArray::number(vtable, 16) + "(?)";
					}
				}
				rl += "\n";
				f.write(rl);

				// 寄存器快照（paintSiblingsRecursive 的 this=父 QWidgetPrivate，常在 R13/R14/RDI 中）
				QByteArray regs;
#ifdef _M_X64
				CONTEXT* cr = ep->ContextRecord;
				regs += "  regs rax=" + QByteArray::number(cr->Rax, 16) + " rbx=" + QByteArray::number(cr->Rbx, 16)
					+ " rcx=" + QByteArray::number(cr->Rcx, 16) + " rdx=" + QByteArray::number(cr->Rdx, 16) + "\n";
				regs += "        rsi=" + QByteArray::number(cr->Rsi, 16) + " rdi=" + QByteArray::number(cr->Rdi, 16)
					+ " rbp=" + QByteArray::number(cr->Rbp, 16) + " rsp=" + QByteArray::number(cr->Rsp, 16) + "\n";
				regs += "        r8=" + QByteArray::number(cr->R8, 16) + " r9=" + QByteArray::number(cr->R9, 16)
					+ " r10=" + QByteArray::number(cr->R10, 16) + " r11=" + QByteArray::number(cr->R11, 16)
					+ " r12=" + QByteArray::number(cr->R12, 16) + " r13=" + QByteArray::number(cr->R13, 16)
					+ " r14=" + QByteArray::number(cr->R14, 16) + " r15=" + QByteArray::number(cr->R15, 16) + "\n";
#endif
				f.write(regs);
				// receiver 内存快照（前 128 字节，判断 freed/reused 后内容是堆数据还是对象残留）
				QByteArray mem = "  receiver mem:";
				for (int i = 0; i < 16; ++i)
					mem += " " + QByteArray::number(safeReadPtr(recv + (quintptr)i * 8), 16);
				mem += "\n";
				f.write(mem);
			}

			// StackWalk64 从 dbghelp.dll 运行时加载，避免静态链接依赖
			static HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
			typedef BOOL(WINAPI* StackWalk64Fn)(DWORD, HANDLE, HANDLE, STACKFRAME64*, PVOID,
				PREAD_PROCESS_MEMORY_ROUTINE, PFUNCTION_TABLE_ACCESS_ROUTINE, PGET_MODULE_BASE_ROUTINE, PTRANSLATE_ADDRESS_ROUTINE);
			static StackWalk64Fn sw64 = dbghelp ? (StackWalk64Fn)GetProcAddress(dbghelp, "StackWalk64") : nullptr;
			static PFUNCTION_TABLE_ACCESS_ROUTINE ftab = dbghelp ? (PFUNCTION_TABLE_ACCESS_ROUTINE)GetProcAddress(dbghelp, "SymFunctionTableAccess64") : nullptr;
			static PGET_MODULE_BASE_ROUTINE mbase = dbghelp ? (PGET_MODULE_BASE_ROUTINE)GetProcAddress(dbghelp, "SymGetModuleBase64") : nullptr;

			if (sw64 && ep->ContextRecord)
			{
				CONTEXT* ctx = ep->ContextRecord;
				// 初始化符号/函数表，让 StackWalk64 能走更多帧（无 .pdb 也能走 .pdata 展开）
				typedef BOOL(WINAPI* SymInitFn)(HANDLE, PCSTR, BOOL);
				static SymInitFn symInit = dbghelp ? (SymInitFn)GetProcAddress(dbghelp, "SymInitialize") : nullptr;
				if (symInit) symInit(GetCurrentProcess(), nullptr, TRUE);
				STACKFRAME64 fr;
				memset(&fr, 0, sizeof(fr));
#ifdef _M_X64
				fr.AddrPC.Offset = ctx->Rip;    fr.AddrPC.Mode = AddrModeFlat;
				fr.AddrFrame.Offset = ctx->Rbp; fr.AddrFrame.Mode = AddrModeFlat;
				fr.AddrStack.Offset = ctx->Rsp; fr.AddrStack.Mode = AddrModeFlat;
#else
				fr.AddrPC.Offset = ctx->Eip;    fr.AddrPC.Mode = AddrModeFlat;
				fr.AddrFrame.Offset = ctx->Ebp; fr.AddrFrame.Mode = AddrModeFlat;
				fr.AddrStack.Offset = ctx->Esp; fr.AddrStack.Mode = AddrModeFlat;
#endif
				for (int i = 0; i < 48; ++i)
				{
					if (!sw64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
						&fr, ctx, nullptr, ftab, mbase, nullptr))
						break;
					if (!fr.AddrPC.Offset) break;
					quintptr addr = (quintptr)fr.AddrPC.Offset;
					HMODULE mod = nullptr;
					if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						(LPCWSTR)fr.AddrPC.Offset, &mod))
					{
						WCHAR wname[MAX_PATH];
						DWORD n = GetModuleFileNameW(mod, wname, MAX_PATH);
						// HMODULE 值即模块基址，可直接求 RVA
						QByteArray m = QString::fromWCharArray(wname, n).section('\\', -1).toUtf8();
						f.write("      " + m + " +0x" + QByteArray::number(addr - (quintptr)mod, 16) + "\n");
					}
					else
					{
						f.write("      <unknown> 0x" + QByteArray::number(addr, 16) + "\n");
					}
				}
			}
			f.close();
		}
		return EXCEPTION_CONTINUE_SEARCH; // 交给默认处理，保持原有闪退行为
	}

	void installRangeExportCrashFilter()
	{
		static bool installed = false;
		if (!installed) { SetUnhandledExceptionFilter(rangeExportCrashFilter); installed = true; }
	}
} // namespace
#endif // _WIN32

namespace
{
class QDrawShapeMapTool : public QgsMapTool
{
public:
	std::function<void(double, double, double, double)> m_onDone;
	std::function<void()> m_onCancelled;   // 取消/中断绘制时的回调

	QDrawShapeMapTool(QgsMapCanvas* canvas, DrawShapeType type)
		: QgsMapTool(canvas)
		, m_drawType(type)
	{
	}

	DrawShapeType m_drawType = DrawRect;
	QgsRubberBand* m_rubberBand = nullptr;
	QVector<QgsPointXY> m_points;   // 多边形顶点
	bool m_bFinished = false;
	bool m_bActive = false;         // 自定义工具激活标志
	int m_vertexCount = 0;
	QgsPointXY m_anchor;           // 矩形/圆起始角
	QgsPointXY m_lastPoint;        // 矩形/圆当前边角点
	bool m_bDragging = false;       // 鼠标按下拖拽中

	void activate() override
	{
		QgsMapTool::activate();
		if (!m_rubberBand)
			m_rubberBand = new QgsRubberBand(canvas(), QgsWkbTypes::PolygonGeometry);
		dbgRangeLog(QString("QDrawShapeMapTool::activate this=%1 canvas=%2 rubber=%3")
			.arg((quintptr)this, 0, 16).arg((quintptr)canvas(), 0, 16).arg((quintptr)m_rubberBand, 0, 16));
		m_rubberBand->setColor(QColor(255, 0, 0, 120));
		m_rubberBand->setFillColor(QColor(255, 0, 0, 40));
		m_rubberBand->setWidth(2);
		m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
		m_bFinished = false;
		m_bActive = true;
		m_bDragging = false;
		m_vertexCount = 0;
		m_points.clear();
		m_anchor = QgsPointXY();
		m_lastPoint = QgsPointXY();
	}

	void deactivate() override
	{
		m_bActive = false;
		m_bDragging = false;
		if (m_rubberBand)
		{
			dbgRangeLog(QString("QDrawShapeMapTool::deactivate delete rubber=%1 rbParent=%2")
				.arg((quintptr)m_rubberBand, 0, 16).arg((quintptr)m_rubberBand->parentWidget(), 0, 16));
			delete m_rubberBand;
			m_rubberBand = nullptr;
		}
		m_points.clear();
		// 若尚未完成绘制就被 deactivate（如按 Esc、切换工具等），通知外部取消
		if (!m_bFinished && m_onCancelled)
		{
			m_onCancelled();
			m_bFinished = true; // 防止重复触发
		}
		QgsMapTool::deactivate();
	}

	// 用 anchor + 当前点更新 rubberBand，让橡皮带矩形/圆实时跟踪鼠标
	void updateRubberBand(const QgsPointXY& cur)
	{
		if (!m_rubberBand) return;
		if (m_drawType == DrawRect)
		{
			// 4 个角构成矩形 polygon（任意方向拖动都可正确显示矩形）
			QgsPointXY bottomLeft(std::min(m_anchor.x(), cur.x()),
				std::min(m_anchor.y(), cur.y()));
			QgsPointXY topRight(std::max(m_anchor.x(), cur.x()),
				std::max(m_anchor.y(), cur.y()));
			m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
			m_rubberBand->addPoint(bottomLeft, false);
			m_rubberBand->addPoint(QgsPointXY(topRight.x(), bottomLeft.y()), false);
			m_rubberBand->addPoint(topRight, false);
			m_rubberBand->addPoint(QgsPointXY(bottomLeft.x(), topRight.y()), true);
		}
		else if (m_drawType == DrawCircle)
		{
			double r = m_anchor.distance(cur);
			m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
			for (int i = 0; i <= 36; ++i)
			{
				double a = i * 10.0 * M_PI / 180.0;
				m_rubberBand->addPoint(QgsPointXY(m_anchor.x() + r * cos(a), m_anchor.y() + r * sin(a)));
			}
		}
	}

	void canvasPressEvent(QgsMapMouseEvent* e) override
	{
		if (!m_bActive || m_bFinished) return;
		// 关键：用 toMapCoordinates(e->pos()) 而非 e->mapPoint()，确保坐标转换可靠（CMapToolExtentPicker 同款写法）
		if (m_drawType == DrawRect || m_drawType == DrawCircle)
		{
			if (e->button() == Qt::LeftButton)
			{
				m_anchor = toMapCoordinates(e->pos());
				m_lastPoint = m_anchor;
				m_bDragging = true;
				// 显示一个 0 大小的标记（4 个相同点）。doUpdateGeometry=true 在最后一个
				if (m_rubberBand)
				{
					m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
					m_rubberBand->addPoint(m_anchor, false);
					m_rubberBand->addPoint(m_anchor, false);
					m_rubberBand->addPoint(m_anchor, false);
					m_rubberBand->addPoint(m_anchor, true);
				}
			}
			else if (e->button() == Qt::RightButton)
			{
				// 右键取消
				if (m_rubberBand) m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
				m_bFinished = true;
				if (m_onCancelled) m_onCancelled();
			}
		}
		else
		{
			// 多边形：左键拾取点
			if (e->button() == Qt::LeftButton)
			{
				QgsPointXY pt = toMapCoordinates(e->pos());
				m_points.append(pt);
				if (m_rubberBand) m_rubberBand->addPoint(pt);
				++m_vertexCount;
			}
			else if (e->button() == Qt::RightButton)
			{
				m_bFinished = true;
				if (m_rubberBand) m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
				finishDraw();
			}
		}
	}

	void canvasMoveEvent(QgsMapMouseEvent* e) override
	{
		if (!m_bActive || m_bFinished) return;
		// 关键：同样用 toMapCoordinates 替代 mapPoint，避免坐标变换异常
		QgsPointXY pt = toMapCoordinates(e->pos());
		m_lastPoint = pt;
		if (m_drawType == DrawRect || m_drawType == DrawCircle)
		{
			if (m_bDragging)
			{
				updateRubberBand(pt);
			}
		}
	}

	void canvasReleaseEvent(QgsMapMouseEvent* e) override
	{
		if (!m_bActive || m_bFinished) return;
		// 矩形/圆：拖拽后释放 = 完成绘制
		if ((m_drawType == DrawRect || m_drawType == DrawCircle)
			&& e->button() == Qt::LeftButton
			&& m_bDragging)
		{
			QgsPointXY endPt = toMapCoordinates(e->pos());
			double dx = endPt.x() - m_anchor.x();
			double dy = endPt.y() - m_anchor.y();
			double dist2 = std::sqrt(dx * dx + dy * dy);
			if (dist2 < 1e-9)
			{
				// click without drag - 取消本次拖拽，等待用户继续
				m_bDragging = false;
				if (m_rubberBand) m_rubberBand->reset(QgsWkbTypes::PolygonGeometry);
				return;
			}
			m_lastPoint = endPt;
			m_bDragging = false;
			finishDraw();
			return;
		}
	}

	void canvasDoubleClickEvent(QgsMapMouseEvent* e) override
	{
		Q_UNUSED(e);
		if (m_bFinished) return;
		if (m_drawType == DrawPolygon)
			finishDraw();
	}

	void finishDraw()
	{
		if (m_bFinished) return;
		m_bFinished = true;

		// 计算外包矩形
		QVector<QgsPointXY> pts;
		if (m_drawType == DrawRect)
		{
			pts << m_anchor << m_lastPoint;
		}
		else if (m_drawType == DrawCircle)
		{
			pts << m_anchor << m_lastPoint;
		}
		else
		{
			pts = m_points;
		}

		if (pts.size() >= 2 && m_onDone)
		{
			double minX = pts[0].x(), minY = pts[0].y();
			double maxX = pts[0].x(), maxY = pts[0].y();
			for (const QgsPointXY& p : pts)
			{
				minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
				minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
			}
			dbgRangeLog(QString("finishDraw: before onDone (%1,%2)-(%3,%4)").arg(minX).arg(minY).arg(maxX).arg(maxY));
			m_onDone(minX, minY, maxX, maxY);
			dbgRangeLog("finishDraw: after onDone");
		}

		if (canvas() && canvas()->mapTool() == this)
			canvas()->unsetMapTool(this);
		dbgRangeLog("finishDraw: after unsetMapTool");
	}
};
} // namespace

// ============================================================
// CMapToolDrawShape 实现
// ============================================================
CMapToolDrawShape::CMapToolDrawShape(QgsMapCanvas* canvas, DrawShapeType type)
	: m_pCanvas(canvas)
	, m_drawType(type)
{
}

CMapToolDrawShape::~CMapToolDrawShape()
{
	deactivate();
}

void CMapToolDrawShape::activate()
{
	if (!m_pCanvas) return;
	dbgRangeLog("CMapToolDrawShape::activate");
	deactivate();
	QDrawShapeMapTool* tool = new QDrawShapeMapTool(m_pCanvas, m_drawType);
	dbgRangeLog(QString("CMapToolDrawShape::activate tool=%1").arg((quintptr)tool, 0, 16));
	tool->m_onDone = [this](double a, double b, double c, double d)
	{
		emit shapeFinished(a, b, c, d);
	};
	tool->m_onCancelled = [this]()
	{
		emit drawCancelled();
	};
	m_pMapTool = tool;
	m_pCanvas->setMapTool((QgsMapTool*)tool);
}

void CMapToolDrawShape::deactivate()
{
	if (!m_pMapTool) return;
	dbgRangeLog(QString("CMapToolDrawShape::deactivate enter tool=%1").arg((quintptr)m_pMapTool, 0, 16));
	if (m_pCanvas && m_pCanvas->mapTool() == (QgsMapTool*)m_pMapTool)
		m_pCanvas->unsetMapTool((QgsMapTool*)m_pMapTool);
	dbgRangeLog(QString("CMapToolDrawShape::deactivate delete tool=%1").arg((quintptr)m_pMapTool, 0, 16));
	// 必须按具体类型删除：m_pMapTool 是 void*，直接 delete 只释放内存不调用析构函数，
	// ~QObject 把自身从 canvas children 移除的清理不会执行 → 已释放对象仍挂在 canvas
	// children 列表，下次离屏绘制遍历 children 调 isWidgetType() 读已释放内存崩溃(0xc0000005)
	delete static_cast<QDrawShapeMapTool*>(m_pMapTool);
	m_pMapTool = nullptr;
	dbgRangeLog("CMapToolDrawShape::deactivate done");
}

void CMapToolDrawShape::finish()
{
	if (m_pMapTool)
		((QDrawShapeMapTool*)m_pMapTool)->finishDraw();
}

// ============================================================
// CSE_DataListExportDialog
// ============================================================
CSE_DataListExportDialog::CSE_DataListExportDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("数据裁剪"));
	// 去掉默认的问号帮助按钮（未实现帮助内容，多余）
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(740, 580);

	QPushButton* btnBrowse = new QPushButton(tr("选择数据根目录"), this);
	// "刷新"按钮承担一切刷新：本地数据列表 + 数据库节点 + 地图图层节点
	QPushButton* btnRefresh = new QPushButton(tr("刷新"), this);
	m_btnConnectDb = new QPushButton(tr("连接数据库"), this);
	QPushButton* btnExpand = new QPushButton(tr("全部展开"), this);
	QPushButton* btnCollapse = new QPushButton(tr("全部收起"), this);

	// 主界面"导出后自动加载到地图"开关（影响批量/条件/范围三种导出）
	m_chkAutoLoadAfterExport = new QCheckBox(tr("导出后自动加载到地图"), this);
	m_chkAutoLoadAfterExport->setChecked(true);
	m_chkAutoLoadAfterExport->setToolTip(tr("勾选后，批量/条件/范围三种导出的结果会自动加载到 QGIS 地图画布"));

	QHBoxLayout* topLayout = new QHBoxLayout;
	topLayout->addWidget(btnBrowse);
	topLayout->addWidget(btnRefresh);
	topLayout->addWidget(m_btnConnectDb);
	topLayout->addStretch();
	topLayout->addWidget(m_chkAutoLoadAfterExport);
	topLayout->addWidget(btnExpand);
	topLayout->addWidget(btnCollapse);

	QLabel* lblTitle = new QLabel(tr("数据列表"), this);
	m_pTree = new QTreeWidget(this);
	m_pTree->setColumnCount(4);
	m_pTree->setHeaderLabels(QStringList() << tr("名称") << tr("类型") << tr("修改时间") << tr("大小"));
	m_pTree->header()->setStretchLastSection(true);
	m_pTree->setContextMenuPolicy(Qt::CustomContextMenu);
	m_pTree->setRootIsDecorated(true);

	m_pLog = new QTextEdit(this);
	m_pLog->setReadOnly(true);
	m_pLog->setMaximumHeight(120);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addLayout(topLayout);
	mainLayout->addWidget(lblTitle);
	mainLayout->addWidget(m_pTree, 1);
	mainLayout->addWidget(m_pLog);

	connect(btnBrowse, &QPushButton::clicked, this, &CSE_DataListExportDialog::on_Button_BrowseRoot_clicked);
	// "刷新" 按钮：刷新本地文件列表 + 已连接数据库 + 当前地图图层（一次完成）
	connect(btnRefresh, &QPushButton::clicked, this, &CSE_DataListExportDialog::on_Button_RefreshLayers_clicked);
	connect(m_btnConnectDb, &QPushButton::clicked, this, &CSE_DataListExportDialog::on_Button_ConnectDb_clicked);
	connect(btnExpand, &QPushButton::clicked, this, &CSE_DataListExportDialog::on_Button_ExpandAll_clicked);
	connect(btnCollapse, &QPushButton::clicked, this, &CSE_DataListExportDialog::on_Button_CollapseAll_clicked);
	connect(m_pTree, &QTreeWidget::customContextMenuRequested,
		this, &CSE_DataListExportDialog::on_treeWidgetDataList_customContextMenuRequested);

	// ---- 与数据库连接配置 UI（DbConfigDialog）共享数据库连接 ----
	// 【2026-08-24】更新版未预置任何连接；本项目按需求保留共享机制：
	// 配置 UI 连接成功后把 host/port/database/schema/user/password 写入
	// QSettings("GarMap","MapProductManager")，这里读取同一批键，把共享连接
	// 预置到本对话框的连接列表（与 se_data_management 的"默认连接(auto)"一致）。
	{
		QSettings dbSettings(QStringLiteral("GarMap"), QStringLiteral("MapProductManager"));
		QString host = dbSettings.value(QStringLiteral("db/host"), QStringLiteral("localhost")).toString();
		int port = dbSettings.value(QStringLiteral("db/port"), 5432).toInt();
		QString dbname = dbSettings.value(QStringLiteral("db/database"), QStringLiteral("map_products")).toString();
		QString user = dbSettings.value(QStringLiteral("db/user"), QStringLiteral("postgres")).toString();
		// 仅当配置 UI 勾选“保存密码”时才持久化密码，未勾选则留空由用户手动输入
		QString password;
		if (dbSettings.value(QStringLiteral("db/savePassword"), false).toBool())
			password = dbSettings.value(QStringLiteral("db/password")).toString();

		DatabaseConnectionInfo info;
		info.strName = QStringLiteral("默认连接(auto)");
		info.strDbType = "PostGIS";
		info.strHost = host;
		info.strPort = QString::number(port);
		info.strDbName = dbname;
		info.strUsername = user;
		info.strPassword = password;

		// 去重后预置到连接列表
		if (info.isValid())
		{
			bool bExists = false;
			for (int i = 0; i < m_dbConns.size(); ++i)
			{
				if (m_dbConns[i].strHost == host
					&& m_dbConns[i].strPort == info.strPort
					&& m_dbConns[i].strDbName == dbname)
				{
					bExists = true;
					break;
				}
			}
			if (!bExists)
			{
				m_dbConns.append(info);
				appendLog(tr("[数据库] 已加载配置 UI 共享连接：%1").arg(info.strName));
			}
		}
	}
}

CSE_DataListExportDialog::~CSE_DataListExportDialog()
{
}

void CSE_DataListExportDialog::setQgisInterface(QgisInterface* iface)
{
	m_pQgisIface = iface;
	if (iface)
		m_pMapCanvas = iface->mapCanvas();
}

void CSE_DataListExportDialog::on_Button_BrowseRoot_clicked()
{
	QString dir = QFileDialog::getExistingDirectory(this, tr("选择数据根目录"), m_strRootDir);
	if (dir.isEmpty()) return;
	m_strRootDir = dir;
	populateDataList();
}

void CSE_DataListExportDialog::on_Button_Refresh_clicked()
{
	populateDataList();
}

void CSE_DataListExportDialog::on_Button_ExpandAll_clicked()
{
	if (m_pTree) m_pTree->expandAll();
}

void CSE_DataListExportDialog::on_Button_CollapseAll_clicked()
{
	if (m_pTree) m_pTree->collapseAll();
}

void CSE_DataListExportDialog::populateDataList()
{
	if (!m_pTree) return;
	m_pTree->clear();
	if (m_strRootDir.isEmpty()) return;

	QTreeWidgetItem* rootItem = new QTreeWidgetItem(m_pTree);
	QFileInfo rootInfo(m_strRootDir);
	rootItem->setText(0, rootInfo.fileName().isEmpty() ? m_strRootDir : rootInfo.fileName());
	rootItem->setText(1, tr("目录"));
	rootItem->setData(0, Qt::UserRole, QVariant(m_strRootDir));
	rootItem->setData(0, Qt::UserRole + 1, QVariant(true));
	rootItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));

	addDirNode(rootItem, m_strRootDir);
	m_pTree->expandItem(rootItem);

	// 追加：当前已连接的数据库节点（每个数据库一个顶级节点）
	populateDatabaseRoot();
	// 追加：当前 QGIS 地图中已加载的图层节点
	populateMapLayersRoot();
}

void CSE_DataListExportDialog::addDirNode(QTreeWidgetItem* parent, const QString& dirPath)
{
	QDir dir(dirPath);
	QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo& di : dirs)
	{
		QString absPath = di.absoluteFilePath();
		// .gdb 是文件夹形式的地理数据库，需用 GDAL 枚举其内部图层作为子节点，而非继续当目录展开
		if (isGdbPath(absPath))
		{
			QStringList ogrLayers;
			try
			{
				if (QgsProviderMetadata* md =
					QgsProviderRegistry::instance()->providerMetadata("ogr"))
				{
					QList<QgsProviderSublayerDetails> subs = md->querySublayers(
						absPath, Qgis::SublayerQueryFlags(), nullptr);
					for (const QgsProviderSublayerDetails& sub : subs)
					{
						if (sub.providerKey() == "ogr")
							ogrLayers << sub.name();
					}
				}
			}
			catch (...) { ogrLayers.clear(); }
			if (ogrLayers.isEmpty())
			{
				// 无法枚举时退化为空目录占位，避免与本地文件中转冲突
				QTreeWidgetItem* item = new QTreeWidgetItem(parent);
				item->setText(0, di.fileName());
				item->setText(1, tr("GDB"));
				item->setText(2, di.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
				item->setData(0, Qt::UserRole, absPath);
				item->setData(0, Qt::UserRole + 1, QVariant(true));
				item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
				continue;
			}
			QTreeWidgetItem* gdbItem = new QTreeWidgetItem(parent);
			gdbItem->setText(0, di.fileName());
			gdbItem->setText(1, tr("GDB"));
			gdbItem->setText(2, di.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
			gdbItem->setData(0, Qt::UserRole, absPath);
			gdbItem->setData(0, Qt::UserRole + 1, QVariant(true));
			gdbItem->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
			for (const QString& layerName : ogrLayers)
			{
				QTreeWidgetItem* child = new QTreeWidgetItem(gdbItem);
				child->setText(0, layerName);
				child->setText(1, tr("数据"));
				child->setData(0, Qt::UserRole, absPath);
				child->setData(0, Qt::UserRole + 1, QVariant(false));
				child->setData(0, Qt::UserRole + 5, layerName); // 记录 GDB 内部图层名
				child->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
			}
			continue;
		}
		QTreeWidgetItem* item = new QTreeWidgetItem(parent);
		item->setText(0, di.fileName());
		item->setText(1, tr("目录"));
		item->setText(2, di.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
		item->setData(0, Qt::UserRole, absPath);
		item->setData(0, Qt::UserRole + 1, QVariant(true));
		item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
		addDirNode(item, absPath);
	}

	QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
	for (const QFileInfo& fi : files)
	{
		if (!isDataFile(fi.absoluteFilePath())) continue;
		QTreeWidgetItem* item = new QTreeWidgetItem(parent);
		item->setText(0, fi.fileName());
		item->setText(1, tr("数据"));
		item->setText(2, fi.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
		item->setText(3, fi.size() > 0 ? QString::number(fi.size() / 1024.0, 'f', 1) + tr(" KB") : QString());
		item->setData(0, Qt::UserRole, fi.absoluteFilePath());
		item->setData(0, Qt::UserRole + 1, QVariant(false));
		item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
	}
}

bool CSE_DataListExportDialog::isDataFile(const QString& filePath)
{
	QString lower = filePath.toLower();
	return lower.endsWith(".shp") || lower.endsWith(".tif") || lower.endsWith(".tiff")
		|| lower.endsWith(".gpkg") || lower.endsWith(".json") || lower.endsWith(".geojson")
		|| lower.endsWith(".tab") || lower.endsWith(".mif") || lower.endsWith(".kml")
		|| lower.endsWith(".img") || lower.endsWith(".ecw") || lower.endsWith(".bmp")
		|| lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

bool CSE_DataListExportDialog::isGdbPath(const QString& filePath)
{
	QString lower = filePath.toLower();
	if (!lower.endsWith(".gdb")) return false;
	QFileInfo fi(filePath);
	return fi.exists() && fi.isDir();
}

bool CSE_DataListExportDialog::isRasterPath(const QString& filePath)
{
	QString lower = filePath.toLower();
	return lower.endsWith(".tif") || lower.endsWith(".tiff") || lower.endsWith(".img")
		|| lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg")
		|| lower.endsWith(".bmp") || lower.endsWith(".ecw");
}

void CSE_DataListExportDialog::on_treeWidgetDataList_customContextMenuRequested(const QPoint& pos)
{
	if (!m_pTree) return;
	QTreeWidgetItem* item = m_pTree->itemAt(pos);
	if (!item) return;

	QString strPath = item->data(0, Qt::UserRole).toString();
	bool bIsDir = item->data(0, Qt::UserRole + 1).toBool();
	int nodeType = item->data(0, Qt::UserRole + 1).toInt();
	m_pTree->setCurrentItem(item);

	QMenu menu(this);
	QAction* actBatch = menu.addAction(tr("批量导出"));
	QAction* actCondition = menu.addAction(tr("按条件导出"));
	QAction* actRange = menu.addAction(tr("按范围导出"));
	QAction* actMainArea = menu.addAction(tr("按主区裁切导出"));
	menu.addSeparator();
	QAction* actShowInMap = menu.addAction(tr("在地图显示"));

	QAction* chosen = menu.exec(m_pTree->viewport()->mapToGlobal(pos));
	if (!chosen) return;

	// 数据库/地图图层节点：直接调用统一的导出与显示处理（仅当节点类型不是 0/1 时使用）
	if (nodeType == (int)NodeDbTable || nodeType == (int)NodeMapLayer)
	{
		if (chosen == actShowInMap)
		{
			loadItemToMap(item);
		}
		else
		{
			doExportForItem(item);
		}
		return;
	}

	// 本地文件 / 目录：原流程
	if (chosen == actBatch)
	{
		doBatchExport(bIsDir ? strPath : QFileInfo(strPath).absolutePath());
	}
	else if (chosen == actCondition)
	{
		doConditionExport(bIsDir ? strPath : QFileInfo(strPath).absolutePath());
	}
	else if (chosen == actRange)
	{
		doRangeExport(strPath);
	}
	else if (chosen == actMainArea)
	{
		// 直接弹出精简的"按主区裁切导出"对话框：只暴露主区+输出+纸张+比例尺等主区相关参数，
		// 不再让用户在 4 个 tab 的完整数据管理 dialog 里二次选择裁切方式
		CSE_MainAreaExportDialog dlg(this);
		dlg.setSourcePath(strPath);
		dlg.setWindowTitle(tr("按主区裁切导出 - %1").arg(QFileInfo(strPath).fileName()));
		if (dlg.exec() != QDialog::Accepted) return;

		// 收集待裁切数据（与"按范围导出"使用同一 exportFiles 流程，但将选取的主区作为范围）
		QStringList files;
		QFileInfo srcFi(strPath);
		if (srcFi.isDir())
		{
			bool isGdb = isGdbPath(strPath);
			if (isGdb)
			{
				// GDB 整目录导出
				files << strPath;
			}
			else
			{
				files = collectDataFiles(strPath, true);
			}
		}
		else
		{
			if (isGdbPath(strPath))
			{
				// GDB 单图层（实际上是 .gdb 父目录 + layerName）
				files << strPath;
			}
			else
			{
				files = collectFromFile(strPath);
			}
		}
		if (files.isEmpty())
		{
			appendLog(tr("没有可导出的数据文件。"));
			return;
		}

		// 用主区数据生成裁剪范围：读主区 shp 中已选要素的 bbox，作为统一裁剪范围
		QgsCoordinateReferenceSystem mainAreaCrs;
		QgsRectangle clipRect = computeMainAreaClipRect(dlg.mainAreaPath(), dlg.mainAreaField(),
			dlg.selectedFeatureIds(), &mainAreaCrs);
		if (clipRect.isEmpty())
		{
			QMessageBox::warning(this, tr("按主区裁切导出"), tr("未能从主区数据中得到有效范围，请确认已选择要素。"));
			return;
		}

		QString outPath = dlg.outputFilePath();
		if (outPath.isEmpty()) return;

		QString errMsg;
		if (files.size() == 1)
		{
			// 单个数据 → 直接输出到用户选择的单个文件，自动加载该文件
			QString dstPath;
			bool ok = exportSingleToFile(files.first(), outPath, &clipRect, &mainAreaCrs, dstPath, errMsg);
			showResult(ok, tr("按主区裁切导出"),
				tr("按主区裁切导出完成：%1").arg(dstPath),
				tr("按主区裁切导出失败：%1").arg(errMsg));
			if (ok && dlg.autoLoadAfterExport()) autoLoadToMap(dstPath);
		}
		else
		{
			// 多数据源（目录）→ 输出目录形式，自动加载只加载本次导出的文件，不扫已有数据
			QString outDir = outputDirFromPick(outPath);
			QDir().mkpath(outDir);
			QStringList written;
			int ok = exportFiles(files, outDir, &clipRect, &mainAreaCrs, QString(), QString(), QString(),
				QDateTime(), QDateTime(), false, errMsg, &written);
			showResult(ok > 0, tr("按主区裁切导出"),
				tr("按主区裁切导出完成，共导出 %1 个数据文件到：\n%2").arg(ok).arg(outDir),
				tr("按主区裁切导出失败：%1").arg(errMsg));
			if (ok > 0 && dlg.autoLoadAfterExport())
			{
				for (const QString& p : written) autoLoadToMap(p);
			}
		}
	}
	else if (chosen == actShowInMap)
	{
		// 仅对单个数据文件（非目录）执行加载；目录可交给批量加载或导出流程
		if (bIsDir)
		{
			QMessageBox::information(this, tr("在地图显示"),
				tr("目录项无法直接显示，请选择具体的数据文件。"));
		}
		else
		{
			loadDataFileToMap(strPath);
		}
	}
}

void CSE_DataListExportDialog::doBatchExport(const QString& dirPath)
{
	QMessageBox::StandardButton ret = QMessageBox::question(this, tr("批量导出"),
		QString::fromUtf8("是否导出当前目录下包括子目录在内的所有数据？\n\n点击【是】导出当前目录及子目录下所有数据；\n点击【否】仅导出当前目录下的数据。"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	// 点 X 关闭、或者用户按 ESC 时 ret 可能是 NoButton，此时视为取消，不应继续弹路径选择
	if (ret != QMessageBox::Yes && ret != QMessageBox::No)
	{
		appendLog(tr("[批量导出] 已取消。"));
		return;
	}
	bool bIncludeSub = (ret == QMessageBox::Yes);

	QString outDir = browseOutputDir();
	if (outDir.isEmpty()) return;

	QStringList files = collectDataFiles(dirPath, bIncludeSub);
	if (files.isEmpty())
	{
		appendLog(tr("没有可导出的数据文件。"));
		QMessageBox::information(this, tr("批量导出"), tr("该目录下没有可导出的数据文件。"));
		return;
	}

	QString errMsg;
	int ok = exportFiles(files, outDir, nullptr, nullptr, QString(), QString(), QString(),
		QDateTime(), QDateTime(), false, errMsg);

	showResult(ok > 0, tr("批量导出"),
		tr("批量导出成功，共导出 %1 个数据文件到：\n%2").arg(ok).arg(outDir),
		tr("批量导出失败：%1").arg(errMsg));
	if (ok > 0 && autoLoadAfterExport()) autoLoadDirToMap(outDir);
}

void CSE_DataListExportDialog::doConditionExport(const QString& dirPath)
{
	CSE_ConditionExportDialog dlg(this);
	if (dlg.exec() != QDialog::Accepted) return;

	QString strName = dlg.dataName().trimmed();
	QString strUser = dlg.userName().trimmed();
	QString strType = dlg.dataType();
	QString outPath = dlg.outputPath();
	QDateTime dtStart = dlg.startTime();
	QDateTime dtEnd = dlg.endTime();

	QStringList files = collectDataFiles(dirPath, true);
	if (files.isEmpty())
	{
		appendLog(tr("没有可导出的数据文件。"));
		QMessageBox::information(this, tr("按条件导出"), tr("该目录下没有可导出的数据文件。"));
		return;
	}

	QString errMsg;
	int ok = exportFiles(files, outPath, nullptr, nullptr, strName, strUser, strType,
		dtStart, dtEnd, true, errMsg);

	showResult(ok > 0, tr("按条件导出"),
		tr("按条件导出成功，共导出 %1 个数据文件到：\n%2").arg(ok).arg(outPath),
		tr("按条件导出失败：%1").arg(errMsg));
	if (ok > 0 && autoLoadAfterExport()) autoLoadDirToMap(outPath);
}

bool CSE_DataListExportDialog::interactiveDrawExtent(QgsRectangle& outRect, DrawShapeType shape)
{
	if (!m_pMapCanvas) return false;
	dbgRangeLog("interactiveDrawExtent: enter");

	// 让 QGIS 主窗口到前台，画布获得焦点
	if (m_pQgisIface)
	{
		if (QWidget* w = m_pQgisIface->mainWindow()) { w->raise(); w->activateWindow(); }
	}
	m_pMapCanvas->setFocus(Qt::OtherFocusReason);
	m_pMapCanvas->setMouseTracking(true);

	// 在非模态环境中激活绘制工具，并用 QEventLoop 阻塞等待绘制完成/取消。
	// 此时没有任何 modal dialog，画布可正常接收鼠标事件。
	CMapToolDrawShape tool(m_pMapCanvas, shape);
	QEventLoop loop;
	dbgRangeLog(QString("interactiveDrawExtent: canvas=%1 tool=%2").arg((quintptr)m_pMapCanvas, 0, 16).arg((quintptr)&tool, 0, 16));
	bool ok = false;
	QgsRectangle rectRaw;   // 画布原始 CRS（投影/米），用于导出
	QObject::connect(&tool, &CMapToolDrawShape::shapeFinished,
		[&](double a, double b, double c, double d)
	{
		rectRaw = QgsRectangle(a, b, c, d);
		dbgRangeLog("lambda: enter");

		// 将绘制的范围从画布当前 CRS 转换到 EPSG:4326（经纬度），
		// 对话框内的最小 X/最小 Y/最大 X/最大 Y 直接显示经纬度。
		// 优先尝试 QGIS 提供的 proj 转换；如果转换失败，或结果与原始投影坐标相同
		//（说明 proj 库不可用/转换被短路），就使用 Web Mercator 数学公式反算兜底。
		QgsRectangle rectLL = rectRaw;
		QgsCoordinateReferenceSystem srcCrs = m_pMapCanvas->mapSettings().destinationCrs();
		QgsCoordinateReferenceSystem dstCrs = QgsCoordinateReferenceSystem::fromEpsgId(4326);
		bool converted = false;

		qDebug("[rangeExport] raw rect (%.6f, %.6f) - (%.6f, %.6f) srcCrs=%s valid=%d",
			rectRaw.xMinimum(), rectRaw.yMinimum(), rectRaw.xMaximum(), rectRaw.yMaximum(),
			qPrintable(srcCrs.authid()), srcCrs.isValid() ? 1 : 0);

		// 自动识别是否是 Web Mercator 米制坐标（北半球，|y| < 2 * pi * R，|x| < 2 * pi * R）
		auto looksLikeWebMercator = [](const QgsRectangle& r) -> bool
		{
			double xMin = r.xMinimum(), yMin = r.yMinimum();
			double xMax = r.xMaximum(), yMax = r.yMaximum();
			const double R = 6378137.0;
			double world = 2.0 * M_PI * R; // ~ 4.007e7
			bool xInWorld = std::abs(xMin) < world && std::abs(xMax) < world;
			bool yInWorld = std::abs(yMin) < world && std::abs(yMax) < world;
			if (!xInWorld || !yInWorld) return false;
			// 进一步：纬度方向上的 y 必须对应南北回归线内 ±85°，即 |y| 不超过 2 * R ≈ 1.27e7
			const double maxY = 2.0 * R; // ≈ 1.276e7
			return std::abs(yMin) < maxY && std::abs(yMax) < maxY;
		};

		// 1) 尝试 QGIS proj 转换
		try
		{
			if (srcCrs.isValid() && dstCrs.isValid())
			{
				// 路径 A: mapCanvas transformContext
				{
					QgsCoordinateTransformContext trCtx = m_pMapCanvas->mapSettings().transformContext();
					QgsCoordinateTransform trA(srcCrs, dstCrs, trCtx);
					if (trA.isValid())
					{
						QgsRectangle out = trA.transform(rectRaw);
						if (!out.isEmpty())
						{
							rectLL = out;
							converted = true;
						}
					}
				}
				// 路径 B: 默认构造 + setSourceCrs/setDestinationCrs
				if (!converted)
				{
					QgsCoordinateTransform trB;
					trB.setSourceCrs(srcCrs);
					trB.setDestinationCrs(dstCrs);
					if (trB.isValid())
					{
						QgsRectangle out = trB.transform(rectRaw);
						if (!out.isEmpty())
						{
							rectLL = out;
							converted = true;
						}
					}
				}
				// 路径 C: project instance（冷启动场景兜底）
				if (!converted && QgsProject::instance())
				{
					QgsCoordinateTransformContext trCtx = QgsProject::instance()->transformContext();
					QgsCoordinateTransform trC(srcCrs, dstCrs, trCtx);
					if (trC.isValid())
					{
						QgsRectangle out = trC.transform(rectRaw);
						if (!out.isEmpty())
						{
							rectLL = out;
							converted = true;
						}
					}
				}
			}
		}
		catch (...)
		{
			// 【2026-08-24】闪退修复：只捕获 QgsException 会漏掉 PROJ/QGIS 在个别 CRS 状态下抛出的
			// 其它异常（std::runtime_error 等），未捕获异常会一路穿透 canvasReleaseEvent
			// -> Qt 事件分发器 -> 进程直接终止（闪退）。改为捕获全部异常，
			// 转换失败立即用原始画布 CRS 范围兜底，导出流程继续，绝不崩溃。
			converted = false;
			rectLL = rectRaw;
		}

		// 2) 兜底：根据 srcCrs authid 或坐标形态自动决定
		if (!converted || (
			std::abs(rectLL.xMinimum() - rectRaw.xMinimum()) < 1e-3 &&
			std::abs(rectLL.yMinimum() - rectRaw.yMinimum()) < 1e-3 &&
			std::abs(rectLL.xMaximum() - rectRaw.xMaximum()) < 1e-3 &&
			std::abs(rectLL.yMaximum() - rectRaw.yMaximum()) < 1e-3 &&
			srcCrs.authid() != dstCrs.authid()))
		{
			bool didFallback = false;
			if (srcCrs.authid() == "EPSG:3857" || srcCrs.authid() == "EPSG:900913")
			{
				didFallback = true;
			}
			else if (srcCrs.authid() == "EPSG:4326")
			{
				rectLL = rectRaw;
				converted = true;
				didFallback = true;
			}
			else if (looksLikeWebMercator(rectRaw))
			{
				// 坐标值落在 Web Mercator 世界范围内，按 Web Mercator 反算
				didFallback = true;
			}

			if (didFallback && srcCrs.authid() != "EPSG:4326")
			{
				auto mercToLL = [](double mx, double my, double& lon, double& lat)
				{
					const double R = 6378137.0;
					lon = (mx / R) * 180.0 / M_PI;
					lat = (my / R) * 180.0 / M_PI;
					// 用 tanh/clamp 防止 |lat| 极端值；最终用 atan 反算纬度
					double t = std::exp(lat * M_PI / 180.0);
					t = std::max(t, 1e-15);
					lat = 180.0 / M_PI * (2.0 * std::atan(t) - M_PI / 2.0);
				};
				double lonMin, latMin, lonMax, latMax;
				mercToLL(rectRaw.xMinimum(), rectRaw.yMinimum(), lonMin, latMin);
				mercToLL(rectRaw.xMaximum(), rectRaw.yMaximum(), lonMax, latMax);
				rectLL = QgsRectangle(
					std::min(lonMin, lonMax), std::min(latMin, latMax),
					std::max(lonMin, lonMax), std::max(latMin, latMax));
				converted = true;
			}
		}

		// 3) 终极兜底：若仍无法转回经纬度，**至少明确标识为投影坐标**，
		//    后续在 setExtent 内同时显示"原始投影 + 经纬度"两行，让用户能判定。
		if (!converted)
		{
			rectLL = rectRaw;
		}

		qDebug("[rangeExport] converted rect (%.6f, %.6f) - (%.6f, %.6f) converted=%d",
			rectLL.xMinimum(), rectLL.yMinimum(), rectLL.xMaximum(), rectLL.yMaximum(),
			converted ? 1 : 0);

		// 写一份"原始投影 rect"到全局，供 doRangeExport 实际导出时使用（exportFiles 需要原始 CRS）
		m_drawnRectMap = rectRaw;
		outRect = rectLL; // 返回经纬度版本给对话框显示
		ok = true;
		loop.quit();
		dbgRangeLog("lambda: after loop.quit");
	});
	QObject::connect(&tool, &CMapToolDrawShape::drawCancelled,
		[&loop]() { loop.quit(); });

	dbgRangeLog("interactiveDrawExtent: before activate");
	tool.activate();
	loop.exec();
	dbgRangeLog("interactiveDrawExtent: after loop.exec");
	tool.deactivate();
	dbgRangeLog("interactiveDrawExtent: after deactivate");

	if (m_pMapCanvas) m_pMapCanvas->refresh();
	dbgRangeLog("interactiveDrawExtent: return");
	return ok;
}

void CSE_DataListExportDialog::doRangeExport(const QString& dirPath)
{
	if (!m_pMapCanvas)
	{
		QMessageBox::warning(this, tr("按范围导出"), tr("未获取到地图画布，无法进行手动绘制或选择要素。"));
		return;
	}
	dbgRangeLog("doRangeExport: enter");
#ifdef _WIN32
	installRangeExportCrashFilter(); // 【临时调试】崩溃时记录调用栈（定位后删除）
#endif

	// 手动绘制/坐标输入的范围位于画布 CRS 下，导出时按画布 CRS 转换到源图层 CRS
	QgsCoordinateReferenceSystem canvasCrs = seCanvasDestCrs(m_pMapCanvas);

	// 1) GDB 子项：单独子图层导出 + 按范围
	if (isGdbPath(dirPath))
	{
		QTreeWidgetItem* cur = m_pTree ? m_pTree->currentItem() : nullptr;
		QString layerName = cur ? cur->data(0, Qt::UserRole + 5).toString() : QString();
		QgsRectangle drawnRect;
		bool haveRect = false;
		// 记录"范围获取方式"和"绘制类型"在用户两次打开之间的选择，避免重新弹对话框时退回首页
		int lastModeIdx = -1;
		int lastDrawTypeIdx = -1;
		// 【2026-08-25 闪退修复】对话框提升到循环外、复用同一实例：原实现每轮循环销毁并重建
		// 栈上 CSE_RangeExportDialog，导致 Qt 事件队列残留指向已析构 widget 的 pending 事件
		// （如 UpdateRequest 重绘），第二轮 exec() 派发残留事件时对已释放 QObject 调
		// isWidgetType() → 0xc0000005 段错误。复用对话框后事件派发到活对象，不再崩溃。
		CSE_RangeExportDialog dlg(m_pMapCanvas, m_pQgisIface, this);
		while (true)
		{
			dlg.resetDrawRequested(); // 复用前清掉上一轮的"开始绘制"标记，否则直接点"确定"也会被误判为重绘
			// 第一次循环：根据用户在第一轮对话框中选择的 mode / drawType 记录之
			if (lastModeIdx >= 0) dlg.setModeIndex(lastModeIdx);
			if (lastDrawTypeIdx >= 0) dlg.setDrawTypeIndex(lastDrawTypeIdx);
			if (haveRect) dlg.setExtent(drawnRect);
			dbgDumpCanvasChildren(m_pMapCanvas); // 【临时】二次 exec 前 dump canvas 子树
			if (dlg.exec() != QDialog::Accepted) return;
			dbgRangeLog("doRangeExport: after exec");
			// 记录本次用户选择
			lastModeIdx = dlg.modeIndex();
			lastDrawTypeIdx = dlg.drawTypeIndex();
			if (dlg.drawRequested())
			{
				QgsRectangle r;
				if (interactiveDrawExtent(r, dlg.selectedDrawType())) { drawnRect = r; haveRect = true; }
				continue;
			}
			if (!dlg.hasExtent())
			{
				QMessageBox::warning(this, tr("按范围导出"), tr("请先定义导出范围。"));
				return;
			}
			QString outPick = dlg.outputPath();
			if (outPick.isEmpty()) return;
			if (layerName.isEmpty())
			{
				// 整 GDB 的所有图层 → 输出目录形式，自动加载只加载本次导出的文件
				QStringList files; files << dirPath;
				QString errMsg;
				// 使用原始画布 CRS 下的范围，确保空间过滤在数据源所在坐标系下正确执行
				QgsRectangle rect = m_drawnRectMap;
				QString outDir = outputDirFromPick(outPick);
				QDir().mkpath(outDir);
				QStringList written;
				int ok = exportFiles(files, outDir, &rect, &canvasCrs, QString(), QString(), QString(),
					QDateTime(), QDateTime(), false, errMsg, &written);
				showResult(ok > 0, tr("按范围导出"),
					tr("按范围导出成功，共导出 %1 个数据文件到：\n%2").arg(ok).arg(outDir),
					tr("按范围导出失败：%1").arg(errMsg));
				if (ok > 0 && dlg.autoLoadAfterExport())
				{
					for (const QString& p : written) autoLoadToMap(p);
				}
				return;
			}
			// 单图层 → 输出到用户选择的单个文件，自动加载该文件
			QString outPath = outPick;
			if (!outPath.toLower().endsWith(".shp"))
				outPath = QFileInfo(outPath).absolutePath() + "/" + QFileInfo(outPath).completeBaseName() + ".shp";
			QDir().mkpath(QFileInfo(outPath).absolutePath());
			QString srcUri = QString("\"%1\" layername=%2").arg(dirPath, layerName);
			QString err;
			exportVectorFile(srcUri, outPath, &m_drawnRectMap, &canvasCrs, QString(), QString(), QString(),
				QDateTime(), QDateTime(), false, err);
			showResult(err.isEmpty(), tr("按范围导出"),
				tr("按范围导出成功：%1").arg(outPath),
				tr("按范围导出失败：%1").arg(err));
			if (err.isEmpty() && dlg.autoLoadAfterExport()) autoLoadToMap(outPath);
			return;
		}
	}

	bool bIsDir = QFileInfo(dirPath).isDir();
	QStringList files = bIsDir ? collectDataFiles(dirPath, true) : collectFromFile(dirPath);
	if (files.isEmpty())
	{
		appendLog(tr("没有可导出的数据文件。"));
		return;
	}

	QgsRectangle drawnRect;
	bool haveRect = false;
	int lastModeIdx2 = -1;
	int lastDrawTypeIdx2 = -1;
	// 【2026-08-25 闪退修复】同上：对话框复用，避免每轮销毁导致残留事件命中已释放 widget
	CSE_RangeExportDialog dlg(m_pMapCanvas, m_pQgisIface, this);
	while (true)
	{
		dlg.resetDrawRequested();
		dbgRangeLog("doRangeExport: loop iter, dlg created");
		if (lastModeIdx2 >= 0) dlg.setModeIndex(lastModeIdx2);
		if (lastDrawTypeIdx2 >= 0) dlg.setDrawTypeIndex(lastDrawTypeIdx2);
		if (haveRect) dlg.setExtent(drawnRect);
		dbgDumpCanvasChildren(m_pMapCanvas); // 【临时】二次 exec 前 dump canvas 子树
		if (dlg.exec() != QDialog::Accepted) return;
		dbgRangeLog("doRangeExport: after exec");
		lastModeIdx2 = dlg.modeIndex();
		lastDrawTypeIdx2 = dlg.drawTypeIndex();

		if (dlg.drawRequested())
		{
			QgsRectangle r;
			if (interactiveDrawExtent(r, dlg.selectedDrawType())) { drawnRect = r; haveRect = true; }
			continue;
		}

		if (!dlg.hasExtent())
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("请先定义导出范围。"));
			return;
		}

		QString outPick = dlg.outputPath();
		if (outPick.isEmpty()) return;
		// 使用原始画布 CRS 下的范围，确保空间过滤在数据源所在坐标系下正确执行
		QgsRectangle rect = m_drawnRectMap;

		QString errMsg;
		if (files.size() == 1)
		{
			// 单个数据 → 直接输出到用户选择的单个文件，自动加载该文件
			QString dstPath;
			bool ok = exportSingleToFile(files.first(), outPick, &rect, &canvasCrs, dstPath, errMsg);
			showResult(ok, tr("按范围导出"),
				tr("按范围导出成功：%1").arg(dstPath),
				tr("按范围导出失败：%1").arg(errMsg));
			if (ok && dlg.autoLoadAfterExport()) autoLoadToMap(dstPath);
		}
		else
		{
			// 多数据源（目录）→ 输出目录形式，自动加载只加载本次导出的文件，不扫已有数据
			QString outDir = outputDirFromPick(outPick);
			QDir().mkpath(outDir);
			QStringList written;
			int ok = exportFiles(files, outDir, &rect, &canvasCrs, QString(), QString(), QString(),
				QDateTime(), QDateTime(), false, errMsg, &written);
			showResult(ok > 0, tr("按范围导出"),
				tr("按范围导出成功，共导出 %1 个数据文件到：\n%2").arg(ok).arg(outDir),
				tr("按范围导出失败：%1").arg(errMsg));
			if (ok > 0 && dlg.autoLoadAfterExport())
			{
				for (const QString& p : written) autoLoadToMap(p);
			}
		}
		return;
	}
}

QString CSE_DataListExportDialog::browseOutputDir()
{
	QString dir = QFileDialog::getExistingDirectory(this, tr("选择数据导出路径"), m_strOutputDir);
	if (!dir.isEmpty()) m_strOutputDir = dir;
	return dir;
}

QStringList CSE_DataListExportDialog::collectDataFiles(const QString& dirPath, bool bIncludeSubDirs) const
{
	QStringList result;
	QDir dir(dirPath);
	QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
	for (const QFileInfo& fi : files)
	{
		if (isDataFile(fi.absoluteFilePath()))
			result << fi.absoluteFilePath();
	}
	// 同级目录中若存在 .gdb 目录，也视作一个数据源（GDB 本身是目录形式存储）
	QFileInfoList topDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo& di : topDirs)
	{
		if (isGdbPath(di.absoluteFilePath()))
			result << di.absoluteFilePath();
	}
	if (bIncludeSubDirs)
	{
		for (const QFileInfo& di : topDirs)
			result << collectDataFiles(di.absoluteFilePath(), true);
	}
	return result;
}

QStringList CSE_DataListExportDialog::collectFromFile(const QString& filePath) const
{
	QStringList result;
	if (isDataFile(filePath))
		result << filePath;
	else if (isGdbPath(filePath))
		result << filePath;
	return result;
}

bool CSE_DataListExportDialog::matchTime(const QFileInfo& fi, const QDateTime& dtStart, const QDateTime& dtEnd)
{
	if (dtStart.isValid() && fi.lastModified() < dtStart) return false;
	if (dtEnd.isValid() && fi.lastModified() > dtEnd) return false;
	return true;
}

int CSE_DataListExportDialog::exportFiles(const QStringList& files, const QString& outDir,
	const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
	const QString& strNameFilter, const QString& strUserFilter,
	const QString& strTypeFilter, const QDateTime& dtStart, const QDateTime& dtEnd,
	bool bUseTime, QString& errMsg, QStringList* writtenPaths)
{
	Q_UNUSED(errMsg);
	int okCount = 0;
	QProgressDialog progress(tr("正在导出数据..."), tr("取消"), 0, files.size(), this);
	progress.setWindowModality(Qt::WindowModal);
	progress.show();

	for (int i = 0; i < files.size(); ++i)
	{
		const QString& file = files.at(i);
		progress.setValue(i);
		progress.setLabelText(tr("正在导出：%1").arg(QFileInfo(file).fileName()));
		QApplication::processEvents();
		if (progress.wasCanceled()) break;

		if (bUseTime)
		{
			QFileInfo fi(file);
			if (!matchTime(fi, dtStart, dtEnd)) continue;
		}
		if (!strNameFilter.isEmpty())
		{
			if (!QFileInfo(file).fileName().contains(strNameFilter, Qt::CaseInsensitive)) continue;
		}
		if (!strTypeFilter.isEmpty() && strTypeFilter != tr("全部"))
		{
			bool bWantRaster = (strTypeFilter == tr("栅格数据"));
			bool isRas = isRasterPath(file);
			if (bWantRaster != isRas) continue;
		}

		QString oneErr;
		if (exportSingleFile(file, outDir, extentRect, extentCrs, strNameFilter, strUserFilter,
			strTypeFilter, dtStart, dtEnd, bUseTime, oneErr, writtenPaths))
		{
			++okCount;
		}
		else
		{
			appendLog(tr("导出失败：%1 —— %2").arg(QFileInfo(file).fileName()).arg(oneErr));
		}
	}
	progress.setValue(files.size());
	return okCount;
}

bool CSE_DataListExportDialog::exportSingleFile(const QString& srcPath, const QString& outDir,
	const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
	const QString& strNameFilter, const QString& strUserFilter,
	const QString& strTypeFilter, const QDateTime& dtStart, const QDateTime& dtEnd,
	bool bUseTime, QString& errMsg, QStringList* writtenPaths)
{
	Q_UNUSED(strNameFilter); Q_UNUSED(strUserFilter); Q_UNUSED(strTypeFilter);
	Q_UNUSED(dtStart); Q_UNUSED(dtEnd); Q_UNUSED(bUseTime);

	bool bRaster = isRasterPath(srcPath);
	bool bGdb = !bRaster && isGdbPath(srcPath);
	QString baseName = QFileInfo(srcPath).completeBaseName();
	QString outSub = outDir + "/" + QFileInfo(srcPath).absolutePath().section('/', -1);
	QDir().mkpath(outSub);

	QString dstPath;
	if (bRaster)
		dstPath = outSub + "/" + baseName + ".tif";
	else if (bGdb)
		dstPath = outSub + "/" + baseName + ".gdb"; // FileGDB 输出为新建的 .gdb 目录
	else
		dstPath = outSub + "/" + baseName + ".shp";

	bool ok;
	if (bRaster)
		ok = exportRasterFile(srcPath, dstPath, extentRect, extentCrs, strNameFilter, strUserFilter,
			strTypeFilter, dtStart, dtEnd, bUseTime, errMsg);
	else
		ok = exportVectorFile(srcPath, dstPath, extentRect, extentCrs, strNameFilter, strUserFilter,
			strTypeFilter, dtStart, dtEnd, bUseTime, errMsg);
	if (ok && writtenPaths) writtenPaths->append(dstPath);
	return ok;
}

// 单数据源导出到用户选择的单个文件：按源类型补全 .shp/.tif 扩展名，
// 直接写 pickPath（不再套一层"源目录名"子目录），返回实际输出路径供自动加载
bool CSE_DataListExportDialog::exportSingleToFile(const QString& srcPath, const QString& pickPath,
	const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
	QString& dstPathOut, QString& errMsg)
{
	QString dstPath = pickPath;
	QDir().mkpath(QFileInfo(dstPath).absolutePath());
	if (isRasterPath(srcPath))
	{
		if (!dstPath.toLower().endsWith(".tif") && !dstPath.toLower().endsWith(".tiff"))
			dstPath = QFileInfo(dstPath).absolutePath() + "/" + QFileInfo(dstPath).completeBaseName() + ".tif";
		dstPathOut = dstPath;
		return exportRasterFile(srcPath, dstPath, extentRect, extentCrs, QString(), QString(), QString(),
			QDateTime(), QDateTime(), false, errMsg);
	}
	else
	{
		if (!dstPath.toLower().endsWith(".shp"))
			dstPath = QFileInfo(dstPath).absolutePath() + "/" + QFileInfo(dstPath).completeBaseName() + ".shp";
		dstPathOut = dstPath;
		return exportVectorFile(srcPath, dstPath, extentRect, extentCrs, QString(), QString(), QString(),
			QDateTime(), QDateTime(), false, errMsg);
	}
}

bool CSE_DataListExportDialog::exportVectorFile(const QString& srcPath, const QString& dstPath,
	const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
	const QString& strNameFilter, const QString& strUserFilter,
	const QString& strTypeFilter, const QDateTime& dtStart, const QDateTime& dtEnd,
	bool bUseTime, QString& errMsg)
{
	Q_UNUSED(strNameFilter); Q_UNUSED(strUserFilter); Q_UNUSED(strTypeFilter);
	Q_UNUSED(dtStart); Q_UNUSED(dtEnd); Q_UNUSED(bUseTime);

	QgsVectorLayer* srcLayer = new QgsVectorLayer(srcPath, QFileInfo(srcPath).fileName(), "ogr");
	if (!srcLayer->isValid())
	{
		errMsg = tr("无法打开矢量数据：%1").arg(srcPath);
		delete srcLayer;
		return false;
	}

	QString dstLower = dstPath.toLower();
	bool bDstGdb = dstLower.endsWith(".gdb");

	QgsVectorFileWriter::SaveVectorOptions options;
	options.driverName = bDstGdb ? "FileGDB" : "ESRI Shapefile";
	options.fileEncoding = "UTF-8";
	options.layerName = QFileInfo(dstPath).completeBaseName();

	// 定义范围时，用范围过滤（取外包矩形）。filterExtent 按源图层 CRS 解释，
	// 必须先把范围矩形从其所在 CRS 转换到源图层 CRS，否则 CRS 不一致时过滤落空 → 空数据。
	// 范围所在 CRS 由调用方经 extentCrs 显式指定（范围导出=画布 CRS，主区裁剪=主区 CRS）；
	// extentCrs 为空时回退到画布 CRS 假设，兼容未显式传 CRS 的调用点。
	if (extentRect && !extentRect->isNull())
	{
		QgsMapCanvas* canvas = m_pMapCanvas ? m_pMapCanvas
			: (m_pQgisIface ? m_pQgisIface->mapCanvas() : nullptr);
		QgsCoordinateReferenceSystem rectCrs = (extentCrs && extentCrs->isValid())
			? *extentCrs : seCanvasDestCrs(canvas);
		options.filterExtent = seXformRect(*extentRect, rectCrs, srcLayer->crs());
	}

	// FileGDB 输出：若目标 .gdb 已存在，先删除；QgsVectorFileWriter 会创建新目录
	if (bDstGdb)
	{
		QFileInfo gdbFi(dstPath);
		if (gdbFi.exists())
			QDir(gdbFi.absoluteFilePath()).removeRecursively();
	}

	QString errMsg2;
	QgsVectorFileWriter::WriterError errCode =
		QgsVectorFileWriter::writeAsVectorFormatV3(srcLayer, dstPath,
			QgsProject::instance()->transformContext(), options,
			&errMsg2);

	bool bOk = (errCode == QgsVectorFileWriter::NoError);
	if (!bOk)
		errMsg = tr("矢量导出失败：%1").arg(errMsg2);

	delete srcLayer;
	return bOk;
}

bool CSE_DataListExportDialog::exportRasterFile(const QString& srcPath, const QString& dstPath,
	const QgsRectangle* extentRect, const QgsCoordinateReferenceSystem* extentCrs,
	const QString& strNameFilter, const QString& strUserFilter,
	const QString& strTypeFilter, const QDateTime& dtStart, const QDateTime& dtEnd,
	bool bUseTime, QString& errMsg)
{
	Q_UNUSED(strNameFilter); Q_UNUSED(strUserFilter); Q_UNUSED(strTypeFilter);
	Q_UNUSED(dtStart); Q_UNUSED(dtEnd); Q_UNUSED(bUseTime);

	GDALAllRegister();
	GDALDataset* poSrcDS = (GDALDataset*)GDALOpen(srcPath.toUtf8().constData(), GA_ReadOnly);
	if (!poSrcDS)
	{
		errMsg = tr("无法打开栅格数据：%1").arg(srcPath);
		return false;
	}

	char** papszOptions = nullptr;
	if (extentRect && !extentRect->isNull())
	{
		QString projwin = QString("%1 %2 %3 %4")
			.arg(extentRect->xMinimum(), 0, 'f', 8)
			.arg(extentRect->yMaximum(), 0, 'f', 8)
			.arg(extentRect->xMaximum(), 0, 'f', 8)
			.arg(extentRect->yMinimum(), 0, 'f', 8);
		papszOptions = CSLAddString(papszOptions, "-projwin");
		papszOptions = CSLAddString(papszOptions, projwin.toUtf8().constData());
		// projwin 所在 CRS：调用方经 extentCrs 显式指定（范围导出=画布 CRS，主区裁剪=主区 CRS）；
		// extentCrs 为空时回退到画布 CRS 假设。用 -projwin_srs 声明该坐标系，
		// 由 GDAL 内部转换到栅格 SRS，避免 CRS 不一致时裁剪错位/落空
		QgsCoordinateReferenceSystem crs;
		if (extentCrs && extentCrs->isValid())
			crs = *extentCrs;
		else
		{
			QgsMapCanvas* canvas = m_pMapCanvas ? m_pMapCanvas
				: (m_pQgisIface ? m_pQgisIface->mapCanvas() : nullptr);
			crs = seCanvasDestCrs(canvas);
		}
		if (crs.isValid() && !crs.authid().isEmpty())
		{
			papszOptions = CSLAddString(papszOptions, "-projwin_srs");
			papszOptions = CSLAddString(papszOptions, crs.authid().toUtf8().constData());
		}
	}
	papszOptions = CSLAddString(papszOptions, "-of");
	papszOptions = CSLAddString(papszOptions, "GTiff");

	GDALTranslateOptions* psOptions = GDALTranslateOptionsNew(papszOptions, nullptr);
	int bUsageError = 0;
	GDALDatasetH hOutDS = GDALTranslate(dstPath.toUtf8().constData(),
		GDALDataset::ToHandle(poSrcDS), psOptions, &bUsageError);

	GDALTranslateOptionsFree(psOptions);
	CSLDestroy(papszOptions);
	GDALClose(poSrcDS);

	if (hOutDS)
	{
		GDALClose(hOutDS);
		return true;
	}
	errMsg = tr("栅格导出失败：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
	return false;
}

void CSE_DataListExportDialog::appendLog(const QString& msg)
{
	if (m_pLog) m_pLog->append(msg);
}

void CSE_DataListExportDialog::loadDataFileToMap(const QString& filePath)
{
	if (!QFile::exists(filePath))
	{
		QMessageBox::warning(this, tr("在地图显示"), tr("文件不存在：%1").arg(filePath));
		return;
	}
	if (!m_pQgisIface || !m_pQgisIface->mapCanvas())
	{
		QMessageBox::warning(this, tr("在地图显示"), tr("未连接到 QGIS 地图画布，无法加载。"));
		return;
	}

	QString base = QFileInfo(filePath).completeBaseName();
	QgsMapLayer* newLayer = nullptr;

	if (isRasterPath(filePath))
	{
		QgsRasterLayer* rLayer = new QgsRasterLayer(filePath, base, "gdal");
		if (!rLayer || !rLayer->isValid())
		{
			if (rLayer) delete rLayer;
			QMessageBox::warning(this, tr("在地图显示"), tr("无法作为栅格图层加载：%1").arg(filePath));
			return;
		}
		newLayer = rLayer;
	}
	else
	{
		QgsVectorLayer* vLayer = new QgsVectorLayer(filePath, base, "ogr");
		if (!vLayer || !vLayer->isValid())
		{
			if (vLayer) delete vLayer;
			QMessageBox::warning(this, tr("在地图显示"), tr("无法作为矢量图层加载：%1").arg(filePath));
			return;
		}
		newLayer = vLayer;
	}

	// 已存在同名图层则先移除，避免重复
	QgsMapLayer* existed = QgsProject::instance()->mapLayer(newLayer->id());
	if (existed) {} // newLayer 尚未加入，id 不会冲突；保留空判断以示意图
	(void)existed;

	// 加入工程并刷新画布
	if (!QgsProject::instance()->addMapLayer(newLayer))
	{
		delete newLayer;
		QMessageBox::warning(this, tr("在地图显示"), tr("图层加入工程失败：%1").arg(filePath));
		return;
	}

	// 将地图缩放到该图层范围（图层 extent 是自身 CRS 下的坐标，需转换到画布 CRS，
	// 否则投影坐标被当画布坐标解释，跑到地球范围外看不到数据）
	QgsMapCanvas* cvs = m_pQgisIface->mapCanvas();
	cvs->setExtent(seXformRect(newLayer->extent(), newLayer->crs(), seCanvasDestCrs(cvs)));
	cvs->refresh();
	appendLog(tr("[在地图显示] 已加载：%1").arg(filePath));
}

void CSE_DataListExportDialog::on_Button_ConnectDb_clicked()
{
	if (!m_pQgisIface)
	{
		QMessageBox::warning(this, tr("连接数据库"), tr("未连接到 QGIS 接口。"));
		return;
	}
	CSE_DatabaseConnectionDialog dlg(this);
	if (dlg.exec() != QDialog::Accepted) return;

	DatabaseConnectionInfo info = dlg.getConnectionInfo();
	if (!info.isValid())
	{
		QMessageBox::warning(this, tr("连接数据库"), tr("连接信息不完整。"));
		return;
	}

	// 同名覆盖
	for (int i = 0; i < m_dbConns.size(); ++i)
	{
		if (m_dbConns[i].strName == info.strName)
		{
			m_dbConns[i] = info;
			appendLog(tr("[数据库] 已更新连接：%1").arg(info.strName));
			populateDataList();
			return;
		}
	}
	m_dbConns.append(info);
	appendLog(tr("[数据库] 已添加连接：%1 (%2@%3:%4/%5)")
		.arg(info.strName, info.strUsername, info.strHost, info.strPort, info.strDbName));
	populateDataList();
}

void CSE_DataListExportDialog::on_Button_RefreshLayers_clicked()
{
	populateDataList();
}

void CSE_DataListExportDialog::populateDatabaseRoot()
{
	if (!m_pTree) return;
	for (int i = 0; i < m_dbConns.size(); ++i)
	{
		const DatabaseConnectionInfo& c = m_dbConns[i];

		QTreeWidgetItem* dbRoot = new QTreeWidgetItem(m_pTree);
		dbRoot->setText(0, QString("[DB] %1  (%2@%3:%4/%5)")
			.arg(c.strName, c.strUsername, c.strHost, c.strPort, c.strDbName));
		dbRoot->setText(1, tr("数据库"));
		dbRoot->setIcon(0, style()->standardIcon(QStyle::SP_DriveNetIcon));
		// 第 2 列存连接信息索引
		dbRoot->setData(0, Qt::UserRole, QVariant(i));
		dbRoot->setData(0, Qt::UserRole + 1, QVariant((int)NodeDbRoot));

		// 使用 QSqlDatabase 列出 geometry/raster_columns 中的表
		QString connName = QString("dlg_list_%1").arg(c.strName);
		QSqlDatabase db = QSqlDatabase::contains(connName)
			? QSqlDatabase::database(connName)
			: QSqlDatabase::addDatabase("QPSQL", connName);
		db.setHostName(c.strHost);
		db.setPort(c.strPort.toInt());
		db.setDatabaseName(c.strDbName);
		db.setUserName(c.strUsername);
		db.setPassword(c.strPassword);

		if (!db.isOpen() && !db.open())
		{
			QTreeWidgetItem* errItem = new QTreeWidgetItem(dbRoot);
			errItem->setText(0, tr("(连接失败：%1)").arg(db.lastError().text()));
			errItem->setText(1, tr("错误"));
			continue;
		}

		// 矢量表
		QSqlQuery qv(db);
		QString sqlV = R"(SELECT f_table_schema AS s, f_table_name AS t, type AS g FROM geometry_columns
						 UNION ALL
						 SELECT f_table_schema AS s, f_table_name AS t, type AS g FROM geography_columns
						 ORDER BY s, t)";
		if (qv.exec(sqlV))
		{
			while (qv.next())
			{
				QTreeWidgetItem* ti = new QTreeWidgetItem(dbRoot);
				ti->setText(0, QString("%1.%2").arg(qv.value("s").toString(), qv.value("t").toString()));
				ti->setText(1, tr("矢量表 (%1)").arg(qv.value("g").toString()));
				ti->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
				ti->setData(0, Qt::UserRole, QVariant(i)); // 连接索引
				ti->setData(0, Qt::UserRole + 1, QVariant((int)NodeDbTable));
				ti->setData(0, Qt::UserRole + 2, QVariant(false)); // 非栅格
				ti->setData(0, Qt::UserRole + 3, QVariant(qv.value("s").toString()));
				ti->setData(0, Qt::UserRole + 4, QVariant(qv.value("t").toString()));
			}
		}

		// 栅格表
		QSqlQuery qr(db);
		QString sqlR = R"(SELECT r_table_schema AS s, r_table_name AS t FROM raster_columns ORDER BY s, t)";
		if (qr.exec(sqlR))
		{
			while (qr.next())
			{
				QTreeWidgetItem* ti = new QTreeWidgetItem(dbRoot);
				ti->setText(0, QString("%1.%2").arg(qr.value("s").toString(), qr.value("t").toString()));
				ti->setText(1, tr("栅格表"));
				ti->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
				ti->setData(0, Qt::UserRole, QVariant(i));
				ti->setData(0, Qt::UserRole + 1, QVariant((int)NodeDbTable));
				ti->setData(0, Qt::UserRole + 2, QVariant(true));
				ti->setData(0, Qt::UserRole + 3, QVariant(qr.value("s").toString()));
				ti->setData(0, Qt::UserRole + 4, QVariant(qr.value("t").toString()));
			}
		}
	}
}

void CSE_DataListExportDialog::populateMapLayersRoot()
{
	if (!m_pTree) return;
	if (!m_pQgisIface || !m_pQgisIface->mapCanvas()) return;

	QTreeWidgetItem* root = new QTreeWidgetItem(m_pTree);
	root->setText(0, tr("[地图] 当前地图图层"));
	root->setText(1, tr("地图"));
	root->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
	root->setData(0, Qt::UserRole, QVariant(-1));
	root->setData(0, Qt::UserRole + 1, QVariant((int)NodeMapLayerRoot));

	const QMap<QString, QgsMapLayer*>& layers = QgsProject::instance()->mapLayers();
	for (auto it = layers.begin(); it != layers.end(); ++it)
	{
		QgsMapLayer* layer = it.value();
		if (!layer) continue;
		QTreeWidgetItem* ti = new QTreeWidgetItem(root);
		bool bIsRaster = (qobject_cast<QgsRasterLayer*>(layer) != nullptr);
		ti->setText(0, layer->name());
		ti->setText(1, bIsRaster ? tr("栅格图层") : tr("矢量图层"));
		ti->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
		ti->setData(0, Qt::UserRole, QVariant(layer->id()));
		ti->setData(0, Qt::UserRole + 1, QVariant((int)NodeMapLayer));
		ti->setData(0, Qt::UserRole + 2, QVariant(bIsRaster));
	}
}

QString CSE_DataListExportDialog::buildDbTableUri(const DatabaseConnectionInfo& conn,
	const QString& schema, const QString& tableName, bool bRaster) const
{
	QgsDataSourceUri uri;
	uri.setConnection(conn.strHost, conn.strPort, conn.strDbName, conn.strUsername, conn.strPassword);
	if (bRaster)
	{
		// 栅格：使用 PG 栅格驱动格式（GDAL PostGIS Raster）
		return QString("PG:%1:%2.%3")
			.arg(conn.strDbName, schema, tableName);
	}
	else
	{
		uri.setDataSource(schema, tableName, QString(), QString());
		return uri.uri();
	}
}

void CSE_DataListExportDialog::loadItemToMap(QTreeWidgetItem* item)
{
	if (!item) return;
	int nodeType = item->data(0, Qt::UserRole + 1).toInt();

	if (nodeType == NodeMapLayer)
	{
		QString layerId = item->data(0, Qt::UserRole).toString();
		QgsMapLayer* layer = QgsProject::instance()->mapLayer(layerId);
		if (!layer)
		{
			QMessageBox::warning(this, tr("在地图显示"), tr("图层已不存在或被移除。"));
			return;
		}
		QgsMapCanvas* cvs = m_pQgisIface->mapCanvas();
		cvs->setExtent(seXformRect(layer->extent(), layer->crs(), seCanvasDestCrs(cvs)));
		cvs->refresh();
		appendLog(tr("[在地图显示] 已聚焦地图图层：%1").arg(layer->name()));
		return;
	}

	if (nodeType == NodeDbTable)
	{
		int idx = item->data(0, Qt::UserRole).toInt();
		if (idx < 0 || idx >= m_dbConns.size()) return;
		bool bRaster = item->data(0, Qt::UserRole + 2).toBool();
		QString schema = item->data(0, Qt::UserRole + 3).toString();
		QString table = item->data(0, Qt::UserRole + 4).toString();
		const DatabaseConnectionInfo& conn = m_dbConns[idx];

		QString uri = buildDbTableUri(conn, schema, table, bRaster);
		QString base = QString("%1_%2").arg(schema, table);
		QgsMapLayer* newLayer = nullptr;
		if (bRaster)
		{
			QgsRasterLayer* r = new QgsRasterLayer(uri, base, "gdal");
			if (!r || !r->isValid()) { if (r) delete r; QMessageBox::warning(this, tr("在地图显示"), tr("无法加载栅格表。")); return; }
			newLayer = r;
		}
		else
		{
			QgsVectorLayer* v = new QgsVectorLayer(uri, base, "postgres");
			if (!v || !v->isValid()) { if (v) delete v; QMessageBox::warning(this, tr("在地图显示"), tr("无法加载矢量表。")); return; }
			newLayer = v;
		}
		QgsProject::instance()->addMapLayer(newLayer);
		QgsMapCanvas* cvs = m_pQgisIface->mapCanvas();
		cvs->setExtent(seXformRect(newLayer->extent(), newLayer->crs(), seCanvasDestCrs(cvs)));
		cvs->refresh();
		appendLog(tr("[在地图显示] 已加载：%1.%2").arg(schema, table));
		return;
	}

	QMessageBox::information(this, tr("在地图显示"), tr("该节点类型不支持直接显示。"));
}

void CSE_DataListExportDialog::doExportForItem(QTreeWidgetItem* item)
{
	if (!item) return;
	int nodeType = item->data(0, Qt::UserRole + 1).toInt();

	// GDB 子项：路径是 .gdb 目录路径、图层名存在 +5；以 GDB 数据源方式单独导出该图层
	if (nodeType == NodeLocalFile)
	{
		QString gdbPath = item->data(0, Qt::UserRole).toString();
		QString layerName = item->data(0, Qt::UserRole + 5).toString();
		if (!layerName.isEmpty() && isGdbPath(gdbPath))
		{
			QString outDir = browseOutputDir();
			if (outDir.isEmpty()) return;
			QString outSub = outDir;
			QDir().mkpath(outSub);
			QString safeName = layerName;
			safeName.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
			QString dstPath = outSub + "/" + safeName + ".shp";

			// 以OGR方式打开GDB内的指定图层作为源：fileName|.layername=xxx
			QString srcUri = QString("\"%1\" layername=%2").arg(gdbPath, layerName);

			QString err;
			exportVectorFile(srcUri, dstPath, nullptr, nullptr, "", "", "",
				QDateTime(), QDateTime(), false, err);
			if (!err.isEmpty())
				QMessageBox::warning(this, tr("导出"), err);
			else
			{
				appendLog(tr("[导出] 已导出 GDB 图层：%1/%2 -> %3").arg(gdbPath, layerName, dstPath));
				QMessageBox::information(this, tr("导出"), tr("导出完成：%1").arg(dstPath));
				autoLoadToMap(dstPath);
			}
			return;
		}
	}

	if (nodeType == NodeMapLayer || nodeType == NodeDbTable)
	{
		// 简单实现：弹出输出目录选择，然后写入单个文件
		QString outDir = browseOutputDir();
		if (outDir.isEmpty()) return;

		QString outSub = outDir;
		QDir().mkpath(outSub);

		QString srcPath, dstPath;
		bool bRaster = false;

		if (nodeType == NodeMapLayer)
		{
			QString layerId = item->data(0, Qt::UserRole).toString();
			QgsMapLayer* layer = QgsProject::instance()->mapLayer(layerId);
			if (!layer) { QMessageBox::warning(this, tr("导出"), tr("图层已不存在。")); return; }
			bRaster = (qobject_cast<QgsRasterLayer*>(layer) != nullptr);
			QString base = layer->name();
			base.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
			dstPath = outSub + "/" + base + (bRaster ? ".tif" : ".shp");
			QString err;
			if (bRaster)
			{
				exportRasterFile(layer->source(), dstPath, nullptr, nullptr, "", "", "",
					QDateTime(), QDateTime(), false, err);
			}
			else
			{
				exportVectorFile(layer->source(), dstPath, nullptr, nullptr, "", "", "",
					QDateTime(), QDateTime(), false, err);
			}
			if (!err.isEmpty())
			{
				QMessageBox::warning(this, tr("导出"), err);
			}
			else
			{
				appendLog(tr("[导出] 已导出地图图层：%1 -> %2").arg(layer->name(), dstPath));
				QMessageBox::information(this, tr("导出"), tr("导出完成：%1").arg(dstPath));
				autoLoadToMap(dstPath);
			}
		}
		else // NodeDbTable
		{
			int idx = item->data(0, Qt::UserRole).toInt();
			if (idx < 0 || idx >= m_dbConns.size()) return;
			bRaster = item->data(0, Qt::UserRole + 2).toBool();
			QString schema = item->data(0, Qt::UserRole + 3).toString();
			QString table = item->data(0, Qt::UserRole + 4).toString();
			const DatabaseConnectionInfo& conn = m_dbConns[idx];
			QString uri = buildDbTableUri(conn, schema, table, bRaster);
			QString base = QString("%1_%2").arg(schema, table);
			dstPath = outSub + "/" + base + (bRaster ? ".tif" : ".shp");
			QString err;
			if (bRaster)
				exportRasterFile(uri, dstPath, nullptr, nullptr, "", "", "", QDateTime(), QDateTime(), false, err);
			else
				exportVectorFile(uri, dstPath, nullptr, nullptr, "", "", "", QDateTime(), QDateTime(), false, err);
			if (!err.isEmpty())
				QMessageBox::warning(this, tr("导出"), err);
			else
			{
				appendLog(tr("[导出] 已导出数据库表：%1.%2 -> %3").arg(schema, table, dstPath));
				QMessageBox::information(this, tr("导出"), tr("导出完成：%1").arg(dstPath));
				autoLoadToMap(dstPath);
			}
		}
	}
}

void CSE_DataListExportDialog::showResult(bool bOk, const QString& title, const QString& successMsg, const QString& failMsg)
{
	appendLog(successMsg);
	if (bOk)
		QMessageBox::information(this, title, successMsg);
	else
		QMessageBox::warning(this, title, failMsg);
}

// 加载单个文件 (.shp / .tif 等) 到 QGIS 当前地图画布
void CSE_DataListExportDialog::autoLoadToMap(const QString& filePath)
{
	if (!m_pQgisIface || !m_pQgisIface->mapCanvas() || filePath.isEmpty()) return;
	QFileInfo fi(filePath);
	if (!fi.exists()) return;

	QString ext = fi.suffix().toLower();
	if (ext == "shp")
	{
		QgsVectorLayer* pLayer = new QgsVectorLayer(filePath, fi.completeBaseName(), "ogr");
		if (pLayer && pLayer->isValid())
		{
			QgsProject::instance()->addMapLayer(pLayer);
			appendLog(tr("[自动加载] 已加载到地图：%1").arg(filePath));
		}
		else if (pLayer) { delete pLayer; }
	}
	else if (ext == "tif" || ext == "tiff" || ext == "img" || ext == "sid")
	{
		QgsRasterLayer* pLayer = new QgsRasterLayer(filePath, fi.completeBaseName(), "gdal");
		if (pLayer && pLayer->isValid())
		{
			QgsProject::instance()->addMapLayer(pLayer);
			appendLog(tr("[自动加载] 已加载到地图：%1").arg(filePath));
		}
		else if (pLayer) { delete pLayer; }
	}
	else if (fi.isDir())
	{
		autoLoadDirToMap(filePath);
	}
}

// 加载某个输出目录下所有 shp/tif（导出产物按"源目录名"分层写入子目录，须递归扫描）
void CSE_DataListExportDialog::autoLoadDirToMap(const QString& outDir)
{
	if (!m_pQgisIface || !m_pQgisIface->mapCanvas() || outDir.isEmpty()) return;
	QDir dir(outDir);
	if (!dir.exists()) return;

	// 顶层数据文件
	QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
	for (const QFileInfo& fi : files)
	{
		autoLoadToMap(fi.absoluteFilePath());
	}
	// 子目录递归（.gdb 是目录形式的 GDB 数据源，其内部结构不逐个扫描）
	QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QFileInfo& sd : subDirs)
	{
		if (sd.suffix().compare("gdb", Qt::CaseInsensitive) == 0) continue;
		autoLoadDirToMap(sd.absoluteFilePath());
	}
}

// 主界面"导出后自动加载到地图"开关（默认 true，与历史行为一致）
bool CSE_DataListExportDialog::autoLoadAfterExport() const
{
	return m_chkAutoLoadAfterExport && m_chkAutoLoadAfterExport->isChecked();
}

void CSE_DataListExportDialog::reject()
{
	QDialog::reject();
}

// ============================================================
// CSE_ConditionExportDialog
// ============================================================
class CSE_ConditionExportDialog::Private
{
public:
	QLineEdit* lineName = nullptr;
	QLineEdit* lineUser = nullptr;
	QDateTimeEdit* editStart = nullptr;
	QDateTimeEdit* editEnd = nullptr;
	QComboBox* comboType = nullptr;
	QLineEdit* lineOutput = nullptr;
};

CSE_ConditionExportDialog::CSE_ConditionExportDialog(QWidget* parent)
	: QDialog(parent)
	, d(new Private)
{
	setWindowTitle(tr("按条件导出"));
	resize(500, 300);

	d->lineName = new QLineEdit(this);
	d->lineUser = new QLineEdit(this);
	d->editStart = new QDateTimeEdit(QDateTime::currentDateTime().addMonths(-1), this);
	d->editEnd = new QDateTimeEdit(QDateTime::currentDateTime(), this);
	d->editStart->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
	d->editEnd->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
	d->editStart->setCalendarPopup(true);
	d->editEnd->setCalendarPopup(true);
	d->comboType = new QComboBox(this);
	d->comboType->addItems(QStringList() << tr("全部") << tr("矢量数据") << tr("栅格数据"));
	d->lineOutput = new QLineEdit(this);

	QPushButton* btnBrowse = new QPushButton(tr("浏览..."), this);
	QHBoxLayout* outLayout = new QHBoxLayout;
	outLayout->addWidget(d->lineOutput, 1);
	outLayout->addWidget(btnBrowse);

	QFormLayout* form = new QFormLayout;
	form->addRow(tr("数据名称:"), d->lineName);
	form->addRow(tr("用户:"), d->lineUser);
	form->addRow(tr("起始时间:"), d->editStart);
	form->addRow(tr("截止时间:"), d->editEnd);
	form->addRow(tr("类型:"), d->comboType);
	form->addRow(tr("输出路径:"), outLayout);

	QPushButton* btnOK = new QPushButton(tr("确定"), this);
	QPushButton* btnCancel = new QPushButton(tr("取消"), this);
	QHBoxLayout* btnLayout = new QHBoxLayout;
	btnLayout->addStretch();
	btnLayout->addWidget(btnOK);
	btnLayout->addWidget(btnCancel);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addLayout(form);
	mainLayout->addLayout(btnLayout);

	connect(btnBrowse, &QPushButton::clicked, this, &CSE_ConditionExportDialog::on_Button_BrowseOutput_clicked);
	connect(btnOK, &QPushButton::clicked, this, &CSE_ConditionExportDialog::on_Button_OK_clicked);
	connect(btnCancel, &QPushButton::clicked, this, &CSE_ConditionExportDialog::on_Button_Cancel_clicked);
}

CSE_ConditionExportDialog::~CSE_ConditionExportDialog()
{
	delete d;
}

QString CSE_ConditionExportDialog::dataName() const { return d->lineName->text(); }
QString CSE_ConditionExportDialog::userName() const { return d->lineUser->text(); }
QString CSE_ConditionExportDialog::dataType() const { return d->comboType->currentText(); }
QDateTime CSE_ConditionExportDialog::startTime() const { return d->editStart->dateTime(); }
QDateTime CSE_ConditionExportDialog::endTime() const { return d->editEnd->dateTime(); }
QString CSE_ConditionExportDialog::outputPath() const { return d->lineOutput->text(); }

void CSE_ConditionExportDialog::on_Button_BrowseOutput_clicked()
{
	QString dir = QFileDialog::getExistingDirectory(this, tr("选择数据导出路径"));
	if (!dir.isEmpty()) d->lineOutput->setText(dir);
}

void CSE_ConditionExportDialog::on_Button_OK_clicked()
{
	if (d->lineOutput->text().isEmpty())
	{
		QMessageBox::warning(this, tr("按条件导出"), tr("请选择输出路径。"));
		return;
	}
	accept();
}

void CSE_ConditionExportDialog::on_Button_Cancel_clicked() { reject(); }

// ============================================================
// CSE_RangeExportDialog
// ============================================================
class CSE_RangeExportDialog::Private
{
public:
	QgsMapCanvas* canvas = nullptr;
	QgisInterface* iface = nullptr;

	QComboBox* comboMode = nullptr;
	QStackedWidget* stack = nullptr;

	QDoubleSpinBox* spinMinX = nullptr;
	QDoubleSpinBox* spinMinY = nullptr;
	QDoubleSpinBox* spinMaxX = nullptr;
	QDoubleSpinBox* spinMaxY = nullptr;

	QComboBox* comboDrawType = nullptr;
	QPushButton* btnStartDraw = nullptr;
	QLineEdit* lineDrawInfo = nullptr;
	CMapToolDrawShape* drawTool = nullptr;

	QComboBox* comboLayer = nullptr;
	QComboBox* comboField = nullptr;
	QComboBox* comboValue = nullptr;
	QPushButton* btnLoad = nullptr;

	bool m_cursorPushed = false; // 是否已 push Cross cursor 到全局 override stack（按"开始绘制"时）

	QLineEdit* lineOutput = nullptr;
	QCheckBox* chkAutoLoad = nullptr; // 导出后自动加载到地图

	bool bHasExtent = false;
	QgsRectangle rect;

	bool m_bDrawRequested = false; // 用户点击了"开始绘制"
};

CSE_RangeExportDialog::CSE_RangeExportDialog(QgsMapCanvas* canvas, QgisInterface* iface, QWidget* parent)
	: QDialog(parent)
	, d(new Private)
{
	d->canvas = canvas;
	d->iface = iface;
	setWindowTitle(tr("按范围导出"));
	resize(580, 480);

	d->comboMode = new QComboBox(this);
	d->comboMode->addItems(QStringList() << tr("输入坐标") << tr("手动绘制") << tr("选择要素"));

	QWidget* pageCoord = new QWidget(this);
	d->spinMinX = new QDoubleSpinBox(pageCoord);
	d->spinMinY = new QDoubleSpinBox(pageCoord);
	d->spinMaxX = new QDoubleSpinBox(pageCoord);
	d->spinMaxY = new QDoubleSpinBox(pageCoord);
	{
		QDoubleSpinBox* spins[4] = { d->spinMinX, d->spinMinY, d->spinMaxX, d->spinMaxY };
		for (int i = 0; i < 4; ++i)
		{
			spins[i]->setRange(-1e9, 1e9);
			spins[i]->setDecimals(8);
			spins[i]->setSingleStep(100);
		}
	}
	QFormLayout* coordForm = new QFormLayout(pageCoord);
	coordForm->addRow(tr("最小 X:"), d->spinMinX);
	coordForm->addRow(tr("最小 Y:"), d->spinMinY);
	coordForm->addRow(tr("最大 X:"), d->spinMaxX);
	coordForm->addRow(tr("最大 Y:"), d->spinMaxY);
	coordForm->addRow(tr("说明:"), new QLabel(tr("请填写正确的坐标范围（外包矩形）。"), pageCoord));

	QWidget* pageDraw = new QWidget(this);
	d->comboDrawType = new QComboBox(pageDraw);
	d->comboDrawType->addItems(QStringList() << tr("矩形") << tr("圆") << tr("多边形"));
	d->btnStartDraw = new QPushButton(tr("开始绘制"), pageDraw);
	d->lineDrawInfo = new QLineEdit(pageDraw);
	d->lineDrawInfo->setReadOnly(true);
	QFormLayout* drawForm = new QFormLayout(pageDraw);
	drawForm->addRow(tr("绘制类型:"), d->comboDrawType);
	drawForm->addRow(d->btnStartDraw);
	drawForm->addRow(tr("范围信息:"), d->lineDrawInfo);

	QWidget* pageFeature = new QWidget(this);
	d->comboLayer = new QComboBox(pageFeature);
	d->comboField = new QComboBox(pageFeature);
	d->comboValue = new QComboBox(pageFeature);
	d->comboValue->setEditable(true);
	d->btnLoad = new QPushButton(tr("读取要素"), pageFeature);
	QFormLayout* featureForm = new QFormLayout(pageFeature);
	featureForm->addRow(tr("矢量图层:"), d->comboLayer);
	featureForm->addRow(tr("属性字段:"), d->comboField);
	featureForm->addRow(tr("要素值:"), d->comboValue);
	featureForm->addRow(d->btnLoad);

	if (iface && iface->mapCanvas())
	{
		const QList<QgsMapLayer*>& layers = iface->mapCanvas()->layers();
		for (QgsMapLayer* l : layers)
		{
			if (l->type() == QgsMapLayerType::VectorLayer)
				d->comboLayer->addItem(l->name(), QVariant::fromValue<void*>((void*)l));
		}
	}
	if (d->comboLayer->count() == 0)
		d->comboLayer->addItem(tr("（无矢量图层）"));

	d->stack = new QStackedWidget(this);
	d->stack->addWidget(pageCoord);
	d->stack->addWidget(pageDraw);
	d->stack->addWidget(pageFeature);

	QHBoxLayout* outLayout = new QHBoxLayout;
	d->lineOutput = new QLineEdit(this);
	QPushButton* btnBrowse = new QPushButton(tr("浏览..."), this);
	outLayout->addWidget(d->lineOutput, 1);
	outLayout->addWidget(btnBrowse);

	QPushButton* btnOK = new QPushButton(tr("确定"), this);
	QPushButton* btnCancel = new QPushButton(tr("取消"), this);
	// 导出后自动加载到地图（默认状态从主对话框同步）
	d->chkAutoLoad = new QCheckBox(tr("导出后自动加载到地图"), this);
	{
		QWidget* w = parentWidget();
		CSE_DataListExportDialog* mainDlg = qobject_cast<CSE_DataListExportDialog*>(w);
		d->chkAutoLoad->setChecked(mainDlg ? mainDlg->autoLoadAfterExport() : true);
	}
	QHBoxLayout* btnLayout = new QHBoxLayout;
	btnLayout->addWidget(d->chkAutoLoad);
	btnLayout->addStretch();
	btnLayout->addWidget(btnOK);
	btnLayout->addWidget(btnCancel);

	QFormLayout* modeForm = new QFormLayout;
	modeForm->addRow(tr("范围获取方式:"), d->comboMode);

	QFormLayout* outForm = new QFormLayout;
	outForm->addRow(tr("输出文件:"), outLayout);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addLayout(modeForm);
	mainLayout->addWidget(d->stack);
	mainLayout->addLayout(outForm);
	mainLayout->addLayout(btnLayout);

	connect(d->comboMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &CSE_RangeExportDialog::onModeChanged);
	connect(btnBrowse, &QPushButton::clicked, this, &CSE_RangeExportDialog::on_Button_BrowseOutput_clicked);
	connect(btnOK, &QPushButton::clicked, this, &CSE_RangeExportDialog::on_Button_OK_clicked);
	connect(btnCancel, &QPushButton::clicked, this, &CSE_RangeExportDialog::on_Button_Cancel_clicked);
	connect(d->btnStartDraw, &QPushButton::clicked, this, [this]()
	{
		// 标记用户需要在地图上绘制，然后关闭对话框（close 会结束 modal exec()）。
		// 真正的绘制流程在调用方（非模态环境）中完成，绘制成功后重新弹出本对话框并带上已绘制的范围。
		d->m_bDrawRequested = true;
		accept();
	});
	connect(d->btnLoad, &QPushButton::clicked, this, &CSE_RangeExportDialog::on_Button_LoadFeatures_clicked);
}

void CSE_RangeExportDialog::setExtent(const QgsRectangle& rect)
{
	if (!rect.isNull())
	{
		d->rect = rect;
		d->bHasExtent = true;
		// 同步到坐标输入框
		if (d->spinMinX) d->spinMinX->setValue(rect.xMinimum());
		if (d->spinMinY) d->spinMinY->setValue(rect.yMinimum());
		if (d->spinMaxX) d->spinMaxX->setValue(rect.xMaximum());
		if (d->spinMaxY) d->spinMaxY->setValue(rect.yMaximum());
		// 判断 x 是否落在经度范围 [-180, 180]、y 是否落在 [-90, 90]，若是则视为经纬度
		bool looksLikeLatLon =
			rect.xMinimum() >= -180.0 && rect.xMaximum() <= 180.0 &&
			rect.yMinimum() >= -90.0 && rect.yMaximum() <= 90.0 &&
			std::abs(rect.xMaximum() - rect.xMinimum()) <= 360.0 &&
			std::abs(rect.yMaximum() - rect.yMinimum()) <= 180.0;
		if (looksLikeLatLon)
		{
			d->lineDrawInfo->setText(tr("已绘制范围（WGS84 经纬度）：\n经度 %1 ~ %2，纬度 %3 ~ %4")
				.arg(rect.xMinimum(), 0, 'f', 6)
				.arg(rect.xMaximum(), 0, 'f', 6)
				.arg(rect.yMinimum(), 0, 'f', 6)
				.arg(rect.yMaximum(), 0, 'f', 6));
		}
		else
		{
			d->lineDrawInfo->setText(tr("已绘制范围（原始投影坐标）：\n(x: %1, y: %2) - (x: %3, y: %4)")
				.arg(rect.xMinimum(), 0, 'f', 2)
				.arg(rect.yMinimum(), 0, 'f', 2)
				.arg(rect.xMaximum(), 0, 'f', 2)
				.arg(rect.yMaximum(), 0, 'f', 2));
		}
	}
}

bool CSE_RangeExportDialog::drawRequested() const
{
	return d->m_bDrawRequested;
}

void CSE_RangeExportDialog::resetDrawRequested()
{
	d->m_bDrawRequested = false;
}

DrawShapeType CSE_RangeExportDialog::selectedDrawType() const
{
	if (!d->comboDrawType) return DrawRect;
	switch (d->comboDrawType->currentIndex())
	{
	case 1: return DrawCircle;
	case 2: return DrawPolygon;
	case 0:
	default:
		return DrawRect;
	}
}

void CSE_RangeExportDialog::setModeIndex(int idx)
{
	if (d->comboMode)
	{
		// 使用 blockSignals 避免触发 onModeChanged 中重新构建 stack 之类副作用
		d->comboMode->blockSignals(true);
		d->comboMode->setCurrentIndex(idx);
		d->comboMode->blockSignals(false);
		// 显式同步 stack 与 lineDrawInfo 等"手动绘制"页 widget 的可见性
		if (d->stack) d->stack->setCurrentIndex(idx);
	}
}

int CSE_RangeExportDialog::modeIndex() const
{
	return d->comboMode ? d->comboMode->currentIndex() : 0;
}

void CSE_RangeExportDialog::setDrawTypeIndex(int idx)
{
	if (d->comboDrawType)
	{
		d->comboDrawType->blockSignals(true);
		d->comboDrawType->setCurrentIndex(idx);
		d->comboDrawType->blockSignals(false);
	}
}

int CSE_RangeExportDialog::drawTypeIndex() const
{
	return d->comboDrawType ? d->comboDrawType->currentIndex() : 0;
}

// 是否在导出后自动加载结果到地图（默认 true）
bool CSE_RangeExportDialog::autoLoadAfterExport() const
{
	return d->chkAutoLoad && d->chkAutoLoad->isChecked();
}

CSE_RangeExportDialog::~CSE_RangeExportDialog()
{
	// 析构期间若仍有未恢复的 Cross cursor，恢复它避免全局光标栈失衡
	if (d->m_cursorPushed) { QApplication::restoreOverrideCursor(); d->m_cursorPushed = false; }
	if (d->drawTool)
	{
		disconnect(d->drawTool, nullptr, this, nullptr); // 析构期间断开，避免 drawCancelled 操作即将销毁的对话框
		d->drawTool->deactivate();
		delete d->drawTool; d->drawTool = nullptr;
	}
	delete d;
}

QgsRectangle CSE_RangeExportDialog::exportExtent() const { return d->rect; }
bool CSE_RangeExportDialog::hasExtent() const { return d->bHasExtent; }
QString CSE_RangeExportDialog::outputPath() const { return d->lineOutput->text(); }

void CSE_RangeExportDialog::onModeChanged(int index)
{
	d->stack->setCurrentIndex(index);
}

void CSE_RangeExportDialog::onShapeFinished(double minX, double minY, double maxX, double maxY)
{
	d->bHasExtent = true;
	d->rect = QgsRectangle(minX, minY, maxX, maxY);
	d->lineDrawInfo->setText(tr("外包矩形：(%1, %2) - (%3, %4)")
		.arg(minX).arg(minY).arg(maxX).arg(maxY));
	// 释放画布可能对鼠标的强制捕获，避免后续 dialog 鼠标记号不响应
	if (d->canvas) d->canvas->releaseMouse();
	// 隐藏 drawTool（已通过 deactivate 清理，避免工具残留影响后续操作）
	if (d->drawTool) { d->drawTool->deactivate(); delete d->drawTool; d->drawTool = nullptr; }
	// 恢复全局光标（按"开始绘制"时 push 的 Cross cursor）
	if (d->m_cursorPushed) { QApplication::restoreOverrideCursor(); d->m_cursorPushed = false; }
	// 绘制完成后恢复本对话框（之前 hide 了，这里 show + raise + activateWindow 拉回前台）
	this->show();
	this->raise();
	this->activateWindow();
}

void CSE_RangeExportDialog::onDrawCancelled()
{
	d->lineDrawInfo->setText(tr("绘制已取消。"));
	// 释放画布可能对鼠标的强制捕获
	if (d->canvas) d->canvas->releaseMouse();
	// 恢复全局光标
	if (d->m_cursorPushed) { QApplication::restoreOverrideCursor(); d->m_cursorPushed = false; }
	// 取消时同样恢复本对话框（之前 hide 了，这里 show + raise + activateWindow 拉回前台）
	this->show();
	this->raise();
	this->activateWindow();
}

void CSE_RangeExportDialog::on_Button_BrowseOutput_clicked()
{
	// 输出为单个文件：用户直接选择目标文件路径（如 D:\rrr\a.shp）
	QString suggest = d->lineOutput->text();
	if (suggest.isEmpty()) suggest = QDir::homePath() + "/range_export.shp";
	QString f = QFileDialog::getSaveFileName(this, tr("选择输出文件"), suggest,
		tr("Shapefile (*.shp);;GeoTIFF (*.tif);;所有文件 (*)"));
	if (!f.isEmpty()) d->lineOutput->setText(f);
}

void CSE_RangeExportDialog::on_Button_LoadFeatures_clicked()
{
	void* p = d->comboLayer->currentData().value<void*>();
	if (!p) return;
	QgsMapLayer* ml = (QgsMapLayer*)p;
	QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>(ml);
	if (!vl) return;

	if (d->comboField->count() == 0)
	{
		for (const QgsField& f : vl->fields())
			d->comboField->addItem(f.name());
	}

	int fieldIdx = d->comboField->currentIndex();
	if (fieldIdx < 0) return;
	d->comboValue->clear();

	QgsFeatureRequest req;
	req.setSubsetOfAttributes(QgsAttributeList() << fieldIdx);
	QgsFeatureIterator it = vl->getFeatures(req);
	QgsFeature f;
	QSet<QString> values;
	while (it.nextFeature(f))
	{
		QVariant v = f.attribute(fieldIdx);
		if (!v.isNull())
			values.insert(v.toString());
	}
	QStringList sorted = values.values();
	sorted.sort();
	d->comboValue->addItems(sorted);
	QMessageBox::information(this, tr("选择要素"),
		tr("已读取 %1 个要素值，请选择一个要素值来定义导出范围。").arg(sorted.size()));
}

void CSE_RangeExportDialog::on_Button_OK_clicked()
{
	if (d->lineOutput->text().isEmpty())
	{
		QMessageBox::warning(this, tr("按范围导出"), tr("请选择输出路径。"));
		return;
	}

	switch (d->stack->currentIndex())
	{
	case RangeInputCoord:
	{
		double minX = d->spinMinX->value(), minY = d->spinMinY->value();
		double maxX = d->spinMaxX->value(), maxY = d->spinMaxY->value();
		if (minX >= maxX || minY >= maxY)
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("坐标范围不合法，请检查最小/最大值。"));
			return;
		}
		d->bHasExtent = true;
		d->rect = QgsRectangle(minX, minY, maxX, maxY);
		break;
	}
	case RangeManualDraw:
	{
		if (!d->bHasExtent)
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("请先在地图上绘制导出范围。"));
			return;
		}
		break;
	}
	case RangeSelectFeature:
	{
		void* p = d->comboLayer->currentData().value<void*>();
		if (!p)
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("请选择矢量图层。"));
			return;
		}
		QgsMapLayer* ml = (QgsMapLayer*)p;
		QgsVectorLayer* vl = qobject_cast<QgsVectorLayer*>(ml);
		if (!vl)
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("所选图层不是矢量数据。"));
			return;
		}
		int fieldIdx = d->comboField->currentIndex();
		if (fieldIdx < 0 || d->comboValue->currentText().isEmpty())
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("请先读取要素并选择一个要素值。"));
			return;
		}
		QString val = d->comboValue->currentText();
		QgsFeatureRequest req;
		req.setSubsetOfAttributes(QgsAttributeList() << fieldIdx);
		QgsFeatureIterator it = vl->getFeatures(req);
		QgsFeature f;
		bool found = false;
		while (it.nextFeature(f))
		{
			if (f.attribute(fieldIdx).toString() == val)
			{
				d->rect = f.geometry().boundingBox();
				found = true;
				break;
			}
		}
		if (!found)
		{
			QMessageBox::warning(this, tr("按范围导出"), tr("未找到对应要素。"));
			return;
		}
		d->bHasExtent = true;
		break;
	}
	}
	accept();
}

void CSE_RangeExportDialog::on_Button_Cancel_clicked()
{
	// 取消前恢复全局光标（避免遗留 Cross cursor 在系统里）
	if (d->m_cursorPushed) { QApplication::restoreOverrideCursor(); d->m_cursorPushed = false; }
	if (d->drawTool)
	{
		disconnect(d->drawTool, nullptr, this, nullptr);
		d->drawTool->deactivate();
		delete d->drawTool; d->drawTool = nullptr;
	}
	reject();
}

// ============================================================
// CSE_MainAreaExportDialog —— 精简"按主区裁切导出"对话框
// ============================================================
class CSE_MainAreaExportDialog::Private
{
public:
	QLineEdit* lineMainArea = nullptr;    // 主区数据文件
	QPushButton* btnBrowseMainArea = nullptr;
	QComboBox* comboField = nullptr;
	QListWidget* listFeatures = nullptr;

	QLineEdit* lineOutput = nullptr;
	QPushButton* btnBrowseOutput = nullptr;
	QComboBox* comboEncoding = nullptr;
	QComboBox* comboFormat = nullptr;
	QCheckBox* checkOverwrite = nullptr;

	// 制图
	QComboBox* comboPaperSize = nullptr;
	QPushButton* btnComputePaper = nullptr; // "计算"：根据主区范围自动确定纸张方向与大小
	QComboBox* comboOrient = nullptr;
	QLabel* labelCustomW = nullptr;
	QLineEdit* lineCustomW = nullptr;
	QLabel* labelCustomH = nullptr;
	QLineEdit* lineCustomH = nullptr;

	QComboBox* comboStandardScale = nullptr;   // 标准比例尺（对照，整比例尺列表）
	QPushButton* btnCompute = nullptr;         // 自动计算
	QCheckBox* checkCustomScale = nullptr;     // 使用自定义比例尺
	QLabel* labelCustomScale = nullptr;        // 自定义比例尺标签
	QLineEdit* lineCustomScale = nullptr;      // 自定义比例尺输入框

	QLineEdit* lineInnerW = nullptr;
	QLineEdit* lineInnerH = nullptr;
	QCheckBox* checkEnableInner = nullptr;
	QLabel* labelInnerW = nullptr;
	QLabel* labelInnerH = nullptr;

	QCheckBox* chkAutoLoad = nullptr; // 导出后自动加载到地图

	QPushButton* btnOK = nullptr;
	QPushButton* btnCancel = nullptr;
};

CSE_MainAreaExportDialog::CSE_MainAreaExportDialog(QWidget* parent)
	: QDialog(parent)
	, d(new Private)
{
	setWindowTitle(tr("按主区裁切导出"));
	resize(620, 600);
	buildUi();
	applyPaperSizeVisibility();
}

CSE_MainAreaExportDialog::~CSE_MainAreaExportDialog()
{
	if (m_pMainAreaLayer) delete m_pMainAreaLayer;
	delete d;
}

void CSE_MainAreaExportDialog::buildUi()
{
	QVBoxLayout* root = new QVBoxLayout(this);

	// 主区数据
	{
		QHBoxLayout* row = new QHBoxLayout;
		QLabel* lab = new QLabel(tr("主区数据："), this);
		d->lineMainArea = new QLineEdit(this);
		d->btnBrowseMainArea = new QPushButton(tr("浏览..."), this);
		row->addWidget(lab);
		row->addWidget(d->lineMainArea, 1);
		row->addWidget(d->btnBrowseMainArea);
		root->addLayout(row);
	}

	// 标识字段
	{
		QHBoxLayout* row = new QHBoxLayout;
		QLabel* lab = new QLabel(tr("标识字段："), this);
		d->comboField = new QComboBox(this);
		row->addWidget(lab);
		row->addWidget(d->comboField, 1);
		root->addLayout(row);
	}

	// 要素列表
	{
		QLabel* lab = new QLabel(tr("选择要素（可多选）："), this);
		root->addWidget(lab);
		d->listFeatures = new QListWidget(this);
		d->listFeatures->setSelectionMode(QAbstractItemView::ExtendedSelection);
		root->addWidget(d->listFeatures, 1);
	}

	{
		QHBoxLayout* row = new QHBoxLayout;
		d->comboEncoding = new QComboBox(this);
		d->comboEncoding->addItems(QStringList() << "UTF-8" << "GBK" << "GB2312");
		d->comboFormat = new QComboBox(this);
		d->comboFormat->addItems(QStringList() << "ESRI Shapefile (.shp)" << "GeoPackage (.gpkg)" << "GDB (.gdb)");
		d->checkOverwrite = new QCheckBox(tr("覆盖已存在文件"), this);
		d->checkOverwrite->setChecked(true);
		row->addWidget(new QLabel(tr("编码："), this));
		row->addWidget(d->comboEncoding);
		row->addWidget(new QLabel(tr("格式："), this));
		row->addWidget(d->comboFormat);
		row->addStretch();
		row->addWidget(d->checkOverwrite);
		root->addLayout(row);
	}

	// 制图
	{
		QGroupBox* gb = new QGroupBox(tr("制图"), this);
		QGridLayout* g = new QGridLayout(gb);

		g->addWidget(new QLabel(tr("纸张方向："), this), 0, 0);
		d->comboOrient = new QComboBox(this);
		d->comboOrient->addItems(QStringList() << tr("自动") << tr("纵向") << tr("横向"));
		g->addWidget(d->comboOrient, 0, 1);

		g->addWidget(new QLabel(tr("纸张大小："), this), 0, 2);
		d->comboPaperSize = new QComboBox(this);
		QStringList papers = QStringList() << "A0" << "A1" << "A2" << "A3" << "A4";
		d->comboPaperSize->addItems(papers);
		d->comboPaperSize->addItem(tr("自定义"));
		// 随内容自适应宽度："计算"按钮会把文本写成"A0：893.0*654.0"这类长组合文本，
		// 默认 AdjustToContentsOnFirstShow 首显后不重算，会导致文本被裁剪
		d->comboPaperSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		d->comboPaperSize->setMinimumWidth(100); // 纯"A0"等短文本时也不至于太窄
		d->comboPaperSize->setCurrentText("A2");
		g->addWidget(d->comboPaperSize, 0, 3);

		d->btnComputePaper = new QPushButton(tr("计算"), this);
		d->btnComputePaper->setToolTip(tr("根据主区范围自动确定纸张方向与大小"));
		g->addWidget(d->btnComputePaper, 0, 4);

		d->labelCustomW = new QLabel(tr("自定义宽(mm)："), this);
		d->lineCustomW = new QLineEdit(this);
		d->lineCustomW->setText("594");
		g->addWidget(d->labelCustomW, 1, 0);
		g->addWidget(d->lineCustomW, 1, 1);

		d->labelCustomH = new QLabel(tr("自定义高(mm)："), this);
		d->lineCustomH = new QLineEdit(this);
		d->lineCustomH->setText("420");
		g->addWidget(d->labelCustomH, 1, 2);
		g->addWidget(d->lineCustomH, 1, 3);

		root->addWidget(gb);
	}

	// 比例尺 / 内图廓
	{
		QGroupBox* gb = new QGroupBox(tr("比例尺 / 内图廓"), this);
		QGridLayout* g = new QGridLayout(gb);

		// 第一行：标准比例尺（对照）+ 自动计算 + 使用自定义
		g->addWidget(new QLabel(tr("标准比例尺："), this), 0, 0);
		d->comboStandardScale = new QComboBox(this);
		d->comboStandardScale->setEditable(true); // 允许手动输入任意标准值
		QStringList stdScales;
		stdScales << "500" << "1000" << "2000" << "2500" << "5000" << "10000"
			<< "25000" << "50000" << "100000" << "200000" << "500000"
			<< "1000000" << "2000000";
		d->comboStandardScale->addItems(stdScales);
		d->comboStandardScale->setCurrentText("10000");
		g->addWidget(d->comboStandardScale, 0, 1);

		d->btnCompute = new QPushButton(tr("自动计算"), this);
		g->addWidget(d->btnCompute, 0, 2);
		d->checkCustomScale = new QCheckBox(tr("使用自定义"), this);
		g->addWidget(d->checkCustomScale, 0, 3);

		// 第二行：自定义比例尺输入框（默认隐藏，勾选"使用自定义"后显示）
		d->labelCustomScale = new QLabel(tr("自定义比例尺："), this);
		d->lineCustomScale = new QLineEdit(this);
		d->lineCustomScale->setText("10000");
		d->lineCustomScale->setPlaceholderText(tr("请输入比例尺分母，如 12000"));
		d->labelCustomScale->setVisible(false);
		d->lineCustomScale->setVisible(false);
		g->addWidget(d->labelCustomScale, 1, 0);
		g->addWidget(d->lineCustomScale, 1, 1, 1, 3);

		// 第三行：内图廓
		d->labelInnerW = new QLabel(tr("内图廓宽(mm)："), this);
		d->lineInnerW = new QLineEdit(this);
		d->lineInnerW->setText("0");
		g->addWidget(d->labelInnerW, 2, 0);
		g->addWidget(d->lineInnerW, 2, 1);

		d->labelInnerH = new QLabel(tr("内图廓高(mm)："), this);
		d->lineInnerH = new QLineEdit(this);
		d->lineInnerH->setText("0");
		g->addWidget(d->labelInnerH, 2, 2);
		g->addWidget(d->lineInnerH, 2, 3);
		d->checkEnableInner = new QCheckBox(tr("启用内图廓"), this);
		g->addWidget(d->checkEnableInner, 2, 4);
		// 未启用时宽/高输入框置灰
		applyInnerFrameEnabled(false);

		root->addWidget(gb);
	}

	// 输出文件
	{
		QHBoxLayout* row = new QHBoxLayout;
		QLabel* lab = new QLabel(tr("输出文件："), this);
		d->lineOutput = new QLineEdit(this);
		d->btnBrowseOutput = new QPushButton(tr("浏览..."), this);
		row->addWidget(lab);
		row->addWidget(d->lineOutput, 1);
		row->addWidget(d->btnBrowseOutput);
		root->addLayout(row);
	}

	// OK / Cancel
	{
		// 导出后自动加载到地图（默认状态从主对话框同步）
		d->chkAutoLoad = new QCheckBox(tr("导出后自动加载到地图"), this);
		{
			QWidget* w = parentWidget();
			CSE_DataListExportDialog* mainDlg = qobject_cast<CSE_DataListExportDialog*>(w);
			d->chkAutoLoad->setChecked(mainDlg ? mainDlg->autoLoadAfterExport() : true);
		}

		QHBoxLayout* row = new QHBoxLayout;
		row->addWidget(d->chkAutoLoad);
		row->addStretch();
		d->btnOK = new QPushButton(tr("导出"), this);
		d->btnOK->setDefault(true);
		d->btnCancel = new QPushButton(tr("取消"), this);
		row->addWidget(d->btnOK);
		row->addWidget(d->btnCancel);
		root->addLayout(row);
	}

	connect(d->btnBrowseMainArea, &QPushButton::clicked, this, &CSE_MainAreaExportDialog::on_Button_BrowseMainArea_clicked);
	connect(d->btnBrowseOutput,   &QPushButton::clicked, this, &CSE_MainAreaExportDialog::on_Button_BrowseOutput_clicked);
	connect(d->comboField, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, [this](int) { this->reloadFeatures(); });
	connect(d->comboPaperSize, QOverload<int>::of(&QComboBox::currentIndexChanged),
		this, &CSE_MainAreaExportDialog::onPaperSizeChanged);
	connect(d->btnCompute, &QPushButton::clicked,
		this, &CSE_MainAreaExportDialog::onBtnComputeScale);
	connect(d->btnComputePaper, &QPushButton::clicked,
		this, &CSE_MainAreaExportDialog::onBtnComputePaper);
	connect(d->checkCustomScale, &QCheckBox::toggled,
		this, &CSE_MainAreaExportDialog::onCheckCustomScaleToggled);
	connect(d->checkEnableInner, &QCheckBox::toggled,
		this, &CSE_MainAreaExportDialog::onCheckEnableInnerToggled);
	connect(d->btnOK,     &QPushButton::clicked, this, &CSE_MainAreaExportDialog::on_Button_OK_clicked);
	connect(d->btnCancel, &QPushButton::clicked, this, &CSE_MainAreaExportDialog::on_Button_Cancel_clicked);
}

void CSE_MainAreaExportDialog::applyPaperSizeVisibility()
{
	bool custom = (d->comboPaperSize && d->comboPaperSize->currentText() == tr("自定义"));
	d->labelCustomW->setVisible(custom);
	d->lineCustomW->setVisible(custom);
	d->labelCustomH->setVisible(custom);
	d->lineCustomH->setVisible(custom);
}

void CSE_MainAreaExportDialog::onPaperSizeChanged(int /*index*/)
{
	applyPaperSizeVisibility();
}

void CSE_MainAreaExportDialog::applyInnerFrameEnabled(bool enabled)
{
	if (!d->lineInnerW || !d->lineInnerH || !d->labelInnerW || !d->labelInnerH)
		return;
	d->labelInnerW->setEnabled(enabled);
	d->lineInnerW->setEnabled(enabled);
	d->labelInnerH->setEnabled(enabled);
	d->lineInnerH->setEnabled(enabled);
}

void CSE_MainAreaExportDialog::onCheckCustomScaleToggled(bool checked)
{
	// 勾选"使用自定义"：显示自定义输入框、禁用标准下拉框与自动计算；否则反之
	d->labelCustomScale->setVisible(checked);
	d->lineCustomScale->setVisible(checked);
	d->comboStandardScale->setEnabled(!checked);
	d->btnCompute->setEnabled(!checked);
	if (checked)
	{
		// 用当前标准值填充自定义输入框，方便微调
		QString cur = d->comboStandardScale->currentText().trimmed();
		if (cur.isEmpty()) cur = d->lineCustomScale->text();
		if (!cur.isEmpty()) d->lineCustomScale->setText(cur);
	}
}

void CSE_MainAreaExportDialog::onCheckEnableInnerToggled(bool checked)
{
	applyInnerFrameEnabled(checked);
}

// 当前已选要素的合并外包矩形（在主区 layer 的原始 CRS 下）
QgsRectangle CSE_MainAreaExportDialog::selectedMainAreaRect() const
{
	QgsRectangle rect;
	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
		return rect;
	const int n = d->listFeatures->count();
	QgsFeatureIds idSet;
	for (int i = 0; i < n; ++i)
	{
		QListWidgetItem* item = d->listFeatures->item(i);
		if (item && item->isSelected())
		{
			bool ok = false;
			long long v = item->data(Qt::UserRole).toString().toLongLong(&ok);
			if (ok) idSet.insert(v);
		}
	}
	if (idSet.isEmpty())
		return rect;

	QgsFeatureRequest req;
	req.setFilterFids(idSet);
	QgsFeatureIterator it = m_pMainAreaLayer->getFeatures(req);
	QgsFeature feat;
	bool first = true;
	while (it.nextFeature(feat))
	{
		QgsGeometry g = feat.geometry();
		if (g.isNull()) continue;
		QgsRectangle b = g.boundingBox();
		if (b.isEmpty()) continue;
		if (first) { rect = b; first = false; }
		else rect.combineExtentWith(b);
	}
	return rect;
}

// 当前生效的纸张宽/高(mm)。A0~A4 固定（兼容"计算"按钮写入的"A0：893.0*654.0"组合文本，
// 取纸张系列前缀对应的 ISO 尺寸）；"自定义"读输入框
double CSE_MainAreaExportDialog::paperWidthMm() const
{
	double w, h;
	if (seParsePaperSpec(paperSize(), w, h)) return w;
	return d->lineCustomW->text().toDouble();
}

double CSE_MainAreaExportDialog::paperHeightMm() const
{
	double w, h;
	if (seParsePaperSpec(paperSize(), w, h)) return h;
	return d->lineCustomH->text().toDouble();
}

// 把比例尺分母向上取整为"整比例尺"（1/2/5 * 10^n 系列），保证无小数且接近计算值
static double roundScaleToNice(double scale)
{
	if (scale <= 0) return 1;
	// 求量级：使 scale 落在 [1,10) 区间
	double mag = std::pow(10.0, std::floor(std::log10(scale)));
	double m = scale / mag; // 1<=m<10
	// 取 1/2/5 系列中第一个 >= m 的
	double nice;
	if (m <= 1.0) nice = 1.0;
	else if (m <= 2.0) nice = 2.0;
	else if (m <= 5.0) nice = 5.0;
	else nice = 10.0;
	return nice * mag;
}

// 依据当前纸张 + 选中主区外包矩形自动计算比例尺，写回标准下拉框
void CSE_MainAreaExportDialog::computeScaleFromExtent()
{
	QgsRectangle rect = selectedMainAreaRect();
	if (rect.isEmpty())
	{
		QMessageBox::information(this, tr("自动计算比例尺"),
			tr("请先在要素列表中选中至少一个要素作为主区，再点击自动计算。"));
		return;
	}
	const double W = rect.width();  // 地面宽度（主区 CRS 单位，通常为米）
	const double H = rect.height(); // 地面高度
	if (W <= 0 || H <= 0)
	{
		QMessageBox::warning(this, tr("自动计算比例尺"), tr("主区范围无效，无法计算。"));
		return;
	}

	// 纸幅（mm）换算为米，预留页边距（四周各留 10mm）
	double paperW_m = std::max(1.0, paperWidthMm() - 20.0) / 1000.0;
	double paperH_m = std::max(1.0, paperHeightMm() - 20.0) / 1000.0;

	// 所需比例尺分母 = 地面尺寸 / 纸面尺寸（取两个方向较大者）
	double scaleW = W / paperW_m;
	double scaleH = H / paperH_m;
	double scale = roundScaleToNice(std::max(scaleW, scaleH));

	d->comboStandardScale->setCurrentText(QString::number(static_cast<long long>(scale)));
	QMessageBox::information(this, tr("自动计算比例尺"),
		tr("主区外包矩形：%1 m × %2 m\n纸幅：%3 mm × %4 mm\n\n推荐标准比例尺：1:%5")
		.arg(QString::number(W, 'f', 1))
		.arg(QString::number(H, 'f', 1))
		.arg(QString::number(paperWidthMm(), 'f', 0))
		.arg(QString::number(paperHeightMm(), 'f', 0))
		.arg(QString::number(scale, 'f', 0)));
}

void CSE_MainAreaExportDialog::onBtnComputeScale()
{
	computeScaleFromExtent();
}

// "计算"按钮：依据主区外包矩形 + 当前比例尺，自动确定纸张方向与大小
// 绘图尺寸(mm) = 地面尺寸(米) / 比例尺 × 1000
// 方向：宽 ≥ 高 → 横向，否则纵向
// 纸张：选能容纳绘图尺寸的最小 ISO A 系列，combo 文本置为 "A0：893.0*654.0"
//（纸张类型后追加绘图尺寸）；超过 A0 时落"自定义"并填绘图尺寸
void CSE_MainAreaExportDialog::onBtnComputePaper()
{
	QgsRectangle rect = selectedMainAreaRect();
	if (rect.isEmpty())
	{
		QMessageBox::information(this, tr("计算纸张"),
			tr("请先在要素列表中选中至少一个要素作为主区，再点击计算。"));
		return;
	}
	const double scale = mapScaleDenominator();
	if (scale <= 0)
	{
		QMessageBox::warning(this, tr("计算纸张"), tr("比例尺无效，无法计算。"));
		return;
	}

	// 主区外包矩形 → 地面尺寸（米）。主区数据通常为平面投影（米）；
	// 经纬度时转换到 Web 墨卡托（米）兜底，英尺/英里/海里换算为米
	double groundW = rect.width();
	double groundH = rect.height();
	if (m_pMainAreaLayer && m_pMainAreaLayer->crs().isValid())
	{
		const QgsCoordinateReferenceSystem crs = m_pMainAreaLayer->crs();
		if (crs.mapUnits() == QgsUnitTypes::DistanceDegrees)
		{
			QgsCoordinateTransform tr(crs,
				QgsCoordinateReferenceSystem::fromEpsgId(3857), QgsProject::instance());
			if (tr.isValid())
			{
				QgsRectangle mRect;
				try { mRect = tr.transformBoundingBox(rect); }
				catch (...) { }
				if (!mRect.isEmpty()) { groundW = mRect.width(); groundH = mRect.height(); }
			}
		}
		else if (crs.mapUnits() == QgsUnitTypes::DistanceFeet)
		{ groundW *= 0.3048; groundH *= 0.3048; }
		else if (crs.mapUnits() == QgsUnitTypes::DistanceMiles)
		{ groundW *= 1609.344; groundH *= 1609.344; }
		else if (crs.mapUnits() == QgsUnitTypes::DistanceNauticalMiles)
		{ groundW *= 1852.0; groundH *= 1852.0; }
	}
	if (groundW <= 0 || groundH <= 0)
	{
		QMessageBox::warning(this, tr("计算纸张"), tr("主区范围无效，无法计算。"));
		return;
	}

	const double drawW = groundW / scale * 1000.0; // 绘图宽(mm)
	const double drawH = groundH / scale * 1000.0; // 绘图高(mm)

	// 方向：宽 ≥ 高 → 横向
	const bool landscape = (drawW >= drawH);
	d->comboOrient->setCurrentText(landscape ? tr("横向") : tr("纵向"));

	// 先把 5 个标准纸张项文本恢复为纯名称，清掉上次"计算"写入的组合文本
	for (int i = 0; i < 5 && i < d->comboPaperSize->count(); ++i)
		d->comboPaperSize->setItemText(i, QLatin1String(kSeIsoPapers[i].name));

	// 选能容纳绘图尺寸的最小 ISO A 系列：从 A4（最小）往 A0（最大）找，
	// 第一个能容纳的即最小适配纸张（若从 A0 起找会永远命中 A0）
	const int paperCount = int(sizeof(kSeIsoPapers) / sizeof(kSeIsoPapers[0]));
	int chosen = -1;
	for (int i = paperCount - 1; i >= 0; --i)
	{
		double pw = landscape ? kSeIsoPapers[i].w : kSeIsoPapers[i].h; // 可用宽
		double ph = landscape ? kSeIsoPapers[i].h : kSeIsoPapers[i].w; // 可用高
		if (drawW <= pw + 1e-9 && drawH <= ph + 1e-9) { chosen = i; break; }
	}

	if (chosen < 0)
	{
		// 超过 A0：落"自定义"并填绘图尺寸
		d->comboPaperSize->setCurrentText(tr("自定义"));
		d->lineCustomW->setText(QString::number(drawW, 'f', 1));
		d->lineCustomH->setText(QString::number(drawH, 'f', 1));
		applyPaperSizeVisibility();
		QMessageBox::information(this, tr("计算纸张"),
			tr("主区绘图尺寸 %1 mm × %2 mm 超出 A0，已按自定义纸张设置。")
			.arg(QString::number(drawW, 'f', 1)).arg(QString::number(drawH, 'f', 1)));
		return;
	}

	// 纸张类型后追加绘图尺寸，如 "A0：893.0*654.0"
	const QString label = QString("%1：%2*%3")
		.arg(QLatin1String(kSeIsoPapers[chosen].name))
		.arg(QString::number(drawW, 'f', 1))
		.arg(QString::number(drawH, 'f', 1));
	d->comboPaperSize->setCurrentIndex(chosen);
	d->comboPaperSize->setItemText(chosen, label);
	applyPaperSizeVisibility();
}

void CSE_MainAreaExportDialog::on_Button_BrowseMainArea_clicked()
{
	QString f = QFileDialog::getOpenFileName(this, tr("选择主区矢量数据"), QString(),
		tr("矢量数据 (*.shp *.geojson *.json);;所有文件 (*.*)"));
	if (!f.isEmpty())
	{
		d->lineMainArea->setText(f);
		loadMainAreaFields();
	}
}

void CSE_MainAreaExportDialog::loadMainAreaFields()
{
	d->comboField->blockSignals(true);
	d->comboField->clear();
	d->listFeatures->clear();
	// 清掉旧缓存
	if (m_pMainAreaLayer)
	{
		delete m_pMainAreaLayer;
		m_pMainAreaLayer = nullptr;
	}
	QString path = d->lineMainArea->text();
	if (path.isEmpty() || !QFileInfo::exists(path))
	{
		d->comboField->blockSignals(false);
		return;
	}
	QgsVectorLayer* pLayer = new QgsVectorLayer(path, QFileInfo(path).completeBaseName(), "ogr");
	if (!pLayer || !pLayer->isValid())
	{
		if (pLayer) delete pLayer;
		d->comboField->blockSignals(false);
		return;
	}
	const QgsFields flds = pLayer->fields();
	for (int i = 0; i < flds.count(); ++i)
		d->comboField->addItem(flds[i].name(), flds[i].name());

	// 把 layer 缓存为成员，comboField 切换时重新填充列表，不用每次打开 shp
	m_pMainAreaLayer = pLayer;

	d->comboField->blockSignals(false);
	reloadFeatures();
}

void CSE_MainAreaExportDialog::reloadFeatures()
{
	d->listFeatures->clear();
	if (!m_pMainAreaLayer || !m_pMainAreaLayer->isValid())
		return;

	const QString fieldName = d->comboField->currentText();
	if (fieldName.isEmpty())
		return;

	// 在图层字段中查找当前 comboField 对应的索引
	int idxField = m_pMainAreaLayer->fields().lookupField(fieldName);
	if (idxField < 0)
		return;

	// 枚举全部要素，按当前字段构造显示文本：
	//   [序号] <fieldValue>   （首行）
	//   如果 fid 转 QString 后和上面不同，作为副标题（tooltip）
	QgsFeatureIterator it = m_pMainAreaLayer->getFeatures();
	QgsFeature feat;
	int dispIdx = 0;
	while (it.nextFeature(feat))
	{
		QString fid = QString::number(feat.id());
		QString name = feat.attribute(idxField).toString();
		QString label = tr("[%1] ").arg(dispIdx) + (name.isEmpty() ? fid : name);
		QListWidgetItem* item = new QListWidgetItem(label, d->listFeatures);
		item->setData(Qt::UserRole, fid);
		item->setToolTip(tr("fid=%1，标识字段=%2").arg(fid).arg(name));
		++dispIdx;
	}
}

void CSE_MainAreaExportDialog::on_Button_BrowseOutput_clicked()
{
	QString suggest = d->lineOutput->text();
	if (suggest.isEmpty())
	{
		QFileInfo fi(d_srcPath);
		suggest = fi.absolutePath() + "/" + fi.completeBaseName() + "_mainArea.shp";
	}
	QString f = QFileDialog::getSaveFileName(this, tr("选择输出文件"), suggest,
		tr("Shapefile (*.shp);;GeoPackage (*.gpkg);;GDB 文件夹 (*)"));
	if (!f.isEmpty()) d->lineOutput->setText(f);
}

void CSE_MainAreaExportDialog::on_Button_OK_clicked()
{
	if (d->lineMainArea->text().isEmpty())
	{
		QMessageBox::warning(this, tr("按主区裁切导出"), tr("请先选择主区数据。"));
		return;
	}
	if (d->listFeatures->selectedItems().isEmpty())
	{
		QMessageBox::warning(this, tr("按主区裁切导出"), tr("请至少选择一个要素作为主区。"));
		return;
	}
	if (d->lineOutput->text().isEmpty())
	{
		QMessageBox::warning(this, tr("按主区裁切导出"), tr("请设置输出文件路径。"));
		return;
	}
	accept();
}

void CSE_MainAreaExportDialog::on_Button_Cancel_clicked()
{
	reject();
}

QString    CSE_MainAreaExportDialog::mainAreaPath() const { return d->lineMainArea->text(); }
QString    CSE_MainAreaExportDialog::mainAreaField()const { return d->comboField->currentText(); }
QStringList CSE_MainAreaExportDialog::selectedFeatureIds() const
{
	QStringList ids;
	for (QListWidgetItem* it : d->listFeatures->selectedItems())
		ids << it->data(Qt::UserRole).toString();
	return ids;
}
QString CSE_MainAreaExportDialog::outputFilePath() const { return d->lineOutput->text(); }
QString CSE_MainAreaExportDialog::paperSize() const { return d->comboPaperSize->currentText(); }
QString CSE_MainAreaExportDialog::paperOrient() const { return d->comboOrient->currentText(); }
bool    CSE_MainAreaExportDialog::useCustomScale() const { return d->checkCustomScale->isChecked(); }

// 生效比例尺字符串：勾选"使用自定义"读自定义输入框，否则读标准下拉框（含手填值）
QString CSE_MainAreaExportDialog::standardScale() const
{
	if (useCustomScale())
		return d->lineCustomScale->text().trimmed();
	return d->comboStandardScale->currentText().trimmed();
}

double CSE_MainAreaExportDialog::mapScaleDenominator() const
{
	return standardScale().toDouble();
}

// 内图廓尺寸：仅当启用时返回输入值，否则返回 0
double CSE_MainAreaExportDialog::innerMapFrameWidthMm() const
{
	if (!enableInnerFrame()) return 0;
	return d->lineInnerW->text().toDouble();
}
double CSE_MainAreaExportDialog::innerMapFrameHeightMm() const
{
	if (!enableInnerFrame()) return 0;
	return d->lineInnerH->text().toDouble();
}
bool CSE_MainAreaExportDialog::enableInnerFrame() const { return d->checkEnableInner->isChecked(); }

// 是否在导出后自动加载结果到地图（默认 true）
bool CSE_MainAreaExportDialog::autoLoadAfterExport() const
{
	return d->chkAutoLoad && d->chkAutoLoad->isChecked();
}

// 从主区矢量中读取用户所选要素的合并外包矩形，作为统一裁剪范围
// mainAreaPath: 主区 shapefile 路径；fieldName: 标识字段；featureIds: 选中的要素 ID 字符串列表
QgsRectangle CSE_DataListExportDialog::computeMainAreaClipRect(const QString& mainAreaPath,
	const QString& /*fieldName*/, const QStringList& featureIds,
	QgsCoordinateReferenceSystem* outCrs) const
{
	QgsRectangle rect;
	if (mainAreaPath.isEmpty() || !QFileInfo::exists(mainAreaPath)) return rect;
	QgsVectorLayer* pLayer = new QgsVectorLayer(mainAreaPath,
		QFileInfo(mainAreaPath).completeBaseName(), "ogr");
	if (!pLayer || !pLayer->isValid())
	{
		if (pLayer) delete pLayer;
		return rect;
	}

	// 将字符串 ID 集合转换为 long long 集合用于 feature 请求
	QgsFeatureIds idSet;
	for (const QString& s : featureIds)
	{
		bool ok = false;
		long long v = s.toLongLong(&ok);
		if (ok) idSet.insert(v);
	}

	QgsFeatureRequest req;
	if (!idSet.isEmpty())
		req.setFilterFids(idSet);

	QgsFeatureIterator it = pLayer->getFeatures(req);
	QgsFeature feat;
	bool first = true;
	while (it.nextFeature(feat))
	{
		QgsGeometry g = feat.geometry();
		if (g.isNull()) continue;
		QgsRectangle b = g.boundingBox();
		if (b.isEmpty()) continue;
		if (first) { rect = b; first = false; }
		else rect.combineExtentWith(b);
	}
	// 输出矩形所在的主区 layer 原始 CRS，供导出时把裁剪范围正确转换到源数据 CRS
	if (outCrs && pLayer->crs().isValid())
		*outCrs = pLayer->crs();
	pLayer->deleteLater();
	return rect;
}
