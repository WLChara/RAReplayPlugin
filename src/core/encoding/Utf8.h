#pragma once

#include <cstddef>
#include <string>

namespace rareplay::core::encoding
{
/**
 * 将有界 UTF-16 前缀转换为 UTF-8，不包含第一个 NUL 及其后的内容。
 *
 * @param text 可为空；非空时，调用方保证直到首个 NUL（含）或 maxLength 个
 * wchar_t 代码单元可读，取先到达的边界；输入仅在本次调用期间借用。
 * @param maxLength 扫描上限，单位为 wchar_t 代码单元，不是字符数或字节数。
 * @return 空输入、零上限、前缀为空、长度超出转换范围或转换失败时返回空字符串。
 * 孤立代理代码单元沿用 Windows 显示转换的替换行为，不进行严格合法性拒绝。
 */
std::string Utf8(const wchar_t* text, std::size_t maxLength);
}
