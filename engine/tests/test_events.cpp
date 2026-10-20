// Event queue encoding/parsing from Ghidra's event_post (0x4795b0).
//
// The record layouts are pinned byte-for-byte: type 1 is 0x1b bytes and type 3
// is 0x17 bytes, both little-endian.  The tests also cover the original
// overflow rule (check before append) and truncated/unknown records.
#include "game/events.h"

#include "world/things.h"

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

    // __ftol truncates toward zero and writes signed i32 coordinates.
    gm::EventQueue coords;
    CHECK(coords.post_generate(1, 0, 1.9, -1.9, 0.5));
    CHECK_EQ(coords.bytes()[7], 0x01);
    CHECK_EQ(coords.bytes()[8], 0x00);
    CHECK_EQ(coords.bytes()[0x0b], 0xff);
    CHECK_EQ(coords.bytes()[0x0c], 0xff);
    const auto parsed_coords = coords.drain();
    CHECK(parsed_coords.ok);
    if (parsed_coords.events.size() == 1) {
        CHECK_EQ(parsed_coords.events[0].x, 1);
        CHECK_EQ(parsed_coords.events[0].y, -1);
        CHECK_EQ(parsed_coords.events[0].z, 0);
    }

    // Two text records: the second starts after NUL + the canonical gap byte.
    gm::EventQueue texts;
    CHECK(texts.post_text(0, 5, "HI"));
    CHECK(texts.post_text(-1, 2, ""));
    CHECK_EQ(texts.size(), 6U + 4U);
    CHECK_EQ(texts.bytes()[6], 4); // system channel
    const auto parsed_texts = texts.drain();
    CHECK(parsed_texts.ok);
    CHECK_EQ(parsed_texts.events.size(), 2U);
    if (parsed_texts.events.size() == 2) {
        CHECK_EQ(parsed_texts.events[0].text, std::string("HI"));
        CHECK_EQ(parsed_texts.events[0].channel, 0);
        CHECK_EQ(parsed_texts.events[1].channel, -1);
        CHECK_EQ(parsed_texts.events[1].text, std::string());
    }

    const std::array<std::uint8_t, 2> truncated_text{5, 1};
    CHECK(!gm::EventQueue::parse(truncated_text).ok);
    const std::array<std::uint8_t, 7> bad_switch{2, 0x00, 0x02, 0x00, 0x00, 0, 0};
    CHECK(!gm::EventQueue::parse(bad_switch).ok);
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

void test_text_registry() {
    // The registry of 0x619860: ten slots, dedup on (text, kind, font), a
    // deadline of `now + 0x111` and the swap-with-last expiry.
    gm::TextRegistry registry;
    CHECK(registry.empty());
    CHECK_EQ(registry.counter(), 0U);

    CHECK(registry.post("hello", 3, -1, 100));
    CHECK(registry.post("player 1", 4, 0, 200));
    CHECK(registry.post("player 2", 4, 1, 300));
    CHECK_EQ(registry.size(), 3U);
    CHECK_EQ(registry.counter(), 3U);
    CHECK_EQ(registry.messages()[0].text, std::string("hello"));
    CHECK_EQ(registry.messages()[0].kind, -1);
    CHECK_EQ(registry.messages()[0].font, 3);
    CHECK_EQ(registry.messages()[0].deadline, 100U);
    CHECK_EQ(registry.messages()[0].updated, 0U);
    CHECK_EQ(registry.messages()[2].updated, 2U);

    // The same text, kind and font refreshes the slot instead of adding one.
    CHECK(registry.post("hello", 3, -1, 111));
    CHECK_EQ(registry.size(), 3U);
    CHECK_EQ(registry.counter(), 4U);
    CHECK_EQ(registry.messages()[0].deadline, 111U);
    CHECK_EQ(registry.messages()[0].updated, 3U);

    // The same text with another font or another channel is a new message.
    CHECK(registry.post("hello", 5, -1, 120));
    CHECK(registry.post("hello", 3, 0, 120));
    CHECK_EQ(registry.size(), 5U);
    CHECK_EQ(registry.counter(), 6U);

    // The lifetime of the original: `now + 0x111`.
    CHECK(registry.post_lifetime("timed", 1, -1, 7));
    CHECK_EQ(registry.messages()[5].deadline, 7U + gm::TextRegistry::kLifetimeFrames);

    // The text is cut to 0x31 characters.
    const std::string long_text(60, 'x');
    CHECK(registry.post(long_text, 2, -1, 400));
    CHECK_EQ(registry.messages().back().text.size(), gm::TextRegistry::kMaxTextLength);

    // A full registry drops the new message and does not move the counter.
    gm::TextRegistry full;
    for (std::size_t i = 0; i < gm::TextRegistry::kMaxMessages; ++i) {
        CHECK(full.post("m" + std::to_string(i), 1, -1, 1000));
    }
    CHECK_EQ(full.size(), gm::TextRegistry::kMaxMessages);
    const std::uint32_t counter = full.counter();
    CHECK(!full.post("overflow", 1, -1, 1000));
    CHECK_EQ(full.size(), gm::TextRegistry::kMaxMessages);
    CHECK_EQ(full.counter(), counter);
    // ... but the same text still refreshes its slot.
    CHECK(full.post("m0", 1, -1, 2000));
    CHECK_EQ(full.messages()[0].deadline, 2000U);

    // Expiry: the freed slot is filled with the last message.
    gm::TextRegistry expiring;
    expiring.post("a", 1, -1, 10);
    expiring.post("b", 1, -1, 20);
    expiring.post("c", 1, -1, 30);
    // "a" dies: the last message moves into the freed slot, then "b" dies
    // too and the registry keeps only "c".
    expiring.expire(25);
    CHECK_EQ(expiring.size(), 1U);
    CHECK_EQ(expiring.messages()[0].text, std::string("c"));
    expiring.expire(31);
    CHECK(expiring.empty());

    // A message survives while its deadline is not in the past.
    gm::TextRegistry survivor;
    survivor.post("a", 1, -1, 10);
    survivor.expire(10);
    CHECK_EQ(survivor.size(), 1U);
    survivor.expire(11);
    CHECK(survivor.empty());

    // Selection: the system channel and the requested one, oldest update
    // first.
    gm::TextRegistry select;
    select.post("sys", 1, -1, 100);
    select.post("p0", 1, 0, 100);
    select.post("p1", 1, 1, 100);
    select.post("p0 again", 1, 0, 100);
    const auto drawn = select.select(0);
    CHECK_EQ(drawn.size(), 3U);
    if (drawn.size() == 3) {
        CHECK_EQ(drawn[0]->text, std::string("sys"));
        CHECK_EQ(drawn[1]->text, std::string("p0"));
        CHECK_EQ(drawn[2]->text, std::string("p0 again"));
    }
    CHECK_EQ(select.select(1).size(), 2U);
    CHECK_EQ(select.select(7).size(), 1U);

    // The queue feeds the registry: `event_dispatch` maps the record type to
    // the channel (type - 5).
    gm::EventQueue queue;
    CHECK(queue.post_text(-1, 2, "from the queue"));
    CHECK(queue.post_text(0, 3, "channel 0"));
    const auto parsed = queue.drain();
    CHECK(parsed.ok);
    gm::TextRegistry fed;
    for (const gm::Event& event : parsed.events) {
        CHECK(gm::post_event(fed, event, 10));
    }
    CHECK_EQ(fed.size(), 2U);
    CHECK_EQ(fed.messages()[0].kind, -1);
    CHECK_EQ(fed.messages()[0].font, 2);
    CHECK_EQ(fed.messages()[0].deadline, 10U + gm::TextRegistry::kLifetimeFrames);
    CHECK_EQ(fed.messages()[1].kind, 0);

    // Non-text events do not reach the registry.
    gm::Event generate;
    generate.type = gm::EventType::generate;
    CHECK(!gm::post_event(fed, generate, 10));
    CHECK_EQ(fed.size(), 2U);
}

void test_network_blob() {
    // The network event blob of the original: a u32 byte count followed by
    // that many event records.
    gm::EventQueue queue;
    CHECK(queue.post_generate(7, 3, 1.0, 2.0, 3.0, 0x11));
    CHECK(queue.post_text(0, 2, "net text"));
    const std::vector<std::uint8_t>& records = queue.bytes();

    std::vector<std::uint8_t> blob;
    const std::uint32_t length = static_cast<std::uint32_t>(records.size());
    blob.push_back(static_cast<std::uint8_t>(length & 0xffU));
    blob.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    blob.push_back(static_cast<std::uint8_t>((length >> 16U) & 0xffU));
    blob.push_back(static_cast<std::uint8_t>((length >> 24U) & 0xffU));
    blob.insert(blob.end(), records.begin(), records.end());

    const auto parsed = gm::EventQueue::parse_network_blob(blob);
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 2U);
    gm::EventQueue receiver;
    const auto accepted = receiver.accept_network_blob(blob);
    CHECK(accepted.ok);
    CHECK_EQ(receiver.size(), length);
    const auto drained = receiver.drain();
    CHECK_EQ(drained.events.size(), 2U);
    if (parsed.events.size() == 2) {
        CHECK(parsed.events[0].type == gm::EventType::generate);
        CHECK_EQ(parsed.events[0].id, 7);
        CHECK_EQ(parsed.events[0].extra, 0x11U);
        CHECK(parsed.events[1].type == gm::EventType::text_channel0);
        CHECK_EQ(parsed.events[1].text, std::string("net text"));
        CHECK_EQ(parsed.events[1].font, 2);
    }

    // A blob that announces more bytes than it carries is an error (the
    // original would copy past the end of the buffer).
    std::vector<std::uint8_t> truncated = blob;
    truncated[0] = static_cast<std::uint8_t>(length + 1U);
    CHECK(!gm::EventQueue::parse_network_blob(truncated).ok);
    // ... and so is a blob without the length field.
    const std::array<std::uint8_t, 3> short_blob{1, 0, 0};
    CHECK(!gm::EventQueue::parse_network_blob(short_blob).ok);
    // An empty blob is valid and carries no events.
    const std::array<std::uint8_t, 4> empty{0, 0, 0, 0};
    const auto none = gm::EventQueue::parse_network_blob(empty);
    CHECK(none.ok);
    CHECK(none.events.empty());
}


void test_dispatch_switch() {
    gm::EventDispatcher dispatcher;
    gm::TextRegistry registry;
    gm::EventQueue queue;

    gm::SwitchEffect effect;
    effect.kind = 0x100;
    effect.event_id = 900;
    effect.x = 1;
    effect.y = 2;
    effect.z = 3;
    effect.sound_id = 700;
    effect.sound_index = 5;
    CHECK(dispatcher.set_switch(3, effect));
    CHECK(queue.post_switch(3, 7));
    const gm::EventDispatchResult direct = dispatcher.dispatch(queue, 10, registry);
    CHECK(direct.ok);
    CHECK_EQ(direct.generated.size(), 1U);
    CHECK_EQ(direct.sounds.size(), 1U);
    CHECK_EQ(direct.text_posted, 0U);
    if (direct.generated.size() == 1) {
        CHECK_EQ(direct.generated[0].id, 900);
        CHECK_EQ(direct.generated[0].x, 1);
        CHECK_EQ(direct.generated[0].y, 2);
        CHECK_EQ(direct.generated[0].z, 3);
        CHECK_EQ(direct.generated[0].extra,
                 static_cast<std::uint32_t>(7 - 0x10000)); // source - 0x10000
    }
    if (direct.sounds.size() == 1) {
        CHECK_EQ(direct.sounds[0].index, 5);
        CHECK_EQ(direct.sounds[0].sound_id, 700U);
        CHECK_EQ(direct.sounds[0].rate, 0x2b11U);
        CHECK_EQ(direct.sounds[0].volume, 1000);
    }

    // kind >= 0x101 uses the sector centroid, not the record xyz.
    gm::SwitchEffect centroid = effect;
    centroid.kind = 0x101;
    centroid.has_centroid = true;
    centroid.centroid = {10.5, 20.5, 30.5};
    CHECK(dispatcher.set_switch(4, centroid));
    CHECK(queue.post_switch(4, 0));
    const gm::EventDispatchResult centred = dispatcher.dispatch(queue, 10, registry);
    CHECK_EQ(centred.generated.size(), 1U);
    if (centred.generated.size() == 1) {
        CHECK_EQ(centred.generated[0].x, 10);
        CHECK_EQ(centred.generated[0].y, 20);
        CHECK_EQ(centred.generated[0].z, 30);
    }

    // Inactive switch: event_id 0 does nothing.  Unknown index is skipped.
    gm::SwitchEffect inactive = effect;
    inactive.event_id = 0;
    CHECK(dispatcher.set_switch(5, inactive));
    CHECK(queue.post_switch(5, 0));
    CHECK(queue.post_switch(99, 0));
    const gm::EventDispatchResult off = dispatcher.dispatch(queue, 10, registry);
    CHECK(off.ok);
    CHECK(off.generated.empty());
    CHECK(off.sounds.empty());
    CHECK(!dispatcher.set_switch(gm::EventDispatcher::kMaxSwitches, effect));

    // Text records go to the message registry; type 1/3 events pass through.
    CHECK(queue.post_text(0, 2, "dispatch"));
    CHECK(queue.post_generate(42, 7, 1.0, 2.0, 3.0));
    const gm::EventDispatchResult text = dispatcher.dispatch(queue, 10, registry);
    CHECK_EQ(text.text_posted, 1U);
    CHECK_EQ(text.generated.size(), 1U);
    CHECK_EQ(registry.size(), 1U);
    if (registry.size() == 1) {
        CHECK_EQ(registry.messages()[0].text, std::string("dispatch"));
        CHECK_EQ(registry.messages()[0].kind, 0);
    }

    // A malformed queue drops everything as a whole.
    gm::EventParseResult bad;
    bad.ok = false;
    const gm::EventDispatchResult dropped = dispatcher.dispatch(bad, 10, registry);
    CHECK(!dropped.ok);
    CHECK(dropped.generated.empty());
}


void test_thing_events() {
    gm::Thing thing;
    thing.id = 5;
    thing.position = std::array<int, 3>{10, 20, 30};
    thing.create_event = 1000;
    thing.damaged_event = 1001;
    thing.collide_event = 1002;
    thing.expire_event = 1003;
    thing.timeout_event = 1004;
    thing.destroy_event = 1005;

    CHECK_EQ(gm::thing_event_id(thing, gm::ThingEventKind::create), 1000U);
    CHECK_EQ(gm::thing_event_id(thing, gm::ThingEventKind::destroy), 1005U);

    gm::EventQueue queue;
    CHECK(gm::post_thing_event(queue, thing, gm::ThingEventKind::create));
    const gm::EventParseResult parsed = queue.drain();
    CHECK(parsed.ok);
    CHECK_EQ(parsed.events.size(), 1U);
    if (parsed.events.size() == 1) {
        CHECK_EQ(parsed.events[0].id, 1000);
        CHECK_EQ(parsed.events[0].x, 10);
        CHECK_EQ(parsed.events[0].y, 20);
        CHECK_EQ(parsed.events[0].z, 30);
        CHECK_EQ(parsed.events[0].extra,
                 static_cast<std::uint32_t>(5 - 0xfff8));
    }

    // A missing event id is a no-op; source-relative things use zero coords.
    thing.create_event = 0;
    CHECK(!gm::post_thing_event(queue, thing, gm::ThingEventKind::create));
    thing.create_event = 1000;
    thing.position.reset();
    CHECK(gm::post_thing_event(queue, thing, gm::ThingEventKind::create));
    const gm::EventParseResult relative = queue.drain();
    CHECK_EQ(relative.events.size(), 1U);
    if (relative.events.size() == 1) {
        CHECK_EQ(relative.events[0].x, 0);
        CHECK_EQ(relative.events[0].y, 0);
        CHECK_EQ(relative.events[0].z, 0);
    }
}

} // namespace

int main() {
    test_type3();
    test_type1();
    test_multiple_and_order();
    test_switch_and_text();
    test_overflow_rule();
    test_bad_records();
    test_text_registry();
    test_network_blob();
    test_dispatch_switch();
    test_thing_events();
    if (g_failures != 0) {
        std::fprintf(stderr, "events: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("events: all checks passed\n");
    return 0;
}
