#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"
#include "pn532_i2c.h"
#include "ctaphid.h"

uint32_t active_channel = 0;

void heartbeat_keepalive() {
    tud_task();
    static uint32_t last_ka = 0;
    if (active_channel != 0 && (time_us_32() - last_ka > 100000)) {
        ctaphid_send_keepalive(active_channel, 0x01); // 0x01 = PROCESSING
        last_ka = time_us_32();
    }
}

// -------------------------------------------------------------
// Debug Logging
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// USB to CTAPHID Bridging
// -------------------------------------------------------------

// Called by CTAPHID layer when it needs to send a packet back to the Mac
void my_ctaphid_send_cb(const ctaphid_packet_t* packet) {
    uint32_t start = time_us_32();
    // Wait until the USB IN endpoint is ready, but timeout after 500ms if Mac disconnected
    while (!tud_hid_ready()) {
        if (time_us_32() - start > 500000) return;
        tud_task();
    }
    tud_hid_report(0, packet->data, 64);
}

// Called by CTAPHID layer when a full, defragmented message is received from the Mac
static bool g_nfc_session_active = false;

void my_ctaphid_msg_cb(uint32_t channel, uint8_t cmd, const uint8_t* payload, uint16_t length) {
    if (active_channel != 0 && active_channel != channel) {
        ctaphid_send_error(channel, 0x0B); // CTAPHID_ERR_CHANNEL_BUSY
        return;
    }
    active_channel = channel;


    
    if (cmd == CTAPHID_CBOR) {
        char debug_buf[50];
        snprintf(debug_buf, sizeof(debug_buf), "Host sent CTAP request: %02X", payload[0]);
        debug_log(debug_buf);
    }
    if (cmd == CTAPHID_CBOR || cmd == CTAPHID_MSG) {


        uint32_t wait_start = time_us_32();
        uint32_t last_keepalive = time_us_32();
        bool card_found = false;
        
        bool allow_keepalive = (cmd == CTAPHID_CBOR && length > 0 && (
            payload[0] == 0x01 || 
            payload[0] == 0x02 || 
            payload[0] == 0x06 || 
            payload[0] == 0x0B
        ));
        
        uint8_t select_apdu[] = {0x00, 0xA4, 0x04, 0x00, 0x08, 0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01, 0x00};
        uint8_t resp[255];
        uint16_t resp_len = 0;
        
        // --- THIS IS THE FIX: Only activate the card if not already active! ---
        if (!g_nfc_session_active) {
            pn532_rf_reset(); // Safe to reset here since there is no session
            while (time_us_32() - wait_start < 30000000) {
                if (pn532_activate_card()) {
                    bool select_success = true;
                    for (int i = 0; i < 3; i++) {
                        if (!pn532_exchange_apdu(select_apdu, sizeof(select_apdu), resp, &resp_len, sizeof(resp))) {
                            select_success = false;
                            break; 
                        }
                        if (resp_len < 2 || resp[resp_len-2] != 0x90 || resp[resp_len-1] != 0x00) {
                            debug_log_hex("Error: Card rejected SELECT. Response: ", resp, resp_len);
                            ctaphid_send_error(channel, 0x01);
                            active_channel = 0;
                            return;
                        }
                    }
                    if (select_success) {
                        card_found = true;
                        g_nfc_session_active = true;
                        debug_log("Card found and FIDO2 Applet selected stably!");
                        break; 
                    } else {
                        pn532_rf_reset();
                    }
                }
                
                if (allow_keepalive && (time_us_32() - last_keepalive > 100000)) {
                    ctaphid_send_keepalive(channel, 0x02); 
                    last_keepalive = time_us_32();
                    gpio_put(25, !gpio_get(25));
                }
                
                uint32_t pause = time_us_32();
                while(time_us_32() - pause < 50000) tud_task();
                
                if (!allow_keepalive && (time_us_32() - wait_start > 2000000)) {
                    break;
                }
            }
        } else {
            // Assume card is still on the reader, keeping state alive
            card_found = true;
        }
        
        if (!card_found) {
            if (allow_keepalive) {
                debug_log("Error: Card not found after 30 seconds.");
            } else {
                debug_log("Error: Card not found after 2 seconds.");
            }
            ctaphid_send_error(channel, 0x05); // CTAPHID_ERR_TIMEOUT
            active_channel = 0;
            return;
        }
        
        uint16_t total_resp_len = 0;
        uint8_t final_resp[2000];
        
retry_cmd:
        total_resp_len = 0; // Reset length before retrying!
        if (cmd == CTAPHID_CBOR) {
            uint16_t sent = 0;
            while (sent < length) {
                uint16_t chunk = length - sent;
                bool is_last = (chunk <= 200);
                if (!is_last) chunk = 200;
                
                uint8_t apdu[256];
                apdu[0] = is_last ? 0x80 : 0x90; // CLA 
                apdu[1] = 0x10; // INS
                apdu[2] = 0x00; // P1
                apdu[3] = 0x00; // P2
                apdu[4] = chunk; // Lc
                memcpy(&apdu[5], &payload[sent], chunk);
                uint16_t out_len = 5 + chunk;
                if (is_last) apdu[out_len++] = 0x00; // Le
                
                debug_log_hex("Sending APDU chunk: ", apdu, out_len);
                
                uint8_t large_resp[300];
                resp_len = 0;
                if (!pn532_exchange_apdu(apdu, out_len, large_resp, &resp_len, sizeof(large_resp))) {
                    debug_log("Error: Card rejected APDU chunk.");
                    g_nfc_session_active = false; // Mark session dead
                    ctaphid_send_error(channel, 0x7F);
                    active_channel = 0;
                    return;
                }
                
                sent += chunk;
                
                if (is_last) {
                    memcpy(&final_resp[total_resp_len], large_resp, resp_len);
                    total_resp_len += resp_len;
                } else {
                    if (resp_len < 2 || large_resp[resp_len-2] != 0x90 || large_resp[resp_len-1] != 0x00) {
                        debug_log_hex("Error: Card rejected chaining APDU chunk. Response: ", large_resp, resp_len);
                        g_nfc_session_active = false;
                        ctaphid_send_error(channel, 0x7F);
                        active_channel = 0;
                        return;
                    }
                }
            }
        } else if (cmd == CTAPHID_MSG) {
            uint8_t rx_buf[256];
            uint16_t rx_len;
            if (pn532_exchange_apdu(payload, length, rx_buf, &rx_len, sizeof(rx_buf))) {
                ctaphid_send_message(channel, cmd, rx_buf, rx_len);
            } else {
                g_nfc_session_active = false;
                ctaphid_send_error(channel, 0x7F);
            }
            active_channel = 0;
            return;
        }
        
        while (total_resp_len >= 2 && final_resp[total_resp_len-2] == 0x61) {
            uint8_t next_len = final_resp[total_resp_len-1];
            total_resp_len -= 2; 
            
            uint8_t get_resp_apdu[] = {0x00, 0xC0, 0x00, 0x00, next_len};
            debug_log("Fetching GET RESPONSE chunk...");
            
            uint8_t large_resp[300];
            resp_len = 0;
            if (!pn532_exchange_apdu(get_resp_apdu, sizeof(get_resp_apdu), large_resp, &resp_len, sizeof(large_resp))) {
                debug_log("Error: Card failed GET RESPONSE.");
                g_nfc_session_active = false;
                ctaphid_send_error(channel, 0x7F);
                active_channel = 0;
                return;
            }
            
            memcpy(&final_resp[total_resp_len], large_resp, resp_len);
            total_resp_len += resp_len;
        }
        
        if (total_resp_len >= 2 && final_resp[total_resp_len-2] == 0x69 && final_resp[total_resp_len-1] == 0x85) {
            debug_log("Card requires UP (69 85). Lift and re-tap card...");
            
            uint32_t retap_start = time_us_32();
            uint32_t last_ka = time_us_32();
            bool retapped = false;
            
            g_nfc_session_active = false;
            pn532_rf_reset();
            uint32_t p = time_us_32();
            while(time_us_32() - p < 100000) tud_task(); 
            
            while (time_us_32() - retap_start < 30000000) {
                if (pn532_activate_card()) {
                    retapped = true;
                    break;
                }
                if (time_us_32() - last_ka > 100000) {
                    ctaphid_send_keepalive(channel, 0x02); 
                    last_ka = time_us_32();
                }
                p = time_us_32();
                while(time_us_32() - p < 50000) tud_task();
            }
            
            if (!retapped) {
                debug_log("Timeout waiting for re-tap.");
                ctaphid_send_error(channel, 0x05); 
                active_channel = 0;
                return;
            }
            
            uint8_t large_resp[300];
            resp_len = 0;
            if (!pn532_exchange_apdu(select_apdu, sizeof(select_apdu), large_resp, &resp_len, sizeof(large_resp)) ||
                resp_len < 2 || large_resp[resp_len-2] != 0x90 || large_resp[resp_len-1] != 0x00) {
                ctaphid_send_error(channel, 0x7F);
                active_channel = 0;
                return;
            }
            g_nfc_session_active = true;
            // Re-send the command now that UP is satisfied!
            goto retry_cmd;
        }
        
        debug_log_hex("Final Card Response: ", final_resp, total_resp_len > 255 ? 255 : total_resp_len);
        
        if (total_resp_len >= 2) {
            if (final_resp[total_resp_len-2] == 0x90 && final_resp[total_resp_len-1] == 0x00) {
                if (cmd == CTAPHID_CBOR) {
                    if (total_resp_len > 2 && payload[0] == 0x02) {
                        bool up_false = false;
                        for (uint16_t i = 1; i < length - 3; i++) {
                            if (payload[i] == 0x62 && payload[i+1] == 0x75 && payload[i+2] == 0x70 && payload[i+3] == 0xF4) {
                                up_false = true;
                                break;
                            }
                        }
                        // Only intercept if the card actually returned a successful FIDO2 payload (starts with 0x00)
                        if (up_false && final_resp[0] == 0x00) {
                            debug_log("Intercepted SUCCESS response for up=false GetAssertion. Card illegally set UP=true. Returning 0x2B (UP_REQUIRED) instead!");
                            uint8_t err = 0x2B;
                            ctaphid_send_message(channel, cmd, &err, 1);
                            active_channel = 0;
                            return;
                        }
                    }
                    if (total_resp_len > 2 && payload[0] == 0x04) {
                        debug_log("Intercepted GetInfo response. Searching for options map...");
                        
                        bool patched = false;
                        for (uint16_t i = 1; i < total_resp_len - 2; i++) {
                            if (final_resp[i] == 0x6E && final_resp[i+1] == 0x70 && final_resp[i+2] == 0x69 && final_resp[i+3] == 0x6E && final_resp[i+4] == 0x55 && final_resp[i+5] == 0x76 && final_resp[i+6] == 0x41 && final_resp[i+7] == 0x75 && final_resp[i+8] == 0x74 && final_resp[i+9] == 0x68 && final_resp[i+10] == 0x50 && final_resp[i+11] == 0x72 && final_resp[i+12] == 0x6F && final_resp[i+13] == 0x74 && final_resp[i+14] == 0x6F && final_resp[i+15] == 0x63 && final_resp[i+16] == 0x6F && final_resp[i+17] == 0x6C && final_resp[i+18] == 0x73) {
                                if (i+19 < total_resp_len - 2 && final_resp[i+19] == 0x82 && final_resp[i+20] == 0x01 && final_resp[i+21] == 0x02) {
                                    final_resp[i+21] = 0x01;
                                    debug_log("Dynamic RAM Patch: Downgraded pinUvAuthProtocols to [1, 1]!");
                                    patched = true;
                                }
                            }
                            
                            if (final_resp[i] == 0x06 && final_resp[i+1] == 0x82 && final_resp[i+2] == 0x01 && final_resp[i+3] == 0x02) {
                                final_resp[i+3] = 0x01;
                                debug_log("Dynamic RAM Patch: Downgraded pinUvAuthProtocols to [1, 1]!");
                                patched = true;
                            }
                            
                            if (final_resp[i] == 0x09 && final_resp[i+1] == 0x81 && final_resp[i+2] == 0x63 && final_resp[i+3] == 0x6E && final_resp[i+4] == 0x66 && final_resp[i+5] == 0x63) {
                                final_resp[i+3] = 0x75; // 'u'
                                final_resp[i+4] = 0x73; // 's'
                                final_resp[i+5] = 0x62; // 'b'
                                debug_log("Dynamic RAM Patch: Replaced 'nfc' with 'usb' in GetInfo response!");
                                patched = true;
                            }
                        }
                    }
                    
                    // final_resp already contains the status byte (0x00) at index 0!
                    ctaphid_send_message(channel, cmd, final_resp, total_resp_len - 2);
                }
            } else {
                if (cmd == CTAPHID_CBOR) {
                    uint8_t err = final_resp[0]; 
                    if (total_resp_len == 2) err = final_resp[0]; 
                    ctaphid_send_message(channel, cmd, &err, 1);
                }
            }
        } else {
            ctaphid_send_error(channel, 0x7F);
        }
    } else if (cmd == 0x81) { // CTAPHID_PING
        ctaphid_send_message(channel, cmd, payload, length);
    } else {
        ctaphid_send_error(channel, 0x01); // CTAPHID_ERR_INVALID_CMD
    }
    
    active_channel = 0;
}

#include "bsp/board.h"

int main() {
    board_init(); // REQUIRED FOR TINYUSB ON RP2040!
    
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);
    
    pn532_hw_init();
    uint32_t t = time_us_32();
    while(time_us_32() - t < 500000); 
    pn532_sam_configuration();
    
    tusb_init();
    
    // Try to init ctaphid if it was there
    ctaphid_init(my_ctaphid_send_cb, my_ctaphid_msg_cb);
    
    while (1) {
        tud_task();
    }
    return 0;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    if (bufsize >= 64) {
        ctaphid_handle_packet(buffer);
    }
}
