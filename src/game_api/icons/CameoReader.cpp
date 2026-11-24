#include "CameoReader.h"

#include "../../assets/CameoPixels.h"

using byte = unsigned char;

#include <Windows.h>
#include <Unknwn.h>
#include <CCINIClass.h>
#include <Drawing.h>
#include <FileSystem.h>
#include <PCX.h>
#include <Surface.h>
#include <SuperWeaponTypeClass.h>
#include <TechnoTypeClass.h>
#include <FileFormats/SHP.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace rareplay::game_api
{
namespace
{
constexpr std::size_t MAX_PCX_ENTRIES = 512;
constexpr int MAX_ICON_DIMENSION = 256;

assets::Rgb16Format NativeRgb16Format()
{
    return {
        static_cast<std::uint16_t>(Drawing::RGB_To_Int(255, 0, 0)),
        static_cast<std::uint16_t>(Drawing::RGB_To_Int(0, 255, 0)),
        static_cast<std::uint16_t>(Drawing::RGB_To_Int(0, 0, 255))};
}

// 不让编译器依据静态 BSurface 类型将 SDK 的虚函数桩去虚拟化。
__declspec(noinline) bool CaptureSurface(Surface* surface, std::vector<std::uint8_t>& outPixels,
    int& outWidth, int& outHeight)
{
    if (!surface)
        return false;
    const int width = surface->GetWidth();
    const int height = surface->GetHeight();
    const int bytesPerPixel = surface->GetBytesPerPixel();
    const int pitchBytes = surface->GetPitch();
    if (width <= 0 || width > MAX_ICON_DIMENSION || height <= 0 || height > MAX_ICON_DIMENSION ||
        bytesPerPixel != 2 || pitchBytes < width * 2 || pitchBytes > MAX_ICON_DIMENSION * 4)
        return false;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 2);
    const auto* locked = static_cast<const std::uint8_t*>(surface->Lock(0, 0));
    if (!locked)
        return false;
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * width * 2,
            locked + static_cast<std::size_t>(y) * pitchBytes, static_cast<std::size_t>(width) * 2);
    const bool unlocked = surface->Unlock();
    if (!unlocked)
        return false;
    outPixels = std::move(pixels);
    outWidth = width;
    outHeight = height;
    return true;
}

__declspec(noinline) bool FillSurface(Surface* surface, std::uint16_t color)
{
    return surface && surface->Fill(color);
}

// key 为结果图像的缓存键；单位与超武用不同的前缀，即使引用同一个 PCX 文件也各自独立缓存。
std::shared_ptr<const viewer::IconImage> LoadPcx(const char* fileName, const std::string& key,
    bool& outInvalidSurface)
{
    if (!PCX::Instance.LoadFile(fileName))
        return {};
    BSurface* pcxSurface = PCX::Instance.GetSurface(fileName, nullptr);
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    if (!CaptureSurface(pcxSurface, pixels, width, height))
    {
        outInvalidSurface = true;
        return {};
    }
    auto image = std::make_shared<viewer::IconImage>();
    image->mKey = key;
    if (!assets::DecodeCameoPixels(pixels, width, height, width * 2,
        NativeRgb16Format(), PCX::DefaultTransparentColor, *image))
    {
        outInvalidSurface = true;
        return {};
    }
    return image;
}

std::shared_ptr<const viewer::IconImage> LoadShp(SHPStruct* shape, const std::string& key,
    bool& outInvalidSurface)
{
    if (!shape)
        return {};
    SHPFile* data = shape->GetData();
    if (!data || data->Frames <= 0 || data->Width <= 0 || data->Height <= 0 ||
        data->Width > MAX_ICON_DIMENSION || data->Height > MAX_ICON_DIMENSION ||
        !FileSystem::CAMEO_PAL)
    {
        outInvalidSurface = true;
        return {};
    }
    const int width = data->Width;
    const int height = data->Height;
    BSurface scratch(width, height);
    Surface* target = &scratch;
    const RectangleStruct bounds{0, 0, width, height};
    const Point2D position{0, 0};
    ConvertClass* palette = FileSystem::CAMEO_PAL;
    auto draw = [&]() {
        CC_Draw_Shape(target, palette, shape, 0, &position, &bounds,
            BlitterFlags::None, 0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
    };
    std::vector<std::uint8_t> darkPixels;
    std::vector<std::uint8_t> lightPixels;
    int capturedWidth = 0;
    int capturedHeight = 0;
    if (!FillSurface(target, COLOR_BLACK))
    {
        outInvalidSurface = true;
        return {};
    }
    draw();
    if (!CaptureSurface(target, darkPixels, capturedWidth, capturedHeight) ||
        capturedWidth != width || capturedHeight != height ||
        !FillSurface(target, COLOR_WHITE))
    {
        outInvalidSurface = true;
        return {};
    }
    draw();
    if (!CaptureSurface(target, lightPixels, capturedWidth, capturedHeight) ||
        capturedWidth != width || capturedHeight != height)
    {
        outInvalidSurface = true;
        return {};
    }
    auto image = std::make_shared<viewer::IconImage>();
    image->mKey = key;
    if (!assets::DecodeCameoPixelsWithBackgrounds(darkPixels, lightPixels, width, height,
        width * 2, NativeRgb16Format(), *image))
    {
        outInvalidSurface = true;
        return {};
    }
    bool hasVisiblePixel = false;
    for (std::size_t index = 3; index < image->mRgba.size(); index += 4)
    {
        if (image->mRgba[index] != 0)
        {
            hasVisiblePixel = true;
            break;
        }
    }
    if (!hasVisiblePixel)
    {
        outInvalidSurface = true;
        return {};
    }
    return image;
}
}

void CameoReader::Clear() noexcept
{
    mTypes.Clear();
    mSuperWeapons.Clear();
    mPcxFiles.clear();
    mStats.mPcx = 0;
    mStats.mShp = 0;
    mStats.mMissing = 0;
    mStats.mInvalidSurface = 0;
    mStats.mCacheLimit = 0;
    mStats.mRecentDetail.clear();
}

void CameoReader::BeginSample() noexcept
{
    mTypes.BeginSample();
    mSuperWeapons.BeginSample();
}

std::shared_ptr<const viewer::IconImage> CameoReader::Resolve(const TechnoTypeClass* type)
{
    if (!type)
        return {};
    const std::string typeId(type->ID, strnlen_s(type->ID, sizeof(type->ID)));
    if (typeId.empty())
        return {};
    bool atCapacity = false;
    auto image = mTypes.Resolve(typeId, [&] { return Load(type, typeId); }, atCapacity);
    if (atCapacity)
        ++mStats.mCacheLimit;
    return image;
}

std::shared_ptr<const viewer::IconImage> CameoReader::Load(const TechnoTypeClass* type,
    const std::string& typeId)
{
    const char* section = type->ImageFile[0] ? type->ImageFile : type->ID;
    char pcxFile[32]{};
    CCINIClass::INI_Art.ReadString(section, "CameoPCX", "", pcxFile, sizeof(pcxFile));
    bool invalidSurface = false;
    std::shared_ptr<const viewer::IconImage> image;
    if (pcxFile[0])
    {
        const std::string fileName(pcxFile);
        if (const auto found = mPcxFiles.find(fileName); found != mPcxFiles.end())
        {
            image = found->second.mImage.lock();
            if (found->second.mMissing)
                invalidSurface = found->second.mInvalidSurface;
            else if (!image)
            {
                image = LoadPcx(pcxFile, "pcx:" + fileName, invalidSurface);
                found->second.mImage = image;
                found->second.mMissing = !image;
                found->second.mInvalidSurface = invalidSurface;
            }
        }
        else
        {
            image = LoadPcx(pcxFile, "pcx:" + fileName, invalidSurface);
            if (mPcxFiles.size() < MAX_PCX_ENTRIES)
                mPcxFiles.emplace(fileName, PcxEntry{image, !image, invalidSurface});
        }
        if (image)
        {
            ++mStats.mPcx;
            mStats.mRecentDetail = typeId + ": PCX " + fileName;
            return image;
        }
    }
    image = LoadShp(type->GetCameo(), "shp:" + typeId, invalidSurface);
    if (image)
    {
        ++mStats.mShp;
        if (invalidSurface)
            ++mStats.mInvalidSurface;
        mStats.mRecentDetail = typeId + (pcxFile[0] ?
            ": PCX " + std::string(pcxFile) + " unavailable; SHP fallback" :
            ": native SHP cameo");
        return image;
    }
    ++mStats.mMissing;
    if (invalidSurface)
        ++mStats.mInvalidSurface;
    mStats.mRecentDetail = typeId + (invalidSurface ? ": invalid PCX/SHP surface" :
        pcxFile[0] ? ": PCX " + std::string(pcxFile) + " unavailable; SHP absent" :
        ": no PCX or SHP cameo");
    return {};
}

std::shared_ptr<const viewer::IconImage> CameoReader::ResolveSuperWeapon(const SuperWeaponTypeClass* type)
{
    if (!type)
        return {};
    const std::string typeId(type->ID, strnlen_s(type->ID, sizeof(type->ID)));
    if (typeId.empty())
        return {};
    bool atCapacity = false;
    auto image = mSuperWeapons.Resolve("sw:" + typeId, [&] { return LoadSuperWeapon(type, typeId); }, atCapacity);
    if (atCapacity)
        ++mStats.mCacheLimit;
    return image;
}

std::shared_ptr<const viewer::IconImage> CameoReader::LoadSuperWeapon(const SuperWeaponTypeClass* type,
    const std::string& typeId)
{
    const std::string key = "sw:" + typeId;
    char pcxFile[64]{};
    if (CCINIClass::INI_Rules)
        CCINIClass::INI_Rules->ReadString(type->ID, "SidebarPCX", "", pcxFile, sizeof(pcxFile));
    // Ares 在预载入时把文件名转为小写；PCX::Instance 按名称查找是否区分大小写未核实，所以先转小写。
    for (char* character = pcxFile; *character; ++character)
        *character = static_cast<char>(std::tolower(static_cast<unsigned char>(*character)));
    bool invalidSurface = false;
    std::shared_ptr<const viewer::IconImage> image;
    if (pcxFile[0])
    {
        image = LoadPcx(pcxFile, key, invalidSurface);
        if (image)
        {
            ++mStats.mPcx;
            mStats.mRecentDetail = typeId + ": PCX " + pcxFile;
            return image;
        }
    }
    // SidebarImage 在 SHP 缺失时被回退为 XXICON.SHP；那只是通用的“XX”图标，视为没有图标。
    // XXICON 此时必已在文件缓存里（回退就是它加载的）；缓存里没有时说明没有任何类型回退到它，
    // 这里的查找至多把它读进文件缓存一次，不影响模拟。
    SHPStruct* shape = type->SidebarImage;
    if (shape && shape != static_cast<SHPStruct*>(FileSystem::LoadFile("XXICON.SHP", false)))
    {
        image = LoadShp(shape, key, invalidSurface);
        if (image)
        {
            ++mStats.mShp;
            mStats.mRecentDetail = typeId + ": native SHP sidebar image";
            return image;
        }
    }
    ++mStats.mMissing;
    if (invalidSurface)
        ++mStats.mInvalidSurface;
    mStats.mRecentDetail = typeId + (invalidSurface ? ": invalid PCX/SHP surface" :
        pcxFile[0] ? ": PCX " + std::string(pcxFile) + " unavailable; no SHP" : ": no PCX or SHP sidebar image");
    return {};
}
}
