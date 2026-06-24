// path_tracer.cpp
//
// Iterative path tracer: each bounce level's ray state lives on the SSDs as a
// chunk_seq<RayState>.  One ChunkMap pass advances all rays by one bounce; the
// previous level's files are deleted immediately after to bound SSD usage.
//
// Contrast with raytracer.cpp (a single tabulate pass): here intermediate
// sequences persist on disk between computation steps, exercising the full
// tabulate → ChunkMap → ChunkMap → … → ChunkMap pipeline.
//
// Build:
//   make bazel-bin/pathTracer
// Run:
//   bazel-bin/pathTracer
//   python3 ChunkSequence/examples/make_png.py ipt_meta.txt path_traced.png

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include <cmath>
#include <cstdint>
#include <vector>
#include <set>
#include <string>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <functional>
#include <unistd.h>

#include "utils/command_line.h"
#include "utils/file_utils.h"
#include "ChunkSequence/chunk_seq.h"
#include "ChunkSequence/chunk_map.h"
#include "ChunkSequence/chunk_seq_reader.h"
#include "ChunkSequence/chunk_filter.h"

// ----------------------------------------------------------------------------
// Vec3
// ----------------------------------------------------------------------------
struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3() = default;
  Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

static inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 operator-(Vec3 a)          { return {-a.x, -a.y, -a.z}; }
static inline Vec3 operator*(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
static inline Vec3 operator*(float s, Vec3 a) { return a * s; }
static inline Vec3 operator*(Vec3 a, Vec3 b)  { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
static inline Vec3 operator/(Vec3 a, float s) { return {a.x/s, a.y/s, a.z/s}; }

static inline float dot(Vec3 a, Vec3 b)       { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float length(Vec3 a)            { return std::sqrt(dot(a, a)); }
static inline Vec3  normalize(Vec3 a)         { return a / length(a); }
static inline Vec3  cross(Vec3 a, Vec3 b) {
  return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static inline Vec3 reflect(Vec3 v, Vec3 n) { return v - n * (2.f * dot(v, n)); }

// ----------------------------------------------------------------------------
// Scene types
// ----------------------------------------------------------------------------
struct Ray { Vec3 orig, dir; Vec3 at(float t) const { return orig + dir * t; } };

struct Sphere {
  Vec3  center;
  float radius;
  Vec3  albedo;
  float reflectivity;
  bool  checker;
};

struct Hit { bool hit = false; float t = 0; Vec3 point, normal; int idx = -1; };

struct Light { Vec3 pos; Vec3 color; float intensity; };

// ----------------------------------------------------------------------------
// Shading helpers
// ----------------------------------------------------------------------------
static inline Vec3 skyColor(Vec3 dir) {
  float t = 0.5f * (normalize(dir).y + 1.f);
  return Vec3(1,1,1) * (1.f - t) + Vec3(0.5f, 0.7f, 1.f) * t;
}

static inline Vec3 checkerColor(Vec3 p) {
  int c = static_cast<int>(std::floor(p.x * 0.6f) + std::floor(p.z * 0.6f));
  return ((c & 1) == 0) ? Vec3(0.85f, 0.85f, 0.85f) : Vec3(0.12f, 0.22f, 0.32f);
}

static Hit closest(const Ray& r, const std::vector<Sphere>& scene, float tmin, float tmax) {
  Hit best; best.hit = false; best.t = tmax;
  for (int i = 0; i < static_cast<int>(scene.size()); ++i) {
    const Sphere& s = scene[i];
    Vec3  oc    = r.orig - s.center;
    float a     = dot(r.dir, r.dir);
    float halfb = dot(oc, r.dir);
    float c     = dot(oc, oc) - s.radius * s.radius;
    float disc  = halfb * halfb - a * c;
    if (disc < 0.f) continue;
    float sq = std::sqrt(disc);
    float t  = (-halfb - sq) / a;
    if (t < tmin || t > best.t) {
      t = (-halfb + sq) / a;
      if (t < tmin || t > best.t) continue;
    }
    best.hit = true; best.t = t; best.idx = i;
  }
  if (best.hit) {
    best.point  = r.at(best.t);
    best.normal = normalize(best.point - scene[best.idx].center);
  }
  return best;
}

// ----------------------------------------------------------------------------
// Camera
// ----------------------------------------------------------------------------
struct Camera {
  Vec3  pos, forward, right, up;
  float halfW, halfH;

  Camera(Vec3 from, Vec3 lookAt, Vec3 worldUp, float vfovDeg, float aspect) {
    pos     = from;
    forward = normalize(lookAt - from);
    right   = normalize(cross(forward, worldUp));
    up      = cross(right, forward);
    halfH   = std::tan(vfovDeg * 0.5f * 3.14159265358979f / 180.f);
    halfW   = aspect * halfH;
  }
  Ray ray(float sx, float sy) const {
    float u = (2.f * sx - 1.f) * halfW;
    float v = (1.f - 2.f * sy) * halfH;
    return Ray{ pos, normalize(forward + right * u + up * v) };
  }
};

// ----------------------------------------------------------------------------
// Beaded text (identical to raytracer.cpp)
// ----------------------------------------------------------------------------
static const unsigned char font8x8[128][8] = {
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0000 (nul)
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0001
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0002
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0003
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0004
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0005
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0006
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0007
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0008
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0009
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000A
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000B
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000C
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000D
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000E
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+000F
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0010
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0011
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0012
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0013
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0014
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0015
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0016
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0017
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0018
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0019
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001A
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001B
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001C
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001D
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001E
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+001F
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0020 (space)
  { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},   // U+0021 (!)
  { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0022 (")
  { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},   // U+0023 (#)
  { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},   // U+0024 ($)
  { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},   // U+0025 (%)
  { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},   // U+0026 (&)
  { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0027 (')
  { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},   // U+0028 (()
  { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},   // U+0029 ())
  { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},   // U+002A (*)
  { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},   // U+002B (+)
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+002C (,)
  { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},   // U+002D (-)
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+002E (.)
  { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},   // U+002F (/)
  { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},   // U+0030 (0)
  { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},   // U+0031 (1)
  { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},   // U+0032 (2)
  { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},   // U+0033 (3)
  { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},   // U+0034 (4)
  { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},   // U+0035 (5)
  { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},   // U+0036 (6)
  { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},   // U+0037 (7)
  { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},   // U+0038 (8)
  { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},   // U+0039 (9)
  { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},   // U+003A (:)
  { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},   // U+003B (;)
  { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},   // U+003C (<)
  { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},   // U+003D (=)
  { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},   // U+003E (>)
  { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},   // U+003F (?)
  { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},   // U+0040 (@)
  { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},   // U+0041 (A)
  { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},   // U+0042 (B)
  { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},   // U+0043 (C)
  { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},   // U+0044 (D)
  { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},   // U+0045 (E)
  { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},   // U+0046 (F)
  { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},   // U+0047 (G)
  { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},   // U+0048 (H)
  { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0049 (I)
  { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},   // U+004A (J)
  { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},   // U+004B (K)
  { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},   // U+004C (L)
  { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},   // U+004D (M)
  { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},   // U+004E (N)
  { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},   // U+004F (O)
  { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},   // U+0050 (P)
  { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},   // U+0051 (Q)
  { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},   // U+0052 (R)
  { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},   // U+0053 (S)
  { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0054 (T)
  { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},   // U+0055 (U)
  { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0056 (V)
  { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},   // U+0057 (W)
  { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},   // U+0058 (X)
  { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   // U+0059 (Y)
  { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},   // U+005A (Z)
  { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},   // U+005B ([)
  { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},   // U+005C (\)
  { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},   // U+005D (])
  { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},   // U+005E (^)
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},   // U+005F (_)
  { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+0060 (`)
  { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},   // U+0061 (a)
  { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},   // U+0062 (b)
  { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},   // U+0063 (c)
  { 0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00},   // U+0064 (d)
  { 0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00},   // U+0065 (e)
  { 0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00},   // U+0066 (f)
  { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0067 (g)
  { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},   // U+0068 (h)
  { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+0069 (i)
  { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},   // U+006A (j)
  { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},   // U+006B (k)
  { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   // U+006C (l)
  { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},   // U+006D (m)
  { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},   // U+006E (n)
  { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},   // U+006F (o)
  { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},   // U+0070 (p)
  { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},   // U+0071 (q)
  { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},   // U+0072 (r)
  { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},   // U+0073 (s)
  { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},   // U+0074 (t)
  { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},   // U+0075 (u)
  { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   // U+0076 (v)
  { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},   // U+0077 (w)
  { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},   // U+0078 (x)
  { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},   // U+0079 (y)
  { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},   // U+007A (z)
  { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},   // U+007B ({)
  { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   // U+007C (|)
  { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},   // U+007D (})
  { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // U+007E (~)
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}    // U+007F
};

static int beadLineLen(const char* s) {
  int n = 0;
  while (*s && *s != '\n') { ++n; ++s; }
  return n;
}

static void addBeadText(std::vector<Sphere>& scene, const char* text, Vec3 center,
                        float spacing, float bead,
                        const Vec3* letterColors, int numColors) {
  const float advance = 9.f * spacing;
  const float glyphExt = 7.f * spacing;
  int lineCount = 1;
  for (const char* p = text; *p; ++p) if (*p == '\n') ++lineCount;
  float blockH = static_cast<float>(lineCount - 1) * advance + glyphExt;
  float topY   = center.y + blockH * 0.5f;
  int k = 0, li = 0;
  const char* p = text;
  for (;;) {
    int   L      = beadLineLen(p);
    float lineW  = L > 0 ? (static_cast<float>(L - 1) * advance + glyphExt) : 0.f;
    float startX = center.x - lineW * 0.5f;
    float baseY  = topY - static_cast<float>(li) * advance;
    int col = 0;
    for (; *p && *p != '\n'; ++p, ++col, ++k) {
      unsigned char ch = static_cast<unsigned char>(*p);
      if (ch >= 128) ch = '?';
      const unsigned char* g = font8x8[ch];
      Vec3 c = letterColors[((k % numColors) + numColors) % numColors];
      float xBase = startX + static_cast<float>(col) * advance;
      for (int r = 0; r < 8; ++r) {
        unsigned char rowbits = g[r];
        if (!rowbits) continue;
        for (int cc = 0; cc < 8; ++cc) {
          if ((rowbits >> cc) & 1) {
            Vec3 ctr{ xBase + static_cast<float>(cc) * spacing,
                      baseY  - static_cast<float>(r)  * spacing,
                      center.z };
            scene.push_back({ ctr, bead, c, 0.f, false });
          }
        }
      }
    }
    if (*p == '\n') { ++p; ++li; continue; }
    break;
  }
}

// ----------------------------------------------------------------------------
// Ray state carried between bounce passes on disk
// ----------------------------------------------------------------------------
struct RayState {
  float ox, oy, oz;     // ray origin
  float dx, dy, dz;     // ray direction (unit vector)
  float tx, ty, tz;     // throughput: product of albedos along path
  float lx, ly, lz;     // accumulated radiance
  uint32_t pixel_idx;   // which output pixel this ray belongs to
  uint32_t depth;       // bounce count; used in RNG seed to vary per-bounce
  uint32_t _pad[2];     // padding to 64 bytes
};
static_assert(sizeof(RayState) == 64,
    "RayState must be 64 bytes so CHUNK_SIZE % sizeof(RayState) == 0");

// ----------------------------------------------------------------------------
// Hash-based RNG — no global state, safe for parallel ChunkMap workers
// ----------------------------------------------------------------------------
static uint32_t hash32(uint32_t x) {
  x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x;
}

// Cosine-weighted hemisphere sample aligned with normal N.
static Vec3 cosineHemisphere(Vec3 N, uint32_t seed) {
  uint32_t h = hash32(seed);
  float u1  = (h & 0xFFFFu) * (1.f / 65536.f);
  h = hash32(h);
  float u2  = (h & 0xFFFFu) * (1.f / 65536.f);
  float r   = std::sqrt(u1);
  float phi = 2.f * 3.14159265358979f * u2;
  float lx  = r * std::cos(phi), ly = r * std::sin(phi);
  float lz  = std::sqrt(std::max(0.f, 1.f - u1));
  // Build orthonormal frame around N
  Vec3 up = std::abs(N.z) < 0.999f ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
  Vec3 T  = normalize(cross(up, N));
  Vec3 B  = cross(N, T);
  return normalize(T * lx + B * ly + N * lz);
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

// Delete all per-drive files belonging to seq (called after each bounce).
static void cleanup_chunk_seq(const chunk_seq& seq) {
  std::set<std::string> seen;
  for (const auto& c : seq.chunks)
    if (seen.insert(c.filename).second)
      unlink(c.filename.c_str());
}

// ----------------------------------------------------------------------------
// main
// ----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  ParseGlobalArguments(argc, argv);

  const int   W         = 3840;
  const int   H         = 2160;
  const int   MAX_DEPTH = 5;
  const int   SAMPLES   = 8;   // samples per pixel — increase to reduce noise
  const float aspect    = static_cast<float>(W) / static_cast<float>(H);

  // ── Scene (identical to raytracer.cpp) ──────────────────────────────────
  std::vector<Sphere> scene;
  scene.push_back({{0, -1000, 0}, 1000.f, {0, 0, 0},              0.00f, true});
  scene.push_back({{0,      1, 0},   1.f, {0.80f, 0.30f, 0.30f},  0.10f, false});
  scene.push_back({{-2.3f,  1, 0},   1.f, {0.92f, 0.92f, 0.92f},  0.88f, false});
  scene.push_back({{ 2.3f,  1, 0},   1.f, {0.30f, 0.45f, 0.90f},  0.30f, false});
  scene.push_back({{ 0.0f, 0.45f, 1.6f}, 0.45f, {0.95f,0.78f,0.25f}, 0.f, false});

  const Vec3 palette[5] = {
    {0.85f,0.25f,0.45f}, {0.25f,0.75f,0.55f}, {0.85f,0.55f,0.20f},
    {0.55f,0.35f,0.85f}, {0.20f,0.70f,0.85f}
  };
  for (int i = 0; i < 7; ++i) {
    float ang = static_cast<float>(i) / 7.f * 2.f * 3.14159265f;
    float rad = 3.4f;
    Vec3  c{ std::cos(ang) * rad, 0.35f, std::sin(ang) * rad - 0.4f };
    scene.push_back({ c, 0.35f, palette[i % 5], (i % 3 == 0) ? 0.55f : 0.f, false });
  }

  Light  light{ {6.f, 7.f, 4.f}, {1.f, 0.98f, 0.92f}, 0.95f };
  Camera cam({0.f, 1.7f, 5.2f}, {0.f, 0.9f, 0.f}, {0, 1, 0}, 42.f, aspect);

  const Vec3 textColors[6] = {
    {0.95f,0.25f,0.30f}, {0.97f,0.55f,0.15f}, {0.98f,0.82f,0.20f},
    {0.30f,0.78f,0.45f}, {0.25f,0.55f,0.95f}, {0.65f,0.35f,0.90f}
  };
  addBeadText(scene, "Get better soon \n Laxman!!!",
              Vec3{0.f, 2.95f, -7.f}, 0.10f, 0.062f, textColors, 6);

  std::cerr << "Rendering " << W << "x" << H
            << "  workers=" << parlay::num_workers()
            << "  depth=" << MAX_DEPTH
            << "  spp=" << SAMPLES
            << "  spheres=" << scene.size() << "\n";

  // Float accumulation buffer in DRAM: 3 floats (R,G,B) per pixel.
  // All SAMPLES passes accumulate here; we divide by SAMPLES at the end.
  // Size: W*H*3*4 bytes = ~24 MB at 1920x1080.
  std::vector<float> accum(static_cast<size_t>(W) * H * 3, 0.f);

  for (int s = 0; s < SAMPLES; ++s) {
    std::string suf = "_s" + std::to_string(s);

    // ── Phase 1: Generate primary rays (one per pixel, jittered per sample) ──
    // Ray i corresponds to pixel i throughout all bounce passes; we never
    // filter out terminated rays (which would break this correspondence) —
    // instead they carry T=(0,0,0) and are no-ops in subsequent ChunkMap passes.
    // _pad[1] stores the sample index so the bounce RNG can vary across samples.
    chunk_seq rays = ChunkSequenceOps::tabulate<RayState>(
        static_cast<size_t>(W) * H,
        "ipt_b0" + suf,
        std::function<RayState(size_t)>([&, s](size_t idx) -> RayState {
          int px = static_cast<int>(idx % W);
          int py = static_cast<int>(idx / W);
          // Sub-pixel jitter differs per sample so multiple passes cover the pixel.
          uint32_t seed0 = hash32(static_cast<uint32_t>(idx) ^ (static_cast<uint32_t>(s) * 2531011u));
          uint32_t h1    = hash32(seed0);
          uint32_t h2    = hash32(h1);
          float jx = (h1 & 0xFFFFu) * (1.f / 65536.f) - 0.5f;
          float jy = (h2 & 0xFFFFu) * (1.f / 65536.f) - 0.5f;
          Ray r = cam.ray((px + 0.5f + jx) / W, (py + 0.5f + jy) / H);
          RayState rs{};
          rs.ox = r.orig.x; rs.oy = r.orig.y; rs.oz = r.orig.z;
          rs.dx = r.dir.x;  rs.dy = r.dir.y;  rs.dz = r.dir.z;
          rs.tx = rs.ty = rs.tz = 1.f;
          rs.lx = rs.ly = rs.lz = 0.f;
          rs.pixel_idx = static_cast<uint32_t>(idx);
          rs.depth     = 0;
          rs._pad[0]   = 0;
          rs._pad[1]   = static_cast<uint32_t>(s);  // sample index for RNG
          return rs;
        }));
    std::cerr << "  sample " << (s+1) << "/" << SAMPLES
              << "  bounce 0 (primary): " << rays.chunks.size() << " chunks\n";

    // ── Phase 2: Bounce loop ───────────────────────────────────────────────
    // Each iteration: (a) advance active rays via ChunkMap, (b) stream the
    // result to accumulate terminated rays' radiance into accum[], (c) filter
    // to keep only active rays for the next bounce.  Terminated rays never
    // enter the next ChunkMap — no need for a T=0 early-exit guard.
    for (int d = 0; d < MAX_DEPTH; ++d) {
      chunk_seq bounce = ChunkSequenceOps::ChunkMap<RayState, RayState>(
          rays,
          "ipt_b" + std::to_string(d + 1) + suf,
          std::function<RayState(RayState)>([&](RayState rs) -> RayState {

            constexpr float EPS = 5e-3f;

            Ray r{ {rs.ox, rs.oy, rs.oz}, {rs.dx, rs.dy, rs.dz} };
            Hit h = closest(r, scene, EPS, 1e30f);

            // ── Sky hit: accumulate sky radiance and terminate ────────────
            if (!h.hit) {
              Vec3 sky = skyColor(r.dir);
              rs.lx += rs.tx * sky.x;
              rs.ly += rs.ty * sky.y;
              rs.lz += rs.tz * sky.z;
              rs.tx = rs.ty = rs.tz = 0.f;
              return rs;
            }

            const Sphere& sp = scene[h.idx];
            Vec3 albedo      = sp.checker ? checkerColor(h.point) : sp.albedo;
            Vec3 N           = h.normal;
            if (dot(N, r.dir) > 0.f) N = -N;  // face normal toward incoming ray

            // ── Direct illumination from point light ──────────────────────
            Vec3  toL  = light.pos - h.point;
            float dist = length(toL);
            Vec3  L    = toL / dist;
            Ray   shadow{ h.point + N * EPS, L };
            Hit   occ = closest(shadow, scene, EPS, dist - EPS);
            if (!occ.hit) {
              float diff = std::max(0.f, dot(N, L));
              rs.lx += rs.tx * albedo.x * light.color.x * diff * light.intensity;
              rs.ly += rs.ty * albedo.y * light.color.y * diff * light.intensity;
              rs.lz += rs.tz * albedo.z * light.color.z * diff * light.intensity;
            }

            // ── Next bounce direction ──────────────────────────────────────
            Vec3 new_dir;
            if (sp.reflectivity > 0.5f) {
              new_dir = normalize(reflect(r.dir, N));
            } else {
              // Seed mixes pixel, bounce depth, and sample index for decorrelation.
              uint32_t seed = rs.pixel_idx * 1664525u
                            ^ (rs.depth * 22695477u + 1u)
                            ^ (rs._pad[1] * 2531011u);
              new_dir = cosineHemisphere(N, seed);
            }

            rs.tx *= albedo.x;
            rs.ty *= albedo.y;
            rs.tz *= albedo.z;

            rs.ox = h.point.x + N.x * EPS;
            rs.oy = h.point.y + N.y * EPS;
            rs.oz = h.point.z + N.z * EPS;
            rs.dx = new_dir.x;
            rs.dy = new_dir.y;
            rs.dz = new_dir.z;
            rs.depth++;
            return rs;
          }));

      cleanup_chunk_seq(rays);

      // Step 2: accumulate radiance from newly-terminated rays into DRAM.
      // pixel_idx is unique within a single sample, so no race conditions.
      {
        ChunkSequenceReader<RayState> reader;
        reader.PrepChunks(bounce);
        reader.Start(5, 32, 16);
        while (true) {
          auto [ptr, n, chunk_index] = reader.Poll();
          if (!ptr) break;
          for (size_t j = 0; j < n; ++j) {
            const RayState& rs = ptr[j];
            if (rs.tx + rs.ty + rs.tz < 1e-6f) {
              size_t off = static_cast<size_t>(rs.pixel_idx) * 3;
              accum[off + 0] += rs.lx;
              accum[off + 1] += rs.ly;
              accum[off + 2] += rs.lz;
            }
          }
          reader.allocator.Free(ptr);
        }
      }

      // Step 3: filter to active-only rays; delete bounce result.
      chunk_seq active = ChunkSequenceOps::ChunkFilter<RayState>(
          bounce,
          "ipt_f" + std::to_string(d) + suf,
          std::function<bool(RayState)>([](RayState rs) -> bool {
            return rs.tx + rs.ty + rs.tz >= 1e-6f;
          }));
      cleanup_chunk_seq(bounce);
      rays = std::move(active);

      std::cerr << "    bounce " << (d + 1) << ": " << rays.chunks.size()
                << " active chunks\n";
      if (rays.chunks.empty()) break;  // all rays terminated early
    }

    // Accumulate any rays that survived all MAX_DEPTH bounces without terminating.
    {
      ChunkSequenceReader<RayState> reader;
      reader.PrepChunks(rays);
      reader.Start(5, 32, 16);
      while (true) {
        auto [ptr, n, chunk_index] = reader.Poll();
        if (!ptr) break;
        for (size_t j = 0; j < n; ++j) {
          const RayState& rs = ptr[j];
          size_t off = static_cast<size_t>(rs.pixel_idx) * 3;
          accum[off + 0] += rs.lx;
          accum[off + 1] += rs.ly;
          accum[off + 2] += rs.lz;
        }
        reader.allocator.Free(ptr);
      }
    }
    cleanup_chunk_seq(rays);
    std::cerr << "  sample " << (s+1) << "/" << SAMPLES << " accumulated.\n";
  }

  // ── Phase 4: Average accumulator → packed pixel chunk_seq on SSDs ────────
  float inv = 1.f / static_cast<float>(SAMPLES);
  chunk_seq pixels = ChunkSequenceOps::tabulate<uint32_t>(
      static_cast<size_t>(W) * H,
      "ipt_pixels",
      std::function<uint32_t(size_t)>([&](size_t idx) -> uint32_t {
        auto g = [](float v) { return std::sqrt(std::min(1.f, std::max(0.f, v))); };
        auto b = [&](float v) { return static_cast<uint32_t>(g(v) * 255.f + 0.5f); };
        float r  = accum[idx * 3 + 0] * inv;
        float gr = accum[idx * 3 + 1] * inv;
        float bl = accum[idx * 3 + 2] * inv;
        return (b(r) << 16) | (b(gr) << 8) | b(bl);
      }));

  std::cerr << "Done. " << pixels.chunks.size() << " output chunk(s) on "
            << GetSSDList().size() << " drive(s).\n";

  // ── Write metadata for make_png.py ──────────────────────────────────────
  {
    std::ofstream meta("ipt_meta.txt");
    meta << W << " " << H << "\n";
    for (const auto& c : pixels.chunks)
      meta << c.index << " " << c.filename
           << " " << c.begin_addr << " " << c.used << "\n";
  }
  std::cerr << "Metadata written to ipt_meta.txt\n"
            << "Run: python3 ChunkSequence/examples/make_png.py ipt_meta.txt path_traced.png\n";

  return 0;
}
