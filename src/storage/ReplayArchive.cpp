#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ReplayArchive.h"
#include "ReplayBlockCodec.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <streambuf>
#include <utility>

namespace rareplay::storage
{
namespace
{
constexpr std::size_t HEADER_BYTES = 96;
constexpr std::size_t METADATA_LIMIT = 16 * 1024 * 1024;
constexpr std::uint64_t STREAM_LIMIT = 1024ULL * 1024ULL * 1024ULL;
// v2：metadata 与帧流按原样存放；v3：清单与帧流按块压缩。读取端两者都接受，写入端只写 v3。
constexpr std::uint32_t ARCHIVE_VERSION_PLAIN = 2;
constexpr std::uint32_t ARCHIVE_VERSION_BLOCKS = 3;
constexpr std::uint32_t FLAG_FINALIZED = 1;
constexpr std::uint32_t FLAG_UNFINALIZED = 0;
// 容器层只要求帧流头（RPF2，32 字节）完整；其内容由帧流解码层校验。
constexpr std::uint64_t STREAM_HEADER_BYTES = 32;
// v3 的清单段与完成文件的帧流段至少是一个块头加 1 字节载荷。
constexpr std::uint64_t MIN_BLOCK_SEGMENT_BYTES = block::HEADER_BYTES + 1;
constexpr std::uint64_t HASH_BUFFER_BYTES = 64 * 1024;
constexpr std::uint8_t ARCHIVE_MAGIC[8] = {'R', 'A', 'R', 'E', 'P', 'L', '2', 0};

void SetError(std::string& error, const char* message)
{
    error = message;
}

void SetWindowsError(std::string& error, const char* operation, DWORD errorCode)
{
    error = operation;
    error += " failed (Win32 error ";
    error += std::to_string(errorCode);
    error += ")";
}

bool WriteAll(HANDLE file, const void* data, std::size_t sizeBytes, std::string& error)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t writtenTotal = 0;
    while (writtenTotal < sizeBytes)
    {
        const DWORD chunkSize = static_cast<DWORD>(std::min<std::size_t>(sizeBytes - writtenTotal, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(file, bytes + writtenTotal, chunkSize, &written, nullptr) || written == 0)
        {
            SetWindowsError(error, "WriteFile", GetLastError());
            return false;
        }
        writtenTotal += written;
    }
    return true;
}

bool ReadExact(HANDLE file, void* data, std::size_t sizeBytes, std::string& error)
{
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t readTotal = 0;
    while (readTotal < sizeBytes)
    {
        const DWORD chunkSize = static_cast<DWORD>(std::min<std::size_t>(sizeBytes - readTotal, MAXDWORD));
        DWORD bytesRead = 0;
        if (!ReadFile(file, bytes + readTotal, chunkSize, &bytesRead, nullptr) || bytesRead == 0)
        {
            SetWindowsError(error, "ReadFile", GetLastError());
            return false;
        }
        readTotal += bytesRead;
    }
    return true;
}

bool SeekFile(HANDLE file, std::uint64_t position, std::string& error)
{
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(position);
    if (!SetFilePointerEx(file, target, nullptr, FILE_BEGIN))
    {
        SetWindowsError(error, "SetFilePointerEx", GetLastError());
        return false;
    }
    return true;
}

bool ReadAt(HANDLE file, std::uint64_t position, void* data, std::size_t sizeBytes, std::string& error)
{
    return SeekFile(file, position, error) && ReadExact(file, data, sizeBytes, error);
}

void StoreU32(std::uint8_t* destination, std::uint32_t value)
{
    for (unsigned int index = 0; index < 4; ++index)
    {
        destination[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
}

void StoreU64(std::uint8_t* destination, std::uint64_t value)
{
    for (unsigned int index = 0; index < 8; ++index)
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

std::uint64_t LoadU64(const std::uint8_t* source)
{
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index)
    {
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8);
    }
    return value;
}

std::array<std::uint8_t, HEADER_BYTES> EncodeHeader(std::uint32_t metadataBytes, std::uint32_t flags,
    std::uint64_t streamBytes, const std::array<std::uint8_t, 32>& targetHash,
    const std::array<std::uint8_t, 32>& payloadHash)
{
    std::array<std::uint8_t, HEADER_BYTES> header{};
    std::memcpy(header.data(), ARCHIVE_MAGIC, sizeof(ARCHIVE_MAGIC));
    StoreU32(header.data() + 8, ARCHIVE_VERSION_BLOCKS);
    StoreU32(header.data() + 12, static_cast<std::uint32_t>(HEADER_BYTES));
    StoreU32(header.data() + 16, metadataBytes);
    StoreU32(header.data() + 20, flags);
    StoreU64(header.data() + 24, streamBytes);
    std::memcpy(header.data() + 32, targetHash.data(), targetHash.size());
    std::memcpy(header.data() + 64, payloadHash.data(), payloadHash.size());
    return header;
}

class Sha256
{
public:
    ~Sha256() { Close(); }

    bool Open(std::string& error)
    {
        NTSTATUS status = BCryptOpenAlgorithmProvider(&mAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status < 0)
        {
            error = "BCryptOpenAlgorithmProvider failed";
            return false;
        }

        DWORD objectBytes = 0;
        DWORD returnedBytes = 0;
        status = BCryptGetProperty(mAlgorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returnedBytes, 0);
        if (status < 0 || objectBytes == 0)
        {
            error = "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
            Close();
            return false;
        }
        mObject.resize(objectBytes);

        DWORD hashBytes = 0;
        status = BCryptGetProperty(mAlgorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes), &returnedBytes, 0);
        if (status < 0 || hashBytes != 32)
        {
            error = "BCryptGetProperty(BCRYPT_HASH_LENGTH) failed";
            Close();
            return false;
        }
        status = BCryptCreateHash(mAlgorithm, &mHash, mObject.data(), objectBytes, nullptr, 0, 0);
        if (status < 0)
        {
            error = "BCryptCreateHash failed";
            Close();
            return false;
        }
        return true;
    }

    bool Update(const void* data, std::size_t sizeBytes, std::string& error)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        while (sizeBytes != 0)
        {
            const ULONG chunkBytes = static_cast<ULONG>((std::min<std::size_t>)(sizeBytes, ((std::numeric_limits<ULONG>::max)())));
            if (BCryptHashData(mHash, const_cast<PUCHAR>(bytes), chunkBytes, 0) < 0)
            {
                error = "BCryptHashData failed";
                return false;
            }
            bytes += chunkBytes;
            sizeBytes -= chunkBytes;
        }
        return true;
    }

    bool Finish(std::array<std::uint8_t, 32>& outHash, std::string& error)
    {
        if (BCryptFinishHash(mHash, outHash.data(), static_cast<ULONG>(outHash.size()), 0) < 0)
        {
            error = "BCryptFinishHash failed";
            return false;
        }
        Close();
        return true;
    }

    void Close()
    {
        if (mHash != nullptr)
        {
            BCryptDestroyHash(mHash);
            mHash = nullptr;
        }
        if (mAlgorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(mAlgorithm, 0);
            mAlgorithm = nullptr;
        }
        mObject.clear();
    }

private:
    BCRYPT_ALG_HANDLE mAlgorithm = nullptr;
    BCRYPT_HASH_HANDLE mHash = nullptr;
    std::vector<std::uint8_t> mObject;
};

/**
 * 逻辑帧流的读取缓冲：位置与长度都相对于逻辑帧流。数据由派生类在 underflow 中加载；
 * 加载失败时记下第一条错误并按 EOF 返回，此后保持失败，由 ArchiveReader::GetStreamError 报告。
 */
class LogicalStreamBuffer : public std::streambuf
{
public:
    std::uint64_t GetPosition() const
    {
        return mAreaStart + static_cast<std::uint64_t>(gptr() - eback());
    }

    /** 只移动读取位置，下一次读取时才加载数据；超过逻辑长度时失败。 */
    bool SetPosition(std::uint64_t position)
    {
        if (position > mLogicalSize)
        {
            return false;
        }
        SetEmptyArea(position);
        return true;
    }

    std::uint64_t GetLogicalSize() const { return mLogicalSize; }
    const std::string& GetError() const { return mError; }

protected:
    explicit LogicalStreamBuffer(std::uint64_t logicalSize) : mLogicalSize(logicalSize)
    {
        SetEmptyArea(0);
    }

    void SetEmptyArea(std::uint64_t position)
    {
        mAreaStart = position;
        setg(nullptr, nullptr, nullptr);
    }

    /** begin 对应逻辑偏移 areaStart，读取位置从 current 开始。 */
    void SetArea(std::uint64_t areaStart, char* begin, char* current, char* end)
    {
        mAreaStart = areaStart;
        setg(begin, current, end);
    }

    void Fail(const std::string& error) noexcept
    {
        if (mError.empty())
        {
            try
            {
                mError = error.empty() ? std::string("Archive frame stream read failed") : error;
            }
            catch (...)
            {
            }
        }
    }

    bool HasFailed() const { return !mError.empty(); }

    std::streamsize xsgetn(char* destination, std::streamsize requested) override
    {
        if (requested <= 0)
        {
            return 0;
        }
        std::streamsize copied = 0;
        while (copied < requested)
        {
            if (gptr() == egptr() && traits_type::eq_int_type(underflow(), traits_type::eof()))
            {
                break;
            }
            const std::streamsize available = static_cast<std::streamsize>(egptr() - gptr());
            const std::streamsize count = (std::min)(available, requested - copied);
            std::memcpy(destination + copied, gptr(), static_cast<std::size_t>(count));
            gbump(static_cast<int>(count));
            copied += count;
        }
        return copied;
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode mode) override
    {
        if ((mode & std::ios_base::in) == 0)
        {
            return pos_type(off_type(-1));
        }
        std::int64_t base = 0;
        if (direction == std::ios_base::cur)
        {
            base = static_cast<std::int64_t>(GetPosition());
        }
        else if (direction == std::ios_base::end)
        {
            base = static_cast<std::int64_t>(mLogicalSize);
        }
        else if (direction != std::ios_base::beg)
        {
            return pos_type(off_type(-1));
        }
        if ((offset < 0 && offset < -base) || (offset > 0 && base > (std::numeric_limits<std::int64_t>::max)() - offset))
        {
            return pos_type(off_type(-1));
        }
        const std::int64_t target = base + offset;
        if (target < 0 || !SetPosition(static_cast<std::uint64_t>(target)))
        {
            return pos_type(off_type(-1));
        }
        return pos_type(static_cast<off_type>(target));
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }

    std::uint64_t mLogicalSize;

private:
    std::uint64_t mAreaStart = 0;
    std::string mError;
};

/** 容器 v2：逻辑帧流就是文件中 payloadStart 起的 payloadSize 字节。 */
class BoundedFileStreamBuffer final : public LogicalStreamBuffer
{
public:
    BoundedFileStreamBuffer(HANDLE file, std::uint64_t payloadStart, std::uint64_t payloadSize)
        : LogicalStreamBuffer(payloadSize), mFile(file), mPayloadStart(payloadStart)
    {
    }

protected:
    int_type underflow() override
    {
        const std::uint64_t position = GetPosition();
        if (position >= mLogicalSize || HasFailed())
        {
            return traits_type::eof();
        }
        const DWORD readCapacity = static_cast<DWORD>(std::min<std::uint64_t>(mBuffer.size(), mLogicalSize - position));
        std::string error;
        if (!SeekFile(mFile, mPayloadStart + position, error))
        {
            Fail(error);
            return traits_type::eof();
        }
        DWORD bytesRead = 0;
        if (!ReadFile(mFile, mBuffer.data(), readCapacity, &bytesRead, nullptr))
        {
            SetWindowsError(error, "ReadFile", GetLastError());
            Fail(error);
            return traits_type::eof();
        }
        // 位置仍在已确认的帧流长度之内却读不到数据，说明文件在打开后变短；这是读取失败，不是正常 EOF。
        if (bytesRead == 0)
        {
            Fail("Archive file ended before the frame stream boundary");
            return traits_type::eof();
        }
        SetArea(position, mBuffer.data(), mBuffer.data(), mBuffer.data() + bytesRead);
        return traits_type::to_int_type(*gptr());
    }

private:
    HANDLE mFile;
    std::uint64_t mPayloadStart;
    std::array<char, HASH_BUFFER_BYTES> mBuffer{};
};

/** 容器 v3 帧流段中一块的位置；索引按逻辑偏移升序，相邻块首尾相接。 */
struct BlockIndexEntry
{
    std::uint64_t mFileOffset = 0;
    std::uint64_t mLogicalOffset = 0;
    std::uint32_t mRawBytes = 0;
    std::uint32_t mStoredBytes = 0;
};

/** 容器 v3：按块索引逐块按需读取并解码，内存中只保留当前块的磁盘字节与解码结果。 */
class BlockStreamBuffer final : public LogicalStreamBuffer
{
public:
    BlockStreamBuffer(HANDLE file, std::vector<BlockIndexEntry> index, std::uint64_t logicalSize)
        : LogicalStreamBuffer(logicalSize), mFile(file), mIndex(std::move(index))
    {
    }

protected:
    int_type underflow() override
    {
        const std::uint64_t position = GetPosition();
        if (position >= mLogicalSize || HasFailed())
        {
            return traits_type::eof();
        }
        const std::size_t blockIndex = FindBlock(position);
        if (blockIndex != mLoadedBlock && !LoadBlock(blockIndex))
        {
            SetEmptyArea(position);
            return traits_type::eof();
        }
        const BlockIndexEntry& entry = mIndex[blockIndex];
        char* begin = reinterpret_cast<char*>(mRaw.data());
        SetArea(entry.mLogicalOffset, begin, begin + (position - entry.mLogicalOffset), begin + entry.mRawBytes);
        return traits_type::to_int_type(*gptr());
    }

private:
    static constexpr std::size_t NO_BLOCK = (std::numeric_limits<std::size_t>::max)();

    /** position 必须小于逻辑长度；索引覆盖 [0, 逻辑长度)。 */
    std::size_t FindBlock(std::uint64_t position) const
    {
        const auto next = std::upper_bound(mIndex.begin(), mIndex.end(), position,
            [](std::uint64_t value, const BlockIndexEntry& entry) { return value < entry.mLogicalOffset; });
        return static_cast<std::size_t>(next - mIndex.begin()) - 1;
    }

    bool LoadBlock(std::size_t blockIndex)
    {
        mLoadedBlock = NO_BLOCK;
        const BlockIndexEntry& entry = mIndex[blockIndex];
        std::string error;
        try
        {
            mStored.resize(block::HEADER_BYTES + entry.mStoredBytes);
        }
        catch (const std::bad_alloc&)
        {
            Fail("Out of memory while reading an archive block");
            return false;
        }
        if (!ReadAt(mFile, entry.mFileOffset, mStored.data(), mStored.size(), error))
        {
            Fail(error);
            return false;
        }
        block::Header header;
        // 块头在打开时已核对；再次核对可以发现打开之后文件被改动的情况。
        if (!block::ParseBlockHeader(mStored.data(), block::STREAM_RAW_LIMIT, header, error) ||
            header.mRawBytes != entry.mRawBytes || header.mStoredBytes != entry.mStoredBytes ||
            !block::DecodeBlockPayload(header, mStored.data() + block::HEADER_BYTES, mRaw, error))
        {
            Fail("Archive frame stream block " + std::to_string(blockIndex) + " is invalid: " +
                (error.empty() ? std::string("block header changed after open") : error));
            return false;
        }
        mLoadedBlock = blockIndex;
        return true;
    }

    HANDLE mFile;
    std::vector<BlockIndexEntry> mIndex;
    std::vector<std::uint8_t> mStored;
    std::vector<std::uint8_t> mRaw;
    std::size_t mLoadedBlock = NO_BLOCK;
};

bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/** 容器头 flags 的接受规则；ByFlags 由 flags 的值在 1 与 0 两种规则之间选择。 */
enum class OpenMode
{
    Finalized,
    Unfinalized,
    ByFlags
};

/** 打开期间独占文件句柄；成功后用 Release 转交给 ArchiveReader::Impl。 */
class FileHandleGuard
{
public:
    explicit FileHandleGuard(HANDLE file) : mFile(file) {}
    ~FileHandleGuard()
    {
        if (mFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mFile);
        }
    }
    FileHandleGuard(const FileHandleGuard&) = delete;
    FileHandleGuard& operator=(const FileHandleGuard&) = delete;

    HANDLE Get() const { return mFile; }
    HANDLE Release()
    {
        const HANDLE file = mFile;
        mFile = INVALID_HANDLE_VALUE;
        return file;
    }

private:
    HANDLE mFile;
};

/** 按打开方式把 flags 归类；返回 false 表示该方式不接受此 flags。 */
bool ClassifyFlags(OpenMode mode, std::uint32_t flags, bool& outIsFinalized, std::string& error)
{
    if (flags == FLAG_FINALIZED && mode != OpenMode::Unfinalized)
    {
        outIsFinalized = true;
        return true;
    }
    if (flags == FLAG_UNFINALIZED && mode != OpenMode::Finalized)
    {
        outIsFinalized = false;
        return true;
    }
    if (mode == OpenMode::Finalized)
    {
        SetError(error, "Archive is not finalized or contains unsupported flags");
    }
    else if (flags == FLAG_FINALIZED)
    {
        SetError(error, "Archive is finalized; it is not an unfinalized recording");
    }
    else
    {
        SetError(error, "Archive contains unsupported flags");
    }
    return false;
}

/** 以头中记录的长度流式计算 metadata 段与帧流段磁盘字节的 SHA-256，并与 expectedHash（32 字节）比较。 */
bool VerifyPayloadHash(HANDLE file, std::uint32_t metadataBytes, std::uint64_t streamBytes,
    const std::uint8_t* expectedHash, std::string& error)
{
    Sha256 hash;
    if (!hash.Open(error) || !SeekFile(file, HEADER_BYTES, error))
    {
        return false;
    }
    std::array<std::uint8_t, HASH_BUFFER_BYTES> buffer{};
    std::uint64_t remaining = static_cast<std::uint64_t>(metadataBytes) + streamBytes;
    while (remaining != 0)
    {
        const DWORD chunkBytes = static_cast<DWORD>((std::min<std::uint64_t>)(buffer.size(), remaining));
        if (!ReadExact(file, buffer.data(), chunkBytes, error) || !hash.Update(buffer.data(), chunkBytes, error))
        {
            return false;
        }
        remaining -= chunkBytes;
    }
    std::array<std::uint8_t, 32> actualPayloadHash{};
    if (!hash.Finish(actualPayloadHash, error))
    {
        return false;
    }
    if (std::memcmp(actualPayloadHash.data(), expectedHash, actualPayloadHash.size()) != 0)
    {
        SetError(error, "Archive payload SHA-256 mismatch");
        return false;
    }
    return true;
}

/** v3 清单段必须恰好是一个有效块（块头加载荷正好占满 metadataBytes），解出的内容即开局清单。 */
bool ReadMetadataBlock(HANDLE file, std::uint32_t metadataBytes, std::vector<std::uint8_t>& outMetadata,
    std::string& error)
{
    std::vector<std::uint8_t> segment;
    try
    {
        segment.resize(metadataBytes);
    }
    catch (const std::bad_alloc&)
    {
        SetError(error, "Out of memory while reading the archive metadata block");
        return false;
    }
    if (!ReadAt(file, HEADER_BYTES, segment.data(), segment.size(), error))
    {
        return false;
    }
    block::Header header;
    std::string blockError;
    if (!block::ParseBlockHeader(segment.data(), block::METADATA_RAW_LIMIT, header, blockError) ||
        block::HEADER_BYTES + static_cast<std::uint64_t>(header.mStoredBytes) != metadataBytes ||
        !block::DecodeBlockPayload(header, segment.data() + block::HEADER_BYTES, outMetadata, blockError))
    {
        error = "Archive metadata block is invalid";
        if (!blockError.empty())
        {
            error += ": " + blockError;
        }
        return false;
    }
    return true;
}

/**
 * 完成文件：从帧流段首起逐个核对块头（不解码载荷），要求各块完整、首尾相接、最后一块正好结束在 streamBytes，
 * 逻辑总长为 32..1 GiB。载荷的解码与 CRC 在播放读取时进行；SHA-256 已保证磁盘字节与写入时相同。
 */
bool IndexFinalizedBlocks(HANDLE file, std::uint64_t segmentStart, std::uint64_t segmentBytes,
    std::vector<BlockIndexEntry>& outIndex, std::uint64_t& outLogicalBytes, std::string& error)
{
    outIndex.clear();
    std::uint64_t offset = 0;
    std::uint64_t logicalBytes = 0;
    while (offset < segmentBytes)
    {
        if (segmentBytes - offset < block::HEADER_BYTES)
        {
            SetError(error, "Archive frame stream segment ends inside a block header");
            return false;
        }
        std::array<std::uint8_t, block::HEADER_BYTES> headerBytes{};
        if (!ReadAt(file, segmentStart + offset, headerBytes.data(), headerBytes.size(), error))
        {
            return false;
        }
        block::Header header;
        std::string blockError;
        if (!block::ParseBlockHeader(headerBytes.data(), block::STREAM_RAW_LIMIT, header, blockError))
        {
            error = "Archive frame stream block " + std::to_string(outIndex.size()) + " header is invalid: " +
                blockError;
            return false;
        }
        if (header.mStoredBytes > segmentBytes - offset - block::HEADER_BYTES)
        {
            SetError(error, "Archive frame stream block extends past the stream segment");
            return false;
        }
        logicalBytes += header.mRawBytes;
        if (logicalBytes > STREAM_LIMIT)
        {
            SetError(error, "Archive logical frame stream exceeds the format limit");
            return false;
        }
        try
        {
            outIndex.push_back({segmentStart + offset, logicalBytes - header.mRawBytes, header.mRawBytes,
                header.mStoredBytes});
        }
        catch (const std::bad_alloc&)
        {
            SetError(error, "Out of memory while indexing archive blocks");
            return false;
        }
        offset += block::HEADER_BYTES + header.mStoredBytes;
    }
    if (logicalBytes < STREAM_HEADER_BYTES)
    {
        SetError(error, "Archive logical frame stream is shorter than its header");
        return false;
    }
    outLogicalBytes = logicalBytes;
    return true;
}

/**
 * 未完成录像：从候选帧流段首起逐块完整判定（块头、载荷完整、解码恰为 rawBytes、CRC），
 * 在第一个无效或不完整的块之前截止，其后全部忽略；只有有效块才计入逻辑长度，累加后超过 1 GiB 即拒绝整份文件。
 * 读文件失败是错误，不是截止。
 */
bool ScanUnfinalizedBlocks(HANDLE file, std::uint64_t segmentStart, std::uint64_t candidateBytes,
    std::vector<BlockIndexEntry>& outIndex, std::uint64_t& outLogicalBytes, std::uint64_t& outStoredBytes,
    std::string& error)
{
    outIndex.clear();
    std::vector<std::uint8_t> stored;
    std::vector<std::uint8_t> raw;
    std::uint64_t offset = 0;
    std::uint64_t logicalBytes = 0;
    try
    {
        while (candidateBytes - offset >= block::HEADER_BYTES)
        {
            std::array<std::uint8_t, block::HEADER_BYTES> headerBytes{};
            if (!ReadAt(file, segmentStart + offset, headerBytes.data(), headerBytes.size(), error))
            {
                return false;
            }
            block::Header header;
            std::string ignoredError;
            if (!block::ParseBlockHeader(headerBytes.data(), block::STREAM_RAW_LIMIT, header, ignoredError) ||
                header.mStoredBytes > candidateBytes - offset - block::HEADER_BYTES)
            {
                break;
            }
            stored.resize(header.mStoredBytes);
            if (!ReadExact(file, stored.data(), stored.size(), error))
            {
                return false;
            }
            if (!block::DecodeBlockPayload(header, stored.data(), raw, ignoredError))
            {
                break;
            }
            logicalBytes += header.mRawBytes;
            if (logicalBytes > STREAM_LIMIT)
            {
                SetError(error, "Unfinalized archive logical frame stream exceeds the format limit");
                return false;
            }
            outIndex.push_back({segmentStart + offset, logicalBytes - header.mRawBytes, header.mRawBytes,
                header.mStoredBytes});
            offset += block::HEADER_BYTES + header.mStoredBytes;
        }
    }
    catch (const std::bad_alloc&)
    {
        SetError(error, "Out of memory while scanning archive blocks");
        return false;
    }
    if (outIndex.empty())
    {
        SetError(error, "Unfinalized archive has no valid frame stream block");
        return false;
    }
    if (logicalBytes < STREAM_HEADER_BYTES)
    {
        SetError(error, "Unfinalized archive lacks a complete frame stream header");
        return false;
    }
    outLogicalBytes = logicalBytes;
    outStoredBytes = offset;
    return true;
}
}

class ArchiveReader::Impl
{
public:
    ~Impl() { Close(); }

    void Close()
    {
        mStream.reset();
        mStreamBuffer.reset();
        if (mFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mFile);
            mFile = INVALID_HANDLE_VALUE;
        }
    }

    bool Open(const std::wstring& path, OpenMode mode, ArchiveInfo& outInfo, std::string& error)
    {
        Close();
        FileHandleGuard file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE)
        {
            SetWindowsError(error, "CreateFileW", GetLastError());
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file.Get(), &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(HEADER_BYTES))
        {
            SetError(error, "Archive is shorter than the fixed header");
            return false;
        }

        std::array<std::uint8_t, HEADER_BYTES> header{};
        if (!ReadExact(file.Get(), header.data(), header.size(), error))
        {
            return false;
        }
        if (std::memcmp(header.data(), ARCHIVE_MAGIC, sizeof(ARCHIVE_MAGIC)) != 0)
        {
            SetError(error, "Archive magic is invalid");
            return false;
        }
        const std::uint32_t version = LoadU32(header.data() + 8);
        const std::uint32_t headerBytes = LoadU32(header.data() + 12);
        const std::uint32_t metadataBytes = LoadU32(header.data() + 16);
        const std::uint32_t flags = LoadU32(header.data() + 20);
        const std::uint64_t headerStreamBytes = LoadU64(header.data() + 24);
        if ((version != ARCHIVE_VERSION_PLAIN && version != ARCHIVE_VERSION_BLOCKS) || headerBytes != HEADER_BYTES)
        {
            SetError(error, "Archive version or header size is unsupported");
            return false;
        }
        const bool hasBlocks = version == ARCHIVE_VERSION_BLOCKS;
        const std::uint64_t minimumMetadataBytes = hasBlocks ? MIN_BLOCK_SEGMENT_BYTES : 1;
        if (metadataBytes < minimumMetadataBytes || metadataBytes > METADATA_LIMIT)
        {
            SetError(error, "Archive metadata size is outside the format limit");
            return false;
        }
        bool isFinalized = true;
        if (!ClassifyFlags(mode, flags, isFinalized, error))
        {
            return false;
        }

        const std::uint64_t payloadStart = HEADER_BYTES + static_cast<std::uint64_t>(metadataBytes);
        const std::uint64_t fileBytes = static_cast<std::uint64_t>(fileSize.QuadPart);
        // 完成文件：帧流段磁盘长度取自头；未完成录像：取 metadata 之后的全部字节（候选帧流段）。
        std::uint64_t segmentBytes = 0;
        if (isFinalized)
        {
            segmentBytes = headerStreamBytes;
            const std::uint64_t minimumStreamBytes = hasBlocks ? MIN_BLOCK_SEGMENT_BYTES : STREAM_HEADER_BYTES;
            if (segmentBytes < minimumStreamBytes || segmentBytes > STREAM_LIMIT)
            {
                SetError(error, "Archive frame stream exceeds the format limit");
                return false;
            }
            if (fileBytes != payloadStart + segmentBytes)
            {
                SetError(error, "Archive length does not match the header");
                return false;
            }
            if (!VerifyPayloadHash(file.Get(), metadataBytes, segmentBytes, header.data() + 64, error))
            {
                return false;
            }
        }
        else
        {
            // 写入端在 Finish 之前不更新这两个字段；非零说明它不是写入端留下的未完成录像。
            const std::array<std::uint8_t, 32> zeroHash{};
            if (headerStreamBytes != 0 || std::memcmp(header.data() + 64, zeroHash.data(), zeroHash.size()) != 0)
            {
                SetError(error, "Unfinalized archive header must have zero stream length and payload hash");
                return false;
            }
            const std::uint64_t minimumFileBytes = payloadStart + (hasBlocks ? 0 : STREAM_HEADER_BYTES);
            if (fileBytes < minimumFileBytes)
            {
                SetError(error, "Unfinalized archive lacks complete metadata or frame stream header");
                return false;
            }
            // 写入端在 1 GiB 处即失败，正常不会产生更大的文件；超限一律拒绝而不是截断。
            segmentBytes = fileBytes - payloadStart;
            if (segmentBytes > STREAM_LIMIT)
            {
                SetError(error, "Unfinalized archive frame stream exceeds the format limit");
                return false;
            }
        }

        ArchiveInfo info;
        std::memcpy(info.mTargetHash.data(), header.data() + 32, info.mTargetHash.size());
        info.mContainerVersion = version;
        info.mIsFinalized = isFinalized;
        std::unique_ptr<LogicalStreamBuffer> streamBuffer;
        if (hasBlocks)
        {
            if (!ReadMetadataBlock(file.Get(), metadataBytes, info.mMetadata, error))
            {
                return false;
            }
            std::vector<BlockIndexEntry> index;
            std::uint64_t logicalBytes = 0;
            std::uint64_t storedBytes = segmentBytes;
            const bool isIndexed = isFinalized ?
                IndexFinalizedBlocks(file.Get(), payloadStart, segmentBytes, index, logicalBytes, error) :
                ScanUnfinalizedBlocks(file.Get(), payloadStart, segmentBytes, index, logicalBytes, storedBytes, error);
            if (!isIndexed)
            {
                return false;
            }
            info.mStreamBytes = logicalBytes;
            info.mStoredStreamBytes = storedBytes;
            streamBuffer = std::make_unique<BlockStreamBuffer>(file.Get(), std::move(index), logicalBytes);
        }
        else
        {
            info.mMetadata.resize(metadataBytes);
            if (!ReadAt(file.Get(), HEADER_BYTES, info.mMetadata.data(), metadataBytes, error))
            {
                return false;
            }
            info.mStreamBytes = segmentBytes;
            info.mStoredStreamBytes = segmentBytes;
            streamBuffer = std::make_unique<BoundedFileStreamBuffer>(file.Get(), payloadStart, segmentBytes);
        }

        mFile = file.Release();
        mStreamBuffer = std::move(streamBuffer);
        mStream = std::make_unique<std::istream>(mStreamBuffer.get());
        outInfo = std::move(info);
        error.clear();
        return true;
    }

    HANDLE mFile = INVALID_HANDLE_VALUE;
    std::unique_ptr<LogicalStreamBuffer> mStreamBuffer;
    std::unique_ptr<std::istream> mStream;
};

ArchiveReader::ArchiveReader() : mImpl(std::make_unique<Impl>()) {}
ArchiveReader::~ArchiveReader() = default;
ArchiveReader::ArchiveReader(ArchiveReader&&) noexcept = default;
ArchiveReader& ArchiveReader::operator=(ArchiveReader&&) noexcept = default;

bool ArchiveReader::Open(const std::wstring& path, ArchiveInfo& outInfo, std::string& error)
{
    return mImpl->Open(path, OpenMode::Finalized, outInfo, error);
}

bool ArchiveReader::OpenUnfinalized(const std::wstring& path, ArchiveInfo& outInfo, std::string& error)
{
    return mImpl->Open(path, OpenMode::Unfinalized, outInfo, error);
}

bool ArchiveReader::OpenByFlags(const std::wstring& path, ArchiveInfo& outInfo, std::string& error)
{
    return mImpl->Open(path, OpenMode::ByFlags, outInfo, error);
}

int ArchiveReader::Read(void* buffer, int sizeBytes)
{
    if (!mImpl || !mImpl->mStream || sizeBytes <= 0 || buffer == nullptr)
    {
        return 0;
    }
    mImpl->mStream->clear();
    mImpl->mStream->read(static_cast<char*>(buffer), sizeBytes);
    return static_cast<int>(mImpl->mStream->gcount());
}

bool ArchiveReader::Seek(int offset, int origin)
{
    if (!mImpl || !mImpl->mStream || !mImpl->mStreamBuffer)
    {
        return false;
    }
    std::ios_base::seekdir direction;
    if (origin == SEEK_SET)
    {
        direction = std::ios_base::beg;
    }
    else if (origin == SEEK_CUR)
    {
        direction = std::ios_base::cur;
    }
    else if (origin == SEEK_END)
    {
        direction = std::ios_base::end;
    }
    else
    {
        return false;
    }
    mImpl->mStream->clear();
    mImpl->mStream->seekg(offset, direction);
    return !mImpl->mStream->fail();
}

int ArchiveReader::GetPosition() const
{
    if (!mImpl || !mImpl->mStreamBuffer)
    {
        return 0;
    }
    return static_cast<int>(mImpl->mStreamBuffer->GetPosition());
}

int ArchiveReader::GetSize() const
{
    return mImpl && mImpl->mStreamBuffer ? static_cast<int>(mImpl->mStreamBuffer->GetLogicalSize()) : 0;
}

std::istream& ArchiveReader::GetStream()
{
    static std::istream emptyStream(nullptr);
    if (!mImpl || !mImpl->mStream)
    {
        emptyStream.clear();
        return emptyStream;
    }
    return *mImpl->mStream;
}

bool ArchiveReader::Rewind()
{
    return Seek(0, SEEK_SET);
}

std::string ArchiveReader::GetStreamError() const
{
    if (!mImpl || !mImpl->mStreamBuffer)
    {
        return {};
    }
    return mImpl->mStreamBuffer->GetError();
}

void ArchiveReader::Close()
{
    if (mImpl)
    {
        mImpl->Close();
    }
}

class ArchiveWriter::Impl
{
public:
    explicit Impl(const ArchiveBlockPolicy& policy) : mPolicy(policy) {}
    ~Impl() { CloseFile(); }

    void CloseFile()
    {
        if (mFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mFile);
            mFile = INVALID_HANDLE_VALUE;
        }
    }

    bool IsPolicyValid() const
    {
        return mPolicy.mTargetRawBytes != 0 && mPolicy.mTargetRawBytes <= block::STREAM_RAW_LIMIT &&
            mPolicy.mMaxUnits != 0;
    }

    /**
     * 把待写块压缩成一块并以一次连续写入写出（进程中途被杀时文件尾最多留下一块不完整的块），随后更新 SHA-256。
     * 帧流段磁盘字节数将超过 1 GiB 或写入失败时返回 false，失败标志由调用方决定；待写块为空时什么也不做。
     */
    bool FlushPendingBlock(std::string& error)
    {
        if (mPendingRaw.empty())
        {
            return true;
        }
        if (!block::EncodeBlock(mPendingRaw.data(), mPendingRaw.size(), mBlockBuffer, error))
        {
            return false;
        }
        if (mBlockBuffer.size() > STREAM_LIMIT - mStoredBytes)
        {
            SetError(error, "Archive frame stream segment would exceed 1 GiB");
            return false;
        }
        if (!WriteAll(mFile, mBlockBuffer.data(), mBlockBuffer.size(), error) ||
            !mHash.Update(mBlockBuffer.data(), mBlockBuffer.size(), error))
        {
            return false;
        }
        mStoredBytes += mBlockBuffer.size();
        mPendingRaw.clear();
        mPendingUnits = 0;
        return true;
    }

    ArchiveBlockPolicy mPolicy;
    HANDLE mFile = INVALID_HANDLE_VALUE;
    std::wstring mPath;
    std::wstring mPartialPath;
    std::array<std::uint8_t, 32> mTargetHash{};
    /** 清单段的磁盘字节数（块头加载荷）。 */
    std::uint32_t mMetadataBytes = 0;
    /** 逻辑帧流字节数，含待写块。 */
    std::uint64_t mLogicalBytes = 0;
    /** 已写出的帧流段磁盘字节数。 */
    std::uint64_t mStoredBytes = 0;
    // 待写块与压缩输出缓冲在写入端生命周期内复用容量，Abort 与重新 Open 只清空内容。
    std::vector<std::uint8_t> mPendingRaw;
    std::uint32_t mPendingUnits = 0;
    std::vector<std::uint8_t> mBlockBuffer;
    Sha256 mHash;
    bool mFailed = false;
    bool mFinished = false;
};

ArchiveWriter::ArchiveWriter(const ArchiveBlockPolicy& policy) : mImpl(std::make_unique<Impl>(policy)) {}
ArchiveWriter::~ArchiveWriter() = default;
ArchiveWriter::ArchiveWriter(ArchiveWriter&&) noexcept = default;
ArchiveWriter& ArchiveWriter::operator=(ArchiveWriter&&) noexcept = default;

bool ArchiveWriter::Open(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash,
    const std::vector<std::uint8_t>& metadata, std::string& error)
{
    Abort();
    if (!mImpl->IsPolicyValid())
    {
        SetError(error, "Archive block policy is invalid");
        return false;
    }
    if (metadata.empty() || metadata.size() > block::METADATA_RAW_LIMIT)
    {
        SetError(error, "Metadata must be 1..16 MiB");
        return false;
    }
    if (PathExists(path))
    {
        SetError(error, "Final archive already exists; refusing to overwrite");
        return false;
    }
    std::vector<std::uint8_t> metadataBlock;
    if (!block::EncodeBlock(metadata.data(), metadata.size(), metadataBlock, error))
    {
        return false;
    }
    // 只有约 16 MiB 且几乎不可压缩的清单才会因块头而超出清单段上限。
    if (metadataBlock.size() > METADATA_LIMIT)
    {
        SetError(error, "Encoded metadata block exceeds 16 MiB");
        return false;
    }

    mImpl->mPath = path;
    mImpl->mPartialPath = path + L".partial";
    mImpl->mFile = CreateFileW(mImpl->mPartialPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (mImpl->mFile == INVALID_HANDLE_VALUE)
    {
        SetWindowsError(error, "CreateFileW(partial)", GetLastError());
        return false;
    }
    mImpl->mTargetHash = targetHash;
    mImpl->mMetadataBytes = static_cast<std::uint32_t>(metadataBlock.size());
    if (!mImpl->mHash.Open(error))
    {
        mImpl->mFailed = true;
        return false;
    }

    const std::array<std::uint8_t, 32> emptyHash{};
    const auto header = EncodeHeader(mImpl->mMetadataBytes, FLAG_UNFINALIZED, 0, mImpl->mTargetHash, emptyHash);
    if (!WriteAll(mImpl->mFile, header.data(), header.size(), error) ||
        !WriteAll(mImpl->mFile, metadataBlock.data(), metadataBlock.size(), error) ||
        !mImpl->mHash.Update(metadataBlock.data(), metadataBlock.size(), error))
    {
        mImpl->mFailed = true;
        return false;
    }
    error.clear();
    return true;
}

int ArchiveWriter::Write(const void* buffer, int sizeBytes)
{
    if (!mImpl || mImpl->mFile == INVALID_HANDLE_VALUE || mImpl->mFailed || mImpl->mFinished || sizeBytes < 0 ||
        (sizeBytes > 0 && buffer == nullptr))
    {
        if (mImpl && (sizeBytes < 0 || (sizeBytes > 0 && buffer == nullptr)))
        {
            mImpl->mFailed = true;
        }
        return 0;
    }
    if (sizeBytes == 0)
    {
        return 0;
    }
    const auto unitBytes = static_cast<std::uint64_t>(sizeBytes);
    // 单元不可拆分，超过一块的上限就无法表示。
    if (unitBytes > STREAM_LIMIT - mImpl->mLogicalBytes || unitBytes > block::STREAM_RAW_LIMIT)
    {
        mImpl->mFailed = true;
        return 0;
    }
    std::string error;
    // 生产策略下追加前待写块不足 64 KiB，加上最长帧记录也远小于 4 MiB；只有更大的测试目标值才会先写出待写块。
    if (mImpl->mPendingRaw.size() + unitBytes > block::STREAM_RAW_LIMIT && !mImpl->FlushPendingBlock(error))
    {
        mImpl->mFailed = true;
        return 0;
    }
    try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(buffer);
        mImpl->mPendingRaw.insert(mImpl->mPendingRaw.end(), bytes, bytes + sizeBytes);
    }
    catch (const std::bad_alloc&)
    {
        mImpl->mFailed = true;
        return 0;
    }
    mImpl->mLogicalBytes += unitBytes;
    ++mImpl->mPendingUnits;
    if ((mImpl->mPendingRaw.size() >= mImpl->mPolicy.mTargetRawBytes ||
            mImpl->mPendingUnits >= mImpl->mPolicy.mMaxUnits) &&
        !mImpl->FlushPendingBlock(error))
    {
        mImpl->mFailed = true;
        return 0;
    }
    return sizeBytes;
}

bool ArchiveWriter::Finish(std::string& error)
{
    if (!mImpl || mImpl->mFile == INVALID_HANDLE_VALUE || mImpl->mFailed || mImpl->mFinished)
    {
        SetError(error, "Archive writer is not in a finishable state");
        return false;
    }
    if (mImpl->mLogicalBytes < STREAM_HEADER_BYTES)
    {
        SetError(error, "Frame stream is shorter than its header");
        mImpl->mFailed = true;
        return false;
    }
    if (!mImpl->FlushPendingBlock(error))
    {
        mImpl->mFailed = true;
        return false;
    }

    std::array<std::uint8_t, 32> payloadHash{};
    if (!mImpl->mHash.Finish(payloadHash, error))
    {
        mImpl->mFailed = true;
        return false;
    }
    const auto header = EncodeHeader(mImpl->mMetadataBytes, FLAG_FINALIZED, mImpl->mStoredBytes, mImpl->mTargetHash, payloadHash);
    if (!SeekFile(mImpl->mFile, 0, error) || !WriteAll(mImpl->mFile, header.data(), header.size(), error) || !FlushFileBuffers(mImpl->mFile))
    {
        if (error.empty())
        {
            SetWindowsError(error, "FlushFileBuffers", GetLastError());
        }
        mImpl->mFailed = true;
        return false;
    }
    mImpl->CloseFile();
    if (!MoveFileW(mImpl->mPartialPath.c_str(), mImpl->mPath.c_str()))
    {
        SetWindowsError(error, "MoveFileW(finalize)", GetLastError());
        mImpl->mFailed = true;
        return false;
    }
    mImpl->mFinished = true;
    error.clear();
    return true;
}

void ArchiveWriter::Abort() noexcept
{
    if (!mImpl)
    {
        return;
    }
    if (mImpl->mFile != INVALID_HANDLE_VALUE && !mImpl->mFailed && !mImpl->mFinished)
    {
        // 待写块只含完整单元，写出后可按未完成录像恢复；不回填头、不改名，任何错误都忽略。
        try
        {
            std::string ignoredError;
            mImpl->FlushPendingBlock(ignoredError);
        }
        catch (...)
        {
        }
    }
    mImpl->CloseFile();
    mImpl->mHash.Close();
    mImpl->mPath.clear();
    mImpl->mPartialPath.clear();
    mImpl->mTargetHash.fill(0);
    mImpl->mMetadataBytes = 0;
    mImpl->mLogicalBytes = 0;
    mImpl->mStoredBytes = 0;
    mImpl->mPendingRaw.clear();
    mImpl->mPendingUnits = 0;
    mImpl->mFailed = false;
    mImpl->mFinished = false;
}

int ArchiveWriter::GetSize() const
{
    return mImpl ? static_cast<int>(mImpl->mLogicalBytes) : 0;
}

int ArchiveWriter::GetStoredSize() const
{
    return mImpl ? static_cast<int>(mImpl->mStoredBytes) : 0;
}

bool ArchiveWriter::Failed() const
{
    return !mImpl || mImpl->mFailed;
}
}
