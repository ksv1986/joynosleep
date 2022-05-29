#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#define PROJECT_NAME "joynosleep"
#define SAVER       "org.freedesktop.ScreenSaver"
#define SAVER_PATH  "/org/freedesktop/ScreenSaver"

#define DBUS        "org.freedesktop.DBus"
#define DBUS_PATH   "/org/freedesktop/DBus"

#define cleanup(f) __attribute__((cleanup(f)))
#define unused __attribute__ ((unused))

static int
log_error(int error, const char *message) {
    const int r = -error;
    fprintf(stderr, "%s: %d %s\n", message, r, strerror(r));
    return error;
}

static int
dbus_call(sd_bus *bus, sd_bus_message **reply,
    const char *dest, const char *path, const char *interface,
    const char *member, const char *types, ...)
{
    cleanup(sd_bus_message_unrefp) sd_bus_message *m = NULL;
    cleanup(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    r = sd_bus_message_new_method_call(bus, &m, dest, path, interface, member);
    if (r < 0)
        return log_error(r, "Failed to create bus message");

    va_list va;
    va_start(va, types);
    r = sd_bus_message_appendv(m, types, va);
    va_end(va);

    if (r < 0)
        return log_error(r, "Failed to append to bus message");

    r = sd_bus_call(bus, m, -1, &error, reply);
    if (r < 0)
        return log_error(r, "Call failed");

    return 0;
}

static int
saver_inhibit(sd_bus *bus, const char *reason, uint32_t *cookie) {
    cleanup(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
    int r;

    r = dbus_call(bus, &reply, SAVER, SAVER_PATH, SAVER, "Inhibit",
        "ss", PROJECT_NAME, reason);
    if (r < 0)
        return r;

    r = sd_bus_message_read_basic(reply, 'u', cookie);
    if (r < 0)
        return log_error(r, "Failed to read Inhibit reply");

    printf("screen saver inhibited; cookie=%u\n", *cookie);
    return 0;
}

static int
saver_uninhibit(sd_bus *bus, uint32_t *cookie) {
    cleanup(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
    int r;

    if (!*cookie)
        return 0;

    r = dbus_call(bus, &reply, SAVER, SAVER_PATH, SAVER, "UnInhibit",
        "u", *cookie);
    if (r < 0)
        return r;

    printf("screen saver restored; cookie=%u\n", *cookie);
    *cookie = 0;

    return 0;
}

static int
saver_is_active(sd_bus *bus) {
    cleanup(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
    int r;

    r = dbus_call(bus, &reply, DBUS, DBUS_PATH, DBUS, "NameHasOwner",
        "s", SAVER);
    if (r < 0)
        return r;

    int v;
    r = sd_bus_message_read_basic(reply, 'b', &v);
    if (r < 0)
        return log_error(r, "Failed to read NameHasOwner reply");

    printf("screensaver is %s\n", v ? "active" : "not active");
    return v;
}

static int
start(sd_bus *bus) {
    int r;
    uint32_t cookie = 0;

    r = saver_is_active(bus);
    if (r < 0)
        return r;

    if (!r)
        return 1;

    r = saver_inhibit(bus, "test", &cookie);
    if (r < 0)
        return r;

    return saver_uninhibit(bus, &cookie);
}

static int
bus_fini(unused sd_event_source *s, void *userdata) {
    sd_bus *bus = userdata;
    sd_bus_unref(bus);
    return 0;
}

static int
bus_init(sd_event_source *s, unused void *userdata) {
    int r;

    sd_bus *bus = NULL;
    r = sd_bus_default_user(&bus);
    if (r < 0)
        return log_error(r, "Can't connect do D-Bus");

    sd_event *ev = sd_event_source_get_event(s);
    r = sd_event_add_exit(ev, NULL, bus_fini, bus);
    assert(r >= 0);

    start(bus);

    r = sd_event_exit(ev, r);
    assert(r >= 0);
    return 0;
}

static void
signal_init(sd_event *ev) {
    int r;
    sigset_t s;

    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    sigprocmask(SIG_BLOCK, &s, NULL);

    // Stop event loop on a signal, exit handlers will take care of allocated resources
    r = sd_event_add_signal(ev, NULL, SIGINT, NULL, NULL);
    assert(r >= 0);
    r = sd_event_add_signal(ev, NULL, SIGTERM, NULL, NULL);
    assert(r >= 0);
}

int
main(int argc, char **argv) {
    if (argc != 1) {
        printf("%s takes no arguments.\n", argv[0]);
        return 1;
    }

    cleanup(sd_event_unrefp) sd_event *ev = NULL;
    int r;

    r = sd_event_default(&ev);
    if (r < 0)
        return log_error(r, "Failed to allocate event loop");

    signal_init(ev);

    r = sd_event_add_defer(ev, NULL, bus_init, NULL);
    if (r < 0)
        return log_error(r, "Failed to add event loop job");

    return sd_event_loop(ev);
}
