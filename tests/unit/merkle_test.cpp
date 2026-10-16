#include "test_support.h"

#include "common/hex.h"
#include "core/hash/hashers.h"
#include "core/merkle/merkle.h"

#include <algorithm>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

va::Blake3Digest digest(std::string_view hex)
{
    va::Blake3Digest result{};
    const auto bytes = va::from_hex(hex);
    if (bytes && bytes->size() == result.size()) {
        std::copy(bytes->begin(), bytes->end(), result.begin());
    }
    return result;
}

// The leaves of the vectors below: the BLAKE3 hashes of "0", "1", "2", ...
std::vector<std::string> sample_hashes(std::size_t count)
{
    std::vector<std::string> hashes;
    for (std::size_t index = 0; index < count; ++index) {
        hashes.push_back(va::blake3_hex(std::to_string(index)));
    }
    return hashes;
}

std::string root_of(const std::vector<std::string>& hashes)
{
    va::MerkleTreeBuilder builder;
    for (const auto& hash : hashes) {
        CHECK(builder.add_hex(hash));
    }
    return builder.root_hex();
}

// BLAKE3 of one prefix byte and the given hashes, through the streaming hasher
// rather than the module's own functions.
va::Blake3Digest prefixed(std::byte prefix, std::initializer_list<va::Blake3Digest> parts)
{
    va::Blake3Hasher hasher;
    hasher.update(std::span(&prefix, 1));
    for (const auto& part : parts) {
        hasher.update(part);
    }
    return digest(hasher.finish_hex());
}

va::Blake3Digest leaf(const va::Blake3Digest& b3)
{
    return prefixed(std::byte{0x00}, {b3});
}

va::Blake3Digest node(const va::Blake3Digest& left, const va::Blake3Digest& right)
{
    return prefixed(std::byte{0x01}, {left, right});
}

// §B6 read literally: pair the nodes of a level left to right, promote an
// unpaired last node unchanged, and do the same with the next level until
// one node is left.
va::Blake3Digest level_root(const std::vector<va::Blake3Digest>& level)
{
    if (level.size() == 1) {
        return level.front();
    }
    std::vector<va::Blake3Digest> next;
    for (std::size_t index = 0; index + 1 < level.size(); index += 2) {
        next.push_back(node(level[index], level[index + 1]));
    }
    if (level.size() % 2 == 1) {
        next.push_back(level.back());
    }
    return level_root(next);
}

// RFC 6962 §2.1: split at the largest power of two below the number of
// leaves. It builds the same tree.
va::Blake3Digest split_root(std::span<const va::Blake3Digest> leaves)
{
    if (leaves.size() == 1) {
        return leaves.front();
    }
    std::size_t split = 1;
    while (split * 2 < leaves.size()) {
        split *= 2;
    }
    return node(split_root(leaves.first(split)), split_root(leaves.subspan(split)));
}

// The root by both references; zero leaves give BLAKE3(0x00).
std::pair<std::string, std::string> reference_roots(const std::vector<std::string>& hashes)
{
    if (hashes.empty()) {
        const auto empty = va::to_hex(prefixed(std::byte{0x00}, {}));
        return {empty, empty};
    }
    std::vector<va::Blake3Digest> leaves;
    for (const auto& hash : hashes) {
        leaves.push_back(leaf(digest(hash)));
    }
    return {va::to_hex(level_root(leaves)), va::to_hex(split_root(leaves))};
}

// Roots computed outside this code base: with a BLAKE3 written in Python from
// its specification (and checked against the official BLAKE3 test vectors),
// and §B6 applied by hand, level by level and by RFC 6962's split.
void pinned_roots()
{
    const std::pair<std::size_t, std::string_view> vectors[] = {
        {0, "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
        {1, "56e773c6f9c7fc6b3d6c2d4bff7417f284ec16528e6e01bc5f4be09b3a182df9"},
        {2, "f9708ea7c870223012b6ce5ce273e4fd4e80c11eb16fb5cb25a8177a08a01829"},
        {3, "1f37ade94891a14a4dbf7f94e1a2c164559c27ede09307f51dc61a0fb17d2aa1"},
        {4, "441cf4fe08902b21846f1bfa7505467d4bf938c91913ff173af1460421adaf2a"},
        {5, "f577e943031c2873623015c6e46ea37bf3c3931998b1f155c8cbb9d5629cb162"},
        {7, "c4468b23d7629dd91df06ad38b5f36489f47d946398b59eb02c11a5ab687ea4b"},
        {8, "38f34cdd2b6838784142ce0628702524cc4bf829435771166a59e640a08173ca"},
        {300, "3f4cc03dcef442c46e7d230177d82636af8dbea80d34aa7e51feaf6f8787bc80"},
    };
    for (const auto& [count, root] : vectors) {
        const bool pinned = root_of(sample_hashes(count)) == root;
        CHECK(pinned);
        if (!pinned) {
            std::cerr << "  Merkle root of " << count << " leaves\n";
        }
    }
    // No leaves: BLAKE3 of the single byte 0x00.
    CHECK(va::to_hex(va::merkle_empty_root()) == vectors[0].second);
}

// The rules of §B6 spelled out on small trees.
void rules()
{
    const auto hashes = sample_hashes(5);
    std::vector<va::Blake3Digest> leaves;
    for (const auto& hash : hashes) {
        leaves.push_back(leaf(digest(hash)));
    }
    CHECK(va::merkle_leaf(digest(hashes[0])) == leaves[0]);
    CHECK(va::merkle_node(leaves[0], leaves[1]) == node(leaves[0], leaves[1]));
    CHECK(va::merkle_node(leaves[0], leaves[1]) != va::merkle_node(leaves[1], leaves[0]));
    // The prefixes keep a leaf from passing for a node.
    CHECK(va::merkle_leaf(digest(hashes[0])) != va::blake3_digest(digest(hashes[0])));

    // One leaf: the root is the leaf hash, not the content hash itself.
    CHECK(root_of({hashes[0]}) == va::to_hex(leaves[0]));
    CHECK(root_of({hashes[0]}) != hashes[0]);
    CHECK(root_of({hashes[0], hashes[1]}) == va::to_hex(node(leaves[0], leaves[1])));
    // Three leaves: the third has no partner and is promoted unchanged, not
    // hashed with itself.
    const auto first_pair = node(leaves[0], leaves[1]);
    CHECK(root_of({hashes[0], hashes[1], hashes[2]}) == va::to_hex(node(first_pair, leaves[2])));
    CHECK(root_of({hashes[0], hashes[1], hashes[2]}) !=
          va::to_hex(node(first_pair, node(leaves[2], leaves[2]))));
    // Five leaves: the fifth is promoted over two levels.
    const auto first_four = node(first_pair, node(leaves[2], leaves[3]));
    CHECK(root_of(hashes) == va::to_hex(node(first_four, leaves[4])));
}

// The streaming builder against both references for 0 to 300 leaves, reading
// its root after every leaf while more are still to come.
void builder_matches_the_references()
{
    std::vector<std::string> hashes;
    va::MerkleTreeBuilder builder;
    for (std::size_t count = 0; count <= 300; ++count) {
        const auto [by_levels, by_split] = reference_roots(hashes);
        const auto root = builder.root_hex();
        const bool same = builder.leaves() == count && root == by_levels && root == by_split &&
                          root_of(hashes) == root && builder.root() == digest(root);
        CHECK(same);
        if (!same) {
            std::cerr << "  Merkle root of " << count << " leaves\n";
            return;
        }
        hashes.push_back(va::blake3_hex(std::to_string(count)));
        CHECK(builder.add_hex(hashes.back()));
    }
}

// A leaf is a hash as manifests write it: 64 lowercase hex digits.
void leaves_are_hashes()
{
    CHECK(va::is_hash_hex(std::string(64, 'a')));
    CHECK(va::is_hash_hex(va::blake3_hex("x")));
    va::MerkleTreeBuilder builder;
    for (const std::string& text : {std::string{}, std::string(63, 'a'), std::string(65, 'a'),
             std::string(64, 'A'), std::string(64, 'g'), std::string(32, 'a') + std::string(32, ' ')}) {
        CHECK(!va::is_hash_hex(text));
        CHECK(!builder.add_hex(text));
    }
    CHECK(builder.leaves() == 0);
    CHECK(builder.root() == va::merkle_empty_root());
    CHECK(builder.add_hex(std::string(64, 'a')) && builder.leaves() == 1);
    CHECK(builder.root() == va::merkle_leaf(digest(std::string(64, 'a'))));
}

} // namespace

void merkle_tests()
{
    pinned_roots();
    rules();
    builder_matches_the_references();
    leaves_are_hashes();
}
