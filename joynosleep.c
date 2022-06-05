#include <systemd/sd-bus.h>
#include <systemd/sd-device.h>
#include <systemd/sd-event.h>

#include <linux/input.h>

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

#define PROJECT_NAME "joynosleep"
#define SAVER       "org.freedesktop.ScreenSaver"
#define SAVER_PATH  "/org/freedesktop/ScreenSaver"

#define DBUS        "org.freedesktop.DBus"
#define DBUS_PATH   "/org/freedesktop/DBus"

// to keep everything simple, use static buffer for tracked joysticks.
// there are not many games (even for arcades) that support more than 4 players,
// so current limit is already too generous.
#define MAX_JOYSTICKS 16

#define cleanup(f) __attribute__((cleanup(f)))
#define unused __attribute__ ((unused))

typedef struct joystick {
    sd_device *dev;
    const char *devname;
    const char *name;
    sd_event_source *source;
    uint64_t n_events;
} joystick;

static sd_bus *g_bus;
static sd_device_monitor *g_monitor;
static uint32_t g_cookie;

static const uint64_t accuracy    =  60000000; //  1min
static uint64_t g_inhibit_timeout = 600000000; // 10min
static sd_event_source *g_timer;

static joystick g_joysticks[MAX_JOYSTICKS];
static size_t n_joysticks;

static int
log_error(int error, const char *message) {
    const int r = -error;
    fprintf(stderr, "%s: %d %s\n", message, r, strerror(r));
    fflush(stderr);
    return error;
}

static int
log_errorf(int error, const char *fmt, ...) {
    const int r = -error;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    fprintf(stderr, ": %d %s\n", r, strerror(r));
    fflush(stderr);
    return error;
}

static void
log_info(const char *line) {
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
}

static void
log_infof(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stdout, fmt, va);
    va_end(va);
    fwrite("\n", 1, 1, stdout);
    fflush(stdout);
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

    log_infof("screen saver inhibited; cookie=%u", *cookie);
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

    log_infof("screen saver restored; cookie=%u", *cookie);
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

    log_infof("screensaver is %s", v ? "active" : "not active");
    return v;
}

static int
joystick_probe(sd_device *d, const char **devname, const char **name) {
    int r;

    const char *v;
    r = sd_device_get_property_value(d, "ID_INPUT_JOYSTICK", &v);
    if (r < 0)
        return r;
    if (!v || strcmp(v, "1"))
        return 0;

    r = sd_device_get_devname(d, &v);
    if (r < 0)
        return r;

    static const char event_pfx[] = "/dev/input/event";
    if (!v || strncmp(v, event_pfx, sizeof(event_pfx)-1))
        return 0;

    sd_device *parent;
    r = sd_device_get_parent(d, &parent);
    if (r < 0)
        return r;

    *devname = v;
    r = sd_device_get_property_value(parent, "NAME", name);
    if (r < 0 || !name || !name[0])
        *name = v;

    return 1;
}

static void
joystick_destroy(void *userdata) {
    joystick *j = userdata;

    sd_device_unref(j->dev);
    assert((signed)n_joysticks > 0);
    --n_joysticks;
    const size_t n = j - g_joysticks;
    if (n < n_joysticks) {
        // we just freed slot in the middle. swap previous last joystick with one.
        *j = g_joysticks[n_joysticks];
        sd_event_source_set_userdata(j->source, j);
    }
}

static void
joystick_del(joystick *j) {
    log_infof("-%zd/%zd: %s %s events=%" PRId64,
        j - g_joysticks, n_joysticks, j->devname, j->name, j->n_events);
    sd_event_source_disable_unref(j->source);
}

static int
is_button_press(const struct input_event *event) {
    return event->type == EV_KEY && event->value == 0;
}

static int
on_joystick_read(unused sd_event_source *s, int fd,
    unused uint32_t revents, unused void *userdata)
{
    int r;
    joystick *j = userdata;

    struct input_event event[1];
    r = read(fd, event, sizeof(event));
    if (r != sizeof(event)) {
        r = -errno;
        if (r == -ENODEV) {
                joystick_del(j);
                return 0;
        } else
                return log_errorf(r, "%s %s read failed", j->name, j->devname);
    }

    ++j->n_events;
    if (!is_button_press(event))
        return 0;

    if (!g_cookie) {
        r = saver_inhibit(g_bus, j->name, &g_cookie);
        if (r < 0)
            return r;
    }

    r = sd_event_source_set_time_relative(g_timer, g_inhibit_timeout);
    if (r < 0)
        return log_error(r, "Failed to reset the timer");

    r = sd_event_source_set_enabled(g_timer, SD_EVENT_ONESHOT);
    if (r < 0)
        return log_error(r, "Failed to enable the timer");

    return 0;
}

static int
joystick_add(sd_event *ev, sd_device *d, const char *devname, const char *name) {
    int r;

    assert(n_joysticks < MAX_JOYSTICKS);

    int fd = open(devname, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if (fd < 0)
        return log_errorf(-errno, "Failed to open %s device %s", name, devname);

    joystick *j = &g_joysticks[n_joysticks];
    r = sd_event_add_io(ev, &j->source, fd, EPOLLIN, on_joystick_read, j);
    if (r < 0) {
        close(fd);
        return log_errorf(r, "Failed to add %s %s to event loop", name, devname);
    }

    r = sd_event_source_set_io_fd_own(j->source, 1);
    assert(r >= 0);

    r = sd_event_source_set_destroy_callback(j->source, joystick_destroy);
    assert(r >= 0);

    j->dev = sd_device_ref(d);
    j->devname = devname;
    j->name = name;
    j->n_events = 0;

    log_infof("+%zd: %s %s", n_joysticks, devname, name);
    ++n_joysticks;
    return 0;
}

static void
joystick_del_all(void) {
    while (n_joysticks) {
        joystick *j = &g_joysticks[n_joysticks - 1];
        joystick_del(j);
    }
}

static int
joystick_exit(unused sd_event_source *s, unused void *userdata) {
    joystick_del_all();
    return 0;
}

static int
on_device_changed(sd_device_monitor *m, sd_device *d, unused void *userdata) {
    int r;

    const char *devname, *name;
    r = joystick_probe(d, &devname, &name);
    if (r <= 0)
        return 0;

    sd_device_action_t a;
    r = sd_device_get_action(d, &a);
    assert(r >= 0);

    sd_event *ev = sd_device_monitor_get_event(m);
    switch (a) {
        case SD_DEVICE_ADD:
            joystick_add(ev, d, devname, name);
            break;
        case SD_DEVICE_REMOVE:
            // no need to track device removal:
            // read() fails with ENODEV first, so it's enough to handle it there
        default:;
    }
    return 0;
}

static int
joystick_monitor_init(sd_event *ev) {
    int r;

    cleanup(sd_device_monitor_unrefp) sd_device_monitor *m = NULL;
    r = sd_device_monitor_new(&m);
    if (r < 0)
        return log_error(r, "Failed to init udev monitor");

    r = sd_device_monitor_filter_add_match_subsystem_devtype(m, "input", NULL);
    if (r < 0)
        return log_error(r, "Failed to add subsystem match to udev monitor");

    r = sd_device_monitor_attach_event(m, ev);
    if (r < 0)
        return log_error(r, "Failed to attach udev monitor");

    g_monitor = sd_device_monitor_ref(m);
    return 0;
}

static void
joystick_monitor_start(void) {
    if (!g_monitor)
        return;

    int r;
    r = sd_device_monitor_start(g_monitor, on_device_changed, NULL);
    if (r < 0) {
        log_error(r, "Failed to start udev monitor");
        g_monitor = sd_device_monitor_unref(g_monitor);
    } else
        log_infof("started joystick hotplug monitor...");
}

static void
joystick_monitor_stop(void) {
    if (!g_monitor)
        return;

    int r;
    r = sd_device_monitor_stop(g_monitor);
    if (r < 0)
        log_error(r, "Failed to stop udev monitor");
}

static int
joystick_enumerate(sd_event *ev) {
    int r;

    cleanup(sd_device_enumerator_unrefp) sd_device_enumerator *e;
    r = sd_device_enumerator_new(&e);
    if (r < 0)
        return log_error(r, "Failed to create device enumerator");

    r = sd_device_enumerator_add_match_subsystem(e, "input", 1);
    if (r < 0)
        return log_error(r, "Failed to add subsystem match");

    int inputs = 0, joysticks = 0;
    sd_device *d;
    for (d = sd_device_enumerator_get_device_first(e);
         d;
         d = sd_device_enumerator_get_device_next(e))
    {
        ++inputs;
        const char *devname, *name;
        r = joystick_probe(d, &devname, &name);
        if (r <= 0)
            continue;

        ++joysticks;
        joystick_add(ev, d, devname, name);
    }

    log_infof("Found %d inputs, %d joysticks, %zd tracked",
        inputs, joysticks, n_joysticks);
    return 0;
}

static int
on_screen_saver_appeared(sd_bus *bus) {
    sd_event *ev = sd_bus_get_event(bus);
    int r = joystick_enumerate(ev);
    joystick_monitor_start();
    return r;
}

static int
on_screen_saver_disappeared(unused sd_bus *bus) {
    int r;

    if (g_cookie) {
        log_infof("stale cookie %u", g_cookie);
        g_cookie = 0;

        r = sd_event_source_set_enabled(g_timer, SD_EVENT_OFF);
        assert(r >= 0);
    }

    // screen saver is gone, no need to read joysticks
    joystick_monitor_stop();
    joystick_del_all();
    return 0;
}

static int
on_name_owner_changed(sd_bus_message *m,
    unused void *userdata,
    unused sd_bus_error *ret_error)
{
    int r;
    const char *name, *old_owner, *new_owner;
    r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
    if (r < 0)
        return log_error(r, "Failed to read NameOwnerChanged reply");

    if (strcmp(name, SAVER))
        return 0;

    sd_bus *bus = sd_bus_message_get_bus(m);
    if (!new_owner || !new_owner[0]) {
        log_info("screen saver disappeared");
        return on_screen_saver_disappeared(bus);
    } else {
        log_info("screen saver appeared");
        return on_screen_saver_appeared(bus);
    }
}

static int
watch_screen_saver(sd_bus *bus) {
    int r;

    r = sd_bus_match_signal(bus, NULL, DBUS, DBUS_PATH, DBUS, "NameOwnerChanged",
        on_name_owner_changed, NULL);
    if (r < 0)
        log_error(r, "Failed to add NameOwnerChanged match");

    return 0;
}

static int
start(sd_bus *bus) {
    int r;

    r = saver_is_active(bus);
    if (r < 0)
        return r;

    // hotplug monitor is nice to have, but not crytical to fail
    joystick_monitor_init(sd_bus_get_event(bus));

    if (r)
        on_screen_saver_appeared(bus);
    else
        log_info("waiting for screen saver to appear...");

    return watch_screen_saver(bus);
}

static int
on_timer(sd_event_source *s, unused uint64_t usec, void *userdata) {
    sd_bus *bus = userdata;
    assert(g_bus == bus);
    assert(g_timer == s);
    assert(g_cookie);

    return saver_uninhibit(bus, &g_cookie);
}

static int
timer_init(sd_event *ev, sd_bus *bus) {
    int r;

    r = sd_event_add_time_relative(ev, &g_timer, CLOCK_MONOTONIC,
        g_inhibit_timeout, accuracy, on_timer, bus);
    if (r < 0)
        return log_error(r, "Failed to initialize timerfd");

    r = sd_event_source_set_enabled(g_timer, SD_EVENT_OFF);
    assert(r >= 0);

    return 0;
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

    r = sd_bus_attach_event(bus, ev, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0)
        return log_error(r, "Failed to attach D-Bus to event loop");

    r = timer_init(ev, bus);
    if (r < 0)
        return r;

    assert(!g_bus);
    g_bus = bus;

    start(bus);

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
        log_infof("%s takes no arguments", argv[0]);
        return 1;
    }

    cleanup(sd_event_unrefp) sd_event *ev = NULL;
    int r;

    r = sd_event_default(&ev);
    if (r < 0)
        return log_error(r, "Failed to allocate event loop");

    signal_init(ev);

    r = sd_event_add_exit(ev, NULL, joystick_exit, NULL);
    assert(r >= 0);

    r = sd_event_add_defer(ev, NULL, bus_init, NULL);
    if (r < 0)
        return log_error(r, "Failed to add event loop job");

    return sd_event_loop(ev);
}
