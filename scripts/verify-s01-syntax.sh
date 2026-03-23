#!/usr/bin/env bash
# verify-s01-syntax.sh — Syntax-only compilation check for DeepC*.cpp
# Uses mock DDImage headers (minimal stubs) to verify C++ syntax without
# the full Nuke SDK. Exits 0 on success, 1 on failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/../src"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# --- Create mock DDImage headers ---

mkdir -p "$TMPDIR/DDImage"

# --- Op.h (base class) ---
cat > "$TMPDIR/DDImage/Op.h" << 'HEADER'
#pragma once
#include <vector>
namespace DD { namespace Image {

class Node;
class Knob;

class Knob;
typedef void (*Knob_Callback)(void);

class Op {
public:
    Op() {}
    Op(Node*) {}
    virtual ~Op() {}
    bool aborted() const { return false; }
    Node* node() const { return nullptr; }
    virtual const char* Class() const { return ""; }
    virtual const char* node_help() const { return ""; }
    virtual void knobs(Knob_Callback) {}
    virtual Op* op() { return this; }
    void inputs(int) {}
    Op* input(int) const { return nullptr; }
    virtual int minimum_inputs() const { return 1; }
    virtual int maximum_inputs() const { return 1; }
    virtual const char* input_label(int, char*) const { return ""; }
    virtual bool test_input(int, Op*) const { return true; }
    virtual Op* default_input(int) const { return nullptr; }
    virtual int knob_changed(Knob*) { return 0; }
    void error(const char*, ...) {}
    struct Description {
        Description(const char*, const char*, Op*(*)(Node*)) {}
    };
};

}} // namespace DD::Image
HEADER

# --- Channel.h ---
cat > "$TMPDIR/DDImage/Channel.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

typedef int Channel;
static const Channel Chan_DeepFront = 0;
static const Channel Chan_DeepBack = 1;
static const Channel Chan_Alpha = 2;
static const Channel Chan_Red = 3;
static const Channel Chan_Green = 4;
static const Channel Chan_Blue = 5;

typedef unsigned int ChannelMask;
static const ChannelMask Mask_None = 0;
static const ChannelMask Mask_RGBA = 0xF;
static const ChannelMask Mask_All  = 0xFFFFFFFF;

class ChannelSet {
public:
    ChannelSet() : _first(0), _count(0) {}
    ChannelSet(ChannelMask) : _first(0), _count(4) {}
    int size() const { return _count; }
    Channel first() const { return _first; }
    Channel next(Channel z) const { return (z + 1 < _first + _count) ? z + 1 : 0; }
    ChannelSet& operator+=(Channel) { return *this; }
    ChannelSet operator+(Channel) const { return *this; }
    ChannelSet operator+(const ChannelSet&) const { return *this; }
    bool contains(Channel) const { return true; }
private:
    Channel _first;
    int _count;
};

}} // namespace DD::Image

// DDImage foreach macro — iterates channels in a ChannelSet
#define foreach(VAR, SET) \
    for (DD::Image::Channel VAR = (SET).first(); VAR; VAR = (SET).next(VAR))
HEADER

# --- Box.h ---
cat > "$TMPDIR/DDImage/Box.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Format {
public:
    Format() {}
    int width()  const { return 1920; }
    int height() const { return 1080; }
};

class Box {
public:
    Box() : _x(0), _y(0), _r(0), _t(0) {}
    Box(int x, int y, int r, int t) : _x(x), _y(y), _r(r), _t(t) {}
    int x() const { return _x; }
    int y() const { return _y; }
    int r() const { return _r; }
    int t() const { return _t; }
    void set(int x, int y, int r, int t) { _x=x; _y=y; _r=r; _t=t; }

    struct iterator {
        int x, y;
        int _x0, _r, _t;
        iterator(int x0, int y0, int r, int t) : x(x0), y(y0), _x0(x0), _r(r), _t(t) {}
        bool operator!=(const iterator& o) const { return x != o.x || y != o.y; }
        iterator& operator++() {
            ++x;
            if (x >= _r) { x = _x0; ++y; }
            return *this;
        }
    };
    iterator begin() const { return iterator(_x, _y, _r, _t); }
    iterator end() const { return iterator(_x, _t, _r, _t); }

private:
    int _x, _y, _r, _t;
};

}} // namespace DD::Image
HEADER

# --- DeepPixel.h ---
cat > "$TMPDIR/DDImage/DeepPixel.h" << 'HEADER'
#pragma once
#include "Channel.h"
namespace DD { namespace Image {

class DeepPixel {
public:
    enum Ordering { eUnordered = 0, eOrdered = 1 };
    DeepPixel() {}
    int getSampleCount() const { return 0; }
    float getUnorderedSample(int s, Channel z) const { return 0.0f; }
    float getOrderedSample(int s, Channel z) const { return 0.0f; }
};

}} // namespace DD::Image
HEADER

# --- DeepPlane.h ---
cat > "$TMPDIR/DDImage/DeepPlane.h" << 'HEADER'
#pragma once
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include <vector>
namespace DD { namespace Image {

class DeepOutPixel {
public:
    DeepOutPixel() {}
    void clear() {}
    void reserve(int) {}
    void push_back(float) {}
    int size() const { return 0; }
};

class DeepOutputPlane {
public:
    DeepOutputPlane() {}
    DeepOutputPlane(const ChannelSet&, const Box&, DeepPixel::Ordering) {}
    void addPixel(const DeepOutPixel&) {}
    void addHole() {}
};

class DeepPlane {
public:
    DeepPlane() {}
    const Box& box() const { static Box b; return b; }
    DeepPixel getPixel(int y, int x) const { return DeepPixel(); }
    DeepPixel getPixel(const Box::iterator&) const { return DeepPixel(); }
};

}} // namespace DD::Image
HEADER

# --- Knobs.h ---
cat > "$TMPDIR/DDImage/Knobs.h" << 'HEADER'
#pragma once
namespace DD { namespace Image {

class Knob {
public:
    const char* name() const { return ""; }
};
typedef void (*Knob_Callback)(void);

// Stub knob creation functions — signatures match DDImage API
inline Knob* Float_knob(Knob_Callback, float*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Int_knob(Knob_Callback, int*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Enumeration_knob(Knob_Callback, int*, const char* const*, const char*, const char* = nullptr) { return nullptr; }
// File_knob: second param is const char** (pointer-to-const-char-pointer), matching NDK signature
inline Knob* File_knob(Knob_Callback, const char**, const char*, const char* = nullptr) { return nullptr; }
inline void SetRange(Knob_Callback, float, float) {}
inline void SetRange(Knob_Callback, double, double) {}
inline void SetRange(Knob_Callback, int, int) {}
inline void Tooltip(Knob_Callback, const char*) {}
inline Knob* WH_knob(Knob_Callback, double*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Bool_knob(Knob_Callback, bool*, const char*, const char* = nullptr) { return nullptr; }
inline void BeginClosedGroup(Knob_Callback, const char*) {}
inline void Divider(Knob_Callback, const char*) {}
inline void EndGroup(Knob_Callback) {}

}} // namespace DD::Image
HEADER

# --- Iop.h ---
cat > "$TMPDIR/DDImage/Iop.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
namespace DD { namespace Image {

class Info : public Box {
public:
    Info() {}
    void set(const Box& b) { Box::set(b.x(), b.y(), b.r(), b.t()); }
    void set(const Box& b, int /*pad*/) { set(b); }
    void channels(ChannelMask) {}
    void full_size_format(const Format&) {}
    const Format& full_size_format() const { static Format f; return f; }
};

class Iop : public Op {
public:
    Iop(Node* n) : Op(n) {}
    virtual ~Iop() {}
    virtual void _validate(bool) {}
    virtual void _close() {}
    virtual int knob_changed(Knob* k) override { return Op::knob_changed(k); }
    void set_out_channels(ChannelMask) {}
    void copy_info() {}
    void validate(bool b) { _validate(b); }

    Info info_;
};

}} // namespace DD::Image
HEADER

# --- ImagePlane.h ---
cat > "$TMPDIR/DDImage/ImagePlane.h" << 'HEADER'
#pragma once
#include "Box.h"
#include "Channel.h"
namespace DD { namespace Image {

class ImagePlane {
public:
    ImagePlane() {}
    const Box& bounds() const { static Box b; return b; }
    const ChannelSet& channels() const { static ChannelSet c; return c; }
    void makeWritable() {}
    float& writableAt(int /*x*/, int /*y*/, Channel /*z*/) {
        static float dummy = 0.0f;
        return dummy;
    }
    float* writable(Channel, int /*y*/) { return nullptr; }
};

}} // namespace DD::Image
HEADER

# --- PlanarIop.h ---
cat > "$TMPDIR/DDImage/PlanarIop.h" << 'HEADER'
#pragma once
#include "Iop.h"
#include "ImagePlane.h"
namespace DD { namespace Image {

struct RequestOutput {};

class PlanarIop : public Iop {
public:
    PlanarIop(Node* n) : Iop(n) {}
    virtual ~PlanarIop() {}
    virtual void renderStripe(ImagePlane& plane) = 0;
    virtual void getRequests(const Box&, const ChannelSet&, int, RequestOutput&) const {}
    virtual int knob_changed(Knob* k) override { return Iop::knob_changed(k); }
    virtual void _close() override { Iop::_close(); }
};

}} // namespace DD::Image
HEADER

# --- DeepOp.h ---
cat > "$TMPDIR/DDImage/DeepOp.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include "DeepPlane.h"
namespace DD { namespace Image {

struct DeepInfo {
    Box& box() { static Box b; return b; }
    const Box& box() const { static Box b; return b; }
    const Format& full_size_format() const { static Format f; return f; }
};

class DeepOp {
public:
    virtual ~DeepOp() {}
    virtual bool deepEngine(const Box&, const ChannelSet&, DeepPlane&) { return true; }
    void validate(bool) {}
    const DeepInfo& deepInfo() const { static DeepInfo di; return di; }
    void deepRequest(const Box&, const ChannelSet&, int) {}
};

}} // namespace DD::Image
HEADER

# --- DeepFilterOp.h ---
cat > "$TMPDIR/DDImage/DeepFilterOp.h" << 'HEADER'
#pragma once
#include "Op.h"
#include "Box.h"
#include "Channel.h"
#include "DeepPixel.h"
#include "DeepPlane.h"
#include "DeepOp.h"
#include "Knobs.h"
#include <vector>
namespace DD { namespace Image {

struct RequestData {
    RequestData(DeepOp*, const Box&, const ChannelSet&, int) {}
};

class DeepFilterOp : public Op {
public:
    DeepFilterOp(Node* n) : Op(n) {}
    virtual ~DeepFilterOp() {}
    DeepOp* input0() { return &_in; }
    bool test_input(int input, Op* op) const override { return true; }

    virtual void _validate(bool) {}
    virtual void getDeepRequests(Box, const ChannelSet&, int, std::vector<RequestData>&) {}
    virtual bool doDeepEngine(Box, const ChannelSet&, DeepOutputPlane&) { return true; }

protected:
    DeepInfo _deepInfo;
private:
    DeepOp _in;
};

}} // namespace DD::Image
HEADER

# --- Run syntax-only compilation ---
for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPO.cpp; do
    if [ -f "$SRC_DIR/$src_file" ]; then
        echo "Running syntax check: g++ -std=c++17 -fsyntax-only -I$TMPDIR -I$SRC_DIR $SRC_DIR/$src_file"
        g++ -std=c++17 -fsyntax-only -I"$TMPDIR" -I"$SRC_DIR" "$SRC_DIR/$src_file"
        echo "Syntax check passed: $src_file"
    fi
done
# --- S02 grep contracts ---
echo "Checking S02 contracts..."
grep -q 'halton'          "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: halton missing from deepc_po_math.h"; exit 1; }
grep -q 'map_to_disk'     "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: map_to_disk missing from deepc_po_math.h"; exit 1; }
grep -q 'mat2_inverse'    "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: mat2_inverse missing from deepc_po_math.h"; exit 1; }
grep -q '0\.45f'          "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: R wavelength 0.45 missing"; exit 1; }
grep -q '0\.55f'          "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: G wavelength 0.55 missing"; exit 1; }
grep -q '0\.65f'          "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: B wavelength 0.65 missing"; exit 1; }
grep -q 'deepEngine'      "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: deepEngine call missing"; exit 1; }
grep -q 'lt_newton_trace' "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: lt_newton_trace not called in scatter loop"; exit 1; }
grep -q 'halton'          "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: halton not called in scatter loop"; exit 1; }
grep -q 'map_to_disk'     "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: map_to_disk not called in scatter loop"; exit 1; }
# Ensure no leftover stub markers
if grep -q 'S02: replace' "$SRC_DIR/DeepCDefocusPO.cpp"; then
    echo "FAIL: leftover S02 stub marker in DeepCDefocusPO.cpp"; exit 1
fi
echo "S02 contracts: all pass."
# --- S03 grep contracts ---
echo "Checking S03 contracts..."
grep -q 'holdoutConnected'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdoutConnected missing"; exit 1; }
grep -q 'transmittance_at'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: transmittance_at lambda missing"; exit 1; }
grep -q 'holdout_w'             "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdout_w factor missing from splat"; exit 1; }
grep -q 'hzf >= Z'              "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: depth gate (hzf >= Z) missing"; exit 1; }
grep -q 'holdoutOp->deepRequest\|holdout.*deepRequest' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdout deepRequest missing from getRequests"; exit 1; }
test "$(grep -c 'input(1)' "$SRC_DIR/DeepCDefocusPO.cpp")" -ge 3 || { echo "FAIL: input(1) appears fewer than 3 times (label + getRequests + renderStripe)"; exit 1; }
! grep -q 'TODO\|STUB' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: leftover TODO/STUB markers"; exit 1; }
echo "S03 contracts: all pass."
# --- S04 grep contracts ---
echo "Checking S04 contracts..."
grep -q '_focal_length_mm'       "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: _focal_length_mm member missing"; exit 1; }
grep -q 'focal_length'           "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: focal_length knob missing"; exit 1; }
! grep -q 'const float focal_length_mm = 50' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: hardcoded 50mm still in renderStripe"; exit 1; }
! grep -q 'S01 skeleton' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment in HELP string"; exit 1; }
grep -q 'Divider' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: Divider missing from knobs()"; exit 1; }
echo "S04 contracts: all pass."
# --- S05 contracts ---
echo "Checking S05 contracts..."
# Stale S01/S02 comment lines removed
! grep -q 'S01 state: skeleton only' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment still present"; exit 1; }
! grep -q 'S02 replaces the renderStripe' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S02 comment still present"; exit 1; }
# CMake entries present (regression guard)
grep -c 'DeepCDefocusPO' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$' || { echo "FAIL: DeepCDefocusPO not in exactly 2 CMakeLists.txt locations"; exit 1; }
# FILTER_NODES entry
grep -q 'FILTER_NODES.*DeepCDefocusPO\|DeepCDefocusPO.*FILTER_NODES' "$SRC_DIR/CMakeLists.txt" || { echo "FAIL: DeepCDefocusPO not in FILTER_NODES"; exit 1; }
# THIRD_PARTY_LICENSES entry
grep -q 'lentil\|hanatos' "$(dirname "$SRC_DIR")/THIRD_PARTY_LICENSES.md" || { echo "FAIL: lentil/poly.h not in THIRD_PARTY_LICENSES.md"; exit 1; }
# Op::Description registration present
grep -q 'Op::Description' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: Op::Description missing"; exit 1; }
echo "S05 contracts: all pass."
echo "All syntax checks passed."
