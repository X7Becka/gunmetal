#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gm {

enum class Key {
    Left,
    Right,
    Up,
    Down,
    W,
    A,
    S,
    D,
    Shift,
    Plus,
    Minus,
    Escape,
};

/// Small platform window with a software framebuffer (XRGB8888).
///
/// Headless mode never touches SDL: it keeps a CPU buffer, which is what the
/// golden-frame tests will render into.
class Window {
public:
    static std::optional<Window> open(std::string_view title, int width, int height,
                                      bool headless, std::string& error);

    ~Window();
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Process events; returns false when the user closed the window (or Esc).
    bool pump();

    /// Keyboard state (always false in headless mode).
    bool key_down(Key key) const;

    /// Copy an RGB frame (nearest-neighbour scaled, letterboxed) into the window.
    void blit_rgb(const std::uint8_t* rgb, int width, int height);

    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b);

    /// Animated gradient, proves the framebuffer path works in both modes.
    void fill_gradient(int t);

    void present();

    /// Sleep for *ms* milliseconds (no-op in headless mode).
    void delay_ms(int ms);

    bool headless() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    explicit Window(Impl* impl);
    Impl* impl_ = nullptr;
};

} // namespace gm
