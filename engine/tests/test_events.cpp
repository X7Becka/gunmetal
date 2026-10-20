// Event queue encoding/parsing from Ghidra's event_post (0x4795b0).
//
// The record layouts are pinned byte-for-byte: type 1 is 0x1b bytes and type 3
// is 0x17 bytes, both little-endian.  The tests also cover the original
// overflow rule (check before append) and truncated/unknown records.
#include "game/events.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);     \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (0)

void test_type3() {
    gm::EventQueue queue;
    CHECK(queue.post_generate(0x12345678, 0xabcd, 1.5F, -2.25F, 3.75F, 0xdeadbeef));
    CHECK_EQ(queue.size(), gm::EventQueue::kType3Size);
    CHECK_EQ(queue.bytes()[0], 3);
    CHECK_EQ(queue.bytes()[1], 0x78);
    CHECK_EQ(queue.bytes()[2], 0x56);
    CHECK_EQ(queue.bytes()[3], 0x34);
    CHECK_EQ(queue.bytes()[4], 0x12);
    CHECK_EQ(queue.bytes()[5], 0xcd);
    CHECK_EQ(queue.bytes()[6], 0xab);

    const auto parsed = queue.drain();
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 1U);
    CHECK(queue.empty());
    if (parsed.events.size() == 1) {
        const gm::Event& event = parsed.events[0];
        CHECK(event.type == gm::EventType::generate);
        CHECK_EQ(event.id, 0x12345678);
        CHECK_EQ(event.param, 0xabcd);
        CHECK_EQ(event.a, 0);
        CHECK_EQ(event.b, 0);
        CHECK_EQ(event.x, 1);
        CHECK_EQ(event.y, -2);
        CHECK_EQ(event.z, 3);
        CHECK_EQ(event.extra, 0xdeadbeefU);
    }
}

void test_type1() {
    gm::EventQueue queue;
    CHECK(queue.post_generate_short(-1, 0x1234, -5, 7, 0.0F, 1.0F, -1.0F, 42));
    CHECK_EQ(queue.size(), gm::EventQueue::kType1Size);
    CHECK_EQ(queue.bytes()[0], 1);
    CHECK_EQ(queue.bytes()[1], 0xff);
    CHECK_EQ(queue.bytes()[2], 0xff);
    CHECK_EQ(queue.bytes()[3], 0xff);
    CHECK_EQ(queue.bytes()[4], 0xff);
    CHECK_EQ(queue.bytes()[7], 0xfb); // -5 little-endian
    CHECK_EQ(queue.bytes()[8], 0xff);
    CHECK_EQ(queue.bytes()[9], 7);
    CHECK_EQ(queue.bytes()[10], 0);

    const auto parsed = queue.drain();
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 1U);
    if (parsed.events.size() == 1) {
        const gm::Event& event = parsed.events[0];
        CHECK(event.type == gm::EventType::generate_short);
        CHECK_EQ(event.id, -1);
        CHECK_EQ(event.param, 0x1234);
        CHECK_EQ(event.a, -5);
        CHECK_EQ(event.b, 7);
        CHECK_EQ(event.x, 0.0F);
        CHECK_EQ(event.y, 1.0F);
        CHECK_EQ(event.z, -1.0F);
        CHECK_EQ(event.extra, 42U);
    }
}

void test_multiple_and_order() {
    gm::EventQueue queue;
    CHECK(queue.post_generate(10, 1, 0.0F, 0.0F, 0.0F));
    CHECK(queue.post_generate_short(20, 2, 3, 4, 5.0F, 6.0F, 7.0F));
    CHECK_EQ(queue.size(), gm::EventQueue::kType3Size + gm::EventQueue::kType1Size);
    const auto parsed = queue.drain();
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 2U);
    if (parsed.events.size() == 2) {
        CHECK_EQ(parsed.events[0].id, 10);
        CHECK(parsed.events[0].type == gm::EventType::generate);
        CHECK_EQ(parsed.events[1].id, 20);
        CHECK(parsed.events[1].type == gm::EventType::generate_short);
    }
}

void test_overflow_rule() {
    gm::EventQueue queue(10);
    CHECK(queue.post_generate(1, 0, 0.0F, 0.0F, 0.0F)); // count was 0 <= 10
    CHECK(!queue.post_generate(2, 0, 0.0F, 0.0F, 0.0F)); // count is now 23 > 10
    CHECK_EQ(queue.size(), gm::EventQueue::kType3Size);
}

void test_switch_and_text() {
    gm::EventQueue queue;
    CHECK(queue.post_switch(0x123, -7));
    CHECK_EQ(queue.size(), gm::EventQueue::kType2Size);
    CHECK_EQ(queue.bytes()[0], 2);
    CHECK_EQ(queue.bytes()[1], 0x23);
    CHECK_EQ(queue.bytes()[2], 0x01);
    CHECK_EQ(queue.bytes()[5], 0xf9); // -7
    CHECK_EQ(queue.bytes()[6], 0xff);

    CHECK(queue.post_text(3, 7, "hello"));
    // type + font + 5 chars + NUL + one gap byte.
    CHECK_EQ(queue.size(), gm::EventQueue::kType2Size + 4 + 5);
    const std::size_t text_at = gm::EventQueue::kType2Size;
    CHECK_EQ(queue.bytes()[text_at], 8); // channel 3 + 5
    CHECK_EQ(queue.bytes()[text_at + 1], 7);
    CHECK_EQ(queue.bytes()[text_at + 2], 'h');
    CHECK_EQ(queue.bytes()[text_at + 7], 0);
    CHECK_EQ(queue.bytes()[text_at + 8], 0); // gap

    const auto parsed = queue.drain();
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 2U);
    CHECK(queue.empty());
    if (parsed.events.size() == 2) {
        CHECK(parsed.events[0].type == gm::EventType::trigger_switch);
        CHECK_EQ(parsed.events[0].switch_index, 0x123U);
        CHECK_EQ(parsed.events[0].source, -7);
        CHECK(parsed.events[1].type == gm::EventType::text_channel3);
        CHECK_EQ(parsed.events[1].channel, 3);
        CHECK_EQ(parsed.events[1].font, 7);
        CHECK_EQ(parsed.events[1].text, std::string("hello"));
    }

    gm::EventQueue small(0x74de);
    CHECK(small.post_text(-1, 0, ""));
    CHECK_EQ(small.size(), 4U);
    CHECK(!small.post_text(8, 0, "x"));
    CHECK(!small.post_text(0, 0x1f, "x"));
}

void test_bad_records() {
    const std::array<std::uint8_t, 4> truncated{3, 1, 0, 0};
    const auto bad = gm::EventQueue::parse(truncated);
    CHECK(!bad.ok);
    CHECK(bad.events.empty());

    const std::array<std::uint8_t, 1> unknown{13};
    const auto unknown_result = gm::EventQueue::parse(unknown);
    CHECK(!unknown_result.ok);
    CHECK(unknown_result.events.empty());

    gm::Event event;
    event.type = static_cast<gm::EventType>(13);
    gm::EventQueue queue;
    CHECK(!queue.post(event));
    CHECK(queue.empty());

    const std::array<std::uint8_t, 4> truncated_type3{3, 1, 0, 0};
    const auto bad_drain = gm::EventQueue::parse(truncated_type3);
    CHECK(!bad_drain.ok);
}

} // namespace

int main() {
    test_type3();
    test_type1();
    test_multiple_and_order();
    test_switch_and_text();
    test_overflow_rule();
    test_bad_records();
    if (g_failures != 0) {
        std::fprintf(stderr, "events: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("events: all checks passed\n");
    return 0;
}
