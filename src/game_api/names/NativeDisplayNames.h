#pragma once

#include <string>

class AbstractTypeClass;

namespace rareplay::game_api
{
/**
 * 读取原生类型的显示名称：优先 UIName，空名称时回退到有界内部 ID。
 * type 可为空（返回“未知类型”）；非空对象及其 UIName 由游戏拥有，
 * 调用期间须保持可读，只能在游戏采样上下文调用。
 */
std::string ReadTypeDisplayName(const AbstractTypeClass* type);
}
