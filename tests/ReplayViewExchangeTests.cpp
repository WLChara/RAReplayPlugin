#include "ReplayViewExchange.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        rareplay::viewer::ReplayViewExchange exchange;
        Require(!exchange.Read(), "Initially empty");
        auto first = std::make_shared<rareplay::viewer::ReplayViewSnapshot>();
        first->mSessionId = 1;
        first->mFrameNumber = 45;
        exchange.Publish(first);
        const auto held = exchange.Read();
        std::thread producer([&exchange]
        {
            for (std::uint32_t frame = 46; frame < 1000; ++frame)
            {
                auto snapshot = std::make_shared<rareplay::viewer::ReplayViewSnapshot>();
                snapshot->mSessionId = 1;
                snapshot->mFrameNumber = frame;
                exchange.Publish(std::move(snapshot));
            }
        });
        for (int index = 0; index < 1000; ++index)
        {
            const auto snapshot = exchange.Read();
            Require(snapshot && snapshot->mSessionId == 1, "Reader sees a complete owned snapshot");
        }
        producer.join();
        Require(held->mFrameNumber == 45, "Old snapshot outlives publication replacement");
        for (int value = 0; value < 100; ++value)
            exchange.Submit({1, rareplay::viewer::ViewerCommandKind::SetPlaybackSpeed, value});
        const auto commands = exchange.Drain();
        Require(commands.size() == 32 && commands.back().mValue == 99, "Bounded queue keeps newest input");
        Require(exchange.Drain().empty(), "Draining does not replay commands");
        exchange.Submit({1, rareplay::viewer::ViewerCommandKind::SetPlaybackSpeed, 2});
        exchange.Clear();
        Require(!exchange.Read() && exchange.Drain().empty(), "Session end clears snapshot and commands");
        Require(held->mFrameNumber == 45, "Render reader remains valid during session end");

        Require(!exchange.ReadHudPresence(), "HUD presence starts withdrawn");
        const auto presentedAt = std::chrono::steady_clock::time_point(std::chrono::seconds(42));
        exchange.ReportHudPresented(3, presentedAt, true);
        const auto presence = exchange.ReadHudPresence();
        Require(presence && presence->mSessionId == 3 && presence->mPresentedAt == presentedAt &&
            presence->mIsPanelCovering, "Presented HUD keeps session, time and covering flag");
        exchange.ReportHudPresented(3, presentedAt, false);
        Require(exchange.ReadHudPresence() && !exchange.ReadHudPresence()->mIsPanelCovering,
            "HUD can be presented without covering the native sidebar");
        exchange.WithdrawHudPresence();
        Require(!exchange.ReadHudPresence(), "F10 hide withdraws presence immediately");
        exchange.ReportHudPresented(3, presentedAt, true);
        exchange.Clear();
        Require(!exchange.ReadHudPresence(), "Session boundary withdraws presence");

        // 落点屏幕位置走独立通道：整体替换、旧读者不受影响、会话边界清空。
        Require(!exchange.ReadMarks(), "Mark channel starts empty");
        auto marks = std::make_shared<rareplay::viewer::SuperWeaponMarkFrame>();
        marks->mSessionId = 4;
        marks->mGameTimeMs = 1000;
        auto info = std::make_shared<rareplay::viewer::SuperWeaponMarkInfo>();
        info->mId = 9;
        marks->mMarks.push_back({info, {10, 20}});
        exchange.PublishMarks(marks);
        const auto heldMarks = exchange.ReadMarks();
        Require(heldMarks && heldMarks->mMarks.size() == 1 && heldMarks->mMarks[0].mPosition.mX == 10,
            "Mark frame is read as published");
        std::thread markProducer([&exchange, info]
        {
            for (int index = 0; index < 1000; ++index)
            {
                auto frame = std::make_shared<rareplay::viewer::SuperWeaponMarkFrame>();
                frame->mSessionId = 4;
                frame->mGameTimeMs = index;
                frame->mMarks.push_back({info, {index, index * 2}});
                exchange.PublishMarks(std::move(frame));
            }
        });
        for (int index = 0; index < 1000; ++index)
        {
            const auto frame = exchange.ReadMarks();
            if (frame)
                Require(frame->mMarks.size() == 1 && frame->mMarks[0].mPosition.mY == frame->mMarks[0].mPosition.mX * 2,
                    "Reader never sees a torn mark frame");
        }
        markProducer.join();
        Require(heldMarks->mMarks[0].mPosition.mX == 10, "Old mark frame outlives replacement");
        exchange.Clear();
        Require(!exchange.ReadMarks(), "Session boundary clears marks");
        std::cout << "Replay view exchange tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
