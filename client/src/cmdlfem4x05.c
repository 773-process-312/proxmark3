//-----------------------------------------------------------------------------
// Copyright (C) 2010 iZsh <izsh at fail0verflow.com>
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Low frequency EM4x commands
//-----------------------------------------------------------------------------

#include "cmdlfem4x05.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdlib.h>

#include "util_posix.h"  // msclock
#include "fileutils.h"
#include "cmdparser.h"    // command_t
#include "comms.h"
#include "commonutil.h"
#include "common.h"
#include "util_posix.h"
#include "protocols.h"
#include "ui.h"
#include "proxgui.h"
#include "graph.h"
#include "cmddata.h"
#include "cmdlf.h"
#include "lfdemod.h"
#include "generator.h"
#include "cliparser.h"
#include "cmdhw.h"

//////////////// 4205 / 4305 commands

static int usage_lf_em4x05_wipe(void) {
    PrintAndLogEx(NORMAL, "Wipe EM4x05/EM4x69.  Tag must be on antenna. ");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  lf em 4x05_wipe [h] <pwd>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h     - this help");
    PrintAndLogEx(NORMAL, "       c     - chip type : 0 em4205");
    PrintAndLogEx(NORMAL, "                           1 em4305 (default)");
    PrintAndLogEx(NORMAL, "       pwd   - password (hex) (optional)");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, "      lf em 4x05_wipe");
    PrintAndLogEx(NORMAL, "      lf em 4x05_wipe 11223344");
    return PM3_SUCCESS;
}
static int usage_lf_em4x05_read(void) {
    PrintAndLogEx(NORMAL, "Read EM4x05/EM4x69.  Tag must be on antenna. ");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  lf em 4x05_read [h] <address> <pwd>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h         - this help");
    PrintAndLogEx(NORMAL, "       address   - memory address to read. (0-15)");
    PrintAndLogEx(NORMAL, "       pwd       - password (hex) (optional)");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, "      lf em 4x05_read 1");
    PrintAndLogEx(NORMAL, "      lf em 4x05_read 1 11223344");
    return PM3_SUCCESS;
}
static int usage_lf_em4x05_write(void) {
    PrintAndLogEx(NORMAL, "Write EM4x05/4x69.  Tag must be on antenna. ");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  lf em 4x05_write [h] <address> <data> <pwd>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h         - this help");
    PrintAndLogEx(NORMAL, "       address   - memory address to write to. (0-13, 99 for Protection Words)");
    PrintAndLogEx(NORMAL, "       data      - data to write (hex)");
    PrintAndLogEx(NORMAL, "       pwd       - password (hex) (optional)");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, "      lf em 4x05_write 1 deadc0de");
    PrintAndLogEx(NORMAL, "      lf em 4x05_write 1 deadc0de 11223344");
    return PM3_SUCCESS;
}
static int usage_lf_em4x05_info(void) {
    PrintAndLogEx(NORMAL, "Tag information EM4205/4305/4469//4569 tags.  Tag must be on antenna.");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(NORMAL, "Usage:  lf em 4x05_info [h] <pwd>");
    PrintAndLogEx(NORMAL, "Options:");
    PrintAndLogEx(NORMAL, "       h         - this help");
    PrintAndLogEx(NORMAL, "       pwd       - password (hex) (optional)");
    PrintAndLogEx(NORMAL, "Examples:");
    PrintAndLogEx(NORMAL, "      lf em 4x05_info");
    PrintAndLogEx(NORMAL, "      lf em 4x05_info deadc0de");
    return PM3_SUCCESS;
}

#define EM_SERIAL_BLOCK 1
#define EM_CONFIG_BLOCK 4
#define EM4305_PROT1_BLOCK 14
#define EM4305_PROT2_BLOCK 15
#define EM4469_PROT_BLOCK 3

typedef enum {
    EM_UNKNOWN,
    EM_4205,
    EM_4305,
    EM_4X69,
} em_tech_type_t;

// 1 = EM4x69
// 2 = EM4x05
static em_tech_type_t em_get_card_type(uint32_t config) {
    uint8_t t = (config >> 1) & 0xF;
    switch (t) {
        case 4:
            return EM_4X69;
        case 8:
            return EM_4205;
        case 9:
            return EM_4305;
    }
    return EM_UNKNOWN;
}

static const char *em_get_card_str(uint32_t config) {
    switch (em_get_card_type(config)) {
        case EM_4305:
            return "EM4305";
        case EM_4X69:
            return "EM4469";
        case EM_4205:
            return "EM4205";
        case EM_UNKNOWN:
            break;
    }
    return "Unknown";
}

// even parity COLUMN
static bool EM_ColParityTest(uint8_t *bs, size_t size, uint8_t rows, uint8_t cols, uint8_t pType) {
    if (rows * cols > size) return false;
    uint8_t colP = 0;

    for (uint8_t c = 0; c < cols - 1; c++) {
        for (uint8_t r = 0; r < rows; r++) {
            colP ^= bs[(r * cols) + c];
        }
        if (colP != pType) return false;
        colP = 0;
    }
    return true;
}

#define EM_PREAMBLE_LEN 8
// download samples from device and copy to Graphbuffer
static bool downloadSamplesEM(void) {

    // 8 bit preamble + 32 bit word response (max clock (128) * 40bits = 5120 samples)
    uint8_t got[6000];
    if (!GetFromDevice(BIG_BUF, got, sizeof(got), 0, NULL, 0, NULL, 2500, false)) {
        PrintAndLogEx(WARNING, "(downloadSamplesEM) command execution time out");
        return false;
    }

    setGraphBuf(got, sizeof(got));
    // set signal properties low/high/mean/amplitude and is_noise detection
    computeSignalProperties(got, sizeof(got));
    RepaintGraphWindow();
    if (getSignalProperties()->isnoise) {
        PrintAndLogEx(DEBUG, "No tag found - signal looks like noise");
        return false;
    }
    return true;
}

// em_demod
static int doPreambleSearch(size_t *startIdx) {

    // sanity check
    if (DemodBufferLen < EM_PREAMBLE_LEN) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM4305 demodbuffer too small");
        return PM3_ESOFT;
    }

    // set size to 11 to only test first 3 positions for the preamble
    // do not set it too long else an error preamble followed by 010 could be seen as success.
    size_t size = (11 > DemodBufferLen) ? DemodBufferLen : 11;
    *startIdx = 0;
    // skip first two 0 bits as they might have been missed in the demod
    uint8_t preamble[EM_PREAMBLE_LEN] = {0, 0, 0, 0, 1, 0, 1, 0};

    if (!preambleSearchEx(DemodBuffer, preamble, EM_PREAMBLE_LEN, &size, startIdx, true)) {
        uint8_t errpreamble[EM_PREAMBLE_LEN] = {0, 0, 0, 0, 0, 0, 0, 1};
        if (!preambleSearchEx(DemodBuffer, errpreamble, EM_PREAMBLE_LEN, &size, startIdx, true)) {
            PrintAndLogEx(DEBUG, "DEBUG: Error - EM4305 preamble not found :: %zu", *startIdx);
            return PM3_ESOFT;
        }
        return PM3_EFAILED; // Error preamble found
    }
    return PM3_SUCCESS;
}

static bool detectFSK(void) {
    // detect fsk clock
    if (GetFskClock("", false) == 0) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: FSK clock failed");
        return false;
    }
    // demod
    int ans = FSKrawDemod(0, 0, 0, 0, false);
    if (ans != PM3_SUCCESS) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: FSK Demod failed");
        return false;
    }
    return true;
}
// PSK clocks should be easy to detect ( but difficult to demod a non-repeating pattern... )
static bool detectPSK(void) {
    int ans = GetPskClock("", false);
    if (ans <= 0) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: PSK clock failed");
        return false;
    }
    //demod
    //try psk1 -- 0 0 6 (six errors?!?)
    ans = PSKDemod(0, 0, 6, false);
    if (ans != PM3_SUCCESS) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: PSK1 Demod failed");

        //try psk1 inverted
        ans = PSKDemod(0, 1, 6, false);
        if (ans != PM3_SUCCESS) {
            PrintAndLogEx(DEBUG, "DEBUG: Error - EM: PSK1 inverted Demod failed");
            return false;
        }
    }
    // either PSK1 or PSK1 inverted is ok from here.
    // lets check PSK2 later.
    return true;
}
// try manchester - NOTE: ST only applies to T55x7 tags.
static bool detectASK_MAN(void) {
    bool stcheck = false;
    if (ASKDemod_ext(0, 0, 50, 0, false, false, false, 1, &stcheck) != PM3_SUCCESS) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: ASK/Manchester Demod failed");
        return false;
    }
    return true;
}

static bool detectASK_BI(void) {
    int ans = ASKbiphaseDemod(0, 0, 1, 50, false);
    if (ans != PM3_SUCCESS) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: ASK/biphase normal demod failed");

        ans = ASKbiphaseDemod(0, 1, 1, 50, false);
        if (ans != PM3_SUCCESS) {
            PrintAndLogEx(DEBUG, "DEBUG: Error - EM: ASK/biphase inverted demod failed");
            return false;
        }
    }
    return true;
}
static bool detectNRZ(void) {
    int ans = NRZrawDemod(0, 0, 1, false);
    if (ans != PM3_SUCCESS) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM: NRZ normal demod failed");

        ans = NRZrawDemod(0, 1, 1, false);
        if (ans != PM3_SUCCESS) {
            PrintAndLogEx(DEBUG, "DEBUG: Error - EM: NRZ inverted demod failed");
            return false;
        }
    }

    return true;
}

// param: idx - start index in demoded data.
static int setDemodBufferEM(uint32_t *word, size_t idx) {

    //test for even parity bits.
    uint8_t parity[45] = {0};
    memcpy(parity, DemodBuffer, 45);
    if (!EM_ColParityTest(DemodBuffer + idx + EM_PREAMBLE_LEN, 45, 5, 9, 0)) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - End Parity check failed");
        return PM3_ESOFT;
    }

    // test for even parity bits and remove them. (leave out the end row of parities so 36 bits)
    if (!removeParity(DemodBuffer, idx + EM_PREAMBLE_LEN, 9, 0, 36)) {
        PrintAndLogEx(DEBUG, "DEBUG: Error - EM, failed removing parity");
        return PM3_ESOFT;
    }
    setDemodBuff(DemodBuffer, 32, 0);
    *word = bytebits_to_byteLSBF(DemodBuffer, 32);
    return PM3_SUCCESS;
}

// FSK, PSK, ASK/MANCHESTER, ASK/BIPHASE, ASK/DIPHASE, NRZ
// should cover 90% of known used configs
// the rest will need to be manually demoded for now...
static int demodEM4x05resp(uint32_t *word, bool onlyPreamble) {
    size_t idx = 0;
    *word = 0;
    bool found_err = false;
    int res = PM3_SUCCESS;
    do {
        if (detectASK_MAN()) {
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                // go on, maybe it's false positive and another modulation will work
                found_err = true;
        }
        if (detectASK_BI()) {
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                found_err = true;
        }
        if (detectNRZ()) {
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                found_err = true;
        }
        if (detectFSK()) {
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                found_err = true;
        }
        if (detectPSK()) {
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                found_err = true;

            psk1TOpsk2(DemodBuffer, DemodBufferLen);
            res = doPreambleSearch(&idx);
            if (res == PM3_SUCCESS)
                break;
            if (res == PM3_EFAILED)
                found_err = true;
        }
        if (found_err)
            return PM3_EFAILED;
        return PM3_ESOFT;
    } while (0);
    if (onlyPreamble)
        return PM3_SUCCESS;
    res = setDemodBufferEM(word, idx);
    if (res == PM3_SUCCESS)
        return res;
    if (found_err)
        return PM3_EFAILED;
    return res;
}

//////////////// 4205 / 4305 commands

static int EM4x05Login_ext(uint32_t pwd) {

    struct {
        uint32_t password;
    } PACKED payload;

    payload.password = pwd;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_EM4X_LOGIN, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_LF_EM4X_LOGIN, &resp, 10000)) {
        PrintAndLogEx(WARNING, "(EM4x05Login_ext) timeout while waiting for reply.");
        return PM3_ETIMEOUT;
    }

    if (downloadSamplesEM() == false) {
        return PM3_ESOFT;
    }
    uint32_t word;
    return demodEM4x05resp(&word, true);
}

int EM4x05ReadWord_ext(uint8_t addr, uint32_t pwd, bool usePwd, uint32_t *word) {

    struct {
        uint32_t password;
        uint8_t address;
        uint8_t usepwd;
    } PACKED payload;

    payload.password = pwd;
    payload.address = addr;
    payload.usepwd = usePwd;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_EM4X_READWORD, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_LF_EM4X_READWORD, &resp, 10000)) {
        PrintAndLogEx(WARNING, "(EM4x05ReadWord_ext) timeout while waiting for reply.");
        return PM3_ETIMEOUT;
    }

    if (downloadSamplesEM() == false) {
        return PM3_ESOFT;
    }
    return demodEM4x05resp(word, false);
}

int CmdEM4x05Demod(const char *Cmd) {
//    uint8_t ctmp = tolower(param_getchar(Cmd, 0));
//   if (ctmp == 'h') return usage_lf_em4x05_demod();
    uint32_t dummy = 0;
    return demodEM4x05resp(&dummy, false);
}

int CmdEM4x05Dump(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf em dump",
                  "Dump EM4x05/EM4x69.  Tag must be on antenna.",
                  "lf em dump\n"
                  "lf em dump -p 0x11223344\n"
                  "lf em dump -f myfile -p 0x11223344"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_u64_0("p", "pwd", "<hex>", "password (0x00000000)"),
        arg_str0("f", "file", "<filename>", "override filename prefix (optional).  Default is based on UID"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    uint64_t inputpwd = arg_get_u64_def(ctx, 1, 0xFFFFFFFFFFFFFFFF);
    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 2), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    CLIParserFree(ctx);

    uint8_t addr = 0;
    uint32_t pwd = 0;
    bool usePwd = false;
    if (inputpwd != 0xFFFFFFFFFFFFFFFF) {

        if (inputpwd & 0xFFFFFFFF00000000) {
            PrintAndLogEx(FAILED, "Pwd too large");
            return PM3_EINVARG;
        }

        usePwd = true;
        pwd = (inputpwd & 0xFFFFFFFF);
    }

    uint32_t block0 = 0;
    // read word 0 (chip info)
    // block 0 can be read even without a password.
    if (EM4x05IsBlock0(&block0) == false)
        return PM3_ESOFT;

    uint8_t bytes[4] = {0};
    uint32_t data[16];

    int success = PM3_SUCCESS;
    int status, status14, status15;
    uint32_t lock_bits = 0x00; // no blocks locked
    bool gotLockBits = false;
    bool lockInPW2 = false;
    uint32_t word = 0;

    const char *info[] = {"Info/User", "UID", "Password", "User", "Config", "User", "User", "User", "User", "User", "User", "User", "User", "User", "Lock", "Lock"};
    const char *info4x69 [] = {"Info", "UID", "Password", "Config", "User", "User", "User", "User", "User", "User", "User", "User", "User", "User", "User", "User"};

    // EM4305 vs EM4469
    em_tech_type_t card_type = em_get_card_type(block0);

    PrintAndLogEx(INFO, "Found a " _GREEN_("%s") " tag", em_get_card_str(block0));

    if (usePwd) {
        // Test first if the password is correct
        status = EM4x05Login_ext(pwd);
        if (status == PM3_SUCCESS) {
            PrintAndLogEx(INFO, "Password is " _GREEN_("correct"));
        } else if (status == PM3_EFAILED) {
            PrintAndLogEx(WARNING, "Password is " _RED_("incorrect") ", will try without password");
            usePwd = false;
        } else if (status != PM3_EFAILED) {
            PrintAndLogEx(WARNING, "Login attempt: No answer from tag");
            return status;
        }
    }

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "Addr | data     | ascii |lck| info");
    PrintAndLogEx(INFO, "-----+----------+-------+---+-----");

    if (card_type == EM_4205 || card_type == EM_4305 || card_type == EM_UNKNOWN) {


        // To flag any blocks locked we need to read blocks 14 and 15 first
        // dont swap endin until we get block lock flags.
        status14 = EM4x05ReadWord_ext(EM4305_PROT1_BLOCK, pwd, usePwd, &word);
        if (status14 == PM3_SUCCESS) {
            if ((word & 0x00008000) != 0x00) {
                lock_bits = word;
                gotLockBits = true;
            }
            data[EM4305_PROT1_BLOCK] = word;
        } else {
            success = PM3_ESOFT; // If any error ensure fail is set so not to save invalid data
        }
        status15 = EM4x05ReadWord_ext(EM4305_PROT2_BLOCK, pwd, usePwd, &word);
        if (status15 == PM3_SUCCESS) {
            if ((word & 0x00008000) != 0x00) { // assume block 15 is the current lock block
                lock_bits = word;
                gotLockBits = true;
                lockInPW2 = true;
            }
            data[EM4305_PROT2_BLOCK] = word;
        } else {
            success = PM3_ESOFT; // If any error ensure fail is set so not to save invalid data
        }
        uint32_t lockbit;
        // Now read blocks 0 - 13 as we have 14 and 15
        for (; addr < 14; addr++) {
            lockbit = (lock_bits >> addr) & 1;
            if (addr == 2) {
                if (usePwd) {
                    data[addr] = BSWAP_32(pwd);
                    num_to_bytes(pwd, 4, bytes);
                    PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %s", addr, pwd, sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info[addr]);
                } else {
                    data[addr] = 0x00; // Unknown password, but not used to set to zeros
                    PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s " _YELLOW_("write only"), addr, info[addr]);
                }
            } else {
                // success &= EM4x05ReadWord_ext(addr, pwd, usePwd, &word);
                status = EM4x05ReadWord_ext(addr, pwd, usePwd, &word); // Get status for single read
                if (status != PM3_SUCCESS)
                    success = PM3_ESOFT; // If any error ensure fail is set so not to save invalid data
                data[addr] = BSWAP_32(word);
                if (status == PM3_SUCCESS) {
                    num_to_bytes(word, 4, bytes);
                    PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %s", addr, word, sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info[addr]);
                } else
                    PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s %s", addr, info[addr], status == PM3_EFAILED ? _RED_("read denied") : _RED_("read failed"));
            }
        }
        // Print blocks 14 and 15
        // Both lock bits are protected with bit idx 14 (special case)
        addr = 14;
        if (status14 == PM3_SUCCESS) {
            lockbit = (lock_bits >> addr) & 1;
            PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %-10s %s", addr, data[addr], sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info[addr], lockInPW2 ? "" : _GREEN_("active"));
        } else {
            PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s %s", addr, info[addr], status14 == PM3_EFAILED ? _RED_("read denied") : _RED_("read failed"));
        }
        addr = 15;
        if (status15 == PM3_SUCCESS) {
            lockbit = (lock_bits >> 14) & 1; // beware lock bit of word15 is pr14
            PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %-10s %s", addr, data[addr], sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info[addr], lockInPW2 ? _GREEN_("active") : "");
        } else {
            PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s %s", addr, info[addr], status15 == PM3_EFAILED ? _RED_("read denied") : _RED_("read failed"));
        }
        // Update endian for files
        data[14] = BSWAP_32(data[14]);
        data[15] = BSWAP_32(data[15]);

    } else if (card_type == EM_4X69) {

        // To flag any blocks locked we need to read blocks 14 and 15 first
        // dont swap endin until we get block lock flags.
        status14 = EM4x05ReadWord_ext(EM4469_PROT_BLOCK, pwd, usePwd, &word);
        if (status14 == PM3_SUCCESS) {
            if ((word & 0x00008000) != 0x00) {
                lock_bits = word;
                gotLockBits = true;
            }
            data[EM4469_PROT_BLOCK] = word;
        } else {
            success = PM3_ESOFT; // If any error ensure fail is set so not to save invalid data
        }

        uint32_t lockbit;

        for (; addr < 15; addr++) {
            lockbit = (lock_bits >> addr) & 1;
            if (addr == 2) {
                if (usePwd) {
                    data[addr] = BSWAP_32(pwd);
                    num_to_bytes(pwd, 4, bytes);
                    PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %s", addr, pwd, sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info4x69[addr]);
                } else {
                    data[addr] = 0x00; // Unknown password, but not used to set to zeros
                    PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s " _YELLOW_("write only"), addr, info4x69[addr]);
                }
            } else {

                status = EM4x05ReadWord_ext(addr, pwd, usePwd, &word);
                if (status != PM3_SUCCESS) {
                    success = PM3_ESOFT; // If any error ensure fail is set so not to save invalid data
                }

                data[addr] = BSWAP_32(word);
                if (status == PM3_SUCCESS) {
                    num_to_bytes(word, 4, bytes);
                    PrintAndLogEx(INFO, "  %02u | %08X | %s  | %s | %s", addr, word, sprint_ascii(bytes, 4), gotLockBits ? (lockbit ? _RED_("x") : " ") : _YELLOW_("?"), info4x69[addr]);
                } else {
                    PrintAndLogEx(INFO, "  %02u |          |       |   | %-10s %s", addr, info4x69[addr], status == PM3_EFAILED ? _RED_("read denied") : _RED_("read failed"));
                }
            }
        }

    } else {
    }

    if (success == PM3_SUCCESS) { // all ok save dump to file
        // saveFileEML will add .eml extension to filename
        // saveFile (binary) passes in the .bin extension.
        if (strcmp(filename, "") == 0) {

            if (card_type == EM_4X69) {
                sprintf(filename, "lf-4x69-%08X-dump", BSWAP_32(data[1]));
            } else {
                sprintf(filename, "lf-4x05-%08X-dump", BSWAP_32(data[1]));
            }

        }
        PrintAndLogEx(NORMAL, "");
        saveFileJSON(filename, (card_type == EM_4X69) ? jsfEM4x69 : jsfEM4x05, (uint8_t *)data, 16 * sizeof(uint32_t), NULL);

        saveFileEML(filename, (uint8_t *)data, 16 * sizeof(uint32_t), sizeof(uint32_t));
        saveFile(filename, ".bin", data, sizeof(data));
    }
    PrintAndLogEx(NORMAL, "");
    return success;
}

int CmdEM4x05Read(const char *Cmd) {
    uint8_t addr;
    uint32_t pwd;
    bool usePwd = false;

    uint8_t ctmp = tolower(param_getchar(Cmd, 0));
    if (strlen(Cmd) == 0 || ctmp == 'h') return usage_lf_em4x05_read();

    addr = param_get8ex(Cmd, 0, 50, 10);
    pwd =  param_get32ex(Cmd, 1, 0xFFFFFFFF, 16);

    if (addr > 15) {
        PrintAndLogEx(WARNING, "Address must be between 0 and 15");
        return PM3_ESOFT;
    }
    if (pwd == 0xFFFFFFFF) {
        PrintAndLogEx(INFO, "Reading address %02u", addr);
    } else {
        usePwd = true;
        PrintAndLogEx(INFO, "Reading address %02u using password %08X", addr, pwd);
    }

    uint32_t word = 0;
    int status = EM4x05ReadWord_ext(addr, pwd, usePwd, &word);
    if (status == PM3_SUCCESS)
        PrintAndLogEx(SUCCESS, "Address %02d | %08X - %s", addr, word, (addr > 13) ? "Lock" : "");
    else if (status == PM3_EFAILED)
        PrintAndLogEx(ERR, "Tag denied Read operation");
    else
        PrintAndLogEx(WARNING, "No answer from tag");
    return status;
}

int CmdEM4x05Write(const char *Cmd) {
    uint8_t ctmp = tolower(param_getchar(Cmd, 0));
    if (strlen(Cmd) == 0 || ctmp == 'h') return usage_lf_em4x05_write();

    bool usePwd = false;
    uint8_t addr;
    uint32_t data, pwd;

    addr = param_get8ex(Cmd, 0, 50, 10);
    data = param_get32ex(Cmd, 1, 0, 16);
    pwd =  param_get32ex(Cmd, 2, 0xFFFFFFFF, 16);
    bool protectOperation = addr == 99; // will do better with cliparser...

    if ((addr > 13) && (!protectOperation)) {
        PrintAndLogEx(WARNING, "Address must be between 0 and 13");
        return PM3_EINVARG;
    }
    if (pwd == 0xFFFFFFFF) {
        if (protectOperation)
            PrintAndLogEx(INFO, "Writing protection words data %08X", data);
        else
            PrintAndLogEx(INFO, "Writing address %d data %08X", addr, data);
    } else {
        usePwd = true;
        if (protectOperation)
            PrintAndLogEx(INFO, "Writing protection words data %08X using password %08X", data, pwd);
        else
            PrintAndLogEx(INFO, "Writing address %d data %08X using password %08X", addr, data, pwd);
    }

    if (protectOperation) { // set Protect Words
        struct {
            uint32_t password;
            uint32_t data;
            uint8_t usepwd;
        } PACKED payload;

        payload.password = pwd;
        payload.data = data;
        payload.usepwd = usePwd;

        clearCommandBuffer();
        SendCommandNG(CMD_LF_EM4X_PROTECTWORD, (uint8_t *)&payload, sizeof(payload));
        PacketResponseNG resp;
        if (!WaitForResponseTimeout(CMD_LF_EM4X_PROTECTWORD, &resp, 2000)) {
            PrintAndLogEx(ERR, "Error occurred, device did not respond during write operation.");
            return PM3_ETIMEOUT;
        }
    } else {
        struct {
            uint32_t password;
            uint32_t data;
            uint8_t address;
            uint8_t usepwd;
        } PACKED payload;

        payload.password = pwd;
        payload.data = data;
        payload.address = addr;
        payload.usepwd = usePwd;

        clearCommandBuffer();
        SendCommandNG(CMD_LF_EM4X_WRITEWORD, (uint8_t *)&payload, sizeof(payload));
        PacketResponseNG resp;
        if (!WaitForResponseTimeout(CMD_LF_EM4X_WRITEWORD, &resp, 2000)) {
            PrintAndLogEx(ERR, "Error occurred, device did not respond during write operation.");
            return PM3_ETIMEOUT;
        }
    }
    if (!downloadSamplesEM())
        return PM3_ENODATA;

    uint32_t dummy = 0;
    int status = demodEM4x05resp(&dummy, true);
    if (status == PM3_SUCCESS)
        PrintAndLogEx(SUCCESS, "Success writing to tag");
    else if (status == PM3_EFAILED)
        PrintAndLogEx(ERR, "Tag denied %s operation", protectOperation ? "Protect" : "Write");
    else
        PrintAndLogEx(DEBUG, "No answer from tag");

    PrintAndLogEx(HINT, "Hint: try " _YELLOW_("`lf em 4x05_read`") " to verify");
    return status;
}

int CmdEM4x05Wipe(const char *Cmd) {
    uint8_t addr = 0;
    uint32_t pwd = 0;
    uint8_t cmdp = 0;
    uint8_t  chipType  = 1; // em4305
    uint32_t chipInfo  = 0x00040072; // Chip info/User Block normal 4305 Chip Type
    uint32_t chipUID   = 0x614739AE; // UID normally readonly, but just in case
    uint32_t blockData = 0x00000000; // UserBlock/Password (set to 0x00000000 for a wiped card1
    uint32_t config    = 0x0001805F; // Default config (no password)
    int success = PM3_SUCCESS;
    char cmdStr [100];
    char optchk[10];

    while (param_getchar(Cmd, cmdp) != 0x00) {
        // check if cmd is a 1 byte option
        param_getstr(Cmd, cmdp, optchk, sizeof(optchk));
        if (strlen(optchk) == 1) { // Have a single character so option not part of password
            switch (tolower(param_getchar(Cmd, cmdp))) {
                case 'c':   // chip type
                    if (param_getchar(Cmd, cmdp) != 0x00)
                        chipType = param_get8ex(Cmd, cmdp + 1, 0, 10);
                    cmdp += 2;
                    break;
                case 'h':   // return usage_lf_em4x05_wipe();
                default :   // Unknown or 'h' send help
                    return usage_lf_em4x05_wipe();
                    break;
            };
        } else { // Not a single character so assume password
            pwd = param_get32ex(Cmd, cmdp, 1, 16);
            cmdp++;
        }
    }

    switch (chipType) {
        case 0  : // em4205
            chipInfo  = 0x00040070;
            config    = 0x0001805F;
            break;
        case 1  : // em4305
            chipInfo  = 0x00040072;
            config    = 0x0001805F;
            break;
        default : // Type 0/Default : EM4305
            chipInfo  = 0x00040072;
            config    = 0x0001805F;
    }

    // block 0 : User Data or Chip Info
    sprintf(cmdStr, "%d %08X %08X", 0, chipInfo, pwd);
    CmdEM4x05Write(cmdStr);
    // block 1 : UID - this should be read only for EM4205 and EM4305 not sure about others
    sprintf(cmdStr, "%d %08X %08X", 1, chipUID, pwd);
    CmdEM4x05Write(cmdStr);
    // block 2 : password
    sprintf(cmdStr, "%d %08X %08X", 2, blockData, pwd);
    CmdEM4x05Write(cmdStr);
    pwd = blockData; // Password should now have changed, so use new password
    // block 3 : user data
    sprintf(cmdStr, "%d %08X %08X", 3, blockData, pwd);
    CmdEM4x05Write(cmdStr);
    // block 4 : config
    sprintf(cmdStr, "%d %08X %08X", 4, config, pwd);
    CmdEM4x05Write(cmdStr);

    // Remainder of user/data blocks
    for (addr = 5; addr < 14; addr++) {// Clear user data blocks
        sprintf(cmdStr, "%d %08X %08X", addr, blockData, pwd);
        CmdEM4x05Write(cmdStr);
    }

    return success;
}

static void printEM4x05config(uint32_t wordData) {
    uint16_t datarate = (((wordData & 0x3F) + 1) * 2);
    uint8_t encoder = ((wordData >> 6) & 0xF);
    char enc[14];
    memset(enc, 0, sizeof(enc));

    uint8_t PSKcf = (wordData >> 10) & 0x3;
    char cf[10];
    memset(cf, 0, sizeof(cf));
    uint8_t delay = (wordData >> 12) & 0x3;
    char cdelay[33];
    memset(cdelay, 0, sizeof(cdelay));
    uint8_t numblks = EM4x05_GET_NUM_BLOCKS(wordData);
    uint8_t LWR = numblks + 5 - 1; //last word read
    switch (encoder) {
        case 0:
            snprintf(enc, sizeof(enc), "NRZ");
            break;
        case 1:
            snprintf(enc, sizeof(enc), "Manchester");
            break;
        case 2:
            snprintf(enc, sizeof(enc), "Biphase");
            break;
        case 3:
            snprintf(enc, sizeof(enc), "Miller");
            break;
        case 4:
            snprintf(enc, sizeof(enc), "PSK1");
            break;
        case 5:
            snprintf(enc, sizeof(enc), "PSK2");
            break;
        case 6:
            snprintf(enc, sizeof(enc), "PSK3");
            break;
        case 7:
            snprintf(enc, sizeof(enc), "Unknown");
            break;
        case 8:
            snprintf(enc, sizeof(enc), "FSK1");
            break;
        case 9:
            snprintf(enc, sizeof(enc), "FSK2");
            break;
        default:
            snprintf(enc, sizeof(enc), "Unknown");
            break;
    }

    switch (PSKcf) {
        case 0:
            snprintf(cf, sizeof(cf), "RF/2");
            break;
        case 1:
            snprintf(cf, sizeof(cf), "RF/8");
            break;
        case 2:
            snprintf(cf, sizeof(cf), "RF/4");
            break;
        case 3:
            snprintf(cf, sizeof(cf), "unknown");
            break;
    }

    switch (delay) {
        case 0:
            snprintf(cdelay, sizeof(cdelay), "no delay");
            break;
        case 1:
            snprintf(cdelay, sizeof(cdelay), "BP/8 or 1/8th bit period delay");
            break;
        case 2:
            snprintf(cdelay, sizeof(cdelay), "BP/4 or 1/4th bit period delay");
            break;
        case 3:
            snprintf(cdelay, sizeof(cdelay), "no delay");
            break;
    }
    uint8_t readLogin = (wordData & EM4x05_READ_LOGIN_REQ) >> 18;
    uint8_t readHKL = (wordData & EM4x05_READ_HK_LOGIN_REQ) >> 19;
    uint8_t writeLogin = (wordData & EM4x05_WRITE_LOGIN_REQ) >> 20;
    uint8_t writeHKL = (wordData & EM4x05_WRITE_HK_LOGIN_REQ) >> 21;
    uint8_t raw = (wordData & EM4x05_READ_AFTER_WRITE) >> 22;
    uint8_t disable = (wordData & EM4x05_DISABLE_ALLOWED) >> 23;
    uint8_t rtf = (wordData & EM4x05_READER_TALK_FIRST) >> 24;
    uint8_t pigeon = (wordData & (1 << 26)) >> 26;

    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Config Information") " ------------------------");
    PrintAndLogEx(INFO, "ConfigWord: %08X (Word 4)", wordData);
    PrintAndLogEx(INFO, " Data Rate:  %02u | "_YELLOW_("RF/%u"), wordData & 0x3F, datarate);
    PrintAndLogEx(INFO, "   Encoder:   %u | " _YELLOW_("%s"), encoder, enc);
    PrintAndLogEx(INFO, "    PSK CF:   %u | %s", PSKcf, cf);
    PrintAndLogEx(INFO, "     Delay:   %u | %s", delay, cdelay);
    PrintAndLogEx(INFO, " LastWordR:  %02u | Address of last word for default read - meaning %u blocks are output", LWR, numblks);
    PrintAndLogEx(INFO, " ReadLogin:   %u | Read login is %s", readLogin, readLogin ? _YELLOW_("required") :  _GREEN_("not required"));
    PrintAndLogEx(INFO, "   ReadHKL:   %u | Read housekeeping words login is %s", readHKL, readHKL ? _YELLOW_("required") : _GREEN_("not required"));
    PrintAndLogEx(INFO, "WriteLogin:   %u | Write login is %s", writeLogin, writeLogin ? _YELLOW_("required") :  _GREEN_("not required"));
    PrintAndLogEx(INFO, "  WriteHKL:   %u | Write housekeeping words login is %s", writeHKL, writeHKL ? _YELLOW_("required") :  _GREEN_("not Required"));
    PrintAndLogEx(INFO, "    R.A.W.:   %u | Read after write is %s", raw, raw ? "on" : "off");
    PrintAndLogEx(INFO, "   Disable:   %u | Disable command is %s", disable, disable ? "accepted" : "not accepted");
    PrintAndLogEx(INFO, "    R.T.F.:   %u | Reader talk first is %s", rtf, rtf ? _YELLOW_("enabled") : "disabled");
    PrintAndLogEx(INFO, "    Pigeon:   %u | Pigeon mode is %s", pigeon, pigeon ? _YELLOW_("enabled") : "disabled");
}

static void printEM4x05info(uint32_t block0, uint32_t serial) {

    uint8_t chipType = (block0 >> 1) & 0xF;
    uint8_t cap = (block0 >> 5) & 3;
    uint16_t custCode = (block0 >> 9) & 0x2FF;

    /* bits
    //  0,   rfu
    //  1,2,3,4  chip type
    //  5,6  resonant cap
    //  7,8, rfu
    //  9 - 18 customer code
    //  19,  rfu

       98765432109876543210
       001000000000
    // 00100000000001111000
                   xxx----
    //                1100
    //             011
    // 00100000000
    */

    PrintAndLogEx(INFO, "--- " _CYAN_("Tag Information") " ---------------------------");

    PrintAndLogEx(SUCCESS, "    Block0: " _GREEN_("%08x") " (Word 0)", block0);
    PrintAndLogEx(SUCCESS, " Chip Type:   %3u | " _YELLOW_("%s"), chipType, em_get_card_str(block0));

    switch (cap) {
        case 3:
            PrintAndLogEx(SUCCESS, "  Cap Type:   %3u | 330pF", cap);
            break;
        case 2:
            PrintAndLogEx(SUCCESS, "  Cap Type:   %3u | %spF", cap, (chipType == 4) ? "75" : "210");
            break;
        case 1:
            PrintAndLogEx(SUCCESS, "  Cap Type:   %3u | 250pF", cap);
            break;
        case 0:
            PrintAndLogEx(SUCCESS, "  Cap Type:   %3u | no resonant capacitor", cap);
            break;
        default:
            PrintAndLogEx(SUCCESS, "  Cap Type:   %3u | unknown", cap);
            break;
    }

    PrintAndLogEx(SUCCESS, " Cust Code: 0x%x | %s", custCode, (custCode == 0x200) ? "Default" : "Unknown");
    if (serial != 0)
        PrintAndLogEx(SUCCESS, "  Serial #: " _YELLOW_("%08X"), serial);
}

static void printEM4x05ProtectionBits(uint32_t word, uint8_t addr) {
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--- " _CYAN_("Protection") " --------------------------------");
    PrintAndLogEx(INFO, "ProtectionWord: %08X (Word %i)", word, addr);
    for (uint8_t i = 0; i < 15; i++) {
        PrintAndLogEx(INFO, "      Word:  %02u | %s", i, ((1 << i) & word) ? _RED_("write Locked") : "unlocked");
        if (i == 14)
            PrintAndLogEx(INFO, "      Word:  %02u | %s", i + 1, ((1 << i) & word) ? _RED_("write locked") : "unlocked");
    }
}

//quick test for EM4x05/EM4x69 tag
bool EM4x05IsBlock0(uint32_t *word) {
    return (EM4x05ReadWord_ext(0, 0, false, word) == PM3_SUCCESS);
}

int CmdEM4x05Info(const char *Cmd) {
    uint32_t pwd;
    uint32_t word = 0, block0 = 0, serial = 0;
    bool usePwd = false;
    uint8_t ctmp = tolower(param_getchar(Cmd, 0));
    if (ctmp == 'h') return usage_lf_em4x05_info();

    // for now use default input of 1 as invalid (unlikely 1 will be a valid password...)
    pwd = param_get32ex(Cmd, 0, 0xFFFFFFFF, 16);

    if (pwd != 0xFFFFFFFF)
        usePwd = true;

    // read word 0 (chip info)
    // block 0 can be read even without a password.
    if (EM4x05IsBlock0(&block0) == false)
        return PM3_ESOFT;

    // based on Block0 ,  decide type.
    int card_type = em_get_card_type(block0);

    // read word 1 (serial #) doesn't need pwd
    // continue if failed, .. non blocking fail.
    EM4x05ReadWord_ext(EM_SERIAL_BLOCK, 0, false, &serial);

    printEM4x05info(block0, serial);

    // read word 4 (config block)
    // needs password if one is set
    if (EM4x05ReadWord_ext(EM_CONFIG_BLOCK, pwd, usePwd, &word) != PM3_SUCCESS)
        return PM3_ESOFT;

    printEM4x05config(word);

    // if 4469 read EM4469_PROT_BLOCK
    // if 4305 read 14,15
    if (card_type == EM_4205 || card_type == EM_4305) {

        // read word 14 and 15 to see which is being used for the protection bits
        if (EM4x05ReadWord_ext(EM4305_PROT1_BLOCK, pwd, usePwd, &word) != PM3_SUCCESS) {
            return PM3_ESOFT;
        }

        if (word & 0x8000) {
            printEM4x05ProtectionBits(word, EM4305_PROT1_BLOCK);
            return PM3_SUCCESS;
        } else { // if status bit says this is not the used protection word
            if (EM4x05ReadWord_ext(EM4305_PROT2_BLOCK, pwd, usePwd, &word) != PM3_SUCCESS)
                return PM3_ESOFT;
            if (word & 0x8000) {
                printEM4x05ProtectionBits(word, EM4305_PROT2_BLOCK);
                return PM3_SUCCESS;
            }
        }
    } else if (card_type == EM_4X69) {
        // read word 3 to see which is being used for the protection bits
        if (EM4x05ReadWord_ext(EM4469_PROT_BLOCK, pwd, usePwd, &word) != PM3_SUCCESS) {
            return PM3_ESOFT;
        }
        printEM4x05ProtectionBits(word, EM4469_PROT_BLOCK);
    }
    //something went wrong
    return PM3_ESOFT;
}

static bool is_cancelled(void) {
    if (kbd_enter_pressed()) {
        PrintAndLogEx(WARNING, "\naborted via keyboard!\n");
        return true;
    }
    return false;
}
// load a default pwd file.
int CmdEM4x05Chk(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf em 4x05_chk",
                  "This command uses a dictionary attack against EM4205/4305/4469/4569",
                  "lf em 4x05_chk\n"
                  "lf em 4x05_chk -e 0x00000022B8        -> remember to use 0x for hex\n"
                  "lf em 4x05_chk -f t55xx_default_pwds  -> use T55xx default dictionary"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_strx0("f", "file", "<*.dic>", "loads a default keys dictionary file <*.dic>"),
        arg_u64_0("e", "em", "<EM4100>", "try the calculated password from some cloners based on EM4100 ID"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    int fnlen = 0;
    char filename[FILE_PATH_SIZE] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)filename, FILE_PATH_SIZE, &fnlen);
    uint64_t card_id = arg_get_u64_def(ctx, 2, 0);
    CLIParserFree(ctx);

    if (strlen(filename) == 0) {
        snprintf(filename, sizeof(filename), "t55xx_default_pwds");
    }
    PrintAndLogEx(NORMAL, "");

    bool found = false;
    uint64_t t1 = msclock();

    // White cloner password based on EM4100 ID
    if (card_id > 0) {

        uint32_t pwd = lf_t55xx_white_pwdgen(card_id & 0xFFFFFFFF);
        PrintAndLogEx(INFO, "testing %08"PRIX32" generated ", pwd);

        int status = EM4x05Login_ext(pwd);
        if (status == PM3_SUCCESS) {
            PrintAndLogEx(SUCCESS, "found valid password [ " _GREEN_("%08"PRIX32) " ]", pwd);
            found = true;
        } else if (status != PM3_EFAILED) {
            PrintAndLogEx(WARNING, "No answer from tag");
        }
    }

    // Loop dictionary
    uint8_t *keyBlock = NULL;
    if (found == false) {

        PrintAndLogEx(INFO, "press " _YELLOW_("'enter'") " to cancel the command");

        uint32_t keycount = 0;

        int res = loadFileDICTIONARY_safe(filename, (void **) &keyBlock, 4, &keycount);
        if (res != PM3_SUCCESS || keycount == 0 || keyBlock == NULL) {
            PrintAndLogEx(WARNING, "no keys found in file");
            if (keyBlock != NULL)
                free(keyBlock);

            return PM3_ESOFT;
        }

        for (uint32_t c = 0; c < keycount; ++c) {

            if (!session.pm3_present) {
                PrintAndLogEx(WARNING, "device offline\n");
                free(keyBlock);
                return PM3_ENODATA;
            }

            if (is_cancelled()) {
                free(keyBlock);
                return PM3_EOPABORTED;
            }

            uint32_t curr_password = bytes_to_num(keyBlock + 4 * c, 4);

            PrintAndLogEx(INFO, "testing %08"PRIX32, curr_password);

            int status = EM4x05Login_ext(curr_password);
            if (status == PM3_SUCCESS) {
                PrintAndLogEx(SUCCESS, "found valid password [ " _GREEN_("%08"PRIX32) " ]", curr_password);
                found = true;
                break;
            } else if (status != PM3_EFAILED) {
                PrintAndLogEx(WARNING, "No answer from tag");
            }
        }
    }

    if (found == false)
        PrintAndLogEx(WARNING, "check pwd failed");

    free(keyBlock);

    t1 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "\ntime in check pwd " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);
    return PM3_SUCCESS;
}

int CmdEM4x05Brute(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf em 4x05_brute",
                  "This command tries to bruteforce the password of a EM4205/4305/4469/4569\n",
                  "Note: if you get many false positives, change position on the antenna"
                  "lf em 4x05_brute\n"
                  "lf em 4x05_brute -n 1                   -> stop after first candidate found\n"
                  "lf em 4x05_brute -s 0x000022B8          -> remember to use 0x for hex"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_u64_0("s", "start", "<pwd>", "Start bruteforce enumeration from this password value"),
        arg_int0("n", "", "<digits>", "Stop after having found n candidates. Default: 0 => infinite"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    uint32_t start_pwd = arg_get_u64_def(ctx, 1, 0);
    uint32_t n = arg_get_int_def(ctx, 2, 0);
    CLIParserFree(ctx);

    PrintAndLogEx(NORMAL, "");

    struct {
        uint32_t start_pwd;
        uint32_t n;
    } PACKED payload;

    payload.start_pwd = start_pwd;
    payload.n = n;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_EM4X_BF, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;
    if (!WaitForResponseTimeout(CMD_LF_EM4X_BF, &resp, 1000)) {
        PrintAndLogEx(WARNING, "(EM4x05 Bruteforce) timeout while waiting for reply.");
        return PM3_ETIMEOUT;
    }
    PrintAndLogEx(INFO, "Bruteforce is running on device side, press button to interrupt");
    return PM3_SUCCESS;
}

typedef struct {
    uint16_t cnt;
    uint32_t value;
} em4x05_unlock_item_t;

static int unlock_write_protect(bool use_pwd, uint32_t pwd, uint32_t data, bool verbose) {

    struct {
        uint32_t password;
        uint32_t data;
        uint8_t usepwd;
    } PACKED payload;

    payload.password = pwd;
    payload.data = data;
    payload.usepwd = use_pwd;

    clearCommandBuffer();
    SendCommandNG(CMD_LF_EM4X_PROTECTWORD, (uint8_t *)&payload, sizeof(payload));
    PacketResponseNG resp;
    if (WaitForResponseTimeout(CMD_LF_EM4X_PROTECTWORD, &resp, 2000) == false) {
        PrintAndLogEx(ERR, "Error occurred, device did not respond during write operation.");
        return PM3_ETIMEOUT;
    }

    if (!downloadSamplesEM())
        return PM3_ENODATA;

    uint32_t dummy = 0;
    int status = demodEM4x05resp(&dummy, true);
    if (status == PM3_SUCCESS && verbose)
        PrintAndLogEx(SUCCESS, "Success writing to tag");
    else if (status == PM3_EFAILED)
        PrintAndLogEx(ERR, "Tag denied PROTECT operation");
    else
        PrintAndLogEx(DEBUG, "No answer from tag");

    return status;
}
static int unlock_reset(bool use_pwd, uint32_t pwd, uint32_t data, bool verbose) {
    if (verbose)
        PrintAndLogEx(INFO, "resetting the " _RED_("active") " lock block");

    return unlock_write_protect(use_pwd, pwd, data, false);
}
static void unlock_add_item(em4x05_unlock_item_t *array, uint8_t len, uint32_t value) {

    uint8_t i = 0;
    for (; i < len; i++) {
        if (array[i].value == value) {
            array[i].cnt++;
            break;
        }
        if (array[i].cnt == 0) {
            array[i].cnt++;
            array[i].value = value;
            break;
        }
    }
}

int CmdEM4x05Unlock(const char *Cmd) {

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf em 4x05_unlock",
                  "execute tear off against EM4205/4305/4469/4569",
                  "lf em 4x05_unlock\n"
                  "lf em 4x05_unlock -s 4100 -e 4100       -> lock on and autotune at 4100us\n"
                  "lf em 4x05_unlock -n 10 -s 3000 -e 4400 -> scan delays 3000us -> 4400us"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_int0("n", NULL, NULL, "steps to skip"),
        arg_int0("s", "start", "<us>", "start scan from delay (us)"),
        arg_int0("e", "end", "<us>", "end scan at delay (us)"),
        arg_u64_0("p", "pwd", "", "password (0x00000000)"),
        arg_lit0("v", "verbose", "verbose output"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    double n = (double)arg_get_int_def(ctx, 1, 0);
    double start = (double)arg_get_int_def(ctx, 2, 2000);
    double end = (double)arg_get_int_def(ctx, 3, 6000);
    uint64_t inputpwd = arg_get_u64_def(ctx, 4, 0xFFFFFFFFFFFFFFFF);
    bool verbose = arg_get_lit(ctx, 5);
    CLIParserFree(ctx);

    if (start > end) {
        PrintAndLogEx(FAILED, "start delay can\'t be larger than end delay %.0lf vs %.0lf", start, end);
        return PM3_EINVARG;
    }

    if (session.pm3_present == false) {
        PrintAndLogEx(WARNING, "device offline\n");
        return PM3_ENODATA;
    }

    bool use_pwd = false;
    uint32_t pwd = 0;
    if (inputpwd != 0xFFFFFFFFFFFFFFFF) {
        use_pwd = true;
        pwd = inputpwd & 0xFFFFFFFF;
    }

    uint32_t search_value = 0;
    uint32_t write_value = 0;
    //
    // inital phase
    //
    // read word 14
    uint32_t init_14 = 0;
    int res = EM4x05ReadWord_ext(14, pwd, use_pwd, &init_14);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "failed to read word 14\n");
        return PM3_ENODATA;
    }


    // read 15
    uint32_t init_15 = 0;
    res = EM4x05ReadWord_ext(15, pwd, use_pwd, &init_15);
    if (res != PM3_SUCCESS) {
        PrintAndLogEx(FAILED, "failed to read word 15\n");
        return PM3_ENODATA;
    }

#define ACTIVE_MASK 0x00008000
    if ((init_15 & ACTIVE_MASK) == ACTIVE_MASK) {
        search_value = init_15;
    } else {
        search_value = init_14;
    }

    if (search_value == ACTIVE_MASK) {
        PrintAndLogEx(SUCCESS, "Tag already fully unlocked, nothing to do");
        return PM3_SUCCESS;
    }

    bool my_auto = false;
    if (n == 0) {
        my_auto = true;
        n = (end - start) / 2;
    }

    // fix at one specific delay
    if (start == end) {
        n = 0;
    }

    PrintAndLogEx(INFO, "--------------- " _CYAN_("EM4x05 tear-off : target PROTECT") " -----------------------\n");

    PrintAndLogEx(INFO, "initial prot 14&15 [ " _GREEN_("%08X") ", " _GREEN_("%08X")  " ]", init_14, init_15);

    if (use_pwd) {
        PrintAndLogEx(INFO, "   target password [ " _GREEN_("%08X") " ]", pwd);
    }
    if (my_auto) {
        PrintAndLogEx(INFO, "    automatic mode [ " _GREEN_("enabled") " ]");
    }

    PrintAndLogEx(INFO, "   target stepping [ " _GREEN_("%.0lf") " ]", n);
    PrintAndLogEx(INFO, "target delay range [ " _GREEN_("%.0lf") " ... " _GREEN_("%.0lf") " ]", start, end);
    PrintAndLogEx(INFO, "      search value [ " _GREEN_("%08X") " ]", search_value);
    PrintAndLogEx(INFO, "       write value [ " _GREEN_("%08X") " ]", write_value);

    PrintAndLogEx(INFO, "----------------------------------------------------------------------------\n");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "press " _YELLOW_("'enter'") " to cancel the command");
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, "--------------- " _CYAN_("start") " -----------------------\n");

    int exit_code = PM3_SUCCESS;
    uint32_t word14 = 0, word15 = 0;
    uint32_t word14b = 0, word15b = 0;
    uint32_t tries = 0;
    uint32_t soon = 0;
    uint32_t late = 0;

    em4x05_unlock_item_t flipped[64] = {{0, 0}};

    //
    // main loop
    //
    bool success = false;
    uint64_t t1 = msclock();
    while (start <= end) {

        if (my_auto && n < 1) {
            PrintAndLogEx(INFO, "Reached n < 1                       => " _YELLOW_("disabling automatic mode"));
            end = start;
            my_auto = false;
            n = 0;
        }

        if (my_auto == false) {
            start += n;
        }

        if (tries >= 5 && n == 0 && soon != late) {

            if (soon > late) {
                PrintAndLogEx(INFO, "Tried %d times, soon:%i late:%i        => " _CYAN_("adjust +1 us >> %.0lf us"), tries, soon, late, start);
                start++;
                end++;
            } else {
                PrintAndLogEx(INFO, "Tried %d times, soon:%i late:%i        => " _CYAN_("adjust -1 us >> %.0lf us"), tries, soon, late, start);
                start--;
                end--;
            }
            tries = 0;
            soon = 0;
            late = 0;
        }

        if (is_cancelled()) {
            exit_code = PM3_EOPABORTED;
            break;
        }

        // set tear off trigger
        clearCommandBuffer();
        tearoff_params_t params = {
            .delay_us = start,
            .on = true,
            .off = false
        };
        res = handle_tearoff(&params, verbose);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "failed to configure tear off");
            return PM3_ESOFT;
        }

        // write
        res = unlock_write_protect(use_pwd, pwd, write_value, verbose);

        // read after trigger
        res = EM4x05ReadWord_ext(14, pwd, use_pwd, &word14);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "failed to read 14");
            return PM3_ESOFT;
        }

        // read after trigger
        res = EM4x05ReadWord_ext(15, pwd, use_pwd, &word15);
        if (res != PM3_SUCCESS) {
            PrintAndLogEx(WARNING, "failed to read 15");
            return PM3_ESOFT;
        }

        if (verbose)
            PrintAndLogEx(INFO, "ref:%08X   14:%08X   15:%08X ", search_value, word14, word15);

        if (word14 == search_value && word15 == 0) {
            PrintAndLogEx(INFO, "Status: Nothing happened            => " _GREEN_("tearing too soon"));

            if (my_auto) {
                start += n;
                PrintAndLogEx(INFO, "                                    => " _CYAN_("adjust +%.0lf us >> %.0lf us"), n, start);
                n /= 2;
            } else {
                soon++;
            }
        } else {

            if (word15 == search_value) {

                if (word14 == 0) {
                    PrintAndLogEx(INFO, "Status: Protect succeeded           => " _GREEN_("tearing too late"));
                } else {
                    if (word14 == search_value) {
                        PrintAndLogEx(INFO, "Status: 15 ok, 14 not yet erased    => " _GREEN_("tearing too late"));
                    } else {
                        PrintAndLogEx(INFO, "Status: 15 ok, 14 partially erased  => " _GREEN_("tearing too late"));
                    }
                }

                unlock_reset(use_pwd, pwd, write_value, verbose);

                // read after reset
                res = EM4x05ReadWord_ext(14, pwd, use_pwd, &word14b);
                if (res != PM3_SUCCESS) {
                    PrintAndLogEx(WARNING, "failed to read 14");
                    return PM3_ESOFT;
                }

                if (word14b == 0) {

                    unlock_reset(use_pwd, pwd, write_value, verbose);

                    res = EM4x05ReadWord_ext(14, pwd, use_pwd, &word14b);
                    if (res != PM3_SUCCESS) {
                        PrintAndLogEx(WARNING, "failed to read 14");
                        return PM3_ESOFT;
                    }
                }

                if (word14b != search_value) {

                    res = EM4x05ReadWord_ext(15, pwd, use_pwd, &word15b);
                    if (res == PM3_SUCCESS) {
                        PrintAndLogEx(INFO, "Status: new definitive value!       => " _RED_("SUCCESS:") " 14: " _CYAN_("%08X") "  15: %08X", word14b, word15b);
                        success = true;
                        break;
                    } else {
                        PrintAndLogEx(WARNING, "failed to read 15");
                        return PM3_ESOFT;
                    }
                }
                if (my_auto) {
                    end = start;
                    start -= n;
                    PrintAndLogEx(INFO, "                                    => " _CYAN_("adjust -%.0lf us >> %.0lf us"), n, start);
                    n /= 2;
                } else {
                    late++;
                }

            } else {

                if ((word15 & ACTIVE_MASK) == ACTIVE_MASK) {

                    PrintAndLogEx(INFO, "Status: 15 bitflipped and active    => " _RED_("SUCCESS?:  ") "14: %08X  15: " _CYAN_("%08X"), word14, word15);
                    PrintAndLogEx(INFO, "Committing results...");

                    unlock_reset(use_pwd, pwd, write_value, verbose);

                    // read after reset
                    res = EM4x05ReadWord_ext(14, pwd, use_pwd, &word14b);
                    if (res != PM3_SUCCESS) {
                        PrintAndLogEx(WARNING, "failed to read 14");
                        return PM3_ESOFT;
                    }

                    res = EM4x05ReadWord_ext(15, pwd, use_pwd, &word15b);
                    if (res != PM3_SUCCESS) {
                        PrintAndLogEx(WARNING, "failed to read 15");
                        return PM3_ESOFT;
                    }

                    if (verbose)
                        PrintAndLogEx(INFO, "ref:%08x   14:%08X   15:%08X", search_value, word14b, word15b);

                    if ((word14b & ACTIVE_MASK) == ACTIVE_MASK) {

                        if (word14b == word15) {
                            PrintAndLogEx(INFO, "Status: confirmed                   => " _RED_("SUCCESS:   ") "14: " _CYAN_("%08X") "  15: %08X", word14b, word15b);

                            unlock_add_item(flipped, 64, word14b);
                            success = true;
                            break;
                        }

                        if (word14b != search_value) {
                            PrintAndLogEx(INFO, "Status: new definitive value!       => " _RED_("SUCCESS:   ") "14: " _CYAN_("%08X") "  15: %08X", word14b, word15b);

                            unlock_add_item(flipped, 64, word14b);
                            success = true;
                            break;
                        }

                        PrintAndLogEx(INFO, "Status: failed to commit bitflip        => " _RED_("FAIL:      ") "14: %08X  15: %08X", word14b, word15b);
                    }
                    if (my_auto) {
                        n = 0;
                        end = start;
                    } else {
                        tries = 0;
                        soon = 0;
                        late = 0;
                    }
                } else {
                    PrintAndLogEx(INFO, "Status: 15 bitflipped but inactive      => " _YELLOW_("PROMISING: ") "14: %08X  15: " _CYAN_("%08X"), word14, word15);

                    unlock_add_item(flipped, 64, word15);

                    soon ++;
                }
            }
        }

        if (my_auto == false) {
            tries++;
        }
    }

    PrintAndLogEx(INFO, "----------------------------- " _CYAN_("exit") " ----------------------------------\n");
    t1 = msclock() - t1;
    PrintAndLogEx(SUCCESS, "\ntime in unlock " _YELLOW_("%.0f") " seconds\n", (float)t1 / 1000.0);
    if (success) {
        uint32_t bitflips = search_value ^ word14b;
        PrintAndLogEx(INFO, "Old protection word => " _YELLOW_("%08X"), search_value);
        char bitstring[9] = {0};
        for (int i = 0; i < 8; i++) {
            bitstring[i] = bitflips & (0xF << ((7 - i) * 4)) ? 'x' : '.';
        }
        // compute number of bits flipped

        PrintAndLogEx(INFO, "Bitflips: %2u events => %s", bitcount32(bitflips), bitstring);
        PrintAndLogEx(INFO, "New protection word => " _CYAN_("%08X") "\n", word14b);


        PrintAndLogEx(INFO, "Try " _YELLOW_("`lf em 4x05_dump`"));
    }

    if (verbose) {
        PrintAndLogEx(NORMAL, "Stats:");
        PrintAndLogEx(INFO, " idx | value    | cnt | flipped bits");
        PrintAndLogEx(INFO, "-----+----------+-----+------");
        for (uint8_t i = 0; i < 64; i++) {
            if (flipped[i].cnt == 0)
                break;

            PrintAndLogEx(INFO, " %3u | %08X | %3u | %u", i, flipped[i].value, flipped[i].cnt, bitcount32(search_value ^ flipped[i].value));
        }
    }
    PrintAndLogEx(NORMAL, "");
    return exit_code;
}

static size_t em4x05_Sniff_GetNextBitStart (size_t idx, size_t sc, int *data, size_t *pulsesamples)
{
    while ((idx < sc) && (data[idx] <= 10)) // find a going high
        idx++;

    while ((idx < sc) && (data[idx] > -10)) // find going low  may need to add something here it SHOULD be a small clk around 0, but white seems to extend a bit.
        idx++;

    (*pulsesamples) = 0;
    while ((idx < sc) && ((data[idx+1] - data[idx]) < 10 )) {  // find "sharp rise"
        (*pulsesamples)++;
        idx++;
    }

    return idx;
}

uint32_t static em4x05_Sniff_GetBlock (char *bits, bool fwd) {
    uint32_t value = 0;
    uint8_t idx;
    bool parityerror = false;
    uint8_t parity;

    parity = 0;
    for (idx = 0; idx < 8; idx++) {
        value <<= 1;
        value += (bits[idx] - '0');
        parity += (bits[idx] - '0');
    }
    parity = parity % 2;
    if (parity != (bits[8] - '0'))
        parityerror = true;

    parity = 0;
    for (idx = 9; idx < 17; idx++) {
        value <<= 1;
        value += (bits[idx] - '0');
        parity += (bits[idx] - '0');
    }
    parity = parity % 2;
    if (parity != (bits[17] - '0'))
        parityerror = true;

    parity = 0;
    for (idx = 18; idx < 26; idx++) {
        value <<= 1;
        value += (bits[idx] - '0');
        parity += (bits[idx] - '0');
    }
    parity = parity % 2;
    if (parity != (bits[26] - '0'))
        parityerror = true;

    parity = 0;
    for (idx = 27; idx < 35; idx++) {
        value <<= 1;
        value += (bits[idx] - '0');
        parity += (bits[idx] - '0');
    }
    parity = parity % 2;
    if (parity != (bits[35] - '0'))
        parityerror = true;

    if (parityerror) printf ("parity error : ");

    if (!fwd) {
        uint32_t t1 = value;
        value = 0;
        for (idx = 0; idx < 32; idx++)
            value |= (((t1 >> idx) & 1) << (31 - idx));
    }
    return value;
}

int CmdEM4x05Sniff(const char *Cmd) {

    bool sampleData = true;
    bool haveData = false;
    size_t idx = 0;
    char cmdText [100];
    char dataText [100];
    char blkAddr[4];
    char bits[80];
    int bitidx;
    int ZeroWidth;    // 32-42 "1" is 32
    int CycleWidth;
    size_t pulseSamples;
    size_t pktOffset;
    int i;
    bool eop = false;
    uint32_t tmpValue;
    bool pwd = false;
    bool fwd = false;

    CLIParserContext *ctx;
    CLIParserInit(&ctx, "lf em 4x05_sniff",
                  "Sniff EM4x05 commands sent from a programmer",
                  "lf em 4x05_sniff -> sniff via lf sniff\n"
                  "lf em 4x05_sniff -1 -> sniff from data loaded into the buffer\n"
                  "lf em 4x05_sniff -r -> reverse the bit order when showing block data"
                 );

    void *argtable[] = {
        arg_param_begin,
        arg_lit0("1", "buf","Use the data in the buffer"),
        arg_lit0("r", "rev", "Reverse the bit order for data blocks"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);

    sampleData = !arg_get_lit(ctx,1);
    fwd = arg_get_lit(ctx,2);

    // setup and sample data from Proxmark
    // if not directed to existing sample/graphbuffer
    if (sampleData) {
        if (!IfPm3Lf()) {
            PrintAndLogEx(WARNING, "Only offline mode is available");
            return PM3_EINVARG;
        }
        CmdLFSniff("");
    }

    // Headings
    PrintAndLogEx(NORMAL, "");
    PrintAndLogEx(INFO, _CYAN_("EM4x05 command detection"));
    PrintAndLogEx(SUCCESS, "offset | Command     |   Data   | blk | raw");
    PrintAndLogEx(SUCCESS, "-------+-------------+----------+-----+------------------------------------------------------------");

    idx = 0;
    // loop though sample buffer
    while (idx < GraphTraceLen) {
        eop = false;
        haveData = false;
        pwd = false;

        idx = em4x05_Sniff_GetNextBitStart (idx, GraphTraceLen, GraphBuffer, &pulseSamples);
        pktOffset = idx;
        if (pulseSamples >= 10)  { // Should be 18 so a bit less to allow for processing
        
            // Use first bit to get "0" bit samples as a reference
            ZeroWidth = idx;
            idx = em4x05_Sniff_GetNextBitStart (idx, GraphTraceLen, GraphBuffer, &pulseSamples);
            ZeroWidth = idx - ZeroWidth;

            if (ZeroWidth <= 50) {
                pktOffset -= ZeroWidth;
                memset(bits,0x00,sizeof(bits));
                bitidx = 0;

                while ((idx < GraphTraceLen) && !eop) {
                    CycleWidth = idx;
                    idx = em4x05_Sniff_GetNextBitStart (idx, GraphTraceLen, GraphBuffer, &pulseSamples);

                    CycleWidth = idx - CycleWidth;
                    if ((CycleWidth > 300) || (CycleWidth < (ZeroWidth-5))) { // to long or too short
                        eop = true;
                        bits[bitidx++] = '0';   // Append last zero from the last bit find
                        cmdText[0] = 0;

                        // EM4305 command lengths
                        // Login        0011 <pwd>          => 4 +     45 => 49
                        // Write Word   0101 <adr> <data>   => 4 + 7 + 45 => 56
                        // Read Word    1001 <adr>          => 4 + 7      => 11
                        // Protect      1100       <data>   => 4 +     45 => 49
                        // Disable      1010       <data>   => 4 +     45 => 49
                        // -> disaable 1010 11111111 0 11111111 0 11111111 0 11111111 0 00000000 0

                        // Check to see if we got the leading 0
                        if  (((strncmp (bits,"00011",5) == 0)&& (bitidx == 50)) ||
                             ((strncmp (bits,"00101",5) == 0)&& (bitidx == 57)) ||
                             ((strncmp (bits,"01001",5) == 0)&& (bitidx == 12)) ||
                             ((strncmp (bits,"01100",5) == 0)&& (bitidx == 50)) ||
                             ((strncmp (bits,"01010",5) == 0)&& (bitidx == 50))) {
                                 memcpy (bits,&bits[1],bitidx-1);
                                 bitidx--;
                             printf ("Trim leading 0\n");
                        }
                        bits[bitidx] = 0;
         //   printf ("==> %s\n",bits);
                        // logon
                        if ((strncmp (bits,"0011",4) == 0) && (bitidx == 49)) {
                            haveData = true;
                            pwd = true;
                            sprintf (cmdText,"Logon");
                            sprintf (blkAddr,"   ");
                            tmpValue = em4x05_Sniff_GetBlock (&bits[4], fwd);
                            sprintf (dataText,"%08X",tmpValue);
                        }

                        // write
                        if ((strncmp (bits,"0101",4) == 0) && (bitidx == 56)) {
                            haveData = true;
                            sprintf (cmdText,"Write");
                            tmpValue = (bits[4] - '0') + ((bits[5] - '0') << 1) + ((bits[6] - '0') << 2)  + ((bits[7] - '0') << 3);
                            sprintf (blkAddr,"%d",tmpValue);
                            if (tmpValue == 2)
                                pwd = true;
                            tmpValue = em4x05_Sniff_GetBlock (&bits[11], fwd);
                            sprintf (dataText,"%08X",tmpValue);
                        }

                        // read
                        if ((strncmp (bits,"1001",4) == 0) && (bitidx == 11)) {
                            haveData = true;
                            pwd = false;
                            sprintf (cmdText,"Read");
                            tmpValue = (bits[4] - '0') + ((bits[5] - '0') << 1) + ((bits[6] - '0') << 2)  + ((bits[7] - '0') << 3);
                            sprintf (blkAddr,"%d",tmpValue);
                            sprintf (dataText," ");
                        }

                        // protect
                        if ((strncmp (bits,"1100",4) == 0) && (bitidx == 49)) {
                            haveData = true;
                            pwd = false;
                            sprintf (cmdText,"Protect");
                            sprintf (blkAddr," ");
                            tmpValue = em4x05_Sniff_GetBlock (&bits[11], fwd);
                            sprintf (dataText,"%08X",tmpValue);
                        }

                        // disable
                        if ((strncmp (bits,"1010",4) == 0) && (bitidx == 49)) {
                            haveData = true;
                            pwd = false;
                            sprintf (cmdText,"Disable");
                            sprintf (blkAddr," ");
                            tmpValue = em4x05_Sniff_GetBlock (&bits[11], fwd);
                            sprintf (dataText,"%08X",tmpValue);
                        }

                      //  bits[bitidx] = 0;
                    } else {
                        i = (CycleWidth - ZeroWidth) / 28;
                        bits[bitidx++] = '0';
                        for (int ii = 0; ii < i; ii++)
                            bits[bitidx++] = '1';
                    }
                }
            }
        }
        idx++;

        // Print results
        if (haveData) { //&& (minWidth > 1) && (maxWidth > minWidth)){
            if (pwd)
                PrintAndLogEx(SUCCESS, "%6llu | %-10s  | "_YELLOW_("%8s")" | "_YELLOW_("%3s")" | %s", pktOffset, cmdText, dataText, blkAddr, bits);
            else
                PrintAndLogEx(SUCCESS, "%6llu | %-10s  | "_GREEN_("%8s")" | "_GREEN_("%3s")" | %s", pktOffset, cmdText, dataText, blkAddr, bits);
        }
    }

    // footer
    PrintAndLogEx(SUCCESS, "---------------------------------------------------------------------------------------------------");
    PrintAndLogEx(NORMAL, "");

    return PM3_SUCCESS;
}
