#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>

#include <linux/input.h>
#include <linux/types.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulse/mainloop-api.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/device-port.h>
#include <pulsecore/card.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/i18n.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/ioline.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

//#include <droid/hardware/audio_policy.h>
//#include <droid/system/audio_policy.h>

#include <droid/droid-util.h>
#include <droid/sllist.h>
#include <droid/utils.h>
#include "droid-sink.h"
#include "droid-source.h"

#include "card-accessories.h"

#ifndef SW_UNSUPPORT_INSERT
#define SW_UNSUPPORT_INSERT     (0x10)
#endif

struct card_accessories {
    pa_core *core;
    pa_card *card;
    int fd;
    int fd_type;
    pa_io_event *io;

    /* Connected devices */
    int headphone;
    int microphone;
    int lineout;
    int unsupported;
    int physical;
    int video_out;
};

static void update_available(card_accessories *u) {
    pa_device_port *p;
    void *state;

    pa_assert(u);

    /* First set new devices as available. */

    PA_HASHMAP_FOREACH(p, u->card->ports, state) {
        pa_droid_port_data *data = PA_DEVICE_PORT_DATA(p);
        dm_config_port *device_port = data->device_port;

        /* Parking port doesn't have device_port */
        if (!device_port)
            continue;

        if (device_port->type == AUDIO_DEVICE_OUT_WIRED_HEADSET && u->headphone && u->microphone)
            pa_device_port_set_available(p, PA_AVAILABLE_YES);
        else if (device_port->type == AUDIO_DEVICE_IN_WIRED_HEADSET && u->headphone && u->microphone)
            pa_device_port_set_available(p, PA_AVAILABLE_YES);
        else if (device_port->type == AUDIO_DEVICE_OUT_WIRED_HEADPHONE && u->headphone && !u->microphone)
            pa_device_port_set_available(p, PA_AVAILABLE_YES);
        else if (device_port->type == AUDIO_DEVICE_OUT_LINE && u->lineout)
            pa_device_port_set_available(p, PA_AVAILABLE_YES);
    }

    /* Then update unavailable devices. */

    PA_HASHMAP_FOREACH(p, u->card->ports, state) {
        pa_droid_port_data *data = PA_DEVICE_PORT_DATA(p);
        dm_config_port *device_port = data->device_port;

        /* Parking port doesn't have device_port */
        if (!device_port)
            continue;

        if (device_port->type == AUDIO_DEVICE_OUT_WIRED_HEADSET && !u->headphone && !u->microphone)
            pa_device_port_set_available(p, PA_AVAILABLE_NO);
        else if (device_port->type == AUDIO_DEVICE_IN_WIRED_HEADSET && !u->headphone && !u->microphone)
            pa_device_port_set_available(p, PA_AVAILABLE_NO);
        else if (device_port->type == AUDIO_DEVICE_OUT_WIRED_HEADPHONE && !u->headphone)
            pa_device_port_set_available(p, PA_AVAILABLE_NO);
        else if (device_port->type == AUDIO_DEVICE_OUT_LINE && !u->lineout)
            pa_device_port_set_available(p, PA_AVAILABLE_NO);
    }
}

static void io_callback(pa_mainloop_api *io, pa_io_event *e, int fd, pa_io_event_flags_t events, void *userdata) {
    card_accessories *u = userdata;

    if (events & PA_IO_EVENT_HANGUP) {
        pa_log("accessories: jack device closed unexpectedly");
        return;
    }

    if (events & PA_IO_EVENT_ERROR) {
        pa_log("accessories: jack device had an I/O error");
        return;
    }

    if (events & PA_IO_EVENT_INPUT) {
        struct input_event ev;

        if (pa_loop_read(u->fd, &ev, sizeof(ev), &u->fd_type) <= 0) {
            pa_log("Failed to read from event device: %s", pa_cstrerror(errno));
            goto fail;
        }

        if (ev.type != EV_SW && ev.type != EV_SYN) {
            pa_log("ignoring jack event type %d", ev.type);
            return;
        }

        if (ev.type == EV_SYN) {
            pa_log("syn event");
            // do the sync
            update_available(u);
            return;
        }

        const char *name = "invalid";
        int *change = NULL;
        int value = (ev.value ? 1 : 0) ^ 0; //dev->inverted;

        switch (ev.code) {
            case SW_HEADPHONE_INSERT:       change = &u->headphone;     name = "headphone";     break;
            case SW_MICROPHONE_INSERT:      change = &u->microphone;    name = "microphone";    break;
            case SW_LINEOUT_INSERT:         change = &u->lineout;       name = "lineout";       break;
            case SW_VIDEOOUT_INSERT:        change = &u->video_out;     name = "video_out";     break;
            case SW_UNSUPPORT_INSERT:       change = &u->unsupported;   name = "unsupported";   break;
            case SW_JACK_PHYSICAL_INSERT:   change = &u->physical;      name = "physical";      break;
            default:
                pa_log_info("Unknown event %d", ev.code);
                break;
        }

        if (change) {
            *change = value;
            pa_log("%s value -> %d", name, value);
        }
    }
    return;

fail:
    return;
}

static void setup_wired(card_accessories *u) {
    const char *dev = "/dev/input/event6";
    u->fd = open(dev, O_RDONLY);
    if (u->fd < 0) {
        pa_log("Could not open %s", dev);
        return;
    }
    pa_log("set callback for %s", dev);

    u->io = u->card->core->mainloop->io_new(u->card->core->mainloop,
                                            u->fd,
                                            PA_IO_EVENT_INPUT|PA_IO_EVENT_HANGUP|PA_IO_EVENT_ERROR,
                                            io_callback,
                                            u);
}

card_accessories *card_accessories_init(pa_card *card) {
    pa_assert(card);

    card_accessories *u = pa_xnew0(card_accessories, 1);
    u->card = card;
    setup_wired(u);

    return u;
}

void card_accessories_done(card_accessories *accessories)
{
    if (!accessories)
        return;

    pa_xfree(accessories);
}
