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

/**
 * 只读取 UIName：没有名称时返回空字符串，不回退到内部 ID。
 * 用于调用方要自己决定回退顺序的场合（例如两段式超武先用前置超武的名称）。type 可为空。
 */
std::string ReadTypeUiName(const AbstractTypeClass* type);
}
