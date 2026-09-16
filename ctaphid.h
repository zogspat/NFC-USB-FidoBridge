#ifndef CTAPHID_H
#define CTAPHID_H

#include <stdint.h>
#include <stdbool.h>

// Standard CTAPHID Commands
#define CTAPHID_PING  0x81
#define CTAPHID_MSG   0x83
#define CTAPHID_INIT  0x86
#define CTAPHID_CBOR  0x90
#define CTAPHID_ERROR 0xBF
#define CTAPHID_KEEPALIVE 0xBB

// Max message length for CTAPHID is typically 7609 bytes
#define CTAPHID_MAX_MSG_LEN 7609

// A single 64-byte USB HID report
typedef struct {
    uint8_t data[64];
} ctaphid_packet_t;

// Callback signature for when a response packet is ready to be sent to the host
typedef void (*ctaphid_send_cb)(const ctaphid_packet_t* packet);

// Callback signature for when a complete (fully defragmented) message is received
typedef void (*ctaphid_msg_cb)(uint32_t channel, uint8_t cmd, const uint8_t* payload, uint16_t length);

// Initialize the parser state machine
void ctaphid_init(ctaphid_send_cb send_func, ctaphid_msg_cb msg_func);

// Feed a 64-byte packet from the host into the parser
void ctaphid_handle_packet(const uint8_t* report);

// Fragment and send a payload back to the host
void ctaphid_send_message(uint32_t channel, uint8_t cmd, const uint8_t* payload, uint16_t length);

// Send an error code back to the host
void ctaphid_send_error(uint32_t channel, uint8_t err_code);

// Send a keepalive packet back to the host (e.g. status = 0x02 for UP_NEEDED)
void ctaphid_send_keepalive(uint32_t channel, uint8_t status);

#endif // CTAPHID_H

