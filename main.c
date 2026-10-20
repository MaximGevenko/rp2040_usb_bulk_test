#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/regs/usb.h"
#include "hardware/structs/usb.h"
#include "hardware/address_mapped.h"

// -----------------------------------------------------------------------------
// Minimal vendor-specific USB Full-Speed device for RP2040.
// One Bulk-IN endpoint (EP1 IN, address 0x81) continuously streams a test
// pattern to the host. No TinyUSB is used.
// -----------------------------------------------------------------------------

#define USB_VID                    0xCAFEu   // Development/test only
#define USB_PID                    0x4002u   // Development/test only
#define BULK_IN_EP_ADDR            0x81u
#define BULK_MAX_PACKET            64u
#define TEST_BUFFER_SIZE           1536u     // 256 CIR samples * 6 bytes

#define USB_DIR_IN                 0x80u
#define USB_DIR_OUT                0x00u

#define USB_DT_DEVICE              0x01u
#define USB_DT_CONFIGURATION       0x02u
#define USB_DT_STRING              0x03u
#define USB_DT_INTERFACE           0x04u
#define USB_DT_ENDPOINT            0x05u

#define USB_REQ_GET_STATUS         0x00u
#define USB_REQ_SET_ADDRESS        0x05u
#define USB_REQ_GET_DESCRIPTOR     0x06u
#define USB_REQ_GET_CONFIGURATION  0x08u
#define USB_REQ_SET_CONFIGURATION  0x09u
#define USB_REQ_GET_INTERFACE      0x0Au
#define USB_REQ_SET_INTERFACE      0x0Bu

#define USB_TRANSFER_BULK          0x02u

#define EP0_IN_STATUS_BIT          (1u << 0)
#define EP0_OUT_STATUS_BIT         (1u << 1)
#define EP1_IN_STATUS_BIT          (1u << 2)

#define MIN_U16(a, b) ((uint16_t)((a) < (b) ? (a) : (b)))

#define usb_hw_set_alias   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear_alias ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_packet_t;

static const uint8_t device_descriptor[] = {
    18,                         // bLength
    USB_DT_DEVICE,              // bDescriptorType
    0x10, 0x01,                 // bcdUSB = 1.10
    0x00,                       // bDeviceClass (defined by interface)
    0x00,                       // bDeviceSubClass
    0x00,                       // bDeviceProtocol
    64,                         // bMaxPacketSize0
    (uint8_t)(USB_VID & 0xFF),
    (uint8_t)(USB_VID >> 8),
    (uint8_t)(USB_PID & 0xFF),
    (uint8_t)(USB_PID >> 8),
    0x00, 0x01,                 // bcdDevice = 1.00
    1,                          // iManufacturer
    2,                          // iProduct
    3,                          // iSerialNumber
    1                           // bNumConfigurations
};

// Configuration + one vendor-specific interface + one Bulk-IN endpoint.
static const uint8_t configuration_descriptor[] = {
    // Configuration descriptor
    9,
    USB_DT_CONFIGURATION,
    25, 0,                      // wTotalLength = 9 + 9 + 7
    1,                          // bNumInterfaces
    1,                          // bConfigurationValue
    0,                          // iConfiguration
    0x80,                       // bus powered
    50,                         // 100 mA

    // Interface descriptor
    9,
    USB_DT_INTERFACE,
    0,                          // bInterfaceNumber
    0,                          // bAlternateSetting
    1,                          // bNumEndpoints
    0xFF,                       // vendor specific
    0x00,
    0x00,
    0,                          // iInterface

    // Endpoint 1 IN descriptor
    7,
    USB_DT_ENDPOINT,
    BULK_IN_EP_ADDR,
    USB_TRANSFER_BULK,
    64, 0,                      // wMaxPacketSize = 64
    0                           // bInterval (ignored for Bulk)
};

static const char *const string_table[] = {
    "UWB Project",
    "RP2040 Raw USB Bulk Test",
    "000001"
};

static uint8_t ep0_reply[64];
static uint8_t test_buffer[TEST_BUFFER_SIZE];

static volatile bool usb_configured = false;
static volatile bool bulk_streaming = false;
static uint8_t current_configuration = 0;
static uint8_t pending_address = 0;
static uint8_t ep0_in_pid = 1;
static uint8_t bulk_in_pid = 0;
static uint32_t stream_offset = 0;

typedef enum {
    CTRL_ACTION_NONE = 0,
    CTRL_ACTION_SET_ADDRESS,
    CTRL_ACTION_START_STREAM
} control_action_t;

static volatile control_action_t control_action = CTRL_ACTION_NONE;

static inline volatile uint8_t *ep0_buffer(void) {
    return &usb_dpram->ep0_buf_a[0];
}

static inline volatile uint8_t *bulk_buffer(void) {
    return &usb_dpram->epx_data[0];
}

static inline uint32_t dpram_offset(const volatile uint8_t *p) {
    return (uint32_t)((uintptr_t)p - (uintptr_t)usb_dpram);
}

static void ep0_arm_out_status(void) {
    // Status OUT stage of a control read uses DATA1 and has zero payload.
    usb_dpram->ep_buf_ctrl[0].out = USB_BUF_CTRL_AVAIL | USB_BUF_CTRL_DATA1_PID;
}

static void ep0_send(const uint8_t *data, uint16_t length) {
    if (length > 64) {
        length = 64;
    }

    if (length && data) {
        memcpy((void *)ep0_buffer(), data, length);
    }

    uint32_t ctrl = (uint32_t)length |
                    USB_BUF_CTRL_AVAIL |
                    USB_BUF_CTRL_FULL |
                    (ep0_in_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID);

    ep0_in_pid ^= 1u;
    usb_dpram->ep_buf_ctrl[0].in = ctrl;
}

static uint16_t build_string_descriptor(uint8_t index) {
    if (index == 0) {
        // English (United States), LANGID 0x0409
        ep0_reply[0] = 4;
        ep0_reply[1] = USB_DT_STRING;
        ep0_reply[2] = 0x09;
        ep0_reply[3] = 0x04;
        return 4;
    }

    if (index < 1 || index > 3) {
        return 0;
    }

    const char *s = string_table[index - 1];
    size_t n = strlen(s);
    if (n > 31) {
        n = 31;
    }

    ep0_reply[0] = (uint8_t)(2 + 2 * n);
    ep0_reply[1] = USB_DT_STRING;

    for (size_t i = 0; i < n; ++i) {
        ep0_reply[2 + 2 * i] = (uint8_t)s[i];
        ep0_reply[3 + 2 * i] = 0;
    }

    return ep0_reply[0];
}

static void bulk_send_next_packet(void) {
    if (!usb_configured || !bulk_streaming) {
        return;
    }

    const uint16_t length = BULK_MAX_PACKET;

    memcpy((void *)bulk_buffer(), &test_buffer[stream_offset], length);

    stream_offset += length;
    if (stream_offset >= TEST_BUFFER_SIZE) {
        stream_offset = 0;
    }

    uint32_t ctrl = (uint32_t)length |
                    USB_BUF_CTRL_AVAIL |
                    USB_BUF_CTRL_FULL |
                    (bulk_in_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID);

    bulk_in_pid ^= 1u;
    usb_dpram->ep_buf_ctrl[1].in = ctrl;
}

static void configure_bulk_endpoint(void) {
    uint32_t ctrl = EP_CTRL_ENABLE_BITS |
                    EP_CTRL_INTERRUPT_PER_BUFFER |
                    (USB_TRANSFER_BULK << EP_CTRL_BUFFER_TYPE_LSB) |
                    dpram_offset(bulk_buffer());

    // EP1 uses ep_ctrl[0].
    usb_dpram->ep_ctrl[0].in = ctrl;
}

static void handle_get_descriptor(const usb_setup_packet_t *setup) {
    uint8_t descriptor_type = (uint8_t)(setup->wValue >> 8);
    uint8_t descriptor_index = (uint8_t)(setup->wValue & 0xFF);

    switch (descriptor_type) {
        case USB_DT_DEVICE: {
            uint16_t n = MIN_U16((uint16_t)sizeof(device_descriptor), setup->wLength);
            ep0_send(device_descriptor, n);
            break;
        }

        case USB_DT_CONFIGURATION: {
            uint16_t n = MIN_U16((uint16_t)sizeof(configuration_descriptor), setup->wLength);
            ep0_send(configuration_descriptor, n);
            break;
        }

        case USB_DT_STRING: {
            uint16_t descriptor_length = build_string_descriptor(descriptor_index);
            uint16_t n = MIN_U16(descriptor_length, setup->wLength);
            ep0_send(ep0_reply, n);
            break;
        }

        default:
            // Minimal test device: respond with a zero-length packet for unsupported descriptors.
            ep0_send(NULL, 0);
            break;
    }
}

static void handle_setup_request(void) {
    const volatile usb_setup_packet_t *raw =
        (const volatile usb_setup_packet_t *)&usb_dpram->setup_packet;

    usb_setup_packet_t setup;
    memcpy(&setup, (const void *)raw, sizeof(setup));

    // Every SETUP transaction resets the data toggle for the first IN data packet.
    ep0_in_pid = 1;
    control_action = CTRL_ACTION_NONE;

    const bool direction_in = (setup.bmRequestType & USB_DIR_IN) != 0;

    if (direction_in) {
        switch (setup.bRequest) {
            case USB_REQ_GET_DESCRIPTOR:
                handle_get_descriptor(&setup);
                break;

            case USB_REQ_GET_CONFIGURATION:
                ep0_reply[0] = current_configuration;
                ep0_send(ep0_reply, MIN_U16(1, setup.wLength));
                break;

            case USB_REQ_GET_STATUS:
                ep0_reply[0] = 0;
                ep0_reply[1] = 0;
                ep0_send(ep0_reply, MIN_U16(2, setup.wLength));
                break;

            case USB_REQ_GET_INTERFACE:
                ep0_reply[0] = 0;
                ep0_send(ep0_reply, MIN_U16(1, setup.wLength));
                break;

            default:
                ep0_send(NULL, 0);
                break;
        }
    } else {
        switch (setup.bRequest) {
            case USB_REQ_SET_ADDRESS:
                pending_address = (uint8_t)(setup.wValue & 0x7F);
                control_action = CTRL_ACTION_SET_ADDRESS;
                ep0_send(NULL, 0); // status IN
                break;

            case USB_REQ_SET_CONFIGURATION:
                current_configuration = (uint8_t)(setup.wValue & 0xFF);
                usb_configured = current_configuration != 0;
                bulk_streaming = false;
                bulk_in_pid = 0;
                stream_offset = 0;
                control_action = usb_configured ? CTRL_ACTION_START_STREAM : CTRL_ACTION_NONE;
                ep0_send(NULL, 0); // status IN
                break;

            case USB_REQ_SET_INTERFACE:
            default:
                ep0_send(NULL, 0); // acknowledge minimal standard OUT request
                break;
        }
    }
}

static void handle_ep0_in_complete(void) {
    switch (control_action) {
        case CTRL_ACTION_SET_ADDRESS:
            usb_hw->dev_addr_ctrl = pending_address;
            control_action = CTRL_ACTION_NONE;
            break;

        case CTRL_ACTION_START_STREAM:
            control_action = CTRL_ACTION_NONE;
            bulk_streaming = true;
            bulk_send_next_packet();
            break;

        case CTRL_ACTION_NONE:
        default:
            // Complete the status OUT stage after a control-read transfer.
            ep0_arm_out_status();
            break;
    }
}

static void handle_buffer_status(void) {
    uint32_t status = usb_hw->buf_status;

    if (status & EP0_IN_STATUS_BIT) {
        usb_hw_clear_alias->buf_status = EP0_IN_STATUS_BIT;
        handle_ep0_in_complete();
    }

    if (status & EP0_OUT_STATUS_BIT) {
        usb_hw_clear_alias->buf_status = EP0_OUT_STATUS_BIT;
        // Nothing to do for the zero-length status OUT stage.
    }

    if (status & EP1_IN_STATUS_BIT) {
        usb_hw_clear_alias->buf_status = EP1_IN_STATUS_BIT;
        // Host consumed the last packet: immediately queue the next one.
        bulk_send_next_packet();
    }
}

static void handle_bus_reset(void) {
    usb_hw->dev_addr_ctrl = 0;
    current_configuration = 0;
    pending_address = 0;
    usb_configured = false;
    bulk_streaming = false;
    control_action = CTRL_ACTION_NONE;
    ep0_in_pid = 1;
    bulk_in_pid = 0;
    stream_offset = 0;

    configure_bulk_endpoint();
}

static void usbctrl_irq_handler(void) {
    uint32_t ints = usb_hw->ints;

    if (ints & USB_INTS_SETUP_REQ_BITS) {
        // Acknowledge the SETUP condition before processing the packet.
        usb_hw_clear_alias->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;
        handle_setup_request();
    }

    if (ints & USB_INTS_BUFF_STATUS_BITS) {
        handle_buffer_status();
    }

    if (ints & USB_INTS_BUS_RESET_BITS) {
        usb_hw_clear_alias->sie_status = USB_SIE_STATUS_BUS_RESET_BITS;
        handle_bus_reset();
    }
}

static void usb_device_init_lowlevel(void) {
    reset_unreset_block_num_wait_blocking(RESET_USBCTRL);

    memset((void *)usb_dpram, 0, sizeof(*usb_dpram));

    irq_set_exclusive_handler(USBCTRL_IRQ, usbctrl_irq_handler);
    irq_set_enabled(USBCTRL_IRQ, true);

    // Connect the USB controller to the integrated PHY and use software connect.
    usb_hw->muxing = USB_USB_MUXING_TO_PHY_BITS |
                     USB_USB_MUXING_SOFTCON_BITS;

    // This board's VBUS sensing is not necessarily wired to the RP2040, so force VBUS present.
    usb_hw->pwr = USB_USB_PWR_VBUS_DETECT_BITS |
                  USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS;

    // Device mode, controller enabled.
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;

    // One interrupt for each EP0 buffer completion.
    usb_hw->sie_ctrl = USB_SIE_CTRL_EP0_INT_1BUF_BITS;

    usb_hw->inte = USB_INTS_BUFF_STATUS_BITS |
                   USB_INTS_BUS_RESET_BITS |
                   USB_INTS_SETUP_REQ_BITS;

    configure_bulk_endpoint();

    // Advertise a Full-Speed device by enabling the D+ pull-up.
    usb_hw_set_alias->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
}

int main(void) {
    // Pattern 00 01 02 ... FF, repeated. This allows optional integrity checking
    // on the host while still resembling a large continuous binary stream.
    for (uint32_t i = 0; i < TEST_BUFFER_SIZE; ++i) {
        test_buffer[i] = (uint8_t)(i & 0xFFu);
    }

    usb_device_init_lowlevel();

    // All USB work is interrupt-driven. The main core is deliberately idle,
    // which makes this a clean measurement of the low-level USB path.
    while (true) {
        tight_loop_contents();
    }
}
