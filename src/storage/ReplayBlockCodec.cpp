#include "ReplayBlockCodec.h"

// 本机来源的 zlib.h 在 5 行注释中带有 GBK 编码的中文，/utf-8 下会报 C4828；
// 只是注释，按第三方原样保留，只在包含处屏蔽该警告。
#pragma warning(push)
#pragma warning(disable : 4828)
#include <zlib.h>
#pragma warning(pop)

#include <algorithm>
#include <cstring>
#include <new>

namespace rareplay::storage::block
{
namespace
{
// 写入端固定的 deflate 参数。tests/fixtures 中容器 v3 跨语言样例的字节与之绑定，改动会改变新录像的字节。
constexpr int DEFLATE_LEVEL = 6;
// 负的 windowBits 表示原始 deflate（不带 zlib/gzip 包装），窗口为 2^15 字节。
constexpr int RAW_DEFLATE_WINDOW_BITS = -15;
constexpr int DEFLATE_MEM_LEVEL = 8;

void StoreU32(std::uint8_t* destination, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
    {
        destination[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
}

std::uint32_t LoadU32(const std::uint8_t* source)
{
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
    {
        value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    }
    return value;
}

/** 构造成功后由析构调用 deflateEnd，保证任何返回路径都释放 zlib 流状态。 */
class DeflateStream
{
public:
    DeflateStream() = default;
    ~DeflateStream()
    {
        if (mIsOpen)
        {
            deflateEnd(&mStream);
        }
    }
    DeflateStream(const DeflateStream&) = delete;
    DeflateStream& operator=(const DeflateStream&) = delete;

    bool Open()
    {
        mIsOpen = deflateInit2(&mStream, DEFLATE_LEVEL, Z_DEFLATED, RAW_DEFLATE_WINDOW_BITS, DEFLATE_MEM_LEVEL,
            Z_DEFAULT_STRATEGY) == Z_OK;
        return mIsOpen;
    }

    z_stream mStream{};

private:
    bool mIsOpen = false;
};

class InflateStream
{
public:
    InflateStream() = default;
    ~InflateStream()
    {
        if (mIsOpen)
        {
            inflateEnd(&mStream);
        }
    }
    InflateStream(const InflateStream&) = delete;
    InflateStream& operator=(const InflateStream&) = delete;

    bool Open()
    {
        mIsOpen = inflateInit2(&mStream, RAW_DEFLATE_WINDOW_BITS) == Z_OK;
        return mIsOpen;
    }

    z_stream mStream{};

private:
    bool mIsOpen = false;
};

void WriteHeader(std::uint8_t* destination, Method method, std::uint32_t rawBytes, std::uint32_t storedBytes,
    std::uint32_t crc)
{
    StoreU32(destination, MAGIC);
    StoreU32(destination + 4, static_cast<std::uint32_t>(method));
    StoreU32(destination + 8, rawBytes);
    StoreU32(destination + 12, storedBytes);
    StoreU32(destination + 16, crc);
}

/**
 * 解码 method 1 的载荷到 outRaw（多留 1 字节容量），返回实际产生的字节数；出错返回 false。
 * 产生的字节数可能是 0..rawBytes+1，由调用方判定是否恰好等于 rawBytes。
 */
bool InflatePayload(const Header& header, const std::uint8_t* stored, std::vector<std::uint8_t>& outRaw,
    std::size_t& outProducedBytes, std::string& error)
{
    // 多留 1 字节：只要还能产生第 rawBytes+1 个字节，就说明载荷解出的内容多于块头所记。
    const std::size_t capacity = static_cast<std::size_t>(header.mRawBytes) + 1;
    outRaw.resize(capacity);
    InflateStream stream;
    if (!stream.Open())
    {
        error = "inflateInit2 failed";
        return false;
    }
    // zlib 未定义 ZLIB_CONST 时 next_in 不带 const；inflate 只读取输入，不会写回。
    stream.mStream.next_in = const_cast<Bytef*>(stored);
    stream.mStream.avail_in = header.mStoredBytes;
    stream.mStream.next_out = outRaw.data();
    stream.mStream.avail_out = static_cast<uInt>(capacity);
    int result = Z_OK;
    // Z_NO_FLUSH 下 inflate 只在有进展时返回 Z_OK，因此循环必然结束：
    // Z_STREAM_END 为 deflate 流结束（其后剩余输入不检查），Z_BUF_ERROR 为输入耗尽（不要求最终块标记），
    // 输出缓冲用满即已产生第 rawBytes+1 个字节。
    do
    {
        result = inflate(&stream.mStream, Z_NO_FLUSH);
    } while (result == Z_OK && stream.mStream.avail_out != 0);
    if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
    {
        error = "Block deflate payload is invalid";
        return false;
    }
    outProducedBytes = capacity - stream.mStream.avail_out;
    return true;
}
}

std::uint32_t Crc32(const void* data, std::size_t sizeBytes)
{
    if (sizeBytes == 0)
    {
        return 0;
    }
    return static_cast<std::uint32_t>(crc32_z(0, static_cast<const Bytef*>(data), sizeBytes));
}

bool EncodeBlock(const std::uint8_t* raw, std::size_t rawBytes, std::vector<std::uint8_t>& outBlock,
    std::string& error)
{
    outBlock.clear();
    if (raw == nullptr || rawBytes == 0 || rawBytes > METADATA_RAW_LIMIT)
    {
        error = "Block raw size must be 1..16 MiB";
        return false;
    }
    try
    {
        DeflateStream stream;
        if (!stream.Open())
        {
            error = "deflateInit2 failed";
            return false;
        }
        // 原样存放时载荷就是 rawBytes，缓冲取两者中较大者，压缩与存放都不再分配。
        const std::size_t bound = (std::max)(static_cast<std::size_t>(
            deflateBound(&stream.mStream, static_cast<uLong>(rawBytes))), rawBytes);
        outBlock.resize(HEADER_BYTES + bound);
        stream.mStream.next_in = const_cast<Bytef*>(raw);
        stream.mStream.avail_in = static_cast<uInt>(rawBytes);
        stream.mStream.next_out = outBlock.data() + HEADER_BYTES;
        stream.mStream.avail_out = static_cast<uInt>(bound);
        // 输出空间不小于 deflateBound，一次 Z_FINISH 即可结束。
        if (deflate(&stream.mStream, Z_FINISH) != Z_STREAM_END)
        {
            outBlock.clear();
            error = "deflate failed";
            return false;
        }
        const std::size_t compressedBytes = bound - stream.mStream.avail_out;
        const std::uint32_t crc = Crc32(raw, rawBytes);
        const auto rawSize = static_cast<std::uint32_t>(rawBytes);
        if (compressedBytes < rawBytes)
        {
            outBlock.resize(HEADER_BYTES + compressedBytes);
            WriteHeader(outBlock.data(), Method::Deflate, rawSize, static_cast<std::uint32_t>(compressedBytes), crc);
        }
        else
        {
            std::memcpy(outBlock.data() + HEADER_BYTES, raw, rawBytes);
            outBlock.resize(HEADER_BYTES + rawBytes);
            WriteHeader(outBlock.data(), Method::Stored, rawSize, rawSize, crc);
        }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        outBlock.clear();
        error = "Out of memory while encoding a block";
        return false;
    }
}

bool ParseBlockHeader(const std::uint8_t* bytes, std::uint32_t maxRawBytes, Header& outHeader,
    std::string& error)
{
    const std::uint32_t magic = LoadU32(bytes);
    const std::uint32_t method = LoadU32(bytes + 4);
    const std::uint32_t rawBytes = LoadU32(bytes + 8);
    const std::uint32_t storedBytes = LoadU32(bytes + 12);
    if (magic != MAGIC)
    {
        error = "Block magic is invalid";
        return false;
    }
    if (method != static_cast<std::uint32_t>(Method::Stored) && method != static_cast<std::uint32_t>(Method::Deflate))
    {
        error = "Block method is unsupported";
        return false;
    }
    if (rawBytes == 0 || rawBytes > maxRawBytes)
    {
        error = "Block raw size is outside the limit";
        return false;
    }
    if (method == static_cast<std::uint32_t>(Method::Stored) && storedBytes != rawBytes)
    {
        error = "Stored block size must equal its raw size";
        return false;
    }
    if (method == static_cast<std::uint32_t>(Method::Deflate) && (storedBytes == 0 || storedBytes >= rawBytes))
    {
        error = "Deflate block payload must be shorter than its raw size";
        return false;
    }
    outHeader.mMethod = static_cast<Method>(method);
    outHeader.mRawBytes = rawBytes;
    outHeader.mStoredBytes = storedBytes;
    outHeader.mCrc32 = LoadU32(bytes + 16);
    return true;
}

bool DecodeBlockPayload(const Header& header, const std::uint8_t* stored, std::vector<std::uint8_t>& outRaw,
    std::string& error)
{
    try
    {
        if (header.mMethod == Method::Stored)
        {
            outRaw.assign(stored, stored + header.mRawBytes);
        }
        else
        {
            std::size_t producedBytes = 0;
            if (!InflatePayload(header, stored, outRaw, producedBytes, error))
            {
                return false;
            }
            if (producedBytes != header.mRawBytes)
            {
                error = producedBytes < header.mRawBytes ? "Block deflate payload decodes to fewer bytes than recorded" :
                    "Block deflate payload decodes to more bytes than recorded";
                return false;
            }
            outRaw.resize(header.mRawBytes);
        }
    }
    catch (const std::bad_alloc&)
    {
        error = "Out of memory while decoding a block";
        return false;
    }
    if (Crc32(outRaw.data(), outRaw.size()) != header.mCrc32)
    {
        error = "Block CRC-32 mismatch";
        return false;
    }
    return true;
}
}
