#include "platform/window.h"

#include "core/log.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#ifndef GM_HAVE_SDL3
#define GM_HAVE_SDL3 0
#endif

#if GM_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace gm {

struct Window::Impl {
    bool headless = false;
    bool quit = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> buffer; // headless framebuffer

#if GM_HAVE_SDL3
    SDL_Window* window = nullptr;
    SDL_Surface* surface = nullptr;
    bool owns_surface = false;
#endif
};

namespace {

#if GM_HAVE_SDL3
bool g_sdl_initialized = false;

bool ensure_sdl(std::string& error) {
    if (g_sdl_initialized) {
        return true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    g_sdl_initialized = true;
    return true;
}
#endif

} // namespace

std::optional<Window> Window::open(std::string_view title, int width, int height, bool headless,
                                   std::string& error) {
    auto* impl = new Impl();
    impl->headless = headless;
    impl->width = width;
    impl->height = height;

    if (headless) {
        impl->buffer.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                            0);
        return Window(impl);
    }

#if GM_HAVE_SDL3
    if (!ensure_sdl(error)) {
        delete impl;
        return std::nullopt;
    }
    impl->window = SDL_CreateWindow(std::string(title).c_str(), width, height, 0);
    if (impl->window == nullptr) {
        error = std::string("SDL_CreateWindow: ") + SDL_GetError();
        delete impl;
        return std::nullopt;
    }
    impl->surface = SDL_GetWindowSurface(impl->window);
    if (impl->surface == nullptr) {
        error = std::string("SDL_GetWindowSurface: ") + SDL_GetError();
        SDL_DestroyWindow(impl->window);
        delete impl;
        return std::nullopt;
    }
    logf(LogLevel::Info, "video", "window ", width, "x", height,
         " driver=", SDL_GetCurrentVideoDriver());
    return Window(impl);
#else
    error = "built without SDL3; use --headless";
    delete impl;
    return std::nullopt;
#endif
}

Window::Window(Impl* impl) : impl_(impl) {}

Window::~Window() {
    if (impl_ == nullptr) {
        return;
    }
#if GM_HAVE_SDL3
    if (impl_->surface != nullptr && impl_->owns_surface) {
        SDL_DestroySurface(impl_->surface);
    }
    if (impl_->window != nullptr) {
        SDL_DestroyWindow(impl_->window);
    }
#endif
    delete impl_;
}

Window::Window(Window&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        std::swap(impl_, other.impl_);
    }
    return *this;
}

bool Window::pump() {
    if (impl_ == nullptr) {
        return false;
    }
#if GM_HAVE_SDL3
    if (!impl_->headless) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                impl_->quit = true;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                impl_->quit = true;
            }
        }
    }
#endif
    return !impl_->quit;
}

void Window::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->headless) {
        const std::uint32_t pixel =
            0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        for (auto& p : impl_->buffer) {
            p = pixel;
        }
        return;
    }
#if GM_HAVE_SDL3
    if (impl_->surface != nullptr) {
        SDL_FillSurfaceRect(impl_->surface, nullptr, SDL_MapSurfaceRGB(impl_->surface, r, g, b));
    }
#endif
}

void Window::fill_gradient(int t) {
    if (impl_ == nullptr) {
        return;
    }
    const int width = impl_->width;
    const int height = impl_->height;

    if (impl_->headless) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::uint8_t r = static_cast<std::uint8_t>((x + t) & 0xff);
                const std::uint8_t g = static_cast<std::uint8_t>((y * 2 + t) & 0xff);
                const std::uint8_t b = static_cast<std::uint8_t>((x + y + t) & 0xff);
                impl_->buffer[static_cast<std::size_t>(y) * width + x] =
                    0xff000000u | (static_cast<std::uint32_t>(r) << 16) |
                    (static_cast<std::uint32_t>(g) << 8) | b;
            }
        }
        return;
    }
#if GM_HAVE_SDL3
    SDL_Surface* surface = impl_->surface;
    if (surface == nullptr) {
        return;
    }
    if (SDL_MUSTLOCK(surface)) {
        SDL_LockSurface(surface);
    }
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(surface->pixels) +
                                                     static_cast<std::ptrdiff_t>(y) *
                                                         surface->pitch);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t r = static_cast<std::uint8_t>((x + t) & 0xff);
            const std::uint8_t g = static_cast<std::uint8_t>((y * 2 + t) & 0xff);
            const std::uint8_t b = static_cast<std::uint8_t>((x + y + t) & 0xff);
            row[x] = SDL_MapSurfaceRGB(surface, r, g, b);
        }
    }
    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }
#endif
}

void Window::present() {
    if (impl_ == nullptr) {
        return;
    }
#if GM_HAVE_SDL3
    if (!impl_->headless && impl_->window != nullptr) {
        SDL_UpdateWindowSurface(impl_->window);
    }
#endif
}

bool Window::headless() const {
    return impl_ != nullptr && impl_->headless;
}

int Window::width() const {
    return impl_ != nullptr ? impl_->width : 0;
}

int Window::height() const {
    return impl_ != nullptr ? impl_->height : 0;
}

} // namespace gm
