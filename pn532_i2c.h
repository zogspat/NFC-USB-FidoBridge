#ifndef PN532_I2C_H
#define PN532_I2C_H

#include "hardware/i2c.h"

// -------------------------------------------------------------
// HARDWARE WIRING CONFIGURATION
// -------------------------------------------------------------
// To make it easy to adapt later, all hardware mappings are defined here.
//
// Your Wiring:
//   - VCC -> Physical Pin 36 (3V3 Out)
//   - GND -> Physical Pin 38 (GND)
//   - SDA -> Physical Pin 6  (GPIO 4 on the Pico)
//   - SCL -> Physical Pin 7  (GPIO 5 on the Pico)
//
// GPIO 4 and 5 are mapped to hardware I2C port 0 on the Pico.

#define PN532_I2C_PORT      i2c0
#define PN532_I2C_SDA_PIN   4
#define PN532_I2C_SCL_PIN   5
#define PN532_I2C_BAUDRATE  400000 // 400kHz fast mode
#define PN532_I2C_ADDRESS   0x24   // 7-bit I2C address for PN532

// Initializes the I2C hardware and configures the pins
void pn532_hw_init(void);

// Send a test command to the PN532 to read its firmware version over I2C
bool pn532_get_firmware_version(void);

// Send an arbitrary command payload to the PN532 and receive its response payload
// Returns true if the command was successfully acknowledged and a valid response frame was received.
bool pn532_send_command(const uint8_t* cmd_data, uint16_t cmd_len, uint8_t* response_data, uint16_t* response_len, uint16_t max_response_len);

// Activates an ISO14443-4 A smartcard (like Cryptnox) in the field
bool pn532_activate_card(void);
void pn532_rf_reset(void);

// Configures the PN532 Security Access Module (SAM) for normal mode operations
bool pn532_sam_configuration(void);

// Exchanges an APDU with the activated smartcard
bool pn532_exchange_apdu(const uint8_t* apdu, uint16_t apdu_len, uint8_t* response, uint16_t* response_len, uint16_t max_response_len);

void debug_log_hex(const char* prefix, const uint8_t* data, uint16_t len);
void debug_log(const char* msg);

#endif // PN532_I2C_H
