#pragma once
// The Merkle tree of a revision (HANDOFF §B6): one hash over the content hashes
// a manifest records, in canonical order, kept in its header as merkle_root.
// Which hashes are its leaves is the manifest's business (manifest.h); here a
// leaf is any BLAKE3 hash.
//
//   leaf = BLAKE3(0x00 || raw32(b3))      node = BLAKE3(0x01 || left || right)
//
// The nodes of each level are paired left to right, and an unpaired node is
// promoted to the next level unchanged (RFC 6962 §2.1 style). With no leaves
// the root is BLAKE3(0x00).
#include "core/hash/hashers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace va {

// Builds the root while the leaves stream in. It keeps one hash per complete
// subtree of the leaves so far, at most one per level of the tree (O(log n)),
// never the leaves themselves.
class MerkleTreeBuilder {
public:
    void add(const Blake3Digest& b3);
    // Adds a leaf given as a manifest writes it: 64 lowercase hex digits.
    // Anything else adds nothing and returns false.
    bool add_hex(std::string_view b3);

    std::uint64_t leaves() const { return leaves_; }

    // The root of the leaves added so far; more may be added afterwards.
    Blake3Digest root() const;
    std::string root_hex() const;  // 64 lowercase hex digits, as merkle_root holds it

private:
    struct Subtree {
        unsigned height = 0;       // a complete subtree of 2^height leaves
        Blake3Digest hash{};
    };
    std::vector<Subtree> subtrees_; // left to right, so their heights decrease
    std::uint64_t leaves_ = 0;
};

// The three hashes of §B6.
Blake3Digest merkle_leaf(const Blake3Digest& b3);
Blake3Digest merkle_node(const Blake3Digest& left, const Blake3Digest& right);
Blake3Digest merkle_empty_root();

} // namespace va
