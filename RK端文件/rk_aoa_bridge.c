#include <arpa/inet.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define VID_GOOGLE 0x18d1

#define AOA_GET_PROTOCOL    51
#define AOA_SEND_STRING     52
#define AOA_START_ACCESSORY 53

#define AOA_STRING_MANUFACTURER 0
#define AOA_STRING_MODEL        1
#define AOA_STRING_DESCRIPTION  2
#define AOA_STRING_VERSION      3
#define AOA_STRING_URI          4
#define AOA_STRING_SERIAL       5

#define AOA_REQ_REGISTER_HID        54
#define AOA_REQ_UNREGISTER_HID      55
#define AOA_REQ_SET_HID_REPORT_DESC 56
#define AOA_REQ_SEND_HID_EVENT      57

#define HID_ID_TOUCH 1
#define HID_ID_KEYBOARD 2

#define USB_BUFFER_SIZE (256 * 1024)
#define MAX_H264_PACKET_SIZE (8 * 1024 * 1024)
#define DEFAULT_VIDEO_PORT 9001
#define DEFAULT_CONTROL_PORT 9002
#define APP_CONTROL_TIMEOUT_MS 50

struct usb_id {
    uint16_t vid;
    uint16_t pid;
    const char *name;
};

static struct usb_id starter_devices[] = {
    {0x12d1, 0x107e, "Huawei P10 smartphone"},
    {0x2717, 0xff08, "Xiaomi ADB"},
    {0x2717, 0xff40, "Xiaomi MTP"},
};

static struct usb_id aoa_devices[] = {
    {VID_GOOGLE, 0x2d00, "AOA only"},
    {VID_GOOGLE, 0x2d01, "AOA + ADB"},
    {VID_GOOGLE, 0x2d02, "AOA audio"},
    {VID_GOOGLE, 0x2d03, "AOA audio + ADB"},
    {VID_GOOGLE, 0x2d04, "AOA accessory + audio"},
    {VID_GOOGLE, 0x2d05, "AOA accessory + audio + ADB"},
};

static volatile int running = 1;
static volatile int stream_forward_enabled = 1;

static pthread_mutex_t aoa_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t aoa_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t aoa_bulk_out_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t hid_mutex = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *aoa_handle = NULL;
static int aoa_out_ep = 0;
static int hid_ready = 0;
static int keyboard_ready = 0;
static uint64_t last_app_control_error_ms = 0;

static const char *windows_ip = NULL;
static int video_port = DEFAULT_VIDEO_PORT;
static int control_port = DEFAULT_CONTROL_PORT;
static int selected_device_index = -1;
static int selected_usb_bus = -1;
static int selected_usb_address = -1;

struct usb_stream_reader {
    libusb_device_handle *handle;
    int ep;
    unsigned char buf[USB_BUFFER_SIZE];
    int pos;
    int len;
};

static const unsigned char touch_hid_desc[] = {
    0x05, 0x0D,        /* Usage Page: Digitizers */
    0x09, 0x04,        /* Usage: Touch Screen */
    0xA1, 0x01,        /* Collection: Application */
    0x09, 0x22,        /*   Usage: Finger */
    0xA1, 0x02,        /*   Collection: Logical */
    0x09, 0x42,        /*     Usage: Tip Switch */
    0x15, 0x00,        /*     Logical Minimum: 0 */
    0x25, 0x01,        /*     Logical Maximum: 1 */
    0x75, 0x01,        /*     Report Size: 1 */
    0x95, 0x01,        /*     Report Count: 1 */
    0x81, 0x02,        /*     Input: Data,Var,Abs */
    0x09, 0x32,        /*     Usage: In Range */
    0x81, 0x02,        /*     Input: Data,Var,Abs */
    0x75, 0x06,        /*     Report Size: 6 */
    0x95, 0x01,        /*     Report Count: 1 */
    0x81, 0x03,        /*     Input: Const,Var,Abs */
    0x09, 0x51,        /*     Usage: Contact Identifier */
    0x75, 0x08,        /*     Report Size: 8 */
    0x95, 0x01,        /*     Report Count: 1 */
    0x15, 0x00,        /*     Logical Minimum: 0 */
    0x25, 0x0F,        /*     Logical Maximum: 15 */
    0x81, 0x02,        /*     Input: Data,Var,Abs */
    0x05, 0x01,        /*     Usage Page: Generic Desktop */
    0x09, 0x30,        /*     Usage: X */
    0x09, 0x31,        /*     Usage: Y */
    0x16, 0x00, 0x00,  /*     Logical Minimum: 0 */
    0x26, 0xFF, 0x7F,  /*     Logical Maximum: 32767 */
    0x75, 0x10,        /*     Report Size: 16 */
    0x95, 0x02,        /*     Report Count: 2 */
    0x81, 0x02,        /*     Input: Data,Var,Abs */
    0xC0,              /*   End Collection */
    0x05, 0x0D,        /*   Usage Page: Digitizers */
    0x09, 0x54,        /*   Usage: Contact Count */
    0x25, 0x7F,        /*   Logical Maximum: 127 */
    0x75, 0x08,        /*   Report Size: 8 */
    0x95, 0x01,        /*   Report Count: 1 */
    0x81, 0x02,        /*   Input: Data,Var,Abs */
    0xC0               /* End Collection */
};

static const unsigned char keyboard_hid_desc[] = {
    0x05, 0x01,        /* Usage Page: Generic Desktop */
    0x09, 0x06,        /* Usage: Keyboard */
    0xA1, 0x01,        /* Collection: Application */
    0x05, 0x07,        /*   Usage Page: Keyboard/Keypad */
    0x19, 0xE0,        /*   Usage Minimum: Left Control */
    0x29, 0xE7,        /*   Usage Maximum: Right GUI */
    0x15, 0x00,        /*   Logical Minimum: 0 */
    0x25, 0x01,        /*   Logical Maximum: 1 */
    0x75, 0x01,        /*   Report Size: 1 */
    0x95, 0x08,        /*   Report Count: 8 */
    0x81, 0x02,        /*   Input: Data,Var,Abs */
    0x95, 0x01,        /*   Report Count: 1 */
    0x75, 0x08,        /*   Report Size: 8 */
    0x81, 0x03,        /*   Input: Const,Var,Abs */
    0x95, 0x06,        /*   Report Count: 6 */
    0x75, 0x08,        /*   Report Size: 8 */
    0x15, 0x00,        /*   Logical Minimum: 0 */
    0x25, 0x65,        /*   Logical Maximum: 101 */
    0x05, 0x07,        /*   Usage Page: Keyboard/Keypad */
    0x19, 0x00,        /*   Usage Minimum: 0 */
    0x29, 0x65,        /*   Usage Maximum: 101 */
    0x81, 0x00,        /*   Input: Data,Array,Abs */
    0xC0               /* End Collection */
};

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void write_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static int send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static int connect_windows_video(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket video");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)video_port);

    if (inet_pton(AF_INET, windows_ip, &addr.sin_addr) != 1) {
        printf("invalid Windows IP: %s\n", windows_ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    printf("connected to Windows video receiver %s:%d\n", windows_ip, video_port);
    return fd;
}

static const char *find_usb_name(struct usb_id *devices, size_t count, uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < count; i++) {
        if (devices[i].vid == vid && devices[i].pid == pid) {
            return devices[i].name;
        }
    }

    return NULL;
}

static int match_selected_usb(libusb_device *dev, int match_index) {
    if (selected_usb_bus >= 0 || selected_usb_address >= 0) {
        int bus = libusb_get_bus_number(dev);
        int address = libusb_get_device_address(dev);
        return bus == selected_usb_bus && address == selected_usb_address;
    }

    if (selected_device_index >= 0) {
        return match_index == selected_device_index;
    }

    return 1;
}

static void print_matching_devices(
    libusb_context *ctx,
    struct usb_id *devices,
    size_t count,
    const char *label
) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int match_index = 0;

    if (n < 0) {
        printf("list %s devices failed: %s (%ld)\n", label, libusb_error_name((int)n), (long)n);
        return;
    }

    printf("%s devices:\n", label);

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;

        const char *name = find_usb_name(devices, count, desc.idVendor, desc.idProduct);
        if (!name) continue;

        printf(
            "  index=%d usb=%03u:%03u id=%04x:%04x %s\n",
            match_index,
            libusb_get_bus_number(list[i]),
            libusb_get_device_address(list[i]),
            desc.idVendor,
            desc.idProduct,
            name
        );
        match_index++;
    }

    if (match_index == 0) {
        printf("  none\n");
    }

    libusb_free_device_list(list, 1);
}

static libusb_device_handle *open_from_list_filtered(
    libusb_context *ctx,
    struct usb_id *devices,
    size_t count,
    int use_selection,
    int verbose
) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int match_index = 0;

    if (n < 0) {
        printf("get device list failed: %s (%ld)\n", libusb_error_name((int)n), (long)n);
        return NULL;
    }

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;

        const char *name = find_usb_name(devices, count, desc.idVendor, desc.idProduct);
        if (!name) continue;

        int bus = libusb_get_bus_number(list[i]);
        int address = libusb_get_device_address(list[i]);

        if (verbose) {
            printf(
                "trying device index=%d usb=%03d:%03d id=%04x:%04x (%s)\n",
                match_index,
                bus,
                address,
                desc.idVendor,
                desc.idProduct,
                name
            );
        }

        if (use_selection && !match_selected_usb(list[i], match_index)) {
            match_index++;
            continue;
        }

        libusb_device_handle *handle = NULL;
        int r = libusb_open(list[i], &handle);

        if (r == 0 && handle) {
            printf(
                "found device index=%d usb=%03d:%03d id=%04x:%04x (%s)\n",
                match_index,
                bus,
                address,
                desc.idVendor,
                desc.idProduct,
                name
            );
            libusb_free_device_list(list, 1);
            return handle;
        }

        if (verbose) {
            printf("open failed: %s (%d)\n", libusb_error_name(r), r);
        }

        match_index++;
    }

    libusb_free_device_list(list, 1);
    return NULL;
}

static libusb_device_handle *open_from_list(
    libusb_context *ctx,
    struct usb_id *devices,
    size_t count,
    int verbose
) {
    return open_from_list_filtered(ctx, devices, count, 0, verbose);
}

static int send_aoa_string(libusb_device_handle *handle, int index, const char *str) {
    int len = (int)strlen(str) + 1;

    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_SEND_STRING,
        0,
        (uint16_t)index,
        (unsigned char *)str,
        (uint16_t)len,
        1000
    );

    if (r < 0) {
        printf("send_string index %d failed: %s (%d)\n",
               index, libusb_error_name(r), r);
        return r;
    }

    printf("send_string index %d ok: %s\n", index, str);
    return 0;
}

static int wait_for_aoa(libusb_context *ctx, int wait_ms) {
    int elapsed = 0;

    while (elapsed <= wait_ms && running) {
        libusb_device_handle *handle = open_from_list(
            ctx,
            aoa_devices,
            sizeof(aoa_devices) / sizeof(aoa_devices[0]),
            0
        );

        if (handle) {
            libusb_close(handle);
            return 1;
        }

        usleep(250000);
        elapsed += 250;
    }

    return 0;
}

static int start_aoa_on_handle(libusb_device_handle *handle) {
    unsigned char protocol[2] = {0};
    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_GET_PROTOCOL,
        0,
        0,
        protocol,
        sizeof(protocol),
        1000
    );

    if (r != 2) {
        printf("AOA_GET_PROTOCOL failed: %s (%d)\n", libusb_error_name(r), r);
        return -1;
    }

    int version = protocol[0] | (protocol[1] << 8);
    printf("AOA protocol version: %d\n", version);

    if (send_aoa_string(handle, AOA_STRING_MANUFACTURER, "RK3568") < 0 ||
        send_aoa_string(handle, AOA_STRING_MODEL, "RK-AOA-DEMO") < 0 ||
        send_aoa_string(handle, AOA_STRING_DESCRIPTION, "RK3568 AOA Demo") < 0 ||
        send_aoa_string(handle, AOA_STRING_VERSION, "1.0") < 0 ||
        send_aoa_string(handle, AOA_STRING_URI, "https://example.com") < 0 ||
        send_aoa_string(handle, AOA_STRING_SERIAL, "rk3568-001") < 0) {
        return -1;
    }

    printf("starting accessory mode...\n");

    r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        AOA_START_ACCESSORY,
        0,
        0,
        NULL,
        0,
        1000
    );

    if (r < 0) {
        printf("AOA_START_ACCESSORY failed: %s (%d)\n", libusb_error_name(r), r);
        return -1;
    }

    printf("AOA_START_ACCESSORY sent\n");
    return 0;
}

static int start_all_starter_devices(libusb_context *ctx) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    int started = 0;

    if (n < 0) {
        printf("get device list failed: %s (%ld)\n", libusb_error_name((int)n), (long)n);
        return -1;
    }

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;

        const char *name = find_usb_name(
            starter_devices,
            sizeof(starter_devices) / sizeof(starter_devices[0]),
            desc.idVendor,
            desc.idProduct
        );
        if (!name) continue;

        printf(
            "start candidate usb=%03u:%03u id=%04x:%04x (%s)\n",
            libusb_get_bus_number(list[i]),
            libusb_get_device_address(list[i]),
            desc.idVendor,
            desc.idProduct,
            name
        );

        libusb_device_handle *handle = NULL;
        int r = libusb_open(list[i], &handle);
        if (r != 0 || !handle) {
            printf("open starter failed: %s (%d)\n", libusb_error_name(r), r);
            continue;
        }

        if (start_aoa_on_handle(handle) == 0) {
            started++;
        }

        libusb_close(handle);
        usleep(250000);
    }

    libusb_free_device_list(list, 1);
    printf("AOA start requests sent: %d\n", started);
    return started > 0 ? 0 : -1;
}

static int start_aoa_if_needed(libusb_context *ctx) {
    libusb_device_handle *handle = open_from_list(
        ctx,
        aoa_devices,
        sizeof(aoa_devices) / sizeof(aoa_devices[0]),
        0
    );

    if (handle) {
        printf("device already in AOA mode\n");
        libusb_close(handle);
        return 0;
    }

    handle = open_from_list(
        ctx,
        starter_devices,
        sizeof(starter_devices) / sizeof(starter_devices[0]),
        1
    );

    if (!handle) {
        printf("starter device not found\n");
        return -1;
    }

    int r = start_aoa_on_handle(handle);

    libusb_close(handle);

    if (r != 0) {
        return -1;
    }

    printf("waiting for AOA re-enumeration...\n");

    if (!wait_for_aoa(ctx, 6000)) {
        printf("AOA device did not appear before timeout\n");
        return -1;
    }

    printf("AOA device is ready\n");
    return 0;
}

static int find_bulk_endpoints(
    libusb_device_handle *handle,
    int *interface_number,
    int *in_ep,
    int *out_ep
) {
    libusb_device *dev = libusb_get_device(handle);
    struct libusb_config_descriptor *config = NULL;

    int r = libusb_get_active_config_descriptor(dev, &config);
    if (r != 0) {
        printf("get config descriptor failed: %s\n", libusb_error_name(r));
        return r;
    }

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *interface = &config->interface[i];

        for (int j = 0; j < interface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &interface->altsetting[j];
            int local_in_ep = 0;
            int local_out_ep = 0;

            for (int k = 0; k < alt->bNumEndpoints; k++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                int ep_addr = ep->bEndpointAddress;
                int ep_type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                int ep_dir = ep_addr & LIBUSB_ENDPOINT_DIR_MASK;

                if (ep_type != LIBUSB_TRANSFER_TYPE_BULK) continue;

                if (ep_dir == LIBUSB_ENDPOINT_IN) {
                    local_in_ep = ep_addr;
                } else {
                    local_out_ep = ep_addr;
                }

                if (local_in_ep && local_out_ep) {
                    *interface_number = alt->bInterfaceNumber;
                    *in_ep = local_in_ep;
                    *out_ep = local_out_ep;
                    printf(
                        "found bulk IN endpoint: 0x%02x, OUT endpoint: 0x%02x, interface: %d\n",
                        *in_ep, *out_ep, *interface_number
                    );
                    libusb_free_config_descriptor(config);
                    return 0;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    return -1;
}

static int aoa_ctrl_out(
    libusb_device_handle *handle,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length
) {
    pthread_mutex_lock(&aoa_send_mutex);

    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        request,
        value,
        index,
        data,
        length,
        1000
    );

    pthread_mutex_unlock(&aoa_send_mutex);

    if (r < 0) {
        printf("AOA ctrl request %u failed: %s (%d)\n",
               request, libusb_error_name(r), r);
        return r;
    }

    return 0;
}

static int aoa_hid_register_touch(libusb_device_handle *handle) {
    int r = aoa_ctrl_out(handle, AOA_REQ_REGISTER_HID, HID_ID_TOUCH,
                         sizeof(touch_hid_desc), NULL, 0);
    if (r != 0) return r;

    return aoa_ctrl_out(handle, AOA_REQ_SET_HID_REPORT_DESC, HID_ID_TOUCH, 0,
                        (unsigned char *)touch_hid_desc,
                        sizeof(touch_hid_desc));
}

static int aoa_hid_unregister_touch(libusb_device_handle *handle) {
    return aoa_ctrl_out(handle, AOA_REQ_UNREGISTER_HID, HID_ID_TOUCH, 0, NULL, 0);
}

static int aoa_hid_register_keyboard(libusb_device_handle *handle) {
    int r = aoa_ctrl_out(handle, AOA_REQ_REGISTER_HID, HID_ID_KEYBOARD,
                         sizeof(keyboard_hid_desc), NULL, 0);
    if (r != 0) return r;

    return aoa_ctrl_out(handle, AOA_REQ_SET_HID_REPORT_DESC, HID_ID_KEYBOARD, 0,
                        (unsigned char *)keyboard_hid_desc,
                        sizeof(keyboard_hid_desc));
}

static int aoa_hid_unregister_keyboard(libusb_device_handle *handle) {
    return aoa_ctrl_out(handle, AOA_REQ_UNREGISTER_HID, HID_ID_KEYBOARD, 0, NULL, 0);
}

static int scale_coord(int v, int max) {
    if (max <= 1) return 0;
    if (v < 0) v = 0;
    if (v >= max) v = max - 1;
    return v * 32767 / (max - 1);
}

static int aoa_hid_send_touch_report(
    libusb_device_handle *handle,
    int touching,
    int x,
    int y,
    int width,
    int height
) {
    int sx = scale_coord(x, width);
    int sy = scale_coord(y, height);
    unsigned char report[7];

    report[0] = touching ? 0x03 : 0x00; /* Tip Switch + In Range */
    report[1] = 1;                      /* Contact Identifier */
    report[2] = (unsigned char)(sx & 0xff);
    report[3] = (unsigned char)((sx >> 8) & 0xff);
    report[4] = (unsigned char)(sy & 0xff);
    report[5] = (unsigned char)((sy >> 8) & 0xff);
    report[6] = touching ? 1 : 0;       /* Contact Count */

    return aoa_ctrl_out(handle, AOA_REQ_SEND_HID_EVENT, HID_ID_TOUCH, 0,
                        report, sizeof(report));
}

static int with_hid_handle(int touching, int x, int y, int width, int height) {
    pthread_mutex_lock(&hid_mutex);

    if (!hid_ready || !aoa_handle) {
        pthread_mutex_unlock(&hid_mutex);
        return -1;
    }

    libusb_device_handle *handle = aoa_handle;
    int r = aoa_hid_send_touch_report(handle, touching, x, y, width, height);

    pthread_mutex_unlock(&hid_mutex);
    return r;
}

static int aoa_hid_send_keyboard_report(
    libusb_device_handle *handle,
    unsigned char modifier,
    unsigned char usage
) {
    unsigned char report[8];
    memset(report, 0, sizeof(report));
    report[0] = modifier;
    report[2] = usage;

    return aoa_ctrl_out(handle, AOA_REQ_SEND_HID_EVENT, HID_ID_KEYBOARD, 0,
                        report, sizeof(report));
}

static int with_keyboard_handle(unsigned char modifier, unsigned char usage, int down) {
    pthread_mutex_lock(&hid_mutex);

    if (!keyboard_ready || !aoa_handle) {
        pthread_mutex_unlock(&hid_mutex);
        return -1;
    }

    libusb_device_handle *handle = aoa_handle;
    int r = aoa_hid_send_keyboard_report(handle, down ? modifier : 0, down ? usage : 0);

    pthread_mutex_unlock(&hid_mutex);
    return r;
}

static int send_app_control_json(const char *json) {
    pthread_mutex_lock(&aoa_bulk_out_mutex);

    libusb_device_handle *handle = aoa_handle;
    int ep = aoa_out_ep;

    if (!handle || ep == 0 || !json) {
        pthread_mutex_unlock(&aoa_bulk_out_mutex);
        return -1;
    }

    size_t len = strlen(json);
    if (len == 0) {
        pthread_mutex_unlock(&aoa_bulk_out_mutex);
        return -1;
    }

    unsigned char *line = (unsigned char *)malloc(len + 1);
    if (!line) {
        pthread_mutex_unlock(&aoa_bulk_out_mutex);
        return -1;
    }

    memcpy(line, json, len);
    line[len] = '\n';

    int transferred = 0;
    int r = libusb_bulk_transfer(handle, ep, line, (int)(len + 1), &transferred, APP_CONTROL_TIMEOUT_MS);
    free(line);

    pthread_mutex_unlock(&aoa_bulk_out_mutex);

    if (r != 0) {
        uint64_t now = now_ms();
        if (now - last_app_control_error_ms >= 1000) {
            last_app_control_error_ms = now;
            printf("send app control failed: %s (%d)\n", libusb_error_name(r), r);
        }
        return r;
    }

    return transferred == (int)(len + 1) ? 0 : -1;
}

static int extract_json_int(const char *body, const char *key, int def) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (!p) return def;
    p = strchr(p, ':');
    if (!p) return def;
    return atoi(p + 1);
}

static int extract_json_string(const char *body, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(body, pattern);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;

    size_t pos = 0;
    while (*p && *p != '"' && pos + 1 < out_size) {
        if (*p == '\\' && p[1]) p++;
        out[pos++] = *p++;
    }

    out[pos] = '\0';
    return pos > 0;
}

static int json_has_type(const char *body, const char *type) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"type\":\"%s\"", type);
    if (strstr(body, pattern)) return 1;
    snprintf(pattern, sizeof(pattern), "\"type\": \"%s\"", type);
    return strstr(body, pattern) != NULL;
}

static int execute_touch_action(const char *body) {
    int x = extract_json_int(body, "x", 0);
    int y = extract_json_int(body, "y", 0);
    int width = extract_json_int(body, "width", 1080);
    int height = extract_json_int(body, "height", 2340);

    if (json_has_type(body, "stream")) {
        char mode[16];
        if (!extract_json_string(body, "mode", mode, sizeof(mode))) {
            return -1;
        }
        stream_forward_enabled = strcmp(mode, "active") == 0 ? 1 : 0;
        printf("stream forward mode: %s\n", stream_forward_enabled ? "active" : "idle");
        return 0;
    }

    if (json_has_type(body, "key")) {
        char action[16];
        int usage = extract_json_int(body, "usage", 0);
        int modifier = extract_json_int(body, "modifier", 0);
        int down = 1;

        if (extract_json_string(body, "action", action, sizeof(action))) {
            down = strcmp(action, "up") != 0;
        }

        if (usage <= 0 || usage > 255 || modifier < 0 || modifier > 255) {
            return -1;
        }

        return with_keyboard_handle((unsigned char)modifier, (unsigned char)usage, down);
    }

    if (json_has_type(body, "cursor")) {
        return 0;
    }

    if (json_has_type(body, "down")) {
        return with_hid_handle(1, x, y, width, height);
    }

    if (json_has_type(body, "move")) {
        return with_hid_handle(1, x, y, width, height);
    }

    if (json_has_type(body, "up")) {
        return with_hid_handle(0, x, y, width, height);
    }

    if (json_has_type(body, "tap")) {
        int r = with_hid_handle(1, x, y, width, height);
        usleep(80000);
        if (r == 0) r = with_hid_handle(0, x, y, width, height);
        return r;
    }

    if (json_has_type(body, "doubletap")) {
        with_hid_handle(1, x, y, width, height);
        usleep(80000);
        with_hid_handle(0, x, y, width, height);
        usleep(120000);
        with_hid_handle(1, x, y, width, height);
        usleep(80000);
        return with_hid_handle(0, x, y, width, height);
    }

    if (json_has_type(body, "swipe")) {
        int x1 = extract_json_int(body, "x1", 0);
        int y1 = extract_json_int(body, "y1", 0);
        int x2 = extract_json_int(body, "x2", 0);
        int y2 = extract_json_int(body, "y2", 0);
        int duration = extract_json_int(body, "duration", 300);
        int steps = duration / 10;
        if (steps < 1) steps = 1;

        int r = with_hid_handle(1, x1, y1, width, height);
        for (int i = 1; i <= steps && r == 0; i++) {
            int nx = x1 + (x2 - x1) * i / steps;
            int ny = y1 + (y2 - y1) * i / steps;
            r = with_hid_handle(1, nx, ny, width, height);
            usleep(10000);
        }
        if (r == 0) r = with_hid_handle(0, x2, y2, width, height);
        return r;
    }

    return -1;
}

static void *control_server_thread(void *arg) {
    (void)arg;

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("control socket");
        return NULL;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)control_port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("control bind");
        close(server);
        return NULL;
    }

    if (listen(server, 8) != 0) {
        perror("control listen");
        close(server);
        return NULL;
    }

    printf("control server listening on 0.0.0.0:%d\n", control_port);

    while (running) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("control accept");
            break;
        }

        char buf[4096];
        size_t pos = 0;

        while (pos + 1 < sizeof(buf)) {
            char c;
            ssize_t n = recv(client, &c, 1, 0);
            if (n <= 0) break;
            if (c == '\n') break;
            buf[pos++] = c;
        }

        buf[pos] = '\0';

        if (pos > 0) {
            int r = execute_touch_action(buf);
            printf("control: %s result=%d\n", buf, r);
        }

        close(client);
    }

    close(server);
    return NULL;
}

static int stream_fill(struct usb_stream_reader *reader) {
    reader->pos = 0;
    reader->len = 0;

    while (running) {
        int transferred = 0;

        int r = libusb_bulk_transfer(
            reader->handle,
            reader->ep,
            reader->buf,
            sizeof(reader->buf),
            &transferred,
            3000
        );

        if (r == LIBUSB_ERROR_TIMEOUT) continue;

        if (r != 0) {
            printf("bulk read failed: %s (%d)\n", libusb_error_name(r), r);
            return r;
        }

        if (transferred <= 0) {
            usleep(10000);
            continue;
        }

        reader->len = transferred;
        return 0;
    }

    return -1;
}

static int read_exact(struct usb_stream_reader *reader, unsigned char *buf, int len) {
    int offset = 0;

    while (running && offset < len) {
        if (reader->pos >= reader->len) {
            int r = stream_fill(reader);
            if (r != 0) return r;
        }

        int available = reader->len - reader->pos;
        int need = len - offset;
        int copy_len = available < need ? available : need;

        if (copy_len > 0) {
            memcpy(buf + offset, reader->buf + reader->pos, copy_len);
            reader->pos += copy_len;
            offset += copy_len;
        }
    }

    return offset == len ? 0 : -1;
}

static int stream_read_byte(struct usb_stream_reader *reader, unsigned char *out) {
    while (running) {
        if (reader->pos >= reader->len) {
            int r = stream_fill(reader);
            if (r != 0) return r;
        }

        if (reader->pos < reader->len) {
            *out = reader->buf[reader->pos++];
            return 0;
        }
    }

    return -1;
}

static int read_avc1_packet(struct usb_stream_reader *reader, unsigned char **out, size_t *out_len) {
    unsigned char window[4] = {0};
    int filled = 0;

    while (running) {
        unsigned char b = 0;
        int r = stream_read_byte(reader, &b);
        if (r != 0) return r;

        if (filled < 4) {
            window[filled++] = b;
        } else {
            window[0] = window[1];
            window[1] = window[2];
            window[2] = window[3];
            window[3] = b;
        }

        if (filled == 4 && memcmp(window, "AVC1", 4) == 0) {
            unsigned char header[28];
            memcpy(header, "AVC1", 4);
            r = read_exact(reader, header + 4, 24);
            if (r != 0) return r;

            uint32_t data_len = read_be32(header + 24);
            if (data_len == 0 || data_len > MAX_H264_PACKET_SIZE) {
                printf("invalid h264 packet size: %u\n", data_len);
                filled = 0;
                continue;
            }

            size_t packet_len = 28 + (size_t)data_len;
            unsigned char *packet = (unsigned char *)malloc(packet_len);
            if (!packet) return -1;

            memcpy(packet, header, 28);
            r = read_exact(reader, packet + 28, (int)data_len);
            if (r != 0) {
                free(packet);
                return r;
            }

            *out = packet;
            *out_len = packet_len;
            return 0;
        }
    }

    return -1;
}

static void update_cached_key_packet(
    const unsigned char *packet,
    size_t packet_len,
    unsigned char **config_cache,
    size_t *config_len,
    unsigned char **key_cache,
    size_t *key_len
) {
    if (!packet || packet_len < 28 || memcmp(packet, "AVC1", 4) != 0) {
        return;
    }

    uint32_t flags = read_be32(packet + 12);
    uint32_t data_len = read_be32(packet + 24);
    if (data_len == 0 || 28 + (size_t)data_len > packet_len) {
        return;
    }

    const unsigned char *data = packet + 28;

    if (flags & 2) {
        unsigned char *next_config = (unsigned char *)malloc(data_len);
        if (next_config) {
            memcpy(next_config, data, data_len);
            free(*config_cache);
            *config_cache = next_config;
            *config_len = data_len;
        }
    }

    if (flags & 1) {
        size_t combined_data_len = (size_t)data_len + *config_len;
        if (combined_data_len > MAX_H264_PACKET_SIZE) {
            return;
        }

        size_t next_key_len = 28 + combined_data_len;
        unsigned char *next_key = (unsigned char *)malloc(next_key_len);
        if (!next_key) {
            return;
        }

        memcpy(next_key, packet, 28);
        write_be32(next_key + 12, flags | ((*config_len > 0) ? 2 : 0));
        write_be32(next_key + 24, (uint32_t)combined_data_len);

        if (*config_len > 0) {
            memcpy(next_key + 28, *config_cache, *config_len);
            memcpy(next_key + 28 + *config_len, data, data_len);
        } else {
            memcpy(next_key + 28, data, data_len);
        }

        free(*key_cache);
        *key_cache = next_key;
        *key_len = next_key_len;
    }
}

static int open_and_claim_aoa(
    libusb_context *ctx,
    libusb_device_handle **handle_out,
    int *interface_out,
    int *in_ep_out,
    int *out_ep_out
) {
    libusb_device_handle *handle = open_from_list_filtered(
        ctx,
        aoa_devices,
        sizeof(aoa_devices) / sizeof(aoa_devices[0]),
        1,
        1
    );

    if (!handle) {
        printf("AOA device not found\n");
        print_matching_devices(ctx, aoa_devices, sizeof(aoa_devices) / sizeof(aoa_devices[0]), "AOA");
        return -1;
    }

    int interface_number = -1;
    int in_ep = 0;
    int out_ep = 0;

    int r = find_bulk_endpoints(handle, &interface_number, &in_ep, &out_ep);
    if (r != 0) {
        printf("bulk endpoints not found\n");
        libusb_close(handle);
        return -1;
    }

    if (libusb_kernel_driver_active(handle, interface_number) == 1) {
        printf("detaching kernel driver from interface %d\n", interface_number);
        libusb_detach_kernel_driver(handle, interface_number);
    }

    r = libusb_claim_interface(handle, interface_number);
    if (r != 0) {
        printf("claim interface failed: %s (%d)\n", libusb_error_name(r), r);
        if (r == LIBUSB_ERROR_BUSY) {
            printf("interface is busy; stop old rk_aoa_bridge processes or other USB users first\n");
        }
        libusb_close(handle);
        return -1;
    }

    *handle_out = handle;
    *interface_out = interface_number;
    *in_ep_out = in_ep;
    *out_ep_out = out_ep;
    return 0;
}

static void print_usage(const char *argv0) {
    printf("Usage: %s WINDOWS_IP [video_port] [control_port] [options]\n", argv0);
    printf("       %s --list\n", argv0);
    printf("       %s --start-all\n", argv0);
    printf("Options:\n");
    printf("  --device-index N   bind the Nth AOA device from --list\n");
    printf("  --usb BUS:ADDR     bind an AOA device by lsusb bus/address, for example 005:032\n");
    printf("Example:\n");
    printf("  %s 192.168.110.23 9001 9002 --device-index 0\n", argv0);
    printf("  %s 192.168.110.23 9011 9012 --usb 005:033\n", argv0);
}

static int parse_usb_selector(const char *text) {
    int bus = -1;
    int address = -1;

    if (!text || sscanf(text, "%d:%d", &bus, &address) != 2) {
        return -1;
    }

    if (bus < 0 || bus > 255 || address < 0 || address > 255) {
        return -1;
    }

    selected_usb_bus = bus;
    selected_usb_address = address;
    return 0;
}

int main(int argc, char **argv) {
    int list_only = 0;
    int start_all_only = 0;
    int positional_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--start-all") == 0) {
            start_all_only = 1;
        } else if (strcmp(argv[i], "--device-index") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            selected_device_index = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--usb") == 0) {
            if (i + 1 >= argc || parse_usb_selector(argv[++i]) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            print_usage(argv[0]);
            return 1;
        } else if (positional_count == 0) {
            windows_ip = argv[i];
            positional_count++;
        } else if (positional_count == 1) {
            video_port = atoi(argv[i]);
            positional_count++;
        } else if (positional_count == 2) {
            control_port = atoi(argv[i]);
            positional_count++;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (video_port <= 0) video_port = DEFAULT_VIDEO_PORT;
    if (control_port <= 0) control_port = DEFAULT_CONTROL_PORT;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    libusb_context *ctx = NULL;
    libusb_device_handle *handle = NULL;
    int interface_number = -1;
    int in_ep = 0;
    int out_ep = 0;
    int video_fd = -1;

    int r = libusb_init(&ctx);
    if (r != 0) {
        printf("libusb_init failed: %s (%d)\n", libusb_error_name(r), r);
        return 1;
    }

    if (list_only) {
        print_matching_devices(ctx, aoa_devices, sizeof(aoa_devices) / sizeof(aoa_devices[0]), "AOA");
        print_matching_devices(ctx, starter_devices, sizeof(starter_devices) / sizeof(starter_devices[0]), "Starter");
        libusb_exit(ctx);
        return 0;
    }

    if (start_all_only) {
        int sr = start_all_starter_devices(ctx);
        if (sr == 0) {
            printf("wait 2 seconds, then run: %s --list\n", argv[0]);
            usleep(2000000);
        }
        libusb_exit(ctx);
        return sr == 0 ? 0 : 1;
    }

    if (!windows_ip) {
        print_usage(argv[0]);
        libusb_exit(ctx);
        return 1;
    }

    if (selected_usb_bus >= 0) {
        printf("device selector: usb=%03d:%03d\n", selected_usb_bus, selected_usb_address);
    } else if (selected_device_index >= 0) {
        printf("device selector: index=%d\n", selected_device_index);
    } else {
        printf("device selector: first available AOA device\n");
    }

    if (start_aoa_if_needed(ctx) != 0) {
        libusb_exit(ctx);
        return 1;
    }

    if (open_and_claim_aoa(ctx, &handle, &interface_number, &in_ep, &out_ep) != 0) {
        libusb_exit(ctx);
        return 1;
    }

    pthread_mutex_lock(&aoa_mutex);
    aoa_handle = handle;
    aoa_out_ep = out_ep;
    pthread_mutex_unlock(&aoa_mutex);

    if (aoa_hid_register_touch(handle) == 0) {
        pthread_mutex_lock(&hid_mutex);
        hid_ready = 1;
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID touch registered\n");
    } else {
        printf("AOA HID touch register failed; control may not work\n");
    }

    if (aoa_hid_register_keyboard(handle) == 0) {
        pthread_mutex_lock(&hid_mutex);
        keyboard_ready = 1;
        pthread_mutex_unlock(&hid_mutex);
        printf("AOA HID keyboard registered\n");
    } else {
        printf("AOA HID keyboard register failed; keyboard control may not work\n");
    }

    pthread_t control_thread;
    if (pthread_create(&control_thread, NULL, control_server_thread, NULL) != 0) {
        perror("pthread_create control");
        running = 0;
    }

    struct usb_stream_reader reader;
    memset(&reader, 0, sizeof(reader));
    reader.handle = handle;
    reader.ep = in_ep;

    uint64_t last_print_ms = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    unsigned char *cached_config = NULL;
    size_t cached_config_len = 0;
    unsigned char *cached_key = NULL;
    size_t cached_key_len = 0;

    printf("video bridge target: %s:%d\n", windows_ip, video_port);
    printf("reading AOA USB video stream...\n");

    while (running) {
        unsigned char *packet = NULL;
        size_t packet_len = 0;

        r = read_avc1_packet(&reader, &packet, &packet_len);
        if (r != 0) {
            if (r == LIBUSB_ERROR_NO_DEVICE) {
                printf("AOA USB device disconnected, stopping this bridge instance\n");
                running = 0;
                break;
            }
            usleep(10000);
            continue;
        }

        update_cached_key_packet(
            packet,
            packet_len,
            &cached_config,
            &cached_config_len,
            &cached_key,
            &cached_key_len
        );

        if (!stream_forward_enabled) {
            if (video_fd >= 0) {
                close(video_fd);
                video_fd = -1;
            }
            free(packet);
            continue;
        }

        if (video_fd < 0) {
            video_fd = connect_windows_video();
            if (video_fd < 0) {
                free(packet);
                usleep(1000000);
                continue;
            }
            if (cached_key && cached_key_len > 0) {
                if (send_all(video_fd, cached_key, cached_key_len) != 0) {
                    printf("send cached keyframe to Windows failed, reconnecting...\n");
                    close(video_fd);
                    video_fd = -1;
                    free(packet);
                    continue;
                }
                printf("sent cached keyframe to Windows size=%zu\n", cached_key_len);
            }
        }

        if (send_all(video_fd, packet, packet_len) != 0) {
            printf("send video to Windows failed, reconnecting...\n");
            close(video_fd);
            video_fd = -1;
            free(packet);
            continue;
        }

        packets++;
        bytes += packet_len;

        uint64_t now = now_ms();
        if (now - last_print_ms >= 1000) {
            uint32_t width = read_be32(packet + 4);
            uint32_t height = read_be32(packet + 8);
            uint32_t flags = read_be32(packet + 12);
            uint32_t data_len = read_be32(packet + 24);
            printf("forward packets=%llu bytes=%llu latest=%ux%u flags=%u size=%u\n",
                   (unsigned long long)packets,
                   (unsigned long long)bytes,
                   width, height, flags, data_len);
            last_print_ms = now;
        }

        free(packet);
    }

    if (video_fd >= 0) close(video_fd);
    free(cached_config);
    free(cached_key);

    pthread_mutex_lock(&hid_mutex);
    hid_ready = 0;
    keyboard_ready = 0;
    pthread_mutex_unlock(&hid_mutex);

    aoa_hid_unregister_keyboard(handle);
    aoa_hid_unregister_touch(handle);

    pthread_mutex_lock(&aoa_mutex);
    aoa_handle = NULL;
    aoa_out_ep = 0;
    pthread_mutex_unlock(&aoa_mutex);

    if (interface_number >= 0) {
        libusb_release_interface(handle, interface_number);
    }

    libusb_close(handle);
    libusb_exit(ctx);

    return 0;
}
