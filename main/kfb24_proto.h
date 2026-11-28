// kfb24_proto.h —— Board <-> PC bridge line protocol.
//
// Every frame is one LF-terminated UTF-8 line:
//   KFB24 CMD <name> <id>              board -> PC
//   KFB24 HELLO <version>              both directions
//   KFB24 ACK <id> OK|ERR              both directions
//   KFB24 CTRL <voice_stop> <id>       PC -> board (ends the audio session)
//   KFB24 CFG <id> <ssid|-> <pass|-> <host|-> <port>
//                                      PC -> board (hex-encoded strings)
//   KFB24 STATE <usb|wifi|ble> <0|1> [detail]
//                                      board -> PC
//   KFB24 AUDIO A <seq> <crc32> <base64>
//                                      board -> PC (16 kHz mono IMA ADPCM,
//                                      640-sample blocks, USB/BLE/Wi-Fi)
//   KFB24 AUDIO END                    board -> PC
//   KFB24 AUDIO P <seq> <crc32> <base64>
//                                      PC -> board (same ADPCM block, played
//                                      through the ES8311 speaker)
//   KFB24 AUDIO PEND                   PC -> board (playback finished)
//   KFB24 TEXT <base64 UTF-8>          PC -> board (dialog text on screen)
//
// Console logs share the USB Serial/JTAG port, so receivers ignore every line
// that does not start with the exact prefix. Audio lines bypass
// kfb24_proto_parse() and are validated by the PC bridge.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KFB24_PROTO_PREFIX "KFB24 "
#define KFB24_PROTO_MAX_LINE 512
#define KFB24_SSID_MAX 32
#define KFB24_PASS_MAX 64
#define KFB24_HOST_MAX 63

typedef enum {
    KFB24_MSG_NONE = 0,
    KFB24_MSG_CMD,
    KFB24_MSG_HELLO,
    KFB24_MSG_ACK,
    KFB24_MSG_CTRL,
    KFB24_MSG_CFG,
    KFB24_MSG_STATE,
} kfb24_msg_type_t;

typedef struct {
    kfb24_msg_type_t type;
    uint32_t id;
    bool ok;
    char name[16];                 // CMD name
    char text[48];                 // HELLO version / STATE transport+detail
    char ssid[KFB24_SSID_MAX + 1];
    char pass[KFB24_PASS_MAX + 1];
    char host[KFB24_HOST_MAX + 1];
    uint16_t port;
} kfb24_msg_t;

// Parse one line. Returns false for logs, malformed frames, and oversized input.
bool kfb24_proto_parse(const char *line, kfb24_msg_t *out);

// Each formatter returns the line length (excluding the NUL) or 0 when the
// destination is too small. The trailing newline is included.
size_t kfb24_proto_fmt_cmd(char *buf, size_t cap, const char *name, uint32_t id);
size_t kfb24_proto_fmt_hello(char *buf, size_t cap, const char *version);
size_t kfb24_proto_fmt_ack(char *buf, size_t cap, uint32_t id, bool ok);
size_t kfb24_proto_fmt_ctrl(char *buf, size_t cap, const char *name, uint32_t id);
size_t kfb24_proto_fmt_state(char *buf, size_t cap, const char *transport,
                             bool up, const char *detail);

// Hex helpers used by CFG. decode() always NUL-terminates when cap > len and
// rejects odd input, non-hex characters, and output larger than cap - 1.
bool kfb24_proto_hex_decode(const char *hex, char *out, size_t cap, size_t *out_len);
size_t kfb24_proto_hex_encode(const char *src, size_t src_len, char *out, size_t cap);

// Base64 used by the PC -> board audio and text frames. Rejects invalid
// characters and padding, and never writes a NUL terminator (the caller adds
// one when the payload is text). Returns false unless the whole input decodes
// and fits in cap.
bool kfb24_proto_b64_decode(const char *src, size_t len, uint8_t *out,
                            size_t cap, size_t *out_len);
