/*
 * Hardware smoke test for the candleLight/gs_usb adapter used by
 * hex-motor-gui.  It deliberately has no dependency on SocketCAN so it also
 * works in WSL kernels that do not ship the gs_usb kernel module.
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra tools/gsusb_smoke.c \
 *           -lusb-1.0 -o /tmp/gsusb_smoke
 */

#define _POSIX_C_SOURCE 200809L

#include <libusb-1.0/libusb.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    kVid = 0x1209,
    kPid = 0x2323,
    kDefaultNodeId = 0x21,
    kChannel = 0,
    kBulkIn = 0x81,
    kBulkOut = 0x01,
    kHostFrameSizeFdMode = 12 + 64,
};

enum {
    kRequestHostFormat = 0,
    kRequestBitTiming = 1,
    kRequestMode = 2,
    kRequestBtConst = 4,
    kRequestDataBitTiming = 10,
    kModeStart = 1,
    kModeFd = 1U << 8,
    kFlagFd = 1U << 1,
    kFlagBrs = 1U << 2,
};
static uint8_t g_node_id = kDefaultNodeId;


typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[64];
    double received_at_ms;
} RxFrame;

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void store_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U);
    p[3] = (uint8_t)(value >> 24U);
}

static double monotonic_ms(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static bool control_out(libusb_device_handle *device,
                        uint8_t request,
                        uint16_t value,
                        uint8_t *data,
                        uint16_t length)
{
    const int result = libusb_control_transfer(device,
                                                0x41, /* vendor, interface, OUT */
                                                request,
                                                value,
                                                0,
                                                data,
                                                length,
                                                1000);
    if (result != (int)length) {
        fprintf(stderr,
                "control OUT request %u failed: %s (%d/%u bytes)\n",
                request,
                libusb_error_name(result),
                result,
                length);
        return false;
    }
    return true;
}

static bool configure_adapter(libusb_device_handle *device)
{
    uint8_t host_format[4];
    store_le32(host_format, 0x0000BEEFU);
    if (!control_out(device, kRequestHostFormat, 1, host_format, sizeof(host_format))) {
        return false;
    }

    uint8_t constants[40] = {0};
    const int constant_size = libusb_control_transfer(device,
                                                       0xC1,
                                                       kRequestBtConst,
                                                       kChannel,
                                                       0,
                                                       constants,
                                                       sizeof(constants),
                                                       1000);
    if (constant_size >= 8) {
        printf("adapter features=0x%08X clock=%u Hz\n",
               load_le32(constants),
               load_le32(constants + 4));
    } else {
        fprintf(stderr, "BT_CONST probe failed: %s\n", libusb_error_name(constant_size));
    }

    /* 80 MHz / (1 * (1 + 31 + 32 + 16)) = 1 Mbit/s, SP 80%, SJW 5. */
    const uint32_t nominal_words[5] = {31, 32, 16, 5, 1};
    /* 80 MHz / (1 * (1 + 5 + 6 + 4)) = 5 Mbit/s, SP 75%, SJW 3. */
    const uint32_t data_words[5] = {5, 6, 4, 3, 1};
    uint8_t nominal[20];
    uint8_t data[20];
    for (size_t i = 0; i < 5; ++i) {
        store_le32(nominal + i * 4, nominal_words[i]);
        store_le32(data + i * 4, data_words[i]);
    }
    if (!control_out(device, kRequestBitTiming, kChannel, nominal, sizeof(nominal)) ||
        !control_out(device, kRequestDataBitTiming, kChannel, data, sizeof(data))) {
        return false;
    }

    uint8_t mode[8] = {0};
    store_le32(mode, kModeStart);
    store_le32(mode + 4, kModeFd);
    return control_out(device, kRequestMode, kChannel, mode, sizeof(mode));
}

static bool send_frame(libusb_device_handle *device,
                       uint32_t id,
                       const uint8_t *payload,
                       uint8_t length,
                       bool fd,
                       bool brs)
{
    static uint32_t echo_id;
    uint8_t frame[kHostFrameSizeFdMode] = {0};
    store_le32(frame, echo_id++);
    store_le32(frame + 4, id);
    frame[8] = length;
    frame[9] = kChannel;
    frame[10] = (fd ? kFlagFd : 0U) | (brs ? kFlagBrs : 0U);
    if (length > 0) {
        memcpy(frame + 12, payload, length);
    }
    int transferred = 0;
    const int result = libusb_bulk_transfer(device,
                                            kBulkOut,
                                            frame,
                                            sizeof(frame),
                                            &transferred,
                                            1000);
    if (result != LIBUSB_SUCCESS || transferred != (int)sizeof(frame)) {
        fprintf(stderr,
                "bulk TX 0x%03X failed: %s (%d/%zu bytes)\n",
                id,
                libusb_error_name(result),
                transferred,
                sizeof(frame));
        return false;
    }
    return true;
}

static int fd_dlc_to_length(uint8_t dlc)
{
    static const uint8_t lengths[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return lengths[dlc & 0x0FU];
}

static bool receive_frame(libusb_device_handle *device, RxFrame *frame, unsigned timeout_ms)
{
    const double deadline = monotonic_ms() + (double)timeout_ms;
    while (monotonic_ms() < deadline) {
        uint8_t raw[512] = {0};
        int transferred = 0;
        const unsigned slice = (unsigned)(deadline - monotonic_ms());
        const int result = libusb_bulk_transfer(device,
                                                kBulkIn,
                                                raw,
                                                sizeof(raw),
                                                &transferred,
                                                slice > 250U ? 250U : slice + 1U);
        if (result == LIBUSB_ERROR_TIMEOUT) {
            continue;
        }
        if (result != LIBUSB_SUCCESS) {
            fprintf(stderr, "bulk RX failed: %s\n", libusb_error_name(result));
            return false;
        }
        if (transferred < 12 || load_le32(raw) != UINT32_MAX || raw[9] != kChannel) {
            continue; /* TX echo or a malformed/other-channel packet. */
        }
        frame->id = load_le32(raw + 4) & 0x7FFU;
        frame->dlc = raw[8];
        frame->flags = raw[10];
        const int length = (frame->flags & kFlagFd) != 0 ? fd_dlc_to_length(frame->dlc)
                                                         : (frame->dlc > 8 ? 8 : frame->dlc);
        if (transferred < 12 + length) {
            continue;
        }
        memcpy(frame->data, raw + 12, (size_t)length);
        frame->received_at_ms = monotonic_ms();
        return true;
    }
    return false;
}

static bool wait_for(libusb_device_handle *device,
                     uint32_t id,
                     uint8_t first_byte,
                     bool match_first_byte,
                     unsigned timeout_ms,
                     RxFrame *matched)
{
    const double deadline = monotonic_ms() + (double)timeout_ms;
    while (monotonic_ms() < deadline) {
        RxFrame frame = {0};
        const unsigned left = (unsigned)(deadline - monotonic_ms());
        if (!receive_frame(device, &frame, left + 1U)) {
            return false;
        }
        if (frame.id >= 0x701U && frame.id <= 0x77FU && frame.id != id) {
            printf("observed heartbeat from unexpected Node-ID: 0x%03X state=0x%02X\n",
                   frame.id,
                   frame.data[0]);
        }
        if (frame.id == id && (!match_first_byte || frame.data[0] == first_byte)) {
            if (matched != NULL) {
                *matched = frame;
            }
            return true;
        }
    }
    return false;
}

static bool sdo_request(libusb_device_handle *device,
                        const uint8_t request[8],
                        uint8_t expected_command,
                        RxFrame *response)
{
    if (!send_frame(device, 0x600U + g_node_id, request, 8, false, false) ||
        !wait_for(device, 0x580U + g_node_id, 0, false, 2000, response)) {
        fprintf(stderr, "SDO 0x%02X%02X:%u timed out\n", request[2], request[1], request[3]);
        return false;
    }
    if (response->data[0] == 0x80) {
        fprintf(stderr,
                "SDO 0x%02X%02X:%u aborted: 0x%08X\n",
                request[2],
                request[1],
                request[3],
                load_le32(response->data + 4));
        return false;
    }
    if (response->data[0] != expected_command || response->data[1] != request[1] ||
        response->data[2] != request[2] || response->data[3] != request[3]) {
        fprintf(stderr, "unexpected SDO response command 0x%02X\n", response->data[0]);
        return false;
    }
    return true;
}

static bool sdo_write(libusb_device_handle *device,
                      uint16_t index,
                      uint8_t subindex,
                      uint32_t value,
                      uint8_t size)
{
    const uint8_t command = size == 1 ? 0x2F : (size == 2 ? 0x2B : 0x23);
    uint8_t request[8] = {command, (uint8_t)index, (uint8_t)(index >> 8U), subindex};
    store_le32(request + 4, value);
    RxFrame response;
    return sdo_request(device, request, 0x60, &response);
}

static bool send_nmt(libusb_device_handle *device, uint8_t command)
{
    const uint8_t payload[2] = {command, g_node_id};
    return send_frame(device, 0, payload, sizeof(payload), false, false);
}

static bool parse_node_id(const char *text, uint8_t *node_id)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value < 1 || value > 127) {
        return false;
    }
    *node_id = (uint8_t)value;
    return true;
}

int main(int argc, char **argv)
{
    bool change_requested = false;
    uint8_t new_node_id = 0;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--node-id") == 0 && index + 1 < argc) {
            if (!parse_node_id(argv[++index], &g_node_id)) {
                fputs("invalid --node-id; expected 1..127\n", stderr);
                return 2;
            }
        } else if (strcmp(argv[index], "--set-node-id") == 0 && index + 1 < argc) {
            if (!parse_node_id(argv[++index], &new_node_id)) {
                fputs("invalid --set-node-id; expected 1..127\n", stderr);
                return 2;
            }
            change_requested = true;
        } else {
            fprintf(stderr,
                    "usage: %s [--node-id ID] [--set-node-id NEW_ID]\n",
                    argv[0]);
            return 2;
        }
    }
    int exit_code = 1;
    libusb_context *context = NULL;
    libusb_device_handle *device = NULL;
    if (libusb_init(&context) != LIBUSB_SUCCESS) {
        fputs("libusb initialization failed\n", stderr);
        return 1;
    }
    device = libusb_open_device_with_vid_pid(context, kVid, kPid);
    if (device == NULL) {
        fprintf(stderr, "gs_usb adapter %04X:%04X not found\n", kVid, kPid);
        goto cleanup;
    }
    (void)libusb_set_auto_detach_kernel_driver(device, 1);
    if (libusb_claim_interface(device, 0) != LIBUSB_SUCCESS) {
        fputs("could not claim gs_usb interface 0\n", stderr);
        goto cleanup;
    }
    if (!configure_adapter(device)) {
        goto release;
    }
    puts("configured CAN-FD: nominal 1 Mbit/s @ 80%, data 5 Mbit/s @ 75%");

    RxFrame frame;
    if (!wait_for(device, 0x700U + g_node_id, 0x7F, true, 5000, &frame)) {
        fprintf(stderr,
                "FAIL: pre-operational heartbeat 0x%03X was not received\n",
                0x700U + g_node_id);
        goto release;
    }
    printf("PASS: heartbeat 0x%03X state=0x%02X\n", frame.id, frame.data[0]);

    if (change_requested) {
        if (!sdo_write(device, 0x2001, 1, new_node_id, 1) ||
            !sdo_write(device, 0x1010, 1, 0x65766173U, 4)) {
            goto release;
        }
        printf("PASS: saved Node-ID 0x%02X; restart required, current ID remains 0x%02X\n",
               new_node_id,
               g_node_id);
        exit_code = 0;
        goto release;
    }

    const uint8_t identity_request[8] = {0x40, 0x18, 0x10, 1, 0, 0, 0, 0};
    if (!sdo_request(device, identity_request, 0x43, &frame)) {
        goto release;
    }
    printf("PASS: SDO 0x1018:1 vendor-id=0x%08X\n", load_le32(frame.data + 4));

    /* Configure TPDO1 as 3 x 32-bit mappings: 12 bytes forces CAN-FD+BRS. */
    const uint32_t application_value_mapping = 0x20000420U;
    if (!sdo_write(device, 0x1A00, 0, 0, 1) ||
        !sdo_write(device, 0x1A00, 1, application_value_mapping, 4) ||
        !sdo_write(device, 0x1A00, 2, application_value_mapping, 4) ||
        !sdo_write(device, 0x1A00, 3, application_value_mapping, 4) ||
        !sdo_write(device, 0x1A00, 0, 3, 1) ||
        !sdo_write(device, 0x1800, 2, 255, 1) ||
        !sdo_write(device, 0x1800, 5, 100, 2) ||
        !sdo_write(device, 0x2000, 4, 0x12345678U, 4)) {
        goto release;
    }

    if (!send_nmt(device, 0x01) ||
        !wait_for(device, 0x700U + g_node_id, 0x05, true, 2000, &frame)) {
        fputs("FAIL: NMT operational heartbeat was not received\n", stderr);
        goto release;
    }
    puts("PASS: NMT start transitioned node to operational");

    if (!wait_for(device, 0x180U + g_node_id, 0, false, 2000, &frame)) {
        fputs("FAIL: TPDO1 was not received\n", stderr);
        goto release;
    }
    if ((frame.flags & (kFlagFd | kFlagBrs)) != (kFlagFd | kFlagBrs) || frame.dlc != 9) {
        fprintf(stderr,
                "FAIL: TPDO1 flags=0x%02X DLC=%u (expected FD+BRS, DLC 9/12 bytes)\n",
                frame.flags,
                frame.dlc);
        goto release;
    }
    for (size_t offset = 0; offset < 12; offset += 4) {
        if (load_le32(frame.data + offset) != 0x12345678U) {
            fputs("FAIL: TPDO1 mapped payload mismatch\n", stderr);
            goto release;
        }
    }
    puts("PASS: TPDO1 is a 12-byte CAN-FD frame with bit-rate switching");

    /* Restore the runtime communication configuration and default Pre-op state. */
    if (!send_nmt(device, 0x82) ||
        !wait_for(device, 0x700U + g_node_id, 0x00, true, 2000, &frame) ||
        !wait_for(device, 0x700U + g_node_id, 0x7F, true, 2000, &frame)) {
        fputs("FAIL: reset-communication heartbeat sequence was not received\n", stderr);
        goto release;
    }
    puts("PASS: communication reset restored the default Pre-operational state");
    exit_code = 0;

release:
    (void)libusb_release_interface(device, 0);
cleanup:
    if (device != NULL) {
        libusb_close(device);
    }
    libusb_exit(context);
    return exit_code;
}
