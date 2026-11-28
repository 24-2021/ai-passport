#include "kfb24_proto.h"

#include <stdio.h>
#include <string.h>

static bool parse_u32(const char *s, uint32_t max, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t value = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (max - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

static bool copy_token(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (len >= cap) return false;
    memcpy(dst, src, len + 1);
    return true;
}

/* Splits on blanks like strtok_r, without depending on POSIX feature macros:
   the host tests compile this file with plain -std=c11, where glibc hides
   strtok_r behind _POSIX_C_SOURCE. */
static char *next_token(char **cursor) {
    char *p = *cursor;
    if (!p) return NULL;
    p += strspn(p, " \t\r\n");
    if (!*p) {
        *cursor = NULL;
        return NULL;
    }
    char *start = p;
    p += strcspn(p, " \t\r\n");
    if (*p) {
        *p = '\0';
        *cursor = p + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

bool kfb24_proto_parse(const char *line, kfb24_msg_t *out) {
    if (!line || !out) return false;
    memset(out, 0, sizeof(*out));

    size_t len = strlen(line);
    if (len + 1 > KFB24_PROTO_MAX_LINE) return false;
    if (strncmp(line, KFB24_PROTO_PREFIX, sizeof(KFB24_PROTO_PREFIX) - 1) != 0) {
        return false;
    }

    char work[KFB24_PROTO_MAX_LINE];
    memcpy(work, line, len + 1);

    char *save = work;
    char *tag = next_token(&save);
    if (!tag || strcmp(tag, "KFB24") != 0) return false;
    char *kind = next_token(&save);
    if (!kind) return false;

    if (strcmp(kind, "CMD") == 0) {
        char *name = next_token(&save);
        char *id = next_token(&save);
        if (!name || !id || next_token(&save)) return false;
        uint32_t value = 0;
        if (!parse_u32(id, UINT32_MAX, &value)) return false;
        if (!copy_token(out->name, sizeof(out->name), name)) return false;
        out->type = KFB24_MSG_CMD;
        out->id = value;
        return true;
    }

    if (strcmp(kind, "CTRL") == 0) {
        char *name = next_token(&save);
        char *id = next_token(&save);
        if (!name || !id || next_token(&save)) return false;
        uint32_t value = 0;
        if (!parse_u32(id, UINT32_MAX, &value)) return false;
        if (!copy_token(out->name, sizeof(out->name), name)) return false;
        out->type = KFB24_MSG_CTRL;
        out->id = value;
        return true;
    }

    if (strcmp(kind, "HELLO") == 0) {
        char *version = next_token(&save);
        if (next_token(&save)) return false;
        out->type = KFB24_MSG_HELLO;
        if (version && !copy_token(out->text, sizeof(out->text), version)) return false;
        return true;
    }

    if (strcmp(kind, "ACK") == 0) {
        char *id = next_token(&save);
        char *status = next_token(&save);
        if (!id || !status || next_token(&save)) return false;
        uint32_t value = 0;
        if (!parse_u32(id, UINT32_MAX, &value)) return false;
        if (strcmp(status, "OK") != 0 && strcmp(status, "ERR") != 0) return false;
        out->type = KFB24_MSG_ACK;
        out->id = value;
        out->ok = strcmp(status, "OK") == 0;
        return true;
    }

    if (strcmp(kind, "CFG") == 0) {
        char *id = next_token(&save);
        char *ssid = next_token(&save);
        char *pass = next_token(&save);
        char *host = next_token(&save);
        char *port = next_token(&save);
        if (!id || !ssid || !pass || !host || !port ||
            next_token(&save)) {
            return false;
        }
        uint32_t value = 0;
        uint32_t port_value = 0;
        if (!parse_u32(id, UINT32_MAX, &value)) return false;
        if (!parse_u32(port, 65535, &port_value) || port_value == 0) return false;

        size_t ssid_len = 0;
        size_t pass_len = 0;
        if (strcmp(ssid, "-") != 0) {
            if (!kfb24_proto_hex_decode(ssid, out->ssid, sizeof(out->ssid), &ssid_len)) {
                return false;
            }
        }
        if (strcmp(pass, "-") != 0) {
            if (!kfb24_proto_hex_decode(pass, out->pass, sizeof(out->pass), &pass_len)) {
                return false;
            }
        }
        (void)ssid_len;
        (void)pass_len;
        if (strcmp(host, "-") == 0) {
            out->host[0] = '\0';
        } else if (!copy_token(out->host, sizeof(out->host), host)) {
            return false;
        }

        out->type = KFB24_MSG_CFG;
        out->id = value;
        out->port = (uint16_t)port_value;
        return true;
    }

    if (strcmp(kind, "STATE") == 0) {
        char *transport = next_token(&save);
        char *up = next_token(&save);
        if (!transport || !up) return false;
        if (!copy_token(out->text, sizeof(out->text), transport)) return false;
        out->type = KFB24_MSG_STATE;
        out->ok = strcmp(up, "1") == 0;
        return true;
    }

    return false;
}

static size_t finish_line(size_t cap, int written) {
    if (written < 0 || (size_t)written >= cap) return 0;
    return (size_t)written;
}

size_t kfb24_proto_fmt_cmd(char *buf, size_t cap, const char *name, uint32_t id) {
    if (!buf || cap == 0 || !name || !*name || strlen(name) >= 16) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') return 0;
    }
    return finish_line(cap,
                       snprintf(buf, cap, "%sCMD %s %lu\n", KFB24_PROTO_PREFIX,
                                name, (unsigned long)id));
}

size_t kfb24_proto_fmt_ctrl(char *buf, size_t cap, const char *name, uint32_t id) {
    if (!buf || cap == 0 || !name || !*name || strlen(name) >= 16) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') return 0;
    }
    return finish_line(cap,
                       snprintf(buf, cap, "%sCTRL %s %lu\n", KFB24_PROTO_PREFIX,
                                name, (unsigned long)id));
}

size_t kfb24_proto_fmt_hello(char *buf, size_t cap, const char *version) {
    if (!buf || cap == 0) return 0;
    return finish_line(cap,
                       snprintf(buf, cap, "%sHELLO %s\n", KFB24_PROTO_PREFIX,
                                (version && *version) ? version : "1"));
}

size_t kfb24_proto_fmt_ack(char *buf, size_t cap, uint32_t id, bool ok) {
    if (!buf || cap == 0) return 0;
    return finish_line(cap,
                       snprintf(buf, cap, "%sACK %lu %s\n", KFB24_PROTO_PREFIX,
                                (unsigned long)id, ok ? "OK" : "ERR"));
}

size_t kfb24_proto_fmt_state(char *buf, size_t cap, const char *transport,
                             bool up, const char *detail) {
    if (!buf || cap == 0 || !transport || !*transport) return 0;
    int written;
    if (detail && *detail) {
        written = snprintf(buf, cap, "%sSTATE %s %d %s\n", KFB24_PROTO_PREFIX,
                           transport, up ? 1 : 0, detail);
    } else {
        written = snprintf(buf, cap, "%sSTATE %s %d\n", KFB24_PROTO_PREFIX,
                           transport, up ? 1 : 0);
    }
    return finish_line(cap, written);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool kfb24_proto_hex_decode(const char *hex, char *out, size_t cap, size_t *out_len) {
    if (!hex || !out || cap == 0) return false;
    size_t len = strlen(hex);
    if ((len & 1u) != 0) return false;
    size_t decoded = len / 2;
    if (decoded + 1 > cap) return false;

    for (size_t i = 0; i < decoded; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (char)((hi << 4) | lo);
    }
    out[decoded] = '\0';
    if (out_len) *out_len = decoded;
    return true;
}

size_t kfb24_proto_hex_encode(const char *src, size_t src_len, char *out, size_t cap) {
    static const char digits[] = "0123456789abcdef";
    if (!src || !out || cap == 0) return 0;
    if (src_len > (cap - 1) / 2) return 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        out[i * 2] = digits[c >> 4];
        out[i * 2 + 1] = digits[c & 0x0F];
    }
    out[src_len * 2] = '\0';
    return src_len * 2;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool kfb24_proto_b64_decode(const char *src, size_t len, uint8_t *out,
                            size_t cap, size_t *out_len) {
    if (!src || !out || cap == 0) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    int pad = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad) return false;  // data after padding
        int v = b64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return false;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    if (len % 4 != 0) return false;
    if (pad > 2) return false;
    if (pad > 0 && bits != pad * 2) return false;
    if (out_len) *out_len = n;
    return true;
}
