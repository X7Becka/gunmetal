#include "core/bytes.h"
#include "core/config.h"
#include "core/log.h"
#include "core/path.h"
#include "core/time.h"
#include "platform/window.h"
#include "res/bigfile.h"
#include "res/bmp.h"
#include "res/png.h"
#include "res/sbm.h"
#include "res/sbw.h"
#include "res/sou.h"
#include "res/sbt.h"
#include "res/sbc.h"
#include "res/sob.h"
#include "res/text.h"
#include "res/level.h"
#include "res/sbv.h"
#include "res/anim.h"
#include "render/render.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
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
        const auto soe = gm::parse_soe_text(*bytes, error);
        if (!soe) {
            gm::logf(gm::LogLevel::Error, "soe", error);
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
        const auto plain = encoded ? gm::sml_decode(*bytes) : *bytes;
        const std::string_view text(reinterpret_cast<const char*>(plain.data()), plain.size());
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
            std::printf("first: %s %d ms\n", soo->frames.front().name.c_str(),
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
        std::printf("mode: %d period: %d loop: %d\n", soa->mode, soa->period, soa->loop);
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

int render_image(const gm::Options& options) {
    std::string error;
    std::vector<gm::RenderMesh> meshes;
    const gm::Sbc* lightmaps = nullptr;

    std::optional<gm::LevelBundle> level;
    std::optional<gm::Sob> model;
    if (!options.model.empty()) {
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
    if (options.has_pos != options.has_look) {
        gm::logf(gm::LogLevel::Error, "render", "--pos and --look must be given together");
        return 2;
    }
    if (options.has_pos) {
        render.camera_override = true;
        render.eye = {options.pos[0], options.pos[1], options.pos[2]};
        render.target = {options.look[0], options.look[1], options.look[2]};
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
    const auto image = gm::render_scene(meshes, render, &pool, lightmaps);

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

int run(const gm::Options& options) {
    std::string error;

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
    double az = options.az;
    double el = options.el;
    double zoom = options.zoom;

    gm::FrameClock clock;
    int frame = 0;
    while (window->pump()) {
        const double dt = clock.tick();
        const double turn = 70.0 * dt;
        if (window->key_down(gm::Key::Left)) {
            az -= turn;
        }
        if (window->key_down(gm::Key::Right)) {
            az += turn;
        }
        if (window->key_down(gm::Key::Up)) {
            el = std::min(89.0, el + turn);
        }
        if (window->key_down(gm::Key::Down)) {
            el = std::max(-89.0, el - turn);
        }
        if (window->key_down(gm::Key::Plus)) {
            zoom *= 1.0 + 1.5 * dt;
        }
        if (window->key_down(gm::Key::Minus)) {
            zoom = std::max(0.1, zoom / (1.0 + 1.5 * dt));
        }

        if (!meshes.empty()) {
            gm::RenderOptions render;
            render.width = window->width();
            render.height = window->height();
            render.az = static_cast<float>(az);
            render.el = static_cast<float>(el);
            render.zoom = static_cast<float>(zoom);
            render.fov = options.fov;
            render.textures = !options.no_textures;
            render.lightmaps = !options.no_lightmaps;
            render.lightmap_world = options.lightmap_world;
            render.wire = options.wire;
            const auto image = gm::render_scene(meshes, render, &pool, lightmaps);
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
