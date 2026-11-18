// Simulated heap: storage growth, tag maintenance, split, and the heap checker.
#include "heap.h"

void Heap::reset(uint64_t reserve_bytes) {
    std::vector<uint8_t>().swap(mem_);
    mem_.reserve(reserve_bytes < INITIAL_SIZE ? INITIAL_SIZE : reserve_bytes);
    extend_calls_ = 0;
    heap_extend(INITIAL_SIZE);
    put(PROLOGUE, FIRST_BLOCK | ALLOC | PREV_ALLOC);
    put(PROLOGUE + WSIZE, FIRST_BLOCK | ALLOC | PREV_ALLOC);
    put(epilogue(), 0 | ALLOC | PREV_ALLOC);
}

uint64_t Heap::heap_extend(uint64_t n) {
    uint64_t old_brk = mem_.size();
    mem_.resize(old_brk + n);
    ++extend_calls_;
    return old_brk;
}

uint64_t Heap::extend(uint64_t n) {
    n = align_up(n);
    if (n < MIN_BLOCK) n = MIN_BLOCK;
    uint64_t off = epilogue();
    uint64_t pa = get(off) & PREV_ALLOC;
    heap_extend(n);  // the old epilogue word becomes the new block's header
    write_tags(off, n, pa);
    put(epilogue(), 0 | ALLOC);
    return off;
}

void Heap::set_prev_alloc_bit(uint64_t off, bool prev_is_alloc) {
    uint64_t h = get(off);
    h = prev_is_alloc ? (h | PREV_ALLOC) : (h & ~PREV_ALLOC);
    put(off, h);
    if (!is_epilogue(off)) put(footer(off), h);
}

void Heap::set_alloc(uint64_t off, bool alloc) {
    uint64_t size = block_size(off);
    write_tags(off, size, (get(off) & PREV_ALLOC) | (alloc ? ALLOC : 0));
    set_prev_alloc_bit(off + size, alloc);
}

uint64_t Heap::split(uint64_t off, uint64_t asize) {
    uint64_t total = block_size(off);
    if (total < asize + MIN_BLOCK) return NIL;
    uint64_t flags = get(off) & FLAG_MASK;
    write_tags(off, asize, flags);
    uint64_t rest = off + asize;
    write_tags(rest, total - asize, (flags & ALLOC) ? PREV_ALLOC : 0);
    set_prev_alloc_bit(rest + total - asize, false);
    return rest;
}

#ifdef HEAP_CHECK
static bool fail(std::string& err, uint64_t off, const std::string& what) {
    err = "heap check: block @" + std::to_string(off) + ": " + what;
    return false;
}

bool Heap::check(std::string& err,
                 const std::function<bool(uint64_t)>& in_free_structure,
                 uint64_t expected_free) const {
    if (size() < INITIAL_SIZE || (size() - WSIZE) % ALIGN != 0)
        return fail(err, 0, "heap size " + std::to_string(size()) + " is not 16k+8");
    const uint64_t pro = FIRST_BLOCK | ALLOC | PREV_ALLOC;
    if (get(PROLOGUE) != pro || get(PROLOGUE + WSIZE) != pro)
        return fail(err, PROLOGUE, "bad prologue");

    const uint64_t end = epilogue();
    bool prev_is_alloc = true;
    uint64_t n_free = 0;
    for (uint64_t off = FIRST_BLOCK; off != end;) {
        uint64_t h = get(off);
        uint64_t sz = h & ~FLAG_MASK;
        if (off % ALIGN != 0) return fail(err, off, "misaligned");
        if (h & FLAG_MASK & ~(ALLOC | PREV_ALLOC)) return fail(err, off, "reserved flag bits set");
        if (sz < MIN_BLOCK) return fail(err, off, "size " + std::to_string(sz) + " below minimum");
        if (sz > end - off)
            return fail(err, off, "size " + std::to_string(sz) + " overruns the epilogue (overlap or out of bounds)");
        if (get(off + sz - WSIZE) != h) return fail(err, off, "header does not match footer");
        if (bool(h & PREV_ALLOC) != prev_is_alloc) return fail(err, off, "PREV_ALLOC bit is wrong");
        bool alloc = h & ALLOC;
        if (!alloc) {
            if (!prev_is_alloc) return fail(err, off, "adjacent free blocks (missed coalesce)");
            if (in_free_structure && !in_free_structure(off))
                return fail(err, off, "free block missing from the free structure");
            ++n_free;
        }
        prev_is_alloc = alloc;
        off += sz;
    }
    uint64_t e = get(end);
    if ((e & ~PREV_ALLOC) != ALLOC) return fail(err, end, "bad epilogue");
    if (bool(e & PREV_ALLOC) != prev_is_alloc) return fail(err, end, "epilogue PREV_ALLOC bit is wrong");
    if (expected_free != NIL && expected_free != n_free)
        return fail(err, end, "free structure holds " + std::to_string(expected_free) +
                                  " blocks but the heap has " + std::to_string(n_free));
    return true;
}
#endif
