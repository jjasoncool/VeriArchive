#include "test_support.h"
#include "common/config.h"
#include "common/json_error.h"
#include "common/utf8.h"
#include "core/write_file.h"
#include "platform/file_writer.h"
#include "platform/platform_services.h"

#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>

void config_tests()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("va-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "config.json";
    const auto writer = va::platform::make_file_writer();
    const auto save = [&](const va::Config& config) {
        return va::write_file_atomically(*writer, path, va::config_to_json(config)).has_value();
    };

    const auto missing = va::read_config(path);
    CHECK(missing && !*missing);

    va::Config original;
    original.repository = va::path_from_utf8("D:\\中文\\archive");
    original.policy.strict_metadata = true;
    original.policy.full_verify_max_age_days = 17;
    original.allow_network_repository = true;
    original.trusted_keys.push_back({"key", "abcd", "operator note"});
    original.document["io"]["rotational_override"]["disk-serial"] = true;
    original.document["extension"] = { {"keep", "unchanged"} };

    CHECK(save(original));
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto loaded = va::read_config(path);
        CHECK(loaded && loaded->has_value());
        if (!loaded || !*loaded) {
            break;
        }
        const auto& config = **loaded;
        CHECK(config.repository == original.repository);
        CHECK(config.policy.strict_metadata);
        CHECK(config.policy.full_verify_max_age_days == 17);
        CHECK(config.allow_network_repository);
        CHECK(config.trusted_keys.size() == 1);
        CHECK(config.document["io"] == original.document["io"]);
        CHECK(config.rotational_override == (std::map<std::string, bool>{{"disk-serial", true}}));
        CHECK(config.document["extension"] == original.document["extension"]);
        CHECK(save(config));
    }

    // R-111: io.rotational_override gives a device serial "hdd" or "ssd" (or
    // true or false); anything else makes the config invalid. Sample serials.
    std::ofstream(path) << R"({"io":{"rotational_override":{"SAMPLE-1":"hdd","SAMPLE-2":"ssd","SAMPLE-3":false}}})";
    const auto overrides = va::read_config(path);
    CHECK(overrides && *overrides &&
          (**overrides).rotational_override ==
              (std::map<std::string, bool>{{"SAMPLE-1", true}, {"SAMPLE-2", false}, {"SAMPLE-3", false}}));
    std::ofstream(path) << R"({"io":{"rotational_override":{"SAMPLE-1":"fast"}}})";
    const auto bad_value = va::read_config(path);
    CHECK(!bad_value &&
          bad_value.error().message == "io.rotational_override must give each device serial \"hdd\" or \"ssd\"");
    std::ofstream(path) << R"({"io":{"rotational_override":["SAMPLE-1"]}})";
    const auto bad_list = va::read_config(path);
    CHECK(!bad_list &&
          bad_list.error().message == "io.rotational_override must give each device serial \"hdd\" or \"ssd\"");
    std::ofstream(path) << R"({"io":"hdd"})";
    const auto bad_io = va::read_config(path);
    CHECK(!bad_io && bad_io.error().message == "io must be an object");

    // Messages carry the JSON library's sentence without its
    // "[json.exception.<kind>.<id>] " tag, which means nothing to people.
    std::ofstream(path) << "{not valid JSON";
    const auto invalid = va::read_config(path);
    CHECK(!invalid && invalid.error().message.starts_with("Invalid config: parse error at line 1, column "));
    std::ofstream(path) << "{\"repository\":42}";
    const auto wrong_type = va::read_config(path);
    CHECK(!wrong_type && wrong_type.error().message == "Invalid config: type must be string, but is number");
    std::ofstream(path) << "{\"trusted_keys\":[{}]}";
    const auto missing_key = va::read_config(path);
    CHECK(!missing_key && missing_key.error().message == "Invalid config: key 'key_id' not found");
    CHECK(va::json_error_text(std::runtime_error("[json.exception.x.1] y")) == "y");
    CHECK(va::json_error_text(std::runtime_error("plain text")) == "plain text");
    CHECK(va::json_error_text(std::runtime_error("[json.exception.no end")) == "[json.exception.no end");

    // R-105
    for (const char* text : {"{}", "{\"policy\":{}}"}) {
        std::ofstream(path) << text;
        const auto defaults = va::read_config(path);
        CHECK(defaults && defaults->has_value());
        if (defaults && *defaults) {
            CHECK((*defaults)->policy.full_verify_max_age_days == 365);
            CHECK((*defaults)->policy.any_verify_max_age_days == 180);
        }
    }

    const auto sentinel = root / ".veriarchive-write-test";
    std::ofstream(sentinel) << "must survive";
    CHECK(va::platform::writable_directory(root));
    std::ifstream input(sentinel);
    std::string contents;
    std::getline(input, contents);
    CHECK(contents == "must survive");
    input.close();

    std::error_code error;
    std::filesystem::remove_all(root, error);
}