#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace gm {

/// ASCII-only case-insensitive comparison (paths, extension names, tokens).
bool iequals_ascii(std::string_view a, std::string_view b);

/// Resolve `relative` under `root` ignoring ASCII case per path component.
///
/// The original installation is case-insensitive on Windows and the extracted
/// assets keep their shipped case, while Linux is case-sensitive. Exact matches
/// win; otherwise each component is matched against the directory entries.
/// Returns `std::nullopt` if a component does not exist.
std::optional<std::filesystem::path> resolve_ci(const std::filesystem::path& root,
                                                std::string_view relative);

} // namespace gm
