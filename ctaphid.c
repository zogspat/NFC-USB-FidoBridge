#include "ctaphid.h"
#include <string.h>

static ctaphid_send_cb send_packet = NULL;
static ctaphid_msg_cb on_msg_complete = NULL;

// In a real device, this handles dynamic channel allocation
static uint32_t next_channel_id = 0x01020304;

// Receiver State Machine
static uint8_t  msg_buffer[CTAPHID_MAX_MSG_LEN];
static uint16_t msg_length = 0;
static uint16_t msg_received = 0;
static uint8_t  expected_seq = 0;
static uint32_t active_channel = 0;
static uint8_t  active_cmd = 0;
static bool     is_receiving = false;

void ctaphid_init(ctaphid_send_cb send_func, ctaphid_msg_cb msg_func) {
    send_packet = send_func;
    on_msg_complete = msg_func;
    is_receiving = false;
}

void ctaphid_handle_packet(const uint8_t* report) {
    if (!send_packet) return;

    uint32_t channel_id = (report[0] << 24) | (report[1] << 16) | (report[2] << 8) | report[3];
    
    // Check if it's an initialization packet (bit 7 is 1)
    if (report[4] & 0x80) {
        uint8_t cmd = report[4];
        uint16_t len = (report[5] << 8) | report[6];

        if (cmd == CTAPHID_INIT) {
            printf("CTAPHID_INIT received from host!\n");
            // INIT is special, it's always handled immediately and fits in one packet
            ctaphid_packet_t response;
            memset(&response, 0, sizeof(response));
            response.data[0] = report[0]; response.data[1] = report[1]; response.data[2] = report[2]; response.data[3] = report[3];
            response.data[4] = CTAPHID_INIT;
            response.data[5] = 0x00; response.data[6] = 17;
            memcpy(&response.data[7], &report[7], 8);
            response.data[15] = (next_channel_id >> 24) & 0xFF; response.data[16] = (next_channel_id >> 16) & 0xFF;
            response.data[17] = (next_channel_id >> 8) & 0xFF;  response.data[18] = next_channel_id & 0xFF;
            response.data[19] = 0x02; // Version
            response.data[20] = 0x01; response.data[21] = 0x00; response.data[22] = 0x00;
            response.data[23] = 0x04; // CBOR support
            send_packet(&response);
            return;
        }

        // Handle standard command packet (start of a transaction)
        if (len > CTAPHID_MAX_MSG_LEN) {
            // Send ERROR_INVALID_LEN (0x03)
            ctaphid_packet_t err;
            memset(&err, 0, sizeof(err));
            err.data[0] = report[0]; err.data[1] = report[1]; err.data[2] = report[2]; err.data[3] = report[3];
            err.data[4] = CTAPHID_ERROR;
            err.data[5] = 0x00; err.data[6] = 0x01;
            err.data[7] = 0x03; 
            send_packet(&err);
            return;
        }

        active_channel = channel_id;
        active_cmd = cmd;
        msg_length = len;
        
        // The first packet holds up to 57 bytes of payload
        uint16_t chunk_size = (len > 57) ? 57 : len;
        memcpy(msg_buffer, &report[7], chunk_size);
        msg_received = chunk_size;

        if (msg_received >= msg_length) {
            // Message fits perfectly in one packet
            if (on_msg_complete) on_msg_complete(active_channel, active_cmd, msg_buffer, msg_length);
            is_receiving = false;
        } else {
            // Wait for continuation packets
            expected_seq = 0;
            is_receiving = true;
        }
    } 
    else {
        // Continuation packet (bit 7 is 0)
        uint8_t seq = report[4];

        if (!is_receiving || channel_id != active_channel || seq != expected_seq) {
            // Ignore stray packets or send error depending on strictness
            return; 
        }

        // Each continuation packet holds up to 59 bytes
        uint16_t remaining = msg_length - msg_received;
        uint16_t chunk_size = (remaining > 59) ? 59 : remaining;
        
        memcpy(&msg_buffer[msg_received], &report[5], chunk_size);
        msg_received += chunk_size;
        expected_seq++;

        if (msg_received >= msg_length) {
            // We have defragmented the full message!
            if (on_msg_complete) on_msg_complete(active_channel, active_cmd, msg_buffer, msg_length);
            is_receiving = false;
        }
    }
}

void ctaphid_send_message(uint32_t channel, uint8_t cmd, const uint8_t* payload, uint16_t length) {
    if (!send_packet) return;

    ctaphid_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    // Common header
    pkt.data[0] = (channel >> 24) & 0xFF;
    pkt.data[1] = (channel >> 16) & 0xFF;
    pkt.data[2] = (channel >> 8) & 0xFF;
    pkt.data[3] = channel & 0xFF;

    // Initialization packet
    pkt.data[4] = cmd; // Command always has bit 7 set (e.g. 0x80 | something, or just the macro which has it set)
    pkt.data[5] = (length >> 8) & 0xFF;
    pkt.data[6] = length & 0xFF;

    uint16_t sent = 0;
    uint16_t chunk = (length > 57) ? 57 : length;
    memcpy(&pkt.data[7], payload, chunk);
    sent += chunk;
    
    send_packet(&pkt);

    uint8_t seq = 0;
    while (sent < length) {
        memset(&pkt.data[4], 0, 60); // Clear payload area
        pkt.data[4] = seq++; // Sequence number (bit 7 is 0)
        
        chunk = (length - sent > 59) ? 59 : (length - sent);
        memcpy(&pkt.data[5], &payload[sent], chunk);
        sent += chunk;
        
        send_packet(&pkt);
    }
}

void ctaphid_send_error(uint32_t channel, uint8_t err_code) {
    if (!send_packet) return;
    
    ctaphid_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    
    pkt.data[0] = (channel >> 24) & 0xFF;
    pkt.data[1] = (channel >> 16) & 0xFF;
    pkt.data[2] = (channel >> 8) & 0xFF;
    pkt.data[3] = channel & 0xFF;
    pkt.data[4] = 0xBF; // CTAPHID_ERROR
    pkt.data[5] = 0x00;
    pkt.data[6] = 0x01; // length is 1
    pkt.data[7] = err_code;
    
    send_packet(&pkt);
}

void ctaphid_send_keepalive(uint32_t channel, uint8_t status) {
    if (!send_packet) return;
    
    ctaphid_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    
    pkt.data[0] = (channel >> 24) & 0xFF;
    pkt.data[1] = (channel >> 16) & 0xFF;
    pkt.data[2] = (channel >> 8) & 0xFF;
    pkt.data[3] = channel & 0xFF;
    pkt.data[4] = CTAPHID_KEEPALIVE;
    pkt.data[5] = 0x00;
    pkt.data[6] = 0x01; // length is 1
    pkt.data[7] = status;
    
    send_packet(&pkt);
}

