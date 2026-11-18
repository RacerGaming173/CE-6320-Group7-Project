// Simulated heap: an sbrk-style byte array with a boundary-tag block layout.
// Blocks are named by the offset of their header. Block offsets and sizes are
// multiples of 16; every block has a header and a footer holding size | flags.
//
//   0        8        16                                  brk-8   brk
//   | pro hdr| pro ftr| block | block | ... | block | epilogue hdr |
//
// The heap knows nothing about free structures: policies supply a callback to
// coalesce() so they can unlink neighbours before they are absorbed.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

class Heap {
public:
    static constexpr uint64_t WSIZE = 8;
    static constexpr uint64_t ALIGN = 16;
    static constexpr uint64_t OVERHEAD = 2 * WSIZE;     // header + footer
    static constexpr uint64_t MIN_BLOCK = 4 * WSIZE;    // header + prev + next + footer
    static constexpr uint64_t ALLOC = 0x1;
    static constexpr uint64_t PREV_ALLOC = 0x2;
    static constexpr uint64_t FLAG_MASK = ALIGN - 1;
    static constexpr uint64_t NIL = ~0ull;
    static constexpr uint64_t PROLOGUE = 0;
    static constexpr uint64_t FIRST_BLOCK = 2 * WSIZE;
    static constexpr uint64_t INITIAL_SIZE = 3 * WSIZE; // prologue + epilogue

    explicit Heap(uint64_t reserve_bytes = 0) { reset(reserve_bytes); }

    // Frees all storage and lays out an empty heap. Reserving the expected peak
    // up front keeps the vector from doubling (old + new buffer) mid-replay.
    void reset(uint64_t reserve_bytes = 0);

    // Appends a free block of at least n bytes where the epilogue was and returns
    // its offset. The block is not coalesced with a free predecessor.
    uint64_t extend(uint64_t n);

    static uint64_t align_up(uint64_t n) { return (n + ALIGN - 1) & ~(ALIGN - 1); }
    static uint64_t block_size_for(uint64_t request) {
        uint64_t s = align_up(request + OVERHEAD);
        return s < MIN_BLOCK ? MIN_BLOCK : s;
    }

    uint64_t size() const { return mem_.size(); }
    uint64_t epilogue() const { return mem_.size() - WSIZE; }
    uint64_t extend_calls() const { return extend_calls_; }

    uint64_t get(uint64_t off) const { uint64_t v; std::memcpy(&v, &mem_[off], WSIZE); return v; }
    void put(uint64_t off, uint64_t v) { std::memcpy(&mem_[off], &v, WSIZE); }
    // Invalidated by extend() if it grows past the reserved capacity.
    uint8_t* data(uint64_t off) { return mem_.data() + off; }

    uint64_t block_size(uint64_t off) const { return get(off) & ~FLAG_MASK; }
    bool is_alloc(uint64_t off) const { return get(off) & ALLOC; }
    bool prev_alloc(uint64_t off) const { return get(off) & PREV_ALLOC; }
    bool is_epilogue(uint64_t off) const { return off == epilogue(); }
    uint64_t footer(uint64_t off) const { return off + block_size(off) - WSIZE; }
    static uint64_t payload(uint64_t off) { return off + WSIZE; }
    static uint64_t block_of(uint64_t payload_off) { return payload_off - WSIZE; }
    uint64_t payload_capacity(uint64_t off) const { return block_size(off) - OVERHEAD; }

    uint64_t next_block(uint64_t off) const { return off + block_size(off); }
    // Reads the previous block's footer, so it is valid only if that block has one.
    uint64_t prev_block(uint64_t off) const { return off - (get(off - WSIZE) & ~FLAG_MASK); }

    // Explicit free-list links stored in a free block's payload.
    uint64_t link_prev(uint64_t off) const { return get(off + WSIZE); }
    uint64_t link_next(uint64_t off) const { return get(off + 2 * WSIZE); }
    void set_link_prev(uint64_t off, uint64_t v) { put(off + WSIZE, v); }
    void set_link_next(uint64_t off, uint64_t v) { put(off + 2 * WSIZE, v); }

    // Sets the alloc bit in both tags and the next block's PREV_ALLOC bit.
    void set_alloc(uint64_t off, bool alloc);

    // Shrinks block off to asize (16-aligned) and turns the tail into a free block,
    // returning its offset, or NIL if the tail would be under MIN_BLOCK. The first
    // part keeps its alloc bit. The tail is not coalesced with its successor.
    uint64_t split(uint64_t off, uint64_t asize);

    // Merges free block off with free physical neighbours and returns the merged
    // block's offset. remove_free(nbr) is called for each neighbour before it is
    // absorbed, while its link fields are still intact.
    template <class RemoveFree>
    uint64_t coalesce(uint64_t off, RemoveFree&& remove_free);

#ifdef HEAP_CHECK
    // Structural check. in_free_structure, if given, must accept every free block;
    // expected_free, if not NIL, must equal the number of free blocks.
    bool check(std::string& err,
               const std::function<bool(uint64_t)>& in_free_structure = nullptr,
               uint64_t expected_free = NIL) const;
#endif

private:
    uint64_t heap_extend(uint64_t n);  // the sbrk analogue: the only place mem_ grows
    void write_tags(uint64_t off, uint64_t size, uint64_t flags) {
        put(off, size | flags);
        put(off + size - WSIZE, size | flags);
    }
    void set_prev_alloc_bit(uint64_t off, bool prev_is_alloc);

    std::vector<uint8_t> mem_;
    uint64_t extend_calls_ = 0;
};

template <class RemoveFree>
uint64_t Heap::coalesce(uint64_t off, RemoveFree&& remove_free) {
    uint64_t size = block_size(off);
    uint64_t start = off;
    uint64_t next = off + size;
    if (!is_alloc(next)) {  // the epilogue is allocated, so this stops at the end
        remove_free(next);
        size += block_size(next);
    }
    if (!prev_alloc(off)) {  // the prologue is allocated, so this stops at the start
        uint64_t prev = prev_block(off);
        remove_free(prev);
        size += block_size(prev);
        start = prev;
    }
    write_tags(start, size, get(start) & PREV_ALLOC);
    set_prev_alloc_bit(start + size, false);
    return start;
}
