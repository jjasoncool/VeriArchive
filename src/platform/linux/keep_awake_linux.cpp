#include "platform/keep_awake.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

namespace va::platform {

struct KeepAwake::Held {
    int lock = -1;  // the inhibitor lock: logind releases it when the last copy is closed
};

KeepAwake::KeepAwake(const std::string& reason)
{
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!bus) {
        g_clear_error(&error);
        return;
    }
    // "sleep" in block mode: logind refuses a suspend that a program asks for
    // while the lock is held, the desktop's own after idling included;
    // `systemd-inhibit --list` names the lock's holder and reason. A call that
    // would need authentication fails at once instead of asking (no
    // G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION).
    GUnixFDList* locks = nullptr;
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, "org.freedesktop.login1",
        "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
        g_variant_new("(ssss)", "sleep", "VeriArchive", reason.c_str(), "block"), G_VARIANT_TYPE("(h)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, nullptr, &locks, nullptr, &error);
    g_object_unref(bus);
    if (!reply) {
        g_clear_error(&error);
        return;
    }
    gint32 index = -1;
    g_variant_get(reply, "(h)", &index);
    g_variant_unref(reply);
    // A copy of the lock's file descriptor, which this object closes; the
    // list closes its own.
    const int lock = locks ? g_unix_fd_list_get(locks, index, nullptr) : -1;
    if (locks) {
        g_object_unref(locks);
    }
    if (lock >= 0) {
        held_ = std::make_unique<Held>(lock);
    }
}

KeepAwake::~KeepAwake()
{
    if (held_) {
        close(held_->lock);
    }
}

} // namespace va::platform
