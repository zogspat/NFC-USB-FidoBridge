#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "pn532_i2c.h"

// tinyUSB HID callbacks
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) { return 0; }
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {}

void debug_log(const char* msg) {
    if (tud_cdc_connected()) {
        tud_cdc_write(msg, strlen(msg));
        tud_cdc_write("\r\n", 2);
        tud_cdc_write_flush();
        for (int i=0; i<100; i++) tud_task();
    }
}
void debug_log_hex(const char* prefix, const uint8_t* data, uint16_t len) {
    if (!tud_cdc_connected()) return;
    tud_cdc_write(prefix, strlen(prefix));
    char buf[4];
    for(uint16_t i=0; i<len; i++) {
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        tud_cdc_write(buf, strlen(buf));
        if (i % 16 == 15) {
            tud_cdc_write_flush();
            for (int k=0; k<20; k++) tud_task();
        }
    }
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
    for (int i=0; i<100; i++) tud_task();
}

void heartbeat_keepalive(void) {
    tud_task();
}

int main() {
    board_init();
    tusb_init();
    
    while (!tud_cdc_connected()) {
        tud_task();
    }
    
    // Give screen time to connect before blasting messages
    for (int i = 0; i < 100; i++) {
        tud_task();
        sleep_ms(10);
    }
    
    debug_log("=========================================");
    debug_log("CRYPTNOX FIDO2 FACTORY RESET TOOL");
    debug_log("=========================================");
    
    pn532_hw_init();
    pn532_sam_configuration();
    pn532_rf_reset();
    
    debug_log("Waiting for card to be placed on reader...");
    
    uint8_t select_apdu[] = {0x00, 0xA4, 0x04, 0x00, 0x08, 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01, 0x00};
    uint8_t reset_apdu[] = {0x80, 0x10, 0x00, 0x00, 0x01, 0x07, 0x00}; // CTAP Command 0x07 = Reset
    
    uint8_t resp[256];
    uint16_t resp_len;
    
    while (1) {
        tud_task();
        
        if (pn532_activate_card()) {
            bool select_success = true;
            for (int i = 0; i < 3; i++) {
                if (!pn532_exchange_apdu(select_apdu, sizeof(select_apdu), resp, &resp_len, sizeof(resp))) {
                    select_success = false;
                    break;
                }
            }
            
            if (!select_success) {
                pn532_rf_reset();
                sleep_ms(50);
                continue;
            }
            
            debug_log("Card Found! FIDO2 Applet selected stably.");
            debug_log("Sending authenticatorReset (0x07)...");
            
            if (pn532_exchange_apdu(reset_apdu, sizeof(reset_apdu), resp, &resp_len, sizeof(resp))) {
                if (resp_len >= 2 && resp[resp_len-2] == 0x69 && resp[resp_len-1] == 0x85) {
                    debug_log(">>> TOUCH REQUIRED <<<");
                    debug_log("Please lift the card and tap it again NOW to confirm reset!");
                    
                    pn532_rf_reset();
                    sleep_ms(200);
                    
                    uint32_t wait_start = time_us_32();
                    bool retapped = false;
                    while (time_us_32() - wait_start < 15000000) { // 15 seconds to retap
                        tud_task();
                        if (pn532_activate_card()) {
                            bool re_select = true;
                            for (int i = 0; i < 3; i++) {
                                if (!pn532_exchange_apdu(select_apdu, sizeof(select_apdu), resp, &resp_len, sizeof(resp))) {
                                    re_select = false;
                                    break;
                                }
                            }
                            if (re_select) {
                                retapped = true;
                                break;
                            } else {
                                pn532_rf_reset();
                                sleep_ms(50);
                            }
                        }
                    }
                    
                    if (retapped) {
                        debug_log("Retap detected! Sending Reset again...");
                        if (pn532_exchange_apdu(reset_apdu, sizeof(reset_apdu), resp, &resp_len, sizeof(resp))) {
                            if (resp_len >= 2 && resp[resp_len-2] == 0x90 && resp[resp_len-1] == 0x00) {
                                debug_log("FACTORY RESET SUCCESSFUL! All resident keys wiped.");
                            } else {
                                debug_log_hex("Reset failed. Card returned: ", resp, resp_len);
                            }
                        } else {
                            debug_log("Reset command failed on retap!");
                        }
                    } else {
                        debug_log("Timeout waiting for retap.");
                    }
                } else if (resp_len >= 2 && resp[resp_len-2] == 0x90 && resp[resp_len-1] == 0x00) {
                    debug_log("FACTORY RESET SUCCESSFUL! All resident keys wiped.");
                } else {
                    debug_log_hex("Reset failed. Card returned: ", resp, resp_len);
                }
            } else {
                debug_log("Reset command failed (timeout or RF drop)!");
            }
            
            break; // Done
        }
        
        sleep_ms(50);
    }
    
    debug_log("Halting.");
    while (1) {
        tud_task();
    }
    return 0;
}
