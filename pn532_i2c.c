#include "pn532_i2c.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include "tusb.h"

// Helper to wait without blocking TinyUSB
static void pn532_wait_ms(uint32_t ms) {
    uint32_t start = time_us_32();
    while (time_us_32() - start < ms * 1000) {
        tud_task();
    }
}

void pn532_hw_init(void) {
    // 1. Initialize the I2C hardware block at our defined baud rate (400kHz)
    i2c_init(PN532_I2C_PORT, PN532_I2C_BAUDRATE);

    // 2. Map the physical GPIO pins to the I2C peripheral
    gpio_set_function(PN532_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PN532_I2C_SCL_PIN, GPIO_FUNC_I2C);

    // 3. Enable the internal pull-up resistors. 
    // (Note: Many PN532 breakout boards have their own physical pull-up resistors, 
    // but enabling the Pico's internal ones guarantees the bus floats high if they don't).
    gpio_pull_up(PN532_I2C_SDA_PIN);
    gpio_pull_up(PN532_I2C_SCL_PIN);
}

// Check if PN532 is ready (RDY status byte == 0x01)
static bool pn532_is_ready(void) {
    uint8_t status;
    int ret = i2c_read_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, &status, 1, false);
    return (ret == 1) && (status == 0x01);
}

bool pn532_get_firmware_version(void) {
    // 1. Wakeup sequence: For I2C, usually just sending the command is enough 
    // if it's not in deep sleep, but some breakout boards require dummy writes.
    // We'll skip dummy writes for the basic test.

    // 2. Command: GetFirmwareVersion (0x02)
    // Frame: 00 00 FF 02 FE D4 02 2A 00
    uint8_t cmd_frame[] = {
        0x00, 0x00, 0xFF, // Preamble + Start Code
        0x02, 0xFE,       // Length (2), Length Checksum (~2 + 1 = 0xFE)
        0xD4, 0x02,       // TFI (Host to PN532 = D4), Command (GetFirmwareVersion = 02)
        0x2A,             // Data Checksum (~(D4+02) + 1 = 2A)
        0x00              // Postamble
    };

    // Send command
    printf("Sending GetFirmwareVersion to PN532...\n");
    int ret = i2c_write_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, cmd_frame, sizeof(cmd_frame), false);
    if (ret != sizeof(cmd_frame)) {
        printf("I2C Write failed! Returned %d\n", ret);
        return false;
    }

    // Wait for RDY flag
    pn532_wait_ms(10);
    int retries = 0;
    while (!pn532_is_ready()) {
        pn532_wait_ms(10);
        retries++;
        if (retries > 50) { // 500ms timeout
            printf("PN532 timed out waiting for RDY.\n");
            return false;
        }
    }

    // Read the ACK frame + Response frame
    // ACK is 6 bytes: 00 00 FF 00 FF 00
    // Firmware response is 12 bytes + 1 byte for RDY status = 19 bytes total
    uint8_t response[25];
    ret = i2c_read_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, response, 20, false);
    
    if (ret < 20) {
        printf("Failed to read response, got %d bytes\n", ret);
        return false;
    }

    // Print raw response
    printf("PN532 Response: ");
    for(int i=0; i<20; i++) printf("%02X ", response[i]);
    printf("\n");

    return true;
}

bool pn532_send_command(const uint8_t* cmd_data, uint16_t cmd_len, uint8_t* response_data, uint16_t* response_len, uint16_t max_response_len) {
    if (cmd_len > 254) return false; 
    
    uint8_t frame[265];
    uint8_t frame_len = 0;
    
    // Preamble + Start Code
    frame[frame_len++] = 0x00;
    frame[frame_len++] = 0x00;
    frame[frame_len++] = 0xFF;
    
    // Length & LCS
    uint8_t len = cmd_len + 1; // +1 for TFI byte
    frame[frame_len++] = len;
    frame[frame_len++] = (uint8_t)(~len + 1);
    
    // TFI + Payload
    frame[frame_len++] = 0xD4; // Host to PN532
    uint8_t sum = 0xD4;
    for (uint16_t i = 0; i < cmd_len; i++) {
        frame[frame_len++] = cmd_data[i];
        sum += cmd_data[i];
    }
    
    // DCS & Postamble
    frame[frame_len++] = (uint8_t)(~sum + 1);
    frame[frame_len++] = 0x00;
    
    // Write frame
    // debug_log_hex("PN532 TX: ", frame, frame_len);
    if (i2c_write_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, frame, frame_len, false) != frame_len) {
        debug_log("PN532 I2C TX Failed.");
        return false;
    }
    
    // Wait for RDY (ACK)
    uint32_t start_time = time_us_32();
    while (!pn532_is_ready()) {
        if (time_us_32() - start_time > 100000) {
            debug_log("PN532 ACK Timeout.");
            return false; // 100ms timeout
        }
        pn532_wait_ms(1);
    }
    
    // Read ACK frame. Every I2C read prepends 0x01.
    // ACK is 6 bytes: 00 00 FF 00 FF 00, so we read 7 bytes.
    uint8_t ack[7];
    if (i2c_read_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, ack, 7, false) != 7) {
        debug_log("PN532 ACK Read Failed.");
        return false;
    }
    // debug_log_hex("PN532 ACK RX: ", ack, 7);
    
extern void heartbeat_keepalive(void);

// Wait for RDY (Response)
    start_time = time_us_32();
    while (!pn532_is_ready()) {
        if (time_us_32() - start_time > 10000000) { // 10s timeout for crypto operations
            debug_log("PN532 Response Timeout.");
            return false;
        }
        heartbeat_keepalive();
        pn532_wait_ms(1);
    }
    
    // Read the ENTIRE maximum possible Response Frame in a single transaction
    // This absolutely guarantees the PN532 won't abort due to premature STOP or RESTART conditions.
    uint8_t full_frame[300];
    if (i2c_read_blocking(PN532_I2C_PORT, PN532_I2C_ADDRESS, full_frame, 300, false) != 300) {
        debug_log("PN532 Full Frame Read Failed.");
        return false;
    }
    
    // Check if the frame is valid
    if (full_frame[0] != 0x01 || full_frame[1] != 0x00 || full_frame[2] != 0x00 || full_frame[3] != 0xFF) {
        debug_log("PN532 Invalid Frame Header.");
        return false;
    }
    
    uint16_t res_len = 0;
    uint16_t payload_start = 0;
    
    if (full_frame[4] == 0xFF && full_frame[5] == 0xFF) {
        // Extended Frame (response > 255 bytes)
        res_len = (full_frame[6] << 8) | full_frame[7];
        payload_start = 10;
    } else {
        // Normal Frame
        res_len = full_frame[4];
        payload_start = 7;
    }
    
    if (res_len > max_response_len + 1) {
        debug_log("PN532 Response too large.");
        return false;
    }
    
    *response_len = res_len - 1; // subtract TFI
    memcpy(response_data, &full_frame[payload_start], *response_len);
    
    return true;
}

bool pn532_activate_card(void) {
    // 0x4A = InListPassiveTarget
    // 0x01 = Max 1 target
    // 0x00 = 106 kbps type A (ISO14443 Type A)
    uint8_t cmd[] = {0x4A, 0x01, 0x00};
    uint8_t resp[64];
    uint16_t resp_len = 0;
    
    if (!pn532_send_command(cmd, sizeof(cmd), resp, &resp_len, sizeof(resp))) {
        return false;
    }
    
    // Response should be: 
    // resp[0] = Command Code (0x4B)
    // resp[1] = Number of targets found (NbTg)
    if (resp_len >= 2 && resp[0] == 0x4B && resp[1] > 0) {
        return true; // Card detected and activated (Target ID 1)
    }
    return false;
}

bool pn532_sam_configuration(void) {
    // 0x14 = SAMConfiguration
    // 0x01 = Normal mode
    // 0x14 = Timeout 50ms
    // 0x01 = Use IRQ pin
    uint8_t cmd_sam[] = {0x14, 0x01, 0x14, 0x01};
    uint8_t resp[16];
    uint16_t resp_len = 0;
    
    if (!pn532_send_command(cmd_sam, sizeof(cmd_sam), resp, &resp_len, sizeof(resp))) {
        return false;
    }
    
    if (resp_len < 1 || resp[0] != 0x15) return false;

    // Configure RF Retries so InListPassiveTarget doesn't hang infinitely
    uint8_t cmd_rf[] = {0x32, 0x05, 0xFF, 0x01, 0x02};
    if (!pn532_send_command(cmd_rf, sizeof(cmd_rf), resp, &resp_len, sizeof(resp))) {
        return false;
    }

    return true;
}

void pn532_rf_reset(void) {
    // RFConfiguration: RF Field off
    uint8_t cmd_off[] = {0x32, 0x01, 0x00};
    uint8_t resp[16];
    uint16_t resp_len = 0;
    pn532_send_command(cmd_off, sizeof(cmd_off), resp, &resp_len, sizeof(resp));
    
    pn532_wait_ms(50);
    
    // RFConfiguration: RF Field on (Auto RF CA = 0, RF On = 1)
    uint8_t cmd_on[] = {0x32, 0x01, 0x02};
    pn532_send_command(cmd_on, sizeof(cmd_on), resp, &resp_len, sizeof(resp));
    
    pn532_wait_ms(50);
}

bool pn532_exchange_apdu(const uint8_t* apdu, uint16_t apdu_len, uint8_t* response, uint16_t* response_len, uint16_t max_response_len) {
    // 0x40 = InDataExchange
    // 0x01 = Target ID 1
    if (apdu_len > 250) return false; // Hardware max per frame limit
    
    uint8_t cmd[255];
    cmd[0] = 0x40; // InDataExchange
    cmd[1] = 0x01; // Target number 1
    memcpy(&cmd[2], apdu, apdu_len);
    
    uint8_t resp[300];
    uint16_t resp_len = 0;
    
    if (!pn532_send_command(cmd, apdu_len + 2, resp, &resp_len, sizeof(resp))) {
        return false;
    }
    
    // Response should be:
    // resp[0] = Command Code (0x41)
    // resp[1] = Status byte (0x00 = success)
    if (resp_len >= 2 && resp[0] == 0x41 && resp[1] == 0x00) {
        *response_len = resp_len - 2;
        if (*response_len > max_response_len) return false;
        
        memcpy(response, &resp[2], *response_len);
        return true;
    }
    
    debug_log_hex("PN532 InDataExchange Error Status: ", resp, resp_len);
    return false;
}

