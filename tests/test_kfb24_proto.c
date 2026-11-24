// Host tests for the kfb24 line protocol: no ESP-IDF or sockets required.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kfb24_proto.h"

static void test_parse_cmd(void) {
    kfb24_msg_t msg;
    assert(kfb24_proto_parse("KFB24 CMD voice 42", &msg));
    assert(msg.type == KFB24_MSG_CMD);
    assert(msg.id == 42);
    assert(strcmp(msg.name, "voice") == 0);

    assert(kfb24_proto_parse("KFB24 CMD terminal 4294967295\r\n", &msg));
    assert(msg.id == 4294967295u);

    // Logs, wrong prefixes, extra tokens, and oversized ids are rejected.
    assert(!kfb24_proto_parse("I (123) kfb24: boot", &msg));
    assert(!kfb24_proto_parse("KFB24 CMD voice", &msg));
    assert(!kfb24_proto_parse("KFB24 CMD voice 1 extra", &msg));
    assert(!kfb24_proto_parse("KFB24 CMD voice 4294967296", &msg));
    assert(!kfb24_proto_parse("KFB24 CMD voice -1", &msg));
    assert(!kfb24_proto_parse("KFB24 CMD 0123456789abcdef 1", &msg));
    assert(!kfb24_proto_parse(NULL, &msg));
}

static void test_parse_hello_ack_state(void) {
    kfb24_msg_t msg;
    assert(kfb24_proto_parse("KFB24 HELLO bridge/1.0.0", &msg));
    assert(msg.type == KFB24_MSG_HELLO);
    assert(strcmp(msg.text, "bridge/1.0.0") == 0);

    assert(kfb24_proto_parse("KFB24 HELLO", &msg));
    assert(msg.type == KFB24_MSG_HELLO);
    assert(msg.text[0] == '\0');

    assert(kfb24_proto_parse("KFB24 ACK 7 OK", &msg));
    assert(msg.type == KFB24_MSG_ACK && msg.id == 7 && msg.ok);
    assert(kfb24_proto_parse("KFB24 ACK 8 ERR", &msg));
    assert(msg.type == KFB24_MSG_ACK && msg.id == 8 && !msg.ok);
    assert(!kfb24_proto_parse("KFB24 ACK 8 MAYBE", &msg));

    assert(kfb24_proto_parse("KFB24 STATE usb 1 bridge", &msg));
    assert(msg.type == KFB24_MSG_STATE);
    assert(strcmp(msg.text, "usb") == 0);
    assert(msg.ok);
    assert(kfb24_proto_parse("KFB24 STATE wifi 0", &msg));
    assert(msg.type == KFB24_MSG_STATE && !msg.ok);
}

static void test_parse_cfg(void) {
    kfb24_msg_t msg;
    // "家的 Wi-Fi" is 10 UTF-8 bytes, "p@ss" is 4.
    const char *ssid_hex = "e5aeb6e79a842057692d4669";
    assert(kfb24_proto_parse("KFB24 CFG 3 e5aeb6e79a842057692d4669 70407373 192.168.1.8 8765",
                             &msg));
    assert(msg.type == KFB24_MSG_CFG);
    assert(msg.id == 3);
    assert(strcmp(msg.ssid, "家的 Wi-Fi") == 0);
    assert(strcmp(msg.pass, "p@ss") == 0);
    assert(strcmp(msg.host, "192.168.1.8") == 0);
    assert(msg.port == 8765);

    (void)ssid_hex;
    assert(kfb24_proto_parse("KFB24 CFG 4 - - - 8765", &msg));
    assert(msg.type == KFB24_MSG_CFG);
    assert(msg.ssid[0] == '\0' && msg.pass[0] == '\0' && msg.host[0] == '\0');

    assert(!kfb24_proto_parse("KFB24 CFG 4 123 - - 8765", &msg));  // odd hex
    assert(!kfb24_proto_parse("KFB24 CFG 4 zz - - 8765", &msg));   // bad hex
    assert(!kfb24_proto_parse("KFB24 CFG 4 - - - 0", &msg));       // port 0
    assert(!kfb24_proto_parse("KFB24 CFG 4 - - - 65536", &msg));   // port overflow
}

static void test_hex_helpers(void) {
    char encoded[16];
    char decoded[16];
    size_t length = 0;
    assert(kfb24_proto_hex_encode("A\x00\xff", 3, encoded, sizeof(encoded)) == 6);
    assert(strcmp(encoded, "4100ff") == 0);
    assert(kfb24_proto_hex_decode(encoded, decoded, sizeof(decoded), &length));
    assert(length == 3);
    assert(decoded[0] == 'A' && decoded[1] == '\0' && (unsigned char)decoded[2] == 0xff);

    // Destination must hold the decoded bytes plus a terminator.
    assert(!kfb24_proto_hex_decode("4142", decoded, 2, &length));
    // Six bytes hold two decoded bytes plus the terminator, not three.
    assert(!kfb24_proto_hex_encode("abc", 3, encoded, 6));
    assert(kfb24_proto_hex_encode("abc", 3, encoded, 7) == 6);
    assert(strcmp(encoded, "616263") == 0);
}

static void test_formatters(void) {
    char line[KFB24_PROTO_MAX_LINE];
    size_t length = kfb24_proto_fmt_cmd(line, sizeof(line), "wechat", 9);
    assert(length == strlen("KFB24 CMD wechat 9\n"));
    assert(strcmp(line, "KFB24 CMD wechat 9\n") == 0);

    length = kfb24_proto_fmt_hello(line, sizeof(line), "kfb24/1.0.0");
    assert(length == strlen("KFB24 HELLO kfb24/1.0.0\n"));
    assert(kfb24_proto_fmt_hello(line, 10, "kfb24/1.0.0") == 0);

    length = kfb24_proto_fmt_ack(line, sizeof(line), 12, false);
    assert(length == strlen("KFB24 ACK 12 ERR\n"));
    assert(strcmp(line, "KFB24 ACK 12 ERR\n") == 0);

    length = kfb24_proto_fmt_state(line, sizeof(line), "wifi", true, "192.168.1.8");
    assert(length == strlen("KFB24 STATE wifi 1 192.168.1.8\n"));
    assert(kfb24_proto_fmt_state(line, sizeof(line), "wifi", false, NULL) ==
           strlen("KFB24 STATE wifi 0\n"));
    assert(kfb24_proto_fmt_cmd(line, sizeof(line), "bad name", 1) == 0);
    assert(kfb24_proto_fmt_cmd(line, sizeof(line), "", 1) == 0);
}

int main(void) {
    test_parse_cmd();
    test_parse_hello_ack_state();
    test_parse_cfg();
    test_hex_helpers();
    test_formatters();
    puts("kfb24 protocol tests: PASS");
    return 0;
}
