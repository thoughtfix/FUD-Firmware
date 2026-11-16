// Backported from FUD-Deck (thoughtfix, GPL-3.0). Same face engine, shared across the FUD family.
// Flat-color vector faces. Traced from FUD's face art (thoughtfix, GPL-3.0) by
// tools/facevec/trace_faces.py into facevec_data.h.
//
// Each face is: a black disk (rim), a base-colored disk inside it, then flat-color
// shapes painted in order. A shape is one or more closed polygons filled even-odd, so
// holes are just more polygons. Points are quarter source pixels ("art units", ART across
// the face box): a 16-bit start point per polygon, then 2-byte int8 steps.
// A shape can be reused by several faces with a per-use offset.
//
// Drawing composes one row at a time in a small line buffer (palette indices), then sends
// each run of one color once. Every pixel is written exactly once: no overdraw, no flicker,
// no frame buffer, no heap. About 400 bytes of stack. Works at any size up to MAX_SIZE.
//
// Animation for free: the features can be turned about the disk center (the rim stays put,
// like a wheel), and a radar sweep can be laid over the face.
//
// Works with any graphics object that has fillRect(x, y, w, h, color) and
// startWrite()/endWrite(), such as LovyanGFX's LGFX or a canvas.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "facevec_data.h"

namespace facevec {

static const int32_t TRANSPARENT = -1;
static const int MAX_SIZE = 320;   // widest face, in device pixels (the line buffer)

// Radar sweep colors.
static const uint16_t SWEEP_LINE = 0x07E0;    // bright green beam
static const uint16_t SWEEP_NEAR = 0x9FE6;    // base color just behind the beam
static const uint16_t SWEEP_FAR = 0xD7A4;     // base color further back

namespace detail {
using namespace facevec_data;

static const int MAX_X = 64;      // crossings per scanline; the generator's shapes need far fewer
static const int MAX_REFS = 32;   // shapes per face (the data uses at most 16)
static const uint8_t I_CLEAR = 255, I_LINE = 254, I_NEAR = 253, I_FAR = 252;

// Pixel px is inside when its center px + 0.5 lies in [xa, xb).
inline void fillIdx(uint8_t* line, int size, float xa, float xb, uint8_t idx) {
  int x0 = (int)ceilf(xa - 0.5f), x1 = (int)ceilf(xb - 0.5f);
  if (x0 < 0) x0 = 0;
  if (x1 > size) x1 = size;
  if (x1 > x0) memset(line + x0, idx, x1 - x0);
}

// Row py of an axis-aligned elliptical disk (center and radii in device px). False if the
// row misses it.
inline bool diskRow(int py, float cx, float cy, float rx, float ry, float& xa, float& xb) {
  float d = (py + 0.5f - cy) / ry;
  if (fabsf(d) >= 1) return false;
  float w = rx * sqrtf(1 - d * d);
  xa = cx - w;
  xb = cx + w;
  return true;
}

// Contour walker: a 16-bit start point, then int8 steps.
struct Walk {
  const uint8_t* q;
  int n, x, y;
  explicit Walk(const uint8_t* p) : q(p + 5), n(p[0]), x(p[1] | (p[2] << 8)), y(p[3] | (p[4] << 8)) {}
  void step() { x += (int8_t)q[0]; y += (int8_t)q[1]; q += 2; }
  const uint8_t* end() const { return q; }   // valid after n - 1 steps
};

// One use of a shape: offset in art units, optionally turned about (rcx, rcy).
struct Use {
  const uint8_t* p;
  int dx, dy;
  uint8_t color;
  bool rot;
  float c, s, rcx, rcy;
  void at(int x, int y, float& ox, float& oy) const {   // vertex > art units, as scanned
    if (!rot) { ox = (float)x; oy = (float)y; return; }
    float u = x + dx - rcx, v = y + dy - rcy;
    ox = rcx + u * c - v * s;
    oy = rcy + u * s + v * c;
  }
};

// Vertical extent of a use, in device rows.
inline void useRows(const Use& u, float k, int size, int& y0, int& y1) {
  float ymin = 1e9f, ymax = -1e9f;
  const int nc = u.p[0];
  const uint8_t* q = u.p + 1;
  for (int i = 0; i < nc; i++) {
    Walk w(q);
    for (int j = 0; j < w.n; j++) {
      float ox, oy;
      u.at(w.x, w.y, ox, oy);
      if (oy < ymin) ymin = oy;
      if (oy > ymax) ymax = oy;
      if (j + 1 < w.n) w.step();
    }
    q = w.end();
  }
  const float off = u.rot ? 0 : (float)u.dy;
  y0 = (int)floorf((ymin + off) * k);
  y1 = (int)ceilf((ymax + off) * k);
  if (y0 < 0) y0 = 0;
  if (y1 > size - 1) y1 = size - 1;
}

// Paint row py of one use into the line buffer, clipped to [clipL, clipR) device px.
inline void useRow(const Use& u, float k, int py, uint8_t* line, int size, float clipL, float clipR) {
  // Unturned: scan in the shape's own coordinates (exactly as traced). Turned: vertices are
  // already in face coordinates, offset included.
  const float offx = u.rot ? 0 : (float)u.dx, offy = u.rot ? 0 : (float)u.dy;
  const float yc = (py + 0.5f) / k - offy;   // scanline in art units
  float xs[MAX_X];
  int nx = 0;
  const int nc = u.p[0];
  const uint8_t* q = u.p + 1;
  for (int i = 0; i < nc; i++) {
    Walk w(q);
    float fx, fy, ax, ay, bx, by;
    u.at(w.x, w.y, fx, fy);
    ax = fx;
    ay = fy;
    for (int j = 0; j < w.n; j++) {
      if (j + 1 < w.n) { w.step(); u.at(w.x, w.y, bx, by); }
      else { bx = fx; by = fy; }   // closing edge
      if ((ay <= yc && by > yc) || (by <= yc && ay > yc)) {
        float x = ax + (yc - ay) * (bx - ax) / (by - ay);
        if (nx < MAX_X) xs[nx++] = (x + offx) * k;
      }
      ax = bx;
      ay = by;
    }
    q = w.end();
  }
  for (int i = 1; i < nx; i++) {   // insertion sort, nx is small
    float v = xs[i];
    int j = i - 1;
    while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
    xs[j + 1] = v;
  }
  for (int i = 0; i + 1 < nx; i += 2) {
    float a = xs[i] > clipL ? xs[i] : clipL, b = xs[i + 1] < clipR ? xs[i + 1] : clipR;
    fillIdx(line, size, a, b, u.color);
  }
}

inline const Face* find(const char* name) {
  for (int i = 0; i < FACE_COUNT; i++)
    if (!strcmp(FACES[i].name, name)) return &FACES[i];
  return nullptr;
}

// angle: degrees the features are turned (clockwise on screen). sweep: radar beam angle in
// degrees (0 = right, clockwise), or < 0 for none.
template <class G>
void drawFace(G& g, const Face& f, int x, int y, int size, int32_t bg, float angle, float sweep) {
  if (size > MAX_SIZE) size = MAX_SIZE;
  const float s = size / 135.0f;            // source px > device px
  const float k = size / (float)ART;        // art units > device px
  const float k8 = s / 8.0f;                // 1/8 source px > device px
  const float cx = f.cx * k8, cy = f.cy * k8;
  const float rox = f.rox * k8, roy = f.roy * k8, rix = f.rix * k8, riy = f.riy * k8;
  const bool rot = fmodf(fabsf(angle), 360.0f) > 0.01f;
  const float ar = angle * (float)M_PI / 180;

  Use uses[MAX_REFS];
  int y0[MAX_REFS], y1[MAX_REFS];
  const int n = f.nref < MAX_REFS ? f.nref : MAX_REFS;
  for (int i = 0; i < n; i++) {
    const uint8_t* r = REFS + 4 * (f.ref0 + i);
    Use& u = uses[i];
    u.p = SHAPES + SHAPE_OFS[r[0]];
    u.color = r[1];
    u.dx = (int8_t)r[2];
    u.dy = (int8_t)r[3];
    u.rot = rot;
    u.c = cosf(ar);
    u.s = sinf(ar);
    u.rcx = f.cx / 2.0f;   // 1/8 px > art units (1/4 px)
    u.rcy = f.cy / 2.0f;
    useRows(u, k, size, y0[i], y1[i]);
  }

  // Sweep: beam direction and the two trail edges (35 and 12 degrees behind it).
  const bool sw = sweep >= 0;
  const float b = sweep * (float)M_PI / 180, bn = b - 12 * (float)M_PI / 180, bf = b - 35 * (float)M_PI / 180;
  const float ux = cosf(b), uy = sinf(b), nxv = cosf(bn), nyv = sinf(bn), fxv = cosf(bf), fyv = sinf(bf);
  const float beamW = 1.2f * s;   // half width of the beam, device px

  uint8_t line[MAX_SIZE];
  for (int py = 0; py < size; py++) {
    memset(line, I_CLEAR, size);
    float xa, xb, ia = 0, ib = 0;
    if (diskRow(py, cx, cy, rox, roy, xa, xb)) fillIdx(line, size, xa, xb, 0);
    bool inner = diskRow(py, cx, cy, rix, riy, ia, ib);
    if (inner) fillIdx(line, size, ia, ib, f.base);
    // Turned features stay inside the rim (the strap would otherwise sweep across it).
    const float cl = rot ? (inner ? ia : 1e9f) : 0.0f, cr = rot ? (inner ? ib : -1e9f) : (float)size;
    for (int i = 0; i < n; i++)
      if (py >= y0[i] && py <= y1[i]) useRow(uses[i], k, py, line, size, cl, cr);
    if (sw && inner) {
      int x0 = (int)ceilf(ia - 0.5f), x1 = (int)ceilf(ib - 0.5f);
      if (x0 < 0) x0 = 0;
      if (x1 > size) x1 = size;
      const float vy = py + 0.5f - cy;
      for (int px = x0; px < x1; px++) {
        const float vx = px + 0.5f - cx;
        const float along = vx * ux + vy * uy, across = ux * vy - uy * vx;   // beam frame
        if (along > 0 && fabsf(across) <= beamW) { line[px] = I_LINE; continue; }
        if (line[px] != f.base) continue;   // the trail tints only the base color
        // Behind the beam: between the trail edge and the beam (wedges under 180 degrees).
        if (across < 0 && (nxv * vy - nyv * vx) >= 0) line[px] = I_NEAR;
        else if (across < 0 && (fxv * vy - fyv * vx) >= 0) line[px] = I_FAR;
      }
    }
    // Send runs.
    int px = 0;
    while (px < size) {
      uint8_t idx = line[px];
      int run = 1;
      while (px + run < size && line[px + run] == idx) run++;
      if (idx != I_CLEAR || bg >= 0) {
        uint16_t c = idx == I_CLEAR ? (uint16_t)bg : idx == I_LINE ? SWEEP_LINE : idx == I_NEAR ? SWEEP_NEAR
                   : idx == I_FAR ? SWEEP_FAR : PALETTE[idx];
        g.fillRect(x + px, y + py, run, 1, c);
      }
      px += run;
    }
  }
}
}  // namespace detail

// Number of faces and their names, for menus or tests.
inline int count() { return facevec_data::FACE_COUNT; }
inline const char* name(int i) { return facevec_data::FACES[i].name; }
inline bool has(const char* n) { return detail::find(n) != nullptr; }

// Draw face `name` filling a size x size square at (x, y). If bg is not TRANSPARENT the rest
// of the square is filled with bg; with TRANSPARENT, only the face's own pixels are written.
// angle turns the features (degrees, clockwise); sweep lays a radar beam at that angle
// (degrees, 0 = right, clockwise; < 0 = none). Unknown names draw "defaultsmile" and
// return false.
template <class G>
bool draw(G& g, const char* name, int x, int y, int size, int32_t bg = TRANSPARENT, float angle = 0,
          float sweep = -1) {
  const facevec_data::Face* f = detail::find(name);
  bool known = f != nullptr;
  if (!f) f = detail::find("defaultsmile");
  g.startWrite();
  if (f) detail::drawFace(g, *f, x, y, size, bg, angle, sweep);
  g.endWrite();
  return known;
}

}  // namespace facevec
