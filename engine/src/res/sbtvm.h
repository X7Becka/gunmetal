#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gm {

/// One decoded instruction of the ``.sbt`` SCRIPT record (record type 11).
/// The opcode ids, operand signatures and structural opcodes are documented in
/// gm/sbtvm.py and docs/FORMATS.md §12.
struct SbtInsn {
    std::uint32_t offset = 0;
    std::uint8_t opcode = 0;
    std::string name;
    std::vector<std::string> operands; // rendered operand text, in order
    std::vector<SbtInsn> body;         // IF/ELSE branch body
};

struct SbtScript {
    std::vector<SbtInsn> insns;
    std::size_t end = 0; // byte position after the final 0xFF
};

/// Decode a SCRIPT code block.  Returns nullopt on a malformed stream.
std::optional<SbtScript> decode_sbt_script(std::span<const std::uint8_t> code,
                                           std::string& error);

/// Canonical text form (one instruction per line, nested bodies indented).
/// Used for golden comparison with gm/sbtvm.py's ``render()``.
std::string render_sbt_script(const SbtScript& script);

} // namespace gm
