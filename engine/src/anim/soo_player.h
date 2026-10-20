#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "res/anim.h"
#include "res/sbw.h"
#include "res/sob.h"

namespace gm {

/// One instant of a ``.soo`` animation: the interpolated pose plus the two
/// source frames and the blend fraction.  ``mesh`` is a normal :class:`Sob`
/// (vertices, faces with textures/indices and interpolated planes), so it can
/// be fed to the existing mesh builders unchanged.
struct SooPlayerPose {
    int frame_a = 0;          // current frame index (engine frames)
    int frame_b = 0;          // pose interpolated towards
    std::string name_a;       // SOBS/<name_a>.sob
    std::string name_b;
    double alpha = 0.0;       // elapsed / duration, 0..1
    int tick = 0;             // normalised 91 Hz clock
    int elapsed = 0;          // ticks inside frame_a
    int total_ticks = 0;      // effective duration
    Sob mesh;                 // interpolated vertices + faces (from frame_a)
    std::vector<Vec3f> normals; // per-face, unit length, parallel to mesh.faces
};

struct SooPlayerStats {
    std::uint64_t soo_loads = 0;   // successful load() calls
    std::uint64_t frame_loads = 0; // engine frames resolved (cache hits + misses)
    std::uint64_t cache_hits = 0;  // a frame name reused a cached .sob
    std::uint64_t cache_misses = 0;// .sob files parsed
    std::size_t cached_models = 0; // distinct names currently held
};

/// Runtime player for ``Sobanim/*.soo`` flipbooks.
///
/// ``load`` parses the ``.soo`` with :func:`parse_soo`, resolves the frame list
/// exactly like the engine loader (``count`` frames, 1..32), loads every
/// ``SOBS/<name>.sob`` through a name-keyed cache and checks that all poses
/// share the vertex/face counts.  ``pose_at`` then only selects a frame and
/// morphs the two cached poses, using the verified engine timeline
/// (:func:`soo_pose_at`): 91 Hz ticks, mode 0 holds, mode 1 wraps.
///
/// The player owns all parsed models.  Only the shipped model data is read
/// (never written) and no reference cycles are created, so destruction frees
/// everything.
class SooPlayer {
public:
    explicit SooPlayer(std::filesystem::path game_dir);

    /// Load ``soo_path``: an absolute path, a path under ``game_dir``, or a
    /// bare name looked up in ``<game_dir>/Sobanim`` (``.soo`` may be
    /// omitted).  ASCII case is ignored per component.  On failure the
    /// previous animation (if any) stays loaded and *error* is set.
    bool load(const std::filesystem::path& soo_path, std::string& error);

    bool loaded() const { return loaded_; }
    const Soo& animation() const { return soo_; }

    /// Interpolated pose at *ms* milliseconds.  Negative time is clamped to 0
    /// (engine ``0x433e84``); huge values wrap/hold in 64-bit arithmetic.
    std::optional<SooPlayerPose> pose_at(int ms, std::string& error) const;

    /// Override the file's ``mode`` for the currently loaded animation:
    /// ``true`` wraps (mode 1), ``false`` holds (mode 0).  A later ``load``
    /// takes the mode from the file again; before the first load this is a
    /// no-op.
    void set_loop(bool loop);
    bool loop() const;

    /// Effective duration of the frames the engine would load, 91 Hz ticks.
    int duration_ticks() const { return total_ticks_; }
    double duration_ms() const { return ticks_to_ms(total_ticks_); }

    SooPlayerStats stats() const;

    const std::filesystem::path& game_dir() const { return game_dir_; }

private:
    // Models are immutable once parsed and shared between cache and frame list.
    using ModelPtr = std::shared_ptr<const Sob>;

    std::optional<ModelPtr> load_model(const std::string& name, std::string& error);

    std::filesystem::path game_dir_;
    Soo soo_;
    std::vector<ModelPtr> frames_; // engine frames, in order
    int total_ticks_ = 0;
    bool loaded_ = false;
    std::map<std::string, ModelPtr> cache_; // key: ASCII-lowercased frame name
    mutable SooPlayerStats stats_;
};

} // namespace gm
