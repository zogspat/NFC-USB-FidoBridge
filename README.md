If you happen to have stumbled onto this via a search engine: as this is security sensitive, use it at your peril! 
This is a working solution to build the firmware to facilitate passkey authentication for Safari on a Mac -> a Raspberry Pi Pico -> PN532 NFC reader -> Cryptnox FIDO2 smartcard. 

It is very difficult to have a generalised solution that works with all FIDO 2 capable sites. This implementation works with sites such as eBay and Github.

It is also very much tied to some features of the Cryptnox card. For instance, as it didn't appear feasible to get the PIN protocol 2 working, there is a workaround that locates the pinUvAuthProtocols key (0x06) in the GetInfo response and dynamically downgrades the supported array from [1, 2] to [1, 1]. This forces Safari to use Protocol 1

This is intended as my backup only. 
