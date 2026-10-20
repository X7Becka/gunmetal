// SOU1 (4-bit Miles ADPCM) decoder parity test.
//
// Corpus: sounds/SOU.IDX holds 999 blobs — 13 SOU0 (raw 8-bit PCM), 983 SOU1
// (this codec) and 3 SOU2 (music, see test_sou2.cpp).  Every expected value
// below comes from docs/manifests/sou1-fnv.json, which
// tools/re/sou1_model.py --verify produced by comparing an independent Python
// model with the *real decoder bytes* of gundx.exe
// (0x852b40/0x852c00/0x852cc0) on all 983 entries: 67,939,382 slots,
// 2123.1 s, 983/983 bit-exact.
//
// Hashes are FNV-1a 64 over the little-endian output of each record:
//
//   raw   - interleaved 32-bit mixer accumulators at 32 kHz (duplicates kept)
//   pcm16 - decode_sou1(..., shift = 5): clamp(value >> 5), the manifest's
//           fnv1a64_pcm16 (full scale of the accumulator is about 2^20)
//   pcm1  - decode_sou1's default clamp(value >> 1), pinned only for the spot
//           records so the default presentation is covered too
//   wav   - sou1_to_wav (source rate, duplicated slots dropped, >> 5)
//
// The full pass checks every record against the manifest table; the quick
// pass (11 pinned records) still parses all 999 container headers.
#include "res/bigfile.h"
#include "res/sou1.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

constexpr std::uint64_t kFnvBasis = 14695981039346656037ull; // 0xcbf29ce484222325
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

// Shipped sounds/SOU.IDX shape.
constexpr std::size_t kEntries = 999;
constexpr std::size_t kSou0 = 13;
constexpr std::size_t kSou1 = 983;
constexpr std::size_t kSou2 = 3;
constexpr std::uint64_t kTotalSlots = 67939382ull;      // per channel, 2123.105687 s
constexpr std::uint64_t kTotalSamples = 75430904ull;    // interleaved L/R
constexpr std::uint64_t kMaxAbsRaw = 1048576ull;        // CRUNCH4

// Aggregate hashes over all 983 SOU1 records in index order (full pass).
constexpr std::uint64_t kRawFnv = 0x538367759d7c7cacull;
constexpr std::uint64_t kPcm5Fnv = 0x156174534588594cull;
constexpr std::uint64_t kPcm1Fnv = 0x0b6bfbab8aeb688bull;

struct RecordRef {
    const char* name;
    std::uint32_t rate;
    int channels;
    std::uint32_t payload_bytes;
    std::uint32_t slots;
    std::uint64_t raw;
    std::uint64_t pcm16;
};

// Per-entry manifest (docs/manifests/sou1-fnv.json), one line per SOU1 entry.
constexpr RecordRef kRecords[] = {
    {"ADON", 8000, 1, 1502, 12016, 0x9d67e203de04e745ull, 0x61661474c31c21a5ull},
    {"ALARMA", 16000, 1, 8000, 32000, 0xd06958af09c0ce45ull, 0xbcba2481704a2191ull},
    {"ALARMABG", 16000, 1, 8000, 32000, 0x1a5c942685055f3dull, 0xa6663b03a8698efdull},
    {"ALARMBIG", 16000, 1, 6802, 27208, 0xabc9b1595874caa5ull, 0xa98ec629570c0939ull},
    {"ALARMK", 16000, 1, 13602, 54408, 0x42f2a46582faa085ull, 0x6a28308d2346ff51ull},
    {"ALARMKP", 16000, 1, 8008, 32032, 0x5e147559ee2eced5ull, 0x4981778a2fdcbd55ull},
    {"AMBEACH", 16000, 1, 26724, 106896, 0xc229f5d3689df5a9ull, 0xb6d47f24a5621f3dull},
    {"AMBOILR1", 16000, 1, 28510, 114040, 0x3461864abde12301ull, 0xd2d38a0b33b08c25ull},
    {"AMBOILR2", 16000, 1, 31730, 126920, 0x9954308e18f0cabdull, 0xea340f9315316221ull},
    {"AMBOILR3", 16000, 1, 39688, 158752, 0x3bd048ac770470b5ull, 0x2b4195814c0cef19ull},
    {"AMBREW1", 16000, 1, 37574, 150296, 0xae4c12945b57e2adull, 0x6845c9ae27dd9751ull},
    {"AMBROOK", 16000, 1, 26814, 107256, 0x28bb70992bd1fb99ull, 0x81df786275696bcdull},
    {"AMCAVERN", 16000, 1, 49958, 199832, 0xa547fd229d0645d1ull, 0xbef3fce4eb6366ddull},
    {"AMCITY1", 16000, 1, 32516, 130064, 0xafa275f5d55141f9ull, 0x77022bcb619cddd5ull},
    {"AMCITY2", 16000, 1, 42210, 168840, 0x8dc58c67d7a81365ull, 0x685ed8240ed0d999ull},
    {"AMCOMPUT", 16000, 1, 19152, 76608, 0x1656c28cbc96c009ull, 0xafc819d5600b8599ull},
    {"AMCOUNTR", 8000, 1, 28054, 224432, 0x5a743c96c0503465ull, 0xd0cac9c03431a0e5ull},
    {"AMCRKICE", 16000, 1, 23570, 94280, 0x6d66647461666619ull, 0x177b6c901cb1a841ull},
    {"AMDESERT", 16000, 1, 40864, 163456, 0xd9b492abd0db2d69ull, 0x996df914e408aef1ull},
    {"AMFACTR1", 16000, 1, 35938, 143752, 0xc2d2862fc363fba1ull, 0xb776ec4e8e03b301ull},
    {"AMFACTR2", 16000, 1, 30314, 121256, 0xb68a901febe57a15ull, 0xde6bb375d1ba6fb1ull},
    {"AMFACTR3", 16000, 1, 19108, 76432, 0x8edee9aead4c4bf9ull, 0x16a4d1d1c4d5f931ull},
    {"AMFACTR4", 16000, 1, 10184, 40736, 0x5f0f8bf7d3161ce5ull, 0x164290a8dc84b7ddull},
    {"AMFACTR5", 16000, 1, 33478, 133912, 0x29940f642119aefdull, 0xf228580c824158b1ull},
    {"AMFACTR6", 16000, 1, 28960, 115840, 0x2a06e9e298ca6779ull, 0x97987e0b09898f69ull},
    {"AMFACTR7", 16000, 1, 33910, 135640, 0x7822a1f799c86e59ull, 0xc681eb0a5e10ff81ull},
    {"AMFACTR8", 16000, 1, 8156, 32624, 0x926972aa404a938dull, 0x188395c5cb54d0e9ull},
    {"AMFACTR9", 16000, 1, 12952, 51808, 0x8e814e08940893d1ull, 0x441dd74c7992a2cdull},
    {"AMFAN", 16000, 1, 11976, 47904, 0x28ad91c56c7139e9ull, 0x68181dd9c6be91e1ull},
    {"AMFORAM1", 16000, 1, 31548, 126192, 0x96e685916258f411ull, 0x6d9e4e8200d6c4a5ull},
    {"AMFORAM2", 16000, 1, 37776, 151104, 0xa1b94bc5424dd605ull, 0x3e32aef5e60c94d1ull},
    {"AMFORAM3", 16000, 1, 37602, 150408, 0xa105051568758bc9ull, 0xb0db48b306e5e6c5ull},
    {"AMFORBUG", 16000, 1, 33162, 132648, 0xbe93b8ecd25c4c85ull, 0x6d8e8bf867833ad5ull},
    {"AMFORPM1", 16000, 1, 38920, 155680, 0xa3bbaa4dcf82fb41ull, 0x9c5edbfc4e7db82dull},
    {"AMFORPM2", 16000, 1, 34654, 138616, 0x5df912a1277cb3d5ull, 0xf6131f717f12965dull},
    {"AMFORPM3", 16000, 1, 34880, 139520, 0x6eb39a581e1536c5ull, 0x018cc7b044bda741ull},
    {"AMFROGS", 16000, 1, 32542, 130168, 0x0e4caaf89a4eda71ull, 0xd3594daeaacf6f09ull},
    {"AMFURNAC", 16000, 1, 29940, 119760, 0x094c851adf37a6e1ull, 0x541f8f3000aa5289ull},
    {"AMGAS", 32000, 1, 37246, 74492, 0x219bccf6d936d74dull, 0xba3f9f39000c5739ull},
    {"AMJUNGLE", 16000, 1, 40602, 162408, 0x9cc675649c0ba49dull, 0x3b555588d85679f1ull},
    {"AMKELLR1", 16000, 1, 39436, 157744, 0x1661bd173a29eecdull, 0xfc644f3c4627fdcdull},
    {"AMKELLR2", 16000, 1, 29258, 117032, 0x7989dc1a50a45a51ull, 0xc61f13692cb326e5ull},
    {"AMKELLR3", 16000, 1, 35004, 140016, 0xabdc7f79b38e76fdull, 0x6d163cd4c4063495ull},
    {"AMKELLR4", 16000, 1, 32216, 128864, 0xfc2643c428995519ull, 0x4cbc56f4373f0289ull},
    {"AMKELLR5", 16000, 1, 32022, 128088, 0x8865bc2ee15d03f9ull, 0x3d1d247bc0b38731ull},
    {"AMKELLR6", 16000, 1, 36748, 146992, 0xafc31d13da7db545ull, 0x81eeb1eae21b7719ull},
    {"AMKELLR7", 16000, 1, 27548, 110192, 0x0bfee15c8b0f1c95ull, 0xb154ee9de7db2ca1ull},
    {"AMKELLR8", 16000, 1, 48146, 192584, 0xb32ad4a38c9d89f5ull, 0x722c3805eeecf6a5ull},
    {"AMMACHN1", 16000, 1, 35252, 141008, 0x3979d3c074ccc4c9ull, 0xe76cbe2be662e745ull},
    {"AMMANUF1", 16000, 1, 34234, 136936, 0x9f3e039554cf50d1ull, 0xdb82f344dd751abdull},
    {"AMMANUF2", 16000, 1, 38040, 152160, 0xabdf15904626b6c9ull, 0x9c6b7251a0622741ull},
    {"AMMANUF3", 16000, 1, 21728, 86912, 0x7f3fe3278350b939ull, 0xbed216c842ad7d75ull},
    {"AMMARINA", 16000, 1, 33738, 134952, 0xb9defb3011e435a5ull, 0xab7aa02059f958c5ull},
    {"AMMETAL", 16000, 1, 34422, 137688, 0xd896f7fc77ebac55ull, 0x3d6c2f8c0793a935ull},
    {"AMMILL1", 16000, 1, 17792, 71168, 0xea44e19010159751ull, 0xfe4a8110d47923cdull},
    {"AMMILL2", 16000, 1, 34204, 136816, 0xf9376c6c6d9d6e11ull, 0x80399c79e70e1f05ull},
    {"AMMILL3", 16000, 1, 16568, 66272, 0x00defd69b409e055ull, 0x82abc63237f41ef5ull},
    {"AMMILL4", 16000, 1, 38796, 155184, 0x47e6cf90e1673615ull, 0x445981d32d2211c1ull},
    {"AMMILL5", 16000, 1, 31596, 126384, 0xfb854dc8eee7d5c1ull, 0xacdf2eb670366151ull},
    {"AMMILL6", 16000, 1, 31358, 125432, 0x0cff0aa3efb461a5ull, 0xdcf26e3fd3c745b9ull},
    {"AMMINE1", 16000, 1, 18856, 75424, 0x9287791aafb507cdull, 0x39e9acb64782e679ull},
    {"AMMINE2", 16000, 1, 21520, 86080, 0x04fae5f36fa8cd8dull, 0x6983c2a45ad2b1e9ull},
    {"AMMINE3", 16000, 1, 27800, 111200, 0xbcfc9aaddbeb9f3dull, 0x7fa8f11a4a1feb45ull},
    {"AMMINE4", 16000, 1, 36900, 147600, 0x318f8888dae46231ull, 0xf000157468d93235ull},
    {"AMMINE5", 16000, 1, 22378, 89512, 0xac5e4372266ebe61ull, 0xc8eb704ee8c4e495ull},
    {"AMMINE6", 16000, 1, 35160, 140640, 0x69beb2df98230e69ull, 0x22f9e48b5e3f34f9ull},
    {"AMMINE7", 16000, 1, 27098, 108392, 0x5d30d187c8929a99ull, 0x197dfb8fb33dd3e5ull},
    {"AMMINE8", 16000, 1, 36336, 145344, 0xe30499fa754f5159ull, 0x3c33e9e3d05c6895ull},
    {"AMMINE9", 16000, 1, 19648, 78592, 0xa7598b27bda7acf9ull, 0xa1c5e93c571be329ull},
    {"AMNIGHT1", 16000, 1, 34848, 139392, 0x88d5b399e2456045ull, 0x061d20aeaa3e4addull},
    {"AMNIGHT2", 16000, 1, 32194, 128776, 0x1d2dc91fdcf4a8a5ull, 0x74f5ff5339602b75ull},
    {"AMOFFICE", 16000, 1, 10322, 41288, 0x54e102ed2b8add65ull, 0xf959ad031c735569ull},
    {"AMPIER", 16000, 1, 35532, 142128, 0xcf7dbe690dadce1dull, 0xa1d39590de1b4f9dull},
    {"AMPORT", 16000, 1, 34074, 136296, 0x4d6b8a53a3606079ull, 0x61eb25ddb38f7265ull},
    {"AMPOWER1", 16000, 1, 32090, 128360, 0x427baf538720922dull, 0x7a0d14f7953ae8a5ull},
    {"AMPOWER2", 16000, 1, 9678, 38712, 0xc6a024afba05af19ull, 0x0bdcba7853faaab1ull},
    {"AMPOWER3", 16000, 1, 34096, 136384, 0xce0f333ea06b75d1ull, 0xdab104432f87ac89ull},
    {"AMPOWER4", 16000, 1, 28014, 112056, 0x48720b1504854255ull, 0xc53acc6cd32fdf5dull},
    {"AMPOWER5", 16000, 1, 32274, 129096, 0x527c897f25d41819ull, 0x01554ab47e778161ull},
    {"AMPOWER6", 16000, 1, 23308, 93232, 0xa0d540d9a286a3a5ull, 0x32dabf22a8fcb701ull},
    {"AMPOWER7", 16000, 1, 18620, 74480, 0xd40405e2d4d8e7e9ull, 0x7c7d2e4ccf028bd5ull},
    {"AMPOWER8", 16000, 1, 33118, 132472, 0x62f4a1892d0ee805ull, 0x095f7227602729d9ull},
    {"AMPOWER9", 16000, 1, 21062, 84248, 0x16134dfbdf9d8c35ull, 0x139dc29c119ce321ull},
    {"AMPUMP", 16000, 1, 28310, 113240, 0x6f4e5d317bb087e9ull, 0x4a0c6c325ea946a5ull},
    {"AMPUMP1", 16000, 1, 32024, 128096, 0x4e434de8e961bfa1ull, 0xc500e994e2edc4c1ull},
    {"AMPUMP2", 16000, 1, 27434, 109736, 0x54695d7bc956caddull, 0x845a6383eff1657dull},
    {"AMRAIN", 16000, 1, 31964, 127856, 0xefbe9d4de2813f85ull, 0xaaba5e62009b1c1dull},
    {"AMRAIN2", 16000, 1, 34004, 136016, 0x9d4cdf7e6e844311ull, 0xd4d20a5c563f9045ull},
    {"AMRAIN3", 16000, 1, 33084, 132336, 0x496708e86a229631ull, 0x294be522d8179821ull},
    {"AMRAIN4", 16000, 1, 37844, 151376, 0x24bfe5de70f319b5ull, 0x8521deb3d5402b4dull},
    {"AMRAIN5", 16000, 1, 36196, 144784, 0xc5eaaa39289913a5ull, 0xb25fa76566555c81ull},
    {"AMRAIN6", 16000, 1, 33322, 133288, 0x946458a2e4c367c5ull, 0xd28cee6275db858dull},
    {"AMRAIN7", 16000, 1, 37898, 151592, 0x3fd2c50d81537c6dull, 0xe02ec1b2e1c709cdull},
    {"AMRAIN8", 16000, 1, 32814, 131256, 0x0f42294a6aed92edull, 0x502a78840960889dull},
    {"AMRAIN9", 16000, 1, 32296, 129184, 0x9e9fceb8bf8ddab9ull, 0x3f356d7373759965ull},
    {"AMREFNRY", 16000, 1, 36926, 147704, 0xdd683b9c08eb213dull, 0xf6fe4006eaca8ba1ull},
    {"AMROAR", 16000, 1, 21536, 86144, 0x95af97911843bca9ull, 0xdd1bed6d1e279f4dull},
    {"AMSBURB2", 16000, 1, 35596, 142384, 0xd5cf5335d8873c99ull, 0xa3b11d5453edd53dull},
    {"AMSBURB3", 16000, 1, 43946, 175784, 0x8932ac33b8b186b1ull, 0xd5e88f99e85ba51dull},
    {"AMSCRPYD", 16000, 1, 12374, 49496, 0x25495833271dafe1ull, 0x5a371e060f911685ull},
    {"AMSPACE", 8000, 1, 5128, 41024, 0xb202a50b2da3b095ull, 0xd8a3c5def4078c55ull},
    {"AMSPACE1", 16000, 1, 36340, 145360, 0x6dbbb1c09f9b37e1ull, 0x6cf483380a7c3671ull},
    {"AMSPACE2", 16000, 1, 34656, 138624, 0x5741cc958d211379ull, 0x7430e0bbf19a3ff1ull},
    {"AMSTEAM", 16000, 1, 18620, 74480, 0xb0ab0add4860bd4dull, 0x61d802ffa663d9f9ull},
    {"AMSUBURB", 16000, 1, 25382, 101528, 0x636e36c780a44bbdull, 0xb3791353050ba805ull},
    {"AMTUNNL1", 16000, 1, 36718, 146872, 0x5d23cedfa632aca9ull, 0x346c17b92ef11ae5ull},
    {"AMTUNNL2", 16000, 1, 35686, 142744, 0x9ca395974477ab69ull, 0xbcf25a30d182e175ull},
    {"AMTUNNL3", 16000, 1, 36736, 146944, 0x501300880eb7eae5ull, 0x8ca11529140a0be9ull},
    {"AMUNDERW", 8000, 1, 7850, 62800, 0xbbd75aef9aae4ff5ull, 0xf430f9781f019d2dull},
    {"AMVARCOL", 16000, 1, 31376, 125504, 0xd161a3e171f43e51ull, 0x3b64a7e09a71ec9dull},
    {"AMVAUTOS", 16000, 1, 26416, 105664, 0x6c3a6f9a3e282589ull, 0x152395ee79004e49ull},
    {"AMVBRIEF", 16000, 1, 23248, 92992, 0x3bf691f79835ebc5ull, 0xeb4d38d0d8e44399ull},
    {"AMVCAUTN", 16000, 1, 10256, 41024, 0x342e55f3ba066df1ull, 0xb538ee84336cae11ull},
    {"AMVCOREB", 16000, 1, 18254, 73016, 0x665121ae1f064c21ull, 0x085298840322b3edull},
    {"AMVDISID", 16000, 1, 32416, 129664, 0xb8f87eca646b0701ull, 0x8ddffa6b4a2ef351ull},
    {"AMVEARTH", 16000, 1, 27304, 109216, 0x8db67e4fd451f465ull, 0x220a4765432c5531ull},
    {"AMVEVACU", 16000, 1, 27870, 111480, 0x56860db3ccc11bc1ull, 0xe17964335e621439ull},
    {"AMVEVERY", 16000, 1, 33026, 132104, 0x09e95fdb10d94935ull, 0xfd0d91b5199916e5ull},
    {"AMVFIRES", 16000, 1, 21556, 86224, 0x848ca2f58890429dull, 0xca2456bd3a5a41fdull},
    {"AMVGMORN", 16000, 1, 25930, 103720, 0xd3dc1dbfa069d209ull, 0xf38988a945ebd261ull},
    {"AMVHCHEM", 16000, 1, 22152, 88608, 0xa39a825d127ff095ull, 0x641a7cfcdb700b9dull},
    {"AMVINSUR", 16000, 1, 30980, 123920, 0x2c2d8ab9b58e87d1ull, 0xfdffbcc79f1f1949ull},
    {"AMVKYOTO", 16000, 1, 18752, 75008, 0xd7c28a9834e48871ull, 0x21f51723fa08cf4dull},
    {"AMVLIFEG", 16000, 1, 19158, 76632, 0x6f94a33aeda4b715ull, 0x093c82536f3e5139ull},
    {"AMVLOADZ", 16000, 1, 17038, 68152, 0x8f00175524c18c09ull, 0xd8b9b4bd78031c01ull},
    {"AMVLOCKD", 16000, 1, 23862, 95448, 0x1c8d5ce9ad990599ull, 0x83c3767021530361ull},
    {"AMVMINE", 16000, 1, 48020, 192080, 0x2ad5f930277088d9ull, 0x2d9bb1389bf3287dull},
    {"AMVMOVAL", 16000, 1, 18076, 72304, 0xc59f80c9be8288d1ull, 0x13e50d25f69dba3dull},
    {"AMVNELEV", 16000, 1, 19956, 79824, 0x98b2bdbddeac3c79ull, 0x94c851f29de6bb39ull},
    {"AMVNICED", 16000, 1, 16630, 66520, 0xd8885ba5bffd7bc1ull, 0x992b8adb43c130b1ull},
    {"AMVNKIDS", 16000, 1, 33030, 132120, 0xfa14bf3492c2c16dull, 0x469f261e60fd67d9ull},
    {"AMVNOEQA", 16000, 1, 21996, 87984, 0x601ab5b1459f4db5ull, 0xabfd48b770269071ull},
    {"AMVNPARK", 16000, 1, 14796, 59184, 0x63001bc24e222a71ull, 0xf6c7df9653bf01a1ull},
    {"AMVNSAFE", 16000, 1, 32328, 129312, 0x7a324f00afb3f3c9ull, 0x748d08abdbd1f90dull},
    {"AMVNWEAP", 16000, 1, 18468, 73872, 0x50f464f270d9ec8dull, 0xe38df8c167e221c5ull},
    {"AMVPDNOT", 8000, 1, 8352, 66816, 0xda77557c04561b8dull, 0xec1c775c456f50f5ull},
    {"AMVPGATL", 16000, 1, 20430, 81720, 0x78a9c3179b12e5b9ull, 0xba821418da18cda5ull},
    {"AMVPGATR", 16000, 1, 20000, 80000, 0x94124d370e13e4e1ull, 0xf301baa0a2f6c769ull},
    {"AMVPLEAS", 16000, 1, 11182, 44728, 0xcf8d3af1809d2e01ull, 0x8bf0bba6efeaa2c5ull},
    {"AMVREFUS", 16000, 1, 32938, 131752, 0x9acdf9a803cba781ull, 0x545f1e4f7801a445ull},
    {"AMVSHAFT", 16000, 1, 25264, 101056, 0xcf3a8364724f3a3dull, 0x74cfee1ec8bb429dull},
    {"AMVSLIPY", 16000, 1, 20508, 82032, 0xf6416953375c79f5ull, 0x749f9ec65d9a9719ull},
    {"AMVSMOKE", 16000, 1, 16176, 64704, 0x735204134d3244cdull, 0x801efacd37f85c15ull},
    {"AMVSORRY", 16000, 1, 24618, 98472, 0x5e93f9d577317271ull, 0x6f6d49d39ff9531dull},
    {"AMVTANKS", 16000, 1, 15838, 63352, 0xfdab86e93173a655ull, 0x9d12aa4786444a15ull},
    {"AMVTDCOM", 16000, 1, 18780, 75120, 0xc1503dd7217c5e49ull, 0x2ac5e92c4d929d49ull},
    {"AMVTRAIN", 16000, 1, 22986, 91944, 0xef0355c4fba5ad4dull, 0x14b0deb4b9928d41ull},
    {"AMVVENT", 16000, 1, 23824, 95296, 0xa88be6cc0238e9adull, 0xfee36a71c38b5701ull},
    {"AMVVISIT", 16000, 1, 19040, 76160, 0x68cfec35b943ed81ull, 0xf318056d37ac4365ull},
    {"AMVWARNG", 16000, 1, 7752, 31008, 0x32dd6ebf959c9e31ull, 0x1e3c87a29d1477f1ull},
    {"AMWHINE", 16000, 1, 32526, 130104, 0x74c2cbe51207be3dull, 0xd319707ed1d276edull},
    {"AMWINDH", 16000, 1, 35922, 143688, 0xac0c5db847d9a031ull, 0x51b000b835d29fd9ull},
    {"AMWINDT", 16000, 1, 35932, 143728, 0xfbf6f832bea77d71ull, 0x6382cfc227390499ull},
    {"ARGPREZ", 16000, 1, 35792, 143168, 0x2f5a289123671f81ull, 0x40ace232d82ca729ull},
    {"BIGLASFR", 16000, 1, 24064, 96256, 0x734d7bf29eb6c049ull, 0x4d2ab1fbdd9e02d1ull},
    {"BIGLASLP", 16000, 1, 24064, 96256, 0x95359c409c782bbdull, 0x1f66440ccf1bf05dull},
    {"BIRDCHIC", 8000, 1, 7570, 60560, 0xf67dd96838069f95ull, 0x282f7849917bb375ull},
    {"BIRDCRO2", 8000, 1, 1304, 10432, 0x4737faee9af2feadull, 0xdff5ec2d475e9b35ull},
    {"BIRDCROW", 16000, 1, 4636, 18544, 0xd0fab9a15bae9295ull, 0x004f68e563725701ull},
    {"BIRDFLYC", 8000, 1, 8188, 65504, 0x61bacc2050f3036dull, 0x6af94349586e3105ull},
    {"BIRDKING", 8000, 1, 1960, 15680, 0x9776b030a8120455ull, 0x4f857de284a3492dull},
    {"BIRDUNK1", 16000, 1, 3776, 15104, 0x7d6e7e9e41068205ull, 0x5dd438a7575e74e1ull},
    {"BIRDUNK2", 8000, 1, 10762, 86096, 0x6f349307a61a9f65ull, 0x745514e4918c58edull},
    {"BIRDUNK3", 8000, 1, 3862, 30896, 0xb55c66936ca9da9dull, 0x372e8d7c39002125ull},
    {"BRAINW", 16000, 1, 40364, 161456, 0x0c129d47860baa81ull, 0xabf6f12fb2858531ull},
    {"BUBBLES", 8000, 1, 2344, 18752, 0xb61f8a0ba91606bdull, 0x06f1d66a2d37cd05ull},
    {"BUGOUT", 16000, 1, 33562, 134248, 0x2a16c7cd520871d5ull, 0x6164290b1df28ef9ull},
    {"BUZZHUM", 8000, 1, 3186, 25488, 0x6835cae218a6ef7dull, 0x4c21137a9968501dull},
    {"BUZZTHIN", 16000, 1, 7520, 30080, 0x869a1ad0baad18a9ull, 0x7c4ce720051bf055ull},
    {"C-MESON", 16000, 1, 24000, 96000, 0x3d617594b6758dddull, 0xd9057f51116e4455ull},
    {"C-RR", 16000, 1, 24000, 96000, 0x1505cb25febc7009ull, 0x50483e47ab34bfbdull},
    {"CAMOFFH", 16000, 1, 12298, 49192, 0x01e608854a8b52adull, 0x3566e1f7fac2267dull},
    {"CAMOFFL", 16000, 1, 12668, 50672, 0xa107e0034d0d3695ull, 0xa3588fbf72de5bd1ull},
    {"CAMOFFM", 16000, 1, 12308, 49232, 0x18093dbb304c8985ull, 0x6979a942234ca08dull},
    {"CAMONH", 16000, 1, 12304, 49216, 0x8493fba4da813b9dull, 0x1664f29cba0c8a45ull},
    {"CAMONL", 16000, 1, 12668, 50672, 0xe36aff506f055a19ull, 0xc4aa9624bb4e2911ull},
    {"CAMONM", 16000, 1, 12308, 49232, 0x9947cc4c7ff7a53dull, 0x444424852bf2b7cdull},
    {"CHANDELR", 16000, 1, 11066, 44264, 0x9e9d23f244fb1cbdull, 0x7e4b4b14b9a81175ull},
    {"CLANG1", 16000, 1, 5600, 22400, 0x9efe6f72b9db2d0dull, 0xdeac10936099d1c5ull},
    {"CLANG2", 16000, 1, 6054, 24216, 0x9157ad1196ef1f31ull, 0x3a3970f3d1e20c79ull},
    {"CLANG3", 16000, 1, 9202, 36808, 0x827fbd674c1f8ce5ull, 0x0fae9e2d154f3d4dull},
    {"CLANG4", 16000, 1, 7504, 30016, 0x7b36f4d0a2967ad9ull, 0x9c8da5a7c13e1685ull},
    {"CLATTER", 16000, 1, 3020, 12080, 0x467dbf3cd2d2c18dull, 0xe69823fade235255ull},
    {"CLICKBLT", 16000, 1, 478, 1912, 0xefbeb12cc846364dull, 0x8fb6f27dc2626061ull},
    {"CONVEYR2", 16000, 1, 22422, 89688, 0xb1ec09d56db8cdfdull, 0x3fb35b8bdd49a869ull},
    {"CONVEYR3", 16000, 1, 22478, 89912, 0x9e430d11e8bff985ull, 0x0c48d5d5237bd845ull},
    {"CONVEYRC", 16000, 1, 9086, 36344, 0x9608773dc6470b29ull, 0x6634b8d7d684ec65ull},
    {"CONVEYRL", 16000, 1, 9168, 36672, 0xc10effc069c852a1ull, 0x8579018207011f69ull},
    {"CONVEYRO", 16000, 1, 4976, 19904, 0xec2ee2dad99516bdull, 0x868f8b0d7742160dull},
    {"COOLNEAT", 16000, 1, 2678, 10712, 0x43fe37b99e1a4a4dull, 0xf507d811ecdbf4c5ull},
    {"COOLWHIP", 16000, 1, 9500, 38000, 0x9b01c63c64772075ull, 0x8a3443a920f4a9a1ull},
    {"COPTER", 8000, 1, 13412, 107296, 0xd784c23aa8b59d45ull, 0xb98f971411845c15ull},
    {"CRKMETAL", 16000, 1, 19582, 78328, 0x0e43a91e93d613b9ull, 0x1c46bf03a3330385ull},
    {"CRUNCH1", 16000, 1, 9756, 39024, 0x0bf0d2b825fd3835ull, 0xd59ea99084eb62d5ull},
    {"CRUNCH2", 16000, 1, 12792, 51168, 0xbbba3bb668c454d1ull, 0xa17e27fab6d645a5ull},
    {"CRUNCH3", 16000, 1, 12848, 51392, 0x1655c3fc7230e6cdull, 0x152a60c87caeab95ull},
    {"CRUNCH4", 16000, 1, 9584, 38336, 0x00e3c5362349bf51ull, 0x6a30a67a41f9965dull},
    {"CULTCHNT", 16000, 1, 22340, 89360, 0xb97f3ba4128fa9a1ull, 0x96b608d3f1f2068dull},
    {"CULTCOM1", 8000, 1, 3902, 31216, 0xebb82dc049f5191dull, 0x5f195150e945e95dull},
    {"CULTCOM2", 8000, 1, 3708, 29664, 0xa2148fcf22b2c995ull, 0xbdcdfb77bb49b05dull},
    {"CULTCOM3", 8000, 1, 6818, 54544, 0xdeb28db6cd704c5dull, 0xd43d27e4810d5ecdull},
    {"CULTDIE", 16000, 1, 13758, 55032, 0xcb02724e6fb9495dull, 0x694bf7b6b8aeb621ull},
    {"CULTMORE", 16000, 1, 22316, 89264, 0x82c1848b3d8f9e9dull, 0x4225b29a5e5a339dull},
    {"CUT-EMP", 32000, 2, 1629212, 1629212, 0x689ce6f75d8162feull, 0xd1ff4f7e31ea0321ull},
    {"CUT-PAM", 32000, 2, 2389256, 2389256, 0x64e21d45ad858491ull, 0x1eb681dc109ef1fcull},
    {"CUT-PREZ", 32000, 2, 2509924, 2509924, 0x0bbe14f5648a7d52ull, 0xfd03fb50626422d4ull},
    {"DOOR1C", 16000, 1, 4054, 16216, 0xc66697fa60be88c9ull, 0x856051e595c02845ull},
    {"DOOR1L", 16000, 1, 5766, 23064, 0xa4d09be1b030bb35ull, 0xf48f4d1069276c91ull},
    {"DOOR1O", 16000, 1, 3430, 13720, 0x2479c3b31939afd5ull, 0x9c84e73fa0669c49ull},
    {"DOOR2C", 16000, 1, 10762, 43048, 0xcc21d68979005c1dull, 0x68b5bafd0e8d5515ull},
    {"DOOR2L", 16000, 1, 3508, 14032, 0x74dc72f4ed547a69ull, 0x890b71b216842e75ull},
    {"DOOR2O", 16000, 1, 878, 3512, 0x4e9a657c97a3f245ull, 0x9131115b1f93b6a9ull},
    {"DOOR2SP", 16000, 1, 1836, 7344, 0x93325d9d8b642599ull, 0x1261bfff0e2437c9ull},
    {"DOOR2ST", 16000, 1, 3010, 12040, 0xd6f2449c09c412b9ull, 0x541f2270c9eb0875ull},
    {"DOOR3C", 16000, 1, 4866, 19464, 0x3756e2690444c9bdull, 0x75a1b8d9b4d40621ull},
    {"DOOR3L", 16000, 1, 6340, 25360, 0x39a799a49e5ec8b1ull, 0xc9611799c3bb16a9ull},
    {"DOOR3O", 16000, 1, 1134, 4536, 0x0aff8ae219421fe9ull, 0xa72d91fc39056a6dull},
    {"DOOR4C", 16000, 1, 16038, 64152, 0xf08e5d7fbd6a9b79ull, 0x1b1b245e45218619ull},
    {"DOOR4L", 16000, 1, 5992, 23968, 0xdc2b78165af809fdull, 0x8979e4e9d2b09c19ull},
    {"DOOR4O", 16000, 1, 5138, 20552, 0xf6a927f143e93999ull, 0x09f538d1672e9655ull},
    {"DOOR5C", 16000, 1, 9072, 36288, 0xf96ddc48feda50b1ull, 0x3d49803726dfb825ull},
    {"DOOR5L", 16000, 1, 11658, 46632, 0xa34cb93dff2051c9ull, 0x7932079ce2ba22d5ull},
    {"DOOR5O", 16000, 1, 7330, 29320, 0x4c394eb883d92a25ull, 0x47bb99aebfd08bb5ull},
    {"DOOR9C", 16000, 1, 25712, 102848, 0xd046bdc8e012930dull, 0x32decd5ce1dc5a31ull},
    {"DOOR9L", 16000, 1, 4152, 16608, 0x27732a00a82935c5ull, 0xd2f14dc0ce2830f1ull},
    {"DOOR9O", 16000, 1, 13560, 54240, 0x3efbce3d4868c02dull, 0x6b5e81e136797251ull},
    {"DOORF1SP", 16000, 1, 1528, 6112, 0x2b8cf6bf5099015dull, 0xab46d7c9d4f16aadull},
    {"DOORF1ST", 16000, 1, 4840, 19360, 0x7865ae698ab8f935ull, 0x2264191cb34c9885ull},
    {"DOORFSP2", 8000, 1, 572, 4576, 0x837ff2a2316a14d5ull, 0xd0cc105cf3eb86bdull},
    {"DOORFSP3", 8000, 1, 502, 4016, 0x566d284958c85f75ull, 0xf38e7614b1096dbdull},
    {"DOORFST", 8000, 1, 1414, 11312, 0x02e9e833b397922dull, 0x89117f21c05d7795ull},
    {"DOORFST2", 8000, 1, 1086, 8688, 0x1b81d577ea7c0675ull, 0x56b6deeaf4637af5ull},
    {"DOORFST3", 8000, 1, 468, 3744, 0xb2a7fac365e5e0c5ull, 0x1031b5b567d77045ull},
    {"DOORFSTP", 8000, 1, 628, 5024, 0x368d672cee5d1fadull, 0x8940e854959b8855ull},
    {"DOORNOPN", 8000, 1, 3670, 29360, 0x838040790f606255ull, 0x40f2c58972a0631dull},
    {"DOORPIPC", 16000, 1, 11986, 47944, 0xf63a15b43b8eac81ull, 0x46db589ef67d4b05ull},
    {"DOORPIPL", 16000, 1, 4684, 18736, 0x351b1efae8c232b1ull, 0xf0665e36ddf731e1ull},
    {"DOORPIPO", 16000, 1, 4146, 16584, 0xfa1d6c04bc3562b1ull, 0x556754aaf307ea7dull},
    {"DOORPS1", 16000, 1, 10788, 43152, 0x8f5a2cb0d4cf0a15ull, 0xda98d6096ec956ddull},
    {"DOORROCK", 16000, 1, 18538, 74152, 0x1187db66890d4e6dull, 0x4ba67642c1fa0b79ull},
    {"DOORSM", 8000, 1, 598, 4784, 0x2e697ba2f893ea6dull, 0x13b75453919cc1b5ull},
    {"DOORSM2", 16000, 1, 4060, 16240, 0x0fb3f8d5b0efcfb5ull, 0xb26ef23264888789ull},
    {"DOORWL", 16000, 1, 11638, 46552, 0x1470b79d1ca0e83dull, 0x7a37855aa05da6fdull},
    {"DRILL", 8000, 1, 2032, 16256, 0x9777bc816443a68dull, 0x15d79f8746cc8abdull},
    {"DRONEST", 16000, 1, 10288, 41152, 0x382ca3f3b4ee1481ull, 0xfb0fbb48bad9971dull},
    {"ELECARC", 16000, 1, 22780, 91120, 0x513d13e5b9775741ull, 0xf0297f3dfea5ba61ull},
    {"ELEV1C", 16000, 1, 11572, 46288, 0x714027e24cfc676dull, 0x9947f83e414e257dull},
    {"ELEV1L", 16000, 1, 9530, 38120, 0xb5eda0d54cb99815ull, 0x8dbd3ad2a021bd2dull},
    {"ELEV1O", 16000, 1, 8674, 34696, 0xffbede8990bce509ull, 0x250e6c820229f6adull},
    {"ELEV2C", 8000, 1, 5198, 41584, 0x6b84a51767ba5fa5ull, 0x193b701827ab1775ull},
    {"ELEV2L", 8000, 1, 1406, 11248, 0x0e2294c0225b70ddull, 0x9316334d08fc1df5ull},
    {"ELEV2O", 8000, 1, 5198, 41584, 0xb0978c8b10429055ull, 0x81115a122fe5d895ull},
    {"EXP1S", 16000, 1, 6450, 25800, 0x6358077af0404715ull, 0xd29ac8fe73ab407dull},
    {"EXP2M", 16000, 1, 18502, 74008, 0x70fc76fa6b83b6b1ull, 0xd571b8073f911631ull},
    {"EXP3L", 16000, 1, 31942, 127768, 0x02b1cd9b28ed3c05ull, 0x7d71d3272b5a4929ull},
    {"EXP3LNEW", 16000, 1, 31942, 127768, 0xa0823b8020f74369ull, 0xe9aeda85a8890371ull},
    {"EXP4N", 16000, 1, 59142, 236568, 0x09b87ef74fce89d1ull, 0x6745de5d6fa843f9ull},
    {"EXP4NEW", 16000, 1, 59142, 236568, 0xe234fe13a4719fcdull, 0x89e82d25a4399b11ull},
    {"EXPL0", 16000, 1, 44454, 177816, 0xf1128d1950d39d19ull, 0x7f8725a51108f0d1ull},
    {"EXPL1", 16000, 1, 18526, 74104, 0x0b937af192617a2dull, 0xe67c2d40e2275fa1ull},
    {"EXPL1NEW", 16000, 1, 18526, 74104, 0x8d58a5f1cb4d06d5ull, 0x6be1a29b12a75ad1ull},
    {"EXPL2", 16000, 1, 33176, 132704, 0x35c4782fadc470edull, 0x0111db786a0f1d35ull},
    {"EXPL2NEW", 16000, 1, 33176, 132704, 0x5f10b1b8c87cdd51ull, 0x5a7c2e1db69b261dull},
    {"EXPL3", 16000, 1, 39118, 156472, 0x2f0d768084d024d5ull, 0x808e6cc0af284835ull},
    {"EXPL3NEW", 16000, 1, 39118, 156472, 0x8da76064a13f1325ull, 0xb428042f28e8fa11ull},
    {"EXPL4", 16000, 1, 43890, 175560, 0x9a19b441caa880a1ull, 0xdfe4c0411b149f6dull},
    {"EXPL4NEW", 16000, 1, 43890, 175560, 0x12473bf11f599c09ull, 0xf798c36e1a2a27b9ull},
    {"EXPL5", 16000, 1, 26322, 105288, 0x7e9b27b73f3aae95ull, 0xce77122767398be9ull},
    {"EXPL5NEW", 16000, 1, 26256, 105024, 0x3594c6f4dd0ac0bdull, 0xc8a9e9649e6d7e25ull},
    {"EXPL6", 16000, 1, 48012, 192048, 0x9ab53be226d04831ull, 0x876111130f232f85ull},
    {"EXPL6NEW", 16000, 1, 48012, 192048, 0xd3b498c2faff58e5ull, 0x9789204061b64879ull},
    {"EXPL7", 16000, 1, 42694, 170776, 0x0e79d198f1ea6925ull, 0x64236ffc83eabb19ull},
    {"EXPL7NEW", 16000, 1, 42694, 170776, 0x47041655781cebedull, 0x6f26b78f25c55c3dull},
    {"EXPL8", 16000, 1, 61044, 244176, 0x3087cdbc433b5fe1ull, 0x9a496941656926a9ull},
    {"EXPL8NEW", 16000, 1, 61044, 244176, 0x198d2d94404ff269ull, 0x849233546108584dull},
    {"EXPL9", 16000, 1, 42990, 171960, 0xd033c7fb83ccc2f5ull, 0x646b51cee3d8b3ddull},
    {"EXPM1", 16000, 1, 24000, 96000, 0xdf7adfe93a9be931ull, 0x1e9a39e523d56735ull},
    {"EXPM1NEW", 16000, 1, 48276, 193104, 0x73a44f341ca5abf9ull, 0x7a9b3890776fbc95ull},
    {"EXPM2", 16000, 1, 25746, 102984, 0x1e2f858a89b4ce6dull, 0x1c6daf1f61dbf67dull},
    {"EXPM2NEW", 16000, 1, 24884, 99536, 0xa00c085270ad95d9ull, 0xa611a3a0f385b9f1ull},
    {"EXPM3", 16000, 1, 20066, 80264, 0xc97e5d079d53f1a9ull, 0x899b6995f95d18b5ull},
    {"EXPM3NEW", 16000, 1, 18980, 75920, 0xc5cbd2a8e1f788d9ull, 0xcf55975e30d4060dull},
    {"EXPM4", 16000, 1, 16000, 64000, 0x62bf3dff6088fdddull, 0x5832e89c73af1855ull},
    {"EXPM4NEW", 16000, 1, 26204, 104816, 0x682b27dae9f2dda5ull, 0xbe0da7a35e2a9379ull},
    {"EXPM5", 16000, 1, 25716, 102864, 0x490fe0083a3edf6dull, 0x2535fd353a9c1d91ull},
    {"EXPM5NEW", 16000, 1, 25716, 102864, 0x4b6a11a1ce038919ull, 0x428fccaa57334739ull},
    {"EXPM6", 16000, 1, 22148, 88592, 0x2a410d2ef807ee31ull, 0x0c33b3195dcd63d1ull},
    {"EXPM6NEW", 16000, 1, 22148, 88592, 0x8ac43e044ab7e52dull, 0x5ee20d4b3254ea85ull},
    {"EXPM7", 16000, 1, 14740, 58960, 0x4cd80a496c6702b5ull, 0xc13e4bf8d2debb75ull},
    {"EXPM7NEW", 16000, 1, 44454, 177816, 0xf1128d1950d39d19ull, 0x7f8725a51108f0d1ull},
    {"EXPM8", 16000, 1, 16000, 64000, 0xd578ed8b525dca41ull, 0x2a5009375b48b41dull},
    {"EXPM8NEW", 16000, 1, 34660, 138640, 0xfc74b20c4d602115ull, 0xb1b9a6f9add6ff25ull},
    {"EXPM9", 16000, 1, 16000, 64000, 0x6a43028d8a20a681ull, 0x22a5d377611997a9ull},
    {"EXPM9NEW", 16000, 1, 27340, 109360, 0x58d99b38fb8a59a9ull, 0x2557d28526cb6c51ull},
    {"EXPS1", 16000, 1, 16000, 64000, 0x120cbd46f023e56dull, 0x04077acbe11c8ad5ull},
    {"EXPS1NEW", 16000, 1, 24396, 97584, 0x777a3ccef3dc900dull, 0x7752a634aec7aea5ull},
    {"EXPS2", 16000, 1, 9526, 38104, 0x2cb4037fe66c810dull, 0xf06ef63a8623ad69ull},
    {"EXPS2NEW", 16000, 1, 9526, 38104, 0x684eaa2245df3771ull, 0x4156ca2556426c1dull},
    {"EXPS3", 16000, 1, 22614, 90456, 0x3b85bddcdb6a9cc1ull, 0xe7ad194f54c900d1ull},
    {"EXPS3NEW", 16000, 1, 21358, 85432, 0xd987bb36b50fc8b5ull, 0x4632733f19c6613dull},
    {"EXPS4", 16000, 1, 16000, 64000, 0x789d56da574fb6e1ull, 0x06fe1c1945269bd1ull},
    {"EXPS4NEW", 16000, 1, 22894, 91576, 0xe6d304e5b1f5d92dull, 0x70ead2cf94393195ull},
    {"F-AC", 32000, 1, 5334, 10668, 0x3d312c2d2eb3d63cull, 0x4f3a1bbba7512031ull},
    {"F-AS", 16000, 1, 16000, 64000, 0x49629a5cd3c985c9ull, 0xbe7fd49469ba3391ull},
    {"F-CAN", 32000, 1, 16000, 32000, 0x4d7f112dc2e28ecdull, 0x9a89b9b44eab3f9full},
    {"F-CES", 16000, 1, 15240, 60960, 0xe9c6f5e51033c34dull, 0xb749ca3c3f10b0edull},
    {"F-CHAFF", 16000, 1, 5326, 21304, 0xa6e89ab8e469d389ull, 0xc9b1fd047e25db09ull},
    {"F-DF", 16000, 1, 14494, 57976, 0x9eb21ab56cf57169ull, 0x137cbc96febc3199ull},
    {"F-DFOLD", 16000, 1, 4246, 16984, 0x75820e9edfa1e9b1ull, 0x3af9f3c186d696fdull},
    {"F-DISS", 16000, 1, 7998, 31992, 0x10b862b2a4faf2d5ull, 0x706d3d54d809b50dull},
    {"F-DPFB", 16000, 1, 5622, 22488, 0x7e53601e629053a1ull, 0xca317f1f7de0fc41ull},
    {"F-DPL", 16000, 1, 2000, 8000, 0xba9d20997766b789ull, 0x425560da7e9c4bcdull},
    {"F-EMP", 16000, 1, 12000, 48000, 0x1eea475450b6e6adull, 0x45882365e8e8d039ull},
    {"F-FBL", 16000, 1, 8000, 32000, 0xb35485e0d135a595ull, 0x4947f8d592eba1f1ull},
    {"F-FBS", 16000, 1, 6702, 26808, 0x544f39cbb82f5011ull, 0x4d1c1e8413d4af1dull},
    {"F-FLARE", 16000, 1, 3178, 12712, 0x51cc5b301c5b1061ull, 0x20ac886740f64189ull},
    {"F-FTL", 32000, 1, 16232, 32464, 0x032e712103b21b4eull, 0xb2090b731e0eb8e7ull},
    {"F-FTS", 16000, 1, 3454, 13816, 0xbca7ecbf0d34915dull, 0xac39b5111a4a6f91ull},
    {"F-GBL", 16000, 1, 5922, 23688, 0xaeace82e997262b9ull, 0x84526483804c25f1ull},
    {"F-GBS", 16000, 1, 1682, 6728, 0xdf3c232e999078c5ull, 0x7da849cf00f362f5ull},
    {"F-GL", 16000, 1, 4000, 16000, 0x93a23287363468ddull, 0xfaa240fa9bb19cadull},
    {"F-GOLIAT", 32000, 1, 3416, 6832, 0xe9a4f2b32f526a73ull, 0x11e6f6cbbfe2260aull},
    {"F-LASL", 16000, 1, 8000, 32000, 0xfba008f0b0a8ca75ull, 0x2bae1b33b6ee3911ull},
    {"F-LASS", 16000, 1, 3832, 15328, 0x907079acb88449e1ull, 0x37cc5c27d280d159ull},
    {"F-MESON", 16000, 1, 3656, 14624, 0x709c95dc87ab755dull, 0x5171583e1857948dull},
    {"F-MG", 16000, 1, 1600, 6400, 0x4c3c25d02c0c9789ull, 0x4c010af4398a66b9ull},
    {"F-MISS", 16000, 1, 6594, 26376, 0x14c2074e01a93985ull, 0xba3c798e98eac73dull},
    {"F-MISSL", 16000, 1, 3960, 15840, 0xaa1c045ee1735bb9ull, 0xc8b0a57c3ac9e481ull},
    {"F-ML", 16000, 1, 2842, 11368, 0x8c58ad20b1cee4fdull, 0x05da6304228b9035ull},
    {"F-MWG", 32000, 1, 1776, 3552, 0xeb7c33a06bd0731full, 0x0cbf3a162ccf56c2ull},
    {"F-PC", 16000, 1, 3320, 13280, 0x4399cdde76d2db0dull, 0x496a80c52e376fe9ull},
    {"F-RG", 16000, 1, 2606, 10424, 0xb6a8a5a2167d23e9ull, 0xe15b51a089ea6c39ull},
    {"F-RR", 16000, 1, 9080, 36320, 0xc55da4cfd9d433cdull, 0x7b71cc351458cb19ull},
    {"F-SD", 16000, 1, 2672, 10688, 0x47c81c59b8be2d0dull, 0x09694bfff7ae09f1ull},
    {"F-TDFL", 16000, 1, 2486, 9944, 0xc1886e44eae983b9ull, 0x824ce30c2b7e1541ull},
    {"F-TDFS", 16000, 1, 4020, 16080, 0x34231be4ac4d8969ull, 0xb981168d5e3aa4f1ull},
    {"F-TRB", 16000, 1, 1572, 6288, 0x710db04474beff4dull, 0x4bfd20f354ba6ba9ull},
    {"F-TRBL", 16000, 1, 6468, 25872, 0x50fef356b1e4e8a5ull, 0x8f869fc6aa783681ull},
    {"FFBIG", 16000, 1, 6496, 25984, 0x1d0f78abfd65a545ull, 0x4e8559b71c6cac61ull},
    {"FFPASS", 16000, 1, 2846, 11384, 0x52b90411537992f9ull, 0x4dc3b3c453e936e5ull},
    {"FIREBURN", 16000, 1, 12408, 49632, 0x2f96077c176f1f91ull, 0x5a0ff7681f494da9ull},
    {"FIREST", 8000, 1, 3746, 29968, 0xc93caa729f885005ull, 0x6f9a7980ff59f5bdull},
    {"FLICKER", 32000, 1, 8584, 17168, 0x4be476d811358b29ull, 0x89c73c716a2c218eull},
    {"GASHISS", 32000, 1, 7998, 15996, 0xbd0b0832f47e09f2ull, 0x05fa2cba9f0c64d7ull},
    {"GKDIE", 16000, 1, 16182, 64728, 0x079945ae018e9009ull, 0x2bfb1be53e8a213dull},
    {"GKFOOD", 16000, 1, 5258, 21032, 0xb05e4e410c40c9e9ull, 0xda5cded1d44e7ac9ull},
    {"GKJUMP", 16000, 1, 9462, 37848, 0x18b4d68acdd09055ull, 0x4c1faa6a1ec6a159ull},
    {"GKLIE", 16000, 1, 1708, 6832, 0x060688201c2c5e39ull, 0x12cef2574fe2c101ull},
    {"GKMELODY", 16000, 1, 32000, 128000, 0x6b1b27015893b2bdull, 0x38c08400d70113a5ull},
    {"GKSCORE", 16000, 1, 5050, 20200, 0xd39cd7b3df90acedull, 0xd509d5c833cc2a2dull},
    {"GKSHOOT", 16000, 1, 6724, 26896, 0xd96486e720af218dull, 0x789bc259098f4391ull},
    {"GKTANK", 16000, 1, 1514, 6056, 0x54c7cc9145d2b369ull, 0x4fa039f6712aaccdull},
    {"GKVILLAG", 16000, 1, 8010, 32040, 0x9222468fca42fad5ull, 0x7fcbb366b6fc5cb1ull},
    {"GKWALK", 16000, 1, 2402, 9608, 0x0a40072929a4ea29ull, 0x5b0c291bab003c45ull},
    {"GKWEAPON", 16000, 1, 4622, 18488, 0xf155b39cb4a43b2dull, 0x6287fc460eedc7b5ull},
    {"GONG", 16000, 1, 32000, 128000, 0x27ba2e2b263959e5ull, 0x3cdd6598adc6e9f5ull},
    {"GRIND", 16000, 1, 13048, 52192, 0x50d7e959736e7bb9ull, 0x9d1490357cedfd5dull},
    {"GRIND2", 16000, 1, 29854, 119416, 0x47e50e3ac9228d11ull, 0xf287b319342c0775ull},
    {"H-AC", 16000, 1, 2668, 10672, 0x53c6597382b843d9ull, 0x34c980d9c4a6b055ull},
    {"H-DF", 16000, 1, 19704, 78816, 0x87855a9687bf0659ull, 0x619a930ed56638e5ull},
    {"H-DFCOLL", 16000, 1, 5422, 21688, 0x425df68af90afa81ull, 0x46b53d402d3fc251ull},
    {"H-DIS", 16000, 1, 16000, 64000, 0xc443a0b25f739155ull, 0x18ec5ea40e7db085ull},
    {"H-DPFB", 16000, 1, 2424, 9696, 0x1af00703342cca61ull, 0xbba0a1b88f27a23dull},
    {"H-DPL", 16000, 1, 3144, 12576, 0x50eab27a8358310dull, 0x51a9e42458d35c2dull},
    {"H-FB", 16000, 1, 4824, 19296, 0xa864281b54d75ba1ull, 0xeeca5e77a9adedadull},
    {"H-FT", 16000, 1, 800, 3200, 0x81a5110072572e15ull, 0xacec7900df4eb45dull},
    {"H-GL", 16000, 1, 4000, 16000, 0x698d607b502ec829ull, 0xbdef9b3934100bfdull},
    {"H-LAS", 16000, 1, 7570, 30280, 0x6d138ed137845f09ull, 0xe6849969a6d237f1ull},
    {"H-MG", 16000, 1, 2416, 9664, 0xb7c30fa841638eb1ull, 0xec79aaba1eef875dull},
    {"H-MG1", 16000, 1, 4002, 16008, 0xa72cc32fbfb6c2a9ull, 0x40c61a73239e72c1ull},
    {"H-MG2", 16000, 1, 4418, 17672, 0x0ab07515554fbdc1ull, 0x8273b1b3eda1c9c9ull},
    {"H-MG3", 16000, 1, 5526, 22104, 0xe75bf27e1d388a4dull, 0x2347c346d5001c9dull},
    {"H-MG4", 16000, 1, 7900, 31600, 0x26a4a38c1722d9e5ull, 0x79da281c95c7a509ull},
    {"H-MG5", 16000, 1, 8616, 34464, 0x41a722e892cf7911ull, 0xb113114dd18f0dc9ull},
    {"H-MG6", 16000, 1, 5830, 23320, 0x7b33520c78aa6601ull, 0x08134d0ea44c75e5ull},
    {"H-MG7", 16000, 1, 6368, 25472, 0x658b7d97e80e419dull, 0xd8046df8162e50edull},
    {"H-MG8", 16000, 1, 7450, 29800, 0x51bad6f5181c4625ull, 0xa76cd9994e70c7e5ull},
    {"H-MINE", 16000, 1, 13336, 53344, 0x688433ba12e2b695ull, 0x38be529e5374edd9ull},
    {"H-MLACT", 16000, 1, 1318, 5272, 0x9a174c70d9032879ull, 0xf76de9274b3c57e1ull},
    {"H-MWG", 16000, 1, 6724, 26896, 0xe43622e48347ff19ull, 0x14ef256f78538899ull},
    {"H-PC", 16000, 1, 4000, 16000, 0x698d607b502ec829ull, 0xbdef9b3934100bfdull},
    {"H-RG", 16000, 1, 8000, 32000, 0xa8e7524993c14c25ull, 0x157dbc9e3741ce09ull},
    {"HYDRACTN", 16000, 1, 6342, 25368, 0x04e8fa246203a639ull, 0xf43c0e1a856c1fadull},
    {"INTRO", 32000, 2, 576000, 576000, 0x0ca1b6274f408593ull, 0xf8d0e99495112d6full},
    {"JETWHINE", 16000, 1, 15620, 62480, 0x4d2173ac47c77889ull, 0x6baffcaa686d2691ull},
    {"KDRONE", 16000, 1, 13652, 54608, 0x3a36211d504a9881ull, 0x8896be03225a6f69ull},
    {"KELLER01", 16000, 1, 29622, 118488, 0x89fde09334f54425ull, 0x4329d348acac7a29ull},
    {"KELLER02", 16000, 1, 24800, 99200, 0xbd725605d03eb869ull, 0xc3c78e028ae6d2ddull},
    {"KELLER03", 16000, 1, 20816, 83264, 0x5e36f9cccc400b05ull, 0x31cc0e7123bf08cdull},
    {"KELLER04", 16000, 1, 37324, 149296, 0x7ebe1486620a9e79ull, 0x03a3dce5b4945205ull},
    {"KELLERA1", 16000, 1, 18066, 72264, 0x39adedf9c1d8a491ull, 0xcc513096e64be6adull},
    {"KELLERS3", 16000, 1, 6464, 25856, 0xaaaaf427cde1915dull, 0xa2af1e5862904475ull},
    {"KELLERS4", 16000, 1, 20440, 81760, 0x1336da2ce4e72f41ull, 0xdd1e55becc5ff759ull},
    {"KELLERS5", 16000, 1, 3866, 15464, 0xe4176d9ae2490aa5ull, 0xd18e59fb440a3a35ull},
    {"KELLERS6", 16000, 1, 6764, 27056, 0xf8dbc659c97a82c9ull, 0x99298b1608eed039ull},
    {"KENNY", 16000, 1, 19014, 76056, 0x10ac635782b716fdull, 0x22a77eeffb6850c9ull},
    {"KING", 16000, 1, 31874, 127496, 0x60f07d308ea265a9ull, 0x2fe62734ced29f99ull},
    {"KINGDIE", 16000, 1, 97898, 391592, 0xdac7f21f57a47099ull, 0x5ca79a536cf22ccdull},
    {"KINGSCRM", 16000, 1, 36556, 146224, 0xab2302eaba2ac3b5ull, 0xca6b48aa8c8de429ull},
    {"KINGSPIT", 16000, 1, 52584, 210336, 0x3ba1c9055b0f6a05ull, 0x7cfd818c46bd4ec5ull},
    {"KLAX", 16000, 1, 16340, 65360, 0x1c94245d8ad74f31ull, 0x472dc873bee95a31ull},
    {"KSCREAM", 16000, 1, 6478, 25912, 0x2feddd0ac4d6ba45ull, 0x056f26b4adb9f255ull},
    {"L2ATAKA", 16000, 1, 10558, 42232, 0x05afa1024f00c069ull, 0xa954c5a0e7d43ebdull},
    {"L2ATAKB", 16000, 1, 11520, 46080, 0xa578f06cc1cef021ull, 0x0f7a263b806fc6cdull},
    {"L2ATAKC", 16000, 1, 10498, 41992, 0x2fb99d96843bcb99ull, 0x4ccceb24ebbced9dull},
    {"L2ATAKD", 16000, 1, 11944, 47776, 0x9456a90f5ae1f13dull, 0x41576d7f79e59639ull},
    {"L2ATAKE", 16000, 1, 10098, 40392, 0x6a4ea92b690335f1ull, 0xf2abba9c98cc33d5ull},
    {"L2ATAKF", 16000, 1, 13178, 52712, 0x27f5e12af89873bdull, 0x2052a48f2d22e6cdull},
    {"L2ATAKG", 16000, 1, 15536, 62144, 0x8ee77206d93ffc65ull, 0x748fff788095c4d1ull},
    {"LASERFIR", 16000, 1, 24064, 96256, 0x734d7bf29eb6c049ull, 0x4d2ab1fbdd9e02d1ull},
    {"LAVA", 16000, 1, 32318, 129272, 0x372fb38cdd0bcb2dull, 0x198ce564517b4579ull},
    {"LOCKED", 16000, 1, 1600, 6400, 0x53a8c36e6a1a41c5ull, 0x1e53edb30634e7e5ull},
    {"LOWHUM", 8000, 1, 1406, 11248, 0x0e2294c0225b70ddull, 0x9316334d08fc1df5ull},
    {"MACHINE1", 16000, 1, 3302, 13208, 0xe4e3b64ce5a31231ull, 0xe5e59141c503fa89ull},
    {"MACHINE2", 16000, 1, 4406, 17624, 0xa570d01f54541529ull, 0x0aafc61a35666ab1ull},
    {"MENUON", 16000, 1, 14250, 57000, 0x94f31017cc4fc4bdull, 0x7c72cd0f9d1d5649ull},
    {"METAL", 16000, 1, 4448, 17792, 0x31f2b00ed5e169f9ull, 0x8b8b9fe0773962c1ull},
    {"METAL2", 16000, 1, 4448, 17792, 0x31f2b00ed5e169f9ull, 0x8b8b9fe0773962c1ull},
    {"MONORAIL", 8000, 1, 2988, 23904, 0x886eaf113fccdabdull, 0xaca376beec187a4dull},
    {"MUZAK", 16000, 1, 69828, 279312, 0x0fe5e940df229fb5ull, 0x5ed09480dd939e39ull},
    {"NATKP1C", 16000, 1, 181374, 725496, 0x45953ebf2ada5389ull, 0x2c5c62b1f256f771ull},
    {"NOISE", 16000, 1, 4000, 16000, 0x05e9e7c14d90a151ull, 0x7f24f961a3fb2bcdull},
    {"NOISE2", 16000, 1, 10190, 40760, 0x7e36b4d3fe7a7e89ull, 0xf3aead83a185bb45ull},
    {"NVON", 16000, 1, 3818, 15272, 0xacfcf0e22ae4e025ull, 0xa5ae9eb371d51c4dull},
    {"OCTAPOD", 8000, 1, 5860, 46880, 0xe958d9b263b2e235ull, 0x1eda2e5b62eb07fdull},
    {"OUTAMMO", 16000, 1, 770, 3080, 0x8072e3439c93043dull, 0x0fdf7edc8e21af5dull},
    {"PAM", 16000, 1, 20128, 80512, 0x4375305c29f6e955ull, 0x51555c02ca33ca29ull},
    {"PICKAMMO", 16000, 1, 4000, 16000, 0x482fe0331b36ae05ull, 0x91dc59f50e2c888dull},
    {"PICKARMR", 16000, 1, 4582, 18328, 0xb11366569958f1c1ull, 0x69dc10b904015a6dull},
    {"PICKGOLD", 16000, 1, 11966, 47864, 0x44268f18f0d6215dull, 0xbba4a527594ce1a5ull},
    {"PICKKEY", 16000, 1, 1868, 7472, 0x9724112035e48fd5ull, 0x710a2fec50c4b32dull},
    {"PICKPART", 16000, 1, 3944, 15776, 0x64bf9bbe482c09a5ull, 0xb2fe1d8905c99889ull},
    {"PICKSAL", 16000, 1, 3494, 13976, 0xaaccbb6c9d6e1345ull, 0x342d86d6e0adda55ull},
    {"PICKTHNG", 16000, 1, 8418, 33672, 0x529794c79334c1cdull, 0x1b4905bcf6e814adull},
    {"PILEDRVR", 16000, 1, 13956, 55824, 0x5dbad31766be41c5ull, 0xa59349f391182ab5ull},
    {"POWER", 16000, 1, 10828, 43312, 0xf3efcbfb38736eb9ull, 0xb3887666cb7850c5ull},
    {"POWRLEAP", 8000, 1, 2950, 23600, 0x2a974cff3a316c45ull, 0xaa3d16f25702d5bdull},
    {"PRAM", 16000, 1, 248864, 995456, 0x4a176ea1ca9a0069ull, 0xe414c77fa953cde9ull},
    {"RADAR", 16000, 1, 1318, 5272, 0x1023bbf05ad23129ull, 0xe6da95f3ba083ea1ull},
    {"RADARUP", 16000, 1, 5772, 23088, 0x8ce5b2d1f582ef61ull, 0x409d6f7566c11ba1ull},
    {"REPAIR", 16000, 1, 12350, 49400, 0x0f2c7b636cd12d59ull, 0xce24cf053e6bbebdull},
    {"REPBOT1", 8000, 1, 6462, 51696, 0x9d05f1d69764d70dull, 0x7cd9fc321bb4c14dull},
    {"REPBOT2", 8000, 1, 9294, 74352, 0x6c247ce41ef7f2adull, 0xf7462ef915d4e2adull},
    {"RF1F1", 16000, 1, 16356, 65424, 0xe07d2d02e5582761ull, 0xcd1af6f15ee036a5ull},
    {"RF1F2", 16000, 1, 24980, 99920, 0x020046c34efa6779ull, 0x9931eade646b9729ull},
    {"RF1H1", 16000, 1, 15626, 62504, 0xc31631956f1d2f45ull, 0x61f756af7321cf69ull},
    {"RF1H2", 16000, 1, 8904, 35616, 0x2c1f146300bcd1f5ull, 0x76a6b1a84f06055dull},
    {"RF1M1", 16000, 1, 18490, 73960, 0x5f5628f76de5732dull, 0x5c7a313888c61e19ull},
    {"RF1S1", 16000, 1, 15040, 60160, 0x988181cfe022f589ull, 0x5de02f296b60fd51ull},
    {"RF1S2", 16000, 1, 16272, 65088, 0x31e3cdb629d90281ull, 0xd12f79170a30f52dull},
    {"RF2F1", 16000, 1, 19010, 76040, 0x0ddce7091e5f647dull, 0x0f2a1494fe3e9409ull},
    {"RF2F2", 16000, 1, 16350, 65400, 0xcaeed340db356149ull, 0x5f5dcac4be953ab5ull},
    {"RF2H1", 16000, 1, 10906, 43624, 0x046528a335b5d7c9ull, 0x20a99f2f88d70001ull},
    {"RF2H2", 16000, 1, 10780, 43120, 0x1e208e3ad5da6cc1ull, 0x440ba376e31561d5ull},
    {"RF2M1", 16000, 1, 14722, 58888, 0xedd892112b9adcd1ull, 0x2ee420599c1b8491ull},
    {"RF2S1", 16000, 1, 13926, 55704, 0x7df8d1380e1b4319ull, 0x46db1239293df68dull},
    {"RF2S2", 16000, 1, 17112, 68448, 0xf000a284d99217fdull, 0x911c4d5ca809b4fdull},
    {"RING", 16000, 1, 17158, 68632, 0xf2ae9dc2d2553365ull, 0xf22e5dc1d402d475ull},
    {"RINGSTAR", 16000, 1, 23786, 95144, 0x02cf0b9c63cc0e01ull, 0xa7e85fcee4d2c649ull},
    {"RINGSTOP", 16000, 1, 43896, 175584, 0xb37d5a3256e84c49ull, 0x9b4ca5f804257b7dull},
    {"RKDIALOG", 16000, 1, 4168, 16672, 0x76233f938fbe7da9ull, 0x58c454e13cd077d1ull},
    {"RN1H1", 16000, 1, 9288, 37152, 0xf35c5de6dcedbc71ull, 0x4ba24335a5947d91ull},
    {"RN1H2", 16000, 1, 16486, 65944, 0x2b0f40521d5a8f3dull, 0xc4953f5a9b487625ull},
    {"RN1M1", 16000, 1, 11642, 46568, 0xbc4cb9c782a87d8dull, 0xea1da876cff13329ull},
    {"RN1S1", 16000, 1, 11488, 45952, 0xa9304835c466ea69ull, 0xf7801dbf4a96f50dull},
    {"RN1S2", 16000, 1, 11002, 44008, 0x73e95188a0a33c6dull, 0xc1482c7369ce3eedull},
    {"RN1W1", 16000, 1, 14970, 59880, 0x8d699bb0f4b45b29ull, 0xe7d285654f69fa5dull},
    {"RN2H1", 16000, 1, 14956, 59824, 0xf58bef22f7824b1dull, 0x755ec18f428282e9ull},
    {"RN2H2", 16000, 1, 10708, 42832, 0x9d998842a1b247b9ull, 0x9199d044be749439ull},
    {"RN2H3", 16000, 1, 13938, 55752, 0x85fdbef3688903adull, 0x1c37af239551148dull},
    {"RN2H4", 16000, 1, 13926, 55704, 0x5d09dd7d7d5abe7dull, 0x458b65bcd6a723b9ull},
    {"RN2M1", 16000, 1, 18936, 75744, 0xe976e924826488ddull, 0x8469bc7eaad11c91ull},
    {"RN2M2", 16000, 1, 13242, 52968, 0xfe840b697b0967d5ull, 0xf91d7a80f6a96281ull},
    {"RN2S1", 16000, 1, 14878, 59512, 0x136e0a481a6cdd21ull, 0x2bc829d2edd0dd25ull},
    {"RN2S2", 16000, 1, 10586, 42344, 0xbdc232bd33463d81ull, 0x9edc211ec5442f99ull},
    {"RN2S3", 16000, 1, 10544, 42176, 0x1ee46b118f6c0571ull, 0x271157fde1df8979ull},
    {"RN2S4", 16000, 1, 15132, 60528, 0x4a5f56cdd9a67669ull, 0xf703fc4d1cdb192dull},
    {"RN2W1", 16000, 1, 13498, 53992, 0xcb86b77fc902bc95ull, 0x9bcb1e954d038de9ull},
    {"RN3H1", 16000, 1, 12010, 48040, 0xca64eb0ce94fdb11ull, 0xc699311b45af92d9ull},
    {"RN3H2", 16000, 1, 6994, 27976, 0x9789be3824ed9f65ull, 0xcc6b1b69367bbcd5ull},
    {"RN3H3", 16000, 1, 10502, 42008, 0x711cf53b5811f0b1ull, 0x7992c26b0d696bddull},
    {"RN3H4", 16000, 1, 9058, 36232, 0xc30f6f8958bb3b3dull, 0x3da6cbde0b4beeedull},
    {"RN3M1", 16000, 1, 12280, 49120, 0xbef9c63ab447c851ull, 0xc2a142c4a70a71f9ull},
    {"RN3M2", 16000, 1, 10856, 43424, 0x47b39d080ed0a715ull, 0x563e33f1c91fdc89ull},
    {"RN3S1", 16000, 1, 8922, 35688, 0x25858cf78724bcc9ull, 0x4001e8ff261e7689ull},
    {"RN3S2", 16000, 1, 12792, 51168, 0x9363059b8f6b8685ull, 0x982fa2fe0e631981ull},
    {"RN3S3", 16000, 1, 12226, 48904, 0xbdde8ca8b482cf45ull, 0x3c3a64d197904dcdull},
    {"RN3S4", 16000, 1, 8992, 35968, 0xe9786f4d344d9089ull, 0xb64d203cf133216dull},
    {"RN3W1", 16000, 1, 15558, 62232, 0x5a836886b0a43b69ull, 0x15c0fc1a59fe6bf5ull},
    {"ROBOT", 16000, 1, 16178, 64712, 0xff51d56b1f3dd691ull, 0x1ca4ed91beeabd75ull},
    {"ROCKET", 16000, 1, 31020, 124080, 0x3d406ed78be3f0d1ull, 0x8bd82a751d208d69ull},
    {"ROCKETL", 16000, 1, 10340, 41360, 0x466fd587cd586535ull, 0xca49eb5622613119ull},
    {"ROCKFALL", 16000, 1, 39944, 159776, 0x0f0486e504b43ba5ull, 0x8c7fe1264053446dull},
    {"ROCKFLP", 16000, 1, 34676, 138704, 0xc1b40299f0f56ed5ull, 0x315cf69e507d96a9ull},
    {"ROOK", 16000, 1, 23996, 95984, 0x0ff2266ec36a10f1ull, 0x923c837e0f011e91ull},
    {"RUSHWIND", 16000, 1, 14388, 57552, 0x47622974c33bd4a9ull, 0x3d12c970c583a809ull},
    {"RUSTLE", 16000, 1, 5406, 21624, 0x80fe75f87cb870e9ull, 0xf782817418136515ull},
    {"SCREAM1", 8000, 1, 3442, 27536, 0x9aa1eed0c7500eddull, 0xf2a29ec840dae2ddull},
    {"SCREAM99", 16000, 1, 19466, 77864, 0xd8fd87efc3e25a05ull, 0xc608140356ef7f2dull},
    {"SELECT", 16000, 1, 3136, 12544, 0x88cd4f801bc99dc9ull, 0x88ddab6d15897321ull},
    {"SELECTM", 16000, 1, 3304, 13216, 0x8680cd55b24f20fdull, 0x3c0cd92d4c1fc315ull},
    {"SHATTER", 16000, 1, 10896, 43584, 0x27e4f097b9f8b545ull, 0xfe5ca0512610a0f1ull},
    {"SHELL1S", 16000, 1, 5392, 21568, 0xd8bb713260574f09ull, 0xc5322cf9b7ee4161ull},
    {"SHELL2S", 16000, 1, 6400, 25600, 0x550f7f9554fa0ab9ull, 0xcd7619476edc875dull},
    {"SHELL3S", 16000, 1, 10732, 42928, 0x233d1bcff093b1fdull, 0x06154f6bf29c5559ull},
    {"SHORTC", 16000, 1, 1894, 7576, 0x63a6dadbbd8b6a95ull, 0x3ecfe971f99c2849ull},
    {"SILENCE", 8000, 1, 400, 3200, 0xbeb46562da647365ull, 0x00410e1ab02ebf95ull},
    {"SIREN", 16000, 1, 4800, 19200, 0x1aac6abbc1b8b30dull, 0x7fa90efea2ee2ec9ull},
    {"SKITTER", 16000, 1, 14382, 57528, 0x1ec2603e267dcf99ull, 0x41e08170e2511889ull},
    {"SLIDE", 16000, 1, 5230, 20920, 0x9d06ff85d0fa57e9ull, 0xf5dea761d936c7cdull},
    {"SLOPULSE", 16000, 1, 5580, 22320, 0x90b139fbe68ef53dull, 0x15d6df03e1abd5d9ull},
    {"SOLDRD1", 8000, 1, 2138, 17104, 0x8e10493320ec5ecdull, 0xef250537a83bc2edull},
    {"SOLDRD2", 8000, 1, 2898, 23184, 0xf8965df1e0ec8dddull, 0xaf46d36c4ba822f5ull},
    {"SOLDRD3", 8000, 1, 2740, 21920, 0x1ff99f1d59a0f38dull, 0xe675d98ed0cc9d2dull},
    {"SONAR", 16000, 1, 17650, 70600, 0xc9649a85a7683cd1ull, 0xaf24865cf2dcc1adull},
    {"SOYBOY", 32000, 1, 263500, 527000, 0x3ed95b2a91404631ull, 0x922aed3a9e92a17full},
    {"SPLASH", 16000, 1, 13400, 53600, 0x83b7d6e769ae6e99ull, 0x40340c7ca9be8249ull},
    {"SPLASH2", 16000, 1, 23692, 94768, 0x0afaffd5b12f43d5ull, 0xf25fea3c64b10491ull},
    {"SPLASH3", 16000, 1, 16300, 65200, 0xd8bd7d72c1fe4c61ull, 0x92af04c1725ece6dull},
    {"STUCK", 16000, 1, 4510, 18040, 0xc41820a60fcb8c71ull, 0xcc20e3b6096efae1ull},
    {"SWELEC1", 8000, 1, 1318, 10544, 0xb72b1e07bbf2fc0dull, 0xbe3a50aaf1e6413dull},
    {"SWELEC10", 16000, 1, 3032, 12128, 0x6a9b524a30bfc6edull, 0x314306286e91bbb9ull},
    {"SWELEC11", 16000, 1, 1950, 7800, 0x0771ef139acf8a09ull, 0x385e7a58e992ae51ull},
    {"SWELEC12", 16000, 1, 3342, 13368, 0x3a2af6d742f0e5f5ull, 0x8812b697e0b6f019ull},
    {"SWELEC13", 16000, 1, 4070, 16280, 0x09cf59eeb4a81891ull, 0x19e0c2823c7068a5ull},
    {"SWELEC2", 16000, 1, 7982, 31928, 0x4a6215d6cee13f4dull, 0x241d1d160f3e32cdull},
    {"SWELEC3", 16000, 1, 1508, 6032, 0x219a076385988719ull, 0xdfac88fb42eb2279ull},
    {"SWELEC4", 16000, 1, 594, 2376, 0x6ab36c7941b174b1ull, 0xbef4ff641539e829ull},
    {"SWELEC5", 16000, 1, 936, 3744, 0xd345b5342e091fe5ull, 0x66943bf141545c25ull},
    {"SWELEC6", 16000, 1, 1372, 5488, 0x56e0b85c1d234e29ull, 0x8f092b3e7ee0afcdull},
    {"SWELEC7", 16000, 1, 10654, 42616, 0x3c70ccb9b68acdddull, 0xcfbc64be4c326155ull},
    {"SWELEC8", 16000, 1, 2760, 11040, 0x1a1d4f4254584e69ull, 0xca8e7aac771317edull},
    {"SWELEC9", 16000, 1, 3808, 15232, 0x264eefa4d7604f71ull, 0x12b1e794a12c0e9dull},
    {"SWITCH3B", 16000, 1, 4284, 17136, 0xf23f0436dbe0780dull, 0xf28c66d0c9829ed9ull},
    {"SWITCHM", 16000, 1, 4230, 16920, 0xc7f372aa06e34765ull, 0xa1657901705e562dull},
    {"SWITCHM2", 16000, 1, 4800, 19200, 0xd7c6d73301c14429ull, 0xee03a84ffff8ded5ull},
    {"SWITCHM3", 16000, 1, 4230, 16920, 0xaec1a04221030ab1ull, 0x5511536e09fee811ull},
    {"SWITCHM4", 16000, 1, 4286, 17144, 0xeb21401e106add9dull, 0x31fa449218a1fa2dull},
    {"SWITCHM5", 16000, 1, 1868, 7472, 0x94f47a5b4c88df3dull, 0x82cfd8d574986ba1ull},
    {"SWITCHM6", 16000, 1, 6570, 26280, 0xb1e4c1866e247a79ull, 0x04a225d9cf3e90f1ull},
    {"SWITCHM7", 16000, 1, 3670, 14680, 0xa1e8bf57de475cbdull, 0xb5815f64a9234381ull},
    {"SWITCHM8", 16000, 1, 2950, 11800, 0x6c442b8052cedba9ull, 0x52fdc911726cd595ull},
    {"SWITCHM9", 16000, 1, 5952, 23808, 0xd9db1b6b395b540dull, 0x74d498ead32d1689ull},
    {"SWITCM10", 16000, 1, 11960, 47840, 0x6cefa2e66ec6a0a9ull, 0xe11bd18fe3c136e1ull},
    {"SWITCM11", 16000, 1, 2982, 11928, 0x3452b88240f73785ull, 0x0c31d82d7fd89eb1ull},
    {"SWITWEAP", 16000, 1, 770, 3080, 0xd0d4f1c00adc412dull, 0x102945f70c964c45ull},
    {"TANKSTE1", 8000, 1, 2278, 18224, 0x49bdf6528e55137dull, 0x96ed05e2172dbb45ull},
    {"TANKSTE2", 8000, 1, 3906, 31248, 0xd9e8fc12bfce362dull, 0xbfffb3f0559e447dull},
    {"TANKSTH", 16000, 1, 6072, 24288, 0x4561c91011738485ull, 0x089b99e0349b4f25ull},
    {"TANKSTL", 16000, 1, 4096, 16384, 0x989ecc0ae66e76b1ull, 0x61664109089d7ea1ull},
    {"TANKSTM", 16000, 1, 5042, 20168, 0x7e9f29515171c2a1ull, 0xe9aa9c47e9a39521ull},
    {"TANKSTP", 8000, 1, 3378, 27024, 0xeab671c4fa0d59b5ull, 0x7f27553e7531ee45ull},
    {"TANKTRD2", 16000, 1, 22774, 91096, 0xb40561143d875739ull, 0xc096b30b8d456f6dull},
    {"TANKTRDS", 16000, 1, 23046, 92184, 0xfbcd541f6fe56a6dull, 0xb5653de9d311d2d9ull},
    {"TELEPC", 16000, 1, 4006, 16024, 0x6fa8ac361b52bc59ull, 0x25fb4f5668fdc259ull},
    {"TELEPIN", 16000, 1, 4376, 17504, 0x09604816caf56d85ull, 0xb39d2879c5209439ull},
    {"TELEPL", 16000, 1, 11230, 44920, 0xeaef3db67922e625ull, 0xe8a6d0cb9525d789ull},
    {"TELEPO", 16000, 1, 5484, 21936, 0x6a0422026161d449ull, 0x0092ff38f7161c2dull},
    {"TELEPR", 16000, 1, 7784, 31136, 0x3ce38dff1af18ac9ull, 0xe52d29b4271a4cadull},
    {"TELESHIP", 16000, 1, 30024, 120096, 0x14429ea0a9627ed9ull, 0xaac7443309bdabe1ull},
    {"THUMOIL", 8000, 1, 4494, 35952, 0x86adc152705a1575ull, 0x5ee51903ca6ff45dull},
    {"THUNDER1", 16000, 1, 56106, 224424, 0xc9a6febf4c3d5951ull, 0xd480dc66028cd35dull},
    {"THUNDER2", 16000, 1, 24070, 96280, 0xe02d0cc380f55555ull, 0x23b084c1bc7b7e81ull},
    {"THUNDER3", 16000, 1, 40234, 160936, 0x2124cd6166bbbca5ull, 0x37d2679e67d2ab41ull},
    {"TOILET", 16000, 1, 52496, 209984, 0xce60567a12dd0a65ull, 0x77e2ec355177a109ull},
    {"TURBO", 16000, 1, 11006, 44024, 0x6119a8440f2dfbd1ull, 0x8042292b595d9175ull},
    {"UPDOWN", 16000, 1, 3136, 12544, 0x3787c218ce60b071ull, 0x4f71377a086eb8edull},
    {"V00", 8000, 1, 2486, 19888, 0x8108ce0c707f0eb5ull, 0x7a6de9092086973dull},
    {"V01", 8000, 1, 1522, 12176, 0xbef7a7bdb92e7e7dull, 0xc69ebe2631f080f5ull},
    {"V02", 8000, 1, 1402, 11216, 0xbb60c811d9192395ull, 0x1479d5a0a38db365ull},
    {"V03", 8000, 1, 1966, 15728, 0xf23ca8fe16682b95ull, 0xe6d4eba79b2f0ae5ull},
    {"V04", 8000, 1, 1758, 14064, 0x54f5ab5a3490b055ull, 0x9964e31d1ebd09edull},
    {"V05", 8000, 1, 1882, 15056, 0x996301d61ab23dbdull, 0xed347220b0ed7085ull},
    {"V06", 8000, 1, 2380, 19040, 0x59a062bc8b174ac5ull, 0x04469d1a6fa83745ull},
    {"V07", 8000, 1, 2296, 18368, 0xf86a8afa6c449a9dull, 0xb912650f64d83585ull},
    {"V08", 8000, 1, 1718, 13744, 0xf9a86209584e8835ull, 0x2bf3a63f1dc008fdull},
    {"V09", 8000, 1, 2168, 17344, 0x053604cd48f9fd25ull, 0x2b796c31562fecddull},
    {"V0AIRPRS", 8000, 1, 5180, 41440, 0x8a5fe37fc318bd35ull, 0x657403689088f805ull},
    {"V10", 8000, 1, 1784, 14272, 0xd5be6b5b072f81a5ull, 0xb992ecb289e68b95ull},
    {"V100MCAN", 8000, 1, 4130, 33040, 0xc51e41cf9d71c965ull, 0x32e1e38ec7f7b8fdull},
    {"V11", 8000, 1, 2438, 19504, 0x231ad0d27e76715dull, 0xa890e2e50daf805dull},
    {"V12", 8000, 1, 2446, 19568, 0xb60cfa517e08f2ddull, 0x37f23c7f6835e3cdull},
    {"V13", 8000, 1, 2974, 23792, 0x6f70672c4c26b9d5ull, 0xa65fff9f329f13fdull},
    {"V14", 8000, 1, 2976, 23808, 0xa9361c6089157be5ull, 0xee9baaef988ee2adull},
    {"V15", 8000, 1, 2796, 22368, 0x5c5b68a44711df2dull, 0x263202287e2c8fbdull},
    {"V16", 8000, 1, 3206, 25648, 0x98609e49d973360dull, 0x3e979653966c61c5ull},
    {"V17", 8000, 1, 3280, 26240, 0x5761498dfddeeb7dull, 0x8e195b1ac8243dddull},
    {"V18", 8000, 1, 2634, 21072, 0x1f00b29ff531e72dull, 0x35880bf1001c9e05ull},
    {"V19", 8000, 1, 3092, 24736, 0xcd0ac573af5a416dull, 0x3d3e194800290aedull},
    {"V20", 8000, 1, 2232, 17856, 0x28637d367a3ffd7dull, 0x5b9ebeb08f90cc95ull},
    {"V20MCAN", 8000, 1, 3988, 31904, 0x0b360df2f00e5425ull, 0xf2153d30fe4a2f2dull},
    {"V30", 8000, 1, 1928, 15424, 0xdb7b0c05476c098dull, 0xb93bba254ba03c3dull},
    {"V40", 8000, 1, 2024, 16192, 0xfeac8e16f8e5ee1dull, 0x03b24812cb81b735ull},
    {"V50", 8000, 1, 2212, 17696, 0x509d0ebbdedaeb15ull, 0xe5a0994922750d6dull},
    {"V60", 8000, 1, 2460, 19680, 0xebbcbbfebc6e36bdull, 0xd05be3d9b050ae35ull},
    {"V70", 8000, 1, 2560, 20480, 0x5c0a4d2904924ca5ull, 0xd83ffe2b9de99d95ull},
    {"V70MCAN", 8000, 1, 4640, 37120, 0x117e0a0e2c12d355ull, 0x575ebcef2f126badull},
    {"V80", 8000, 1, 1576, 12608, 0x170b72dd934ec0d5ull, 0x66f28481f63f8c05ull},
    {"V90", 8000, 1, 2322, 18576, 0x2802d428c4e76865ull, 0xd1e22414f104e54dull},
    {"VACCESS", 8000, 1, 2434, 19472, 0x0285ca2f4aab1105ull, 0x421ad038123219f5ull},
    {"VACSCODE", 8000, 1, 4066, 32528, 0x74f16d1d9f73b965ull, 0xa1d914dbd289d5a5ull},
    {"VACSDENY", 8000, 1, 5258, 42064, 0x4c0a814b83014425ull, 0x840a20e17d540875ull},
    {"VADMIRBL", 8000, 1, 3076, 24608, 0x29c39605a9635d05ull, 0x7e70bb79e709c0e5ull},
    {"VALERT", 8000, 1, 2290, 18320, 0xe7171e8d386a9bc5ull, 0xbe11b04491c1eb85ull},
    {"VALL", 8000, 1, 1588, 12704, 0xca2729d41fc21205ull, 0xae481bddc49e39c5ull},
    {"VALLARMD", 8000, 1, 5668, 45344, 0xa08eda7f7d74a175ull, 0x2e292ae8bd0be7fdull},
    {"VALLMISS", 8000, 1, 6642, 53136, 0xa2147a20fa2b9ebdull, 0xf3a95a72075a66edull},
    {"VALPHAH1", 8000, 1, 13600, 108800, 0x968b9b6b0eb5eafdull, 0x7ac14869f392df8dull},
    {"VALPHAS1", 8000, 1, 13804, 110432, 0x7314fc95f12eb09dull, 0xa67bfc2439ac7645ull},
    {"VALPHAW1", 8000, 1, 25060, 200480, 0x4d6c0fa3319f7c0dull, 0xca04407640b91efdull},
    {"VALPHDED", 8000, 1, 31114, 248912, 0x415fd3bd3760cc3dull, 0x90dcf01b11c9418dull},
    {"VAMMODEP", 8000, 1, 5646, 45168, 0xb31f375f2d95641dull, 0x83549ba190276055ull},
    {"VARCOLGY", 8000, 1, 12036, 96288, 0x76714fa4af29c2c5ull, 0xb39be80edffe4cadull},
    {"VAREASEC", 8000, 1, 4202, 33616, 0xb9fcbfc4e55ae8adull, 0xfae376e271e65105ull},
    {"VARMDEST", 8000, 1, 4656, 37248, 0xa7e1e5881857756dull, 0xe1df0d5a7bf04a35ull},
    {"VARMREPR", 8000, 1, 3786, 30288, 0xf2a315e7129038cdull, 0x235e65fed81d34a5ull},
    {"VATTENTN", 8000, 1, 2384, 19072, 0x8bce7d6d30a62645ull, 0x9e3cc1d85304ef4dull},
    {"VAUTOCAN", 8000, 1, 3400, 27200, 0xf238bc211c5900cdull, 0xac27826f3cb81cd5ull},
    {"VAUTOSEC", 8000, 1, 9080, 72640, 0xe2278fa431f713a5ull, 0xef72a49c9922ca35ull},
    {"VAUTOSEN", 8000, 1, 3466, 27728, 0xe4a68b142e83078dull, 0xf0c1d627b90877e5ull},
    {"VAUXTHRS", 8000, 1, 5414, 43312, 0x9fc97c3fd1d41a9dull, 0x4532127eb583cee5ull},
    {"VBACK", 8000, 1, 2084, 16672, 0x9bfce85d2d6fa7f5ull, 0xf183661cca4e86cdull},
    {"VBATTERY", 8000, 1, 2536, 20288, 0xeee79b4d3c171fbdull, 0xd9fd7c820c2d1c1dull},
    {"VBLACK", 8000, 1, 2082, 16656, 0x9f634c48c562139dull, 0x9ab9ea3369513fa5ull},
    {"VBLIMP1", 16000, 1, 59448, 237792, 0x5988edd6675ccf01ull, 0xacbbf8460cec41c1ull},
    {"VBLIMP1A", 16000, 1, 10670, 42680, 0x4de5f51474a9a8a1ull, 0xc546649644666fb9ull},
    {"VBLIMP1B", 16000, 1, 6590, 26360, 0xbb0500bfbd4a4d4dull, 0x9097e8dab39142b1ull},
    {"VBLIMP1C", 16000, 1, 9138, 36552, 0x8f04d7eba3ae1275ull, 0x108797d99789a641ull},
    {"VBLIMP1D", 16000, 1, 9470, 37880, 0xc0b1f033cedbaba1ull, 0xd7c4c5d088cc3111ull},
    {"VBLIMP1E", 16000, 1, 9038, 36152, 0x9ff2766363b7797dull, 0xeefd509e2a79ae49ull},
    {"VBLIMP1F", 16000, 1, 9010, 36040, 0x693c393ada08c8b1ull, 0x7089bc16f15685c9ull},
    {"VBLIMP1G", 16000, 1, 8426, 33704, 0x5a5f11ae554cbb29ull, 0xb5e4ffa86a4538c9ull},
    {"VBLIMP2", 16000, 1, 26760, 107040, 0x4e72a8f6fd188d41ull, 0x813afbe8a112ddb5ull},
    {"VBLIMP2A", 16000, 1, 12290, 49160, 0xa407a0927a4f940dull, 0x0455843f045b0e5dull},
    {"VBLIMP2B", 16000, 1, 7272, 29088, 0x2c0d08989dfe85f1ull, 0x3fde6befeaa64be1ull},
    {"VBLIMP2C", 16000, 1, 7196, 28784, 0xed2d1794022e19e1ull, 0x93e064d654597c51ull},
    {"VBLIMP3", 16000, 1, 35190, 140760, 0xb4b051a6932ee465ull, 0x2ba994fa92d928ddull},
    {"VBLIMP3A", 16000, 1, 3738, 14952, 0x9eccbb347fea7481ull, 0xcb2fb24efb341c35ull},
    {"VBLIMP3B", 16000, 1, 10266, 41064, 0x3eeff973b0b5f845ull, 0x2b59c5f9b294d06dull},
    {"VBLIMP3C", 16000, 1, 10230, 40920, 0x634f872bb7f386b5ull, 0xd30cb380957575e1ull},
    {"VBLIMP3D", 16000, 1, 10954, 43816, 0xcfe4303508f0bbddull, 0xf776a619ec0fd755ull},
    {"VBLIMP4", 16000, 1, 32778, 131112, 0x90bf1cc2b7b3b091ull, 0x141d721fb2668795ull},
    {"VBLIMP4A", 16000, 1, 11968, 47872, 0x4e405762118e7ea9ull, 0x44d7e871ec016171ull},
    {"VBLIMP4B", 16000, 1, 7152, 28608, 0x263aa1703527d495ull, 0x70049db4f7d336adull},
    {"VBLIMP4C", 16000, 1, 11962, 47848, 0xbf3f9347460fa8ddull, 0xa44837bdb62e070dull},
    {"VBLIMP4D", 16000, 1, 8058, 32232, 0x7705b5227a2991bdull, 0xba8b617f88856d8dull},
    {"VBLIMP5", 16000, 1, 13818, 55272, 0xb10bc225c4e6d849ull, 0xc797dd57d973d5cdull},
    {"VBLIMP6", 16000, 1, 36586, 146344, 0xe58e7790a5525c05ull, 0x0b7386061914faa1ull},
    {"VBLUE", 8000, 1, 1994, 15952, 0xdfefec75ab3d3ce5ull, 0x110a3139a250b67dull},
    {"VBORG", 8000, 1, 5558, 44464, 0x299ab4ecb29a9675ull, 0x13d9847a552dd60dull},
    {"VBRIEFNG", 8000, 1, 8118, 64944, 0x37af4f1497d74d55ull, 0x3da1036451edb94dull},
    {"VCAMOFLD", 8000, 1, 4104, 32832, 0x4466e9429b79d145ull, 0xf381b20e3cf5f555ull},
    {"VCANNON", 8000, 1, 2158, 17264, 0xcfbefa844ec64745ull, 0x8fab85d924ae98fdull},
    {"VCAUTION", 8000, 1, 2198, 17584, 0xbb6b17874bb8899dull, 0x623f43d03892dd7dull},
    {"VCHAFF", 8000, 1, 2290, 18320, 0xb8c7425705bcf105ull, 0x74b4784cfe76cfe5ull},
    {"VCHEMARM", 8000, 1, 5332, 42656, 0xcfeb928f9813836dull, 0x426cd6f2040d70bdull},
    {"VCHOLEST", 8000, 1, 8178, 65424, 0x6d853e94739e6725ull, 0x75d3fede3bf4bd4dull},
    {"VCLIP", 8000, 1, 2142, 17136, 0xb16822283888e9bdull, 0xf93e86e7b37f0955ull},
    {"VCODE", 8000, 1, 1920, 15360, 0x3ddb8baa67bb7aedull, 0x2361dd3e21e0c055ull},
    {"VCORE01", 8000, 1, 8856, 70848, 0xe40025fbbadb5435ull, 0xb04ed18133e11375ull},
    {"VCORE02", 8000, 1, 8464, 67712, 0x3d4e318d7fa901fdull, 0x84474a388f2f39cdull},
    {"VCORE03", 8000, 1, 6594, 52752, 0x2e0a1af29266159dull, 0xb6c6ee9c35a66af5ull},
    {"VCORE04", 8000, 1, 12724, 101792, 0x340fde2cb203da2dull, 0x3d463839c343b46dull},
    {"VCORE05", 8000, 1, 6290, 50320, 0x24e4a8dc7b96681dull, 0x682fa24a7c9a5055ull},
    {"VCORE06", 8000, 1, 9428, 75424, 0xe3b1c715ac9c14d5ull, 0x03e3abe43d018125ull},
    {"VCORE07", 8000, 1, 6954, 55632, 0x929e2f063d521605ull, 0x753391f9cfe5978dull},
    {"VCORE08", 8000, 1, 12802, 102416, 0xb3ae2a81e9acd565ull, 0x9459f85ca485478dull},
    {"VCORE09", 8000, 1, 2834, 22672, 0xbab04d2bffeebe2dull, 0x47bff8a4ea420a7dull},
    {"VCORE1", 8000, 1, 8916, 71328, 0xc6b8f8fd3a3233c5ull, 0x81dcd8ed6748c4c5ull},
    {"VCORE10", 8000, 1, 5410, 43280, 0x1577e3a8fb53770dull, 0xbe3067f983e98865ull},
    {"VCORE11", 8000, 1, 11380, 91040, 0xce53033265868765ull, 0x580f16221c094a1dull},
    {"VCORE12", 8000, 1, 6140, 49120, 0x41a233fef744abb5ull, 0x095a9269ca31dc4dull},
    {"VCORE13", 8000, 1, 8058, 64464, 0xe529e084c3f1d31dull, 0xebb679c562edc5d5ull},
    {"VCORE14", 8000, 1, 6478, 51824, 0x3f9da86364f0e47dull, 0x5eb48dc4f8a5adadull},
    {"VCORE15", 8000, 1, 7310, 58480, 0x643e61e70ef899edull, 0xfeaa1e222b495a65ull},
    {"VCORE16", 8000, 1, 2736, 21888, 0xd862610f420c0e95ull, 0x084eddfbfb08a115ull},
    {"VCORE17", 8000, 1, 2398, 19184, 0xd192a4118a27e11dull, 0x7b1295da0038cc4dull},
    {"VCRTSYSF", 8000, 1, 5572, 44576, 0xf28f2c6135ed67bdull, 0x70ff87fafdd964b5ull},
    {"VDANGER", 8000, 1, 2492, 19936, 0xc53a7a09d28accd5ull, 0x4f526e42d1f28dddull},
    {"VDECOY", 8000, 1, 2484, 19872, 0x19d5657292203175ull, 0x9afb21187702f345ull},
    {"VDENIED", 8000, 1, 2402, 19216, 0x3ba17654042674f5ull, 0x265fc89b689988e5ull},
    {"VDESTNAT", 8000, 1, 14646, 117168, 0x098a9bfd12af5a45ull, 0x1f4ec6bc9d5b21e5ull},
    {"VDETECTD", 8000, 1, 2476, 19808, 0xb84ace29791d29adull, 0xde198a33111b12c5ull},
    {"VDETECTR", 8000, 1, 2668, 21344, 0x006c12b132c7dd0dull, 0xf98e1e63a2838e15ull},
    {"VDISNTGR", 8000, 1, 3388, 27104, 0x07f43ae3c0261f0dull, 0x766ab1cbf0b9ad85ull},
    {"VDISPLID", 8000, 1, 15654, 125232, 0x1cf07f869948790dull, 0xc41cd053e52395bdull},
    {"VDMILITR", 8000, 1, 9562, 76496, 0x249a3fb63d450dedull, 0xa28b1b75c98f8335ull},
    {"VDOLLVAL", 8000, 1, 14366, 114928, 0x5437c673a0a7804dull, 0xd1c12b850b4c1545ull},
    {"VDRONE", 8000, 1, 2570, 20560, 0x5397cba4794752b5ull, 0x6efb648638bef375ull},
    {"VDRONEH", 8000, 1, 4314, 34512, 0xd57c6b2b6564c0adull, 0x30084734dcc63375ull},
    {"VDRONEL", 8000, 1, 3352, 26816, 0x8ddae8c1e83141bdull, 0x1a21cf1d9fd58eb5ull},
    {"VDRONES", 8000, 1, 4452, 35616, 0x4ba29c83e800abedull, 0x06c57912c9299175ull},
    {"VDSRPFLD", 8000, 1, 4170, 33360, 0x6d721c3471852ec5ull, 0x150e27d1473db72dull},
    {"VECMJPOD", 8000, 1, 6362, 50896, 0xe251988126d79e8dull, 0x7a414bac20bd979dull},
    {"VEHWHEEL", 8000, 1, 2952, 23616, 0x27ae57cf50049d0dull, 0x65089ee64b792c85ull},
    {"VEMPGUN", 8000, 1, 4204, 33632, 0x39dd36245051bd8dull, 0xe3241d363da3dec5ull},
    {"VENCOMBT", 8000, 1, 5704, 45632, 0xd582dd9f4f7555e5ull, 0xcf1c8063b71d907dull},
    {"VENT", 16000, 1, 42406, 169624, 0x9713fd62e56f4501ull, 0xf96775db37f128bdull},
    {"VEVAPROC", 8000, 1, 7328, 58624, 0xa26cc752a2583f85ull, 0x316414f2f9b310d5ull},
    {"VEVRYDAY", 8000, 1, 12676, 101408, 0xabe4a9992757bbedull, 0xda62dbf95998efd5ull},
    {"VEXMAGFL", 8000, 1, 6804, 54432, 0xb54ceba2b3aa5bfdull, 0x741ad3b4c9597da5ull},
    {"VFAILURE", 8000, 1, 2156, 17248, 0x479f1cc882069055ull, 0xd7f77d51a0e699fdull},
    {"VFLAMETH", 8000, 1, 3252, 26016, 0x3adbe6a4ed85beb5ull, 0x39b46e7ae0e9b135ull},
    {"VFLARE", 8000, 1, 1902, 15216, 0x74759eb88328e9a5ull, 0x9b45bb074c799fd5ull},
    {"VFLARES", 8000, 1, 2570, 20560, 0x9d76e076511aef4dull, 0x720d088a00a74b45ull},
    {"VFORCEFD", 8000, 1, 3246, 25968, 0x8d9079220870d0adull, 0x7d8d13a348ff6a05ull},
    {"VFR1A", 16000, 1, 10778, 43112, 0x36ba61366af58561ull, 0x120ec4031ee461e1ull},
    {"VFR1B", 16000, 1, 19580, 78320, 0xf2678d0e6d71b061ull, 0xac924ecc9d10237dull},
    {"VFR1C", 16000, 1, 12068, 48272, 0xc62d824edc927cd1ull, 0xeeafcd62ac8aa721ull},
    {"VFR1D", 16000, 1, 20412, 81648, 0x10494e2eccdb7d4dull, 0x5836bb53dc990db9ull},
    {"VFR1E", 16000, 1, 9718, 38872, 0x5d1e17f6b48c90e9ull, 0x2b7740f78eb20351ull},
    {"VFR2A", 16000, 1, 18896, 75584, 0xfaa182fdaca815c9ull, 0xd2f37a0458f6b489ull},
    {"VFR2B", 16000, 1, 11940, 47760, 0xacfd98463d3abbf1ull, 0x9db91a1d67bd2cf9ull},
    {"VFR2C", 16000, 1, 12974, 51896, 0xc77eaf12dc19d999ull, 0xac8037a3088e6141ull},
    {"VFR2D", 16000, 1, 12058, 48232, 0xd3322e4ccce3dc99ull, 0x964dbc6386a9452dull},
    {"VFR2E", 16000, 1, 11132, 44528, 0x5f315c4fa08727edull, 0x82097a2ced1989f9ull},
    {"VFR2F", 16000, 1, 15136, 60544, 0x19c3df767f73d855ull, 0xb4b28c4054e61b39ull},
    {"VFRFIRE1", 16000, 1, 15414, 61656, 0x026d4d98e7748c41ull, 0x2718f294d1f670f1ull},
    {"VFRFIRE2", 16000, 1, 27700, 110800, 0xcedfb7cdb2abe5c1ull, 0xd45cf724eb3a49cdull},
    {"VFRFIRE3", 16000, 1, 22004, 88016, 0x72c8a64d134ee9cdull, 0x5a0af1cc35c8b511ull},
    {"VFRONT", 8000, 1, 1988, 15904, 0xd84fb6055bc6bf25ull, 0x7cbf53243fde6f75ull},
    {"VFUELCRT", 8000, 1, 4994, 39952, 0x69736a6d537d8d1dull, 0xd700283b1edb71fdull},
    {"VFUSBSTR", 8000, 1, 4494, 35952, 0x48c371369672b4adull, 0x17bf207da4696bcdull},
    {"VFUSIONB", 8000, 1, 3584, 28672, 0x96b60e197060354dull, 0x0cd7850b385e37ddull},
    {"VGHANDIK", 8000, 1, 7468, 59744, 0xd618dc2ab552c055ull, 0x561967357ea0c4e5ull},
    {"VGOODMOR", 8000, 1, 9944, 79552, 0x62acac1fb0ae6625ull, 0x79889d150215d0fdull},
    {"VGRAVARE", 8000, 1, 3644, 29152, 0xb64c181ba7b5c615ull, 0x3f1a9870d87754ddull},
    {"VGRAVITB", 8000, 1, 3250, 26000, 0x124940163e3cf59dull, 0x7408e6fd06cb669dull},
    {"VGREEN", 8000, 1, 1932, 15456, 0xd9cce29c0891ef7dull, 0xd5edc2f5d8dfb405ull},
    {"VGRENADE", 8000, 1, 2180, 17440, 0x59c1f1f7b5c59aadull, 0xb913981b200b504dull},
    {"VGRENADL", 8000, 1, 3818, 30544, 0xed7990d20f6beea5ull, 0x9e391451937fe1cdull},
    {"VGRENADS", 8000, 1, 2422, 19376, 0xa3736b556777f8ddull, 0x68b6ca30b45425edull},
    {"VHAL", 8000, 1, 6162, 49296, 0x38cc18ecbce78705ull, 0x4f9b1ee0973918e5ull},
    {"VHARDCUR", 8000, 1, 4238, 33904, 0x2d26d3e1554b4155ull, 0xc979b6cf5606ec05ull},
    {"VHEADLIT", 8000, 1, 2886, 23088, 0x9435f588fb144e3dull, 0x7e4012126544006dull},
    {"VHEAVY", 8000, 1, 1608, 12864, 0xe98e82046cbadc35ull, 0x506f4b64066e2135ull},
    {"VHH1A", 16000, 1, 11840, 47360, 0x5850806a653fb589ull, 0xfb2010d76681c001ull},
    {"VHH1B", 16000, 1, 20586, 82344, 0x46a260ff8182d909ull, 0x2f192cf0de9a21a9ull},
    {"VHIGH", 8000, 1, 1862, 14896, 0xd7a97102a9a5206dull, 0x5661bdbacd18bb0dull},
    {"VHIGRAVT", 8000, 1, 5044, 40352, 0x02cd07b7acec2ad5ull, 0xd635d1aa6ef3c265ull},
    {"VHIRADLS", 8000, 1, 5864, 46912, 0x9ea13e1e80ed2a4dull, 0xb0f074464390d7ddull},
    {"VHM1A", 16000, 1, 10758, 43032, 0x846359038f75243dull, 0x2d9020295ca1c265ull},
    {"VHOLLY", 8000, 1, 25812, 206496, 0xeabc369ae417616dull, 0x1c48bd1b61b4e815ull},
    {"VHORROR", 8000, 1, 3114, 24912, 0x59383173827cccfdull, 0x2df84a157ed08785ull},
    {"VHOSTILE", 8000, 1, 6506, 52048, 0x8fcc855388b2d795ull, 0x72c7557e15c32f25ull},
    {"VHOUR", 8000, 1, 1816, 14528, 0xcd60e6daac9c1325ull, 0x05f67e64c3a4780dull},
    {"VHOURS", 8000, 1, 2462, 19696, 0x098b195ca7fa6175ull, 0x8676b670c4b3d9c5ull},
    {"VHULLPRC", 8000, 1, 4790, 38320, 0x0bca5144bfa174cdull, 0xd7766afd2cee3895ull},
    {"VHULLPRI", 8000, 1, 5758, 46064, 0xd2dbc79494c8041dull, 0x49a05910b7e0368dull},
    {"VHULTEMP", 8000, 1, 5372, 42976, 0x64f939d3f590947dull, 0x460cc399dea6c68dull},
    {"VHUNDRED", 8000, 1, 2352, 18816, 0x33e24e558311b99dull, 0xd6df267a84e09a2dull},
    {"VHW1A", 16000, 1, 23376, 93504, 0x89e1f45092302b8dull, 0x8994db4536dc7d41ull},
    {"VHW1B", 16000, 1, 11398, 45592, 0x47a09e6240c62b31ull, 0x78800399c7425435ull},
    {"VIMSORRY", 8000, 1, 2942, 23536, 0x104a5a4073064d75ull, 0x686051f17389246dull},
    {"VINCHOST", 8000, 1, 5314, 42512, 0x133663989a3d7955ull, 0x0985e21459345955ull},
    {"VINCMISS", 8000, 1, 3786, 30288, 0x5ae2dbda5f37d6f5ull, 0x433e7eee9f0da875ull},
    {"VINERTDA", 8000, 1, 4946, 39568, 0x7c599164f387ff65ull, 0x767a92b79521e7ddull},
    {"VIRGMISS", 8000, 1, 5682, 45456, 0xb2211a9e446249a5ull, 0x23537094606d5ad5ull},
    {"VIRGUIDE", 8000, 1, 4394, 35152, 0xa813febfd11cf80dull, 0xe63104ebb41a911dull},
    {"VIRUSHIS", 16000, 1, 8002, 32008, 0x2cfac25a84ac2fe5ull, 0xcf014eef77a5f781ull},
    {"VJAMIEFF", 8000, 1, 4736, 37888, 0xa8d2ac953d60b445ull, 0x7f7b33c04bb0831dull},
    {"VJUMPJET", 8000, 1, 3252, 26016, 0x857dd1ff33b95ed5ull, 0xcaa102b4994299edull},
    {"VKEY", 8000, 1, 1520, 12160, 0x34f07b36d863981dull, 0xa02fa87254428535ull},
    {"VKS1", 16000, 1, 8800, 35200, 0x20b258e7e8c52db1ull, 0x223f91b763101de5ull},
    {"VKS2", 16000, 1, 9482, 37928, 0xfdd3613f38d04759ull, 0xcbb03881b8fc7e99ull},
    {"VKS3", 16000, 1, 8566, 34264, 0x3a3af40849131a39ull, 0x3b559206845950a1ull},
    {"VKS4", 16000, 1, 15852, 63408, 0x446314fd6e0c6429ull, 0xcf1b34e29f7099a1ull},
    {"VKS5", 16000, 1, 13734, 54936, 0xa62a4b63954c6ba9ull, 0x5d3c550b62e547d9ull},
    {"VKVGOLS1", 8000, 1, 7808, 62464, 0x846fcf18b7fe0795ull, 0x3d42c078761ef37dull},
    {"VKVGOLS2", 8000, 1, 6528, 52224, 0xdfb81dda9f3159edull, 0x3d00af2dafae572dull},
    {"VKVGOLW1", 8000, 1, 21858, 174864, 0x6cfdc7753e085c75ull, 0xdd57022d2782e08dull},
    {"VKVOVH1", 8000, 1, 12000, 96000, 0x3470ac46ddce41a5ull, 0xc579c08633525e6dull},
    {"VKVOVH2", 8000, 1, 12400, 99200, 0x268eba43e4417a3dull, 0x4a06ac1dee7d2fe5ull},
    {"VKVOVS1", 8000, 1, 15600, 124800, 0x4f53a213a3aa547dull, 0x4b50c77d2a2ccd1dull},
    {"VKVOVS2", 8000, 1, 15026, 120208, 0x717a6beb5a8c425dull, 0x8b5db6fc03a9b9adull},
    {"VKVOVW1", 8000, 1, 12290, 98320, 0x08d37f83e10e0f35ull, 0x5de582d557963a85ull},
    {"VKW1", 16000, 1, 8644, 34576, 0x787e24bc284afcb1ull, 0x67dbb1684e68188dull},
    {"VKW2", 16000, 1, 12228, 48912, 0xebaf6a54d6a609e9ull, 0xfcdb10ec6758d475ull},
    {"VKW3", 16000, 1, 8396, 33584, 0x6a3b6b3c5fa55745ull, 0x327ce3efb48828a1ull},
    {"VKW4", 16000, 1, 9522, 38088, 0x5b0dab0243fc77c9ull, 0x1f059a534147a785ull},
    {"VKW5", 16000, 1, 20570, 82280, 0x534db1a3806dd731ull, 0x9459c963298204f5ull},
    {"VKW6", 16000, 1, 14012, 56048, 0x4e36fa67435573adull, 0x14e1d48e3de689d1ull},
    {"VKW7", 16000, 1, 19366, 77464, 0x1e5047a8699963e9ull, 0xac6a29fe460d7acdull},
    {"VKYOTO", 8000, 1, 5606, 44848, 0x35f1bd379be6a7ddull, 0x966b6b83b25b315dull},
    {"VLARGE", 8000, 1, 2314, 18512, 0x33ebcdd319bc5e7dull, 0x21721141508917b5ull},
    {"VLASACTD", 8000, 1, 6754, 54032, 0x1934f251b9a9c825ull, 0x6e7665548303768dull},
    {"VLASER", 8000, 1, 2180, 17440, 0xdc8933a8fed387b5ull, 0x5a9f3cbdac6fa1f5ull},
    {"VLAUNCHR", 8000, 1, 2438, 19504, 0x403d5435496c4a95ull, 0xf5b28ea636e85c85ull},
    {"VLAYER", 8000, 1, 1894, 15152, 0xc80356306e97819dull, 0xeb19174e62ed21f5ull},
    {"VLEFT", 8000, 1, 2304, 18432, 0xe0f417ee36320715ull, 0xd9ee3f3f0dceeccdull},
    {"VLIGHT", 8000, 1, 1946, 15568, 0x720e06e684ac4aa5ull, 0x4db968bb392b1ec5ull},
    {"VLOADZON", 8000, 1, 4916, 39328, 0x793e4e98363db6d5ull, 0x9f01d243c63819d5ull},
    {"VLOGRAVT", 8000, 1, 5544, 44352, 0xd9092e7e4c30b5a5ull, 0x3d769b21f658330dull},
    {"VLOW", 8000, 1, 2036, 16288, 0xbb11694d81864275ull, 0x2f0f35cb5d57e80dull},
    {"VMACHGUN", 8000, 1, 2848, 22784, 0xa591d12c0a56d465ull, 0x517aa6b56edfe40dull},
    {"VMAGDEF", 8000, 1, 5054, 40432, 0xb134b1a09ee3c3e5ull, 0xd0238808c5fc3a85ull},
    {"VMAGLEV", 8000, 1, 2696, 21568, 0x5813334aa5c49b05ull, 0xc5a8515156931c65ull},
    {"VMCRWGUN", 8000, 1, 4404, 35232, 0xf3b69ff906471025ull, 0xab16704c5609ca0dull},
    {"VMEDIUM", 8000, 1, 2300, 18400, 0x971907f9a20074e5ull, 0x83107f01f9d3ba3dull},
    {"VMESNGUN", 8000, 1, 3674, 29392, 0x91af9138dbd99d1dull, 0x079d774764db1d65ull},
    {"VMILLION", 8000, 1, 2542, 20336, 0x163a26c94335e815ull, 0x963ea01029458775ull},
    {"VMINE", 8000, 1, 2334, 18672, 0x5b3adb99dbec3c5dull, 0x81009216aa4ae6cdull},
    {"VMINEDTD", 8000, 1, 4564, 36512, 0x13808a9c29a42f05ull, 0x9e09e52c1b8e9775ull},
    {"VMINEDTR", 8000, 1, 4000, 32000, 0x41710e305937cea5ull, 0x6b2955d90cc9b815ull},
    {"VMINELAY", 8000, 1, 2910, 23280, 0x1f5fb1c91100bff5ull, 0x304ef8a107b0058dull},
    {"VMINES", 8000, 1, 2568, 20544, 0x8358b53efead8785ull, 0xc0b18ea601b285edull},
    {"VMINESTR", 8000, 1, 3918, 31344, 0x96c0e2ff0274c335ull, 0x3550d026a3c666b5ull},
    {"VMINUTE", 8000, 1, 1836, 14688, 0xc873ff24f9c3216dull, 0xe56909c1fc60bfb5ull},
    {"VMINUTES", 8000, 1, 2580, 20640, 0x7f4800171a8398adull, 0x793e761b8d6666cdull},
    {"VMISFAIL", 8000, 1, 6266, 50128, 0xf04af3b43fad84b5ull, 0xcb357931aad24265ull},
    {"VMISLAUN", 8000, 1, 3706, 29648, 0xcf8800605e5df195ull, 0x1657e3aed117d8c5ull},
    {"VMISSCOM", 8000, 1, 5912, 47296, 0x6f5e36ccd24666cdull, 0xf16bf30b320f6c8dull},
    {"VMISSILE", 8000, 1, 2004, 16032, 0x6a7861d6dfe8ee7dull, 0x6781d25fdecd53fdull},
    {"VMISTLET", 8000, 1, 2420, 19360, 0xfef6eb0cef56dc9dull, 0x122ee01a708076f5ull},
    {"VMM", 8000, 1, 2440, 19520, 0x9cded06340bce53dull, 0xfcadf8af1cff956dull},
    {"VMORTAR", 8000, 1, 1714, 13712, 0x2aa9f5f4f50d0e55ull, 0xfdc373668b22ed0dull},
    {"VMOVEALG", 8000, 1, 3566, 28528, 0xcb0904476c078665ull, 0x2c072e4cc949c1edull},
    {"VMULTROC", 8000, 1, 3864, 30912, 0xcac2815c625b8795ull, 0x37019578768fa09dull},
    {"VNANOVIR", 8000, 1, 11518, 92144, 0x98fd354be653cdbdull, 0xa73ea1d02e45175dull},
    {"VNAPALM", 8000, 1, 2354, 18832, 0xc7882176f3b222bdull, 0xb89f97edaffc0c0dull},
    {"VNARMREP", 8000, 1, 7460, 59680, 0x565b1417a96f46fdull, 0xbc9b9f7221230855ull},
    {"VNATSALU", 8000, 1, 4848, 38784, 0x51c1db1fc14802f5ull, 0x2c1012e0262f0f7dull},
    {"VNEWCOMP", 8000, 1, 10236, 81888, 0xcfe3883ebd9e011dull, 0x012a7bcba27a214dull},
    {"VNEWMISS", 8000, 1, 6264, 50112, 0x282e4899eccbffd5ull, 0x92b1cd2edd29243dull},
    {"VNEWMOBJ", 8000, 1, 5202, 41616, 0x0df10fb52ffa859dull, 0x89804f0dfb27ae75ull},
    {"VNICEDAY", 8000, 1, 4668, 37344, 0x73d9500476f75ac5ull, 0xa33ebf264fee5485ull},
    {"VNITEVIS", 8000, 1, 3026, 24208, 0xc55e2eba5bb29e1dull, 0x6d60529d6503c1d5ull},
    {"VNOEQUNA", 8000, 1, 7314, 58512, 0xffa9501a70726485ull, 0xebbbd6b9a7051cd5ull},
    {"VNOPARKG", 8000, 1, 3564, 28512, 0xdb5c3c4f0fa23075ull, 0x02118d8e0798952dull},
    {"VNOSMOKG", 8000, 1, 3732, 29856, 0x4ddbb284c229b425ull, 0xdd2e697110d4a6fdull},
    {"VNOWEAPS", 8000, 1, 5252, 42016, 0x47bc121c0402d335ull, 0x96e41ed63f1a593dull},
    {"VOB1", 8000, 1, 8956, 71648, 0xe6c813404340a40dull, 0xe984ee01682e65ddull},
    {"VODLOVH1", 8000, 1, 13600, 108800, 0x04e104487507c63dull, 0x887fc4186acbf175ull},
    {"VODLOVH2", 8000, 1, 10696, 85568, 0x87aa963b6454ba7dull, 0xf697005110ba5ccdull},
    {"VODLOVS1", 8000, 1, 14000, 112000, 0xc74fdd7dc1e2c81dull, 0x47a10659cdf32135ull},
    {"VODLOVS2", 8000, 1, 10400, 83200, 0xc96602395c45059dull, 0x0577ed217b22bd75ull},
    {"VODLOVW1", 8000, 1, 17200, 137600, 0x0fae0c775a3e41edull, 0xa23cd288ad154265ull},
    {"VOFFLINE", 8000, 1, 2774, 22192, 0xd8301b886d43c08dull, 0x88eae6fd270a6555ull},
    {"VOHDARN", 8000, 1, 2606, 20848, 0xba6b9366d1cbeeadull, 0xd6dda545e92c0c5dull},
    {"VONLINE", 8000, 1, 2352, 18816, 0x5322007503fc31c5ull, 0x904bc72388ea172dull},
    {"VOVERDED", 8000, 1, 24260, 194080, 0x88717ccd5b2de815ull, 0x1ced63223ab0875dull},
    {"VOVERDRV", 8000, 1, 2982, 23856, 0x9ac2d85bc3aeb345ull, 0x9a074ce28c9b7c05ull},
    {"VOVERTHR", 8000, 1, 3860, 30880, 0x8c7c0d6f809f9885ull, 0xa624bd8a1ae3fa35ull},
    {"VPATRADS", 8000, 1, 6442, 51536, 0x8407bbda8a6a4ec5ull, 0xf7a1391539c1b61dull},
    {"VPERCENT", 8000, 1, 2622, 20976, 0xad1618f37f01c7a5ull, 0x46a52c6e8f53d33dull},
    {"VPHALADS", 8000, 1, 6714, 53712, 0x3ea1ab380ad467cdull, 0x65db66aff9497695ull},
    {"VPLASMAC", 8000, 1, 3860, 30880, 0x8a2c056373afed15ull, 0x20f24b40d9182bedull},
    {"VPLEASE", 8000, 1, 2410, 19280, 0x603bb137eab449adull, 0x19668f0c56ed5185ull},
    {"VPOLIC6A", 16000, 1, 13582, 54328, 0xa342381728b9e085ull, 0x4008fbcd81ff35bdull},
    {"VPOLIC6B", 16000, 1, 14146, 56584, 0xa933d18792291f01ull, 0xe1b3dc8a0a1559ddull},
    {"VPOLICE1", 16000, 1, 47062, 188248, 0xa950510b0cb509a1ull, 0x4d69349aa1d9f261ull},
    {"VPOLICE2", 16000, 1, 12698, 50792, 0x8ea112fff4610639ull, 0x161771b657e9da31ull},
    {"VPOLICE3", 16000, 1, 10976, 43904, 0x6f956a4aec24c009ull, 0x0e90c416ce65e46dull},
    {"VPOLICE4", 16000, 1, 11256, 45024, 0xd0cc1f7459121469ull, 0xe3269a4d2563de49ull},
    {"VPOLICE5", 16000, 1, 10228, 40912, 0x3039db1703fe1035ull, 0x1b71271add63b63dull},
    {"VPOLICE6", 16000, 1, 27730, 110920, 0x01bc4ae937919ecdull, 0xfb26ecf64ac95d61ull},
    {"VPOWERUP", 8000, 1, 2760, 22080, 0x422a43a6155c1545ull, 0x49d6cf1c7546b83dull},
    {"VPROCAUT", 8000, 1, 5484, 43872, 0xec48c657a6a6b4d5ull, 0x6168d3bba238d68dull},
    {"VPROCBRF", 8000, 1, 5300, 42400, 0x3adef6325ce01df5ull, 0x9a33d90863f3a68dull},
    {"VPROCTBR", 8000, 1, 6298, 50384, 0x8b0dbc999b5308a5ull, 0xb7597beb148380cdull},
    {"VPROCTIH", 8000, 1, 7262, 58096, 0xe63a9aab0d11d98dull, 0x8fb74e6cd7be751dull},
    {"VPROCTTG", 8000, 1, 6262, 50096, 0xc91c648279a04a5dull, 0x14f596e44293a9adull},
    {"VRADAR", 8000, 1, 2482, 19856, 0xbd2f4c97120b4215ull, 0x07520ac7e0e20a7dull},
    {"VRADLEVL", 8000, 1, 5162, 41296, 0xf0111f0aa0564085ull, 0x52fc24c02ba6b835ull},
    {"VRAILGUN", 8000, 1, 2734, 21872, 0x1d6b56095562bff5ull, 0xb1db96299b0c1e85ull},
    {"VRCHAMBR", 32000, 2, 188180, 188180, 0x5fa952cce363cc2aull, 0x8158ee095dc06e16ull},
    {"VRCHAMHQ", 32000, 2, 198950, 198950, 0x3508a3bdbd2abb7cull, 0x31df76a43edff3e6ull},
    {"VRED", 8000, 1, 1170, 9360, 0x7dff60d74ecf7905ull, 0x9aecdcdb8e35b535ull},
    {"VREFUSE", 8000, 1, 10642, 85136, 0x7ffad6e15fa21fcdull, 0x1c871512b7253a65ull},
    {"VREGRETS", 8000, 1, 9692, 77536, 0xf49f436c50f02a1dull, 0xcdebeeacc4592d2dull},
    {"VREPCOIL", 8000, 1, 4782, 38256, 0xb7220d9ae602adc5ull, 0x8643de14a8c79b55ull},
    {"VREPRKIT", 8000, 1, 3226, 25808, 0x40a7540d9f628b85ull, 0x7e085ba6bff4cce5ull},
    {"VRESTORE", 8000, 1, 2444, 19552, 0x50ad80d63f3fccbdull, 0x4ae45ff850266a45ull},
    {"VRGMISS", 8000, 1, 5312, 42496, 0x2e8ccae55c798505ull, 0x00c2163d6120a90dull},
    {"VRGUIDED", 8000, 1, 3550, 28400, 0xbfc57dbad250b87dull, 0x0220cd8d6e129c7dull},
    {"VRH1A", 16000, 1, 13262, 53048, 0x222d30f58ff9d1f9ull, 0xfcc81dca1d9a3b39ull},
    {"VRH1B", 16000, 1, 11792, 47168, 0xade2f8607df8ea41ull, 0x74ae51e69d912c31ull},
    {"VRH1C", 16000, 1, 10966, 43864, 0x033d4ff2ae0f13c9ull, 0x65400ef9c0b5fd51ull},
    {"VRH1D", 16000, 1, 12190, 48760, 0x0bd5ab7e05472c35ull, 0x402f602fd4fc0135ull},
    {"VRIGHT", 8000, 1, 1760, 14080, 0x586e616e378680c5ull, 0x492eff1485ae93c5ull},
    {"VRM1A", 16000, 1, 13094, 52376, 0x6039f666eaec3981ull, 0xaa115fafb9884021ull},
    {"VRM1B", 16000, 1, 6892, 27568, 0x99679fb2ce087165ull, 0xa13c38af901bcfe1ull},
    {"VRM2A", 16000, 1, 19456, 77824, 0x988ec8c09e8a82b1ull, 0x046eb8cd73fd5ab1ull},
    {"VRM2B", 16000, 1, 9356, 37424, 0x68de0093f546b111ull, 0x76f55500e8d9c81dull},
    {"VROCKET", 8000, 1, 2158, 17264, 0x5b3bf8c1395a0575ull, 0x88dbdab97a03bf8dull},
    {"VRPV", 8000, 1, 3298, 26384, 0x6ac5436f404ac245ull, 0x59915933921da8b5ull},
    {"VRPVDEST", 8000, 1, 5456, 43648, 0x9b588b65342dd615ull, 0xf2eb0e4718d1bbcdull},
    {"VRS1A", 16000, 1, 23564, 94256, 0x9e26a4a7dfdcc0c5ull, 0x9a137ff7813ee9e5ull},
    {"VRS1B", 16000, 1, 15254, 61016, 0x6500b1b28e33ed71ull, 0xb487496b6fc92221ull},
    {"VRS1C", 16000, 1, 13544, 54176, 0xb24c82d71dcfa7c1ull, 0xe15621db4fa9035dull},
    {"VRS2A", 16000, 1, 12670, 50680, 0x98fcd49a803410b5ull, 0xdbd22f0979870471ull},
    {"VRW1A", 16000, 1, 15470, 61880, 0xb5bb6d762c46b045ull, 0xedf8e81f4af10cd9ull},
    {"VRW1B", 16000, 1, 13810, 55240, 0x66ab9bc61a1a3e9dull, 0x2d23b43f061f012dull},
    {"VRW1C", 16000, 1, 14600, 58400, 0x0d8947a448c76a85ull, 0xdf1cca648182bc95ull},
    {"VRW2A", 16000, 1, 17828, 71312, 0x576ca1c17b44c971ull, 0x5fc47afd5c980071ull},
    {"VRW2B", 16000, 1, 17160, 68640, 0x5c611528cf2a8695ull, 0x3f454dddefb96dc5ull},
    {"VSECBRCH", 8000, 1, 4734, 37872, 0x014b36ba5248212dull, 0x649f30ffc65c54cdull},
    {"VSECOND", 8000, 1, 2246, 17968, 0xf54ba4a4ac130eedull, 0xf05d094e199bd895ull},
    {"VSECONDS", 8000, 1, 2998, 23984, 0xc811bcf4acce64b5ull, 0x0c111850196941e5ull},
    {"VSELFDES", 8000, 1, 6176, 49408, 0x0bb23f2f068bbec5ull, 0x039e9436b7cb10f5ull},
    {"VSIMHEL1", 8000, 1, 7374, 58992, 0x2e63655a254800edull, 0x62723e6ce59c43cdull},
    {"VSIMHEL2", 8000, 1, 13896, 111168, 0xe580f7ab15b22ed5ull, 0xca67c4cb7907270dull},
    {"VSIMHEL3", 8000, 1, 11088, 88704, 0x1ac899fc405d9005ull, 0xc876466af3b4fa8dull},
    {"VSIMHEL4", 8000, 1, 5988, 47904, 0x5f5b8cee7b7436bdull, 0x3f3798580c34bd75ull},
    {"VSIMHEL5", 8000, 1, 5494, 43952, 0xa170a38306c27745ull, 0x03693989b5e4f63dull},
    {"VSIMHEL6", 8000, 1, 4812, 38496, 0x225b33b1b91c964dull, 0x77e6753e12b35715ull},
    {"VSIMHEL7", 8000, 1, 7052, 56416, 0x8b5ae8a8eea35b35ull, 0xa4ded4cbb84121adull},
    {"VSIMHEL8", 8000, 1, 4354, 34832, 0x18fb8faabf53f8cdull, 0x71daccf024394705ull},
    {"VSIMHEL9", 8000, 1, 4866, 38928, 0x5eb327a300e3520dull, 0x5362e1ff9f8288a5ull},
    {"VSIMHELA", 8000, 1, 14308, 114464, 0xc7dda56d5f026c45ull, 0x19b91581aafbe41dull},
    {"VSIMHELB", 8000, 1, 4438, 35504, 0xa05a4f9fc428f725ull, 0x56173edc29aa927dull},
    {"VSIMHELC", 8000, 1, 6242, 49936, 0x051a1cd127640175ull, 0x536ac71bffcc4f0dull},
    {"VSMALL", 8000, 1, 2360, 18880, 0x7a02072e3162fbc5ull, 0xa759041356e876f5ull},
    {"VSMOKDIS", 8000, 1, 4682, 37456, 0x0ce7981550a4431dull, 0xf28754b931d7e995ull},
    {"VSONICDA", 8000, 1, 4424, 35392, 0xcecb822cec4ba695ull, 0x9eb7b7f2dbbc4ed5ull},
    {"VSORRY", 8000, 1, 2448, 19584, 0x13ed0318f41da30dull, 0xa980bcc0fcf68a75ull},
    {"VSPAPART", 8000, 1, 3700, 29600, 0xb91208d8251df18dull, 0x52e0184ca09acdddull},
    {"VSTANDRD", 8000, 1, 3126, 25008, 0x74dc0f0d283da2fdull, 0x48672f6280be6b65ull},
    {"VSTEALTH", 8000, 1, 2794, 22352, 0xdc34c4eb3f60891dull, 0x4daece9083b08625ull},
    {"VSYSFAIL", 8000, 1, 3908, 31264, 0x1f6bf6f336cee8c5ull, 0x629a536df026363dull},
    {"VTACNUKE", 8000, 1, 3284, 26272, 0xf99d15862726143dull, 0x6274b350f806c00dull},
    {"VTANKDES", 8000, 1, 6072, 48576, 0xb1159dcf9f2633e5ull, 0x5f3ee3d8c90f043dull},
    {"VTANKSEL", 8000, 1, 4312, 34496, 0xd603e0b6b3c54d0dull, 0x75f4287f271fe405ull},
    {"VTARGCOM", 8000, 1, 4524, 36192, 0x80721d2953221575ull, 0x432a3e7e33ab39a5ull},
    {"VTELPORT", 8000, 1, 3062, 24496, 0xb90a6bc7f4feb41dull, 0x9b072e7162649d5dull},
    {"VTELPRTL", 8000, 1, 3250, 26000, 0xdb25cf6626e02305ull, 0x5d251b709e57775dull},
    {"VTGMISS", 8000, 1, 4388, 35104, 0xeca7eea219a1faedull, 0x8f3b1eb0ef164725ull},
    {"VTGUIDED", 8000, 1, 3404, 27232, 0x445d95191a5f86ddull, 0xc103da56e41de395ull},
    {"VTHOUSND", 8000, 1, 2386, 19088, 0x87c7204e8947f9adull, 0x5d1b65cac5d684b5ull},
    {"VTHTHAT", 8000, 1, 2886, 23088, 0xf6f3d7a76c68603dull, 0xe6a0800dde379ac5ull},
    {"VTHTTHAT", 8000, 1, 3548, 28384, 0x756af7d3975db0b5ull, 0xc1c0efc7a6a69d9dull},
    {"VTIMEDIS", 8000, 1, 5940, 47520, 0x3536b1e21b05ed5dull, 0xf79230e04b95c28dull},
    {"VTMPSTEL", 8000, 1, 7848, 62784, 0x8875ac8ae54da04dull, 0xa83a6146cb14402dull},
    {"VTONUKE", 8000, 1, 5368, 42944, 0x716da062f4398d7dull, 0x18297200f6de726dull},
    {"VTRACERS", 8000, 1, 3084, 24672, 0xef1642b720d6118dull, 0x6485fd7519511d45ull},
    {"VTRANGA", 8000, 1, 6516, 52128, 0xec791943c7c933a5ull, 0x2f7ddb755aff81ddull},
    {"VTRBEAM", 8000, 1, 3398, 27184, 0xee4367b60351ab1dull, 0xd68cb02d416f93fdull},
    {"VTURBOB", 8000, 1, 3956, 31648, 0xa991b3019a7f5465ull, 0x4bbdf2ba95d7a445ull},
    {"VTWINPKS", 8000, 1, 23044, 184352, 0x70b77379f71f30d5ull, 0xda9385197a5aa0cdull},
    {"VUNAUTHT", 8000, 1, 9982, 79856, 0xb052d88fb28632edull, 0x95d38337c3246475ull},
    {"VVIRTCON", 8000, 1, 6028, 48224, 0xbb825549dbfe9825ull, 0xa6d3e20a8eba6545ull},
    {"VVP1", 16000, 1, 20118, 80472, 0x0419878d650601b1ull, 0x87b2166157aae0d9ull},
    {"VVP2", 16000, 1, 12632, 50528, 0xe0c0dbbfc8a6a38dull, 0x6664e0886abd2191ull},
    {"VVP3", 16000, 1, 14792, 59168, 0x0870db98f48ca1a9ull, 0xf3432eebf3da75d9ull},
    {"VVPREN", 16000, 1, 10188, 40752, 0x3039f4cf0fe4f615ull, 0x9609d365dcfd0c39ull},
    {"VVRYGOOD", 8000, 1, 3368, 26944, 0x7f0532e930396bddull, 0x82e93b24e75f8e8dull},
    {"VWARNING", 8000, 1, 2244, 17952, 0x643f0b7a7168dee5ull, 0x566b8b42faa7a0e5ull},
    {"VWATERRS", 8000, 1, 11348, 90784, 0xe4ecf8118453575dull, 0x35e3fc096ea6f405ull},
    {"VWHITE", 8000, 1, 1682, 13456, 0x5c61d9fa892c39ddull, 0x11f8afff93560d4dull},
    {"VWOOHOO", 8000, 1, 2058, 16464, 0x53c57b95e120e025ull, 0xcae4066d65183845ull},
    {"WATERF", 16000, 1, 31948, 127792, 0x34a7dcbd0d79a065ull, 0x20c249f90af76d35ull},
    {"WETS2", 16000, 1, 10650, 42600, 0x52f8bf60d74fe2d5ull, 0xb2e539f6cdd219f9ull},
    {"WETSWIT1", 16000, 1, 4730, 18920, 0x7e13e5011941c911ull, 0xf5b9cfd9f29b7b05ull},
    {"WETSWIT3", 16000, 1, 4308, 17232, 0xdeed9685dd3147c1ull, 0xcfd1c481fb4e2a3dull},
    {"WETSWIT4", 16000, 1, 16376, 65504, 0xd67772527275b439ull, 0x72140c2f5e248a2dull},
    {"WGCE", 16000, 1, 20616, 82464, 0xb34d1aa63e904609ull, 0xd9edf91265505dbdull},
    {"WGCF", 16000, 1, 11546, 46184, 0x2567ebed8820c015ull, 0x72c5c3daaa62b445ull},
    {"WGL", 16000, 1, 13768, 55072, 0x13a87c01656567b5ull, 0x907209e166efd125ull},
    {"WGO", 16000, 1, 5514, 22056, 0x85f552dd137dd831ull, 0x31fe2c3fc1e769fdull},
    {"WINDTUNK", 16000, 1, 21162, 84648, 0xcf7b0f0f58f48c91ull, 0x986973d161523c65ull},
    {"WINDTUNL", 16000, 1, 23564, 94256, 0xf606d7c1cd88e639ull, 0x041c8445494356e9ull},
    {"WINDTUNX", 16000, 1, 21162, 84648, 0x42f9dbcd6df7449dull, 0x620a6bcfc3ee2b45ull},
    {"XALLARMD", 8000, 1, 7108, 56864, 0x86b7856197cdf8fdull, 0xf75839aace2297cdull},
    {"XALLMISS", 8000, 1, 11854, 94832, 0xae6e4fde181aa5bdull, 0x398a11cdc370478dull},
    {"XAMMODEP", 8000, 1, 9046, 72368, 0x0daa478a3d6c269dull, 0x30371507e127204dull},
    {"XARMDEST", 8000, 1, 5146, 41168, 0x78e0906562127335ull, 0xca26322cb3df8c1dull},
    {"XBACK", 8000, 1, 9134, 73072, 0xc72884ee4e117945ull, 0xd5bfe7ef6763f15dull},
    {"XCAUTION", 8000, 1, 3426, 27408, 0xbc5e26eeae4e1c45ull, 0x950bba875d3fa065ull},
    {"XCRTSYSF", 8000, 1, 8550, 68400, 0xc2dba59a0304fccdull, 0x551b5a6cda09658dull},
    {"XDANGER", 8000, 1, 2492, 19936, 0x2623f4cb219a2a75ull, 0x7b38ecf4187c3bedull},
    {"XENCOMBT", 8000, 1, 6992, 55936, 0xc4458d42b2fde985ull, 0xda4eca7990c4d605ull},
    {"XFAILURE", 8000, 1, 12804, 102432, 0xae7d7f3d3de87525ull, 0x7e896bf3a6872aa5ull},
    {"XFRONT", 8000, 1, 2906, 23248, 0x5de6a1c92ae4ea25ull, 0xa99d7678d77435e5ull},
    {"XHOSTILE", 8000, 1, 10680, 85440, 0x6e2b51ba4c06ab15ull, 0x17be06fbffe58ca5ull},
    {"XLEFT", 8000, 1, 8154, 65232, 0x60498b9b41adca0dull, 0xe72bbba60f6b36b5ull},
    {"XMISFAIL", 8000, 1, 7608, 60864, 0x7296231e14cbc75dull, 0xd245db3f2464c715ull},
    {"XMISSCOM", 8000, 1, 10000, 80000, 0x977893bbcd5606a5ull, 0x0fb4a421175efad5ull},
    {"XOFFLINE", 8000, 1, 4172, 33376, 0xcc6c846737dc501dull, 0xb77ae68bd4caf6cdull},
    {"XONLINE", 8000, 1, 4606, 36848, 0xa1bce231c01e8bfdull, 0xfa4e9709cc3b4f25ull},
    {"XRIGHT", 8000, 1, 3094, 24752, 0x10bb984829063f45ull, 0x9e07548e72aa314dull},
    {"XSYSFAIL", 8000, 1, 4882, 39056, 0x0e9fafa9930167cdull, 0x7206bdb74e6c5ecdull},
    {"XWARNING", 8000, 1, 8730, 69840, 0x16c8487524b70fcdull, 0xe454b2d2b97a9fe5ull},
    {"YODEL", 16000, 1, 23106, 92424, 0x57d8dafe4092e629ull, 0x109e691705556e11ull},
};

// Quick-pass pinning: every sample rate, mono and stereo, the longest tracks,
// the harness-bug boundary (PRAM, SOYBOY, INTRO), and the loudest record.
struct SpotRef {
    const char* name;
    std::uint64_t raw;
    std::uint64_t pcm5;
    std::uint64_t pcm1;
    std::uint64_t wav;
};

constexpr SpotRef kSpots[] = {
    {"ADON", 0x9d67e203de04e745ull, 0x61661474c31c21a5ull, 0x39d9b5f9353e0b85ull,
     0xa282a26f9df9e307ull},
    {"ALARMA", 0xd06958af09c0ce45ull, 0xbcba2481704a2191ull, 0x2a54f306a7711601ull,
     0xb489d1ba21d25347ull},
    {"CLICKBLT", 0xefbeb12cc846364dull, 0x8fb6f27dc2626061ull, 0x0d7f4a775f4de38dull,
     0x62d16aa30aa140c3ull},
    {"CRUNCH4", 0x00e3c5362349bf51ull, 0x6a30a67a41f9965dull, 0x6fc19f405dfb7ae5ull,
     0x27beb6313d9a7a21ull},
    {"PRAM", 0x4a176ea1ca9a0069ull, 0xe414c77fa953cde9ull, 0xb5dccc8eafa8f1d9ull,
     0x80492e1bd01ad179ull},
    {"SILENCE", 0xbeb46562da647365ull, 0x00410e1ab02ebf95ull, 0xbae216d03fe7d2e5ull,
     0x64277043ca559075ull},
    {"SOYBOY", 0x3ed95b2a91404631ull, 0x922aed3a9e92a17full, 0x344c630009d8330full,
     0x96a40c803ead8105ull},
    {"CUT-EMP", 0x689ce6f75d8162feull, 0xd1ff4f7e31ea0321ull, 0x9a2130c2cd6af49cull,
     0x13641494e1ac10f9ull},
    {"INTRO", 0x0ca1b6274f408593ull, 0xf8d0e99495112d6full, 0xf429af07086f852aull,
     0x1949608de71abe37ull},
    {"VRCHAMBR", 0x5fa952cce363cc2aull, 0x8158ee095dc06e16ull, 0xa32b13dd2d77093full,
     0xd02f02f83f76555eull},
    {"VRCHAMHQ", 0x3508a3bdbd2abb7cull, 0x31df76a43edff3e6ull, 0x3224b7dc1aa9b32dull,
     0xe2dc9a37a988707eull},
};

bool g_quick = true;

// The volume index selects the starting row of the shared delta curve
// (0x850480).  These hashes cover the rows the corpus does not reach (every
// shipped entry plays at volume 31) by decoding two records through all of the
// table bases the reference model was checked with.
struct VolumeRef {
    const char* name;
    int volume;
    std::uint64_t raw;
};

constexpr VolumeRef kVolumes[] = {
    {"SILENCE", 0, 0x9db61af1ed32cb25ull},  {"SILENCE", 7, 0x457b8ff446097a05ull},
    {"SILENCE", 15, 0x59636ecbfed26dc5ull}, {"SILENCE", 23, 0x96f55a93315b4645ull},
    {"SILENCE", 31, 0xbeb46562da647365ull}, {"ADON", 0, 0x9fc5cd2ca573371dull},
    {"ADON", 7, 0xee117dddfd25a545ull},     {"ADON", 15, 0x23bb7adb333fcc05ull},
    {"ADON", 23, 0x88436493cf201bd5ull},    {"ADON", 31, 0x9d67e203de04e745ull},
};

std::uint64_t fnv_u32(std::uint64_t h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xFFu)) * kFnvPrime;
    }
    return h;
}

std::uint64_t fnv_u16(std::uint64_t h, std::uint16_t v) {
    h = (h ^ (v & 0xFFu)) * kFnvPrime;
    return (h ^ (v >> 8)) * kFnvPrime;
}

std::uint64_t fnv_bytes(std::uint64_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * kFnvPrime;
    }
    return h;
}

const SpotRef* find_spot(const std::string& name) {
    for (const SpotRef& ref : kSpots) {
        if (name == ref.name) {
            return &ref;
        }
    }
    return nullptr;
}

std::int16_t clip16(std::int32_t v) {
    return static_cast<std::int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

// Reference PCM16 conversion over the raw interleaved slots, as Python does.
std::uint64_t pcm16_fnv(const gm::RawClip& raw, int shift) {
    std::uint64_t h = kFnvBasis;
    for (const std::int32_t v : raw.samples) {
        const std::int16_t s = clip16(v >> shift);
        h = (h ^ static_cast<std::uint8_t>(s & 0xFF)) * kFnvPrime;
        h = (h ^ static_cast<std::uint8_t>((s >> 8) & 0xFF)) * kFnvPrime;
    }
    return h;
}

void test_synthetic() {
    // Minimal valid SOU1 header with no payload.
    std::vector<std::uint8_t> blob = {'S', 'O', 'U', '1', 0x40, 0x1F, 0, 0,
                                      0,   0,   0,   0,   0,    0,    0, 0};
    std::string error;
    auto info = gm::parse_sou1_header(blob, error);
    CHECK(info.has_value());
    if (info) {
        CHECK(info->magic == "SOU1");
        CHECK(info->sample_rate == 8000);
        CHECK(info->channels == 1);
        CHECK(info->payload_bytes == 0);
        CHECK(info->slots_per_byte == 8);
    }
    auto raw = gm::decode_sou1_raw(blob, error);
    CHECK(raw.has_value());
    if (raw) {
        CHECK(raw->sample_rate == 32000);
        CHECK(raw->channels == 1);
        CHECK(raw->samples.empty());
    }

    // Declared length is clamped to the blob and rounded down to even.
    blob = {'S', 'O', 'U', '1', 0x80, 0x3E, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0x11, 0x22, 0x33};
    info = gm::parse_sou1_header(blob, error);
    CHECK(info.has_value());
    if (info) {
        CHECK(info->payload_bytes == 2);
        CHECK(info->slots_per_byte == 4);
    }

    // Stereo flag.
    blob[8] = 1;
    info = gm::parse_sou1_header(blob, error);
    CHECK(info.has_value() && info->channels == 2);

    // Failures: wrong magic, short header, implausible flags, bad volume,
    // SOU0 (raw PCM) through the ADPCM decoder.
    auto bad = std::vector<std::uint8_t>{'S', 'O', 'U', '2', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!gm::parse_sou1_header(bad, error));
    bad = std::vector<std::uint8_t>{'S', 'O', 'U', '1', 0, 0, 0};
    CHECK(!gm::parse_sou1_header(bad, error));
    bad = std::vector<std::uint8_t>{'S', 'O', 'U', '1', 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!gm::parse_sou1_header(bad, error));
    blob = std::vector<std::uint8_t>{'S', 'O', 'U', '1', 0x40, 0x1F, 0, 0,
                                     0,   0,   0,   0,   0,    0,    0, 0};
    CHECK(!gm::decode_sou1_raw(blob, error, 32));
    CHECK(!gm::decode_sou1_raw(blob, error, -1));
    CHECK(!gm::decode_sou1(blob, error, 31, 32));
    blob[3] = '0';
    CHECK(gm::parse_sou1_header(blob, error).has_value());
    CHECK(!gm::decode_sou1_raw(blob, error));
}

void test_archive() {
    const fs::path idx = "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX";
    std::error_code ec;
    if (!fs::is_regular_file(idx, ec)) {
        std::printf("sou1: game data not present, parity test skipped\n");
        return;
    }

    std::string error;
    const auto big = gm::BigFile::open(idx, error);
    CHECK(big.has_value());
    if (!big) {
        std::printf("sou1: %s\n", error.c_str());
        return;
    }
    CHECK(big->entries().size() == kEntries);

    std::unordered_map<std::string, const RecordRef*> manifest;
    for (const RecordRef& ref : kRecords) {
        manifest.emplace(ref.name, &ref);
    }
    CHECK(manifest.size() == kSou1);

    std::size_t n0 = 0;
    std::size_t n1 = 0;
    std::size_t n2 = 0;
    std::size_t bad_headers = 0;
    std::size_t decode_errors = 0;
    std::size_t checked = 0;
    std::size_t spot_checks = 0;
    std::size_t rate8 = 0;
    std::size_t rate16 = 0;
    std::size_t rate32 = 0;
    std::size_t stereo = 0;
    std::uint64_t total_slots = 0;
    std::uint64_t total_samples = 0;
    std::uint64_t raw_fnv = kFnvBasis;
    std::uint64_t pcm5_fnv = kFnvBasis;
    std::uint64_t pcm1_fnv = kFnvBasis;
    std::uint64_t max_abs_raw = 0;
    std::string first_error;

    for (const auto& entry : big->entries()) {
        const auto bytes = big->read(entry, error);
        CHECK(bytes.has_value());
        if (!bytes) {
            break;
        }
        if (bytes->size() < 4) {
            ++bad_headers;
            continue;
        }
        const bool is0 = std::memcmp(bytes->data(), "SOU0", 4) == 0;
        const bool is1 = std::memcmp(bytes->data(), "SOU1", 4) == 0;
        const bool is2 = std::memcmp(bytes->data(), "SOU2", 4) == 0;
        if (is2) {
            ++n2;
            CHECK(!gm::parse_sou1_header(*bytes, error));
            continue;
        }
        if (!is0 && !is1) {
            ++bad_headers;
            continue;
        }
        const auto info = gm::parse_sou1_header(*bytes, error);
        CHECK(info.has_value());
        if (!info) {
            ++bad_headers;
            continue;
        }
        if (is0) {
            ++n0;
            CHECK(!gm::decode_sou1_raw(*bytes, error));
            continue;
        }
        ++n1;
        if (info->sample_rate == 8000) {
            ++rate8;
        } else if (info->sample_rate == 16000) {
            ++rate16;
        } else if (info->sample_rate == 32000) {
            ++rate32;
        }
        stereo += info->channels == 2 ? 1 : 0;

        const auto it = manifest.find(entry.name);
        CHECK(it != manifest.end());
        if (it == manifest.end()) {
            continue;
        }
        const RecordRef& ref = *it->second;
        CHECK(info->sample_rate == ref.rate);
        CHECK(info->channels == ref.channels);
        CHECK(info->payload_bytes == ref.payload_bytes);

        const SpotRef* spot = find_spot(entry.name);
        if (g_quick && spot == nullptr) {
            continue; // quick pass: headers and the pinned records only
        }

        const auto raw = gm::decode_sou1_raw(*bytes, error);
        if (!raw) {
            if (decode_errors == 0) {
                first_error = entry.name + ": " + error;
            }
            ++decode_errors;
            continue;
        }
        CHECK(raw->sample_rate == 32000); // slot stream, see res/sou1.h
        CHECK(raw->channels == ref.channels);
        const std::size_t slots = raw->samples.size() / static_cast<std::size_t>(ref.channels);
        CHECK(slots == ref.slots);
        if (slots != ref.slots) {
            continue;
        }
        ++checked;
        total_slots += slots;
        total_samples += raw->samples.size();

        std::uint64_t rec_raw = kFnvBasis;
        for (const std::int32_t v : raw->samples) {
            const std::uint32_t u = static_cast<std::uint32_t>(v);
            rec_raw = fnv_u32(rec_raw, u);
            raw_fnv = fnv_u32(raw_fnv, u);
            const std::uint64_t mag =
                v < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(v))
                      : static_cast<std::uint64_t>(v);
            if (mag > max_abs_raw) {
                max_abs_raw = mag;
            }
        }
        const std::uint64_t rec_pcm5 = pcm16_fnv(*raw, 5);
        const std::uint64_t rec_pcm1 = pcm16_fnv(*raw, 1);
        if (!g_quick) {
            // Aggregate PCM hashes are built sample by sample (FNV is not
            // composable from per-record hashes).
            for (const std::int32_t v : raw->samples) {
                pcm5_fnv = fnv_u16(pcm5_fnv, static_cast<std::uint16_t>(clip16(v >> 5)));
                pcm1_fnv = fnv_u16(pcm1_fnv, static_cast<std::uint16_t>(clip16(v >> 1)));
            }
        }

        CHECK(rec_raw == ref.raw);
        CHECK(rec_pcm5 == ref.pcm16);
        if (spot != nullptr) {
            ++spot_checks;
            CHECK(rec_raw == spot->raw);
            CHECK(rec_pcm5 == spot->pcm5);
            CHECK(rec_pcm1 == spot->pcm1);
            // Exercise the public 16-bit presentation and the WAV writer.
            const auto pcm1 = gm::decode_sou1(*bytes, error, 31, 1);
            const auto pcm5 = gm::decode_sou1(*bytes, error, 31, 5);
            const auto wav = gm::sou1_to_wav(*bytes, error);
            CHECK(pcm1.has_value() && pcm5.has_value() && wav.has_value());
            if (pcm1 && pcm5 && wav) {
                CHECK(pcm1->sample_rate == 32000);
                CHECK(pcm5->sample_rate == 32000);
                CHECK(pcm1->channels == ref.channels);
                CHECK(pcm1->samples.size() == raw->samples.size());
                CHECK(pcm5->samples.size() == raw->samples.size());
                std::uint64_t h1 = kFnvBasis;
                std::uint64_t h5 = kFnvBasis;
                for (std::size_t i = 0; i < pcm1->samples.size(); ++i) {
                    h1 = fnv_u16(h1, static_cast<std::uint16_t>(pcm1->samples[i]));
                    h5 = fnv_u16(h5, static_cast<std::uint16_t>(pcm5->samples[i]));
                }
                CHECK(h1 == spot->pcm1);
                CHECK(h5 == spot->pcm5);
                const std::uint64_t hw =
                    fnv_bytes(kFnvBasis, wav->data(), wav->size());
                CHECK(hw == spot->wav);
                const std::size_t frames = static_cast<std::size_t>(slots) /
                                           static_cast<std::size_t>(info->slots_per_byte / 2);
                CHECK(wav->size() == 44 + frames * ref.channels * 2);
            }
        }
    }

    // Volume table sweep: every base row of the shared delta curve.
    std::size_t volume_checks = 0;
    for (const VolumeRef& ref : kVolumes) {
        const gm::BigEntry* target = nullptr;
        for (const auto& entry : big->entries()) {
            if (entry.name == ref.name) {
                target = &entry;
                break;
            }
        }
        CHECK(target != nullptr);
        if (target == nullptr) {
            continue;
        }
        const auto bytes = big->read(*target, error);
        CHECK(bytes.has_value());
        if (!bytes) {
            continue;
        }
        const auto raw = gm::decode_sou1_raw(*bytes, error, ref.volume);
        CHECK(raw.has_value());
        if (!raw) {
            continue;
        }
        std::uint64_t h = kFnvBasis;
        for (const std::int32_t v : raw->samples) {
            h = fnv_u32(h, static_cast<std::uint32_t>(v));
        }
        CHECK(h == ref.raw);
        ++volume_checks;
    }

    std::printf("sou1: %zu entries, SOU0 %zu, SOU1 %zu, SOU2 %zu, bad headers %zu\n",
                big->entries().size(), n0, n1, n2, bad_headers);
    std::printf("sou1: rates 8000=%zu 16000=%zu 32000=%zu, stereo %zu\n", rate8, rate16,
                rate32, stereo);
    std::printf("sou1: decoded %zu records, %" PRIu64 " slots/ch, %" PRIu64
                " interleaved samples (%.6f s), max |raw| %" PRIu64 "\n",
                checked, total_slots, total_samples,
                static_cast<double>(total_slots) / 32000.0, max_abs_raw);
    if (!g_quick) {
        std::printf("sou1: raw fnv %016" PRIx64 ", pcm>>5 fnv %016" PRIx64
                    ", pcm>>1 fnv %016" PRIx64 "\n",
                    raw_fnv, pcm5_fnv, pcm1_fnv);
    } else {
        std::printf("sou1: quick pass (%zu pinned records, %zu volume rows)\n", spot_checks,
                    volume_checks);
    }

    CHECK(n0 == kSou0);
    CHECK(n1 == kSou1);
    CHECK(n2 == kSou2);
    CHECK(bad_headers == 0);
    CHECK(decode_errors == 0);
    CHECK(rate8 == 361);
    CHECK(rate16 == 607);
    CHECK(rate32 == 15);
    CHECK(stereo == 6);
    CHECK(spot_checks == sizeof(kSpots) / sizeof(kSpots[0]));
    CHECK(volume_checks == sizeof(kVolumes) / sizeof(kVolumes[0]));
    if (!g_quick) {
        CHECK(checked == kSou1);
        CHECK(total_slots == kTotalSlots);
        CHECK(total_samples == kTotalSamples);
        CHECK(max_abs_raw == kMaxAbsRaw);
        CHECK(raw_fnv == kRawFnv);
        CHECK(pcm5_fnv == kPcm5Fnv);
        CHECK(pcm1_fnv == kPcm1Fnv);
    }
    if (decode_errors != 0) {
        std::fprintf(stderr, "FAIL: %zu SOU1 payloads failed; first: %s\n", decode_errors,
                     first_error.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    g_quick = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--full") {
            g_quick = false;
        }
    }
    test_synthetic();
    test_archive();
    if (g_failures != 0) {
        std::fprintf(stderr, "sou1: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sou1: ok\n");
    return 0;
}
