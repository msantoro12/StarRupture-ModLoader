// Image disk test: the files the image library saves and the rules around them.
//
// The format (UI/image_cache_file.cpp): a file written and read back, and every way a file can be
// wrong (bad magic, wrong revision, truncated, a flipped byte, too many levels). The folder key
// (a change in any input gives a new folder). The writer (UI/image_cache_writer.cpp) on a real
// temporary folder: tmp then rename, a file of the same size left alone, a folder that cannot be
// written switching the disk part off once, the bounded queue, and the stop. The saver
// (UI/image_disk.cpp) on a fake readback: what a finished copy becomes and when the set list is
// written.
//
// A console program: no game, no GPU.
#include "UI/image_disk.h"
#include "UI/image_shrink.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

using namespace ImageCache;

// ---------------------------------------------------------------------------
// Helpers

static std::vector<uint8_t> MakePixels(uint32_t width, uint32_t height, uint8_t seed)
{
    const uint32_t levels = ImageShrink::LevelCount(width, height);
    std::vector<uint8_t> pixels(static_cast<size_t>(ImageShrink::Bytes({ width, height }, levels)));
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<uint8_t>(seed + i * 7);
    return pixels;
}

static Info MakeInfo(uint32_t width = 64, uint32_t height = 32)
{
    Info info;
    info.sourceFormat = 77;
    info.sourceWidth  = 512;
    info.sourceHeight = 256;
    info.storedWidth  = width;
    info.storedHeight = height;
    info.levels       = ImageShrink::LevelCount(width, height);
    info.path         = "/Game/UI/T_Icon.T_Icon";
    return info;
}

static void Put32At(std::vector<uint8_t>& file, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        file[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

static std::vector<uint8_t> Valid(Info* outInfo = nullptr, std::vector<uint8_t>* outPixels = nullptr)
{
    const Info info = MakeInfo();
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 3);
    if (outInfo) *outInfo = info;
    if (outPixels) *outPixels = pixels;
    return Serialize(info, pixels.data(), pixels.size());
}

static Parsed ParseOf(const std::vector<uint8_t>& file)
{
    Info info;
    const uint8_t* pixels = nullptr;
    return Parse(file.data(), file.size(), info, pixels);
}

// ---------------------------------------------------------------------------
// The format

static void TestCrc()
{
    const char* text = "123456789";
    CHECK(Crc32(text, 9) == 0xCBF43926u);
    CHECK(Crc32(nullptr, 0) == 0);
    // Chained over two parts it is the crc of the whole.
    CHECK(Crc32(text + 4, 5, Crc32(text, 4)) == Crc32(text, 9));
}

static void TestRoundTrip()
{
    Info info;
    std::vector<uint8_t> pixels;
    const std::vector<uint8_t> file = Valid(&info, &pixels);
    CHECK(!file.empty());
    CHECK(file.size() == kHeaderBytes + info.path.size() + pixels.size());

    Info read;
    const uint8_t* payload = nullptr;
    CHECK(Parse(file.data(), file.size(), read, payload) == Parsed::Ok);
    CHECK(read.sourceFormat == info.sourceFormat);
    CHECK(read.sourceWidth == info.sourceWidth && read.sourceHeight == info.sourceHeight);
    CHECK(read.storedWidth == info.storedWidth && read.storedHeight == info.storedHeight);
    CHECK(read.levels == info.levels);
    CHECK(read.path == info.path);
    CHECK(payload == file.data() + kHeaderBytes + info.path.size());
    CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
}

static void TestRoundTripShapes()
{
    // The shapes the shrink makes: square, wide, tall and the smallest.
    const uint32_t sizes[][2] = { { 256, 256 }, { 256, 171 }, { 181, 256 }, { 1, 1 }, { 32, 32 } };
    for (const auto& size : sizes)
    {
        const Info info = MakeInfo(size[0], size[1]);
        const std::vector<uint8_t> pixels = MakePixels(size[0], size[1], 9);
        const std::vector<uint8_t> file = Serialize(info, pixels.data(), pixels.size());
        Info read;
        const uint8_t* payload = nullptr;
        CHECK(Parse(file.data(), file.size(), read, payload) == Parsed::Ok);
        CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
    }
}

static void TestSerializeRefusals()
{
    const Info info = MakeInfo();
    const std::vector<uint8_t> pixels = MakePixels(info.storedWidth, info.storedHeight, 1);

    CHECK(Serialize(info, pixels.data(), pixels.size() - 1).empty());
    CHECK(Serialize(info, pixels.data(), pixels.size() + 4).empty());
    CHECK(Serialize(info, nullptr, pixels.size()).empty());

    Info wrongLevels = info;
    wrongLevels.levels = info.levels + 1;
    CHECK(Serialize(wrongLevels, pixels.data(), pixels.size()).empty());

    Info noPath = info;
    noPath.path.clear();
    CHECK(Serialize(noPath, pixels.data(), pixels.size()).empty());

    Info longPath = info;
    longPath.path.assign(kMaxPathLength + 1, 'a');
    CHECK(Serialize(longPath, pixels.data(), pixels.size()).empty());

    Info empty = info;
    empty.storedWidth = 0;
    CHECK(Serialize(empty, pixels.data(), pixels.size()).empty());
}

static void TestBadMagic()
{
    std::vector<uint8_t> file = Valid();
    file[0] ^= 0xFF;
    CHECK(ParseOf(file) == Parsed::BadMagic);
    // A set list is not an image file, whatever its size.
    SetList sets;
    sets.push_back({ 1, { "/Game/UI/T_A.T_A", "/Game/UI/T_B.T_B", "/Game/UI/T_C.T_C" } });
    CHECK(ParseOf(SerializeSets(sets)) == Parsed::BadMagic);
}

static void TestWrongRevision()
{
    std::vector<uint8_t> file = Valid();
    Put32At(file, 4, kRevision + 1);
    CHECK(ParseOf(file) == Parsed::WrongRevision);
    Put32At(file, 4, 0);
    CHECK(ParseOf(file) == Parsed::WrongRevision);
}

static void TestTruncated()
{
    const std::vector<uint8_t> file = Valid();

    CHECK(ParseOf({}) == Parsed::TooShort);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + 10)) == Parsed::TooShort);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes - 1)) == Parsed::TooShort);

    // Header only, header and part of the path, and the payload cut anywhere.
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + kHeaderBytes + 5)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.end() - 1)) == Parsed::Truncated);
    CHECK(ParseOf(std::vector<uint8_t>(file.begin(), file.begin() + file.size() / 2)) == Parsed::Truncated);

    // Bytes after the payload are not part of the file.
    std::vector<uint8_t> longer = file;
    longer.push_back(0);
    CHECK(ParseOf(longer) == Parsed::Truncated);
}

static void TestCrcMismatch()
{
    const std::vector<uint8_t> file = Valid();

    std::vector<uint8_t> pixel = file;
    pixel.back() ^= 1;
    CHECK(ParseOf(pixel) == Parsed::BadCrc);

    std::vector<uint8_t> path = file;
    path[kHeaderBytes + 3] ^= 1;
    CHECK(ParseOf(path) == Parsed::BadCrc);

    // A header field the other checks do not constrain (the source format, the source size).
    std::vector<uint8_t> format = file;
    format[8] ^= 1;
    CHECK(ParseOf(format) == Parsed::BadCrc);
    std::vector<uint8_t> source = file;
    source[12] ^= 1;
    CHECK(ParseOf(source) == Parsed::BadCrc);

    std::vector<uint8_t> stored = file;
    stored[44] ^= 1;   // the crc itself
    CHECK(ParseOf(stored) == Parsed::BadCrc);
}

static void TestBadHeader()
{
    const std::vector<uint8_t> file = Valid();

    // More levels than a chain can have, however well the rest agrees.
    std::vector<uint8_t> levels = file;
    Put32At(levels, 28, kMaxLevels + 1);
    CHECK(ParseOf(levels) == Parsed::BadHeader);
    Put32At(levels, 28, 0);
    CHECK(ParseOf(levels) == Parsed::BadHeader);
    Put32At(levels, 28, 0xFFFFFFFFu);
    CHECK(ParseOf(levels) == Parsed::BadHeader);

    // A level count that is not the chain of the stored size.
    std::vector<uint8_t> chain = file;
    Put32At(chain, 28, ImageShrink::LevelCount(64, 32) - 1);
    CHECK(ParseOf(chain) == Parsed::BadHeader);

    std::vector<uint8_t> width = file;
    Put32At(width, 20, kMaxSide + 1);
    CHECK(ParseOf(width) == Parsed::BadHeader);
    Put32At(width, 20, 0);
    CHECK(ParseOf(width) == Parsed::BadHeader);

    std::vector<uint8_t> path = file;
    Put32At(path, 32, kMaxPathLength + 1);
    CHECK(ParseOf(path) == Parsed::BadHeader);
    Put32At(path, 32, 0);
    CHECK(ParseOf(path) == Parsed::BadHeader);

    // A payload size that is not the size of the levels.
    std::vector<uint8_t> payload = file;
    Put32At(payload, 36, 1);
    CHECK(ParseOf(payload) == Parsed::BadHeader);
    std::vector<uint8_t> huge = file;
    Put32At(huge, 40, 0x7FFFFFFFu);
    CHECK(ParseOf(huge) == Parsed::BadHeader);

    std::vector<uint8_t> source = file;
    Put32At(source, 12, 0);
    CHECK(ParseOf(source) == Parsed::BadHeader);
}

static void TestSetListRoundTrip()
{
    SetList sets;
    sets.push_back({ 1, { "/Game/A.A", "/Game/B.B" } });
    sets.push_back({ 2, { "/Game/C.C" } });

    const std::vector<uint8_t> file = SerializeSets(sets);
    SetList read;
    CHECK(ParseSets(file.data(), file.size(), read) == Parsed::Ok);
    CHECK(read == sets);

    // An empty list is a valid file.
    const std::vector<uint8_t> none = SerializeSets({});
    SetList empty = sets;
    CHECK(ParseSets(none.data(), none.size(), empty) == Parsed::Ok);
    CHECK(empty.empty());
}

static void TestSetListBad()
{
    SetList sets;
    sets.push_back({ 1, { "/Game/A.A", "/Game/B.B" } });
    const std::vector<uint8_t> file = SerializeSets(sets);
    SetList read;

    std::vector<uint8_t> magic = file;
    magic[0] ^= 1;
    CHECK(ParseSets(magic.data(), magic.size(), read) == Parsed::BadMagic);

    std::vector<uint8_t> revision = file;
    Put32At(revision, 4, kRevision + 1);
    CHECK(ParseSets(revision.data(), revision.size(), read) == Parsed::WrongRevision);

    CHECK(ParseSets(file.data(), 8, read) == Parsed::TooShort);
    CHECK(ParseSets(file.data(), file.size() - 1, read) == Parsed::BadCrc);

    std::vector<uint8_t> flipped = file;
    flipped[flipped.size() - 8] ^= 1;
    CHECK(ParseSets(flipped.data(), flipped.size(), read) == Parsed::BadCrc);

    // A count that runs past the data, with a crc that is right for the bytes.
    std::vector<uint8_t> count = file;
    Put32At(count, 16, 1000);
    Put32At(count, count.size() - 4, Crc32(count.data(), count.size() - 4));
    CHECK(ParseSets(count.data(), count.size(), read) == Parsed::Truncated);

    std::vector<uint8_t> setCount = file;
    Put32At(setCount, 8, kMaxSets + 1);
    Put32At(setCount, setCount.size() - 4, Crc32(setCount.data(), setCount.size() - 4));
    CHECK(ParseSets(setCount.data(), setCount.size(), read) == Parsed::BadHeader);
}

// ---------------------------------------------------------------------------
// Names and the folder key

static void TestFileName()
{
    const std::string a = FileName("/game/ui/t_a.t_a");
    CHECK(a == FileName("/game/ui/t_a.t_a"));
    CHECK(a != FileName("/game/ui/t_b.t_b"));
    CHECK(a.size() == 16 + 4);
    CHECK(a.substr(16) == ".img");
    CHECK(a.find_first_not_of("0123456789abcdef.img") == std::string::npos);
}

static void TestFolderKey()
{
    FolderKey base;
    base.section    = L"1.0.0-CL-127004|65A1B2C3|3F40000";
    base.pakHash    = 0x1234;
    base.revision   = kRevision;
    base.storedSize = 256;
    const std::wstring name = FolderName(base);

    CHECK(name == FolderName(base));
    CHECK(name.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos);

    FolderKey changed = base;
    changed.section = L"1.0.0-CL-129402|65A1B2C3|3F40000";    // a game update
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.section = L"1.0.0-CL-127004|65A1B2C4|3F40000";    // the same version, another exe
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.section = L"1.0.0-CL-127004|65A1B2C3|3F40001";    // another image size
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.pakHash = 0x1235;                                 // a pak change
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.revision = kRevision + 1;                         // a format change
    CHECK(FolderName(changed) != name);
    changed = base;
    changed.storedSize = 128;                                 // a size change
    CHECK(FolderName(changed) != name);
}

static void TestPakHash()
{
    const std::vector<PakEntry> paks = {
        { "C:/Game/Content/Paks/pakchunk0.pak", "../../../StarRupture/Content/" },
        { "C:/Game/Content/Paks/~mods/a.pak", "../../../StarRupture/Content/" },
    };
    const uint64_t hash = HashPakSet(paks);

    // The order the pak list comes in does not matter.
    CHECK(HashPakSet({ paks[1], paks[0] }) == hash);

    std::vector<PakEntry> added = paks;
    added.push_back({ "C:/Game/Content/Paks/~mods/b.pak", "../../../StarRupture/Content/" });
    CHECK(HashPakSet(added) != hash);

    std::vector<PakEntry> removed = { paks[0] };
    CHECK(HashPakSet(removed) != hash);

    std::vector<PakEntry> moved = paks;
    moved[1].mountPoint = "../../../Other/";
    CHECK(HashPakSet(moved) != hash);

    // A path and a mount point do not run into each other.
    CHECK(HashPakSet({ { "ab", "c" } }) != HashPakSet({ { "a", "bc" } }));
    CHECK(HashPakSet({}) != hash);
}

// ---------------------------------------------------------------------------
// The writer, on a real folder

struct TempFolder
{
    std::wstring path;

    TempFolder()
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"image_disk_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    }

    ~TempFolder() { Remove(path); }

    static void Remove(const std::wstring& dir)
    {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..")
                    continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    Remove(dir + L"\\" + name);
                else
                    DeleteFileW((dir + L"\\" + name).c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        RemoveDirectoryW(dir.c_str());
    }

    std::vector<std::wstring> Files(const std::wstring& sub = L"") const
    {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW((path + sub + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return files;
        do
        {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return files;
    }

    std::vector<uint8_t> Read(const std::wstring& file) const
    {
        std::vector<uint8_t> bytes;
        HANDLE h = CreateFileW((path + L"\\" + file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return bytes;
        bytes.resize(GetFileSize(h, nullptr));
        DWORD read = 0;
        ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(h);
        return bytes;
    }
};

struct LogCollector
{
    std::mutex lock;
    std::vector<std::string> lines;
    Writer::Log Sink()
    {
        return [this](const std::string& line) { std::lock_guard<std::mutex> l(lock); lines.push_back(line); };
    }
    size_t Count() { std::lock_guard<std::mutex> l(lock); return lines.size(); }
};

static Job BytesJob(const std::wstring& folder, const std::string& name, std::vector<uint8_t> bytes, bool replace = false)
{
    Job job;
    job.folder  = [folder] { return folder; };
    job.name    = name;
    job.make    = [bytes] { return bytes; };
    job.replace = replace;
    return job;
}

static bool WaitFor(const std::function<bool()>& done, int ms = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static void TestWriterWrites()
{
    TempFolder temp;
    LogCollector log;
    const std::wstring folder = temp.path + L"\\a\\b";   // neither level exists yet
    {
        Writer writer(log.Sink());
        CHECK(writer.Submit(BytesJob(folder, "one.img", { 1, 2, 3, 4 })));
        CHECK(writer.Submit(BytesJob(folder, "two.img", { 5, 6 })));
        CHECK(WaitFor([&] { return temp.Files(L"\\a\\b").size() == 2; }));
        writer.Stop();
    }
    // Only the finished files: no tmp name is left behind.
    const std::vector<std::wstring> files = temp.Files(L"\\a\\b");
    CHECK(files.size() == 2);
    for (const std::wstring& file : files)
        CHECK(file.find(L".tmp") == std::wstring::npos);
    CHECK(temp.Read(L"a\\b\\one.img") == (std::vector<uint8_t>{ 1, 2, 3, 4 }));
    CHECK(temp.Read(L"a\\b\\two.img") == (std::vector<uint8_t>{ 5, 6 }));
    CHECK(log.Count() == 0);
}

static void TestWriterKeepsSameSize()
{
    TempFolder temp;
    LogCollector log;
    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 1, 1, 1, 1 })));
    CHECK(WaitFor([&] { return temp.Read(L"x.img").size() == 4; }));

    // The same size is left alone; a job that says replace writes over it.
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 2, 2, 2, 2 })));
    CHECK(writer.Submit(BytesJob(temp.path, "marker.img", { 9 })));
    CHECK(WaitFor([&] { return temp.Read(L"marker.img").size() == 1; }));
    CHECK(temp.Read(L"x.img") == (std::vector<uint8_t>{ 1, 1, 1, 1 }));

    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 3, 3, 3, 3 }, true)));
    CHECK(WaitFor([&] { return temp.Read(L"x.img") == std::vector<uint8_t>{ 3, 3, 3, 3 }; }));

    // A different size is written.
    CHECK(writer.Submit(BytesJob(temp.path, "x.img", { 4, 4 })));
    CHECK(WaitFor([&] { return temp.Read(L"x.img") == std::vector<uint8_t>{ 4, 4 }; }));
    writer.Stop();
    CHECK(temp.Files().size() == 2);
}

static void TestWriterEmptyBytesWriteNothing()
{
    TempFolder temp;
    LogCollector log;
    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "none.img", {})));
    CHECK(writer.Submit(BytesJob(temp.path, "marker.img", { 1 })));
    CHECK(WaitFor([&] { return temp.Read(L"marker.img").size() == 1; }));
    writer.Stop();
    CHECK(temp.Files().size() == 1);
    CHECK(!writer.Off());
}

static void TestWriterOffOnUnusableFolder()
{
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    // A file where the folder should be: the folder cannot be made.
    {
        HANDLE h = CreateFileW((temp.path + L"\\blocked").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CloseHandle(h);
    }

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path + L"\\blocked\\sub", "a.img", { 1 })));
    CHECK(WaitFor([&] { return writer.Off(); }));

    // One line, and nothing more is taken or logged.
    CHECK(log.Count() == 1);
    CHECK(!writer.Submit(BytesJob(temp.path + L"\\fine", "b.img", { 1 })));
    CHECK(!writer.Submit(BytesJob(temp.path + L"\\blocked\\sub", "c.img", { 1 })));
    writer.Stop();
    CHECK(log.Count() == 1);
    CHECK(temp.Files(L"\\fine").empty());
}

static void TestWriterOffOnReadOnlyFile()
{
    // A read-only file in the way of the rename is an access error on the folder's files.
    TempFolder temp;
    LogCollector log;
    CreateDirectoryW(temp.path.c_str(), nullptr);
    const std::wstring name = temp.path + L"\\ro.img";
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);
    DWORD wrote = 0;
    WriteFile(h, "ab", 2, &wrote, nullptr);
    CloseHandle(h);

    Writer writer(log.Sink());
    CHECK(writer.Submit(BytesJob(temp.path, "ro.img", { 1, 2, 3 })));
    CHECK(WaitFor([&] { return writer.Off(); }));
    CHECK(log.Count() == 1);
    writer.Stop();

    // The tmp file is not left behind.
    CHECK(temp.Files().size() == 1);
    SetFileAttributesW(name.c_str(), FILE_ATTRIBUTE_NORMAL);
}

static void TestWriterQueueIsBounded()
{
    TempFolder temp;
    LogCollector log;
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };

    Writer writer(log.Sink(), 3);
    Job blocker;
    blocker.folder = [&temp] { return temp.path; };
    blocker.name   = "blocker.img";
    blocker.make   = [&] { started = true; WaitFor([&] { return release.load(); }); return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(blocker)));
    CHECK(WaitFor([&] { return started.load(); }));

    // The worker holds one job, so three more fit and the fourth does not.
    CHECK(writer.Submit(BytesJob(temp.path, "1.img", { 1, 1 })));
    CHECK(writer.Submit(BytesJob(temp.path, "2.img", { 1, 1 })));
    CHECK(writer.Submit(BytesJob(temp.path, "3.img", { 1, 1 })));
    CHECK(!writer.Submit(BytesJob(temp.path, "4.img", { 1, 1 })));
    CHECK(!writer.Off());

    release = true;
    CHECK(WaitFor([&] { return temp.Files().size() == 4; }));
    // Room again.
    CHECK(writer.Submit(BytesJob(temp.path, "5.img", { 1, 1 })));
    writer.Stop();
}

static void TestWriterRefusesBadJobs()
{
    LogCollector log;
    Writer writer(log.Sink());
    Job noMake;
    noMake.folder = [] { return std::wstring(L"x"); };
    noMake.name   = "a.img";
    CHECK(!writer.Submit(std::move(noMake)));
    Job noFolder;
    noFolder.name = "a.img";
    noFolder.make = [] { return std::vector<uint8_t>{ 1 }; };
    CHECK(!writer.Submit(std::move(noFolder)));
}

static void TestWriterStop()
{
    TempFolder temp;
    LogCollector log;
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };

    Writer writer(log.Sink());
    Job blocker;
    blocker.folder = [&temp] { return temp.path; };
    blocker.name   = "blocker.img";
    blocker.make   = [&] { started = true; WaitFor([&] { return release.load(); }); return std::vector<uint8_t>{ 1 }; };
    CHECK(writer.Submit(std::move(blocker)));
    CHECK(WaitFor([&] { return started.load(); }));
    CHECK(writer.Submit(BytesJob(temp.path, "queued.img", { 2 })));

    std::thread stopper([&] { writer.Stop(); });
    // Stop has begun once nothing is taken any more.
    CHECK(WaitFor([&] { return !writer.Submit(BytesJob(temp.path, "late.img", { 3 })); }));
    release = true;
    stopper.join();

    // The job in hand finished; the one queued and the late one were never written.
    const std::vector<std::wstring> files = temp.Files();
    CHECK(files.size() == 1 && files[0] == L"blocker.img");
    CHECK(!writer.Submit(BytesJob(temp.path, "after.img", { 4 })));
    writer.Stop();   // twice is fine
    CHECK(temp.Files().size() == 1);
}

static void TestWriterStopBeforeStart()
{
    LogCollector log;
    Writer writer(log.Sink());
    writer.Stop();   // no thread was ever started
    CHECK(!writer.Submit(BytesJob(L"x", "a.img", { 1 })));
}

// ---------------------------------------------------------------------------
// The saver, on a fake readback and a fake sink

class FakeSink : public Sink
{
public:
    std::vector<Job> jobs;
    bool off      = false;
    bool accept   = true;
    int  stops    = 0;

    bool Submit(Job&& job) override
    {
        if (!accept || off)
            return false;
        jobs.push_back(std::move(job));
        return true;
    }
    bool Off() const override { return off; }
    void Stop() override { ++stops; }
};

class FakeReadbacks : public ImageDisk::Host
{
public:
    struct Entry
    {
        ImageDisk::Readback state = ImageDisk::Readback::Pending;
        std::vector<uint8_t> pixels;
    };
    std::map<uint32_t, Entry> entries;
    std::vector<uint32_t> taken;       // tickets asked for
    std::vector<bool> hadDest;         // whether each ask had somewhere to put the pixels

    ImageDisk::Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) override
    {
        taken.push_back(ticket);
        hadDest.push_back(dest != nullptr);
        const auto found = entries.find(ticket);
        if (found == entries.end())
            return ImageDisk::Readback::Gone;
        if (found->second.state == ImageDisk::Readback::Done && dest && found->second.pixels.size() == bytes)
            memcpy(dest, found->second.pixels.data(), bytes);
        return found->second.state;
    }

    std::function<std::wstring()> CacheFolder() override { return [] { return std::wstring(L"cache\\folder"); }; }
};

static ImageDisk::Started MakeStarted(uint32_t ticket, const char* path = "/Game/UI/T_Icon.T_Icon")
{
    ImageDisk::Started started;
    started.path         = path;
    started.sourceFormat = 77;
    started.sourceWidth  = 512;
    started.sourceHeight = 512;
    started.storedWidth  = 64;
    started.storedHeight = 32;
    started.levels       = ImageShrink::LevelCount(64, 32);
    started.ticket       = ticket;
    return started;
}

static void TestSaverWritesFinishedCopy()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    const std::vector<uint8_t> pixels = MakePixels(64, 32, 11);
    host.entries[5].pixels = pixels;

    saver.Begin(MakeStarted(5));
    saver.Poll(0);
    // Still on the GPU: asked, nothing queued, asked again next time.
    CHECK(host.taken.size() == 1 && sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 2 && sink.jobs.empty());

    host.entries[5].state = ImageDisk::Readback::Done;
    saver.Poll(2);
    CHECK(sink.jobs.size() == 1);
    // Taken once: it is not asked about again.
    saver.Poll(3);
    CHECK(host.taken.size() == 3);

    if (!sink.jobs.empty())
    {
        const Job& job = sink.jobs[0];
        CHECK(job.folder && job.folder() == L"cache\\folder");
        CHECK(job.name == FileName(ImageLibrary::NormalizeObjectPath("/Game/UI/T_Icon.T_Icon")));
        CHECK(!job.replace);

        const std::vector<uint8_t> file = job.make();
        Info info;
        const uint8_t* payload = nullptr;
        CHECK(Parse(file.data(), file.size(), info, payload) == Parsed::Ok);
        CHECK(info.path == "/Game/UI/T_Icon.T_Icon");
        CHECK(info.sourceFormat == 77 && info.sourceWidth == 512 && info.sourceHeight == 512);
        CHECK(info.storedWidth == 64 && info.storedHeight == 32);
        CHECK(payload && std::equal(pixels.begin(), pixels.end(), payload));
    }
}

static void TestSaverIgnoresWhatIsNotSaved()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // No ticket: the readback ring was full, the image stays in memory only.
    saver.Begin(MakeStarted(0));
    saver.Poll(0);
    CHECK(host.taken.empty() && sink.jobs.empty());

    // A readback that is gone ends without a file.
    host.entries[3].state = ImageDisk::Readback::Gone;
    saver.Begin(MakeStarted(3));
    saver.Poll(1);
    saver.Poll(2);
    CHECK(host.taken.size() == 1 && sink.jobs.empty());
}

static void TestSaverWithDiskOffOnlyFreesTheReadback()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    sink.off = true;
    host.entries[2].state = ImageDisk::Readback::Done;
    host.entries[2].pixels = MakePixels(64, 32, 1);
    saver.Begin(MakeStarted(2));
    saver.Poll(0);
    CHECK(host.taken.size() == 1 && !host.hadDest[0]);
    CHECK(sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 1);
}

static void TestSaverQueueFullDropsTheImage()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    sink.accept = false;
    host.entries[1].state = ImageDisk::Readback::Done;
    host.entries[1].pixels = MakePixels(64, 32, 1);
    saver.Begin(MakeStarted(1));
    saver.Poll(0);
    CHECK(sink.jobs.empty());
    saver.Poll(1);
    CHECK(host.taken.size() == 1);   // not kept waiting for room
}

static void TestSaverSetList()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // No set members: no file.
    saver.Poll(0);
    CHECK(sink.jobs.empty());

    const PluginImageId a = library.Path("/Game/UI/T_A.T_A");
    const PluginImageId b = library.Path("/Game/UI/T_B.T_B");
    const PluginImageId c = library.Path("/Game/UI/T_C.T_C");
    library.AddToSets(a, PLUGIN_IMAGE_SET_ITEMS);
    library.AddToSets(b, PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS);
    (void)c;   // in no set

    // Looked at once per interval, not per poll.
    saver.Poll(ImageDisk::kSetListIntervalMs - 1);
    CHECK(sink.jobs.empty());
    saver.Poll(ImageDisk::kSetListIntervalMs);
    CHECK(sink.jobs.size() == 1);

    if (!sink.jobs.empty())
    {
        const Job& job = sink.jobs[0];
        CHECK(job.name == SetListFileName());
        CHECK(job.replace);
        const std::vector<uint8_t> file = job.make();
        SetList sets;
        CHECK(ParseSets(file.data(), file.size(), sets) == Parsed::Ok);
        CHECK(sets.size() == 2);
        if (sets.size() == 2)
        {
            CHECK(sets[0].set == PLUGIN_IMAGE_SET_ITEMS && sets[0].paths.size() == 2);
            CHECK(sets[1].set == PLUGIN_IMAGE_SET_BUILDINGS && sets[1].paths.size() == 1);
            CHECK(sets[1].paths[0] == "/Game/UI/T_B.T_B");
        }
    }

    // Unchanged: not written again.
    saver.Poll(ImageDisk::kSetListIntervalMs * 2);
    CHECK(sink.jobs.size() == 1);

    // A new member: written again, at the next look.
    library.AddToSets(c, PLUGIN_IMAGE_SET_BUILDINGS);
    saver.Poll(ImageDisk::kSetListIntervalMs * 2 + 1);
    CHECK(sink.jobs.size() == 1);
    saver.Poll(ImageDisk::kSetListIntervalMs * 3);
    CHECK(sink.jobs.size() == 2);

    // A queue with no room: tried again at the next look.
    library.AddToSets(library.Path("/Game/UI/T_D.T_D"), PLUGIN_IMAGE_SET_ITEMS);
    sink.accept = false;
    saver.Poll(ImageDisk::kSetListIntervalMs * 4);
    CHECK(sink.jobs.size() == 2);
    sink.accept = true;
    saver.Poll(ImageDisk::kSetListIntervalMs * 5);
    CHECK(sink.jobs.size() == 3);
}

static void TestSaverStopAndAbandon()
{
    ImageLibrary::Library library;
    FakeReadbacks host;
    FakeSink sink;
    ImageDisk::DiskSaver saver(library, host, sink);

    // The GPU is gone: pending readbacks are dropped without being asked about.
    saver.Begin(MakeStarted(1));
    saver.Abandon();
    saver.Poll(0);
    CHECK(host.taken.empty());
    CHECK(sink.stops == 0);
    saver.Begin(MakeStarted(2));
    saver.Poll(1);
    CHECK(host.taken.empty());

    ImageDisk::DiskSaver other(library, host, sink);
    host.entries[4].state = ImageDisk::Readback::Done;
    host.entries[4].pixels = MakePixels(64, 32, 1);
    other.Begin(MakeStarted(4));
    other.Stop();
    CHECK(sink.stops == 1);
    other.Begin(MakeStarted(4));
    other.Poll(0);
    CHECK(host.taken.empty() && sink.jobs.empty());
}

// ---------------------------------------------------------------------------

int main()
{
    RunTest("crc32", TestCrc);
    RunTest("an image file reads back as it was written", TestRoundTrip);
    RunTest("every shape the shrink makes round-trips", TestRoundTripShapes);
    RunTest("a file that cannot be valid is not written", TestSerializeRefusals);
    RunTest("a file with the wrong magic is refused", TestBadMagic);
    RunTest("a file of another revision is refused", TestWrongRevision);
    RunTest("a truncated file is refused", TestTruncated);
    RunTest("a changed byte fails the crc", TestCrcMismatch);
    RunTest("a header that disagrees with itself is refused", TestBadHeader);
    RunTest("a set list reads back as it was written", TestSetListRoundTrip);
    RunTest("a bad set list is refused", TestSetListBad);
    RunTest("file names", TestFileName);
    RunTest("the folder changes when any input changes", TestFolderKey);
    RunTest("the pak set hash", TestPakHash);
    RunTest("the writer writes files and makes the folders", TestWriterWrites);
    RunTest("the writer leaves a file of the same size alone", TestWriterKeepsSameSize);
    RunTest("a job with no bytes writes nothing", TestWriterEmptyBytesWriteNothing);
    RunTest("a folder that cannot be made switches the disk part off once", TestWriterOffOnUnusableFolder);
    RunTest("a file that cannot be replaced switches the disk part off once", TestWriterOffOnReadOnlyFile);
    RunTest("the writer's queue is bounded", TestWriterQueueIsBounded);
    RunTest("the writer refuses a job without bytes or folder", TestWriterRefusesBadJobs);
    RunTest("nothing is written after stop", TestWriterStop);
    RunTest("stop before any job", TestWriterStopBeforeStart);
    RunTest("a finished readback becomes a file", TestSaverWritesFinishedCopy);
    RunTest("no ticket and a lost readback make no file", TestSaverIgnoresWhatIsNotSaved);
    RunTest("with the disk part off a readback is only freed", TestSaverWithDiskOffOnlyFreesTheReadback);
    RunTest("a full queue drops the image", TestSaverQueueFullDropsTheImage);
    RunTest("the set list is written when it changes", TestSaverSetList);
    RunTest("stop and a lost GPU end the saver", TestSaverStopAndAbandon);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
