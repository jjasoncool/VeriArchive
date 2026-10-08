#include "test_support.h"
#include "common/config.h"
#include "common/utf8.h"
#include "platform/platform_services.h"

#include <chrono>
#include <fstream>

void config_tests()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("va-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "config.json";

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

    CHECK(va::write_config(path, original));
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
        CHECK(config.document["extension"] == original.document["extension"]);
        CHECK(va::write_config(path, config));
    }

    std::ofstream(path) << "{not valid JSON";
    CHECK(!va::read_config(path));
    std::ofstream(path) << "{\"repository\":42}";
    CHECK(!va::read_config(path));

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