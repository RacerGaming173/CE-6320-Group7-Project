// Tests for the simulated heap (src/heap.*) with no allocation policy: unit tests
// of the boundary-tag primitives, checker negative tests, a large-block test, and
// a randomized stress test that runs the heap checker after every operation.
//   test_heap [stress_ops]
#include "heap.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#ifndef HEAP_CHECK
#error "build tests with -DHEAP_CHECK"
#endif

static int g_failures = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

static bool heap_ok(const Heap& h) {
    std::string err;
    if (h.check(err)) return true;
    std::fprintf(stderr, "  %s\n", err.c_str());
    return false;
}

static bool check_fails_with(const Heap& h, const char* needle) {
    std::string err;
    return !h.check(err) && err.find(needle) != std::string::npos;
}

static const auto no_free_structure = [](uint64_t) {};

// Carves one fresh extension into consecutive allocated blocks of the given sizes.
static std::vector<uint64_t> make_blocks(Heap& h, const std::vector<uint64_t>& sizes) {
    uint64_t total = 0;
    for (uint64_t s : sizes) total += s;
    uint64_t off = h.extend(total);
    std::vector<uint64_t> offs;
    for (size_t i = 0; i < sizes.size(); ++i) {
        offs.push_back(off);
        uint64_t rest = (i + 1 < sizes.size()) ? h.split(off, sizes[i]) : Heap::NIL;
        h.set_alloc(off, true);
        off = rest;
    }
    return offs;
}

static void test_initial_layout() {
    Heap h;
    EXPECT(h.size() == Heap::INITIAL_SIZE);
    EXPECT(h.epilogue() == Heap::FIRST_BLOCK);
    EXPECT(h.is_epilogue(Heap::FIRST_BLOCK));
    EXPECT(h.is_alloc(Heap::PROLOGUE) && h.block_size(Heap::PROLOGUE) == 16);
    EXPECT(h.is_alloc(h.epilogue()) && h.block_size(h.epilogue()) == 0);
    EXPECT(heap_ok(h));
}

static void test_block_size_for() {
    EXPECT(Heap::block_size_for(0) == 32);
    EXPECT(Heap::block_size_for(1) == 32);
    EXPECT(Heap::block_size_for(16) == 32);
    EXPECT(Heap::block_size_for(17) == 48);
    EXPECT(Heap::block_size_for(1000) == 1024);
    EXPECT(Heap::block_size_for(1008) == 1024);
    EXPECT(Heap::block_size_for(1009) == 1040);
}

static void test_extend() {
    Heap h;
    uint64_t b = h.extend(100);
    EXPECT(b == Heap::FIRST_BLOCK);
    EXPECT(h.block_size(b) == 112);
    EXPECT(!h.is_alloc(b) && h.prev_alloc(b));
    EXPECT(h.epilogue() == b + 112);
    EXPECT(!h.prev_alloc(h.epilogue()));
    EXPECT(h.size() == Heap::INITIAL_SIZE + 112);
    EXPECT(heap_ok(h));

    h.set_alloc(b, true);
    EXPECT(h.prev_alloc(h.epilogue()));
    uint64_t c = h.extend(0);
    EXPECT(c == b + 112 && h.block_size(c) == Heap::MIN_BLOCK && h.prev_alloc(c));
    EXPECT(heap_ok(h));
    EXPECT(h.extend_calls() == 3);  // reset + two extends
}

static void test_split_and_alloc_bits() {
    Heap h;
    uint64_t b = h.extend(256);
    uint64_t rest = h.split(b, 48);
    EXPECT(rest == b + 48);
    EXPECT(h.block_size(b) == 48 && h.block_size(rest) == 208);
    EXPECT(!h.is_alloc(rest));
    // Two adjacent free blocks right after a split of a free block: the caller
    // is expected to allocate the front part next.
    EXPECT(check_fails_with(h, "adjacent free"));
    h.set_alloc(b, true);
    EXPECT(h.prev_alloc(rest) && h.get(h.footer(rest)) == h.get(rest));
    EXPECT(heap_ok(h));

    EXPECT(h.split(rest, 192) == Heap::NIL);  // 16 B tail is under MIN_BLOCK
    EXPECT(h.split(rest, 176) == rest + 176);  // 32 B tail is allowed
    h.set_alloc(rest, true);
    EXPECT(heap_ok(h));
    EXPECT(h.split(rest, 4096) == Heap::NIL);  // larger than the block

    // Splitting an allocated block (realloc shrink) keeps it allocated.
    uint64_t big = make_blocks(h, {512})[0];
    uint64_t tail = h.split(big, 64);
    EXPECT(h.is_alloc(big) && !h.is_alloc(tail) && h.prev_alloc(tail));
    EXPECT(h.block_size(tail) == 448 && !h.prev_alloc(h.next_block(tail)));
    EXPECT(heap_ok(h));
}

static void test_coalesce_cases() {
    Heap h;
    std::vector<uint64_t> b = make_blocks(h, {32, 48, 64, 80, 96, 112, 128});
    EXPECT(heap_ok(h));
    std::vector<uint64_t> removed;
    auto record = [&](uint64_t off) { removed.push_back(off); };

    // Case 1: both neighbours allocated.
    h.set_alloc(b[1], false);
    EXPECT(h.coalesce(b[1], record) == b[1]);
    EXPECT(removed.empty() && h.block_size(b[1]) == 48);
    EXPECT(heap_ok(h));

    // Case 2: merge with previous only.
    h.set_alloc(b[2], false);
    EXPECT(h.coalesce(b[2], record) == b[1]);
    EXPECT(removed.size() == 1 && removed[0] == b[1]);
    EXPECT(h.block_size(b[1]) == 48 + 64);
    EXPECT(heap_ok(h));

    // Case 3: merge with next only.
    removed.clear();
    h.set_alloc(b[5], false);
    h.coalesce(b[5], record);
    h.set_alloc(b[4], false);
    removed.clear();
    EXPECT(h.coalesce(b[4], record) == b[4]);
    EXPECT(removed.size() == 1 && removed[0] == b[5]);
    EXPECT(h.block_size(b[4]) == 96 + 112);
    EXPECT(heap_ok(h));

    // Case 4: merge with both (b[1..2] on the left, b[4..5] on the right).
    removed.clear();
    h.set_alloc(b[3], false);
    EXPECT(h.coalesce(b[3], record) == b[1]);
    EXPECT(removed.size() == 2 && removed[0] == b[4] && removed[1] == b[1]);
    EXPECT(h.block_size(b[1]) == 48 + 64 + 80 + 96 + 112);
    EXPECT(h.next_block(b[1]) == b[6] && !h.prev_alloc(b[6]));
    EXPECT(heap_ok(h));

    // Edges: first block (prologue on the left) and last block (epilogue on the right).
    h.set_alloc(b[0], false);
    EXPECT(h.coalesce(b[0], no_free_structure) == b[0]);
    EXPECT(h.block_size(b[0]) == 32 + 400);
    h.set_alloc(b[6], false);
    EXPECT(h.coalesce(b[6], no_free_structure) == b[0]);
    EXPECT(h.next_block(b[0]) == h.epilogue() && !h.prev_alloc(h.epilogue()));
    EXPECT(h.block_size(b[0]) == h.size() - Heap::INITIAL_SIZE);
    EXPECT(heap_ok(h));
}

static void test_extend_coalesces_with_trailing_free() {
    Heap h;
    std::vector<uint64_t> b = make_blocks(h, {64, 64});
    h.set_alloc(b[1], false);
    h.coalesce(b[1], no_free_structure);
    uint64_t n = h.extend(128);
    EXPECT(n == b[1] + 64 && !h.prev_alloc(n));
    EXPECT(check_fails_with(h, "adjacent free"));
    EXPECT(h.coalesce(n, no_free_structure) == b[1]);
    EXPECT(h.block_size(b[1]) == 192);
    EXPECT(heap_ok(h));
}

static void test_link_fields() {
    Heap h;
    std::vector<uint64_t> b = make_blocks(h, {32, 32, 32});
    h.set_alloc(b[1], false);
    h.set_link_prev(b[1], 0x1234560);
    h.set_link_next(b[1], Heap::NIL);
    EXPECT(h.link_prev(b[1]) == 0x1234560 && h.link_next(b[1]) == Heap::NIL);
    EXPECT(h.block_size(b[1]) == 32 && h.get(h.footer(b[1])) == h.get(b[1]));
    EXPECT(heap_ok(h));
}

static void test_checker_detects_corruption() {
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64});
        h.put(h.footer(b[0]), h.get(b[0]) ^ 0x40);
        EXPECT(check_fails_with(h, "footer"));
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64, 64});
        h.set_alloc(b[0], false);
        h.set_alloc(b[1], false);
        EXPECT(check_fails_with(h, "adjacent free"));
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64});
        uint64_t v = h.get(b[1]) & ~Heap::PREV_ALLOC;
        h.put(b[1], v);
        h.put(h.footer(b[1]), v);
        EXPECT(check_fails_with(h, "PREV_ALLOC"));
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64});
        h.put(b[1], (4096 | Heap::ALLOC | Heap::PREV_ALLOC));
        EXPECT(check_fails_with(h, "overruns"));
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64});
        h.put(b[0], 72 | Heap::ALLOC | Heap::PREV_ALLOC);
        EXPECT(check_fails_with(h, "reserved flag bits"));  // size 72: bit 3 lands in the flags
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64});
        h.put(h.epilogue(), Heap::PREV_ALLOC);
        EXPECT(check_fails_with(h, "epilogue"));
    }
    {
        Heap h;
        std::vector<uint64_t> b = make_blocks(h, {64, 64, 64});
        h.set_alloc(b[1], false);
        std::string err;
        EXPECT(h.check(err, [&](uint64_t off) { return off == b[1]; }, 1));
        EXPECT(!h.check(err, [](uint64_t) { return false; }) &&
               err.find("missing from the free structure") != std::string::npos);
        EXPECT(!h.check(err, nullptr, 2) && err.find("free structure holds 2") != std::string::npos);
    }
}

static void test_large_blocks() {
    // malloc-large scale: 26 MiB blocks, growing past the reserved capacity.
    const uint64_t MiB = 1ull << 20;
    Heap h(64 * MiB);
    uint64_t a = h.extend(26 * MiB);
    h.set_alloc(a, true);
    uint64_t b = h.extend(26 * MiB);
    h.set_alloc(b, true);
    uint64_t c = h.extend(26 * MiB);
    h.set_alloc(c, true);
    h.data(Heap::payload(c))[26 * MiB - Heap::OVERHEAD - 1] = 0xAB;
    EXPECT(h.size() == Heap::INITIAL_SIZE + 78 * MiB);
    EXPECT(heap_ok(h));
    h.set_alloc(a, false);
    h.coalesce(a, no_free_structure);
    h.set_alloc(b, false);
    EXPECT(h.coalesce(b, no_free_structure) == a);
    EXPECT(h.block_size(a) == 52 * MiB);
    EXPECT(h.data(Heap::payload(c))[26 * MiB - Heap::OVERHEAD - 1] == 0xAB);
    EXPECT(heap_ok(h));
    h.reset();
    EXPECT(h.size() == Heap::INITIAL_SIZE && heap_ok(h));
}

// Random malloc/free/shrink on the raw heap. Fits are picked at random among
// all fitting free blocks (not a policy), and every live payload is filled with
// a per-block byte so overlap or tag corruption shows up as a wrong byte.
static void test_random_stress(uint64_t ops) {
    struct Live { uint64_t off, req; uint8_t tag; };
    std::mt19937_64 rng(6320);
    auto uniform = [&](uint64_t lo, uint64_t hi) {
        return std::uniform_int_distribution<uint64_t>(lo, hi)(rng);
    };
    auto request_size = [&]() -> uint64_t {
        uint64_t r = uniform(0, 99);
        if (r < 2) return 0;
        if (r < 70) return uniform(1, 64);
        if (r < 95) return uniform(65, 1024);
        return uniform(1025, 65536);
    };

    Heap h(1 << 20);
    std::vector<Live> live;
    std::vector<uint64_t> fits;
    uint64_t next_tag = 1, n_malloc = 0, n_free = 0, n_shrink = 0, n_extend = 0;
    uint64_t peak_live = 0;

    auto fill = [&](const Live& l) { std::memset(h.data(Heap::payload(l.off)), l.tag, l.req); };
    auto intact = [&](const Live& l) {
        const uint8_t* p = h.data(Heap::payload(l.off));
        for (uint64_t i = 0; i < l.req; ++i)
            if (p[i] != l.tag) return false;
        return true;
    };

    for (uint64_t i = 0; i < ops && g_failures == 0; ++i) {
        uint64_t r = uniform(0, 99);
        // Drift the live set up to ~1500 blocks, then hover around it.
        bool do_alloc = live.empty() || (live.size() < 1500 ? r < 60 : r < 45);
        if (do_alloc) {
            uint64_t req = request_size();
            uint64_t asize = Heap::block_size_for(req);
            fits.clear();
            for (uint64_t off = Heap::FIRST_BLOCK; !h.is_epilogue(off); off = h.next_block(off))
                if (!h.is_alloc(off) && h.block_size(off) >= asize) fits.push_back(off);
            uint64_t off;
            if (fits.empty()) {
                off = h.coalesce(h.extend(asize > 4096 ? asize : 4096), no_free_structure);
                ++n_extend;
            } else {
                off = fits[uniform(0, fits.size() - 1)];
            }
            h.split(off, asize);
            h.set_alloc(off, true);
            EXPECT(h.payload_capacity(off) >= req);
            Live l{off, req, uint8_t(next_tag++ % 255 + 1)};
            fill(l);
            live.push_back(l);
            ++n_malloc;
        } else if (r < 90) {
            uint64_t k = uniform(0, live.size() - 1);
            EXPECT(intact(live[k]));
            h.set_alloc(live[k].off, false);
            h.coalesce(live[k].off, no_free_structure);
            live[k] = live.back();
            live.pop_back();
            ++n_free;
        } else {
            Live& l = live[uniform(0, live.size() - 1)];
            uint64_t new_req = l.req / 2;
            uint64_t tail = h.split(l.off, Heap::block_size_for(new_req));
            if (tail != Heap::NIL) h.coalesce(tail, no_free_structure);
            l.req = new_req;
            EXPECT(intact(l));
            ++n_shrink;
        }
        if (live.size() > peak_live) peak_live = live.size();

        std::string err;
        if (!h.check(err)) {
            std::fprintf(stderr, "FAIL stress op %llu: %s\n", (unsigned long long)i, err.c_str());
            ++g_failures;
        }
        if (i % 4096 == 0)
            for (const Live& l : live) EXPECT(intact(l));
    }
    for (const Live& l : live) EXPECT(intact(l));
    std::printf("  stress: %llu ops (%llu malloc, %llu free, %llu shrink, %llu extend), "
                "peak %llu live blocks, heap %llu B\n",
                (unsigned long long)ops, (unsigned long long)n_malloc, (unsigned long long)n_free,
                (unsigned long long)n_shrink, (unsigned long long)n_extend,
                (unsigned long long)peak_live, (unsigned long long)h.size());
}

int main(int argc, char** argv) {
    uint64_t stress_ops = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    struct { const char* name; void (*fn)(); } tests[] = {
        {"initial_layout", test_initial_layout},
        {"block_size_for", test_block_size_for},
        {"extend", test_extend},
        {"split_and_alloc_bits", test_split_and_alloc_bits},
        {"coalesce_cases", test_coalesce_cases},
        {"extend_coalesces_with_trailing_free", test_extend_coalesces_with_trailing_free},
        {"link_fields", test_link_fields},
        {"checker_detects_corruption", test_checker_detects_corruption},
        {"large_blocks", test_large_blocks},
    };
    for (auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    int before = g_failures;
    test_random_stress(stress_ops);
    std::printf("%s random_stress\n", g_failures == before ? "ok  " : "FAIL");

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all heap tests passed\n");
    return 0;
}
