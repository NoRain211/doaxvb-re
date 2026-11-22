/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_profile.h"

#include "sha.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static void write(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

static std::vector<std::uint8_t> read(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// A SaveMeta.xbx as the game writes it: UTF-16 with a byte order mark.
static std::vector<std::uint8_t> meta(const std::u16string &text)
{
    std::vector<std::uint8_t> bytes{0xffu, 0xfeu};
    for (const char16_t c : text) {
        bytes.push_back(static_cast<std::uint8_t>(c));
        bytes.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    return bytes;
}

// A blob with a correct hash, so only its entries decide.
struct Entry {
    std::uint8_t kind;
    std::string path;
    std::vector<std::uint8_t> bytes;
};
static std::vector<std::uint8_t> blobOf(const std::vector<Entry> &entries)
{
    std::vector<std::uint8_t> body;
    const auto put = [&body](std::size_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) body.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    };
    for (const Entry &entry : entries) {
        body.push_back(entry.kind);
        put(entry.path.size(), 2u);
        body.insert(body.end(), entry.path.begin(), entry.path.end());
        if (entry.kind != 1u) continue;
        put(entry.bytes.size(), 4u);
        body.insert(body.end(), entry.bytes.begin(), entry.bytes.end());
    }
    const std::string hash = doaxbv::sha256Hex(body);
    std::vector<std::uint8_t> blob(hash.begin(), hash.end());
    blob.insert(blob.end(), body.begin(), body.end());
    return blob;
}

static bool unpacks(const std::vector<std::uint8_t> &blob, const fs::path &root)
{
    std::vector<std::string> names;
    return recomp_netplay_unpack_profile(blob.data(), blob.size(), root, names);
}

int main()
{
    const auto root = fs::temp_directory_path() / ("recomp-netplay-profile-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path mine = root / "mine", theirs = root / "theirs";

    // Names: the Name= line wherever it is, without the mark, zero-width spaces or controls, as UTF-8.
    const std::vector<std::uint8_t> kasumi = meta(u"Title=DOA2U\r\nName=Ka\u200bsumi \u00e9\u4e00\r\n");
    check("save name", recomp_netplay_profile_name(kasumi.data(), kasumi.size()) == "Kasumi \xc3\xa9\xe4\xb8\x80");
    check("no name line", recomp_netplay_profile_name(kasumi.data(), 8u).empty());
    const std::vector<std::uint8_t> wide = meta(u"Name=\U0001F600" + std::u16string(1u, char16_t(0xd800u)) + u"a");
    check("surrogates", recomp_netplay_profile_name(wide.data(), wide.size()) == "\xf0\x9f\x98\x80\xef\xbf\xbd" "a");

    // A profile, a second one, title saves and a file beside the profiles. doaxuser.dat is 11,176 bytes in every save.
    const std::vector<std::uint8_t> data(11176u, 3u), title{4u}, beside{5u}, other(11176u, 6u);
    write(mine / "UDATA/54430007/0F515488395E/SaveMeta.xbx", kasumi);
    write(mine / "UDATA/54430007/0F515488395E/doaxuser.dat", data);
    write(mine / "UDATA/54430007/AAAAAAAAAAAA/SaveMeta.xbx", meta(u"Name=Other\r\n"));
    write(mine / "UDATA/54430007/AAAAAAAAAAAA/doaxuser.dat", other);
    write(mine / "UDATA/54430007/TitleMeta.xbx", beside);
    write(mine / "TDATA/54430007/title.dat", title);
    fs::last_write_time(mine / "UDATA/54430007/AAAAAAAAAAAA/doaxuser.dat",
        fs::last_write_time(mine / "UDATA/54430007/0F515488395E/doaxuser.dat") - std::chrono::hours(1));
    fs::last_write_time(mine / "UDATA/54430007/AAAAAAAAAAAA/SaveMeta.xbx",
        fs::last_write_time(mine / "UDATA/54430007/0F515488395E/doaxuser.dat") - std::chrono::hours(1));
    const std::string latest = recomp_netplay_latest_profile(mine);
    check("latest profile", latest == "UDATA/54430007/0F515488395E");
    check("own name", recomp_netplay_own_profile_name(mine) == "Kasumi \xc3\xa9\xe4\xb8\x80");

    // A host's blob carries its profile and the title saves; a joiner's only its profile.
    std::vector<std::uint8_t> host, joiner;
    check("pack host", recomp_netplay_pack_profile(mine, latest, true, host));
    check("pack joiner", recomp_netplay_pack_profile(mine, latest, false, joiner));
    std::vector<std::string> names;
    check("unpack host", recomp_netplay_unpack_profile(host.data(), host.size(), theirs / "host", names));
    check("host names", names == std::vector<std::string>{"Kasumi \xc3\xa9\xe4\xb8\x80"});
    check("host files", read(theirs / "host/UDATA/54430007/0F515488395E/doaxuser.dat") == data &&
        read(theirs / "host/TDATA/54430007/title.dat") == title &&
        read(theirs / "host/UDATA/54430007/TitleMeta.xbx") == beside &&
        !fs::exists(theirs / "host/UDATA/54430007/AAAAAAAAAAAA"));
    check("unpack joiner", unpacks(joiner, theirs / "joiner"));
    check("joiner files", fs::exists(theirs / "joiner/UDATA/54430007/0F515488395E/doaxuser.dat") &&
        !fs::exists(theirs / "joiner/TDATA") && !fs::exists(theirs / "joiner/UDATA/54430007/TitleMeta.xbx"));
    std::vector<std::uint8_t> none;
    check("pack no profile", recomp_netplay_pack_profile(root / "missing", "", false, none) && unpacks(none, theirs / "none"));
    // The host refuses to send what the joiner would refuse to unpack.
    write(root / "spaced/UDATA/54430007/a b", data);
    check("pack unsafe name", !recomp_netplay_pack_profile(root / "spaced", "", true, none));

    // Damaged blobs.
    if (host.size() > 64u) { // pack host above reports the failure
        std::vector<std::uint8_t> flipped = host;
        flipped.back() ^= 1u;
        check("changed byte", !unpacks(flipped, theirs / "flipped"));
        check("short", !unpacks(std::vector<std::uint8_t>(host.begin(), host.begin() + 63), theirs / "short"));
    }
    std::vector<std::uint8_t> cut = blobOf({{1u, "UDATA/a", data}});
    cut.erase(cut.end() - 1);
    const std::string body(cut.begin() + 64, cut.end());
    const std::string rehash = doaxbv::sha256Hex(std::vector<std::uint8_t>(body.begin(), body.end()));
    std::copy(rehash.begin(), rehash.end(), cut.begin());
    check("file cut short", !unpacks(cut, theirs / "cut"));
    check("unknown kind", !unpacks(blobOf({{2u, "UDATA/a", {}}}), theirs / "kind"));
    check("long path", !unpacks(blobOf({{0u, "UDATA/" + std::string(200u, 'a'), {}}}), theirs / "long"));
    std::vector<Entry> many(513u, Entry{0u, "UDATA/a", {}});
    check("too many entries", !unpacks(blobOf(many), theirs / "many"));

    // Paths that must never be written: outside UDATA and TDATA, not plain names, or Windows devices.
    const char *const unsafe[] = {"../escape", "UDATA/../../escape", "UDATA/./a", "UDATA//a", "/escape",
        "C:/escape", "UDATA\\..\\escape", "SYSTEM/a", "udata/a", "UDATA/a b", "UDATA/con", "UDATA/CON.txt",
        "UDATA/com1", "UDATA/LPT9.dat", "UDATA/a/", "", "UDATA/a:b"};
    for (const char *path : unsafe) {
        const std::string name = std::string("unsafe \"") + path + '"';
        check(name.c_str(), !unpacks(blobOf({{1u, path, data}}), theirs / "unsafe" / "x"));
    }
    check("nothing escaped", !fs::exists(theirs / "unsafe" / "escape") && !fs::exists(theirs / "escape") &&
        !fs::exists(root / "escape"));
    check("device-like names are fine", unpacks(blobOf({{1u, "UDATA/CONSOLE", data}, {1u, "UDATA/COM10", data}}),
        theirs / "names"));

    // A save folder holds only the game's files, so a crafted one never reaches its loader.
    const std::string save = "UDATA/54430007/0F515488395E";
    const std::vector<std::uint8_t> named = meta(u"Name=Vacation 1\r\nNoCopy=1\r\n");
    check("a game save", unpacks(blobOf({{0u, save, {}}, {1u, save + "/SaveMeta.xbx", named},
        {1u, save + "/DOAXUSER.DAT", data}}), theirs / "save"));
    std::vector<std::uint8_t> odd = named, big = meta(u"Name=A\r\n");
    odd.push_back(0u);
    big.resize(0x1002u);
    const std::pair<const char *, Entry> refused[] = {
        {"short doaxuser.dat", {1u, save + "/doaxuser.dat", std::vector<std::uint8_t>(11175u)}},
        {"long doaxuser.dat", {1u, save + "/doaxuser.dat", std::vector<std::uint8_t>(11177u)}},
        {"another file", {1u, save + "/SaveImage.xbx", beside}},
        {"a folder inside", {0u, save + "/sub", {}}},
        {"a file below a folder inside", {1u, save + "/sub/doaxuser.dat", data}},
        {"SaveMeta.xbx without a mark", {1u, save + "/SaveMeta.xbx", std::vector<std::uint8_t>(named.begin() + 2, named.end())}},
        {"SaveMeta.xbx of odd size", {1u, save + "/SaveMeta.xbx", odd}},
        {"SaveMeta.xbx over 4 KiB", {1u, save + "/SaveMeta.xbx", big}},
        {"SaveMeta.xbx without a name", {1u, save + "/SaveMeta.xbx", meta(u"NoCopy=1\r\n")}},
        {"SaveMeta.xbx with two names", {1u, save + "/SaveMeta.xbx", meta(u"NoCopy=1\r\nTitle=Name=B\r\nName=A\r\n")}},
    };
    for (const auto &[label, entry] : refused) check(label, !unpacks(blobOf({entry}), theirs / "refused"));
    check("nothing refused was written", !fs::exists(theirs / "refused" / save));

    std::error_code error;
    fs::remove_all(root, error);
    if (failures == 0) std::puts("netplay profile tests passed");
    return failures == 0 ? 0 : 1;
}
