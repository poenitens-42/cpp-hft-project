#include "protocol/itch_parser.hpp"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================
// itch_stats.cpp
//
// Walks an entire real ITCH 5.0 file (mmap, zero-copy) using
// itch_parser's self-describing length-prefix framing. Reports:
//   - whether the framing held for the whole file (no truncation)
//   - a message-type histogram
//   - distinct symbols seen via Stock Directory messages
//
// This is the validation pass before trusting the parser for
// LOB replay — run it against the whole file, not a sample.
//
// Usage: itch_stats <path-to-itch-file>
// ============================================================

// Symbol field is 8 space-padded bytes — trim for display.
std::string trim_symbol(const uint8_t* s) {
    std::string sym(reinterpret_cast<const char*>(s), 8);
    while (!sym.empty() && sym.back() == ' ') sym.pop_back();
    return sym;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <path-to-itch-file>\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "Failed to open %s\n", path);
        return 1;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0) {
        std::fprintf(stderr, "fstat failed on %s\n", path);
        close(fd);
        return 1;
    }
    size_t file_size = static_cast<size_t>(st.st_size);

    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed on %s\n", path);
        close(fd);
        return 1;
    }
    // Hint the kernel we'll read this sequentially, start to finish —
    // triggers more aggressive readahead than the default.
    madvise(mapped, file_size, MADV_SEQUENTIAL);

    std::printf("Mapped %s: %.2f GB\n", path, file_size / 1e9);

    std::set<std::string> symbols;
    uint64_t stock_directory_count = 0;

    hft::itch::ItchCallbacks cb;
    cb.on_stock_directory = [&](const hft::itch::StockDirectoryMsg& m) {
        symbols.insert(trim_symbol(m.stock));
        ++stock_directory_count;
    };

    const uint8_t* data = static_cast<const uint8_t*>(mapped);
    hft::itch::ParseStats stats = hft::itch::parse_itch_buffer(data, file_size, cb);

    munmap(mapped, file_size);
    close(fd);

    // ---- Report ----
    std::printf("\n========== Parse Result ==========\n");
    std::printf("Total messages parsed : %llu\n", (unsigned long long)stats.total_messages);
    std::printf("Total bytes consumed  : %llu / %zu\n",
                (unsigned long long)stats.total_bytes_consumed, file_size);
    std::printf("Ended cleanly         : %s\n", stats.ended_cleanly ? "YES" : "NO");
    if (!stats.ended_cleanly) {
        std::printf("  -> framing broke at byte offset %llu\n",
                    (unsigned long long)stats.truncated_at_offset);
        std::printf("  -> %zu bytes left unparsed at end of file\n",
                    file_size - stats.total_bytes_consumed);
    }

    std::printf("\n--- Message type histogram ---\n");
    std::vector<std::pair<char, uint64_t>> sorted_counts(
        stats.counts_by_type.begin(), stats.counts_by_type.end());
    std::sort(sorted_counts.begin(), sorted_counts.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [type, count] : sorted_counts) {
        std::printf("  %c : %llu\n", type, (unsigned long long)count);
    }

    std::printf("\nDistinct symbols (Stock Directory 'R' messages): %zu\n", symbols.size());
    std::printf("Stock Directory message count: %llu\n", (unsigned long long)stock_directory_count);
    if (!symbols.empty()) {
        std::printf("First few symbols: ");
        int shown = 0;
        for (auto& s : symbols) {
            if (shown++ >= 10) break;
            std::printf("%s ", s.c_str());
        }
        std::printf("\n");
    }

    return stats.ended_cleanly ? 0 : 1;
}
