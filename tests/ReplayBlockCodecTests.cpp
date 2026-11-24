#include "../src/storage/ReplayBlockCodec.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace rareplay::storage;

void Require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

void StoreU32(std::uint8_t* destination, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
        destination[index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::uint32_t LoadU32(const std::uint8_t* source)
{
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    return value;
}

std::vector<std::uint8_t> MakeHeader(std::uint32_t magic, std::uint32_t method, std::uint32_t rawBytes,
    std::uint32_t storedBytes, std::uint32_t crc)
{
    std::vector<std::uint8_t> header(block::HEADER_BYTES);
    StoreU32(header.data(), magic);
    StoreU32(header.data() + 4, method);
    StoreU32(header.data() + 8, rawBytes);
    StoreU32(header.data() + 12, storedBytes);
    StoreU32(header.data() + 16, crc);
    return header;
}

bool ParseOk(const std::vector<std::uint8_t>& header, std::uint32_t limit, block::Header& outHeader)
{
    std::string error;
    const bool parsed = block::ParseBlockHeader(header.data(), limit, outHeader, error);
    Require(parsed || !error.empty(), "a rejected header must explain why");
    return parsed;
}

bool ParseOk(const std::vector<std::uint8_t>& header, std::uint32_t limit = block::STREAM_RAW_LIMIT)
{
    block::Header ignored;
    return ParseOk(header, limit, ignored);
}

/** 解码一个完整的块（块头加载荷）：先按块头规则解析，再解码载荷。 */
bool DecodeWhole(const std::vector<std::uint8_t>& whole, std::vector<std::uint8_t>& outRaw,
    std::uint32_t limit = block::STREAM_RAW_LIMIT)
{
    Require(whole.size() >= block::HEADER_BYTES, "test block has a header");
    block::Header header;
    std::string error;
    if (!block::ParseBlockHeader(whole.data(), limit, header, error))
        return false;
    Require(whole.size() == block::HEADER_BYTES + header.mStoredBytes, "test block carries exactly its payload");
    const bool decoded = block::DecodeBlockPayload(header, whole.data() + block::HEADER_BYTES, outRaw, error);
    Require(decoded || !error.empty(), "a rejected payload must explain why");
    return decoded;
}

bool DecodeWhole(const std::vector<std::uint8_t>& whole)
{
    std::vector<std::uint8_t> ignored;
    return DecodeWhole(whole, ignored);
}

std::vector<std::uint8_t> Encode(const std::vector<std::uint8_t>& raw)
{
    std::vector<std::uint8_t> encoded;
    std::string error;
    Require(block::EncodeBlock(raw.data(), raw.size(), encoded, error) && error.empty(), "block encodes");
    return encoded;
}

std::vector<std::uint8_t> Compressible(std::size_t sizeBytes)
{
    std::vector<std::uint8_t> bytes(sizeBytes);
    for (std::size_t index = 0; index < sizeBytes; ++index)
        bytes[index] = static_cast<std::uint8_t>("RAReplay frame "[index % 15] + (index / 4096) % 3);
    return bytes;
}

std::vector<std::uint8_t> Incompressible(std::size_t sizeBytes)
{
    std::vector<std::uint8_t> bytes(sizeBytes);
    std::uint32_t state = 0x2545F491u;
    for (auto& byte : bytes)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<std::uint8_t>(state >> 24);
    }
    return bytes;
}

/** 按 RFC 1951 手工组装位流：普通字段低位在前，Huffman 码高位在前。 */
class BitWriter
{
public:
    void WriteBits(std::uint32_t value, unsigned int count)
    {
        for (unsigned int index = 0; index < count; ++index)
            PutBit((value >> index) & 1u);
    }
    void WriteCode(std::uint32_t code, unsigned int length)
    {
        for (unsigned int index = length; index > 0; --index)
            PutBit((code >> (index - 1)) & 1u);
    }
    std::size_t GetBitCount() const { return mBitCount; }
    const std::vector<std::uint8_t>& GetBytes() const { return mBytes; }

private:
    void PutBit(std::uint32_t bit)
    {
        if (mBitCount % 8 == 0)
            mBytes.push_back(0);
        if (bit != 0)
            mBytes.back() = static_cast<std::uint8_t>(mBytes.back() | (1u << (mBitCount % 8)));
        ++mBitCount;
    }

    std::vector<std::uint8_t> mBytes;
    std::size_t mBitCount = 0;
};

constexpr std::uint32_t HAND_RAW_BYTES = 259;

/**
 * 固定 Huffman 块：字面量 'A'，再接长度 258、距离 1 的匹配，共解出 259 个 'A'。
 * 块头 3 位 + 字面量 8 位 + 长度码 8 位 + 距离码 5 位正好 24 位，EOB（7 位）单独落在第 4 字节。
 */
std::vector<std::uint8_t> HandDeflate(bool isFinal, bool withEndOfBlock, std::uint32_t distanceCode = 0)
{
    BitWriter writer;
    writer.WriteBits(isFinal ? 1u : 0u, 1);
    writer.WriteBits(1, 2);                 // BTYPE=01 固定 Huffman
    writer.WriteCode(0x30 + 'A', 8);        // 字面量 0..143：00110000 + 值
    writer.WriteCode(0xC0 + (285 - 280), 8); // 长度符号 285 = 258，无额外位
    writer.WriteCode(distanceCode, 5);      // 距离码 0 = 距离 1，无额外位
    Require(writer.GetBitCount() == 24, "the match ends exactly on a byte boundary");
    if (withEndOfBlock)
        writer.WriteCode(0, 7);             // 符号 256（EOB）
    return writer.GetBytes();
}

std::vector<std::uint8_t> HandBlock(const std::vector<std::uint8_t>& payload, std::uint32_t rawBytes)
{
    const std::vector<std::uint8_t> raw(rawBytes, 'A');
    auto whole = MakeHeader(block::MAGIC, 1, rawBytes, static_cast<std::uint32_t>(payload.size()),
        block::Crc32(raw.data(), raw.size()));
    whole.insert(whole.end(), payload.begin(), payload.end());
    return whole;
}

void TestCrc32Vectors()
{
    const char* text = "123456789";
    Require(block::Crc32(text, std::strlen(text)) == 0xCBF43926u, "CRC-32 check value of \"123456789\"");
    Require(block::Crc32(nullptr, 0) == 0 && block::Crc32(text, 0) == 0, "CRC-32 of empty input is zero");
    const std::uint8_t zero = 0;
    Require(block::Crc32(&zero, 1) == 0xD202EF8Du, "CRC-32 of a single zero byte");
}

void TestRoundTrips()
{
    // 可压缩数据走 method 1，块头字段与载荷长度一致。
    for (const std::size_t size : {std::size_t{64}, std::size_t{4096}, std::size_t{64 * 1024}, std::size_t{300 * 1024}})
    {
        const auto raw = Compressible(size);
        const auto encoded = Encode(raw);
        Require(LoadU32(encoded.data()) == block::MAGIC && LoadU32(encoded.data() + 4) == 1 &&
            LoadU32(encoded.data() + 8) == size && LoadU32(encoded.data() + 12) == encoded.size() - block::HEADER_BYTES &&
            LoadU32(encoded.data() + 12) < size && LoadU32(encoded.data() + 16) == block::Crc32(raw.data(), raw.size()),
            "compressible data is stored as a deflate block");
        std::vector<std::uint8_t> decoded;
        Require(DecodeWhole(encoded, decoded) && decoded == raw, "deflate block round trips");
        Require(Encode(raw) == encoded, "encoding is deterministic");
    }
    // 不可压缩或很短的数据原样存放（method 0）。
    for (const auto& raw : {Incompressible(4096), Incompressible(1), std::vector<std::uint8_t>{0x41, 0x00, 0xfe, 0x42, 0x43}})
    {
        const auto encoded = Encode(raw);
        Require(LoadU32(encoded.data() + 4) == 0 && LoadU32(encoded.data() + 8) == raw.size() &&
            LoadU32(encoded.data() + 12) == raw.size() && encoded.size() == block::HEADER_BYTES + raw.size() &&
            std::equal(raw.begin(), raw.end(), encoded.begin() + block::HEADER_BYTES),
            "incompressible data is stored as is");
        std::vector<std::uint8_t> decoded;
        Require(DecodeWhole(encoded, decoded) && decoded == raw, "stored block round trips");
    }
    // 输出缓冲覆盖旧内容但可复用。
    std::vector<std::uint8_t> reused = Encode(Compressible(64 * 1024));
    std::string error;
    const auto small = Incompressible(10);
    Require(block::EncodeBlock(small.data(), small.size(), reused, error) &&
        reused.size() == block::HEADER_BYTES + small.size(), "the output buffer is overwritten");
}

void TestEncodeLimits()
{
    std::vector<std::uint8_t> encoded{1, 2, 3};
    std::string error;
    const std::uint8_t one = 1;
    Require(!block::EncodeBlock(&one, 0, encoded, error) && !error.empty() && encoded.empty(),
        "an empty block is refused");
    Require(!block::EncodeBlock(nullptr, 1, encoded, error), "a null input is refused");
    std::vector<std::uint8_t> limit(block::METADATA_RAW_LIMIT, 0);
    Require(block::EncodeBlock(limit.data(), limit.size(), encoded, error) && LoadU32(encoded.data() + 4) == 1 &&
        LoadU32(encoded.data() + 8) == block::METADATA_RAW_LIMIT, "exactly 16 MiB encodes");
    std::vector<std::uint8_t> decoded;
    Require(DecodeWhole(encoded, decoded, block::METADATA_RAW_LIMIT) && decoded == limit,
        "a 16 MiB block decodes under the metadata limit");
    Require(!DecodeWhole(encoded), "a 16 MiB block exceeds the frame stream block limit");
    limit.push_back(0);
    Require(!block::EncodeBlock(limit.data(), limit.size(), encoded, error) && encoded.empty(),
        "more than 16 MiB is refused");
}

void TestHeaderRules()
{
    const std::uint32_t limit = block::STREAM_RAW_LIMIT;
    block::Header header;
    Require(ParseOk(MakeHeader(block::MAGIC, 0, 10, 10, 7), limit, header) && header.mMethod == block::Method::Stored &&
        header.mRawBytes == 10 && header.mStoredBytes == 10 && header.mCrc32 == 7, "a stored header parses");
    Require(ParseOk(MakeHeader(block::MAGIC, 1, 10, 9, 7), limit, header) && header.mMethod == block::Method::Deflate &&
        header.mStoredBytes == 9, "a deflate header parses");
    Require(ParseOk(MakeHeader(block::MAGIC, 1, 10, 1, 0)), "a one-byte deflate payload is allowed");
    Require(ParseOk(MakeHeader(block::MAGIC, 0, limit, limit, 0)), "the raw size may equal the limit");

    Require(!ParseOk(MakeHeader(block::MAGIC + 1, 0, 10, 10, 0)), "magic is required");
    Require(!ParseOk(MakeHeader(0, 0, 10, 10, 0)), "a zero magic is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 2, 10, 9, 0)), "method 2 is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0xffffffffu, 10, 9, 0)), "unknown methods are invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0, 0, 0, 0)), "a zero raw size is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 1, 0, 0, 0)), "a zero raw size is invalid for deflate");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0, limit + 1, limit + 1, 0)), "the raw size is bounded");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0, 11, 11, 0), 10), "the caller's limit applies");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0, 10, 9, 0)), "stored payload shorter than raw is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 0, 10, 11, 0)), "stored payload longer than raw is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 1, 10, 10, 0)), "deflate payload equal to raw is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 1, 10, 11, 0)), "deflate payload longer than raw is invalid");
    Require(!ParseOk(MakeHeader(block::MAGIC, 1, 10, 0, 0)), "an empty deflate payload is invalid");
}

void TestPayloadRules()
{
    // 原样存放的 CRC 不符。
    auto stored = Encode(Incompressible(100));
    Require(DecodeWhole(stored), "control stored block decodes");
    stored[block::HEADER_BYTES + 50] ^= 1;
    Require(!DecodeWhole(stored), "a stored block with a CRC mismatch is invalid");

    // deflate 载荷的 CRC 不符：只改块头的 CRC 字段。
    auto deflated = Encode(Compressible(4096));
    Require(DecodeWhole(deflated), "control deflate block decodes");
    auto badCrc = deflated;
    badCrc[16] ^= 1;
    Require(!DecodeWhole(badCrc), "a deflate block with a CRC mismatch is invalid");

    // 载荷截断：块头的 storedBytes 随之缩短，输入在中途耗尽，解出的字节不足。
    auto truncated = deflated;
    const std::uint32_t half = (LoadU32(deflated.data() + 12)) / 2;
    StoreU32(truncated.data() + 12, half);
    truncated.resize(block::HEADER_BYTES + half);
    Require(!DecodeWhole(truncated), "a truncated deflate payload is invalid");

    // 损坏的 deflate 数据：保留块类型 11 是解码错误。
    BitWriter reserved;
    reserved.WriteBits(1, 1);
    reserved.WriteBits(3, 2);
    auto reservedBlock = HandBlock(reserved.GetBytes(), 10);
    Require(!DecodeWhole(reservedBlock), "a reserved deflate block type is invalid");

    // 手工固定 Huffman 块：完整形式有效。
    const auto full = HandDeflate(true, true);
    Require(full.size() == 4, "the hand-built stream is four bytes");
    std::vector<std::uint8_t> decoded;
    Require(DecodeWhole(HandBlock(full, HAND_RAW_BYTES), decoded) &&
        decoded == std::vector<std::uint8_t>(HAND_RAW_BYTES, 'A'), "the hand-built deflate block decodes");

    // 解码不足与解码多出：同一载荷配不同的 rawBytes（CRC 按块头所记长度的内容计算）。
    Require(!DecodeWhole(HandBlock(full, HAND_RAW_BYTES + 1)), "a payload decoding to fewer bytes than recorded is invalid");
    Require(!DecodeWhole(HandBlock(full, HAND_RAW_BYTES - 1)), "a payload decoding to more bytes than recorded is invalid");

    // 截掉 EOB 所在的末字节：rawBytes 与 CRC 仍相符，输入耗尽即结束，不要求流结束标记，必须有效。
    const auto withoutEnd = HandDeflate(true, false);
    Require(withoutEnd.size() == 3 && std::equal(withoutEnd.begin(), withoutEnd.end(), full.begin()),
        "dropping the last byte removes only the end-of-block code");
    Require(DecodeWhole(HandBlock(withoutEnd, HAND_RAW_BYTES), decoded) &&
        decoded == std::vector<std::uint8_t>(HAND_RAW_BYTES, 'A'), "a payload cut before its end-of-block code is valid");
    // 没有最终块标记（BFINAL=0），EOB 之后输入耗尽：同样有效。
    Require(DecodeWhole(HandBlock(HandDeflate(false, true), HAND_RAW_BYTES)), "a missing final-block flag is valid");
    // deflate 流结束之后载荷内的剩余字节不检查。
    auto withTrailing = full;
    withTrailing.insert(withTrailing.end(), {0xff, 0xff, 0x00});
    Require(DecodeWhole(HandBlock(withTrailing, HAND_RAW_BYTES)), "bytes after the deflate stream end are ignored");
    // 非最终块之后继续按下一块解码：第二块再解出 1 个 'A'，共 260 字节。
    BitWriter twoBlocks;
    twoBlocks.WriteBits(0, 1);
    twoBlocks.WriteBits(1, 2);
    twoBlocks.WriteCode(0x30 + 'A', 8);
    twoBlocks.WriteCode(0xC0 + (285 - 280), 8);
    twoBlocks.WriteCode(0, 5);
    twoBlocks.WriteCode(0, 7);
    twoBlocks.WriteBits(1, 1);
    twoBlocks.WriteBits(1, 2);
    twoBlocks.WriteCode(0x30 + 'A', 8);
    twoBlocks.WriteCode(0, 7);
    Require(DecodeWhole(HandBlock(twoBlocks.GetBytes(), HAND_RAW_BYTES + 1)), "decoding continues past a non-final block");
    Require(!DecodeWhole(HandBlock(twoBlocks.GetBytes(), HAND_RAW_BYTES)),
        "a later block producing byte rawBytes+1 makes the payload invalid");

    // 距离超出已输出长度：zlib 报错，插件判为无效（.NET Framework 的托管解码器对此宽松，见格式契约的已知差异）。
    Require(!DecodeWhole(HandBlock(HandDeflate(true, true, 1), HAND_RAW_BYTES)),
        "a distance reaching before the output start is invalid for the plugin");
}
}

int main()
{
    TestCrc32Vectors();
    TestRoundTrips();
    TestEncodeLimits();
    TestHeaderRules();
    TestPayloadRules();
}
