#include "core/bytes.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/time.h"
#include "platform/window.h"
#include "platform/audio.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/png.h"
#include "res/sbm.h"
#include "res/sbw.h"
#include "res/sou.h"
#include "res/sound_catalog.h"
#include "res/sof.h"
#include "res/sou1.h"
#include "res/sou2.h"
#include "res/sbt.h"
#include "res/scene.h"
#include "res/sbc.h"
#include "res/sob.h"
#include "res/text.h"
#include "res/level.h"
#include "res/sbv.h"
#include "res/sbv_video.h"
#include "res/anim.h"
#include "res/save.h"
#include "res/soe.h"
#include "res/sml.h"
#include "camera/camera.h"
#include "game/runtime.h"
#include "audio/music.h"
#include "audio/sample_cache.h"
#include "ui/frontend_menu.h"
#include "ui/save_slots.h"
#include "ui/sml_pager.h"
#include "game/sim_player.h"
#include "game/weapons.h"
#include "res/vm_exec.h"
#include "world/doors.h"
#include "player/controller.h"
#include "render/collide.h"
#include "render/hud_player.h"
#include "render/hud_compose.h"
#include "render/render.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open: " + path.string();
        return std::nullopt;
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::vector<std::uint8_t> bmp_to_rgba(const gm::BmpImage& bmp) {
    std::vector<std::uint8_t> rgba(bmp.pixels.size() * 4, 0);
    const std::size_t colors = bmp.palette.size() / 4;
    for (std::size_t i = 0; i < bmp.pixels.size(); ++i) {
        const std::size_t index = bmp.pixels[i];
        if (index < colors) {
            rgba[i * 4 + 0] = bmp.palette[index * 4 + 0];
            rgba[i * 4 + 1] = bmp.palette[index * 4 + 1];
            rgba[i * 4 + 2] = bmp.palette[index * 4 + 2];
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}

int export_png(const gm::Options& options, int width, int height,
               const std::vector<std::uint8_t>& rgba) {
    const auto png = gm::encode_png_rgba(width, height, rgba);
    if (png.empty() || !write_file(options.png, png)) {
        gm::logf(gm::LogLevel::Error, "png", "cannot write ", options.png.string());
        return 2;
    }
    std::printf("png: %s (%zu bytes)\n", options.png.string().c_str(), png.size());
    return 0;
}

bool apply_save_edits(const gm::Options& options, gm::SavePlayer& player,
                      std::string& error);

/// `--sample NAME`: the engine's own lookup (`res/sound_catalog.*`, `0x40ade0`):
/// basename, 8-byte uppercase key, `sounds/SOU.IDX`, then `MUSIC/SOU.IDX`.
/// A miss is not an error — the original returns -1 and plays silence.
int show_sample(const gm::Options& options) {
    std::string error;
    const auto catalog = gm::SoundCatalog::open(options.game, error);
    if (!catalog) {
        gm::logf(gm::LogLevel::Error, "sample", error);
        return 2;
    }
    const auto hit = catalog->find(options.sample);
    if (!hit) {
        std::printf("sample %s: not found (the original plays silence)\n",
                    options.sample.c_str());
        return 0;
    }
    std::printf("sample %s: archive=%s name=%s offset=%u size=%u\n",
                options.sample.c_str(), hit->archive.c_str(), hit->name.c_str(),
                hit->offset, hit->size);
    return 0;
}

/// `--sample-play NAME` (M7.4, slice 2): the runtime path, not just a lookup.
///
/// Resolves `NAME` in the level's `SOUND_SAMPLE_TABLE` (record 12 of
/// `<level>.sbt`, `--level`, default `level0`), takes it through `SampleCache`
/// (decode SOU0/SOU1/SOU2 + refcount + LRU) and plays the decoded PCM once
/// through `AudioDevice`.  `--headless` never opens the device: it prints the
/// cache's numbers instead.  `--twice` acquires the same name a second time —
/// that acquire must be a cache hit (`hits` grows, `misses` does not).
///
/// Only `<level>.sbt`, `sounds/SOU.IDX` and the archive it points at are read:
/// no window, no `.sbw`/`.sbc`, no mixer.
int play_sample(const gm::Options& options) {
    const std::string level_name = options.level.empty() ? "level0" : options.level;
    std::string error;
    const auto path = gm::resolve_ci(options.game, level_name + ".sbt");
    if (!path) {
        gm::logf(gm::LogLevel::Error, "sample-play",
                 "level '" + level_name + "': " + level_name + ".sbt not found under " +
                     options.game.string());
        return 2;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        gm::logf(gm::LogLevel::Error, "sample-play", error);
        return 2;
    }
    const auto level = gm::parse_sbt(*bytes, error);
    if (!level) {
        gm::logf(gm::LogLevel::Error, "sample-play", path->filename().string() + ": " + error);
        return 2;
    }
    if (!level->sound_samples) {
        gm::logf(gm::LogLevel::Error, "sample-play",
                 path->filename().string() + " has no SOUND_SAMPLE_TABLE (record 12)");
        return 2;
    }

    auto cache = gm::audio::SampleCache::open(options.game, *level->sound_samples, error);
    if (!cache) {
        gm::logf(gm::LogLevel::Error, "sample-play", error);
        return 2;
    }

    const int plays = options.twice ? 2 : 1;
    std::vector<gm::audio::SampleCache::Handle> handles;
    std::optional<gm::AudioDevice> device;
    for (int i = 0; i < plays; ++i) {
        std::string acquire_error;
        const auto handle = cache->acquire(options.sample_play, &acquire_error);
        if (!handle) {
            gm::logf(gm::LogLevel::Error, "sample-play", acquire_error);
            return 2;
        }
        handles.push_back(*handle);

        const gm::audio::Sound* sound = cache->sound(*handle);
        if (sound == nullptr || sound->sample_rate == 0 || sound->channels == 0 ||
            sound->frames() == 0) {
            gm::logf(gm::LogLevel::Error, "sample-play",
                     "sample \"" + options.sample_play + "\" decoded to an empty clip");
            return 2;
        }
        const auto info = cache->find(options.sample_play);
        if (!info) {
            gm::logf(gm::LogLevel::Error, "sample-play",
                     "sample \"" + options.sample_play + "\" is not resident after acquire");
            return 2;
        }
        std::printf("sample: name=%s frames=%u rate=%u channels=%u bytes=%zu\n",
                    info->name.c_str(), sound->frames(), sound->sample_rate,
                    static_cast<unsigned>(sound->channels), info->bytes);

        if (options.headless) {
            continue;
        }
        if (!device) {
            auto opened = gm::AudioDevice::open(static_cast<int>(sound->sample_rate),
                                                static_cast<int>(sound->channels), false,
                                                error);
            if (!opened) {
                gm::logf(gm::LogLevel::Error, "sample-play", error);
                return 1;
            }
            device = std::move(*opened);
        }
        device->queue(sound->samples.data(), sound->frames());
        const double seconds = static_cast<double>(sound->frames()) /
                               static_cast<double>(sound->sample_rate);
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds + 0.2));
    }

    for (const auto& handle : handles) {
        std::string release_error;
        if (!cache->release(handle, &release_error)) {
            gm::logf(gm::LogLevel::Error, "sample-play", release_error);
            return 2;
        }
    }

    const auto stats = cache->stats();
    std::printf("stats: hits=%llu misses=%llu evictions=%llu peak=%zu\n",
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses),
                static_cast<unsigned long long>(stats.evictions), stats.peak_bytes);
    return 0;
}

int dump_file(const gm::Options& options) {
    fs::path path = options.dump;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (path.is_relative()) {
            if (const auto resolved = gm::resolve_ci(options.game, path.string())) {
                path = *resolved;
            }
        }
    }
    if (!fs::exists(path, ec)) {
        gm::logf(gm::LogLevel::Error, "dump", "no such file: ", options.dump.string());
        return 2;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        gm::logf(gm::LogLevel::Error, "dump", "cannot open: ", path.string());
        return 2;
    }
    const auto size = fs::file_size(path, ec);
    if (ec) {
        gm::logf(gm::LogLevel::Warn, "dump", "file_size: ", ec.message());
    }
    std::vector<std::uint8_t> head(64);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));

    std::printf("file: %s\n", path.string().c_str());
    std::printf("size: %s bytes\n", ec ? "?" : std::to_string(size).c_str());
    std::printf("head: %s\n", gm::format_hex(head).c_str());
    std::printf("ascii: %s\n", gm::format_ascii(head).c_str());

    const std::string ext = path.extension().string();
    std::string error;

    if (gm::iequals_ascii(ext, ".sbm")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sbm = gm::parse_sbm(*bytes, error);
        if (!sbm) {
            gm::logf(gm::LogLevel::Error, "sbm", error);
            return 2;
        }
        std::printf("kind: SBM texture\n");
        std::printf("dimensions: %dx%d\n", sbm->width, sbm->height);
        std::printf("levels: %d\n", sbm->levels);
        std::printf("flags: %u\n", sbm->flags);
        for (int i = 0; i < sbm->levels; ++i) {
            std::printf("mip%d: %dx%d\n", i, sbm->mips[static_cast<std::size_t>(i)].width,
                        sbm->mips[static_cast<std::size_t>(i)].height);
        }
        if (!options.png.empty()) {
            const auto palette = gm::installed_palette(options.game, error);
            if (!palette) {
                gm::logf(gm::LogLevel::Error, "palette", error);
                return 2;
            }
            const auto& level = sbm->mips[0];
            const auto rgba = gm::sbm_to_rgba(level, *palette);
            return export_png(options, level.width, level.height, rgba);
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sbv")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto clip = gm::parse_sbv(*bytes, error);
        if (!clip) {
            gm::logf(gm::LogLevel::Error, "sbv", error);
            return 2;
        }
        std::printf("kind: SBV video clip\n");
        std::printf("size: %ux%u\n", clip->width, clip->height);
        std::printf("frames: %u\n", clip->frame_count);
        std::printf("data start: %zu\n", clip->data_start);
        std::printf("offsets valid: %s\n", gm::sbv_offsets_valid(*clip) ? "yes" : "no");
        if (!clip->table.empty()) {
            std::printf("duration: %u ms\n", clip->table.back().timestamp_ms);
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sog")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sog = gm::parse_sog(*bytes, error);
        if (!sog) {
            gm::logf(gm::LogLevel::Error, "sog", error);
            return 2;
        }
        std::printf("kind: SOG song playlist\n");
        std::printf("tracks: %zu\n", sog->names.size());
        std::printf("first: %s\n", sog->names.front().c_str());
        std::printf("last: %s\n", sog->names.back().c_str());
        return 0;
    }

    if (gm::iequals_ascii(ext, ".soe")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        std::string structured_error;
        const auto structured = gm::parse_soe(
            *bytes, structured_error, std::filesystem::path(path).stem().string());
        if (structured) {
            std::printf("kind: SOE thing template\n");
            std::printf("thing: %s id: %s version: %d\n",
                        structured->thing().name.c_str(),
                        structured->thing().id.c_str(),
                        structured->thing().version);
            std::printf("fields: %zu extra: %zu\n",
                        structured->thing().fields.size(),
                        structured->thing().extra);
            std::printf("map: %zu fields\n", gm::field_map().size());
            return 0;
        }
        const auto soe = gm::parse_soe_text(*bytes, error);
        if (!soe) {
            gm::logf(gm::LogLevel::Error, "soe", structured_error);
            return 2;
        }
        std::printf("kind: SOE thing template\n");
        std::printf("tokens: %zu\n", soe->tokens.size());
        if (soe->tokens.size() >= 4) {
            std::printf("head: %s %s %s %s\n", soe->tokens[0].c_str(),
                        soe->tokens[1].c_str(), soe->tokens[2].c_str(),
                        soe->tokens[3].c_str());
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sml") || gm::iequals_ascii(ext, ".sme")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const bool encoded = gm::iequals_ascii(ext, ".sme");
        std::string sml_error;
        const auto doc = gm::parse_sml(
            *bytes, encoded ? gm::SmlSource::Auto : gm::SmlSource::Plain, sml_error);
        if (doc) {
            const auto summary = gm::describe_sml(*doc);
            std::printf("kind: SML markup\n");
            std::printf("note: %s\n", doc->note.c_str());
            std::printf("chars: %zu lines: %zu tags: %zu\n", summary.chars,
                        summary.lines, summary.tags);
            std::printf("images: %zu fonts: %zu music: %zu text runs: %zu\n",
                        summary.images, summary.fonts, summary.music,
                        summary.text_runs);
            std::printf("document: %s\n", summary.doc_ok ? "ok" : "invalid");
            return 0;
        }
        const auto plain = encoded ? gm::sml_decode(*bytes) : *bytes;
        const std::string_view text(reinterpret_cast<const char*>(plain.data()), plain.size());
        // A file that neither parses as a document nor is framed by markup is a
        // broken asset (DoD: no silent skip, rc=2).  The shipped
        // `SML/prelevel/dm4.sml` is unbalanced markup, so it keeps the summary
        // path with a warning.
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        const std::size_t last = text.find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos || text[first] != '<' ||
            last == std::string_view::npos || text[last] != '>') {
            gm::logf(gm::LogLevel::Error, "dump",
                     "not SML markup: the document does not parse and has no <...> frame");
            return 2;
        }
        gm::logf(gm::LogLevel::Warn, "dump",
                 "the document is not well-formed; the summary below is best effort");
        const auto tags = gm::sml_tags(text);
        std::printf("kind: SML markup\n");
        std::printf("encoded: %s\n", encoded ? "yes" : "no");
        std::printf("bytes: %zu\n", plain.size());
        std::printf("tags: %zu\n", tags.size());
        if (!tags.empty()) {
            std::printf("first tag: <%s%s>\n", tags.front().closing ? "/" : "",
                        tags.front().name.c_str());
        }
        const auto stripped = gm::sml_plain(text);
        const std::size_t newline = stripped.find('\n');
        std::printf("text: %s\n",
                    stripped.substr(0, newline == std::string::npos ? stripped.size() : newline)
                        .c_str());
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sob")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sob = gm::parse_sob(*bytes, error);
        if (!sob) {
            gm::logf(gm::LogLevel::Error, "sob", error);
            return 2;
        }
        std::printf("kind: SOB model\n");
        std::printf("vertices: %zu\n", sob->vertices.size());
        std::printf("faces: %zu\n", sob->faces.size());
        if (!sob->faces.empty()) {
            std::printf("first texture: %s\n", sob->faces.front().texture().c_str());
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".soo")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto soo = gm::parse_soo(*bytes, error);
        if (!soo) {
            gm::logf(gm::LogLevel::Error, "soo", error);
            return 2;
        }
        std::printf("kind: SOO model animation\n");
        std::printf("mode: %d\n", soo->mode);
        std::printf("declared frames: %d\n", soo->declared);
        std::printf("frames: %zu\n", soo->frames.size());
        if (!soo->frames.empty()) {
            std::printf("first: %s %d ticks\n", soo->frames.front().name.c_str(),
                        soo->frames.front().duration);
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".soa")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto soa = gm::parse_soa(*bytes, error);
        if (!soa) {
            gm::logf(gm::LogLevel::Error, "soa", error);
            return 2;
        }
        std::printf("kind: SOA animation\n");
        std::printf("mode: %d period: %d (%d ticks) loop: %d\n", soa->mode,
                    soa->period, soa->period * gm::kEngineTickHz / 100, soa->loop);
        std::printf("declared frames: %d\n", soa->declared);
        std::printf("names: %zu\n", soa->names.size());
        if (!soa->names.empty()) {
            std::printf("first: %s\n", soa->names.front().c_str());
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sbc")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sbc = gm::parse_sbc(*bytes, error);
        if (!sbc) {
            gm::logf(gm::LogLevel::Error, "sbc", error);
            return 2;
        }
        std::printf("kind: SBC lightmaps\n");
        std::printf("magic: %s\n", sbc->magic.c_str());
        std::printf("records: %zu\n", sbc->records.size());
        std::size_t texels = 0;
        for (const auto& record : sbc->records) {
            texels += record.pixels.size();
        }
        std::printf("texels: %zu\n", texels);
        if (!sbc->records.empty()) {
            std::printf("first: %dx%d\n", sbc->records.front().width,
                        sbc->records.front().height);
        }
        if (!options.png.empty()) {
            if (sbc->records.empty()) {
                gm::logf(gm::LogLevel::Error, "png", "no records in ", path.string());
                return 2;
            }
            const auto& record = sbc->records.front();
            const std::uint8_t shift = sbc->magic == "SBC0" ? 2 : 0;
            std::vector<std::uint8_t> rgba(record.pixels.size() * 4, 255);
            for (std::size_t i = 0; i < record.pixels.size(); ++i) {
                const std::uint8_t value =
                    static_cast<std::uint8_t>(record.pixels[i] << shift);
                rgba[i * 4 + 0] = value;
                rgba[i * 4 + 1] = value;
                rgba[i * 4 + 2] = value;
            }
            return export_png(options, record.width, record.height, rgba);
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sbt")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        // A save is a .sbt-shaped container with a printable 12-byte title;
        // a level script starts with binary SBT records.  Try the save reader
        // first; on failure fall through to the level parser.
        std::string save_error;
        if (auto save = gm::parse_save(*bytes, save_error)) {
            std::printf("kind: SAVE\n");
            std::printf("title: %s\n", save->header.title.c_str());
            std::printf("version word: 0x%x\n", save->header.version);
            std::printf("level: %s\n", save->header.level_name.c_str());
            std::printf("blocks: %zu\n", save->blocks.size());
            for (const auto& block : save->blocks) {
                std::printf("  id %2u count %u size %zu @0x%zx\n", block.id,
                            block.count, block.size, block.offset);
            }
            if (save->sector_stuff.size() == gm::kSbtSectorSlots) {
                std::size_t active = 0;
                for (const auto& slot : save->sector_stuff) {
                    active += slot.empty() ? 0 : 1;
                }
                std::printf("sector stuff: %zu slots, %zu active\n",
                            save->sector_stuff.size(), active);
            }
            std::printf("F0/F1: %u/%u\n", save->header.fields[0],
                        save->header.fields[1]);
            if (!options.save_edits.empty()) {
                if (!save->player) {
                    gm::logf(gm::LogLevel::Error, "save", "--set needs a player block");
                    return 2;
                }
                std::string edit_error;
                if (!apply_save_edits(options, *save->player, edit_error)) {
                    gm::logf(gm::LogLevel::Error, "save", edit_error);
                    return 2;
                }
            }
            if (!options.save_out.empty()) {
                std::string write_error;
                const auto out = save->to_bytes(write_error);
                if (!out) {
                    gm::logf(gm::LogLevel::Error, "save", write_error);
                    return 2;
                }
                if (!write_file(options.save_out, *out)) {
                    gm::logf(gm::LogLevel::Error, "save",
                             "cannot write ", options.save_out.string());
                    return 2;
                }
                std::printf("save-out: %s (%zu bytes, byte-exact=%s)\n",
                            options.save_out.string().c_str(), out->size(),
                            *out == *bytes ? "yes" : "no");
            }
            if (save->player) {
                const auto& p = *save->player;
                std::printf("player: present=%d pos=%.3f,%.3f,%.3f "
                            "angles=%.6f,%.6f,%.6f armor=%.2f/%.2f "
                            "shields=%.2f/%.2f/%.2f/%.2f ammo=%u,%u\n",
                            p.has_slot ? 1 : 0, p.position[0], p.position[1],
                            p.position[2], p.angles[0], p.angles[1], p.angles[2],
                            p.armor_points(), p.armor_max_points(),
                            p.shield_points(0), p.shield_points(1),
                            p.shield_points(2), p.shield_points(3), p.ammo[0],
                            p.ammo[1]);
            }
            return 0;
        }
        const auto level = gm::parse_sbt(*bytes, error);
        if (!level) {
            gm::logf(gm::LogLevel::Error, "sbt", error);
            return 2;
        }
        std::printf("kind: SBT level\n");
        std::printf("version: %u\n", level->version);
        std::printf("records: %zu\n", level->records.size());
        for (const auto& record : level->records) {
            std::printf("  %3u %-20s %7zu\n", record.type, record.name.c_str(), record.size);
        }
        std::printf("poly handles: %zu\n", level->poly_handles.size());
        std::printf("object handles: %zu\n", level->object_handles.size());
        std::printf("sector handles: %zu\n", level->sector_handles.size());
        std::printf("sobs: %zu\n", level->sobs.size());
        std::printf("script: things=%u handles=%u code=%u flag=%u\n", level->script.things,
                    level->script.handles, level->script.code_size, level->script.flag);
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sou")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto sou = gm::parse_sou(*bytes, error);
        if (!sou) {
            gm::logf(gm::LogLevel::Error, "sou", error);
            return 2;
        }
        std::printf("kind: SOU sample\n");
        std::printf("magic: %s\n", sou->magic.c_str());
        std::printf("rate: %u\n", sou->rate);
        if (sou->magic == "SOU2") {
            std::printf("units: %u\n", sou->units);
        } else {
            std::printf("channels: %u\n", sou->flags ? 2u : 1u);
            std::printf("length: %u\n", sou->length);
        }
        std::printf("payload: %zu bytes\n", sou->payload.size());
        return 0;
    }

    if (head.size() >= 2 && head[0] == 'B' && head[1] == 'M') {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto bmp = gm::parse_bmp(*bytes, error);
        if (!bmp) {
            gm::logf(gm::LogLevel::Error, "bmp", error);
            return 2;
        }
        std::printf("kind: BMP image\n");
        std::printf("dimensions: %dx%d\n", bmp->width, bmp->height);
        std::printf("bpp: %d\n", bmp->bpp);
        std::printf("palette entries: %zu\n", bmp->palette.size() / 4);
        if (!options.png.empty()) {
            return export_png(options, bmp->width, bmp->height, bmp_to_rgba(*bmp));
        }
        return 0;
    }

    if (gm::iequals_ascii(ext, ".sbw")) {
        const auto bytes = read_file(path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        const auto level = gm::parse_sbw(*bytes, error);
        if (!level) {
            gm::logf(gm::LogLevel::Error, "sbw", error);
            return 2;
        }
        std::printf("kind: SBW level\n");
        std::printf("textures: %zu\n", level->header.textures.size());
        std::printf("effects: %zu\n", level->header.effects.size());
        std::printf("sectors: %zu\n", level->header.sectors.size());
        std::printf("objects: %zu\n", level->header.object_count);
        std::printf("vertices: %zu\n", level->total_vertices);
        std::printf("polygons: %zu\n", level->total_polygons);
        std::printf("header: %zu bytes\n", level->header.header_end);
        std::printf("data end: %zu bytes\n", level->file_end);
        if (!level->header.textures.empty()) {
            std::printf("first texture: %s\n", level->header.textures.front().name.c_str());
        }
        return 0;
    }

    if (head.size() >= 7 && std::memcmp(head.data(), "BIGFILE", 7) == 0) {
        auto big = gm::BigFile::open(path, error);
        if (!big) {
            gm::logf(gm::LogLevel::Error, "dump", error);
            return 2;
        }
        std::printf("kind: BIGFILE index\n");
        std::printf("entries: %zu\n", big->entries().size());
        std::printf("data: %llu bytes\n",
                    static_cast<unsigned long long>(big->dat_size()));
        if (!big->entries().empty()) {
            const auto& first = big->entries().front();
            const auto& last = big->entries().back();
            std::printf("first: %s (%u bytes)\n", first.name.c_str(), first.size);
            std::printf("last: %s (%u bytes)\n", last.name.c_str(), last.size);
        }
        return 0;
    }

    if (!options.png.empty()) {
        gm::logf(gm::LogLevel::Error, "png", "unsupported input for --png: ", path.string());
        return 2;
    }
    return 0;
}

bool start_camera(const gm::Options& options, const gm::SbtLevel& entities,
                  gm::Vec3d& eye, gm::Vec3d& target, std::string& error);

/// Append the player chassis at SET_PLAYER_START and fill the third-person
/// eye/target.  The chassis is placed feet-on-spawn and turned by the spawn
/// yaw; the camera is the tested `CameraRig` third-person mode with level
/// occlusion.
bool third_person_camera(const gm::Options& options, const gm::LevelBundle& level,
                         std::vector<gm::RenderMesh>& meshes, gm::Vec3d& eye,
                         gm::Vec3d& target, std::string& error);

/// Apply ``--set`` edits to the first present save player.
bool apply_save_edits(const gm::Options& options, gm::SavePlayer& player,
                      std::string& error) {
    for (const std::string& spec : options.save_edits) {
        const std::size_t eq = spec.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) {
            error = "--set expects KEY=VALUE: " + spec;
            return false;
        }
        std::string key = spec.substr(0, eq);
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::string value = spec.substr(eq + 1);
        try {
            const long number = std::stol(value);
            if (key == "armor") {
                player.armor = static_cast<std::int32_t>(number * 100);
            } else if (key == "armor_max") {
                player.armor_max = static_cast<std::int32_t>(number * 100);
            } else if (key == "shield_max") {
                player.shield_max = static_cast<std::int32_t>(number * 100);
            } else if (key == "weapon") {
                player.weapon_slot = static_cast<std::int32_t>(number);
            } else if (key == "posx" || key == "posy" || key == "posz") {
                const double v = std::stod(value);
                player.position[key == "posx" ? 0 : (key == "posy" ? 1 : 2)] = v;
            } else if (key.rfind("ammo", 0) == 0) {
                const int slot = std::stoi(key.substr(4));
                if (slot < 0 || slot >= 12) {
                    error = "--set ammo slot must be 0..11: " + spec;
                    return false;
                }
                player.ammo[static_cast<std::size_t>(slot)] =
                    static_cast<std::uint32_t>(number);
            } else if (key.rfind("shield", 0) == 0) {
                const int slot = std::stoi(key.substr(6));
                if (slot < 0 || slot >= 4) {
                    error = "--set shield slot must be 0..3: " + spec;
                    return false;
                }
                player.shields[static_cast<std::size_t>(slot)] =
                    static_cast<std::int32_t>(number * 100);
            } else {
                error = "unknown --set key: " + key;
                return false;
            }
        } catch (const std::exception&) {
            error = "bad --set value: " + spec;
            return false;
        }
    }
    return true;
}

/// Compose the optional `--hud` film onto an already rendered frame.
bool overlay_hud(const gm::Options& options, gm::RenderResult& image,
                 std::uint64_t tick, std::string& error) {
    if (options.hud.empty()) {
        return true;
    }
    const auto film = gm::load_hud_film(options.game, options.hud, error);
    if (!film) {
        return false;
    }
    const auto palette = gm::installed_palette(options.game, error);
    if (!palette) {
        return false;
    }
    return gm::draw(*film, image, *palette, options.hud_x, options.hud_y,
                    true, options.hud_scale, options.hud_opacity, tick, error);
}

int render_image(const gm::Options& options) {
    std::string error;
    std::vector<gm::RenderMesh> meshes;
    const gm::Sbc* lightmaps = nullptr;

    std::optional<gm::LevelBundle> level;
    std::optional<gm::Sob> model;
    if (!options.anim.empty()) {
        std::string name = options.anim;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".soo") != 0) {
            name += ".soo";
        }
        auto path = gm::resolve_ci(options.game / "Sobanim", name);
        if (!path) {
            gm::logf(gm::LogLevel::Error, "anim", "not found: " + name);
            return 2;
        }
        const auto bytes = read_file(*path, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "anim", error);
            return 2;
        }
        const auto soo = gm::parse_soo(*bytes, error);
        if (!soo) {
            gm::logf(gm::LogLevel::Error, "anim", error);
            return 2;
        }
        const auto pose = gm::soo_pose_at(*soo, options.at, error);
        if (!pose) {
            gm::logf(gm::LogLevel::Error, "anim", error);
            return 2;
        }
        auto load_pose = [&](const std::string& model_name) -> std::optional<gm::Sob> {
            const auto model_path = gm::resolve_ci(options.game / "SOBS",
                                                   model_name + ".sob");
            if (!model_path) {
                error = "pose model not found: " + model_name;
                return std::nullopt;
            }
            const auto model_bytes = read_file(*model_path, error);
            return model_bytes ? gm::parse_sob(*model_bytes, error) : std::nullopt;
        };
        const auto first = load_pose(pose->name);
        const auto second = load_pose(pose->next_name);
        if (!first || !second) {
            gm::logf(gm::LogLevel::Error, "anim", error);
            return 2;
        }
        const auto morphed = gm::morph_sob(*first, *second, pose->alpha, error);
        if (!morphed) {
            gm::logf(gm::LogLevel::Error, "anim", error);
            return 2;
        }
        meshes = gm::build_model_meshes(*morphed);
        gm::logf(gm::LogLevel::Info, "anim", "mode=", soo->mode, " frame=", pose->index,
                 "->", pose->next_index, " alpha=", pose->alpha, " at=", options.at,
                 "ms");
    } else if (!options.model.empty()) {
        const auto bytes = read_file(options.model, error);
        if (!bytes) {
            gm::logf(gm::LogLevel::Error, "render", error);
            return 2;
        }
        model = gm::parse_sob(*bytes, error);
        if (!model) {
            gm::logf(gm::LogLevel::Error, "render", options.model.string() + ": " + error);
            return 2;
        }
        meshes = gm::build_model_meshes(*model);
    } else {
        if (options.level.empty()) {
            gm::logf(gm::LogLevel::Error, "render", "--render needs --level NAME or --model FILE");
            return 2;
        }
        level = gm::load_level(options.game, options.level, error);
        if (!level || !level->world) {
            gm::logf(gm::LogLevel::Error, "render", error);
            return 2;
        }
        lightmaps = level->lightmaps ? &*level->lightmaps : nullptr;
        meshes = gm::build_level_meshes(*level->world, lightmaps);

        if (options.things && level->entities) {
            std::string scene_error;
            const auto scene = gm::extract_scene(*level->entities, scene_error);
            if (!scene) {
                gm::logf(gm::LogLevel::Error, "things", scene_error);
            } else {
                std::map<std::string, std::optional<gm::Sob>> cache;
                std::size_t placed = 0;
                for (const auto& thing : scene->things) {
                    if (!thing.absolute || thing.model.empty()) {
                        continue;
                    }
                    auto it = cache.find(thing.model);
                    if (it == cache.end()) {
                        std::optional<gm::Sob> model;
                        const auto path = gm::resolve_ci(options.game / "SOBS",
                                                         thing.model + ".sob");
                        if (path) {
                            std::string read_error;
                            if (const auto bytes = read_file(*path, read_error)) {
                                std::string parse_error;
                                model = gm::parse_sob(*bytes, parse_error);
                                if (!model) {
                                    gm::logf(gm::LogLevel::Error, "things", thing.model,
                                             ": ", parse_error);
                                }
                            } else {
                                gm::logf(gm::LogLevel::Error, "things", read_error);
                            }
                        }
                        it = cache.emplace(thing.model, std::move(model)).first;
                    }
                    if (!it->second) {
                        continue;
                    }
                    // gm/scene.py and the C++ .sbw renderer both use the
                    // engine world frame (`origin + basis * local`, basis =
                    // identity, docs/WORLD_FIELDS.md §3).  Do not rescale
                    // ADD_THING positions: they are already world.
                    auto placed_meshes = gm::place_model_meshes(
                        *it->second,
                        {static_cast<double>(thing.x), static_cast<double>(thing.y),
                         static_cast<double>(thing.z)},
                        options.thing_scale);
                    meshes.insert(meshes.end(),
                                  std::make_move_iterator(placed_meshes.begin()),
                                  std::make_move_iterator(placed_meshes.end()));
                    ++placed;
                }
                gm::logf(gm::LogLevel::Info, "things", "placed=", placed,
                         " models=", cache.size());
            }
        }
    }

    gm::TexturePool pool(options.game);
    gm::RenderOptions render;
    render.width = options.width;
    render.height = options.height;
    render.az = options.az;
    render.el = options.el;
    render.zoom = options.zoom;
    render.fov = options.fov;
    render.textures = !options.no_textures;
    render.lightmaps = !options.no_lightmaps;
    render.lightmap_world = options.lightmap_world;
    render.wire = options.wire;
    render.mip = options.mip;
    render.mip_auto = options.mip_auto;
    render.bilinear = options.bilinear;
    render.alpha_test = options.alpha_test;
    render.transparent_index = options.transparent_index;
    render.chromakey = options.chromakey;
    render.texel_uv = options.texel_uv;
    render.skip_untextured = options.skip_untextured;
    render.haze = options.haze;
    if (options.has_pos != options.has_look) {
        gm::logf(gm::LogLevel::Error, "render", "--pos and --look must be given together");
        return 2;
    }
    gm::Vec3d third_eye{};
    gm::Vec3d third_target{};
    bool third_ready = false;
    if (options.third_person) {
        if (!level || !level->world) {
            gm::logf(gm::LogLevel::Error, "third-person",
                     "--third-person needs --level NAME");
            return 2;
        }
        std::string third_error;
        if (!third_person_camera(options, *level, meshes, third_eye, third_target,
                                 third_error)) {
            gm::logf(gm::LogLevel::Error, "third-person", third_error);
            return 2;
        }
        third_ready = true;
    }
    if (options.has_pos) {
        render.camera_override = true;
        render.eye = {options.pos[0], options.pos[1], options.pos[2]};
        render.target = {options.look[0], options.look[1], options.look[2]};
    } else if (third_ready) {
        render.camera_override = true;
        render.eye = third_eye;
        render.target = third_target;
    } else if (options.spawn_camera && level && level->entities) {
        std::string spawn_error;
        if (!start_camera(options, *level->entities, render.eye, render.target,
                          spawn_error)) {
            gm::logf(gm::LogLevel::Error, "render", spawn_error);
            return 2;
        }
        render.camera_override = true;
    }
    render.clip = options.clip;

    std::size_t faces = 0;
    for (const auto& mesh : meshes) {
        faces += mesh.faces.size();
    }
    if (!meshes.empty() && !meshes.front().vertices.empty()) {
        gm::Vec3d lo{1e30, 1e30, 1e30};
        gm::Vec3d hi{-1e30, -1e30, -1e30};
        for (const auto& mesh : meshes) {
            for (const auto& v : mesh.vertices) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
        }
        std::printf("bounds: (%g, %g, %g) .. (%g, %g, %g)\n", lo.x, lo.y, lo.z, hi.x, hi.y,
                    hi.z);
    }
    auto image = gm::render_scene(meshes, render, &pool, lightmaps);
    std::string hud_error;
    if (!overlay_hud(options, image, static_cast<std::uint64_t>(options.hud_tick),
                     hud_error)) {
        gm::logf(gm::LogLevel::Error, "hud", hud_error);
        return 2;
    }
    if (!options.hud_screen.empty()) {
        const auto palette = gm::installed_palette(options.game, hud_error);
        if (!palette) {
            gm::logf(gm::LogLevel::Error, "hud", hud_error);
            return 2;
        }
        gm::HudState state;
        const auto summary = gm::compose_hud(
            image, options.game, options.game / options.hud_screen, state,
            *palette, static_cast<std::uint64_t>(options.hud_tick), hud_error);
        if (!summary) {
            gm::logf(gm::LogLevel::Error, "hud", hud_error);
            return 2;
        }
        std::printf("hud: %s family=%s film=%s line=%d frame=%zu "
                    "placements=%zu visible=%zu clipped=%zu\n",
                    summary->stem.c_str(), summary->family_name.c_str(),
                    summary->film.c_str(), summary->line, summary->frame,
                    summary->placements, summary->visible, summary->clipped);
    }

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(image.width) * image.height * 4, 255);
    for (std::size_t i = 0; i < image.rgb.size() / 3; ++i) {
        rgba[i * 4 + 0] = image.rgb[i * 3 + 0];
        rgba[i * 4 + 1] = image.rgb[i * 3 + 1];
        rgba[i * 4 + 2] = image.rgb[i * 3 + 2];
    }
    const auto png = gm::encode_png_rgba(image.width, image.height, rgba);
    if (!write_file(options.render, png)) {
        gm::logf(gm::LogLevel::Error, "render", "cannot write " + options.render.string());
        return 2;
    }
    std::printf("render: %dx%d\n", image.width, image.height);
    std::printf("meshes: %zu faces: %zu\n", meshes.size(), faces);
    std::printf("textures: %zu loaded, %zu missing\n", pool.loads(), pool.misses());
    std::printf("output: %s (%zu bytes)\n", options.render.string().c_str(), png.size());
    return 0;
}

int show_level(const gm::Options& options) {
    std::string error;
    const auto level = gm::load_level(options.game, options.level, error);
    if (!level) {
        gm::logf(gm::LogLevel::Error, "level", error);
        return 2;
    }
    std::printf("level: %s\n", level->name.c_str());
    if (level->world) {
        std::printf("world: textures=%zu effects=%zu sectors=%zu objects=%zu "
                    "vertices=%zu polygons=%zu bytes=%zu\n",
                    level->world->header.textures.size(), level->world->header.effects.size(),
                    level->world->header.sectors.size(), level->world->header.object_count,
                    level->world->total_vertices, level->world->total_polygons,
                    level->world->file_end);
    }
    if (level->entities) {
        std::printf("entities: records=%zu poly=%zu object=%zu sector=%zu sobs=%zu "
                    "script=%u/%u\n",
                    level->entities->records.size(), level->entities->poly_handles.size(),
                    level->entities->object_handles.size(),
                    level->entities->sector_handles.size(), level->entities->sobs.size(),
                    level->entities->script.handles, level->entities->script.code_size);
        std::map<int, int> histogram;
        for (const auto& record : level->entities->records) {
            ++histogram[record.type];
        }
        std::printf("record types:");
        for (const auto& [type, count] : histogram) {
            std::printf(" %d:%d", type, count);
        }
        std::printf("\n");
    }
    if (level->lightmaps) {
        std::size_t texels = 0;
        for (const auto& record : level->lightmaps->records) {
            texels += record.pixels.size();
        }
        std::printf("lightmaps: magic=%s records=%zu texels=%zu\n",
                    level->lightmaps->magic.c_str(), level->lightmaps->records.size(), texels);
    }
    return 0;
}

/// `--sim LEVEL`: one headless fixed-tick run (`M6.5.1` scaffold).
///
/// Builds exactly what the tests build (`vm_state_from_level` +
/// `GameRuntime::build`) from a shipped level and steps the 91 Hz tick
/// `--ticks` times.  No window, no input, no rendering: the point is a
/// deterministic, scriptable run that can be diffed between builds.  The
/// digest covers every thing position/alive flag and every door phase, so two
/// runs of the same build must print the same number.
int simulate(const gm::Options& options) {
    std::string error;
    const auto level = gm::load_level(options.game, options.sim, error);
    if (!level) {
        gm::logf(gm::LogLevel::Error, "sim", error);
        return 2;
    }
    if (!level->world || !level->entities) {
        gm::logf(gm::LogLevel::Error, "sim",
                 "the level has no .sbw/.sbt pair: " + options.sim);
        return 2;
    }

    gm::VmContext context;
    context.game_mode = options.sim_mode;
    context.player_count = options.sim_players;
    context.players = options.sim_players;
    context.difficulty = options.sim_difficulty;
    context.god_mode = options.sim_god;
    const gm::VmState vm = gm::vm_state_from_level(*level->entities, context);

    // Opt-in AI -> movement bridge: without it the automata never receive a
    // target and a long run would be indistinguishable from a short one.
    gm::GameRuntimeConfig config;
    config.ai_apply_aim = true;
    gm::GameRuntime runtime(config);
    if (!runtime.build(*level->entities, vm, *level->world, &error)) {
        gm::logf(gm::LogLevel::Error, "sim", error);
        return 2;
    }

    // The first campaign spawn is the player's feet point (`SET_PLAYER_START`,
    // slot 0) and the eye target of the AI phase.  Geometry-less runs keep the
    // origin.
    gm::Vec3d feet{};
    double spawn_yaw = 0.0;
    bool have_spawn = false;
    std::string scene_error;
    if (const auto scene = gm::extract_scene(*level->entities, scene_error, false)) {
        for (const gm::SceneSpawn& spawn : scene->spawns) {
            if (!spawn.deathmatch && spawn.slot == 0) {
                feet.x = static_cast<double>(spawn.x);
                feet.y = static_cast<double>(spawn.y);
                feet.z = static_cast<double>(spawn.z);
                spawn_yaw = spawn.angles[2];
                have_spawn = true;
                break;
            }
        }
    }
    gm::Vec3d eye = feet;
    eye.z += 10.0;
    const std::span<const gm::Vec3d> eyes(&eye, 1);
    if (!runtime.set_player_eyes(eyes)) {
        gm::logf(gm::LogLevel::Error, "sim", "player eye point rejected");
        return 2;
    }

    // `--sim-drive PLAN`: one scripted player on top of the runtime
    // (`docs/SIM_PLAYER.md`).  Without it the runtime just ticks, which is the
    // M6.5.1 scaffold.
    std::vector<gm::DriveSegment> drive_plan;
    gm::SimPlayer player;
    std::size_t drive_walls = 0;
    int drive_ticks = 0;
    if (!options.sim_drive.empty()) {
        const auto parsed = gm::parse_drive(options.sim_drive, &error);
        if (!parsed) {
            gm::logf(gm::LogLevel::Error, "sim", error);
            return 2;
        }
        drive_plan = *parsed;
        if (!have_spawn) {
            gm::logf(gm::LogLevel::Error, "sim",
                     "no campaign spawn for slot 0: cannot place the player");
            return 2;
        }
        const std::filesystem::path level_path = options.game / (options.sim + ".sbt");
        std::string chassis_error;
        const auto chassis = gm::chassis_from_level(level_path, &chassis_error);
        if (!chassis) {
            gm::logf(gm::LogLevel::Error, "sim", chassis_error);
            return 2;
        }
        const gm::ChassisSpec* light = nullptr;
        for (const gm::ChassisSpec& spec : *chassis) {
            if (spec.name.find("LIGHT") != std::string::npos) {
                light = &spec;
                break;
            }
        }
        if (light == nullptr) {
            gm::logf(gm::LogLevel::Error, "sim",
                     "no LIGHT chassis record in " + level_path.string());
            return 2;
        }
        if (!player.init(feet, spawn_yaw, *light, &error)) {
            gm::logf(gm::LogLevel::Error, "sim", error);
            return 2;
        }
        // `--sim-vitals ARMOR,SHIELD`: the starting gauges (`docs/DAMAGE.md`
        // §7 for the LIGHT defaults; the shields are the same on all sides).
        if (!options.sim_vitals.empty()) {
            const std::string& spec = options.sim_vitals;
            const std::size_t comma = spec.find(',');
            if (comma == std::string::npos) {
                gm::logf(gm::LogLevel::Error, "sim",
                         "--sim-vitals wants ARMOR,SHIELD, got \"" + spec + "\"");
                return 2;
            }
            auto number = [](const std::string& text, int& out) {
                if (text.empty()) {
                    return false;
                }
                std::size_t used = 0;
                try {
                    out = std::stoi(text, &used);
                } catch (const std::exception&) {
                    return false;
                }
                return used == text.size() && out >= 0;
            };
            int armor = 0;
            int shield = 0;
            if (!number(spec.substr(0, comma), armor) ||
                !number(spec.substr(comma + 1), shield)) {
                gm::logf(gm::LogLevel::Error, "sim",
                         "--sim-vitals wants two numbers ARMOR,SHIELD, got \"" + spec + "\"");
                return 2;
            }
            gm::Vitals vitals = gm::kLightPlayerVitals;
            vitals.armor = armor;
            vitals.armor_max = armor > 0 ? armor : gm::kLightPlayerVitals.armor_max;
            vitals.shields = {shield, shield, shield, shield};
            vitals.shield_max = shield > 0 ? shield : gm::kLightPlayerVitals.shield_max;
            if (!player.set_vitals(vitals, &error)) {
                gm::logf(gm::LogLevel::Error, "sim", error);
                return 2;
            }
        }
        // A table-only load has an empty save loadout, so the drive would hold
        // the trigger with no weapon selected.  Make every table slot
        // available with its ammo-type maximum, like a full pickup loadout.
        std::string weapons_error;
        if (auto table = gm::Weapons::load(level_path, &weapons_error)) {
            gm::Arsenal arsenal = table->initial();
            arsenal.mask = 0;
            int projectile_slot = -1;
            for (int slot = 0; slot < static_cast<int>(table->size()); ++slot) {
                const gm::Weapon* weapon = table->at(slot);
                if (weapon == nullptr) {
                    continue;
                }
                arsenal.mask |= 1u << slot;
                arsenal.ammo[slot] = table->ammo_max_for(weapon->ammo_type);
                // Prefer an automatic projectile weapon: the charge modes
                // (2..6) fire on release, and the drive plan has no explicit
                // release verb, so a charged slot would never shoot.
                // Rank the projectile weapons: automatic (mode 1) with blast
                // data first, then automatic, then any non-hitscan.  The
                // charge modes (2..6) fire on release, and the drive plan has
                // no explicit release verb.
                if (weapon->name == "ROCKET") {
                    // The documented projectile weapon of the shipped data
                    // (`docs/SIM_PROJECTILE.md`: slot 7, round TT31).
                    projectile_slot = slot;
                } else if (weapon->hitscan == false && projectile_slot >= 0 &&
                           table->at(projectile_slot)->name == "ROCKET") {
                    // ROCKET stays the choice.
                } else if (weapon->hitscan == false) {
                    auto rank = [table](int candidate) {
                        if (candidate < 0) {
                            return 0;
                        }
                        const gm::Weapon* other = table->at(candidate);
                        if (other == nullptr || other->hitscan) {
                            return 0;
                        }
                        const bool blasts = !other->impact_blasts.empty() ||
                                            !other->expire_blasts.empty();
                        return 1 + (other->fire_mode == 1 ? 2 : 0) + (blasts ? 4 : 0);
                    };
                    const int rank_now = 1 + (weapon->fire_mode == 1 ? 2 : 0) +
                        ((!weapon->impact_blasts.empty() ||
                          !weapon->expire_blasts.empty()) ? 4 : 0);
                    if (rank_now > rank(projectile_slot)) {
                        projectile_slot = slot;
                    }
                }
            }
            arsenal.current = 0;
            if (options.sim_projectile) {
                if (projectile_slot < 0) {
                    gm::logf(gm::LogLevel::Error, "sim",
                             "no projectile weapon in " + level_path.string());
                    return 2;
                }
                // A hitscan weapon never spawns a round, so the driven player
                // must hold a projectile weapon.
                arsenal.current = projectile_slot;
            }
            runtime.set_player_weapons(std::move(*table), arsenal);
        } else {
            gm::logf(gm::LogLevel::Info, "sim", "no player weapons: ", weapons_error);
        }
        // `--sim-projectile`: a round template instead of hitscan.  Prefer a
        // shipped thing with the projectile movement type; the shipped levels
        // carry none at start, so fall back to the synthetic round of
        // `engine/tests/test_sim_projectile.cpp` (constants cited in
        // `docs/SIM.md`).
        if (options.sim_projectile) {
            // The shipped levels start without a round in flight, so the
            // template is the synthetic one of `test_sim_projectile.cpp`
            // (movement type 6, see `docs/SIM_PROJECTILE.md`).
            gm::Thing projectile;
            projectile.id = -1;
            projectile.type = 31; // TT31, the ROCKET round
            projectile.movement_type = 6;
            projectile.gravity = 1.0;
            projectile.damping = 0.8;
            projectile.collision_shape = 1;
            projectile.radius = 10.0;
            projectile.hits_left = 1;
            projectile.expire_ticks = 364;
            projectile.create_event = 901;
            player.set_projectile(projectile, 0);
        }
        // A dead player's `tick()` is a no-op and its own counter stops, so
        // the drive counts its ticks itself (that counter also drives the
        // respawn delay).
        for (const gm::DriveSegment& segment : drive_plan) {
            for (int index = 0; index < segment.ticks; ++index) {
                ++drive_ticks;
                if (!player.tick(runtime, segment, index, &error)) {
                    gm::logf(gm::LogLevel::Error, "sim",
                             "tick " + std::to_string(player.tick_index()) + ": " + error);
                    return 2;
                }
                // In projectile mode the player already ticked the runtime
                // (the round and its blast are stepped there); a second tick
                // would advance the world twice per fixed tick.
                if (!player.has_projectile()) {
                    runtime.tick(1);
                }
                if (player.last_step().hit_wall) {
                    ++drive_walls;
                }
                // `--sim-respawn N`: the death rule is real (`armor <= 0`),
                // the delay is the drive plan's, not a DM rule
                // (`docs/SIM_DAMAGE.md` §5).
                if (!player.alive() && options.sim_respawn >= 0 &&
                    drive_ticks - player.death_tick() >= options.sim_respawn) {
                    if (!player.respawn(&error)) {
                        gm::logf(gm::LogLevel::Error, "sim", error);
                        return 2;
                    }
                }
            }
        }
        const gm::PlayerState& state = player.state();
        const int slot = runtime.player_arsenal().current;
        std::printf("sim: drive segments=%zu ticks=%d pos=(%.3f, %.3f, %.3f) yaw=%.6f\n",
                    drive_plan.size(), drive_ticks, state.pos.x, state.pos.y,
                    state.pos.z, player.yaw());
        std::printf("sim: drive on_ground=%d wall_ticks=%zu shots=%d "
                    "slot=%d ammo=%u shot_index=%llu\n",
                    state.on_ground ? 1 : 0, drive_walls, player.shots_fired(), slot,
                    player.ammo(slot),
                    static_cast<unsigned long long>(runtime.player_arsenal().shot_index));
        if (options.sim_projectile) {
            std::printf("sim: drive projectile=synthetic spawned=%d blasts=%d "
                        "things_damaged=%d\n", player.projectiles_spawned(),
                        player.blast_hits(), player.things_damaged());
        }
        const gm::Vitals& vitals = player.vitals();
        std::printf("sim: drive armor=%d/%d shields=%d/%d/%d/%d alive=%d "
                    "damage_taken=%d death_tick=%d deaths=%d\n",
                    vitals.armor, vitals.armor_max, vitals.shields[0],
                    vitals.shields[1], vitals.shields[2], vitals.shields[3],
                    player.alive() ? 1 : 0, player.damage_taken(),
                    player.death_tick(), player.deaths());
    }

    const std::size_t things_before = runtime.things().things().size();
    const std::uint64_t ticks = static_cast<std::uint64_t>(options.sim_ticks);
    if (drive_plan.empty() && ticks != 0) {
        runtime.tick(ticks);
    }

    std::uint64_t digest = 1469598103934665603ull;
    auto mix = [&digest](std::uint64_t value) {
        digest ^= value;
        digest *= 1099511628211ull;
    };
    // World coordinates are signed doubles; go through int64 so a negative
    // value is a well-defined bit pattern instead of a UB float->unsigned cast.
    auto mix_real = [&mix](double value) {
        mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(value * 1000.0)));
    };
    std::size_t alive = 0;
    for (const gm::Thing& thing : runtime.things().things()) {
        mix(static_cast<std::uint64_t>(thing.id));
        mix(static_cast<std::uint64_t>(thing.type + 1));
        mix(static_cast<std::uint64_t>(thing.alive ? 1 : 0));
        if (thing.alive) {
            ++alive;
        }
        if (thing.world_position) {
            for (double axis : *thing.world_position) {
                mix_real(axis);
            }
        } else if (thing.position) {
            for (int axis : *thing.position) {
                mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(axis)));
            }
        }
    }
    // Movement automata keep their own position (`thing_motion.h`), so the
    // thing record alone would hide every AI-driven move.
    const auto& moved_things = runtime.things().things();
    for (std::size_t index = 0; index < moved_things.size(); ++index) {
        const gm::ThingMotionState* state = runtime.things().motion_state(index);
        if (state == nullptr) {
            continue;
        }
        mix_real(state->position.x);
        mix_real(state->position.y);
        mix_real(state->position.z);
        mix_real(state->yaw);
    }
    for (const gm::Door& door : runtime.world().doors()) {
        mix(static_cast<std::uint64_t>(door.id));
        mix(door.phase);
        mix_real(door.open_fraction);
    }
    // A driven run also covers the player, so the digest moves when only the
    // player does (the runtime state alone would not see it).
    if (!drive_plan.empty()) {
        mix_real(player.state().pos.x);
        mix_real(player.state().pos.y);
        mix_real(player.state().pos.z);
        mix_real(player.yaw());
        mix(static_cast<std::uint64_t>(player.shots_fired()));
        mix(static_cast<std::uint64_t>(drive_ticks));
        mix(static_cast<std::uint64_t>(player.projectiles_spawned()));
        mix(static_cast<std::uint64_t>(player.blast_hits()));
        mix(static_cast<std::uint64_t>(player.things_damaged()));
        mix(static_cast<std::uint64_t>(player.vitals().armor));
        mix(static_cast<std::uint64_t>(player.deaths()));
        mix(player.alive() ? 1u : 0u);
    }

    const std::uint64_t printed_ticks =
        drive_plan.empty() ? ticks : static_cast<std::uint64_t>(drive_ticks);
    std::printf("sim: level=%s ticks=%llu mode=%d players=%d difficulty=%d god=%d\n",
                level->name.c_str(), static_cast<unsigned long long>(printed_ticks),
                options.sim_mode, options.sim_players, options.sim_difficulty,
                options.sim_god ? 1 : 0);
    std::printf("sim: things=%zu (alive %zu, initial %zu) doors=%zu switches=%zu "
                "lifts=%zu agents=%zu\n",
                runtime.things().things().size(), alive, things_before,
                runtime.world().doors().size(), runtime.world().switches().size(),
                runtime.world().lifts().size(), runtime.ai().agents().size());
    std::printf("sim: motion=stepped %zu moved %zu pushed %zu\n",
                runtime.things().stats().motion_steps,
                runtime.things().stats().motion_moved,
                runtime.things().stats().motion_pushed);
    std::printf("sim: digest=%016llx\n",
                static_cast<unsigned long long>(digest));
    std::printf("sim: ok\n");
    return 0;
}

/// The campaign spawn for `--spawn-slot`, copied out of the parsed scene.
std::optional<gm::SceneSpawn> campaign_spawn(const gm::Options& options,
                                             const gm::SbtLevel& entities,
                                             std::string& error) {
    const auto scene = gm::extract_scene(entities, error, false);
    if (!scene) {
        return std::nullopt;
    }
    for (const auto& spawn : scene->spawns) {
        if (!spawn.deathmatch && spawn.slot == options.spawn_slot) {
            return spawn;
        }
    }
    error = "no campaign spawn for slot " + std::to_string(options.spawn_slot);
    return std::nullopt;
}

/// Camera at SET_PLAYER_START: eye = feet + height, forward from the proven
/// 0x41e025 basis (X right, Y forward at yaw 0, Z up).
bool start_camera(const gm::Options& options, const gm::SbtLevel& entities,
                  gm::Vec3d& eye, gm::Vec3d& target, std::string& error) {
    const auto found = campaign_spawn(options, entities, error);
    if (!found) {
        return false;
    }
    const double pitch = found->angles[0];
    const double yaw = found->angles[2];
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const gm::Vec3d forward{sy * cp, cy * cp, -sp};
    eye = {static_cast<double>(found->x), static_cast<double>(found->y),
           static_cast<double>(found->z) + options.spawn_eye};
    target = {eye.x + forward.x * 100.0, eye.y + forward.y * 100.0,
              eye.z + forward.z * 100.0};
    return true;
}

bool third_person_camera(const gm::Options& options, const gm::LevelBundle& level,
                         std::vector<gm::RenderMesh>& meshes, gm::Vec3d& eye,
                         gm::Vec3d& target, std::string& error) {
    if (!level.entities) {
        error = "--third-person needs the level's .sbt (SET_PLAYER_START)";
        return false;
    }
    const auto spawn = campaign_spawn(options, *level.entities, error);
    if (!spawn) {
        return false;
    }

    const auto path = gm::resolve_ci(options.game / "SOBS",
                                     options.player_model + ".sob");
    if (!path) {
        error = "player model not found: SOBS/" + options.player_model + ".sob";
        return false;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        return false;
    }
    const auto model = gm::parse_sob(*bytes, error);
    if (!model) {
        error = options.player_model + ".sob: " + error;
        return false;
    }

    double min_z = 1e30;
    for (const auto& vertex : model->vertices) {
        min_z = std::min(min_z, static_cast<double>(vertex.z));
    }
    if (!(min_z < 1e30)) {
        error = options.player_model + ".sob has no vertices";
        return false;
    }

    gm::Vec3d feet{static_cast<double>(spawn->x), static_cast<double>(spawn->y),
                   static_cast<double>(spawn->z)};
    double yaw = spawn->angles[2];
    if (options.has_third_pos) {
        feet = {options.third_pos[0], options.third_pos[1], options.third_pos[2]};
    }
    if (options.has_third_yaw) {
        yaw = static_cast<double>(options.third_yaw) * 0.017453292519943295;
    }
    const double model_scale = static_cast<double>(options.player_scale);
    const double ground_offset =
        options.player_origin ? 0.0 : min_z * model_scale;
    const gm::Vec3d pos{feet.x, feet.y, feet.z - ground_offset};
    const double cy = std::cos(yaw), sy = std::sin(yaw);

    // Place feet-on-spawn, then turn the chassis to the spawn yaw.  The UV
    // frame (`local`, face origin/u/v) is left untouched, so the texture turns
    // with the world-space vertices; only the lighting normal is rotated.
    auto placed = gm::place_model_meshes(*model, pos, model_scale);
    for (auto& mesh : placed) {
        for (auto& vertex : mesh.vertices) {
            const double dx = vertex.x - pos.x;
            const double dy = vertex.y - pos.y;
            vertex.x = pos.x + dx * cy + dy * sy;
            vertex.y = pos.y - dx * sy + dy * cy;
        }
        for (auto& face : mesh.faces) {
            const double nx = face.normal.x;
            const double ny = face.normal.y;
            face.normal.x = static_cast<float>(nx * cy + ny * sy);
            face.normal.y = static_cast<float>(-nx * sy + ny * cy);
        }
        mesh.aabb_lo = {1e30, 1e30, 1e30};
        mesh.aabb_hi = {-1e30, -1e30, -1e30};
        for (const auto& vertex : mesh.vertices) {
            mesh.aabb_lo = {std::min(mesh.aabb_lo.x, vertex.x),
                            std::min(mesh.aabb_lo.y, vertex.y),
                            std::min(mesh.aabb_lo.z, vertex.z)};
            mesh.aabb_hi = {std::max(mesh.aabb_hi.x, vertex.x),
                            std::max(mesh.aabb_hi.y, vertex.y),
                            std::max(mesh.aabb_hi.z, vertex.z)};
        }
    }
    meshes.insert(meshes.end(), std::make_move_iterator(placed.begin()),
                  std::make_move_iterator(placed.end()));

    gm::ControllerConfig player_config;
    player_config.eye_height = options.spawn_eye;
    const gm::PlayerState player = gm::make_state(feet, yaw, player_config);

    gm::CameraConfig config;
    config.focus_distance = static_cast<double>(options.third_distance) + 100.0;
    config.third_distance = options.third_distance;
    config.third_height = options.third_height;
    config.third_radius = 1.0;
    config.fov = static_cast<double>(options.fov) * 0.017453292519943295;
    config.aspect = options.height > 0
                        ? static_cast<double>(options.width) / options.height
                        : 4.0 / 3.0;
    gm::CameraRig rig(config);
    rig.set_mode(gm::CameraMode::Third);

    gm::CollisionWorld collision;
    if (level.world) {
        std::string build_error;
        if (collision.build(*level.world, &build_error)) {
            rig.set_collision_world(&collision);
        } else {
            gm::logf(gm::LogLevel::Warn, "third-person", "collision: ", build_error);
        }
    }

    const auto update = rig.update(player, {}, 1.0 / 91.0);
    if (!update.ok) {
        error = "third-person: " + update.error;
        return false;
    }
    eye = rig.eye();
    target = rig.target();
    gm::logf(gm::LogLevel::Info, "third-person", "model=", options.player_model,
             " feet=", feet.x, ",", feet.y, ",", feet.z, " yaw=", yaw,
             " source=", options.has_third_pos ? "override" : "spawn",
             " scale=", model_scale, " origin=", options.player_origin ? 1 : 0,
             " distance=", rig.state().distance,
             " occluded=", rig.state().occluded ? 1 : 0, " eye=", eye.x, ",",
             eye.y, ",", eye.z);
    return true;
}

int show_film(const gm::Options& options) {
    std::string error;
    std::string name = options.film;
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".sof") != 0) {
        name += ".sof";
    }
    auto path = gm::resolve_ci(options.game / "Film", name);
    if (!path) {
        path = gm::resolve_ci(options.game, name);
    }
    if (!path) {
        gm::logf(gm::LogLevel::Error, "film", "not found: " + name);
        return 2;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        gm::logf(gm::LogLevel::Error, "film", error);
        return 2;
    }
    const auto film = gm::parse_sof(*bytes, error);
    if (!film) {
        gm::logf(gm::LogLevel::Error, "film", error);
        return 2;
    }
    std::printf("film: %s\n", path->filename().string().c_str());
    std::printf("bytes: %zu tiles: %zu tail: %zu\n", bytes->size(), film->tiles(),
                film->tail());
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto frame =
        film->frame(static_cast<std::size_t>(std::max(0, options.frame)));
    if (frame) {
        for (std::uint8_t b : *frame) {
            h ^= b;
            h *= 0x100000001b3ull;
        }
        std::printf("frame %d: %zu bytes fnv1a64=%016llx\n", options.frame, frame->size(),
                    static_cast<unsigned long long>(h));
        if (!options.png.empty()) {
            // Every film is indexed by the shared VGA table resolved in
            // Film/Kflm0000.bmp (docs/FILM.md proves 30/30 frames equal it).
            const auto pal_path = gm::resolve_ci(path->parent_path(), "Kflm0000.bmp");
            const auto pal_bytes = pal_path ? read_file(*pal_path, error) : std::nullopt;
            const auto pal = pal_bytes ? gm::parse_bmp(*pal_bytes, error)
                                       : std::optional<gm::BmpImage>{};
            if (!pal || pal->palette.size() < 256 * 4) {
                gm::logf(gm::LogLevel::Error, "film", "palette not found: ",
                         pal_path ? pal_path->string() : "Kflm0000.bmp");
                return 2;
            }
            std::vector<std::uint8_t> rgba(frame->size() * 4);
            for (std::size_t i = 0; i < frame->size(); ++i) {
                const std::uint8_t idx = (*frame)[i];
                rgba[i * 4 + 0] = pal->palette[idx * 4 + 0];
                rgba[i * 4 + 1] = pal->palette[idx * 4 + 1];
                rgba[i * 4 + 2] = pal->palette[idx * 4 + 2];
                rgba[i * 4 + 3] = 255;
            }
            const auto png = gm::encode_png_rgba(gm::kSofTileWidth, gm::kSofTileHeight,
                                                 rgba);
            if (!write_file(options.png, png)) {
                gm::logf(gm::LogLevel::Error, "film", "cannot write ",
                         options.png.string());
                return 2;
            }
            std::printf("png: %s (%dx%d)\n", options.png.string().c_str(),
                        gm::kSofTileWidth, gm::kSofTileHeight);
        }
    } else {
        std::printf("frame %d: out of range\n", options.frame);
    }
    std::string stem = path->stem().string();
    std::string letters;
    for (char c : stem) {
        if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
            letters.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!letters.empty()) {
        if (const auto fdx_path = gm::resolve_ci(path->parent_path(), letters + ".fdx")) {
            const auto fdx_bytes = read_file(*fdx_path, error);
            if (const auto fdx = fdx_bytes ? gm::parse_fdx(*fdx_bytes, error)
                                           : std::optional<gm::Fdx>{}) {
                std::printf("fdx: %s entries: %zu", fdx_path->filename().string().c_str(),
                            fdx->entries.size());
                const int index = static_cast<int>(path->stem().string().back() - '0');
                if (index >= 0 && static_cast<std::size_t>(index) < fdx->entries.size()) {
                    const auto& e = fdx->entries[static_cast<std::size_t>(index)];
                    std::printf(" line %d: count=%u fps=%u", index, e.count, e.fps);
                }
                std::printf("\n");
            }
        }
    }
    return 0;
}

int show_scene(const gm::Options& options) {
    std::string error;
    auto path = gm::resolve_ci(options.game, options.scene + ".sbt");
    if (!path) {
        gm::logf(gm::LogLevel::Error, "scene", ".sbt not found: " + options.scene);
        return 2;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        gm::logf(gm::LogLevel::Error, "scene", error);
        return 2;
    }
    const auto level = gm::parse_sbt(*bytes, error);
    if (!level) {
        gm::logf(gm::LogLevel::Error, "scene", error);
        return 2;
    }
    const auto scene = gm::extract_scene(*level, error);
    if (!scene) {
        gm::logf(gm::LogLevel::Error, "scene", error);
        return 2;
    }
    std::size_t sky = 0;
    for (const auto& light : scene->lights) {
        sky += light.kind == "sky" ? 1 : 0;
    }
    std::printf("scene: %s\n", path->filename().string().c_str());
    std::printf("lights: %zu (sky %zu, local %zu)\n", scene->lights.size(), sky,
                scene->lights.size() - sky);
    std::printf("locations: %zu items: %zu sounds: %zu sprites: %zu\n",
                scene->locations.size(), scene->items.size(), scene->sounds.size(),
                scene->sprites.size());
    std::printf("doors: %zu switches: %zu entities: %zu\n", scene->doors.size(),
                scene->switches.size(), scene->entities.size());
    std::size_t placed = 0;
    std::size_t modelled = 0;
    for (const auto& thing : scene->things) {
        placed += thing.absolute ? 1 : 0;
        modelled += thing.model.empty() ? 0 : 1;
    }
    std::printf("things: %zu (absolute %zu, models %zu)\n", scene->things.size(),
                placed, modelled);
    std::size_t campaign = 0;
    std::size_t deathmatch = 0;
    for (const auto& spawn : scene->spawns) {
        if (spawn.deathmatch) {
            ++deathmatch;
        } else {
            ++campaign;
        }
    }
    std::printf("spawns: %zu (campaign %zu, deathmatch %zu)\n", scene->spawns.size(),
                campaign, deathmatch);
    for (const auto& spawn : scene->spawns) {
        if (!spawn.deathmatch && spawn.slot == 0) {
            std::printf("start: slot 0 at %d,%d,%d yaw=%.3f eye=+10 (LIGHT)\n", spawn.x,
                        spawn.y, spawn.z, spawn.angles[2]);
        }
    }
    for (const auto& [name, count] : scene->other) {
        std::printf("other: %s %d\n", name.c_str(), count);
    }
    return 0;
}

std::vector<std::uint8_t> make_wav(const gm::PcmClip& clip) {
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(clip.samples.size() * 2);
    const std::uint16_t channels = static_cast<std::uint16_t>(std::max(1, clip.channels));
    const std::uint32_t rate = static_cast<std::uint32_t>(std::max(1, clip.sample_rate));
    const std::uint16_t block = static_cast<std::uint16_t>(channels * 2);
    std::vector<std::uint8_t> out;
    out.reserve(44 + data_bytes);
    auto put32 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 24));
    };
    auto put16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto tag = [&](const char* s) { out.insert(out.end(), s, s + 4); };
    tag("RIFF");
    put32(36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1); // PCM
    put16(channels);
    put32(rate);
    put32(rate * block);
    put16(block);
    put16(16);
    tag("data");
    put32(data_bytes);
    for (std::int16_t sample : clip.samples) {
        put16(static_cast<std::uint16_t>(sample));
    }
    return out;
}

int export_sou1(const gm::Options& options) {
    std::string error;
    if (options.wav.empty()) {
        gm::logf(gm::LogLevel::Error, "sou1", "--sou1 needs --wav OUT");
        return 2;
    }
    const gm::BigEntry* found = nullptr;
    std::optional<gm::BigFile> archive;
    for (const char* sub : {"sounds", "MUSIC"}) {
        const auto idx = gm::resolve_ci(options.game / sub, "SOU.IDX");
        if (!idx) {
            continue;
        }
        auto file = gm::BigFile::open(*idx, error);
        if (!file) {
            continue;
        }
        for (const auto& entry : file->entries()) {
            if (gm::iequals_ascii(entry.name, options.sou1)) {
                found = &entry;
                archive = std::move(*file);
                break;
            }
        }
        if (found != nullptr) {
            break;
        }
    }
    if (found == nullptr || !archive) {
        gm::logf(gm::LogLevel::Error, "sou1", "entry not found: " + options.sou1);
        return 2;
    }
    const auto blob = archive->read(*found, error);
    if (!blob) {
        gm::logf(gm::LogLevel::Error, "sou1", error);
        return 2;
    }
    const auto info = gm::parse_sou1_header(*blob, error);
    const auto wav = gm::sou1_to_wav(*blob, error);
    if (!info || !wav) {
        gm::logf(gm::LogLevel::Error, "sou1", error);
        return 2;
    }
    if (!write_file(options.wav, *wav)) {
        gm::logf(gm::LogLevel::Error, "sou1", "cannot write " + options.wav.string());
        return 2;
    }
    const double seconds = static_cast<double>(wav->size() - 44) /
                           (2.0 * info->channels * info->sample_rate);
    std::printf("sou1: %s\n", found->name.c_str());
    std::printf("format: %u Hz, %d ch, %.3f s\n", info->sample_rate,
                info->channels, seconds);
    std::printf("wav: %s (%zu bytes)\n", options.wav.string().c_str(), wav->size());
    return 0;
}

int export_sou2(const gm::Options& options) {
    std::string error;
    if (options.wav.empty()) {
        gm::logf(gm::LogLevel::Error, "sou2", "--sou2 needs --wav OUT");
        return 2;
    }
    const gm::BigEntry* found = nullptr;
    std::optional<gm::BigFile> archive;
    for (const char* sub : {"MUSIC", "sounds"}) {
        const auto idx = gm::resolve_ci(options.game / sub, "SOU.IDX");
        if (!idx) {
            continue;
        }
        auto file = gm::BigFile::open(*idx, error);
        if (!file) {
            continue;
        }
        for (const auto& entry : file->entries()) {
            if (gm::iequals_ascii(entry.name, options.sou2)) {
                found = &entry;
                archive = std::move(*file);
                break;
            }
        }
        if (found != nullptr) {
            break;
        }
    }
    if (found == nullptr || !archive) {
        gm::logf(gm::LogLevel::Error, "sou2", "entry not found: " + options.sou2);
        return 2;
    }
    const auto blob = archive->read(*found, error);
    if (!blob) {
        gm::logf(gm::LogLevel::Error, "sou2", error);
        return 2;
    }
    const auto clip = gm::decode_sou2(*blob, error);
    if (!clip) {
        gm::logf(gm::LogLevel::Error, "sou2", error);
        return 2;
    }
    const auto wav = make_wav(*clip);
    if (!write_file(options.wav, wav)) {
        gm::logf(gm::LogLevel::Error, "sou2", "cannot write " + options.wav.string());
        return 2;
    }
    const double seconds =
        clip->sample_rate > 0 && clip->channels > 0
            ? static_cast<double>(clip->samples.size()) / clip->channels / clip->sample_rate
            : 0.0;
    std::printf("sou2: %s\n", found->name.c_str());
    std::printf("format: %d Hz, %d ch, %zu samples (%.3f s)\n", clip->sample_rate,
                clip->channels, clip->samples.size(), seconds);
    std::printf("wav: %s (%zu bytes)\n", options.wav.string().c_str(), wav.size());
    return 0;
}

int play_music(const gm::Options& options) {
    std::string error;
    auto sog_path = gm::resolve_ci(options.game / "MUSIC", options.music + ".sog");
    if (!sog_path) {
        gm::logf(gm::LogLevel::Error, "music", "playlist not found: " + options.music);
        return 2;
    }
    const auto sog_bytes = read_file(*sog_path, error);
    if (!sog_bytes) {
        gm::logf(gm::LogLevel::Error, "music", error);
        return 2;
    }
    const auto sog = gm::parse_sog(*sog_bytes, error);
    if (!sog) {
        gm::logf(gm::LogLevel::Error, "music", error);
        return 2;
    }
    auto idx = gm::resolve_ci(options.game / "MUSIC", "SOU.IDX");
    if (!idx) {
        gm::logf(gm::LogLevel::Error, "music", "MUSIC/SOU.IDX not found");
        return 2;
    }
    const auto archive = gm::BigFile::open(*idx, error);
    if (!archive) {
        gm::logf(gm::LogLevel::Error, "music", error);
        return 2;
    }

    std::vector<gm::PcmClip> clips;
    std::uint64_t total_samples = 0;
    for (const auto& name : sog->names) {
        const gm::BigEntry* entry = nullptr;
        for (const auto& candidate : archive->entries()) {
            if (gm::iequals_ascii(candidate.name, name)) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            gm::logf(gm::LogLevel::Error, "music", "missing entry " + name);
            return 2;
        }
        const auto blob = archive->read(*entry, error);
        if (!blob) {
            gm::logf(gm::LogLevel::Error, "music", error);
            return 2;
        }
        auto clip = gm::decode_sou2(*blob, error);
        if (!clip) {
            gm::logf(gm::LogLevel::Error, "music", name + ": " + error);
            return 2;
        }
        total_samples += clip->samples.size();
        clips.push_back(std::move(*clip));
    }
    if (clips.empty()) {
        gm::logf(gm::LogLevel::Error, "music", "playlist is empty");
        return 2;
    }
    const int rate = clips.front().sample_rate;
    const int channels = clips.front().channels;
    const double seconds = rate > 0 && channels > 0
                               ? static_cast<double>(total_samples) / channels / rate
                               : 0.0;
    gm::logf(gm::LogLevel::Info, "music", sog_path->filename().string(), " tracks=",
             clips.size(), " ", rate, " Hz ", channels, " ch ", seconds, "s");

    // `--fade-in/--fade-out`: the envelope of `audio/music.*` is applied to the
    // decoded PCM offline (the model is linear, `docs/MUSIC.md` §3).
    const double fade_in = options.music_fade_in;
    const double fade_out = options.music_fade_out;
    std::vector<std::int16_t> rendered;
    double gain_first = 1.0;
    double gain_mid = 1.0;
    double gain_last = 1.0;
    if (fade_in > 0.0 || fade_out > 0.0) {
        const int channels_safe = std::max(1, channels);
        bool first_clip = true;
        for (const gm::PcmClip& clip : clips) {
            gm::audio::MusicTrack track;
            std::string track_error;
            if (!track.load_at(clip, clip.sample_rate, rate, &track_error)) {
                gm::logf(gm::LogLevel::Error, "music", track_error);
                return 2;
            }
            track.play();
            if (fade_in > 0.0 && first_clip) {
                track.fade_in(fade_in);
            }
            const int frames = static_cast<int>(clip.samples.size() / channels_safe);
            std::vector<float> buffer(1024 * channels_safe);
            int done = 0;
            while (done < frames) {
                const int chunk = std::min(1024, frames - done);
                if (fade_out > 0.0) {
                    const double left = static_cast<double>(frames - done) / rate;
                    if (left <= fade_out) {
                        track.fade_out(left);
                    }
                }
                const int got = track.mix_into(
                    std::span<float>(buffer.data(),
                                     static_cast<std::size_t>(chunk) * channels_safe),
                    channels_safe);
                if (got <= 0) {
                    break;
                }
                for (int i = 0; i < got * channels_safe; ++i) {
                    const double value = std::clamp<double>(buffer[i], -1.0, 1.0) * 32767.0;
                    rendered.push_back(static_cast<std::int16_t>(std::lround(value)));
                }
                if (first_clip && done == 0) {
                    gain_first = track.gain();
                }
                if (first_clip && done <= frames / 2 && done + got > frames / 2) {
                    gain_mid = track.gain();
                }
                gain_last = track.gain();
                // `mix_into()` already advanced the clock (`docs/MUSIC.md` §4).
                done += got;
            }
            first_clip = false;
        }
    }

    if (options.headless) {
        std::printf("music: %zu tracks, %.3f s\n", clips.size(), seconds);
        if (!rendered.empty()) {
            std::printf("music: fade-in=%.3f fade-out=%.3f gain first=%.6f mid=%.6f "
                        "last=%.6f frames=%zu\n", fade_in, fade_out, gain_first,
                        gain_mid, gain_last, rendered.size() / std::max(1, channels));
        }
        return 0;
    }
    auto device = gm::AudioDevice::open(rate, channels, false, error);
    if (!device) {
        gm::logf(gm::LogLevel::Error, "music", error);
        return 1;
    }
    auto window = gm::Window::open("Gunmetal", options.width, options.height, false, error);
    if (!window) {
        gm::logf(gm::LogLevel::Error, "music", error);
        return 1;
    }
    if (!rendered.empty()) {
        device->queue(rendered.data(), rendered.size() / std::max(1, channels));
    } else {
        for (const auto& clip : clips) {
            device->queue(clip.samples.data(),
                          clip.samples.size() / std::max(1, clip.channels));
        }
    }
    gm::FrameClock clock;
    int frame = 0;
    while (window->pump()) {
        window->fill_gradient(frame);
        window->present();
        (void)clock.tick();
        ++frame;
        if (clock.elapsed() > seconds + 0.2) {
            break;
        }
        window->delay_ms(10);
    }
    return 0;
}

int show_song(const gm::Options& options) {
    std::string error;
    auto path = gm::resolve_ci(options.game / "MUSIC", options.song + ".sog");
    if (!path) {
        gm::logf(gm::LogLevel::Error, "song", "playlist not found: " + options.song);
        return 2;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        gm::logf(gm::LogLevel::Error, "song", error);
        return 2;
    }
    const auto sog = gm::parse_sog(*bytes, error);
    if (!sog) {
        gm::logf(gm::LogLevel::Error, "song", error);
        return 2;
    }
    auto idx = gm::resolve_ci(options.game / "MUSIC", "SOU.IDX");
    if (!idx) {
        gm::logf(gm::LogLevel::Error, "song", "MUSIC/SOU.IDX not found");
        return 2;
    }
    const auto archive = gm::BigFile::open(*idx, error);
    if (!archive) {
        gm::logf(gm::LogLevel::Error, "song", error);
        return 2;
    }

    std::printf("song: %s\n", path->filename().string().c_str());
    std::printf("tracks: %zu\n", sog->names.size());
    std::uint64_t total = 0;
    std::size_t missing = 0;
    for (const auto& name : sog->names) {
        const gm::BigEntry* found = nullptr;
        for (const auto& entry : archive->entries()) {
            if (gm::iequals_ascii(entry.name, name)) {
                found = &entry;
                break;
            }
        }
        if (found == nullptr) {
            std::printf("  %s missing\n", name.c_str());
            ++missing;
            continue;
        }
        total += found->size;
        std::printf("  %s %u bytes\n", name.c_str(), found->size);
    }
    std::printf("total: %llu bytes, %zu missing\n",
                static_cast<unsigned long long>(total), missing);
    return missing == 0 ? 0 : 1;
}

int play_video(const gm::Options& options) {
    std::string error;
    std::string name = options.play;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".sbv") {
        name += ".sbv";
    }
    auto path = gm::resolve_ci(options.game / "Vid", name);
    if (!path) {
        path = gm::resolve_ci(options.game, name);
    }
    if (!path) {
        gm::logf(gm::LogLevel::Error, "video", "clip not found: " + name);
        return 2;
    }
    const auto bytes = read_file(*path, error);
    if (!bytes) {
        gm::logf(gm::LogLevel::Error, "video", error);
        return 2;
    }
    const auto clip = gm::parse_sbv(*bytes, error);
    if (!clip) {
        gm::logf(gm::LogLevel::Error, "video", error);
        return 2;
    }

    auto window = gm::Window::open("Gunmetal", options.width, options.height, options.headless,
                                   error);
    if (!window) {
        gm::logf(gm::LogLevel::Error, "video", error);
        return 1;
    }
    gm::logf(gm::LogLevel::Info, "video", "clip=", path->filename().string(), " ",
             clip->width, "x", clip->height, " frames=", clip->frame_count);

    gm::FrameClock clock;
    std::size_t next = 0;
    int frame = 0;
    std::uint32_t previous_end = 0;
    const std::uint32_t first_ts = clip->table.empty() ? 0 : clip->table.front().timestamp_ms;
    while (window->pump()) {
        if (next >= clip->table.size()) {
            break;
        }
        const std::uint32_t ts = clip->table[next].timestamp_ms;
        if (!window->headless()) {
            const double now_ms = clock.elapsed() * 1000.0;
            if (now_ms + 0.5 < static_cast<double>(ts - first_ts)) {
                window->delay_ms(1);
                continue;
            }
        }
        const std::size_t begin = clip->data_start + previous_end;
        const std::size_t end = clip->data_start + clip->table[next].end_offset;
        previous_end = clip->table[next].end_offset;
        if (end > bytes->size() || begin > end) {
            gm::logf(gm::LogLevel::Error, "video", "frame ", next, " outside the file");
            return 2;
        }
        const auto decoded = gm::decode_sbv_frame(
            std::span<const std::uint8_t>(bytes->data() + begin, end - begin), clip->width,
            clip->height, error);
        if (!decoded) {
            gm::logf(gm::LogLevel::Error, "video", "frame ", next, ": ", error);
            return 2;
        }
        window->blit_rgb(decoded->rgb.data(), decoded->width, decoded->height);
        window->present();
        ++next;
        ++frame;
        if (options.frames >= 0 && frame >= options.frames) {
            break;
        }
    }
    gm::logf(gm::LogLevel::Info, "run", "frames=", frame, " clip frames=",
             clip->frame_count);
    return 0;
}

int run(const gm::Options& options) {
    std::string error;

    if (!options.play.empty()) {
        return play_video(options);
    }

    std::optional<gm::LevelBundle> level;
    std::vector<gm::RenderMesh> meshes;
    const gm::Sbc* lightmaps = nullptr;
    if (!options.level.empty()) {
        level = gm::load_level(options.game, options.level, error);
        if (!level || !level->world) {
            gm::logf(gm::LogLevel::Error, "run", error);
            return 2;
        }
        lightmaps = level->lightmaps ? &*level->lightmaps : nullptr;
        meshes = gm::build_level_meshes(*level->world, lightmaps);

        if (options.things && level->entities) {
            std::string scene_error;
            const auto scene = gm::extract_scene(*level->entities, scene_error);
            if (!scene) {
                gm::logf(gm::LogLevel::Error, "things", scene_error);
            } else {
                std::map<std::string, std::optional<gm::Sob>> cache;
                std::size_t placed = 0;
                for (const auto& thing : scene->things) {
                    if (!thing.absolute || thing.model.empty()) {
                        continue;
                    }
                    auto it = cache.find(thing.model);
                    if (it == cache.end()) {
                        std::optional<gm::Sob> model;
                        const auto path = gm::resolve_ci(options.game / "SOBS",
                                                         thing.model + ".sob");
                        if (path) {
                            std::string read_error;
                            if (const auto bytes = read_file(*path, read_error)) {
                                std::string parse_error;
                                model = gm::parse_sob(*bytes, parse_error);
                                if (!model) {
                                    gm::logf(gm::LogLevel::Error, "things", thing.model,
                                             ": ", parse_error);
                                }
                            } else {
                                gm::logf(gm::LogLevel::Error, "things", read_error);
                            }
                        }
                        it = cache.emplace(thing.model, std::move(model)).first;
                    }
                    if (!it->second) {
                        continue;
                    }
                    // gm/scene.py and the C++ .sbw renderer both use the
                    // engine world frame (`origin + basis * local`, basis =
                    // identity, docs/WORLD_FIELDS.md §3).  Do not rescale
                    // ADD_THING positions: they are already world.
                    auto placed_meshes = gm::place_model_meshes(
                        *it->second,
                        {static_cast<double>(thing.x), static_cast<double>(thing.y),
                         static_cast<double>(thing.z)},
                        options.thing_scale);
                    meshes.insert(meshes.end(),
                                  std::make_move_iterator(placed_meshes.begin()),
                                  std::make_move_iterator(placed_meshes.end()));
                    ++placed;
                }
                gm::logf(gm::LogLevel::Info, "things", "placed=", placed,
                         " models=", cache.size());
            }
        }
    }

    gm::Vec3d third_eye{};
    gm::Vec3d third_target{};
    bool third_ready = false;
    if (options.third_person) {
        if (!level || !level->world) {
            gm::logf(gm::LogLevel::Error, "third-person",
                     "--third-person needs --level NAME");
            return 2;
        }
        if (!third_person_camera(options, *level, meshes, third_eye, third_target,
                                 error)) {
            gm::logf(gm::LogLevel::Error, "third-person", error);
            return 2;
        }
        third_ready = true;
    }

    std::optional<gm::HudFilm> hud_film;
    std::vector<std::uint8_t> hud_palette;
    if (!options.hud.empty()) {
        hud_film = gm::load_hud_film(options.game, options.hud, error);
        if (!hud_film) {
            gm::logf(gm::LogLevel::Error, "hud", error);
            return 2;
        }
        const auto palette = gm::installed_palette(options.game, error);
        if (!palette) {
            gm::logf(gm::LogLevel::Error, "hud", error);
            return 2;
        }
        hud_palette = *palette;
    }

    auto window = gm::Window::open("Gunmetal", options.width, options.height, options.headless,
                                   error);
    if (!window) {
        gm::logf(gm::LogLevel::Error, "video", error);
        return 1;
    }
    gm::logf(gm::LogLevel::Info, "run", "game=", options.game.string(),
             options.headless ? " headless" : "", level ? " level=" + level->name : "");

    gm::TexturePool pool(options.game);

    // Free camera: start at --pos/--look when given, otherwise frame the level
    // the same way the auto camera does.
    gm::Vec3d eye{0.0, 0.0, 0.0};
    gm::Vec3d target{0.0, 0.0, 0.0};
    double radius = 1.0;
    if (!meshes.empty()) {
        gm::Vec3d lo{1e30, 1e30, 1e30};
        gm::Vec3d hi{-1e30, -1e30, -1e30};
        for (const auto& mesh : meshes) {
            for (const auto& v : mesh.vertices) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
        }
        const gm::Vec3d center{(lo.x + hi.x) / 2.0, (lo.y + hi.y) / 2.0, (lo.z + hi.z) / 2.0};
        const double dx = hi.x - lo.x;
        const double dy = hi.y - lo.y;
        const double dz = hi.z - lo.z;
        radius = std::max(1e-6, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
        if (options.has_pos && options.has_look) {
            eye = {options.pos[0], options.pos[1], options.pos[2]};
            target = {options.look[0], options.look[1], options.look[2]};
        } else if (third_ready) {
            eye = third_eye;
            target = third_target;
        } else if (options.spawn_camera && level && level->entities) {
            std::string spawn_error;
            if (!start_camera(options, *level->entities, eye, target, spawn_error)) {
                gm::logf(gm::LogLevel::Error, "run", spawn_error);
                return 2;
            }
        } else {
            const double a = options.az * 0.017453292519943295;
            const double e = options.el * 0.017453292519943295;
            const gm::Vec3d dir{std::cos(e) * std::cos(a), std::cos(e) * std::sin(a),
                                std::sin(e)};
            const double dist = radius / std::sin(options.fov * 0.017453292519943295 / 2.0) /
                                std::max(1e-3f, options.zoom);
            eye = {center.x + dir.x * dist, center.y + dir.y * dist, center.z + dir.z * dist};
            target = center;
        }
    }

    gm::FrameClock clock;
    int frame = 0;
    while (window->pump()) {
        const double dt = clock.tick();
        const double turn = 70.0 * dt;
        gm::Vec3d forward{target.x - eye.x, target.y - eye.y, target.z - eye.z};
        const double flen = std::sqrt(forward.x * forward.x + forward.y * forward.y +
                                      forward.z * forward.z);
        if (flen > 1e-9) {
            forward = {forward.x / flen, forward.y / flen, forward.z / flen};
        }
        const gm::Vec3d right{forward.y, -forward.x, 0.0};
        const double rlen = std::sqrt(right.x * right.x + right.y * right.y);
        const gm::Vec3d strafe = rlen > 1e-9 ? gm::Vec3d{right.x / rlen, right.y / rlen, 0.0}
                                             : gm::Vec3d{1.0, 0.0, 0.0};

        if (window->key_down(gm::Key::Left)) {
            const double a = -turn * 0.017453292519943295;
            const double c = std::cos(a);
            const double s = std::sin(a);
            const double x = forward.x * c - forward.y * s;
            const double y = forward.x * s + forward.y * c;
            forward = {x, y, forward.z};
            target = {eye.x + forward.x * flen, eye.y + forward.y * flen,
                      eye.z + forward.z * flen};
        }
        if (window->key_down(gm::Key::Right)) {
            const double a = turn * 0.017453292519943295;
            const double c = std::cos(a);
            const double s = std::sin(a);
            const double x = forward.x * c - forward.y * s;
            const double y = forward.x * s + forward.y * c;
            forward = {x, y, forward.z};
            target = {eye.x + forward.x * flen, eye.y + forward.y * flen,
                      eye.z + forward.z * flen};
        }
        if (window->key_down(gm::Key::Up) || window->key_down(gm::Key::W)) {
            const double step = radius * 0.5 * dt;
            eye = {eye.x + forward.x * step, eye.y + forward.y * step, eye.z + forward.z * step};
            target = {target.x + forward.x * step, target.y + forward.y * step,
                      target.z + forward.z * step};
        }
        if (window->key_down(gm::Key::Down) || window->key_down(gm::Key::S)) {
            const double step = radius * 0.5 * dt;
            eye = {eye.x - forward.x * step, eye.y - forward.y * step, eye.z - forward.z * step};
            target = {target.x - forward.x * step, target.y - forward.y * step,
                      target.z - forward.z * step};
        }
        if (window->key_down(gm::Key::A)) {
            const double step = radius * 0.5 * dt;
            eye = {eye.x - strafe.x * step, eye.y - strafe.y * step, eye.z};
            target = {target.x - strafe.x * step, target.y - strafe.y * step, target.z};
        }
        if (window->key_down(gm::Key::D)) {
            const double step = radius * 0.5 * dt;
            eye = {eye.x + strafe.x * step, eye.y + strafe.y * step, eye.z};
            target = {target.x + strafe.x * step, target.y + strafe.y * step, target.z};
        }
        if (window->key_down(gm::Key::Plus)) {
            radius = std::max(1e-3, radius * (1.0 - dt));
        }
        if (window->key_down(gm::Key::Minus)) {
            radius = std::max(1e-3, radius * (1.0 + dt));
        }

        if (!meshes.empty()) {
            gm::RenderOptions render;
            render.width = window->width();
            render.height = window->height();
            render.fov = options.fov;
            render.textures = !options.no_textures;
            render.lightmaps = !options.no_lightmaps;
            render.lightmap_world = options.lightmap_world;
            render.wire = options.wire;
            render.mip = options.mip;
            render.mip_auto = options.mip_auto;
            render.bilinear = options.bilinear;
            render.alpha_test = options.alpha_test;
            render.transparent_index = options.transparent_index;
            render.chromakey = options.chromakey;
            render.texel_uv = options.texel_uv;
            render.skip_untextured = options.skip_untextured;
            render.haze = options.haze;
            render.camera_override = true;
            render.eye = eye;
            render.target = target;
            auto image = gm::render_scene(meshes, render, &pool, lightmaps);
            if (hud_film) {
                std::string hud_error;
                if (!gm::draw(*hud_film, image, hud_palette, options.hud_x,
                              options.hud_y, true, options.hud_scale,
                              options.hud_opacity,
                              static_cast<std::uint64_t>(frame), hud_error)) {
                    gm::logf(gm::LogLevel::Error, "hud", hud_error);
                    hud_film.reset();
                }
            }
            window->blit_rgb(image.rgb.data(), image.width, image.height);
        } else {
            window->fill_gradient(frame);
        }
        window->present();
        ++frame;
        if (options.frames >= 0 && frame >= options.frames) {
            break;
        }
    }
    const double elapsed = clock.elapsed();
    const double fps = elapsed > 0.0 ? frame / elapsed : 0.0;
    gm::logf(gm::LogLevel::Info, "run", "frames=", frame, " elapsed=", elapsed, "s fps=", fps);
    return 0;
}

} // namespace


// `--menu`: the headless frontend (`docs/FRONTEND.md`).  No game files are
// needed: the screen table and the items are the §2/§4 snapshot.
int show_menu(const gm::Options& options) {
    gm::FrontendMenu menu;
    menu.reset();
    if (!options.menu_screen.empty()) {
        int id = -1;
        std::size_t used = 0;
        try {
            id = std::stoi(options.menu_screen, &used);
        } catch (const std::exception&) {
            gm::logf(gm::LogLevel::Error, "menu",
                     "--menu-screen wants a §2 screen id, got \"" +
                         options.menu_screen + "\"");
            return 2;
        }
        if (used != options.menu_screen.size()) {
            gm::logf(gm::LogLevel::Error, "menu",
                     "--menu-screen wants a §2 screen id, got \"" +
                         options.menu_screen + "\"");
            return 2;
        }
        gm::Screen target = gm::Screen::None;
        for (const gm::Screen screen : gm::FrontendMenu::screens()) {
            if (gm::FrontendMenu::screen_id(screen) == id) {
                target = screen;
                break;
            }
        }
        if (target == gm::Screen::None) {
            gm::logf(gm::LogLevel::Error, "menu",
                     "no §2 screen with id " + std::to_string(id));
            return 2;
        }
        menu.enter(target);
    }

    auto dump = [&options, &menu](const char* prefix) {
        const gm::Screen screen = menu.screen();
        if (options.menu_json) {
            std::printf("{\"stage\": \"%s\", \"screen\": %d, \"name\": \"%s\", "
                        "\"selected\": %d, \"exit\": %s, \"items\": [",
                        prefix, gm::FrontendMenu::screen_id(screen),
                        std::string(gm::FrontendMenu::screen_name(screen)).c_str(),
                        static_cast<int>(menu.selected()),
                        screen == gm::Screen::Exit ? "true" : "false");
            bool first = true;
            for (const gm::Item& item : menu.items()) {
                std::printf("%s{\"row\": %d, \"id\": %d, \"label\": \"%s\", "
                            "\"va\": \"%s\"}", first ? "" : ", ", item.row, item.id,
                            std::string(item.label).c_str(),
                            std::string(item.va).c_str());
                first = false;
            }
            std::printf("]}\n");
            return;
        }
        std::printf("%s: %s (id %d), selected=%d\n", prefix,
                    std::string(gm::FrontendMenu::screen_name(screen)).c_str(),
                    gm::FrontendMenu::screen_id(screen),
                    static_cast<int>(menu.selected()));
        for (const gm::Item& item : menu.items()) {
            // `va` is the doc's value; unresolved pointers stay hidden.
            const bool have_va = item.va != "-" && item.va.rfind("0x", 0) != 0;
            std::printf("  %2d %s %s%s%s\n", item.row,
                        item.selectable() ? "*" : " ", std::string(item.label).c_str(),
                        have_va ? " = " : "", have_va ? std::string(item.va).c_str() : "");
        }
    };

    if (options.menu_keys.empty()) {
        dump("menu");
        return 0;
    }
    dump("start");

    std::size_t begin = 0;
    while (begin <= options.menu_keys.size()) {
        std::size_t end = options.menu_keys.find(',', begin);
        if (end == std::string::npos) {
            end = options.menu_keys.size();
        }
        const std::string token = options.menu_keys.substr(begin, end - begin);
        begin = end + 1;
        if (token.empty()) {
            gm::logf(gm::LogLevel::Error, "menu", "empty key in --menu-keys");
            return 2;
        }
        gm::KeyEvent event;
        if (token == "up") {
            event.kind = gm::KeyEvent::Up;
        } else if (token == "down") {
            event.kind = gm::KeyEvent::Down;
        } else if (token == "left") {
            event.kind = gm::KeyEvent::Left;
        } else if (token == "right") {
            event.kind = gm::KeyEvent::Right;
        } else if (token == "enter") {
            event.kind = gm::KeyEvent::Enter;
        } else if (token == "esc" || token == "escape") {
            event.kind = gm::KeyEvent::Escape;
        } else if (token == "back") {
            event.kind = gm::KeyEvent::Backspace;
        } else if (token.rfind("text:", 0) == 0) {
            const std::string hex = token.substr(5);
            char* tail = nullptr;
            const long value = std::strtol(hex.c_str(), &tail, 16);
            if (hex.empty() || tail == nullptr || *tail != '\0' || value < 0 ||
                value > 0xff) {
                gm::logf(gm::LogLevel::Error, "menu",
                         "bad text key \"" + token + "\" (text:0xNN)");
                return 2;
            }
            event.kind = gm::KeyEvent::Text;
            event.code = static_cast<int>(value);
        } else {
            gm::logf(gm::LogLevel::Error, "menu",
                     "unknown key \"" + token +
                         "\" (up, down, left, right, enter, esc, back, text:0xNN)");
            return 2;
        }
        const bool changed = menu.feed(event);
        if (options.menu_json) {
            std::printf("{\"stage\": \"key\", \"key\": \"%s\", \"changed\": %s, "
                        "\"screen\": %d, \"name\": \"%s\"}\n", token.c_str(),
                        changed ? "true" : "false",
                        gm::FrontendMenu::screen_id(menu.screen()),
                        std::string(gm::FrontendMenu::screen_name(menu.screen())).c_str());
        } else {
            std::printf("  key %-10s -> %s%s\n", token.c_str(),
                        std::string(gm::FrontendMenu::screen_name(menu.screen())).c_str(),
                        changed ? "" : " (ignored)");
        }
        if (menu.screen() == gm::Screen::Exit) {
            break;
        }
        if (begin > options.menu_keys.size()) {
            break;
        }
    }
    dump("final");
    return 0;
}


// `--pager`: the headless SML pager over `text_layout` pages
// (`docs/SML_PAGER.md`).  No window and no game files: the font metrics are
// table-driven.
int show_pager(const gm::Options& options) {
    // Atlases come from the install (`FRONTEND/BMP.IDX`); a bare name is
    // resolved case-insensitively under the game dir and then under `SML`
    // (`SmlPager::open`).
    gm::SmlPager pager(std::filesystem::path(options.game));
    std::string error;
    if (!pager.open(std::filesystem::path(options.pager), &error)) {
        gm::logf(gm::LogLevel::Error, "pager", error);
        return 2;
    }
    if (options.pager_page > 0) {
        if (!pager.show_page(options.pager_page - 1)) {
            gm::logf(gm::LogLevel::Error, "pager",
                     "no page " + std::to_string(options.pager_page) + " (1.." +
                         std::to_string(pager.page_count()) + ")");
            return 2;
        }
    }

    auto dump = [&pager](const char* prefix) {
        std::printf("%s: %s pages=%d lines=%d\n", prefix, pager.status().c_str(),
                    pager.page_count(), pager.line_count());
        const std::string& text = pager.document().text;
        int index = 0;
        for (const gm::LayoutLine& line : pager.lines()) {
            std::string shown;
            if (line.first < text.size() && line.last <= text.size() &&
                line.last > line.first) {
                shown = text.substr(line.first, line.last - line.first);
                for (char& c : shown) {
                    if (c == '\r' || c == '\n') {
                        c = ' ';
                    }
                }
            }
            std::printf("  line %2d x=%-3d y=%-3d w=%-3d h=%-2d font=%d  %s\n",
                        index, line.x, line.y, line.width, line.height, line.font,
                        shown.c_str());
            ++index;
        }
    };

    if (options.pager_all) {
        for (int page = 0; page < pager.page_count(); ++page) {
            pager.show_page(page);
            dump("page");
        }
        return 0;
    }
    dump("pager");

    if (options.pager_keys.empty()) {
        return 0;
    }
    std::size_t begin = 0;
    while (begin <= options.pager_keys.size()) {
        std::size_t end = options.pager_keys.find(',', begin);
        if (end == std::string::npos) {
            end = options.pager_keys.size();
        }
        const std::string token = options.pager_keys.substr(begin, end - begin);
        begin = end + 1;
        bool moved = false;
        if (token == "next") {
            moved = pager.next_page();
        } else if (token == "prev") {
            moved = pager.prev_page();
        } else if (token == "first") {
            moved = pager.first_page();
        } else if (token == "last") {
            moved = pager.last_page();
        } else if (token == "line") {
            moved = pager.scroll_next_line();
        } else if (token == "up") {
            moved = pager.scroll_prev_line();
        } else {
            gm::logf(gm::LogLevel::Error, "pager",
                     "unknown key \"" + token +
                         "\" (next, prev, first, last, line, up)");
            return 2;
        }
        std::printf("  key %-6s -> %s%s\n", token.c_str(), pager.status().c_str(),
                    moved ? "" : " (no change)");
    }
    dump("final");
    return 0;
}


// `--save-slots`: the headless save-slot table (`docs/SAVE_SLOTS.md`):
// 20 slots of `0x13` bytes, 6 visible rows, scroll 0..13.
int show_save_slots(const gm::Options& options) {
    gm::SaveSlotList slots;
    std::string error;
    slots.refresh(std::filesystem::path(options.game), &error);
    if (!error.empty()) {
        gm::logf(gm::LogLevel::Warn, "save-slots", error);
    }
    int present = 0;
    for (int index = 0; index < slots.count(); ++index) {
        if (slots.at(index).present) {
            ++present;
        }
    }
    std::printf("save-slots: present=%d/%d scroll=%d..%d selected=%d\n", present,
                slots.count(), slots.scroll(),
                slots.scroll() + slots.visible_rows() - 1, slots.selected());
    for (int row = 0; row < slots.visible_rows(); ++row) {
        const int index = slots.scroll() + row;
        const gm::SaveSlot& slot = slots.at(index);
        std::printf("  %2d %s %-19s level=%d mission=%d\n", index,
                    index == slots.selected() ? "*" : " ",
                    std::string(slots.item_label(row)).c_str(), slot.level,
                    slot.mission);
    }
    // Slot 19 is unreachable by design: `scroll + 6 < 20` stops at 13
    // (`0x48aa60`/`0x48abd0`), the model reproduces it instead of fixing it.
    return 0;
}

int main(int argc, char** argv) {
    gm::set_log_level_from_env();

    std::string error;
    const auto options = gm::parse_args(argc, argv, error);
    if (!options) {
        std::fprintf(stderr, "gunmetal: %s\n\n%s", error.c_str(), gm::usage().c_str());
        return 2;
    }
    if (options->show_version) {
        std::printf("gunmetal %s\n", gm::kVersion.data());
        return 0;
    }
    if (options->show_help) {
        std::fputs(gm::usage().c_str(), stdout);
        return 0;
    }
    if (options->verbose) {
        gm::set_log_level(gm::LogLevel::Debug);
    }
    // The headless menu needs no game files, so it runs before the `--game`
    // check (`M8`, `docs/FRONTEND.md`).
    if (options->menu) {
        return show_menu(*options);
    }
    if (!options->pager.empty()) {
        return show_pager(*options);
    }
    if (options->save_slots) {
        return show_save_slots(*options);
    }
    // M12.5: say *which* directory is missing instead of a bare ".sbw not
    // found": every data command joins names under `--game`.
    {
        std::error_code ec;
        if (!fs::is_directory(options->game, ec)) {
            std::fprintf(stderr,
                         "[error] game: directory not found: %s\n"
                         "hint: pass -g/--game DIR or set GUNMETAL_DIR; the "
                         "directory must be the Gunmetal 3dfx install "
                         "(gundx.exe and the data files)\n",
                         options->game.string().c_str());
            return 2;
        }
    }
    if (!options->music.empty()) {
        return play_music(*options);
    }
    if (!options->film.empty()) {
        return show_film(*options);
    }
    if (!options->scene.empty()) {
        return show_scene(*options);
    }
    if (!options->sim.empty()) {
        return simulate(*options);
    }
    if (!options->sou1.empty()) {
        return export_sou1(*options);
    }
    if (!options->sou2.empty()) {
        return export_sou2(*options);
    }
    if (!options->sample_play.empty()) {
        return play_sample(*options);
    }
    if (!options->sample.empty()) {
        return show_sample(*options);
    }
    if (!options->song.empty()) {
        return show_song(*options);
    }
    if (!options->play.empty()) {
        return play_video(*options);
    }
    if (!options->dump.empty()) {
        return dump_file(*options);
    }
    if (!options->render.empty()) {
        return render_image(*options);
    }
    if (!options->level.empty() && options->info) {
        return show_level(*options);
    }
    if (!options->png.empty()) {
        gm::logf(gm::LogLevel::Error, "png", "--png needs --dump FILE");
        return 2;
    }
    return run(*options);
}
