#pragma once
#include <iostream>

inline int test_failures = 0;

#define CHECK(expression)                                                                  \
    do {                                                                                   \
        if (!(expression)) {                                                               \
            std::cerr << __FILE__ << ':' << __LINE__                                       \
                      << ": CHECK failed: " #expression "\n";                            \
            ++test_failures;                                                               \
        }                                                                                  \
    } while (false)

void archive_path_tests();
void canonical_json_tests();
void config_tests();
