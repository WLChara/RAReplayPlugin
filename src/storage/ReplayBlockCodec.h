#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rareplay::storage
{
/**
 * RAREPL2 容器 v3 的块编解码：20 字节块头后接载荷，
 * 载荷为原样存放或 RFC 1951 原始 deflate，块头记录解压后内容的 CRC-32。
 *
 * 纯逻辑：不打开文件，不保存状态，可在任意线程调用；输出缓冲由调用方持有并可跨调用复用容量。
 * 内存只用到 rawBytes 加 deflate 上界的缓冲与 zlib 自身的流状态。zlib 只出现在本模块的实现文件中。
 */
namespace block
{
constexpr std::size_t HEADER_BYTES = 20;
/** 块 magic，字节为 `R B L K`。 */
constexpr std::uint32_t MAGIC = 0x4B4C4252;
/** 清单段唯一一块的 rawBytes 上限（16 MiB）。 */
constexpr std::uint32_t METADATA_RAW_LIMIT = 16 * 1024 * 1024;
/**
 * 帧流段每块的 rawBytes 上限（4 MiB）。单条帧记录最长约 2.08 MiB，按写入端切块策略一块最多为
 * 64 KiB 减 1 再加一条最长记录，因此不会触及此上限。
 */
constexpr std::uint32_t STREAM_RAW_LIMIT = 4 * 1024 * 1024;

enum class Method : std::uint32_t
{
    Stored = 0,
    Deflate = 1
};

/** 已通过 ParseBlockHeader 的块头字段。 */
struct Header
{
    Method mMethod = Method::Stored;
    std::uint32_t mRawBytes = 0;
    std::uint32_t mStoredBytes = 0;
    std::uint32_t mCrc32 = 0;
};

/** zlib 的 CRC-32（IEEE 802.3，反射多项式，初值与终值异或 0xFFFFFFFF）；sizeBytes 为 0 时 data 可为 nullptr。 */
std::uint32_t Crc32(const void* data, std::size_t sizeBytes);

/**
 * 把 raw 编成一块（块头加载荷）写入 outBlock，覆盖其原有内容但保留容量。
 * 先以 deflate（6 级、windowBits −15、memLevel 8、默认策略）压缩，结果小于 rawBytes 时写 method 1，否则原样存放。
 *
 * @param raw rawBytes 个字节，不得为 nullptr。
 * @param rawBytes 1..METADATA_RAW_LIMIT；帧流块的 4 MiB 上限由调用方保证。
 * @return 失败（rawBytes 越界、zlib 报错或内存不足）时返回 false、outBlock 为空并写 error；不抛出异常。
 */
bool EncodeBlock(const std::uint8_t* raw, std::size_t rawBytes, std::vector<std::uint8_t>& outBlock,
    std::string& error);

/**
 * 只看 bytes 开头的 HEADER_BYTES 个字节，执行与载荷无关的全部块头规则：magic、method 只能为 0 或 1、
 * rawBytes 为 1..maxRawBytes、method 0 时 storedBytes 等于 rawBytes、method 1 时 1 ≤ storedBytes < rawBytes。
 *
 * @param bytes 至少 HEADER_BYTES 个字节，不得为 nullptr。
 * @return 无效时返回 false 并写 error，outHeader 不变。
 */
bool ParseBlockHeader(const std::uint8_t* bytes, std::uint32_t maxRawBytes, Header& outHeader,
    std::string& error);

/**
 * 按已解析的块头解码载荷并核对 CRC-32，结果覆盖 outRaw（保留容量）。
 * method 1 一直解码到输入耗尽或 deflate 流结束：出错、输出少于 rawBytes 或还能产生第 rawBytes+1 个字节均无效；
 * 不要求见到最终块标记，deflate 流结束后载荷内的剩余字节不检查（与 C# 读取端一致）。
 *
 * @param header 必须来自 ParseBlockHeader。
 * @param stored header.mStoredBytes 个字节，不得为 nullptr。
 * @return 无效或内存不足时返回 false、写 error，outRaw 内容未定义；不抛出异常。
 */
bool DecodeBlockPayload(const Header& header, const std::uint8_t* stored, std::vector<std::uint8_t>& outRaw,
    std::string& error);
}
}
