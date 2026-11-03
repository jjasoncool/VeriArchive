// A drive's health from udisks2 over the system bus (decision 0044): its
// daemon reads SMART as root and shows what it read to every program, so
// nothing here needs elevation (R-08). Without udisks2 (a server, a
// container) the health is Unknown.
#include "platform/linux/device_health_linux.h"

#include "platform/linux/udisks_health.h"

#include <gio/gio.h>

#include <memory>

namespace va::platform::linux_health {
namespace {

constexpr int call_timeout_ms = 3000;

struct ObjectUnref {
    void operator()(gpointer object) const { g_object_unref(object); }
};
struct VariantUnref {
    void operator()(GVariant* variant) const { g_variant_unref(variant); }
};
using Bus = std::unique_ptr<GDBusConnection, ObjectUnref>;
using Variant = std::unique_ptr<GVariant, VariantUnref>;

// A call to udisks2's daemon, which is not started for it
// (G_DBUS_CALL_FLAGS_NO_AUTO_START): where it does not run, nothing is read.
Variant call(GDBusConnection* bus, const char* object, const char* interface, const char* method,
    GVariant* parameters, const char* reply_type)
{
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(bus, "org.freedesktop.UDisks2", object, interface, method,
        parameters, G_VARIANT_TYPE(reply_type), G_DBUS_CALL_FLAGS_NO_AUTO_START, call_timeout_ms, nullptr, &error);
    g_clear_error(&error);
    return Variant(reply);
}

// The properties of `interface` on `object`, as a{sv}; nothing where the
// object has no such interface.
Variant properties(GDBusConnection* bus, const char* object, const char* interface)
{
    const auto reply =
        call(bus, object, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", interface), "(a{sv})");
    if (!reply) {
        return {};
    }
    return Variant(g_variant_get_child_value(reply.get(), 0));
}

} // namespace

DeviceHealth udisks_device_health(const std::string& device_ref)
{
    GError* error = nullptr;
    Bus bus(g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error));
    g_clear_error(&error);
    if (!bus) {
        return {};
    }
    // The block device's object, from its device node.
    GVariantBuilder spec;
    g_variant_builder_init(&spec, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&spec, "{sv}", "path", g_variant_new_string(device_ref.c_str()));
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    const auto resolved = call(bus.get(), "/org/freedesktop/UDisks2/Manager", "org.freedesktop.UDisks2.Manager",
        "ResolveDevice", g_variant_new("(a{sv}a{sv})", &spec, &options), "(ao)");
    if (!resolved) {
        return {};
    }
    const Variant blocks(g_variant_get_child_value(resolved.get(), 0));
    if (g_variant_n_children(blocks.get()) == 0) {
        return {};
    }
    const Variant block(g_variant_get_child_value(blocks.get(), 0));
    const std::string block_path = g_variant_get_string(block.get(), nullptr);

    // Its drive.
    const auto drive_reply = call(bus.get(), block_path.c_str(), "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.UDisks2.Block", "Drive"), "(v)");
    if (!drive_reply) {
        return {};
    }
    const Variant boxed(g_variant_get_child_value(drive_reply.get(), 0));
    const Variant drive_value(g_variant_get_variant(boxed.get()));
    if (!g_variant_is_of_type(drive_value.get(), G_VARIANT_TYPE_OBJECT_PATH)) {
        return {};
    }
    const std::string drive = g_variant_get_string(drive_value.get(), nullptr);
    if (drive == "/") {
        return {};
    }

    // An ATA drive's SMART, else an NVMe controller's SMART log.
    if (const auto ata = properties(bus.get(), drive.c_str(), "org.freedesktop.UDisks2.Drive.Ata")) {
        AtaSmart smart;
        gboolean flag = FALSE;
        if (g_variant_lookup(ata.get(), "SmartSupported", "b", &flag)) {
            smart.supported = flag != FALSE;
        }
        if (g_variant_lookup(ata.get(), "SmartEnabled", "b", &flag)) {
            smart.enabled = flag != FALSE;
        }
        if (g_variant_lookup(ata.get(), "SmartFailing", "b", &flag)) {
            smart.failing = flag != FALSE;
        }
        guint64 updated = 0;
        if (g_variant_lookup(ata.get(), "SmartUpdated", "t", &updated)) {
            smart.updated = updated;
        }
        gint32 attributes = 0;
        if (g_variant_lookup(ata.get(), "SmartNumAttributesFailing", "i", &attributes)) {
            smart.attributes_failing = attributes;
        }
        gint64 sectors = 0;
        if (g_variant_lookup(ata.get(), "SmartNumBadSectors", "x", &sectors)) {
            smart.bad_sectors = sectors;
        }
        return health_from_ata(smart);
    }
    if (const auto nvme = properties(bus.get(), drive.c_str(), "org.freedesktop.UDisks2.NVMe.Controller")) {
        guint64 updated = 0;
        (void)g_variant_lookup(nvme.get(), "SmartUpdated", "t", &updated);
        std::vector<std::string> warnings;
        if (const Variant listed(g_variant_lookup_value(nvme.get(), "SmartCriticalWarning", G_VARIANT_TYPE("as")));
            listed) {
            const auto count = g_variant_n_children(listed.get());
            for (gsize index = 0; index < count; ++index) {
                const Variant warning(g_variant_get_child_value(listed.get(), index));
                warnings.emplace_back(g_variant_get_string(warning.get(), nullptr));
            }
        }
        return health_from_nvme(updated, warnings);
    }
    return {};
}

} // namespace va::platform::linux_health
