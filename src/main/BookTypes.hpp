#pragma once

#include <vector>

// Aggregated price level used both by the historical L3 book and by
// per-engine simulated views.  The book itself remains order-id based;
// this structure is the safe read-only snapshot boundary.
struct BookLevel {
    double price = 0.0;
    long long size = 0;
};

// Top-of-book/depth snapshot.  Bids are sorted best-to-worst
// (descending price), asks are sorted best-to-worst (ascending price).
struct BookSnapshot {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};
