#include "core/CommandLine.h"
#include "core/Hotkeys.h"
#include "core/MediaFiles.h"
#include "core/MediaItem.h"
#include "core/PlayerService.h"
#include "core/Playlist.h"
#include "core/PlaylistParser.h"
#include "core/ResumeStore.h"
#include "core/Settings.h"
#include "core/TimeFormat.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
using namespace reel;

static int g_passed = 0, g_failed = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; std::cerr << "FAIL at line " << __LINE__ << ": " #cond "\n"; } \
} while (0)

static fs::path makeTempDir() {
    auto dir = fs::temp_directory_path() / (L"reel_cpp_test_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(rand()));
    fs::create_directories(dir);
    return dir;
}

int main() {
    // 1. TimeFormat
    CHECK(TimeFormat::format(0) == L"00:00");
    CHECK(TimeFormat::format(5000) == L"00:05");
    CHECK(TimeFormat::format(65000) == L"01:05");
    CHECK(TimeFormat::format(3600000) == L"1:00:00");
    CHECK(TimeFormat::format(36125000) == L"10:02:05");
    CHECK(TimeFormat::format(-10) == L"00:00");

    int64_t ms = 0;
    CHECK(TimeFormat::tryParse(L"90", ms) && ms == 90000);
    CHECK(TimeFormat::tryParse(L"1:30", ms) && ms == 90000);
    CHECK(TimeFormat::tryParse(L"1:02:03", ms) && ms == 3723000);
    CHECK(TimeFormat::tryParse(L"0:00:01.5", ms) && ms == 1500);
    CHECK(TimeFormat::tryParse(L" 12:00 ", ms) && ms == 720000);
    CHECK(!TimeFormat::tryParse(L"", ms));
    CHECK(!TimeFormat::tryParse(L"abc", ms));
    CHECK(!TimeFormat::tryParse(L"1:75", ms));
    CHECK(!TimeFormat::tryParse(L"1:2:3:4", ms));
    CHECK(!TimeFormat::tryParse(L"-5", ms));

    // 2. PlaylistParser
    {
        std::wstring base = L"C:\\Media";
        auto items = PlaylistParser::parseM3U(
            "#EXTM3U\n#EXTINF:123,First Song\nmusic/a.mp3\n#EXTINF:-1 tvg-id=\"x\",Live\nhttps://example.com/live.m3u8\n\nC:\\Other\\b.mkv\n",
            base);
        CHECK(items.size() == 3);
        CHECK(items[0]->location == L"C:\\Media\\music\\a.mp3");
        CHECK(items[0]->title == L"First Song");
        CHECK(items[0]->durationMs == 123000);
        CHECK(items[1]->location == L"https://example.com/live.m3u8");
        CHECK(items[1]->title == L"Live");
        CHECK(items[1]->durationMs == 0);
        CHECK(items[2]->location == L"C:\\Other\\b.mkv");
        CHECK(items[2]->title == L"b.mkv");

        auto uriItems = PlaylistParser::parseM3U("file:///C:/Videos/My%20Movie.mp4", base);
        CHECK(uriItems.size() == 1 && uriItems[0]->location == L"C:\\Videos\\My Movie.mp4");

        auto pls = PlaylistParser::parsePls("[playlist]\nFile2=two.mp3\nTitle2=Two\nFile1=one.mp3\nLength1=60\nNumberOfEntries=2\n", base);
        CHECK(pls.size() == 2);
        CHECK(pls[0]->location == L"C:\\Media\\one.mp3" && pls[0]->durationMs == 60000);
        CHECK(pls[1]->location == L"C:\\Media\\two.mp3" && pls[1]->title == L"Two");

        auto xspf = PlaylistParser::parseXspf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\"><trackList>\n"
            "  <track><location>file:///C:/A/x.mkv</location><title>X</title><duration>5000</duration></track>\n"
            "  <track><location>rel/y.mp4</location></track>\n"
            "</trackList></playlist>", base);
        CHECK(xspf.size() == 2);
        CHECK(xspf[0]->location == L"C:\\A\\x.mkv" && xspf[0]->title == L"X" && xspf[0]->durationMs == 5000);
        CHECK(xspf[1]->location == L"C:\\Media\\rel\\y.mp4");

        // Round-trip
        std::vector<MediaItemPtr> rt = {
            std::make_shared<MediaItem>(L"C:\\V\\a b.mkv", L"A B", 61000),
            std::make_shared<MediaItem>(L"https://h/s.m3u8", L"Stream", 0)
        };
        auto m3uRt = PlaylistParser::parseM3U(PlaylistParser::toM3U(rt), base);
        CHECK(m3uRt.size() == 2 && m3uRt[0]->location == rt[0]->location && m3uRt[0]->durationMs == 61000);
        auto xspfRt = PlaylistParser::parseXspf(PlaylistParser::toXspf(rt), base);
        CHECK(xspfRt.size() == 2 && xspfRt[0]->location == rt[0]->location && xspfRt[1]->location == rt[1]->location);

        // HLS detection
        auto dir = makeTempDir();
        {
            std::ofstream(dir / L"stream.m3u8") << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\nseg1.ts\n";
            std::ofstream(dir / L"list.m3u8") << "#EXTM3U\na.mp3\n";
        }
        CHECK(!PlaylistParser::shouldExpand((dir / L"stream.m3u8").wstring()));
        CHECK(PlaylistParser::shouldExpand((dir / L"list.m3u8").wstring()));
        fs::remove_all(dir);
    }

    // 3. Playlist
    {
        Playlist p(42);
        p.addRange({
            std::make_shared<MediaItem>(L"C:\\m\\1.mp4"),
            std::make_shared<MediaItem>(L"C:\\m\\2.mp4"),
            std::make_shared<MediaItem>(L"C:\\m\\3.mp4"),
        });
        p.setCurrentIndex(2);
        CHECK(p.peekNext(false) == nullptr);
        p.setLoop(LoopMode::All);
        CHECK(p.peekNext(false) == p.items()[0]);
        p.setCurrentIndex(0);
        CHECK(p.peekPrevious() == p.items()[2]);
        p.setLoop(LoopMode::One);
        p.setCurrentIndex(1);
        CHECK(p.peekNext(false) == p.items()[1]);
        CHECK(p.peekNext(true) == p.items()[2]);

        // Shuffle plays all items once
        Playlist sp(42);
        sp.setShuffle(true);
        for (int i = 1; i <= 6; ++i) sp.addRange({ std::make_shared<MediaItem>(L"C:\\m\\" + std::to_wstring(i) + L".mp4") });
        std::set<MediaItemPtr> seen;
        while (auto next = sp.peekNext(false)) {
            seen.insert(next);
            sp.setCurrent(next);
            if (seen.size() > 6) break;
        }
        CHECK(seen.size() == 6);
    }

    // 4. Hotkeys
    {
        CHECK(HotkeyMap::normalize("shift+ctrl+left") == "Ctrl+Shift+Left");
        CHECK(HotkeyMap::normalize("Control+Alt+Q") == "Ctrl+Alt+Q");
        CHECK(HotkeyMap::normalize("esc") == "Escape");
        CHECK(HotkeyMap::normalize("Ctrl++") == "Ctrl++");

        HotkeyMap map;
        CHECK(map.resolve("Space") == "PlayPause");
        CHECK(map.resolve("Shift+S") == "Snapshot");
        CHECK(map.resolve("Ctrl+R") == "RotateVideo");
        CHECK(map.resolve("Shift+L") == "AbLoop");

        std::set<std::string> unique;
        size_t total = 0;
        for (const auto& a : HotkeyActions::all()) {
            for (const auto& g : a.defaultGestures) {
                ++total;
                unique.insert(HotkeyMap::normalize(g));
            }
        }
        CHECK(unique.size() == total);
    }

    // 5. ResumeStore & Settings
    {
        auto dir = makeTempDir();
        {
            ResumeStore rs(dir.wstring());
            rs.update(L"C:\\a.mkv", 120000, 3600000);
            rs.update(L"C:\\b.mkv", 2000, 3600000);
            CHECK(rs.save());
        }
        {
            ResumeStore rs(dir.wstring());
            CHECK(rs.get(L"c:\\A.MKV") == 120000);
            CHECK(!rs.get(L"C:\\b.mkv").has_value());
        }
        {
            SettingsService svc(dir.wstring());
            svc.load();
            svc.current().volume = 135;
            svc.current().loop = LoopMode::One;
            svc.current().hotkeys["Mute"] = "Ctrl+M";
            CHECK(svc.save());
        }
        {
            SettingsService svc(dir.wstring());
            auto& s = svc.load();
            CHECK(s.volume == 135);
            CHECK(s.loop == LoopMode::One);
            CHECK(s.hotkeys["Mute"] == "Ctrl+M");
        }
        fs::remove_all(dir);
    }

    // 6. CommandLine & MediaFiles
    {
        auto cmd = CommandLine::parse({ L"--fullscreen", L"--start-time=90.5", L"C:\\a.mkv", L"https://x/y", L"--bogus" });
        CHECK(cmd.fullscreen);
        CHECK(cmd.startTimeMs == 90500);
        CHECK(cmd.locations.size() == 2 && cmd.locations[1] == L"https://x/y");

        std::vector<std::wstring> names = { L"ep10.mkv", L"ep2.mkv", L"Ep1.mkv" };
        std::sort(names.begin(), names.end(), MediaFiles::naturalLess);
        CHECK(names[0] == L"Ep1.mkv" && names[1] == L"ep2.mkv" && names[2] == L"ep10.mkv");

        auto dir = makeTempDir();
        auto f1 = (dir / L"ep1.mkv").wstring();
        auto f2 = (dir / L"ep2.mkv").wstring();
        auto f10 = (dir / L"ep10.mkv").wstring();
        std::ofstream(dir / L"ep1.mkv"); std::ofstream(dir / L"ep2.mkv"); std::ofstream(dir / L"ep10.mkv");
        std::ofstream(dir / L"ep3.txt");
        CHECK(MediaFiles::findSiblingMedia(f1, +1) == f2);
        CHECK(MediaFiles::findSiblingMedia(f2, +1) == f10);
        CHECK(!MediaFiles::findSiblingMedia(f10, +1).has_value());
        CHECK(MediaFiles::findSiblingMedia(f10, -1) == f2);
        fs::remove_all(dir);
    }

    std::cout << "CoreTests: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
