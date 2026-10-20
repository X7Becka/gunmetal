// SML/SME UI markup: C++ reader parity with ``gm/sml.py``.
//
// Golden data: all 300 shipped ``SML/**/*.sml|*.sme`` files (149 plain, 151
// encoded).  Every row pins the decode note, tag/element inventory and FNV-1a64
// of the decoded markup, the tag-stripped ``plain`` text and ``describe.body``;
// the aggregate block pins the corpus findings from docs/UI.md (147 pairs,
// 131 identical, 14 revisions, 2 line-ending repairs, 72 images / 15 unique,
// 268 fonts with s=2/3, 56 music tags).
//
// Corpus is read-only: the game install is never written.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "res/sml.h"
#include "res/text.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void check_eq(std::size_t got, std::size_t want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got " << got << ", want " << want << ")\n";
    }
}

void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << " (got \"" << got << "\", want \"" << want
                  << "\")\n";
    }
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::string read_text(const fs::path& path) {
    const auto bytes = read_file(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string normalize(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

bool ends_with(std::string_view text, std::string_view tail) {
    return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

bool starts_with(std::string_view text, std::string_view head) {
    return text.size() >= head.size() && text.substr(0, head.size()) == head;
}

struct GoldenFile {
    const char* path;
    std::uint32_t bytes;
    bool encoded;
    const char* note;
    std::uint32_t chars;
    std::uint32_t lines;
    std::uint32_t tags;
    std::uint32_t images;
    std::uint32_t fonts;
    std::uint32_t music;
    std::uint32_t text_runs;
    const char* unknown; // comma separated, nullptr when empty
    bool ok;
    bool balanced;
    std::uint64_t markup_fnv;
    std::uint64_t plain_fnv;
    std::uint64_t body_fnv;
};

const GoldenFile kGolden[] = {
    {"brief/LEVEL0A.SME", 968, 1, "as-is", 968, 2, 109, 0, 0, 0, 37, nullptr, 1, 1, 0xbac2b3adbd8ac5d1ull, 0xb8d157e214b6e886ull, 0x872aae9e8dd45828ull},
    {"brief/LEVEL1.SME", 1056, 1, "as-is", 1056, 1, 19, 2, 3, 1, 8, nullptr, 1, 1, 0xfb9b0717b5104bdeull, 0x16ff8024bf7e85e4ull, 0x99fea978566fcbdbull},
    {"brief/LEVEL11.SME", 1570, 1, "as-is", 1570, 1, 19, 1, 4, 1, 8, nullptr, 1, 1, 0x073b61a0e248cc1aull, 0x95f5b78dee3cfc34ull, 0x0a5cddf2b182ad69ull},
    {"brief/LEVEL12.SME", 2460, 1, "as-is", 2460, 1, 27, 3, 5, 2, 12, nullptr, 1, 1, 0x390e6fe82779f985ull, 0x5d3f2a2f0bc5fe6bull, 0x6c8c8444f63ccdb5ull},
    {"brief/LEVEL13.SME", 618, 1, "as-is", 618, 1, 20, 1, 4, 2, 8, nullptr, 1, 1, 0x7137fa3a2bc925b0ull, 0xab5ea2e5b40b7ec4ull, 0xfee33f8b7ec23c85ull},
    {"brief/LEVEL14.SME", 1082, 1, "as-is", 1082, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0xbaf7a23eb1ddd3b7ull, 0x496432b8d2a1f31cull, 0xcd051097ee2ac04cull},
    {"brief/LEVEL15.SME", 1568, 1, "as-is", 1568, 1, 18, 2, 3, 1, 8, nullptr, 1, 1, 0x766f986bf41056aeull, 0xf5fc7e1655cb8b40ull, 0xbfeff395917c3479ull},
    {"brief/LEVEL16.SME", 1895, 1, "as-is", 1895, 1, 20, 1, 4, 2, 8, nullptr, 1, 1, 0xf20107bffd6aa147ull, 0x7aabe39c2896689bull, 0x29c8bc2f23427babull},
    {"brief/LEVEL18.SME", 1101, 1, "as-is", 1101, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0x982d31fbb5b9607full, 0x063bfdebf4ee7b2cull, 0x5cdb2e53b8f0c29bull},
    {"brief/LEVEL19.SME", 436, 1, "as-is", 436, 1, 14, 1, 2, 1, 7, nullptr, 1, 1, 0xb3bbeb60327a83e2ull, 0x5025e09dffd85ddeull, 0x9ffcfbbe35dba6b5ull},
    {"brief/LEVEL2.SME", 893, 1, "as-is", 893, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0x799d759bb0dec7feull, 0x447531bb7bca372dull, 0x256e6e0c6221657dull},
    {"brief/LEVEL20.SME", 1623, 1, "as-is", 1623, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0x513e64ac8c21be8dull, 0xc12fdb77b35a7978ull, 0x3fae3df4587011d3ull},
    {"brief/LEVEL3.SME", 1186, 1, "as-is", 1186, 1, 18, 2, 3, 1, 9, nullptr, 1, 1, 0x680b17ab6dc9d478ull, 0xa558b4a020d9cf70ull, 0xb98ddcd4d0e7d2ccull},
    {"brief/LEVEL4.SME", 1829, 1, "as-is", 1829, 1, 28, 2, 5, 1, 14, nullptr, 1, 1, 0x11a5416aac85e8dfull, 0xd2b4f7cb5a65f04full, 0x888cd68eba473b15ull},
    {"brief/LEVEL6.SME", 1526, 1, "as-is", 1526, 1, 17, 2, 3, 1, 8, nullptr, 1, 1, 0x63cf48e1f869d82aull, 0x81e3054a82f4efadull, 0x07d8a8a4ad3eda2dull},
    {"brief/Level24.sme", 1285, 1, "as-is", 1285, 1, 20, 1, 3, 1, 9, nullptr, 1, 1, 0x2832d5bf30061c21ull, 0x22cff20eb7a7751full, 0x49612ac6e2eb0428ull},
    {"brief/level1.sml", 1056, 0, "plain", 1056, 1, 19, 2, 3, 1, 8, nullptr, 1, 1, 0xfb9b0717b5104bdeull, 0x16ff8024bf7e85e4ull, 0x99fea978566fcbdbull},
    {"brief/level10.sme", 1817, 1, "as-is", 1817, 1, 18, 2, 3, 1, 8, nullptr, 1, 1, 0x45504d27bbbc614cull, 0x69c9559d6ef32957ull, 0x2c460c72e97e6589ull},
    {"brief/level10.sml", 1735, 0, "plain", 1735, 1, 18, 2, 3, 1, 8, nullptr, 1, 1, 0x00c23ab6f60e47cbull, 0x762887f7b3a908a0ull, 0x6466c2d655e93972ull},
    {"brief/level11.sml", 1570, 0, "plain", 1570, 1, 19, 1, 4, 1, 8, nullptr, 1, 1, 0x073b61a0e248cc1aull, 0x95f5b78dee3cfc34ull, 0x0a5cddf2b182ad69ull},
    {"brief/level12.sml", 2460, 0, "plain", 2460, 1, 27, 3, 5, 2, 12, nullptr, 1, 1, 0x390e6fe82779f985ull, 0x5d3f2a2f0bc5fe6bull, 0x6c8c8444f63ccdb5ull},
    {"brief/level13.sml", 618, 0, "plain", 618, 1, 20, 1, 4, 2, 8, nullptr, 1, 1, 0x7137fa3a2bc925b0ull, 0xab5ea2e5b40b7ec4ull, 0xfee33f8b7ec23c85ull},
    {"brief/level14.sml", 1082, 0, "plain", 1082, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0xbaf7a23eb1ddd3b7ull, 0x496432b8d2a1f31cull, 0xcd051097ee2ac04cull},
    {"brief/level15.sml", 1568, 0, "plain", 1568, 1, 18, 2, 3, 1, 8, nullptr, 1, 1, 0x766f986bf41056aeull, 0xf5fc7e1655cb8b40ull, 0xbfeff395917c3479ull},
    {"brief/level16.sml", 1895, 0, "plain", 1895, 1, 20, 1, 4, 2, 8, nullptr, 1, 1, 0xf20107bffd6aa147ull, 0x7aabe39c2896689bull, 0x29c8bc2f23427babull},
    {"brief/level17.sme", 1582, 1, "newline-repair", 1581, 1, 18, 1, 3, 1, 9, nullptr, 1, 1, 0x811d8138f989eda0ull, 0x554089cf7faaef50ull, 0xf1f14ee086c28ee6ull},
    {"brief/level17.sml", 1483, 0, "plain", 1483, 1, 18, 1, 3, 1, 9, nullptr, 1, 1, 0x903b0eaaba305395ull, 0x682176caaa2d90e7ull, 0x8cca2462a11f5c43ull},
    {"brief/level18.sml", 1101, 0, "plain", 1101, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0x982d31fbb5b9607full, 0x063bfdebf4ee7b2cull, 0x5cdb2e53b8f0c29bull},
    {"brief/level19.sml", 436, 0, "plain", 436, 1, 14, 1, 2, 1, 7, nullptr, 1, 1, 0xb3bbeb60327a83e2ull, 0x5025e09dffd85ddeull, 0x9ffcfbbe35dba6b5ull},
    {"brief/level2.sml", 893, 0, "plain", 893, 1, 16, 1, 3, 1, 7, nullptr, 1, 1, 0x799d759bb0dec7feull, 0x447531bb7bca372dull, 0x256e6e0c6221657dull},
    {"brief/level20.sml", 1623, 0, "plain", 1623, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0x513e64ac8c21be8dull, 0xc12fdb77b35a7978ull, 0x3fae3df4587011d3ull},
    {"brief/level21.sme", 1595, 1, "as-is", 1595, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0x8ad80d185cf2f6c7ull, 0x1ce4f17332c2afe9ull, 0x3c0a77bd03d643a8ull},
    {"brief/level21.sml", 1595, 0, "plain", 1595, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0x8ad80d185cf2f6c7ull, 0x1ce4f17332c2afe9ull, 0x3c0a77bd03d643a8ull},
    {"brief/level22.sme", 3865, 1, "as-is", 3865, 1, 28, 1, 5, 2, 14, nullptr, 1, 1, 0xd82763e90196d4e1ull, 0x9efc80d0b3d994e1ull, 0x4283dd3dfc4d9786ull},
    {"brief/level22.sml", 3865, 0, "plain", 3865, 1, 28, 1, 5, 2, 14, nullptr, 1, 1, 0xd82763e90196d4e1ull, 0x9efc80d0b3d994e1ull, 0x4283dd3dfc4d9786ull},
    {"brief/level23.sme", 1295, 1, "as-is", 1295, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0x77bb5481153eb528ull, 0xee550bb0079f7fc1ull, 0x98a7a03efaa6a6b4ull},
    {"brief/level23.sml", 1330, 0, "plain", 1330, 1, 18, 2, 3, 1, 9, nullptr, 1, 1, 0x6063be0d66a2b305ull, 0x11d40a96c808f018ull, 0x98a7a03efaa6a6b4ull},
    {"brief/level24.sml", 1285, 0, "plain", 1285, 1, 20, 1, 3, 1, 9, nullptr, 1, 1, 0x2832d5bf30061c21ull, 0x22cff20eb7a7751full, 0x49612ac6e2eb0428ull},
    {"brief/level3.sml", 1186, 0, "plain", 1186, 1, 18, 2, 3, 1, 9, nullptr, 1, 1, 0x680b17ab6dc9d478ull, 0xa558b4a020d9cf70ull, 0xb98ddcd4d0e7d2ccull},
    {"brief/level4.sml", 1829, 0, "plain", 1829, 1, 28, 2, 5, 1, 14, nullptr, 1, 1, 0x11a5416aac85e8dfull, 0xd2b4f7cb5a65f04full, 0x888cd68eba473b15ull},
    {"brief/level5.sme", 1449, 1, "as-is", 1449, 1, 18, 2, 3, 1, 9, nullptr, 1, 1, 0x2b242a16c099607bull, 0x7068d2f58e0413fbull, 0x8daf6a292710e48cull},
    {"brief/level5.sml", 1449, 0, "plain", 1449, 1, 18, 2, 3, 1, 8, nullptr, 1, 1, 0xf8ec238f7f025acfull, 0xbb73e32a6ddc0e63ull, 0x8daf6a292710e48cull},
    {"brief/level6.sml", 1526, 0, "plain", 1526, 1, 17, 2, 3, 1, 8, nullptr, 1, 1, 0x63cf48e1f869d82aull, 0x81e3054a82f4efadull, 0x07d8a8a4ad3eda2dull},
    {"brief/level7.sme", 1149, 1, "as-is", 1149, 1, 18, 2, 3, 1, 7, nullptr, 1, 1, 0xe917b848b346ae75ull, 0x1d0ddfe5c9bd5062ull, 0x72dd17c2df5b561dull},
    {"brief/level7.sml", 1149, 0, "plain", 1149, 1, 18, 2, 3, 1, 7, nullptr, 1, 1, 0x73757e477844f464ull, 0xc4d686b43b0d8315ull, 0xd7df5ca2b041eeaeull},
    {"brief/level8.sme", 1686, 1, "as-is", 1686, 1, 20, 1, 4, 1, 9, nullptr, 1, 1, 0xc5f2d0c0fbc9a340ull, 0x8a462e77667040b5ull, 0x0b54463be1b87ba0ull},
    {"brief/level8.sml", 1255, 0, "plain", 1255, 1, 17, 1, 3, 1, 8, nullptr, 1, 1, 0xb7bbf4a0ed01ca8dull, 0x0dc0ae44e16408bdull, 0xd25772763e34a3b8ull},
    {"brief/level9.sme", 1445, 1, "as-is", 1445, 1, 18, 3, 3, 1, 9, nullptr, 1, 1, 0xf71eeadb863cbd0full, 0xbd02fdf6d1c07694ull, 0x1b1b771de91cfcd7ull},
    {"brief/level9.sml", 1409, 0, "plain", 1409, 1, 17, 2, 3, 1, 8, nullptr, 1, 1, 0xa8b2b0f5b6dc3a3eull, 0x9e2de12857666279ull, 0x1b1b771de91cfcd7ull},
    {"credits.sme", 1499, 1, "as-is", 1499, 1, 147, 0, 0, 0, 65, nullptr, 1, 1, 0x259656d299259b50ull, 0x89150f905e405962ull, 0xbb317c8dfd1f4a3eull},
    {"credits.sml", 1470, 0, "plain", 1470, 1, 145, 0, 0, 0, 64, nullptr, 1, 1, 0xc8b07f2deca4184dull, 0x341e9ced3ffb0b22ull, 0x10723ca3c0f735f0ull},
    {"design/AMMO0.SME", 89, 1, "as-is", 89, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x58baa94a20c5dea4ull, 0x0a367a3440d9d903ull, 0x1fd28967bf6a0c32ull},
    {"design/EQUIP0.SME", 163, 1, "as-is", 163, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x563b266037ea947bull, 0x84597013436f641eull, 0x7658fa80322f9f77ull},
    {"design/EQUIP1.SME", 190, 1, "as-is", 190, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x306a8288cc509aeaull, 0x31822f4c8a9a6d61ull, 0x38eaaceac0d80b92ull},
    {"design/SHIELD0.sml", 86, 0, "plain", 86, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x2da227ef43347cd6ull, 0x1421f3a8caa893a7ull, 0xee350fbfc4bb3014ull},
    {"design/TANK0.SME", 145, 1, "as-is", 145, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x2d4e3e3213a399edull, 0xb6a3eea42435c6f6ull, 0x470ae17e140e7d73ull},
    {"design/TANK1.SME", 183, 1, "as-is", 183, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9dd2597be9335c69ull, 0x21d1310024a8c43cull, 0x79ba524d6ada1671ull},
    {"design/TANK2.SME", 216, 1, "as-is", 216, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xd312c601858342c8ull, 0x89b71a036779eaadull, 0xf296422686ea0512ull},
    {"design/WEAPON0.SME", 194, 1, "as-is", 194, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xfc1712ae847b2bc3ull, 0xd7454af49cf38b62ull, 0x3ef3ef98e7787229ull},
    {"design/WEAPON0.SML", 194, 0, "plain", 194, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xfc1712ae847b2bc3ull, 0xd7454af49cf38b62ull, 0x3ef3ef98e7787229ull},
    {"design/WEAPON1.SME", 192, 1, "as-is", 192, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x0cc10598c4b27371ull, 0x70357b3df06615eeull, 0xdfc4d6cae8ae8ef9ull},
    {"design/WEAPON1.SML", 192, 0, "plain", 192, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x0cc10598c4b27371ull, 0x70357b3df06615eeull, 0xdfc4d6cae8ae8ef9ull},
    {"design/WEAPON10.SME", 182, 1, "as-is", 182, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x04c43cb7ee9b076eull, 0xd8167ee63cd7704full, 0x2e877e0d631a0180ull},
    {"design/WEAPON10.sml", 182, 0, "plain", 182, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x04c43cb7ee9b076eull, 0xd8167ee63cd7704full, 0x2e877e0d631a0180ull},
    {"design/WEAPON11.SME", 144, 1, "as-is", 144, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9879fc4284869ae1ull, 0x5f356cfffc24bb48ull, 0xc6250a7dbd6c3d1full},
    {"design/WEAPON11.sml", 144, 0, "plain", 144, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9879fc4284869ae1ull, 0x5f356cfffc24bb48ull, 0xc6250a7dbd6c3d1full},
    {"design/WEAPON12.SME", 203, 1, "as-is", 203, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x63a9e5976c2354a4ull, 0x4aa48f1bc0df1857ull, 0x1541fc9f8e796586ull},
    {"design/WEAPON12.sml", 203, 0, "plain", 203, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x63a9e5976c2354a4ull, 0x4aa48f1bc0df1857ull, 0x1541fc9f8e796586ull},
    {"design/WEAPON13.SME", 202, 1, "as-is", 202, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x913c5b19b43abbdcull, 0xa6610ebdd55af621ull, 0x2ba2e4472cabd8b2ull},
    {"design/WEAPON13.sml", 202, 0, "plain", 202, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x913c5b19b43abbdcull, 0xa6610ebdd55af621ull, 0x2ba2e4472cabd8b2ull},
    {"design/WEAPON14.SME", 189, 1, "as-is", 189, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x1e614407b9db8aeeull, 0xea0980fa83423625ull, 0x00bbcf3832d19a10ull},
    {"design/WEAPON14.sml", 189, 0, "plain", 189, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x1e614407b9db8aeeull, 0xea0980fa83423625ull, 0x00bbcf3832d19a10ull},
    {"design/WEAPON15.SME", 181, 1, "as-is", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x1d51583c06901046ull, 0x5daa0a00a3053389ull, 0x142e976b2b903b40ull},
    {"design/WEAPON15.sml", 181, 0, "plain", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x1d51583c06901046ull, 0x5daa0a00a3053389ull, 0x142e976b2b903b40ull},
    {"design/WEAPON16.SME", 180, 1, "as-is", 180, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x122d9d565adb2e59ull, 0x6701491ea2fa5b3aull, 0x7dd93de37e0bc309ull},
    {"design/WEAPON16.sml", 180, 0, "plain", 180, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x122d9d565adb2e59ull, 0x6701491ea2fa5b3aull, 0x7dd93de37e0bc309ull},
    {"design/WEAPON17.sml", 215, 0, "plain", 215, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9d0b0c1080b60460ull, 0x5180e1701244e077ull, 0xa9afd410a12f75e2ull},
    {"design/WEAPON18.sml", 203, 0, "plain", 203, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xad583bd7a6b6c226ull, 0x84db86f705282d95ull, 0x159535f1b351c628ull},
    {"design/WEAPON19.SML", 216, 0, "plain", 216, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xa178b4076f2db3eeull, 0x189688ab8dd37aefull, 0x127654a219718f2cull},
    {"design/WEAPON2.SME", 182, 1, "as-is", 182, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x89029e2efa5d7747ull, 0xe19ada4520916650ull, 0xfdc89927374ce18full},
    {"design/WEAPON2.SML", 182, 0, "plain", 182, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x89029e2efa5d7747ull, 0xe19ada4520916650ull, 0xfdc89927374ce18full},
    {"design/WEAPON20.SML", 68, 0, "plain", 68, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x07e12a4df5b2251eull, 0xfa3fd279520d86d9ull, 0x443e7b9ee0449bc6ull},
    {"design/WEAPON3.SME", 197, 1, "as-is", 197, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9a90fcae73dd48f1ull, 0x80596f9b1701945eull, 0x1f57864331b97dc3ull},
    {"design/WEAPON3.SML", 197, 0, "plain", 197, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9a90fcae73dd48f1ull, 0x80596f9b1701945eull, 0x1f57864331b97dc3ull},
    {"design/WEAPON4.SME", 176, 1, "as-is", 176, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xeb249a75ac993aaeull, 0x2fcfa0b36c1ac68bull, 0x526ba72acd51cbf8ull},
    {"design/WEAPON4.SML", 176, 0, "plain", 176, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xeb249a75ac993aaeull, 0x2fcfa0b36c1ac68bull, 0x526ba72acd51cbf8ull},
    {"design/WEAPON5.SME", 176, 1, "as-is", 176, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x25d19d0363f06180ull, 0xf0b8a08c4448be2dull, 0x83b295f76415106eull},
    {"design/WEAPON5.sml", 176, 0, "plain", 176, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x25d19d0363f06180ull, 0xf0b8a08c4448be2dull, 0x83b295f76415106eull},
    {"design/WEAPON6.SME", 208, 1, "as-is", 208, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x66d9f66821338642ull, 0x7b042e3077cdeea5ull, 0xd94887afd4f8ccfaull},
    {"design/WEAPON6.SML", 208, 0, "plain", 208, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x66d9f66821338642ull, 0x7b042e3077cdeea5ull, 0xd94887afd4f8ccfaull},
    {"design/WEAPON7.SME", 181, 1, "as-is", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x8d1fe24f381e724dull, 0x0a26a33eb99485a6ull, 0x15be69162fd7c8bfull},
    {"design/WEAPON7.SML", 181, 0, "plain", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x8d1fe24f381e724dull, 0x0a26a33eb99485a6ull, 0x15be69162fd7c8bfull},
    {"design/WEAPON8.SME", 183, 1, "as-is", 183, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xbfe7b3adb5a06b4dull, 0xd487fbc7064b5f98ull, 0x6c2a8d1f09b90b71ull},
    {"design/WEAPON8.sml", 183, 0, "plain", 183, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xbfe7b3adb5a06b4dull, 0xd487fbc7064b5f98ull, 0x6c2a8d1f09b90b71ull},
    {"design/ammo0.sml", 89, 0, "plain", 89, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x58baa94a20c5dea4ull, 0x0a367a3440d9d903ull, 0x1fd28967bf6a0c32ull},
    {"design/ammo1.sme", 116, 1, "as-is", 116, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xf1fa3f74ce587f1bull, 0x2fb36c59937939faull, 0x18e18d3221bb1c81ull},
    {"design/ammo1.sml", 116, 0, "plain", 116, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xf1fa3f74ce587f1bull, 0x2fb36c59937939faull, 0x18e18d3221bb1c81ull},
    {"design/ammo10.sme", 210, 1, "as-is", 210, 2, 7, 0, 1, 0, 3, nullptr, 1, 1, 0x76ea8114dec70761ull, 0xcedca4cfde00ea36ull, 0x5b00301763564eebull},
    {"design/ammo10.sml", 210, 0, "plain", 210, 2, 7, 0, 1, 0, 3, nullptr, 1, 1, 0x76ea8114dec70761ull, 0xcedca4cfde00ea36ull, 0x5b00301763564eebull},
    {"design/ammo11.sme", 152, 1, "as-is", 152, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xff0972fa482d106bull, 0xbf53abb906ec13e0ull, 0x64fb9fe3b8666811ull},
    {"design/ammo11.sml", 152, 0, "plain", 152, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xff0972fa482d106bull, 0xbf53abb906ec13e0ull, 0x64fb9fe3b8666811ull},
    {"design/ammo2.sme", 181, 1, "as-is", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xf7cb18c73842cb19ull, 0x9f197627906bf7b0ull, 0x9f80c19a55d32589ull},
    {"design/ammo2.sml", 181, 0, "plain", 181, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xf7cb18c73842cb19ull, 0x9f197627906bf7b0ull, 0x9f80c19a55d32589ull},
    {"design/ammo3.sme", 137, 1, "as-is", 137, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xef59a45c8a736640ull, 0x89f122bf9d9c443dull, 0x47934e9503e76dc8ull},
    {"design/ammo3.sml", 137, 0, "plain", 137, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xef59a45c8a736640ull, 0x89f122bf9d9c443dull, 0x47934e9503e76dc8ull},
    {"design/ammo4.sme", 148, 1, "as-is", 148, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x105a7698d88f3fbbull, 0x8291b778390cf7b0ull, 0xed229a34b76acc7full},
    {"design/ammo4.sml", 148, 0, "plain", 148, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x105a7698d88f3fbbull, 0x8291b778390cf7b0ull, 0xed229a34b76acc7full},
    {"design/ammo5.sme", 63, 1, "as-is", 63, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xaff98196f81094d1ull, 0xd082d68fd7f47d38ull, 0xf571605a8fc6c1e1ull},
    {"design/ammo5.sml", 63, 0, "plain", 63, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xaff98196f81094d1ull, 0xd082d68fd7f47d38ull, 0xf571605a8fc6c1e1ull},
    {"design/ammo6.sme", 128, 1, "as-is", 128, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xa3dad8eb5fc06b45ull, 0x6e3bf9df3a585d12ull, 0x5460254b3e6064c9ull},
    {"design/ammo6.sml", 128, 0, "plain", 128, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xa3dad8eb5fc06b45ull, 0x6e3bf9df3a585d12ull, 0x5460254b3e6064c9ull},
    {"design/ammo7.sme", 202, 1, "as-is", 202, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x5a96e68ceb7546d6ull, 0x19a58f7e76aa63dbull, 0xa32b9f0e8aa88a7cull},
    {"design/ammo7.sml", 202, 0, "plain", 202, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x5a96e68ceb7546d6ull, 0x19a58f7e76aa63dbull, 0xa32b9f0e8aa88a7cull},
    {"design/ammo8.sme", 234, 1, "as-is", 234, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x4df8ced1b0420847ull, 0xbd925770eaf7fa74ull, 0xe49e8ff697a43723ull},
    {"design/ammo8.sml", 234, 0, "plain", 234, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x4df8ced1b0420847ull, 0xbd925770eaf7fa74ull, 0xe49e8ff697a43723ull},
    {"design/ammo9.sme", 67, 1, "as-is", 67, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x01712ad0fbdbe77full, 0x51d9a56b86f36ab0ull, 0x0a4a766a79d2515dull},
    {"design/ammo9.sml", 67, 0, "plain", 67, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x01712ad0fbdbe77full, 0x51d9a56b86f36ab0ull, 0x0a4a766a79d2515dull},
    {"design/equip0.sml", 163, 0, "plain", 163, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x563b266037ea947bull, 0x84597013436f641eull, 0x7658fa80322f9f77ull},
    {"design/equip1.sml", 190, 0, "plain", 190, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x306a8288cc509aeaull, 0x31822f4c8a9a6d61ull, 0x38eaaceac0d80b92ull},
    {"design/equip2.sme", 107, 1, "as-is", 107, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x3af5b94d3414e6f8ull, 0xc73644487a3c2d97ull, 0xe187191f97a647c2ull},
    {"design/equip2.sml", 107, 0, "plain", 107, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x3af5b94d3414e6f8ull, 0xc73644487a3c2d97ull, 0xe187191f97a647c2ull},
    {"design/equip3.sme", 156, 1, "as-is", 156, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9bacc83ac50366bdull, 0xed13d5d4211eebacull, 0xedb6fb283e9b16fbull},
    {"design/equip3.sml", 156, 0, "plain", 156, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9bacc83ac50366bdull, 0xed13d5d4211eebacull, 0xedb6fb283e9b16fbull},
    {"design/equip4.sme", 129, 1, "as-is", 129, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x92532c026bcecd90ull, 0x918671dc3d24b5cbull, 0x300a8c4064a1954eull},
    {"design/equip4.sml", 129, 0, "plain", 129, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x92532c026bcecd90ull, 0x918671dc3d24b5cbull, 0x300a8c4064a1954eull},
    {"design/equip5.sme", 162, 1, "as-is", 162, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x158fbf46ee063f2full, 0x15dc5787ba879a8eull, 0x5cf663133c977535ull},
    {"design/equip5.sml", 162, 0, "plain", 162, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x158fbf46ee063f2full, 0x15dc5787ba879a8eull, 0x5cf663133c977535ull},
    {"design/equip6.sme", 205, 1, "as-is", 205, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xee175a53f8c5e0b7ull, 0xefee981c63c7f558ull, 0x37e1fc9254231299ull},
    {"design/equip6.sml", 205, 0, "plain", 205, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xee175a53f8c5e0b7ull, 0xefee981c63c7f558ull, 0x37e1fc9254231299ull},
    {"design/equip7.sme", 114, 1, "as-is", 114, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x43aa95f9a26f859cull, 0x770fda3827cd092full, 0x783867f6119a0820ull},
    {"design/equip7.sml", 114, 0, "plain", 114, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x43aa95f9a26f859cull, 0x770fda3827cd092full, 0x783867f6119a0820ull},
    {"design/equip8.sme", 114, 1, "as-is", 114, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xb76139d51aeff677ull, 0x3e9c04bc5549cbd0ull, 0xd954104021feda7full},
    {"design/equip8.sml", 114, 0, "plain", 114, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xb76139d51aeff677ull, 0x3e9c04bc5549cbd0ull, 0xd954104021feda7full},
    {"design/shield0.sme", 86, 1, "as-is", 86, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x2da227ef43347cd6ull, 0x1421f3a8caa893a7ull, 0xee350fbfc4bb3014ull},
    {"design/tank0.sml", 145, 0, "plain", 145, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x2d4e3e3213a399edull, 0xb6a3eea42435c6f6ull, 0x470ae17e140e7d73ull},
    {"design/tank1.sml", 183, 0, "plain", 183, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9dd2597be9335c69ull, 0x21d1310024a8c43cull, 0x79ba524d6ada1671ull},
    {"design/tank2.sml", 216, 0, "plain", 216, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xd312c601858342c8ull, 0x89b71a036779eaadull, 0xf296422686ea0512ull},
    {"design/tank3.sme", 165, 1, "as-is", 165, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x3d7235ba58b2a143ull, 0xfebf976a627f2c7cull, 0xe786488256b14035ull},
    {"design/tank3.sml", 165, 0, "plain", 165, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x3d7235ba58b2a143ull, 0xfebf976a627f2c7cull, 0xe786488256b14035ull},
    {"design/temp0.sme", 45, 1, "as-is", 45, 1, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xa39dbd59bdb1906aull, 0xce746542e1bf8583ull, 0xf812fb2af87d6e38ull},
    {"design/temp0.sml", 51, 0, "plain", 51, 1, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xb74c41a839de7f1cull, 0xe9aea1e3c157347dull, 0xfcd13738fe459432ull},
    {"design/weapon17.sme", 215, 1, "as-is", 215, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x9d0b0c1080b60460ull, 0x5180e1701244e077ull, 0xa9afd410a12f75e2ull},
    {"design/weapon18.sme", 203, 1, "as-is", 203, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xad583bd7a6b6c226ull, 0x84db86f705282d95ull, 0x159535f1b351c628ull},
    {"design/weapon19.sme", 216, 1, "as-is", 216, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0xa178b4076f2db3eeull, 0x189688ab8dd37aefull, 0x127654a219718f2cull},
    {"design/weapon20.sme", 68, 1, "as-is", 68, 2, 6, 0, 1, 0, 2, nullptr, 1, 1, 0x07e12a4df5b2251eull, 0xfa3fd279520d86d9ull, 0x443e7b9ee0449bc6ull},
    {"help.sme", 1049, 1, "as-is", 1049, 2, 90, 0, 0, 0, 42, nullptr, 1, 1, 0xfe0162f507ef4c49ull, 0x5a7ed0541790d62full, 0x32cb4c7ba4bd9689ull},
    {"help.sml", 807, 0, "plain", 807, 2, 63, 0, 0, 0, 29, nullptr, 1, 1, 0xcfb771af6e42d359ull, 0xb891b88c74e624f6ull, 0x44d366e92144680aull},
    {"prelevel/DM1.SME", 385, 1, "as-is", 385, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x286507559e5d30beull, 0x0c3a1c3cd5b3da2eull, 0x0a236515d806ef8cull},
    {"prelevel/DM2.SME", 279, 1, "as-is", 279, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x2e91f101c657d8f8ull, 0x9a4d02af73f93364ull, 0xf9230506bff0e37aull},
    {"prelevel/DM3.SME", 370, 1, "as-is", 370, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x43446d97e9b0303full, 0x31be2941f2641187ull, 0xe43f1f61396d9a91ull},
    {"prelevel/LEVEL0.SME", 652, 1, "as-is", 652, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xb3b2422efde799abull, 0x5e0b7f26645bdd11ull, 0xa4abaa6f2aecc457ull},
    {"prelevel/LEVEL1.SME", 353, 1, "as-is", 353, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x135dc5b62ff4214cull, 0xd1abdccf01d50b1bull, 0xb3434b37ea3b0c4bull},
    {"prelevel/LEVEL10.SME", 602, 1, "as-is", 602, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x4310398804724794ull, 0x27690fa3bde84e6bull, 0x2bbb72fe2bbd351dull},
    {"prelevel/LEVEL11.SME", 680, 1, "as-is", 680, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x23eff91cd27bbf11ull, 0x3dcf34f729c56b52ull, 0x227a2e2d287e2b68ull},
    {"prelevel/LEVEL12.SME", 680, 1, "as-is", 680, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xda321310ecea108bull, 0xd45c84f1175f22d4ull, 0x3c90e699837dda8eull},
    {"prelevel/LEVEL13.SME", 714, 1, "as-is", 714, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x7ce7913e1df5eb8bull, 0x44ded1f544432364ull, 0x62ee46c0ea06f11eull},
    {"prelevel/LEVEL14.SME", 665, 1, "as-is", 665, 1, 6, 0, 1, 0, 3, nullptr, 1, 1, 0xb770faa82dc856b5ull, 0xed420352b0c665c1ull, 0xb55db0feffdaa2edull},
    {"prelevel/LEVEL15.SME", 692, 1, "as-is", 692, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xc6fbefbc47b5f412ull, 0xaaac022de310f7a1ull, 0x0b45f1ead2d0c8fbull},
    {"prelevel/LEVEL16.SME", 726, 1, "as-is", 726, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x12a9b87d1499cd6aull, 0x8a867c941a7d54a1ull, 0xf8abf517451420c3ull},
    {"prelevel/LEVEL17.SME", 701, 1, "as-is", 701, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x9e9110fe4f217993ull, 0x5a4f52c410496664ull, 0x6064bc0b2af3ee76ull},
    {"prelevel/LEVEL18.SME", 715, 1, "as-is", 715, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe2985490075d30b3ull, 0xff3592d27ec6c9a4ull, 0x5f2a0da23e605586ull},
    {"prelevel/LEVEL19.SME", 162, 1, "as-is", 162, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x44904e4cb5098740ull, 0xe419a026e70dfbd3ull, 0x7685266ad1cf17e7ull},
    {"prelevel/LEVEL2.SME", 512, 1, "as-is", 512, 1, 6, 0, 1, 0, 3, nullptr, 1, 1, 0xc18b0ad0dc1612a4ull, 0x93f6823e401ad06eull, 0xa5e85afdddd4d39eull},
    {"prelevel/LEVEL20.SME", 800, 1, "crlf-strip", 787, 14, 2, 0, 0, 0, 1, nullptr, 1, 1, 0xcecbce8aae890901ull, 0x544ac335357656b2ull, 0xca2a47194ad697a6ull},
    {"prelevel/LEVEL21.SME", 690, 1, "as-is", 690, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x063e2969ba9da052ull, 0x6206b57e121c08f9ull, 0x2ef0e4d1eb17e627ull},
    {"prelevel/LEVEL22.SME", 748, 1, "as-is", 748, 1, 13, 0, 3, 0, 8, nullptr, 1, 1, 0xc596e474374e4f35ull, 0xda697c3ce5ad4584ull, 0x24807bb755d70ebcull},
    {"prelevel/LEVEL3.SME", 608, 1, "as-is", 608, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x15790325a6423ba5ull, 0x773190495ab3d43eull, 0x24004eb3a48854e0ull},
    {"prelevel/LEVEL4.SME", 701, 1, "crlf-strip", 699, 3, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xa3aeda0e9575be1aull, 0xf247d7d4712caed9ull, 0xba9ce0df902a0959ull},
    {"prelevel/LEVEL5.SME", 567, 1, "as-is", 567, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x5685a1ee7d7c7ce8ull, 0x3b23f5773ef60a5full, 0x5fe7aecf9060f759ull},
    {"prelevel/LEVEL6.SME", 579, 1, "as-is", 579, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x9ecd218c25b30d39ull, 0x9b4e382e24a25c4aull, 0x7e511a5a2544ff60ull},
    {"prelevel/LEVEL7.SME", 699, 1, "as-is", 699, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x29e1681308439ed0ull, 0x3501aeb3f703f10full, 0xd752bbbb5c02508dull},
    {"prelevel/LEVEL8.SME", 778, 1, "as-is", 778, 1, 5, 0, 1, 0, 2, nullptr, 1, 1, 0x8556f1bfbb82fcabull, 0x4170faef7b63cf17ull, 0xb727f5bd755c8e09ull},
    {"prelevel/LEVEL9.SME", 691, 1, "as-is", 691, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x686dc2abf3f1607aull, 0xa0dd4decb5febf09ull, 0x38969b90000985cfull},
    {"prelevel/RAND0.SME", 421, 1, "as-is", 421, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xd6a66f6cb71df1a8ull, 0x384041029ad26e3full, 0x92ff8e4e1db788adull},
    {"prelevel/RAND18.SME", 323, 1, "as-is", 323, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xab962b6910eaa856ull, 0xd9b1c90e743a059full, 0x52dbe85e4b65d6ffull},
    {"prelevel/RAND19.SME", 378, 1, "as-is", 378, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x81b297f693b7e1aeull, 0x43ca50d954b0f7f7ull, 0x136c581a630fe817ull},
    {"prelevel/RAND27.SME", 97, 1, "as-is", 97, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xfc1dd7e3f3d2ccb2ull, 0x284f15f7606e39c3ull, 0x91f5398dc92bb7f3ull},
    {"prelevel/RAND30.SME", 482, 1, "as-is", 482, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe61c1cd10570dc55ull, 0x8941123404f1766eull, 0xdd32b088457294e4ull},
    {"prelevel/RAND31.SME", 400, 1, "as-is", 400, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe10a0c728ee22fafull, 0x8f266dacd699e3a0ull, 0x053fee08edaba37aull},
    {"prelevel/RAND32.SME", 351, 1, "as-is", 351, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xaa329777a6082577ull, 0xead1bbd62b088bd8ull, 0xd8ee2cfec26b5d22ull},
    {"prelevel/RAND33.SME", 346, 1, "as-is", 346, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x993256afc3eccf67ull, 0x345a967302b79f70ull, 0x8e9185a32ad07f2cull},
    {"prelevel/RAND34.SME", 82, 1, "as-is", 82, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x72c71cff7a43b0c5ull, 0x26e18852eaed0a08ull, 0x8bf08d75a1f181f0ull},
    {"prelevel/RAND35.SME", 456, 1, "as-is", 456, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x91101b7e5b7a3fd1ull, 0xbb4d0e477b7a183aull, 0x434cc7757c4afc38ull},
    {"prelevel/RAND38.SME", 580, 1, "as-is", 580, 1, 5, 0, 0, 0, 4, nullptr, 1, 1, 0xc797f8ad8f024badull, 0xde2622c49aa17b40ull, 0x46771c3dc20355b6ull},
    {"prelevel/RAND4.SME", 121, 1, "as-is", 121, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x556fa6bdc81f7626ull, 0x8d55f93add5c00b7ull, 0x89d0d327afc29255ull},
    {"prelevel/RAND41.SME", 429, 1, "as-is", 429, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x2ce816e13de1a203ull, 0xcd83b4a60ed97f34ull, 0x24ff76b55900db46ull},
    {"prelevel/RAND42.SME", 548, 1, "as-is", 548, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xee1625be561a3e36ull, 0xd75496fec8da0fddull, 0x8213197faa1422afull},
    {"prelevel/RAND45.SME", 623, 1, "as-is", 623, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x123c5c44a9586e8full, 0x7af49dd4fe96f490ull, 0x1ec8e3458b45edaeull},
    {"prelevel/RAND5.SME", 315, 1, "as-is", 315, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xbbd8baeeb4c50c76ull, 0xf5da41ac1bd6d8adull, 0x095c9c06fee42fdfull},
    {"prelevel/RAND6.SME", 195, 1, "as-is", 195, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x0b4c3747405fc554ull, 0x162a9ffd731d4d5bull, 0x87e1cc829784106bull},
    {"prelevel/RAND8.SME", 279, 1, "as-is", 279, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x2b32317e8d6aa077ull, 0xf2770a1fe78053aaull, 0x50361632ef43f4c2ull},
    {"prelevel/Rand22.sme", 500, 1, "as-is", 500, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x8c96e55a1086ec64ull, 0x34f6dc542cbd88c1ull, 0x8c7b7eb8fc054e61ull},
    {"prelevel/Rand23.sme", 78, 1, "as-is", 78, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x4d9eda0819d0443dull, 0x66b5c673cff89704ull, 0x259143224d86302cull},
    {"prelevel/Rand24.sme", 401, 1, "as-is", 401, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3ef9d3204cc56169ull, 0xd61be4319f2f7910ull, 0x522a3b4d5c85d228ull},
    {"prelevel/dm1.sml", 385, 0, "plain", 385, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x286507559e5d30beull, 0x0c3a1c3cd5b3da2eull, 0x0a236515d806ef8cull},
    {"prelevel/dm2.sml", 279, 0, "plain", 279, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x2e91f101c657d8f8ull, 0x9a4d02af73f93364ull, 0xf9230506bff0e37aull},
    {"prelevel/dm3.sml", 370, 0, "plain", 370, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x43446d97e9b0303full, 0x31be2941f2641187ull, 0xe43f1f61396d9a91ull},
    {"prelevel/dm4.sme", 408, 1, "as-is", 408, 2, 4, 0, 0, 0, 2, nullptr, 1, 1, 0x49961498f4a860cbull, 0x1822b3ea9534eec9ull, 0xa03f470b79833233ull},
    {"prelevel/dm4.sml", 404, 0, "plain", 404, 2, 3, 0, 0, 0, 2, nullptr, 0, 1, 0xa34ffc40b9aed8fcull, 0x1822b3ea9534eec9ull, 0xa03f470b79833233ull},
    {"prelevel/level0.sml", 652, 0, "plain", 652, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xb3b2422efde799abull, 0x5e0b7f26645bdd11ull, 0xa4abaa6f2aecc457ull},
    {"prelevel/level1.sml", 353, 0, "plain", 353, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x135dc5b62ff4214cull, 0xd1abdccf01d50b1bull, 0xb3434b37ea3b0c4bull},
    {"prelevel/level10.sml", 602, 0, "plain", 602, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x4310398804724794ull, 0x27690fa3bde84e6bull, 0x2bbb72fe2bbd351dull},
    {"prelevel/level11.sml", 680, 0, "plain", 680, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x23eff91cd27bbf11ull, 0x3dcf34f729c56b52ull, 0x227a2e2d287e2b68ull},
    {"prelevel/level12.sml", 680, 0, "plain", 680, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xda321310ecea108bull, 0xd45c84f1175f22d4ull, 0x3c90e699837dda8eull},
    {"prelevel/level13.sml", 714, 0, "plain", 714, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x7ce7913e1df5eb8bull, 0x44ded1f544432364ull, 0x62ee46c0ea06f11eull},
    {"prelevel/level14.sml", 665, 0, "plain", 665, 1, 6, 0, 1, 0, 3, nullptr, 1, 1, 0xb770faa82dc856b5ull, 0xed420352b0c665c1ull, 0xb55db0feffdaa2edull},
    {"prelevel/level15.sml", 692, 0, "plain", 692, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xc6fbefbc47b5f412ull, 0xaaac022de310f7a1ull, 0x0b45f1ead2d0c8fbull},
    {"prelevel/level16.sml", 726, 0, "plain", 726, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x12a9b87d1499cd6aull, 0x8a867c941a7d54a1ull, 0xf8abf517451420c3ull},
    {"prelevel/level17.sml", 701, 0, "plain", 701, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x9e9110fe4f217993ull, 0x5a4f52c410496664ull, 0x6064bc0b2af3ee76ull},
    {"prelevel/level18.sml", 715, 0, "plain", 715, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe2985490075d30b3ull, 0xff3592d27ec6c9a4ull, 0x5f2a0da23e605586ull},
    {"prelevel/level19.sml", 162, 0, "plain", 162, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x44904e4cb5098740ull, 0xe419a026e70dfbd3ull, 0x7685266ad1cf17e7ull},
    {"prelevel/level2.sml", 512, 0, "plain", 512, 1, 6, 0, 1, 0, 3, nullptr, 1, 1, 0xc18b0ad0dc1612a4ull, 0x93f6823e401ad06eull, 0xa5e85afdddd4d39eull},
    {"prelevel/level20.sml", 800, 0, "plain", 800, 14, 2, 0, 0, 0, 1, nullptr, 1, 1, 0x7acc9f579a49d3f2ull, 0x544ac335357656b2ull, 0xca2a47194ad697a6ull},
    {"prelevel/level21.sml", 690, 0, "plain", 690, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x063e2969ba9da052ull, 0x6206b57e121c08f9ull, 0x2ef0e4d1eb17e627ull},
    {"prelevel/level22.sml", 748, 0, "plain", 748, 1, 13, 0, 3, 0, 8, nullptr, 1, 1, 0xc596e474374e4f35ull, 0xda697c3ce5ad4584ull, 0x24807bb755d70ebcull},
    {"prelevel/level23.sme", 634, 1, "as-is", 634, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xbeb9c99c105df7ceull, 0x0f60f5e48d01c263ull, 0x766c61bd17325a55ull},
    {"prelevel/level23.sml", 705, 0, "plain", 705, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xb71130206579b394ull, 0x6c587609035331d3ull, 0xb606de6307ccd45dull},
    {"prelevel/level24.sme", 683, 1, "as-is", 683, 2, 7, 0, 1, 0, 4, nullptr, 1, 1, 0x3865e6fbf9fad674ull, 0xbcb3ace65fbe429dull, 0x533d3ab42cbdc4e7ull},
    {"prelevel/level24.sml", 634, 0, "plain", 634, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xbeb9c99c105df7ceull, 0x0f60f5e48d01c263ull, 0x766c61bd17325a55ull},
    {"prelevel/level3.sml", 608, 0, "plain", 608, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x15790325a6423ba5ull, 0x773190495ab3d43eull, 0x24004eb3a48854e0ull},
    {"prelevel/level4.sml", 701, 0, "plain", 701, 3, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x2f7b83d006eaafdaull, 0xf247d7d4712caed9ull, 0xba9ce0df902a0959ull},
    {"prelevel/level5.sml", 567, 0, "plain", 567, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x5685a1ee7d7c7ce8ull, 0x3b23f5773ef60a5full, 0x5fe7aecf9060f759ull},
    {"prelevel/level6.sml", 579, 0, "plain", 579, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x9ecd218c25b30d39ull, 0x9b4e382e24a25c4aull, 0x7e511a5a2544ff60ull},
    {"prelevel/level7.sml", 699, 0, "plain", 699, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x29e1681308439ed0ull, 0x3501aeb3f703f10full, 0xd752bbbb5c02508dull},
    {"prelevel/level8.sml", 778, 0, "plain", 778, 1, 5, 0, 1, 0, 2, nullptr, 1, 1, 0x8556f1bfbb82fcabull, 0x4170faef7b63cf17ull, 0xb727f5bd755c8e09ull},
    {"prelevel/level9.sml", 691, 0, "plain", 691, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x686dc2abf3f1607aull, 0xa0dd4decb5febf09ull, 0x38969b90000985cfull},
    {"prelevel/rand0.sml", 421, 0, "plain", 421, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xd6a66f6cb71df1a8ull, 0x384041029ad26e3full, 0x92ff8e4e1db788adull},
    {"prelevel/rand1.sme", 280, 1, "as-is", 280, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xfcc914f4170f1387ull, 0x1f0720f47582c41aull, 0x896aaa570f283e10ull},
    {"prelevel/rand1.sml", 280, 0, "plain", 280, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xfcc914f4170f1387ull, 0x1f0720f47582c41aull, 0x896aaa570f283e10ull},
    {"prelevel/rand10.sme", 355, 1, "as-is", 355, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xdfd590ef97ef1d92ull, 0xeed06e6ea1f3c7abull, 0x4eeed15e39e26f7bull},
    {"prelevel/rand10.sml", 355, 0, "plain", 355, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xdfd590ef97ef1d92ull, 0xeed06e6ea1f3c7abull, 0x4eeed15e39e26f7bull},
    {"prelevel/rand11.sme", 193, 1, "as-is", 193, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xad2a1270b8648b97ull, 0xaed925a34ac5757aull, 0xa0ce4a8c80060412ull},
    {"prelevel/rand11.sml", 193, 0, "plain", 193, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xad2a1270b8648b97ull, 0xaed925a34ac5757aull, 0xa0ce4a8c80060412ull},
    {"prelevel/rand12.sme", 329, 1, "as-is", 329, 1, 9, 0, 0, 0, 8, nullptr, 1, 1, 0x49cb3dbb42530c32ull, 0xce848225c5b55f5dull, 0x330657025eddb5b7ull},
    {"prelevel/rand12.sml", 329, 0, "plain", 329, 1, 9, 0, 0, 0, 8, nullptr, 1, 1, 0x49cb3dbb42530c32ull, 0xce848225c5b55f5dull, 0x330657025eddb5b7ull},
    {"prelevel/rand13.sme", 309, 1, "as-is", 309, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x91203611383e36c2ull, 0x639a8b7c6353a7a3ull, 0xee5baa81ca5af64full},
    {"prelevel/rand13.sml", 309, 0, "plain", 309, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x91203611383e36c2ull, 0x639a8b7c6353a7a3ull, 0xee5baa81ca5af64full},
    {"prelevel/rand14.sme", 488, 1, "as-is", 488, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x64b79ae8dc61e156ull, 0x4b1d9b8a19baad77ull, 0x1abfa2cb28199d97ull},
    {"prelevel/rand14.sml", 488, 0, "plain", 488, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x64b79ae8dc61e156ull, 0x4b1d9b8a19baad77ull, 0x1abfa2cb28199d97ull},
    {"prelevel/rand15.sme", 318, 1, "as-is", 318, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x5e200a9430676ca0ull, 0x3fd44f482d048db5ull, 0xa67e6bb4202f5e25ull},
    {"prelevel/rand15.sml", 318, 0, "plain", 318, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x5e200a9430676ca0ull, 0x3fd44f482d048db5ull, 0xa67e6bb4202f5e25ull},
    {"prelevel/rand16.sme", 154, 1, "as-is", 154, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0xc660616ab271e84eull, 0x8010b47ceed3d1efull, 0x9a0dfff0e4f881dfull},
    {"prelevel/rand16.sml", 154, 0, "plain", 154, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0xc660616ab271e84eull, 0x8010b47ceed3d1efull, 0x9a0dfff0e4f881dfull},
    {"prelevel/rand17.sme", 407, 1, "as-is", 407, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x6eafacb1a19c8b99ull, 0x28822daf9669e6d0ull, 0x6033f1f69bc432e8ull},
    {"prelevel/rand17.sml", 407, 0, "plain", 407, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x6eafacb1a19c8b99ull, 0x28822daf9669e6d0ull, 0x6033f1f69bc432e8ull},
    {"prelevel/rand18.sml", 323, 0, "plain", 323, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xab962b6910eaa856ull, 0xd9b1c90e743a059full, 0x52dbe85e4b65d6ffull},
    {"prelevel/rand19.sml", 378, 0, "plain", 378, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x81b297f693b7e1aeull, 0x43ca50d954b0f7f7ull, 0x136c581a630fe817ull},
    {"prelevel/rand2.sme", 180, 1, "as-is", 180, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x1e9cc32fce771a5full, 0xd8862e5d91f75cd6ull, 0xd8d869e1f6cf2152ull},
    {"prelevel/rand2.sml", 180, 0, "plain", 180, 1, 6, 0, 0, 0, 5, nullptr, 1, 1, 0x1e9cc32fce771a5full, 0xd8862e5d91f75cd6ull, 0xd8d869e1f6cf2152ull},
    {"prelevel/rand20.sme", 321, 1, "as-is", 321, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3debb73b594c8ef4ull, 0x206dfc9ae7da8db1ull, 0x362ffc37c571b9d1ull},
    {"prelevel/rand20.sml", 321, 0, "plain", 321, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3debb73b594c8ef4ull, 0x206dfc9ae7da8db1ull, 0x362ffc37c571b9d1ull},
    {"prelevel/rand21.sme", 184, 1, "as-is", 184, 1, 8, 0, 0, 0, 7, nullptr, 1, 1, 0xe0c95b2dd09cefcdull, 0x3e4fdcb19e2a983cull, 0x989242f1fa4939ccull},
    {"prelevel/rand21.sml", 181, 0, "plain", 181, 1, 8, 0, 0, 0, 7, nullptr, 1, 1, 0x2289fadf5cfc81b4ull, 0x23772a02bd43ec37ull, 0xa68b2d6126c5bd4bull},
    {"prelevel/rand22.sml", 500, 0, "plain", 500, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x8c96e55a1086ec64ull, 0x34f6dc542cbd88c1ull, 0x8c7b7eb8fc054e61ull},
    {"prelevel/rand23.sml", 78, 0, "plain", 78, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x4d9eda0819d0443dull, 0x66b5c673cff89704ull, 0x259143224d86302cull},
    {"prelevel/rand24.sml", 401, 0, "plain", 401, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3ef9d3204cc56169ull, 0xd61be4319f2f7910ull, 0x522a3b4d5c85d228ull},
    {"prelevel/rand25.sme", 401, 1, "as-is", 401, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3ef9d3204cc56169ull, 0xd61be4319f2f7910ull, 0x522a3b4d5c85d228ull},
    {"prelevel/rand26.sme", 227, 1, "as-is", 227, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xcb9bd32b81a2dbb5ull, 0x9a3354e9c5d9c070ull, 0xa6da8b1786a60a08ull},
    {"prelevel/rand26.sml", 227, 0, "plain", 227, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xcb9bd32b81a2dbb5ull, 0x9a3354e9c5d9c070ull, 0xa6da8b1786a60a08ull},
    {"prelevel/rand27.sml", 97, 0, "plain", 97, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0xfc1dd7e3f3d2ccb2ull, 0x284f15f7606e39c3ull, 0x91f5398dc92bb7f3ull},
    {"prelevel/rand28.sme", 229, 1, "as-is", 229, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x998b1b2169d3100eull, 0x8678959144fbf6f7ull, 0x561a9cd2535ae717ull},
    {"prelevel/rand28.sml", 229, 0, "plain", 229, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x998b1b2169d3100eull, 0x8678959144fbf6f7ull, 0x561a9cd2535ae717ull},
    {"prelevel/rand29.sme", 176, 1, "as-is", 176, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x6fcfb74b1627933eull, 0x24ded4fde2452ebfull, 0x438feb7863c6a99full},
    {"prelevel/rand29.sml", 176, 0, "plain", 176, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x6fcfb74b1627933eull, 0x24ded4fde2452ebfull, 0x438feb7863c6a99full},
    {"prelevel/rand3.sme", 318, 1, "as-is", 318, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xa8ca16ea476ef96eull, 0x22408e701c526d45ull, 0xcd676b0056cd0b33ull},
    {"prelevel/rand3.sml", 318, 0, "plain", 318, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xa8ca16ea476ef96eull, 0x22408e701c526d45ull, 0xcd676b0056cd0b33ull},
    {"prelevel/rand30.sml", 482, 0, "plain", 482, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe61c1cd10570dc55ull, 0x8941123404f1766eull, 0xdd32b088457294e4ull},
    {"prelevel/rand31.sml", 400, 0, "plain", 400, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe10a0c728ee22fafull, 0x8f266dacd699e3a0ull, 0x053fee08edaba37aull},
    {"prelevel/rand32.sml", 351, 0, "plain", 351, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xaa329777a6082577ull, 0xead1bbd62b088bd8ull, 0xd8ee2cfec26b5d22ull},
    {"prelevel/rand33.sml", 346, 0, "plain", 346, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x993256afc3eccf67ull, 0x345a967302b79f70ull, 0x8e9185a32ad07f2cull},
    {"prelevel/rand34.sml", 82, 0, "plain", 82, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x72c71cff7a43b0c5ull, 0x26e18852eaed0a08ull, 0x8bf08d75a1f181f0ull},
    {"prelevel/rand35.sml", 456, 0, "plain", 456, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x91101b7e5b7a3fd1ull, 0xbb4d0e477b7a183aull, 0x434cc7757c4afc38ull},
    {"prelevel/rand36.sme", 667, 1, "as-is", 667, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x6f24131948c81b14ull, 0x92976f528853e3f3ull, 0x956f97743667a939ull},
    {"prelevel/rand36.sml", 667, 0, "plain", 667, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x6f24131948c81b14ull, 0x92976f528853e3f3ull, 0x956f97743667a939ull},
    {"prelevel/rand37.sme", 435, 1, "as-is", 435, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe7725e7b3c9e0c18ull, 0x9694a4d34c862d97ull, 0x61df909b48fcd749ull},
    {"prelevel/rand37.sml", 435, 0, "plain", 435, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xe7725e7b3c9e0c18ull, 0x9694a4d34c862d97ull, 0x61df909b48fcd749ull},
    {"prelevel/rand38.sml", 580, 0, "plain", 580, 1, 5, 0, 0, 0, 4, nullptr, 1, 1, 0xc797f8ad8f024badull, 0xde2622c49aa17b40ull, 0x46771c3dc20355b6ull},
    {"prelevel/rand39.sme", 715, 1, "as-is", 715, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xc7a9485891193222ull, 0xf187bcb345ae3c33ull, 0xafe70ae3d9c5c7b5ull},
    {"prelevel/rand39.sml", 715, 0, "plain", 715, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xc7a9485891193222ull, 0xf187bcb345ae3c33ull, 0xafe70ae3d9c5c7b5ull},
    {"prelevel/rand4.sml", 121, 0, "plain", 121, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x556fa6bdc81f7626ull, 0x8d55f93add5c00b7ull, 0x89d0d327afc29255ull},
    {"prelevel/rand40.sme", 564, 1, "as-is", 564, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xfc1c63e1a399c0cdull, 0x66187cbf9d6ef276ull, 0x72f3ef78271cc1a8ull},
    {"prelevel/rand40.sml", 564, 0, "plain", 564, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xfc1c63e1a399c0cdull, 0x66187cbf9d6ef276ull, 0x72f3ef78271cc1a8ull},
    {"prelevel/rand41.sml", 429, 0, "plain", 429, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x2ce816e13de1a203ull, 0xcd83b4a60ed97f34ull, 0x24ff76b55900db46ull},
    {"prelevel/rand42.sml", 548, 0, "plain", 548, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xee1625be561a3e36ull, 0xd75496fec8da0fddull, 0x8213197faa1422afull},
    {"prelevel/rand43.sme", 686, 1, "as-is", 686, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x441896026a1f1ec4ull, 0x17a5d6ede945b17bull, 0xdd6540105e36597dull},
    {"prelevel/rand43.sml", 686, 0, "plain", 686, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x441896026a1f1ec4ull, 0x17a5d6ede945b17bull, 0xdd6540105e36597dull},
    {"prelevel/rand44.sme", 533, 1, "as-is", 533, 1, 5, 0, 0, 0, 4, nullptr, 1, 1, 0xbdd13b13664729ccull, 0x64bf7706e0ca94b9ull, 0x7cb797db17f38d2bull},
    {"prelevel/rand44.sml", 533, 0, "plain", 533, 1, 5, 0, 0, 0, 4, nullptr, 1, 1, 0xbdd13b13664729ccull, 0x64bf7706e0ca94b9ull, 0x7cb797db17f38d2bull},
    {"prelevel/rand45.sml", 623, 0, "plain", 623, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x123c5c44a9586e8full, 0x7af49dd4fe96f490ull, 0x1ec8e3458b45edaeull},
    {"prelevel/rand5.sml", 315, 0, "plain", 315, 1, 3, 0, 0, 0, 2, nullptr, 1, 1, 0xbbd8baeeb4c50c76ull, 0xf5da41ac1bd6d8adull, 0x095c9c06fee42fdfull},
    {"prelevel/rand6.sml", 195, 0, "plain", 195, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x0b4c3747405fc554ull, 0x162a9ffd731d4d5bull, 0x87e1cc829784106bull},
    {"prelevel/rand7.sme", 475, 1, "as-is", 475, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x356db4491b69be5full, 0xe89c938f8d3c6072ull, 0x6597f19951d890aaull},
    {"prelevel/rand7.sml", 475, 0, "plain", 475, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x356db4491b69be5full, 0xe89c938f8d3c6072ull, 0x6597f19951d890aaull},
    {"prelevel/rand8.sml", 279, 0, "plain", 279, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x2b32317e8d6aa077ull, 0xf2770a1fe78053aaull, 0x50361632ef43f4c2ull},
    {"prelevel/rand9.sme", 338, 1, "as-is", 338, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x87434364b0699999ull, 0x38ea2f912e5e0a0cull, 0x45e78cc640c37b94ull},
    {"prelevel/rand9.sml", 338, 0, "plain", 338, 1, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x87434364b0699999ull, 0x38ea2f912e5e0a0cull, 0x45e78cc640c37b94ull},
    {"prelevel/s1.sml", 364, 0, "plain", 364, 2, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3833ab618af0807dull, 0x01894ebcb839b312ull, 0x5c8466264db3a468ull},
    {"prelevel/s2.sml", 337, 0, "plain", 337, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x6310424ee19569daull, 0x8157fdf27c0bb0b2ull, 0x78ff3edb39138014ull},
    {"prelevel/secrt1.sme", 364, 1, "as-is", 364, 2, 4, 0, 0, 0, 3, nullptr, 1, 1, 0x3833ab618af0807dull, 0x01894ebcb839b312ull, 0x5c8466264db3a468ull},
    {"prelevel/secrt2.sme", 337, 1, "as-is", 337, 2, 3, 0, 0, 0, 2, nullptr, 1, 1, 0x6310424ee19569daull, 0x8157fdf27c0bb0b2ull, 0x78ff3edb39138014ull},
};

} // namespace

int main() {
    fs::path game = "/mnt/d/games/Gunmetal 3dfx";
    if (const char* env = std::getenv("GUNMETAL_DIR")) {
        game = env;
    }
    const fs::path dir = game / "SML";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cout << "sml: game not installed, skipped\n";
        return 0;
    }

    // ------------------------------------------------------------- corpus --
    std::vector<std::pair<std::string, fs::path>> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = lower(entry.path().extension().string());
        if (ext != ".sml" && ext != ".sme") {
            continue;
        }
        const std::string rel = fs::relative(entry.path(), dir, ec).generic_string();
        files.emplace_back(rel, entry.path());
    }
    std::sort(files.begin(), files.end());

    std::size_t sme = 0;
    std::size_t sml = 0;
    std::map<std::string, fs::path> sme_by_stem;
    std::map<std::string, fs::path> sml_by_stem;
    std::set<std::string> unique_images;
    std::set<int> font_values;
    std::set<int> music_tracks;
    std::set<int> music_offsets;
    std::size_t total_images = 0;
    std::size_t image_left = 0;
    std::size_t image_right = 0;
    std::size_t image_64 = 0;
    std::size_t total_fonts = 0;
    std::size_t total_music = 0;
    std::size_t total_tags = 0;

    for (const auto& [rel, path] : files) {
        const bool encoded = ends_with(lower(rel), ".sme");
        if (encoded) {
            ++sme;
        } else {
            ++sml;
        }
        const std::string stem = lower(rel.substr(0, rel.size() - 4));
        if (encoded) {
            sme_by_stem[stem] = path;
        } else {
            sml_by_stem[stem] = path;
        }
        const auto data = read_file(path);
        std::string error;
        const auto doc = gm::parse_sml(data, error);
        if (!doc) {
            // Only prelevel/dm4.sml (no closing </d>) is shipped malformed.
            check(rel == "prelevel/dm4.sml", rel + ": parse failed: " + error);
            check_eq(error, std::string("sml: last tag is not </d>"),
                     rel + ": error text");
            // The aggregate tag count still covers this (plain) file.
            total_tags += gm::sml_scan_tags(read_text(path)).size();
            continue;
        }
        const gm::SmlSummary summary = gm::describe_sml(*doc);
        for (const auto& element : doc->elements) {
            if (element.kind == gm::SmlKind::Image) {
                ++total_images;
                if (element.align && *element.align == 'l') {
                    ++image_left;
                } else if (element.align && *element.align == 'r') {
                    ++image_right;
                }
                if (element.x && element.y && *element.x == 64 && *element.y == 64) {
                    ++image_64;
                }
                if (!element.value.empty()) {
                    unique_images.insert(lower(element.value));
                }
            } else if (element.kind == gm::SmlKind::Font) {
                ++total_fonts;
                if (element.font) {
                    font_values.insert(*element.font);
                }
            } else if (element.kind == gm::SmlKind::Music) {
                ++total_music;
                if (element.track) {
                    music_tracks.insert(*element.track);
                }
                if (element.offset) {
                    music_offsets.insert(*element.offset);
                }
            }
        }
        total_tags += summary.tags;
    }

    check_eq(files.size(), 300, "corpus files");
    check_eq(sme, 151, "corpus .sme");
    check_eq(sml, 149, "corpus .sml");
    check_eq(total_images, 72, "corpus image tags");
    check_eq(unique_images.size(), 15, "corpus unique images");
    check_eq(image_left, 50, "corpus left-aligned images");
    check_eq(image_right, 22, "corpus right-aligned images");
    check_eq(image_64, 72, "corpus images at x=y=64");
    check_eq(total_fonts, 268, "corpus font tags");
    check_eq(total_music, 56, "corpus music tags");
    check_eq(total_tags, 2632, "corpus tags");
    check(font_values == std::set<int>{2, 3}, "font values are 2 and 3");
    check(music_tracks == std::set<int>({0, 1, 60}), "music B values");
    check(music_offsets == std::set<int>({0, 40, 50, 70, 80, 100, 170}),
          "music S values");

    // ------------------------------------------------------------- golden --
    std::map<std::string, fs::path> by_path;
    std::map<std::string, fs::path> by_lower_path;
    for (const auto& [rel, path] : files) {
        by_path[rel] = path;
        by_lower_path[lower(rel)] = path;
    }
    check_eq(sizeof(kGolden) / sizeof(kGolden[0]), 300, "golden rows");
    std::size_t golden_ok = 0;
    for (const GoldenFile& row : kGolden) {
        const auto it = by_path.find(row.path);
        check(it != by_path.end(), std::string("missing file ") + row.path);
        if (it == by_path.end()) {
            continue;
        }
        const auto data = read_file(it->second);
        check_eq(data.size(), row.bytes, std::string(row.path) + ": bytes");
        std::string error;
        const auto doc = gm::parse_sml(data, error);
        if (!row.ok) {
            check(!doc, std::string(row.path) + ": expected malformed document");
            continue;
        }
        check(doc.has_value(), std::string(row.path) + ": " + error);
        if (!doc) {
            continue;
        }
        ++golden_ok;
        const gm::SmlSummary summary = gm::describe_sml(*doc);
        const std::string where = row.path;
        check_eq(ends_with(lower(row.path), ".sme"), row.encoded, where + ": encoded flag");
        check_eq(doc->note, row.note, where + ": decode note");
        check_eq(summary.chars, row.chars, where + ": chars");
        check_eq(summary.lines, row.lines, where + ": lines");
        check_eq(summary.tags, row.tags, where + ": tags");
        check_eq(summary.images, row.images, where + ": images");
        check_eq(summary.fonts, row.fonts, where + ": fonts");
        check_eq(summary.music, row.music, where + ": music");
        check_eq(summary.text_runs, row.text_runs, where + ": text runs");
        check_eq(summary.doc_ok, row.ok, where + ": doc ok");
        check_eq(summary.brackets_balanced, row.balanced, where + ": balanced");
        const std::string unknown =
            row.unknown == nullptr ? std::string() : std::string(row.unknown);
        std::string joined;
        for (const auto& name : summary.unknown_tags) {
            if (!joined.empty()) {
                joined.push_back(',');
            }
            joined += name;
        }
        check_eq(joined, unknown, where + ": unknown tags");
        check_eq(fnv1a(doc->text), row.markup_fnv, where + ": markup fnv");
        check_eq(fnv1a(summary.plain), row.plain_fnv, where + ": plain fnv");
        check_eq(fnv1a(summary.body), row.body_fnv, where + ": body fnv");

        // The cipher is its own inverse: encode(decode(bytes)) == bytes.
        const auto round_trip = gm::sml_encode(gm::sml_decode(data));
        check(round_trip == data, where + ": cipher round trip");
    }
    check_eq(golden_ok, 299, "golden parsed documents");

    // -------------------------------------------------------------- pairs --
    std::size_t pairs = 0;
    std::size_t identical = 0;
    std::size_t revisions = 0;
    std::size_t line_endings = 0;
    std::size_t unpaired_sme = 0;
    std::size_t unpaired_sml = 0;
    for (const auto& [stem, sme_path] : sme_by_stem) {
        const auto sml_it = sml_by_stem.find(stem);
        if (sml_it == sml_by_stem.end()) {
            ++unpaired_sme;
            continue;
        }
        ++pairs;
        const auto raw = read_file(sme_path);
        const std::string expected = normalize(read_text(sml_it->second));
        const auto [decoded, note] = gm::sml_decode_best(raw);
        auto candidates = gm::sml_decode_candidates(raw);
        if (candidates.empty()) {
            candidates.emplace_back(note, decoded);
        }
        std::optional<std::string> match_note;
        for (const auto& candidate : candidates) {
            if (normalize(candidate.second) == expected) {
                match_note = candidate.first;
                break;
            }
        }
        if (note == "as-is" && normalize(decoded) == expected) {
            ++identical;
        } else if (match_note && *match_note == "crlf-strip") {
            ++line_endings;
        } else if (match_note) {
            ++identical;
        } else {
            ++revisions;
        }
    }
    for (const auto& [stem, sml_path] : sml_by_stem) {
        (void)sml_path;
        if (sme_by_stem.find(stem) == sme_by_stem.end()) {
            ++unpaired_sml;
        }
    }
    check_eq(pairs, 147, "pairs");
    check_eq(identical, 131, "identical pairs");
    check_eq(revisions, 14, "revision pairs");
    check_eq(line_endings, 2, "line-ending pairs");
    check_eq(unpaired_sme, 4, "sme without sml");
    check_eq(unpaired_sml, 2, "sml without sme");

    // ------------------------------------------------------ named corpus ---
    const auto load = [&](const std::string& rel) {
        const auto it = by_lower_path.find(lower(rel));
        if (it == by_lower_path.end()) {
            check(false, "missing named file " + rel);
            return std::optional<gm::SmlDocument>{};
        }
        std::string error;
        auto doc = gm::parse_sml(read_file(it->second), error);
        check(doc.has_value(), rel + ": " + error);
        return doc;
    };

    // prelevel/level17.sme is a normal as-is decode; the *brief* level17 is
    // the newline anomaly.
    if (auto doc = load("prelevel/level17.sme")) {
        check_eq(doc->note, std::string("as-is"), "prelevel/level17.sme note");
        check_eq(doc->tags.size(), std::size_t{3}, "prelevel/level17.sme tags");
        check(starts_with(doc->text, "<D>"), "prelevel/level17.sme starts <D>");
        check(doc->doc_close, "prelevel/level17.sme closes");
    }
    if (auto doc = load("prelevel/level20.sme")) {
        check_eq(doc->note, std::string("crlf-strip"), "prelevel/level20.sme note");
        check_eq(doc->tags.size(), std::size_t{2}, "prelevel/level20.sme tags");
        check(starts_with(doc->text, "<D>"), "prelevel/level20.sme starts <D>");
        check(doc->doc_close, "prelevel/level20.sme closes");
    }
    if (auto doc = load("brief/level17.sme")) {
        check_eq(doc->note, std::string("newline-repair"), "brief/level17.sme note");
        check_eq(doc->tags.size(), std::size_t{18}, "brief/level17.sme tags");
        check(doc->text.find('\r') == std::string::npos,
              "brief/level17.sme has no CR after repair");
        check(doc->doc_close, "brief/level17.sme closes");
    }
    // dm4.sml lacks </d>; its encoded sibling is the copy the engine loads.
    {
        const auto it = by_lower_path.find("prelevel/dm4.sml");
        check(it != by_lower_path.end(), "prelevel/dm4.sml present");
        if (it != by_lower_path.end()) {
            std::string error;
            check(!gm::parse_sml(read_file(it->second), error),
                  "prelevel/dm4.sml rejected");
            check_eq(error, std::string("sml: last tag is not </d>"),
                     "prelevel/dm4.sml error");
        }
        if (auto doc = load("prelevel/dm4.sme")) {
            check_eq(doc->tags.size(), std::size_t{4}, "prelevel/dm4.sme tags");
            check(doc->doc_close, "prelevel/dm4.sme closes");
        }
    }
    // help.sme is the corrected revision of help.sml.
    {
        const auto sme_it = by_lower_path.find("help.sme");
        const auto sml_it = by_lower_path.find("help.sml");
        check(sme_it != by_lower_path.end(), "help.sme present");
        check(sml_it != by_lower_path.end(), "help.sml present");
        if (sme_it != by_lower_path.end() && sml_it != by_lower_path.end()) {
            std::string error;
            const auto sme_doc = gm::parse_sml(read_file(sme_it->second), error);
            check(sme_doc.has_value(), "help.sme parses: " + error);
            if (sme_doc) {
                const gm::SmlSummary summary = gm::describe_sml(*sme_doc);
                check_eq(summary.tags, std::size_t{90}, "help.sme tags");
                check(summary.body.find("Toggle Player ID") != std::string::npos,
                      "help.sme fixes Toggle Player ID");
                check(summary.body.find("P - Pause") != std::string::npos,
                      "help.sme adds P - Pause");
                check(sme_doc->text.find("PLayer") == std::string::npos,
                      "help.sme has no PLayer typo");
            }
            const auto sml_doc = gm::parse_sml(read_file(sml_it->second), error);
            check(sml_doc.has_value(), "help.sml parses: " + error);
            if (sml_doc) {
                check_eq(gm::describe_sml(*sml_doc).tags, std::size_t{63},
                         "help.sml tags");
                check(sml_doc->text.find("PLayer") != std::string::npos,
                      "help.sml keeps PLayer typo");
            }
        }
    }

    // ---------------------------------------------------------- negatives --
    const auto parse_text = [](std::string_view text, gm::SmlSource source = gm::SmlSource::Plain) {
        std::string error;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
        auto doc = gm::parse_sml(std::span<const std::uint8_t>(bytes, text.size()), source,
                                 error);
        return std::make_pair(std::move(doc), error);
    };

    {
        auto [doc, error] = parse_text("");
        check(!doc, "empty input rejected");
        check_eq(error, std::string("sml: empty input"), "empty input error");
    }
    {
        auto [doc, error] = parse_text("no tags here");
        check(!doc, "tagless input rejected");
        check_eq(error, std::string("sml: no tags"), "tagless error");
    }
    {
        auto [doc, error] = parse_text("<p>x</p>");
        check(!doc, "missing <d> rejected");
        check_eq(error, std::string("sml: first tag is not <d>"), "missing <d> error");
    }
    {
        auto [doc, error] = parse_text("<d><p>x");
        check(!doc, "missing </d> rejected");
        check_eq(error, std::string("sml: last tag is not </d>"), "missing </d> error");
    }
    {
        auto [doc, error] = parse_text("<d>a < b</d>");
        check(!doc, "unbalanced < rejected");
        check_eq(error, std::string("sml: bracket imbalance"), "bracket error");
    }
    {
        auto [doc, error] = parse_text("<d><p>hello</d");
        check(!doc, "truncated tail rejected");
        check_eq(error, std::string("sml: last tag is not </d>"), "truncated error");
    }

    // Exactly 511 tags is the loader limit; 512 must fail.
    {
        std::string at_limit = "<d>";
        for (int i = 0; i < 509; ++i) {
            at_limit += "<r>";
        }
        at_limit += "</d>";
        auto [doc, error] = parse_text(at_limit);
        check(doc.has_value(), "511 tags accepted: " + error);
        if (doc) {
            check_eq(doc->tags.size(), std::size_t{511}, "511 tag count");
            check(!doc->too_many_tags, "511 tags under limit");
        }
        std::string over = "<d>";
        for (int i = 0; i < 510; ++i) {
            over += "<r>";
        }
        over += "</d>";
        auto [doc2, error2] = parse_text(over);
        check(!doc2, "512 tags rejected");
        check_eq(error2, std::string("sml: tag limit exceeded"), "tag limit error");
    }

    // Unknown tags are reported but not fatal.
    {
        auto [doc, error] = parse_text("<d><z>x</z></d>");
        check(doc.has_value(), "unknown tag document parses: " + error);
        if (doc) {
            check_eq(doc->unknown_tags.size(), std::size_t{1}, "one unknown tag");
            check_eq(doc->unknown_tags.front(), std::string("z"), "unknown tag name");
        }
    }

    // Image attributes: double quotes parse, single quotes do not (Python
    // regex parity), and a="..." is lower-cased.
    {
        auto [doc, error] = parse_text("<d><i x=\"1\" y=\"2\" f=\"img\" a=\"R\"></d>");
        check(doc.has_value(), "image document parses: " + error);
        if (doc) {
            const auto& elements = doc->elements;
            const auto it = std::find_if(elements.begin(), elements.end(), [](const auto& e) {
                return e.kind == gm::SmlKind::Image;
            });
            check(it != elements.end(), "image element present");
            if (it != elements.end()) {
                check_eq(it->value, std::string("img"), "image f");
                check(it->x && *it->x == 1, "image x");
                check(it->y && *it->y == 2, "image y");
                check(it->align && *it->align == 'r', "image align lower-cased");
            }
        }
        auto [single, single_error] =
            parse_text("<d><i x='1' y='2' f='a' a='l'></d>");
        check(single.has_value(), "single-quoted image parses: " + single_error);
        if (single) {
            const auto it = std::find_if(single->elements.begin(), single->elements.end(),
                                         [](const auto& e) {
                                             return e.kind == gm::SmlKind::Image;
                                         });
            check(it != single->elements.end(), "single-quoted image element");
            if (it != single->elements.end()) {
                check(it->value.empty(), "single quotes are not attributes");
                check(!it->x && !it->y && !it->align,
                      "single quotes yield no typed attrs");
            }
        }
    }

    // A corrupted cipher: change the 'd' of the closing tag.  The positional
    // shift means one byte is enough to lose the wrapper.
    {
        const std::string source = "<d><p>hello world</p></d>";
        const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(source.data());
        auto encoded = gm::sml_encode(
            std::span<const std::uint8_t>(source_bytes, source.size()));
        encoded[encoded.size() - 2] = static_cast<std::uint8_t>('e');
        std::string error;
        check(!gm::parse_sml(encoded, error), "corrupted cipher rejected");
    }
    // A byte inserted in a shipped .sme desynchronises the positional key.
    {
        const auto it = by_path.find("brief/LEVEL1.SME");
        check(it != by_path.end(), "brief/LEVEL1.SME present");
        if (it != by_path.end()) {
            auto data = read_file(it->second);
            check(!data.empty(), "LEVEL1.SME non-empty");
            data.insert(data.begin() + static_cast<std::ptrdiff_t>(data.size() / 2), 'X');
            std::string error;
            check(!gm::parse_sml(data, error), "desynchronised cipher rejected");
            check(!error.empty(), "desynchronised cipher has an error");
        }
    }

    // Synthetic CRLF transport and the level17-shaped stray newline.
    {
        const std::string plain = "<d><p>Hello world.</p>\n<p>Second line.</p></d>\n";
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(plain.data());
        const auto encoded = gm::sml_encode(
            std::span<const std::uint8_t>(bytes, plain.size()));
        std::vector<std::uint8_t> transported;
        for (std::uint8_t byte : encoded) {
            if (byte == '\n') {
                transported.push_back('\r');
            }
            transported.push_back(byte);
        }
        std::string error;
        const auto doc = gm::parse_sml(transported, error);
        check(doc.has_value(), "CRLF transport parses: " + error);
        if (doc) {
            check_eq(doc->note, std::string("crlf-strip"), "CRLF transport note");
            check_eq(doc->text, plain, "CRLF transport text");
        }
    }
    {
        const std::string plain =
            "<d><p>" + std::string(60, 'A') + " " + std::string(60, 'B') + "</p></d>";
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(plain.data());
        auto encoded =
            gm::sml_encode(std::span<const std::uint8_t>(bytes, plain.size()));
        const std::size_t cut = plain.find(' ');
        std::vector<std::uint8_t> mangled(encoded.begin(),
                                          encoded.begin() + static_cast<std::ptrdiff_t>(cut));
        mangled.push_back('\r');
        mangled.push_back('\n');
        mangled.insert(mangled.end(),
                       encoded.begin() + static_cast<std::ptrdiff_t>(cut + 1), encoded.end());
        std::string error;
        const auto doc = gm::parse_sml(mangled, error);
        check(doc.has_value(), "newline insertion parses: " + error);
        if (doc) {
            check_eq(doc->note, std::string("newline-repair"), "newline insertion note");
            check_eq(doc->text, plain, "newline insertion text");
        }
    }

    if (failures == 0) {
        std::cout << "sml: " << files.size() << " files (" << sme << " .sme, " << sml
                  << " .sml), " << pairs << " pairs: identical " << identical
                  << ", revisions " << revisions << ", line-endings " << line_endings
                  << "; images " << total_images << " (" << unique_images.size()
                  << " unique), music " << total_music << "; OK\n";
        return 0;
    }
    std::cerr << "sml: " << failures << " failure(s)\n";
    return 1;
}
