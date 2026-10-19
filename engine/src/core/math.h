#pragma once

#include <cmath>

namespace gm {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

constexpr Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

constexpr Vec3 operator*(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

constexpr Vec3 operator*(float s, Vec3 v) {
    return v * s;
}

constexpr float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalized(Vec3 v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : Vec3{};
}

constexpr float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

constexpr float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace gm
