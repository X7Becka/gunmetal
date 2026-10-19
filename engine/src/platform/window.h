#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gm {

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

    void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b);

    /// Animated gradient, proves the framebuffer path works in both modes.
    void fill_gradient(int t);

    void present();

    bool headless() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    explicit Window(Impl* impl);
    Impl* impl_ = nullptr;
};

} // namespace gm
