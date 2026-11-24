#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <istream>
#include <string>
#include <vector>

namespace rareplay::storage
{
struct ArchiveInfo
{
    std::array<std::uint8_t, 32> mTargetHash{};
    /** 开局清单的原始字节；容器 v3 为解压后的清单块内容。 */
    std::vector<std::uint8_t> mMetadata;
    /** 容器版本：2 或 3。 */
    std::uint32_t mContainerVersion = 0;
    /**
     * 逻辑帧流（RPF2 帧流头加帧记录）的字节数，也是 Read、Seek、GetSize 使用的坐标。
     * 完成文件为已校验的长度；未完成录像 v2 为文件实际帧流长度，v3 为有效块前缀解压后的总长（均已保证不超过 1 GiB）。
     */
    std::uint64_t mStreamBytes = 0;
    /**
     * 帧流段的磁盘字节数。完成文件为头中 streamBytes；未完成录像 v2 与 mStreamBytes 相同，v3 为有效块前缀的磁盘字节数。
     * v2 两者总是相等。
     */
    std::uint64_t mStoredStreamBytes = 0;
    /** 未完成录像（flags==0）为 false；它没有内容哈希，帧流内容尚未经过帧级校验。 */
    bool mIsFinalized = true;
};

/**
 * 读取 RAREPL2 容器 v2 与 v3；帧流以有长度边界的逻辑流形式提供，v3 按块索引逐块按需解码，内存中只保留当前块。
 * Read、Seek 和 GetStream 共享同一读取位置，位置只相对于逻辑帧流（帧流头位于偏移 0）。
 *
 * 三种打开方式只在容器头 flags 的处理上不同，其余头部要求（magic、版本、metadata 范围）一致。
 * 是否为未完成录像只由 flags 决定，与文件后缀无关。
 * 逻辑流因块解码失败、CRC 不符或读文件失败而提前停止时按 EOF 返回，原因由 GetStreamError 报告，调用方不得把它当作正常结束。
 */
class ArchiveReader
{
public:
    ArchiveReader();
    ~ArchiveReader();
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
    ArchiveReader(ArchiveReader&&) noexcept;
    ArchiveReader& operator=(ArchiveReader&&) noexcept;

    /**
     * 只接受 flags==1 的完成文件：严格核对长度并校验头之后全部磁盘字节的 SHA-256。
     * v3 另核对清单块与帧流块头链（首尾相接、正好结束在 streamBytes），帧流块在读取时才解码。
     */
    bool Open(const std::wstring& path, ArchiveInfo& outInfo, std::string& error);
    /**
     * 只接受 flags==0 的未完成录像：头中 streamBytes 与内容哈希必须全为零，不校验哈希；
     * 候选帧流段为文件中 metadata 之后的全部字节，超过 1 GiB 时拒绝。
     * v2 的逻辑帧流就是候选帧流段；v3 逐块完整解码，在第一个无效或不完整的块之前截止，有效块拼接为逻辑帧流。
     * 逻辑帧流不足 32 字节帧流头时拒绝。不检查帧流内容；可恢复的帧由调用方扫描确定。
     */
    bool OpenUnfinalized(const std::wstring& path, ArchiveInfo& outInfo, std::string& error);
    /** 按 flags 分流：1 走 Open 的规则，0 走 OpenUnfinalized 的规则，其他值拒绝。 */
    bool OpenByFlags(const std::wstring& path, ArchiveInfo& outInfo, std::string& error);
    int Read(void* buffer, int sizeBytes);
    bool Seek(int offset, int origin);
    int GetPosition() const;
    /** 逻辑帧流字节数。 */
    int GetSize() const;
    std::istream& GetStream();
    bool Rewind();
    /**
     * 逻辑流提前停止的原因：块解码失败、CRC 不符或读文件失败；为空表示没有发生读取错误。
     * 错误一经记下即保持到 Close 或重新打开，此后读取一律按 EOF 返回。
     */
    std::string GetStreamError() const;
    void Close();

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

/**
 * 帧流段的切块策略。默认值为生产值；测试与样例生成可以传入更小的值。
 * 追加一个单元后，待写块达到 mTargetRawBytes 字节或 mMaxUnits 个单元即压缩写出。
 */
struct ArchiveBlockPolicy
{
    /** 1..4 MiB。 */
    std::uint32_t mTargetRawBytes = 64 * 1024;
    /** 至少为 1。 */
    std::uint32_t mMaxUnits = 600;
};

/**
 * 一律写容器 v3：清单为一块，帧流按策略切块，每块独立 deflate 压缩并带 CRC-32。
 * 每次 Write 是不可拆分的单元，块只在单元边界切分；压缩在调用线程同步进行。
 * 析构只关闭临时文件（待写块随之丢失，等同进程崩溃），不会伪造完成状态。
 */
class ArchiveWriter
{
public:
    explicit ArchiveWriter(const ArchiveBlockPolicy& policy = ArchiveBlockPolicy{});
    ~ArchiveWriter();
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;
    ArchiveWriter(ArchiveWriter&&) noexcept;
    ArchiveWriter& operator=(ArchiveWriter&&) noexcept;

    /** metadata 为 1..16 MiB，编码后的清单块也不得超过 16 MiB；创建独占 .partial 并写出头与清单块。 */
    bool Open(const std::wstring& path, const std::array<std::uint8_t, 32>& targetHash,
        const std::vector<std::uint8_t>& metadata, std::string& error);
    /** 追加一个不可拆分的单元；成功返回 sizeBytes，失败返回 0 并进入失败状态。 */
    int Write(const void* buffer, int sizeBytes);
    /** 写出最后一块，回填 streamBytes、内容哈希与 flags=1，flush 后无覆盖地改名为最终文件。 */
    bool Finish(std::string& error);
    /**
     * 未处于失败状态时尽力写出待写块（任何错误都忽略），不回填头、不改名；随后关闭文件并清空状态，保留 .partial。
     */
    void Abort() noexcept;
    /** 逻辑帧流字节数（含待写块）；Finish 成功后保持到下次 Open 或 Abort。 */
    int GetSize() const;
    /** 已写出的帧流段磁盘字节数（不含待写块）；Finish 成功后保持到下次 Open 或 Abort。 */
    int GetStoredSize() const;
    bool Failed() const;

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};
}
