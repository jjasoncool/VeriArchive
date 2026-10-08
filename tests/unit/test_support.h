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
void repository_tests();
void config_tests();
void file_writer_tests();
void platform_tests();
void unicode_tests();
void identity_tests();
void hash_tests();
void fs_access_tests();
void scanner_tests();
void manifest_tests();
