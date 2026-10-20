#include "res/sou1.h"

#include "core/bytes.h"

#include <algorithm>
#include <array>
#include <bit>

namespace gm {
namespace {

// Engine tables (gundx.exe's ``adpcmseg``, docs/SOU1.md §2-3).
constexpr std::uint32_t kDeltaBaseVa = 0x850540;
constexpr std::uint32_t kDeltaEndVa = 0x8522c0; // exclusive; then the SOU2 step table
constexpr int kMixerRate = 32000; // slot rate; the source rate is 8/16/32 kHz

constexpr std::size_t kCurveWords = (kDeltaEndVa - kDeltaBaseVa) / 4;

// 0x850500: signed level step per nibble code.  Codes 0..3 / 8..11 step the
// adaptation level down, codes 4..7 / 12..15 step it up.
constexpr std::array<std::int32_t, 16> kStep = {{-64, -64, -64, -64, 128, 256, 384, 512,
                                                 -64, -64, -64, -64, 128, 256, 384, 512}};

// 0x850480: start of the delta curve for each of the 32 volume rows.  The
// index is the game's per-channel volume minus one (0..31, 31 = loudest).
constexpr std::array<std::uint32_t, 32> kVolumePointer = {{
    0x850540, 0x850640, 0x850700, 0x8507c0, 0x850840, 0x8508c0, 0x850900, 0x850940,
    0x850980, 0x8509c0, 0x850a00, 0x850a00, 0x850a40, 0x850a40, 0x850a80, 0x850a80,
    0x850ac0, 0x850ac0, 0x850b00, 0x850b00, 0x850b40, 0x850b40, 0x850b80, 0x850b80,
    0x850bc0, 0x850bc0, 0x850c00, 0x850c00, 0x850c40, 0x850c40, 0x850c80, 0x850c80,
}};

// 0x850540..0x8522c0: *one* shared signed dword delta curve (118 rows of 16).
// ``level`` is a byte offset into it, so adaptation may walk past the row
// selected by the volume pointer.  There is no clamp in the original code; the
// shipped corpus peaks at level 0x1600 (ADON), 0x13 bytes below the end.
constexpr std::array<std::int32_t, kCurveWords> kCurve = {{
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 0, 0, 32, 32, 32, 32,
    0, 0, 0, 0, -32, -32, -32, -32,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 0, 32, 32, 64, 64, 96, 96,
    0, 0, -32, -32, -64, -64, -96, -96,
    0, 32, 32, 64, 96, 128, 128, 160,
    0, -32, -32, -64, -96, -128, -128, -160,
    0, 32, 32, 64, 96, 128, 128, 160,
    0, -32, -32, -64, -96, -128, -128, -160,
    0, 32, 32, 64, 96, 128, 128, 160,
    0, -32, -32, -64, -96, -128, -128, -160,
    0, 32, 32, 64, 96, 128, 128, 160,
    0, -32, -32, -64, -96, -128, -128, -160,
    0, 32, 64, 96, 128, 160, 192, 224,
    0, -32, -64, -96, -128, -160, -192, -224,
    0, 32, 64, 96, 128, 160, 192, 224,
    0, -32, -64, -96, -128, -160, -192, -224,
    0, 32, 64, 96, 128, 160, 192, 224,
    0, -32, -64, -96, -128, -160, -192, -224,
    0, 32, 96, 128, 160, 192, 256, 288,
    0, -32, -96, -128, -160, -192, -256, -288,
    0, 32, 96, 128, 160, 192, 256, 288,
    0, -32, -96, -128, -160, -192, -256, -288,
    0, 64, 96, 160, 192, 256, 288, 352,
    0, -64, -96, -160, -192, -256, -288, -352,
    0, 64, 96, 160, 192, 256, 288, 352,
    0, -64, -96, -160, -192, -256, -288, -352,
    0, 64, 128, 192, 224, 288, 352, 416,
    0, -64, -128, -192, -224, -288, -352, -416,
    0, 64, 128, 192, 224, 288, 352, 416,
    0, -64, -128, -192, -224, -288, -352, -416,
    32, 96, 160, 224, 288, 352, 416, 480,
    -32, -96, -160, -224, -288, -352, -416, -480,
    32, 96, 160, 224, 320, 384, 448, 512,
    -32, -96, -160, -224, -320, -384, -448, -512,
    32, 96, 192, 256, 352, 416, 512, 576,
    -32, -96, -192, -256, -352, -416, -512, -576,
    32, 128, 192, 288, 384, 480, 544, 640,
    -32, -128, -192, -288, -384, -480, -544, -640,
    32, 128, 224, 320, 416, 512, 608, 704,
    -32, -128, -224, -320, -416, -512, -608, -704,
    32, 128, 256, 352, 448, 544, 672, 768,
    -32, -128, -256, -352, -448, -544, -672, -768,
    32, 160, 256, 384, 480, 608, 704, 832,
    -32, -160, -256, -384, -480, -608, -704, -832,
    64, 192, 320, 448, 576, 704, 832, 960,
    -64, -192, -320, -448, -576, -704, -832, -960,
    64, 192, 320, 448, 608, 736, 864, 992,
    -64, -192, -320, -448, -608, -736, -864, -992,
    64, 224, 352, 512, 672, 832, 960, 1120,
    -64, -224, -352, -512, -672, -832, -960, -1120,
    64, 224, 416, 576, 736, 896, 1088, 1248,
    -64, -224, -416, -576, -736, -896, -1088, -1248,
    64, 256, 448, 640, 800, 992, 1184, 1376,
    -64, -256, -448, -640, -800, -992, -1184, -1376,
    96, 288, 480, 672, 896, 1088, 1280, 1472,
    -96, -288, -480, -672, -896, -1088, -1280, -1472,
    96, 320, 544, 768, 992, 1216, 1440, 1664,
    -96, -320, -544, -768, -992, -1216, -1440, -1664,
    96, 352, 608, 864, 1088, 1344, 1600, 1856,
    -96, -352, -608, -864, -1088, -1344, -1600, -1856,
    128, 384, 672, 928, 1216, 1472, 1760, 2016,
    -128, -384, -672, -928, -1216, -1472, -1760, -2016,
    128, 416, 736, 1024, 1312, 1600, 1920, 2208,
    -128, -416, -736, -1024, -1312, -1600, -1920, -2208,
    160, 480, 800, 1120, 1472, 1792, 2112, 2432,
    -160, -480, -800, -1120, -1472, -1792, -2112, -2432,
    160, 512, 896, 1248, 1600, 1952, 2336, 2688,
    -160, -512, -896, -1248, -1600, -1952, -2336, -2688,
    192, 576, 992, 1376, 1792, 2176, 2592, 2976,
    -192, -576, -992, -1376, -1792, -2176, -2592, -2976,
    192, 640, 1088, 1536, 1952, 2400, 2848, 3296,
    -192, -640, -1088, -1536, -1952, -2400, -2848, -3296,
    224, 704, 1184, 1664, 2144, 2624, 3104, 3584,
    -224, -704, -1184, -1664, -2144, -2624, -3104, -3584,
    256, 768, 1312, 1824, 2368, 2880, 3424, 3936,
    -256, -768, -1312, -1824, -2368, -2880, -3424, -3936,
    288, 864, 1440, 2016, 2624, 3200, 3776, 4352,
    -288, -864, -1440, -2016, -2624, -3200, -3776, -4352,
    320, 960, 1600, 2240, 2880, 3520, 4160, 4800,
    -320, -960, -1600, -2240, -2880, -3520, -4160, -4800,
    352, 1056, 1760, 2464, 3168, 3872, 4576, 5280,
    -352, -1056, -1760, -2464, -3168, -3872, -4576, -5280,
    384, 1152, 1920, 2688, 3488, 4256, 5024, 5792,
    -384, -1152, -1920, -2688, -3488, -4256, -5024, -5792,
    416, 1280, 2112, 2976, 3840, 4704, 5536, 6400,
    -416, -1280, -2112, -2976, -3840, -4704, -5536, -6400,
    448, 1408, 2336, 3296, 4224, 5184, 6112, 7072,
    -448, -1408, -2336, -3296, -4224, -5184, -6112, -7072,
    512, 1536, 2592, 3616, 4672, 5696, 6752, 7776,
    -512, -1536, -2592, -3616, -4672, -5696, -6752, -7776,
    544, 1696, 2848, 4000, 5120, 6272, 7424, 8576,
    -544, -1696, -2848, -4000, -5120, -6272, -7424, -8576,
    608, 1856, 3136, 4384, 5632, 6880, 8160, 9408,
    -608, -1856, -3136, -4384, -5632, -6880, -8160, -9408,
    672, 2048, 3456, 4832, 6208, 7584, 8992, 10368,
    -672, -2048, -3456, -4832, -6208, -7584, -8992, -10368,
    736, 2272, 3776, 5312, 6816, 8352, 9856, 11392,
    -736, -2272, -3776, -5312, -6816, -8352, -9856, -11392,
    832, 2496, 4160, 5824, 7520, 9184, 10848, 12512,
    -832, -2496, -4160, -5824, -7520, -9184, -10848, -12512,
    896, 2752, 4576, 6432, 8256, 10112, 11936, 13792,
    -896, -2752, -4576, -6432, -8256, -10112, -11936, -13792,
    992, 3008, 5056, 7072, 9088, 11104, 13152, 15168,
    -992, -3008, -5056, -7072, -9088, -11104, -13152, -15168,
    1088, 3328, 5568, 7808, 10016, 12256, 14496, 16736,
    -1088, -3328, -5568, -7808, -10016, -12256, -14496, -16736,
    1216, 3680, 6112, 8576, 11040, 13504, 15936, 18400,
    -1216, -3680, -6112, -8576, -11040, -13504, -15936, -18400,
    1344, 4032, 6720, 9408, 12128, 14816, 17504, 20192,
    -1344, -4032, -6720, -9408, -12128, -14816, -17504, -20192,
    1472, 4448, 7392, 10368, 13344, 16320, 19264, 22240,
    -1472, -4448, -7392, -10368, -13344, -16320, -19264, -22240,
    1632, 4896, 8160, 11424, 14688, 17952, 21216, 24480,
    -1632, -4896, -8160, -11424, -14688, -17952, -21216, -24480,
    1792, 5376, 8960, 12544, 16160, 19744, 23328, 26912,
    -1792, -5376, -8960, -12544, -16160, -19744, -23328, -26912,
    1952, 5920, 9856, 13824, 17760, 21728, 25664, 29632,
    -1952, -5920, -9856, -13824, -17760, -21728, -25664, -29632,
    2176, 6528, 10880, 15232, 19584, 23936, 28288, 32640,
    -2176, -6528, -10880, -15232, -19584, -23936, -28288, -32640,
    2368, 7168, 11936, 16736, 21504, 26304, 31072, 35872,
    -2368, -7168, -11936, -16736, -21504, -26304, -31072, -35872,
    2624, 7872, 13152, 18400, 23680, 28928, 34208, 39456,
    -2624, -7872, -13152, -18400, -23680, -28928, -34208, -39456,
    2880, 8672, 14464, 20256, 26048, 31840, 37632, 43424,
    -2880, -8672, -14464, -20256, -26048, -31840, -37632, -43424,
    3168, 9536, 15904, 22272, 28640, 35008, 41376, 47744,
    -3168, -9536, -15904, -22272, -28640, -35008, -41376, -47744,
    3488, 10496, 17504, 24512, 31520, 38528, 45536, 52544,
    -3488, -10496, -17504, -24512, -31520, -38528, -45536, -52544,
    3840, 11552, 19232, 26944, 34656, 42368, 50048, 57760,
    -3840, -11552, -19232, -26944, -34656, -42368, -50048, -57760,
    4224, 12704, 21184, 29664, 38144, 46624, 55104, 63584,
    -4224, -12704, -21184, -29664, -38144, -46624, -55104, -63584,
    4640, 13984, 23296, 32640, 41952, 51296, 60608, 69952,
    -4640, -13984, -23296, -32640, -41952, -51296, -60608, -69952,
    5120, 15360, 25632, 35872, 46144, 56384, 66656, 76896,
    -5120, -15360, -25632, -35872, -46144, -56384, -66656, -76896,
    5632, 16928, 28192, 39488, 50784, 62080, 73344, 84640,
    -5632, -16928, -28192, -39488, -50784, -62080, -73344, -84640,
    6208, 18624, 31040, 43456, 55872, 68288, 80704, 93120,
    -6208, -18624, -31040, -43456, -55872, -68288, -80704, -93120,
    6816, 20480, 34112, 47776, 61440, 75104, 88736, 102400,
    -6816, -20480, -34112, -47776, -61440, -75104, -88736, -102400,
    7488, 22528, 37536, 52576, 67584, 82624, 97632, 112672,
    -7488, -22528, -37536, -52576, -67584, -82624, -97632, -112672,
    8256, 24768, 41312, 57824, 74368, 90880, 107424, 123936,
    -8256, -24768, -41312, -57824, -74368, -90880, -107424, -123936,
    9088, 27264, 45440, 63616, 81792, 99968, 118144, 136320,
    -9088, -27264, -45440, -63616, -81792, -99968, -118144, -136320,
    9984, 29984, 49952, 69952, 89952, 109952, 129920, 149920,
    -9984, -29984, -49952, -69952, -89952, -109952, -129920, -149920,
    10976, 32960, 54976, 76960, 98944, 120928, 142944, 164928,
    -10976, -32960, -54976, -76960, -98944, -120928, -142944, -164928,
    12096, 36288, 60480, 84672, 108864, 133056, 157248, 181440,
    -12096, -36288, -60480, -84672, -108864, -133056, -157248, -181440,
    13280, 39904, 66528, 93152, 119744, 146368, 172992, 199616,
    -13280, -39904, -66528, -93152, -119744, -146368, -172992, -199616,
    14624, 43904, 73184, 102464, 131744, 161024, 190304, 219584,
    -14624, -43904, -73184, -102464, -131744, -161024, -190304, -219584,
    16096, 48288, 80512, 112704, 144928, 177120, 209344, 241536,
    -16096, -48288, -80512, -112704, -144928, -177120, -209344, -241536,
    17696, 53120, 88544, 123968, 159392, 194816, 230240, 265664,
    -17696, -53120, -88544, -123968, -159392, -194816, -230240, -265664,
    19456, 58432, 97408, 136384, 175328, 214304, 253280, 292256,
    -19456, -58432, -97408, -136384, -175328, -214304, -253280, -292256,
    21408, 64288, 107136, 150016, 192864, 235744, 278592, 321472,
    -21408, -64288, -107136, -150016, -192864, -235744, -278592, -321472,
    23552, 70720, 117856, 165024, 212160, 259328, 306464, 353632,
    -23552, -70720, -117856, -165024, -212160, -259328, -306464, -353632,
    25920, 77792, 129664, 181536, 233408, 285280, 337152, 389024,
    -25920, -77792, -129664, -181536, -233408, -285280, -337152, -389024,
    28512, 85568, 142624, 199680, 256736, 313792, 370848, 427904,
    -28512, -85568, -142624, -199680, -256736, -313792, -370848, -427904,
    31360, 94112, 156896, 219648, 282400, 345152, 407936, 470688,
    -31360, -94112, -156896, -219648, -282400, -345152, -407936, -470688,
    34496, 103552, 172576, 241632, 310656, 379712, 448736, 517792,
    -34496, -103552, -172576, -241632, -310656, -379712, -448736, -517792,
    37952, 113888, 189856, 265792, 341728, 417664, 493632, 569568,
    -37952, -113888, -189856, -265792, -341728, -417664, -493632, -569568,
    41760, 125280, 208832, 292352, 375904, 459424, 542976, 626496,
    -41760, -125280, -208832, -292352, -375904, -459424, -542976, -626496,
    45920, 137824, 229728, 321632, 413504, 505408, 597312, 689216,
    -45920, -137824, -229728, -321632, -413504, -505408, -597312, -689216,
    50528, 151616, 252672, 353760, 454848, 555936, 656992, 758080,
    -50528, -151616, -252672, -353760, -454848, -555936, -656992, -758080,
    55584, 166784, 277952, 389152, 500352, 611552, 722720, 833920,
    -55584, -166784, -277952, -389152, -500352, -611552, -722720, -833920,
    61152, 183456, 305760, 428064, 550400, 672704, 795008, 917312,
    -61152, -183456, -305760, -428064, -550400, -672704, -795008, -917312,
    67264, 201792, 336352, 470880, 605440, 739968, 874528, 1009056,
    -67264, -201792, -336352, -470880, -605440, -739968, -874528, -1009056,
    73984, 221984, 369984, 517984, 665984, 813984, 961984, 1109984,
    -73984, -221984, -369984, -517984, -665984, -813984, -961984, -1109984,
    81376, 244192, 406976, 569792, 732576, 895392, 1058176, 1220992,
    -81376, -244192, -406976, -569792, -732576, -895392, -1058176, -1220992,
    89536, 268608, 447680, 626752, 805856, 984928, 1164000, 1343072,
    -89536, -268608, -447680, -626752, -805856, -984928, -1164000, -1343072,
    98464, 295456, 492448, 689440, 886400, 1083392, 1280384, 1477376,
    -98464, -295456, -492448, -689440, -886400, -1083392, -1280384, -1477376,
    108320, 325024, 541696, 758400, 975072, 1191776, 1408448, 1625152,
    -108320, -325024, -541696, -758400, -975072, -1191776, -1408448, -1625152,
    119168, 357504, 595872, 834208, 1072576, 1310912, 1549280, 1787616,
    -119168, -357504, -595872, -834208, -1072576, -1310912, -1549280, -1787616,
    131040, 393184, 655328, 917472, 1179584, 1441728, 1703872, 1966016,
    -131040, -393184, -655328, -917472, -1179584, -1441728, -1703872, -1966016,
}};

int slots_per_byte(std::uint32_t rate) {
    if (rate < 10000) {
        return 8; // four copies per code, 0x852cc0
    }
    if (rate < 18000) {
        return 4; // two copies per code, 0x852c00
    }
    return 2;     // one copy per code, 0x852b40
}

/// One channel of the ADPCM stream.  ``out`` holds ``slots * channels``
/// interleaved samples; this channel starts at ``channel`` and advances by
/// ``channels``.  ``acc``/``level`` are 32-bit and wrap like the x86 code.
bool decode_channel(std::span<const std::uint8_t> payload, int copies,
                    std::uint32_t base_word, int channels, int channel,
                    std::span<std::int32_t> out) {
    if (base_word >= kCurve.size() || kCurve.size() - base_word < 16) {
        return false;
    }
    // Last byte offset whose 16-code row still fits in the curve.
    const std::size_t max_level = (kCurve.size() - base_word - 16) * 4;
    std::uint32_t acc = 0;
    std::uint32_t level = 0;
    std::size_t o = static_cast<std::size_t>(channel);
    const std::size_t o_step = static_cast<std::size_t>(channels);
    for (const std::uint8_t byte : payload) {
        const std::array<unsigned, 2> codes = {
            {static_cast<unsigned>(byte >> 4), static_cast<unsigned>(byte & 0x0Fu)}};
        for (const unsigned code : codes) {
            if (level > max_level) {
                return false; // malformed stream: adaptation left the curve
            }
            const std::size_t word = base_word + (level >> 2) + code;
            if (word >= kCurve.size()) {
                return false;
            }
            acc += std::bit_cast<std::uint32_t>(kCurve[word]);
            const std::int32_t value = std::bit_cast<std::int32_t>(acc);
            for (int c = 0; c < copies; ++c) {
                out[o] = value;
                o += o_step;
            }
            level += static_cast<std::uint32_t>(kStep[code]);
        }
    }
    return true;
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v));
    put16(out, static_cast<std::uint16_t>(v >> 16));
}

std::int16_t clip16(std::int32_t v) {
    return static_cast<std::int16_t>(std::clamp(v, -32768, 32767));
}

} // namespace

std::optional<Sou1Info> parse_sou1_header(std::span<const std::uint8_t> blob,
                                          std::string& error) {
    if (blob.size() < 4) {
        error = "blob too short for .sou";
        return std::nullopt;
    }
    Sou1Info info;
    info.magic.assign(reinterpret_cast<const char*>(blob.data()), 4);
    if (info.magic != "SOU0" && info.magic != "SOU1") {
        error = "not a SOU0/SOU1 blob (magic '" + info.magic + "')";
        return std::nullopt;
    }
    if (blob.size() < 16) {
        error = "truncated SOU0/SOU1 header";
        return std::nullopt;
    }
    info.sample_rate = rd32(blob.data() + 4);
    info.flags = rd32(blob.data() + 8);
    info.declared_length = rd32(blob.data() + 12);
    if (info.flags > 1) {
        error = "implausible SOU flag word " + std::to_string(info.flags);
        return std::nullopt;
    }
    // The declared length is exact for mono entries and one greater than the
    // payload for the stereo ones, and the loader masks the low bit.
    const std::size_t available = blob.size() - 16;
    std::size_t payload = std::min<std::size_t>(info.declared_length, available);
    payload &= ~std::size_t{1};
    info.payload_bytes = payload;
    info.channels = 1 + static_cast<int>(info.flags & 1u);
    info.slots_per_byte = slots_per_byte(info.sample_rate);
    return info;
}

std::optional<RawClip> decode_sou1_raw(std::span<const std::uint8_t> blob,
                                       std::string& error, int volume) {
    const auto info = parse_sou1_header(blob, error);
    if (!info) {
        return std::nullopt;
    }
    if (info->magic != "SOU1") {
        error = "SOU0 payload is raw 8-bit PCM, not SOU1 ADPCM";
        return std::nullopt;
    }
    if (volume < 0 || volume > 31) {
        error = "SOU1 volume table index must be 0..31";
        return std::nullopt;
    }
    const std::uint32_t pointer = kVolumePointer[static_cast<std::size_t>(volume)];
    if (pointer < kDeltaBaseVa || pointer >= kDeltaEndVa) {
        error = "SOU1 volume table pointer outside the delta curve";
        return std::nullopt;
    }
    const std::uint32_t base_word = (pointer - kDeltaBaseVa) / 4;
    const int copies = info->slots_per_byte / 2;
    const auto payload = blob.subspan(16, info->payload_bytes);

    RawClip clip;
    clip.sample_rate = kMixerRate; // slots, not the 8/16/32 kHz source rate
    clip.channels = info->channels;
    // Stereo payloads carry one half-size stream per channel (length/2 each,
    // loader 0x40b0e8); each channel decodes independently.
    const std::size_t stream_bytes =
        payload.size() / static_cast<std::size_t>(info->channels);
    const std::size_t slots =
        stream_bytes * static_cast<std::size_t>(info->slots_per_byte);
    clip.samples.resize(slots * static_cast<std::size_t>(info->channels));

    const bool ok = [&] {
        if (info->channels != 2) {
            return decode_channel(payload, copies, base_word, 1, 0, clip.samples);
        }
        // Stereo payloads are nibble-interleaved by the loader (0x40b2d8):
        // two bytes A B become one byte of each channel.
        const std::size_t half = payload.size() / 2;
        std::vector<std::uint8_t> left(half);
        std::vector<std::uint8_t> right(half);
        for (std::size_t i = 0; i < half; ++i) {
            const std::uint8_t a = payload[2 * i];
            const std::uint8_t b = payload[2 * i + 1];
            left[i] = static_cast<std::uint8_t>((a & 0xF0u) | (b >> 4));
            right[i] = static_cast<std::uint8_t>(
                ((static_cast<unsigned>(a) << 4) & 0xF0u) | (b & 0x0Fu));
        }
        return decode_channel(left, copies, base_word, 2, 0, clip.samples) &&
               decode_channel(right, copies, base_word, 2, 1, clip.samples);
    }();
    if (!ok) {
        error = "SOU1 adaptation level leaves the delta curve";
        return std::nullopt;
    }
    return clip;
}

std::optional<PcmClip> decode_sou1(std::span<const std::uint8_t> blob, std::string& error,
                                   int volume, int shift) {
    if (shift < 0 || shift > 31) {
        error = "SOU1 PCM shift must be 0..31";
        return std::nullopt;
    }
    const auto raw = decode_sou1_raw(blob, error, volume);
    if (!raw) {
        return std::nullopt;
    }
    PcmClip clip;
    clip.sample_rate = raw->sample_rate;
    clip.channels = raw->channels;
    clip.samples.resize(raw->samples.size());
    for (std::size_t i = 0; i < raw->samples.size(); ++i) {
        clip.samples[i] = clip16(raw->samples[i] >> shift);
    }
    return clip;
}

std::optional<std::vector<std::uint8_t>> sou1_to_wav(std::span<const std::uint8_t> blob,
                                                     std::string& error, int volume,
                                                     int shift) {
    if (shift < 0 || shift > 31) {
        error = "SOU1 PCM shift must be 0..31";
        return std::nullopt;
    }
    const auto info = parse_sou1_header(blob, error);
    if (!info) {
        return std::nullopt;
    }
    const auto raw = decode_sou1_raw(blob, error, volume);
    if (!raw) {
        return std::nullopt;
    }
    const int copies = info->slots_per_byte / 2;
    const std::size_t channels = static_cast<std::size_t>(raw->channels);
    const std::size_t slots_per_channel = raw->samples.size() / channels;
    const std::size_t frames = slots_per_channel / static_cast<std::size_t>(copies);
    const std::uint16_t out_channels = static_cast<std::uint16_t>(channels);
    const std::uint32_t data_bytes =
        static_cast<std::uint32_t>(frames * channels * sizeof(std::int16_t));
    const std::uint32_t rate = info->sample_rate;
    const std::uint16_t block = static_cast<std::uint16_t>(channels * 2);
    std::vector<std::uint8_t> out;
    out.reserve(44 + data_bytes);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    put32(out, 36 + data_bytes);
    out.insert(out.end(), {'W', 'A', 'V', 'E'});
    out.insert(out.end(), {'f', 'm', 't', ' '});
    put32(out, 16);
    put16(out, 1); // PCM
    put16(out, out_channels);
    put32(out, rate);
    put32(out, rate * block);
    put16(out, block);
    put16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    put32(out, data_bytes);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t base = i * static_cast<std::size_t>(copies) * channels;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const std::int32_t v = raw->samples[base + ch] >> shift;
            put16(out, static_cast<std::uint16_t>(clip16(v)));
        }
    }
    return out;
}

} // namespace gm
