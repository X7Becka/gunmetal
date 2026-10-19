#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

struct SbtRecord {
    std::uint8_t type = 0;
    std::string name;
    std::size_t offset = 0; // includes the type byte
    std::size_t size = 0;   // includes the type byte
    std::vector<std::uint8_t> payload;
};

struct SbtHandle {
    std::uint32_t tag = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
};

struct SbtSob {
    std::string name;
    std::uint32_t amount = 0;
    std::uint32_t ptr = 0;
};

struct SbtScript {
    std::uint32_t things = 0;
    std::uint32_t handles = 0;
    std::uint32_t code_size = 0;
    std::uint8_t flag = 0;
};

/// Compiled ``.sbt`` level file: 84-byte header + typed record stream.
/// Layout: docs/FORMATS.md §7 and gm/sbt.py.
struct SbtLevel {
    std::uint32_t version = 0;
    std::vector<SbtRecord> records;
    std::vector<SbtHandle> poly_handles;
    std::vector<SbtHandle> object_handles;
    std::vector<SbtHandle> sector_handles;
    std::vector<SbtSob> sobs;
    SbtScript script;
    // SCRIPT record payload, kept for the VM/scene layer: per-thing rows,
    // the handle columns (3*n u32), the compiled bytecode and the optional
    // 9*0x400 tables.
    std::vector<std::uint8_t> script_things;
    std::vector<std::uint8_t> script_handles_data;
    std::vector<std::uint8_t> script_code;
    std::vector<std::uint8_t> script_tables;
    std::size_t file_end = 0;
};

std::optional<SbtLevel> parse_sbt(std::span<const std::uint8_t> data, std::string& error);
const char* sbt_type_name(std::uint8_t type);

} // namespace gm
