#ifndef QGIS_JSON_COMPAT_H
#define QGIS_JSON_COMPAT_H

// 仅 MSVC（Windows + QGIS 预编译库）需要。GCC/Linux（麒麟）上不存在 dllimport
// 本地虚表问题，若强行替换 json 前置声明反而会和麒麟侧 QGIS 头文件冲突，
// 所以非 MSVC 编译时本头文件展开为空，可以安全保留 include 语句。
#if defined(_MSC_VER)

// ============================================================
//  qgis_json_compat.h — QGIS 库与其 json_fwd.hpp 版本不一致的兼容垫片
//
//  现象：编译 se_mission458_check.cpp 时链接报
//        LNK2001 无法解析的外部符号 QgsGeometry::asJsonObject(int) const
//
//  原因：qgis_core.dll 是按 nlohmann/json 3.7（basic_json 有 9 个模板形参，
//        最后一个是 adl_serializer，没有 BinaryType）编译的；而本机
//        extern\QGIS_32815\include\json_fwd.hpp 是 3.8 以后的新版
//        （多一个 class BinaryType = std::vector<std::uint8_t>，共 10 个形参）。
//        QGIS 头文件里 json 就是 basic_json<>，两边形参个数不同 → 修饰名不同。
//
//  为什么只在 458 上出问题：MSVC 对 dllimport 类在本编译单元内构造对象时，
//        会生成“本地虚表”（??_S，dumpbin /symbols 可见），虚表里要填每一个
//        虚函数地址，其中就包括 QgsGeometry::asJsonObject。签名对不上的只有
//        这一个，于是成为唯一的未解析符号。已核对：全工程只有
//        se_mission458_check.obj 生成了这类本地虚表。
//
//  做法：抢在 qgsgeometry.h 之前把 json_fwd.hpp 的 include guard 定义掉，
//        再按 DLL 实际导出的 9 形参写法重新前置声明 nlohmann::json。
//        本文件由 se_mission458_check.cpp 在第一行包含，且该编译单元不引用
//        nlohmann 的完整 json.hpp，所以不会造成重复定义/形参冲突。
//
//  后续：若以后换成与 json_fwd.hpp 版本一致的 QGIS 库，删掉那两行引用即可。
// ============================================================

#define INCLUDE_NLOHMANN_JSON_FWD_HPP_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nlohmann
{
template<typename T = void, typename SFINAE = void>
struct adl_serializer;

template<template<typename U, typename V, typename... Args> class ObjectType = std::map,
         template<typename U, typename... Args> class ArrayType = std::vector,
         class StringType = std::string, class BooleanType = bool,
         class NumberIntegerType = std::int64_t,
         class NumberUnsignedType = std::uint64_t,
         class NumberFloatType = double,
         template<typename U> class AllocatorType = std::allocator,
         template<typename T, typename SFINAE = void> class JSONSerializer = adl_serializer>
class basic_json;

using json = basic_json<>;
}  // namespace nlohmann

#endif // _MSC_VER

#endif // QGIS_JSON_COMPAT_H
