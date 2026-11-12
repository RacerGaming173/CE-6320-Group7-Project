// Cleans a raw shim trace into a clean trace (stdin to stdout), or with --check
// strictly validates a clean trace against the format contract in README.md.
// Prints trace statistics and can append them to a CSV.
//   trace_convert [--check] [--max-ops N] [--name NAME] [--stats-csv FILE] < in > out
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

struct Stats {
    uint64_t in_lines = 0, out_ops = 0;
    uint64_t n_alloc = 0, n_realloc = 0, n_free = 0;
    uint64_t dropped_unknown_free = 0, realloc_unknown_to_alloc = 0, synthetic_free = 0;
    uint64_t bad_lines = 0, realloc_inplace = 0;
    uint64_t live_bytes = 0, peak_live_bytes = 0, live_blocks = 0, peak_live_blocks = 0;
    uint64_t total_req_bytes = 0, max_size = 0, min_size = UINT64_MAX;
    std::vector<uint32_t> sizes;  // request sizes (capped at 4G) for percentiles
};

static std::unordered_map<uint64_t, uint64_t> live;  // id -> requested size
static Stats S;
static bool check_mode = false;
static FILE *out = stdout;
static char wbuf[1 << 20];

static void note_size(uint64_t sz) {
    S.total_req_bytes += sz;
    S.max_size = std::max(S.max_size, sz);
    S.min_size = std::min(S.min_size, sz);
    S.sizes.push_back(sz > 0xffffffffull ? 0xffffffffu : (uint32_t)sz);
}
static void bump_live(int64_t dbytes, int64_t dblocks) {
    S.live_bytes += dbytes;
    S.live_blocks += dblocks;
    S.peak_live_bytes = std::max(S.peak_live_bytes, S.live_bytes);
    S.peak_live_blocks = std::max(S.peak_live_blocks, S.live_blocks);
}
static void out_a(uint64_t id, uint64_t sz) {
    if (!check_mode) fprintf(out, "a %" PRIx64 " %" PRIu64 "\n", id, sz);
    live[id] = sz; S.n_alloc++; S.out_ops++; note_size(sz); bump_live(sz, 1);
}
static void out_f(uint64_t id) {
    if (!check_mode) fprintf(out, "f %" PRIx64 "\n", id);
    auto it = live.find(id);
    bump_live(-(int64_t)it->second, -1);
    live.erase(it); S.n_free++; S.out_ops++;
}
static void out_r(uint64_t old_id, uint64_t sz, uint64_t new_id) {
    if (!check_mode) fprintf(out, "r %" PRIx64 " %" PRIu64 " %" PRIx64 "\n", old_id, sz, new_id);
    auto it = live.find(old_id);
    bump_live((int64_t)sz - (int64_t)it->second, 0);
    live.erase(it);
    live[new_id] = sz;
    if (old_id == new_id) S.realloc_inplace++;
    S.n_realloc++; S.out_ops++; note_size(sz);
}

static bool parse_hex(const char *&p, uint64_t &v) {
    v = 0; const char *s = p;
    for (;; p++) {
        char c = *p; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else break;
        v = (v << 4) | (uint64_t)d;
    }
    return p != s && (p - s) <= 16;
}
static bool parse_dec(const char *&p, uint64_t &v) {
    v = 0; const char *s = p;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    return p != s;
}
static bool sp(const char *&p) { if (*p != ' ') return false; p++; return true; }

static int fail(const char *why, const char *line) {
    fprintf(stderr, "CHECK FAILED at line %" PRIu64 ": %s: %s\n", S.in_lines, why, line);
    return 1;
}

int main(int argc, char **argv) {
    uint64_t max_ops = UINT64_MAX;
    std::string name = "trace", stats_csv;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check_mode = true;
        else if (!strcmp(argv[i], "--max-ops") && i + 1 < argc) max_ops = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--stats-csv") && i + 1 < argc) stats_csv = argv[++i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    setvbuf(stdout, wbuf, _IOFBF, sizeof wbuf);
    live.reserve(1 << 20);

    char line[256];
    while (S.out_ops < max_ops && fgets(line, sizeof line, stdin)) {
        S.in_lines++;
        const char *p = line + 1;
        uint64_t a = 0, sz = 0, b = 0;
        bool ok = false;
        char op = line[0];
        if (op == 'a') ok = sp(p) && parse_hex(p, a) && sp(p) && parse_dec(p, sz);
        else if (op == 'f') ok = sp(p) && parse_hex(p, a);
        else if (op == 'r') ok = sp(p) && parse_hex(p, a) && sp(p) && parse_dec(p, sz) && sp(p) && parse_hex(p, b);
        ok = ok && *p == '\n';
        if (!ok) {
            if (check_mode) return fail("malformed line", line);
            S.bad_lines++; continue;
        }
        if (op == 'a') {
            if (live.count(a)) {  // the shim missed this id's free
                if (check_mode) return fail("alloc of live id", line);
                out_f(a); S.synthetic_free++;
            }
            out_a(a, sz);
        } else if (op == 'f') {
            if (!live.count(a)) {  // allocated before the shim loaded
                if (check_mode) return fail("free of unknown id", line);
                S.dropped_unknown_free++; continue;
            }
            out_f(a);
        } else {  // 'r'
            if (check_mode && sz == 0) return fail("realloc to size 0", line);
            if (!live.count(a)) {  // allocated before the shim loaded; log as an alloc
                if (check_mode) return fail("realloc of unknown id", line);
                if (live.count(b)) { out_f(b); S.synthetic_free++; }
                out_a(b, sz); S.realloc_unknown_to_alloc++; continue;
            }
            if (b != a && live.count(b)) {
                if (check_mode) return fail("realloc to live id", line);
                out_f(b); S.synthetic_free++;
            }
            out_r(a, sz, b);
        }
    }
    fflush(stdout);
    bool truncated = S.out_ops >= max_ops && !feof(stdin) && fgetc(stdin) != EOF;

    std::sort(S.sizes.begin(), S.sizes.end());
    auto pct = [&](double q) -> uint64_t {
        if (S.sizes.empty()) return 0;
        size_t i = (size_t)(q * (S.sizes.size() - 1));
        return S.sizes[i];
    };
    double mean = S.sizes.empty() ? 0 : (double)S.total_req_bytes / S.sizes.size();
    if (S.min_size == UINT64_MAX) S.min_size = 0;

    fprintf(stderr,
            "%s: ops=%" PRIu64 " (a=%" PRIu64 " r=%" PRIu64 " [inplace %" PRIu64 "] f=%" PRIu64 ")%s\n"
            "  sizes: min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64 " p99.9=%" PRIu64
            " max=%" PRIu64 " mean=%.1f\n"
            "  peak live: %" PRIu64 " bytes in %" PRIu64 " blocks; at end: %" PRIu64 " bytes in %" PRIu64 " blocks\n"
            "  cleaning: dropped_unknown_free=%" PRIu64 " realloc_unknown->a=%" PRIu64
            " synthetic_free=%" PRIu64 " bad_lines=%" PRIu64 "\n",
            name.c_str(), S.out_ops, S.n_alloc, S.n_realloc, S.realloc_inplace, S.n_free,
            truncated ? " TRUNCATED" : "", S.min_size, pct(.5), pct(.9), pct(.99), pct(.999),
            S.max_size, mean, S.peak_live_bytes, S.peak_live_blocks, S.live_bytes, S.live_blocks,
            S.dropped_unknown_free, S.realloc_unknown_to_alloc, S.synthetic_free, S.bad_lines);
    if (check_mode) fprintf(stderr, "  CHECK OK\n");

    if (!stats_csv.empty()) {
        FILE *f = fopen(stats_csv.c_str(), "a");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0)
                fprintf(f, "trace,ops,mallocs,reallocs,reallocs_inplace,frees,truncated,size_min,size_p50,"
                           "size_p90,size_p99,size_p999,size_max,size_mean,peak_live_bytes,peak_live_blocks,"
                           "end_live_bytes,end_live_blocks\n");
            fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 "\n",
                    name.c_str(), S.out_ops, S.n_alloc, S.n_realloc, S.realloc_inplace, S.n_free,
                    truncated ? 1 : 0, S.min_size, pct(.5), pct(.9), pct(.99), pct(.999), S.max_size, mean,
                    S.peak_live_bytes, S.peak_live_blocks, S.live_bytes, S.live_blocks);
            fclose(f);
        }
    }
    return 0;
}
