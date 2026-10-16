#include "core/merkle/merkle.h"

#include "common/hex.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace va {
namespace {

// Domain separation (§B6): a leaf can never be read as a node, or a node as
// a leaf, so no two different trees have the same root.
constexpr std::byte leaf_prefix{0x00};
constexpr std::byte node_prefix{0x01};

} // namespace

Blake3Digest merkle_leaf(const Blake3Digest& b3)
{
    std::array<std::byte, 1 + 32> input{};
    input[0] = leaf_prefix;
    std::copy(b3.begin(), b3.end(), input.begin() + 1);
    return blake3_digest(input);
}

Blake3Digest merkle_node(const Blake3Digest& left, const Blake3Digest& right)
{
    std::array<std::byte, 1 + 32 + 32> input{};
    input[0] = node_prefix;
    std::copy(left.begin(), left.end(), input.begin() + 1);
    std::copy(right.begin(), right.end(), input.begin() + 1 + 32);
    return blake3_digest(input);
}

Blake3Digest merkle_empty_root()
{
    const std::array<std::byte, 1> input{leaf_prefix};
    return blake3_digest(input);
}

void MerkleTreeBuilder::add(const Blake3Digest& b3)
{
    Subtree joined{0, merkle_leaf(b3)};
    // Two complete subtrees of the same height are the two halves of the
    // next one up: the one already here is the left half.
    while (!subtrees_.empty() && subtrees_.back().height == joined.height) {
        joined.hash = merkle_node(subtrees_.back().hash, joined.hash);
        ++joined.height;
        subtrees_.pop_back();
    }
    subtrees_.push_back(joined);
    ++leaves_;
}

bool MerkleTreeBuilder::add_hex(std::string_view b3)
{
    if (!is_hash_hex(b3)) {
        return false;
    }
    const auto bytes = from_hex(b3);
    Blake3Digest digest{};
    std::copy(bytes->begin(), bytes->end(), digest.begin());
    add(digest);
    return true;
}

Blake3Digest MerkleTreeBuilder::root() const
{
    if (subtrees_.empty()) {
        return merkle_empty_root();
    }
    // Level by level, a smaller subtree on the right has no partner of its
    // height, so it is promoted unchanged until it meets the next larger one
    // on its left: the root joins them from the right.
    auto hash = subtrees_.back().hash;
    for (auto left = std::next(subtrees_.rbegin()); left != subtrees_.rend(); ++left) {
        hash = merkle_node(left->hash, hash);
    }
    return hash;
}

std::string MerkleTreeBuilder::root_hex() const
{
    return to_hex(root());
}

} // namespace va
