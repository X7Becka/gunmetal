#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace gm {

/// One decoded operand of a ``.sbt`` SCRIPT instruction.
/// The Python reference keeps a plain ``str``/``int``/``float``; the kind is
/// preserved here because the text form depends on it (``%g`` for doubles,
/// ``"..."`` for strings, ``dec(hex)`` for large integers).
struct SbtValue {
    enum class Kind { UInt, Real, String };

    Kind kind = Kind::UInt;
    std::uint32_t u = 0; // Kind::UInt
    double d = 0.0;      // Kind::Real
    std::string s;       // Kind::String

    static SbtValue from_uint(std::uint32_t value);
    static SbtValue from_real(double value);
    static SbtValue from_string(std::string value);
};

/// One record of a ``DEFINE_*`` table (opcodes 170/171/172/175).  The leading
/// ``S`` letters of the signature are read as strings, the rest as values.
struct SbtTableRecord {
    std::vector<std::string> strings;
    std::vector<SbtValue> values;
};

/// One decoded instruction of the ``.sbt`` SCRIPT record (record type 11).
/// The opcode ids, operand signatures and structural opcodes are documented in
/// gm/sbtvm.py and docs/FORMATS.md §12.
struct SbtInsn {
    std::uint32_t offset = 0;
    std::uint8_t opcode = 0;
    std::string name;

    /// Ordinary operands, or the operands of the condition carried by IF/EVAL.
    std::vector<SbtValue> args;

    /// IF/ELSE branch body (decoded recursively).
    std::vector<SbtInsn> body;

    /// Records of a DEFINE_* table (opcodes 170/171/172/175).
    std::vector<SbtTableRecord> table;

    /// SOURCEPLAYER_PATH points / ADD_LIFT stops.
    std::vector<std::uint32_t> ints;

    /// Condition command name for IF/EVAL (``OP_<id>`` when the engine has no
    /// entry for the condition id).
    std::string condition;

    /// ELSE jump target (byte offset) and, for IF, the body size field.
    std::uint32_t target = 0;
    std::uint32_t size = 0;

    /// Record count of a table / path / lift.
    std::uint32_t count = 0;

    /// True for an ELSE that carries a decoded else-branch.
    bool has_body = false;
};

/// A decoded SCRIPT code block.  Named ``SbtProgram`` to avoid clashing with
/// ``gm::SbtScript`` in sbt.h, which holds the record's container fields.
struct SbtProgram {
    std::vector<SbtInsn> insns;
    std::size_t end = 0; // byte position after the final 0xFF
};

/// Decode a SCRIPT code block.  Returns nullopt on a malformed stream.
std::optional<SbtProgram> decode_sbt_script(std::span<const std::uint8_t> code,
                                           std::string& error);

/// Canonical text form (one instruction per line, nested bodies indented).
/// Used for golden comparison with gm/sbtvm.py's ``render()``.
std::string render_sbt_script(const SbtProgram& script);

/// Operand signature of a condition command (``""``/nullptr -> no operands,
/// empty string -> unknown name).  Used by the scene layer to classify the
/// ``ADD_THING*`` operands the same way the engine does.
std::string sbt_condition_signature(std::string_view name);

} // namespace gm
