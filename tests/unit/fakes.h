#pragma once
// Test doubles (R-93): an in-memory file system with error injection, and a
// progress sink that ignores progress. They let core engines be tested on any
// OS without special hardware.
#include "common/utf8.h"
#include "core/interfaces.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>

namespace test {

class NullProgress final : public va::IProgressSink {
public:
    void on_progress(const va::Progress&) override {}
};

// Reports `volume` for every path, except paths below the mount point of one of
// `extra_volumes` (the deepest match wins), like real nested mounts.
class FakeMediaEnumerator final : public va::IMediaEnumerator {
public:
    va::VolumeInfo volume;
    std::vector<va::VolumeInfo> extra_volumes;
    std::optional<va::IoError> error;

    std::expected<std::vector<va::VolumeInfo>, va::IoError> volumes() override
    {
        if (error) {
            return std::unexpected(*error);
        }
        std::vector<va::VolumeInfo> all{volume};
        all.insert(all.end(), extra_volumes.begin(), extra_volumes.end());
        return all;
    }

    std::expected<va::VolumeInfo, va::IoError> volume_for_path(const std::filesystem::path& path) override
    {
        if (error) {
            return std::unexpected(*error);
        }
        const va::VolumeInfo* best = &volume;
        for (const auto& extra : extra_volumes) {
            const auto relative = path.lexically_normal().lexically_relative(extra.mount_point.lexically_normal());
            const bool inside = !relative.empty() && !relative.native().starts_with(std::filesystem::path("..").native());
            if (inside && extra.mount_point.native().size() >= best->mount_point.native().size()) {
                best = &extra;
            }
        }
        return *best;
    }
};

class FakeFileSystem final : public va::IFileSystemAccess {
public:
    struct Node {
        va::EntryType type = va::EntryType::File;
        std::string content;                  // file bytes
        std::string link_target;              // link target
        std::int64_t mtime_ns = 1'700'000'000'000'000'000;
        std::optional<std::uint32_t> attrs;
        bool mount_point = false;
        std::optional<va::IoError> list_error; // directory cannot be listed
        std::optional<va::IoError> open_error; // file cannot be opened
        std::optional<std::uint64_t> read_error_offset; // MediaError from this offset on
    };

    static Node directory_node()
    {
        Node node;
        node.type = va::EntryType::Directory;
        return node;
    }

    explicit FakeFileSystem(std::filesystem::path root) : root_(std::move(root))
    {
        nodes_[key(root_)] = directory_node();
    }

    const std::filesystem::path& root() const { return root_; }

    Node& add_file(const std::string& relative, std::string content)
    {
        add_parents(relative);
        Node node;
        node.content = std::move(content);
        return nodes_[key(path_of(relative))] = node;
    }

    Node& add_directory(const std::string& relative)
    {
        add_parents(relative);
        Node node;
        node.type = va::EntryType::Directory;
        return nodes_[key(path_of(relative))] = node;
    }

    Node& add_link(const std::string& relative, std::string target)
    {
        add_parents(relative);
        Node node;
        node.type = va::EntryType::Link;
        node.link_target = std::move(target);
        return nodes_[key(path_of(relative))] = node;
    }

    Node& node(const std::string& relative) { return nodes_.at(key(path_of(relative))); }
    void remove(const std::string& relative)
    {
        const auto prefix = key(path_of(relative));
        std::erase_if(nodes_, [&](const auto& item) {
            return item.first == prefix || item.first.starts_with(prefix + "/");
        });
    }

    std::uint64_t bytes_read = 0;

    std::expected<std::vector<va::DirEntry>, va::IoError> list(
        const std::filesystem::path& directory) override
    {
        const auto directory_key = key(directory);
        const auto found = nodes_.find(directory_key);
        if (found == nodes_.end()) {
            return std::unexpected(va::IoError{va::IoErrorKind::NotFound, 0, "no such directory"});
        }
        if (found->second.list_error) {
            return std::unexpected(*found->second.list_error);
        }
        std::vector<va::DirEntry> entries;
        for (const auto& [node_key, node] : nodes_) {
            if (node_key.size() <= directory_key.size() + 1 ||
                !node_key.starts_with(directory_key + "/") ||
                node_key.find('/', directory_key.size() + 1) != std::string::npos) {
                continue;
            }
            va::DirEntry entry{};
            entry.native_name = va::path_from_utf8(node_key.substr(directory_key.size() + 1));
            entry.type = node.type;
            entry.size = node.type == va::EntryType::File ? node.content.size() : 0;
            entry.mtime_ns = node.mtime_ns;
            entry.attrs = node.attrs;
            entry.is_mount_point = node.mount_point;
            entries.push_back(entry);
        }
        // Real directories list in arbitrary order; reverse to prove nobody relies on it.
        std::reverse(entries.begin(), entries.end());
        return entries;
    }

    std::expected<std::unique_ptr<va::IByteSource>, va::IoError> open(
        const std::filesystem::path& file, va::ReadOptions) override
    {
        const auto found = nodes_.find(key(file));
        if (found == nodes_.end() || found->second.type != va::EntryType::File) {
            return std::unexpected(va::IoError{va::IoErrorKind::NotFound, 0, "no such file"});
        }
        if (found->second.open_error) {
            return std::unexpected(*found->second.open_error);
        }
        return std::make_unique<Source>(found->second, bytes_read);
    }

    std::expected<std::string, va::IoError> read_link(const std::filesystem::path& link) override
    {
        const auto found = nodes_.find(key(link));
        if (found == nodes_.end() || found->second.type != va::EntryType::Link) {
            return std::unexpected(va::IoError{va::IoErrorKind::NotFound, 0, "no such link"});
        }
        return found->second.link_target;
    }

    std::expected<void, va::IoError> drop_cache(const std::filesystem::path&) override { return {}; }

private:
    class Source final : public va::IByteSource {
    public:
        Source(const Node& node, std::uint64_t& counter) : node_(node), counter_(counter) {}

        std::expected<std::size_t, va::IoError> read(
            std::uint64_t offset, std::span<std::byte> output) override
        {
            if (node_.read_error_offset && offset + output.size() > *node_.read_error_offset) {
                return std::unexpected(va::IoError{va::IoErrorKind::MediaError, 23, "CRC error"});
            }
            if (offset >= node_.content.size()) {
                return 0;
            }
            const auto count = std::min<std::size_t>(output.size(), node_.content.size() - offset);
            std::memcpy(output.data(), node_.content.data() + offset, count);
            counter_ += count;
            return count;
        }
        std::uint64_t size() const override { return node_.content.size(); }
        std::size_t alignment() const override { return 1; }

    private:
        Node node_;
        std::uint64_t& counter_;
    };

    std::filesystem::path path_of(const std::string& relative) const
    {
        return root_ / va::path_from_utf8(relative);
    }

    static std::string key(const std::filesystem::path& path)
    {
        auto text = va::path_to_utf8(path.lexically_normal());
        std::replace(text.begin(), text.end(), '\\', '/');
        while (text.size() > 1 && text.back() == '/') {
            text.pop_back();
        }
        return text;
    }

    void add_parents(const std::string& relative)
    {
        auto parent = path_of(relative).parent_path();
        while (key(parent).size() > key(root_).size()) {
            if (!nodes_.contains(key(parent))) {
                nodes_[key(parent)] = directory_node();
            }
            parent = parent.parent_path();
        }
    }

    std::filesystem::path root_;
    std::map<std::string, Node> nodes_;
};

} // namespace test
