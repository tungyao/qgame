#include <cassert>
#include <cstdio>
#include "core/math/Vec2.h"
#include "core/math/Rect.h"
#include "core/math/Mat3.h"
#include "core/math/Color.h"
#include "core/Handle.h"
#include "core/containers/HandleMap.h"
#include "core/containers/RingBuffer.h"
#include "core/memory/Arena.h"
#include "core/memory/PoolAllocator.h"

using namespace core;

void testVec2() {
    Vec2 a{3.f, 4.f};
    assert(a.length() == 5.f);
    Vec2 n = a.normalized();
    assert(n.x > 0.59f && n.x < 0.61f);
    Vec2 b = a + Vec2{1.f, 0.f};
    assert(b.x == 4.f);
    ::printf("  Vec2 OK\n");
}

void testRect() {
    Rect r{0.f, 0.f, 10.f, 10.f};
    assert(r.contains({5.f, 5.f}));
    assert(!r.contains({11.f, 5.f}));
    Rect r2{5.f, 5.f, 10.f, 10.f};
    assert(r.overlaps(r2));
    Rect inter = r.intersect(r2);
    assert(inter.w == 5.f && inter.h == 5.f);
    ::printf("  Rect OK\n");
}

void testMat3() {
    Mat3 t = Mat3::translation(3.f, 4.f);
    Vec2 p = t.transformPoint({0.f, 0.f});
    assert(p.x == 3.f && p.y == 4.f);
    ::printf("  Mat3 OK\n");
}

using MyHandle = Handle<struct MyTag>;
void testHandleMap() {
    HandleMap<MyHandle, int> map;
    MyHandle h = map.insert(42);
    assert(map.valid(h));
    assert(map.get(h) == 42);
    map.remove(h);
    assert(!map.valid(h));
    ::printf("  HandleMap OK\n");
}

void testRingBuffer() {
    RingBuffer<int, 4> rb;
    assert(rb.push(1) && rb.push(2) && rb.push(3));
    // capacity=4 but 1 slot reserved, so max 3
    int v;
    assert(rb.pop(v) && v == 1);
    assert(rb.pop(v) && v == 2);
    assert(rb.pop(v) && v == 3);
    assert(!rb.pop(v));
    ::printf("  RingBuffer OK\n");
}

void testArena() {
    Arena a{1024};
    int* p = a.create<int>(99);
    assert(*p == 99);
    assert(a.used() >= sizeof(int));
    a.reset();
    assert(a.used() == 0);
    ::printf("  Arena OK\n");
}

int main() {
    ::printf("Running core tests...\n");
    testVec2();
    testRect();
    testMat3();
    testHandleMap();
    testRingBuffer();
    testArena();
    ::printf("All tests passed.\n");
    return 0;
}
