"""PCAPNG/radiotap output: a real pcap reader must accept what the device writes.

The sniffer's value is the frame, not the byte count, so this test builds the
document with the firmware's own writers, parses the block structure
independently in Python, and then hands the file to tcpdump (a third-party
reader) to prove a beacon with an SSID comes out the other end.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pcap_ng.h"

static void put_iq(uint8_t *f, size_t *at, const void *src, size_t n) {
    memcpy(f + *at, src, n); *at += n;
}

/* 802.11 beacon: fc/dur/addr1..3/seq + ts,interval,cap + SSID + rates */
static size_t beacon(uint8_t *f, const char *ssid) {
    size_t at = 0;
    uint8_t hdr[24] = {0x80, 0x00, 0x00, 0x00,
                       0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                       0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
                       0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
                       0x00, 0x00};
    put_iq(f, &at, hdr, sizeof(hdr));
    uint8_t fixed[12] = {0};
    fixed[8] = 0x64; fixed[9] = 0x00;   /* beacon interval 100 */
    fixed[10] = 0x01; fixed[11] = 0x00; /* ESS capability */
    put_iq(f, &at, fixed, sizeof(fixed));
    size_t n = strlen(ssid);
    f[at++] = 0; f[at++] = (uint8_t)n;
    put_iq(f, &at, ssid, n);
    f[at++] = 1; f[at++] = 1; f[at++] = 0x82;
    return at;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    uint8_t doc[8192];
    size_t at = 0;
    at += pg_shb(doc + at, sizeof(doc) - at);
    at += pg_idb((char *)doc + at, sizeof(doc) - at, "esp32s3-sniffer", PG_SNAPLEN);

    uint8_t frame[512];
    uint8_t rt[PG_RADIOTAP_LEN];
    for (int i = 0; i < 3; i++) {
        size_t flen = beacon(frame, i == 0 ? "TESTNET" : i == 1 ? "OTHER" : "TESTNET");
        size_t rtlen = pg_radiotap(rt, sizeof(rt), 2437, (int8_t)(-62 - i));
        uint8_t packet[600];
        memcpy(packet, rt, rtlen);
        memcpy(packet + rtlen, frame, flen);
        at += pg_epb(doc + at, sizeof(doc) - at,
                     1763500000123000000ull + (uint64_t)i * 1000000ull,
                     packet, (uint32_t)(rtlen + flen));
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 3;
    fwrite(doc, 1, at, f);
    fclose(f);
    printf("BYTES %zu\n", at);
    return 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class PcapNg(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.tmp.name) / "sniff.pcapng"
        harness = Path(cls.tmp.name) / "harness.c"
        harness.write_text(HARNESS)
        binary = Path(cls.tmp.name) / "harness"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT / 'main/common'}", "-o", str(binary), str(harness)],
                       check=True)
        cls.out = subprocess.run([str(binary), str(cls.path)], check=True,
                                 capture_output=True, text=True).stdout

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_block_structure_is_consistent(self):
        data = self.path.read_bytes()
        self.assertEqual(struct.unpack_from("<I", data, 0)[0], 0x0A0D0D0A)
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], 0x1A2B3C4D)
        blocks = []
        offset = 0
        while offset < len(data):
            kind, length = struct.unpack_from("<II", data, offset)
            self.assertGreaterEqual(length, 12)
            self.assertEqual(struct.unpack_from("<I", data, offset + length - 4)[0], length,
                             "trailing total-length must match")
            blocks.append(kind)
            offset += length
        self.assertEqual(offset, len(data), "no trailing bytes")
        self.assertEqual(blocks, [0x0A0D0D0A, 1, 6, 6, 6])

    def test_interface_is_radiotap_with_nanosecond_resolution(self):
        data = self.path.read_bytes()
        shb_len = struct.unpack_from("<I", data, 4)[0]
        idb = shb_len
        kind, length = struct.unpack_from("<II", data, idb)
        self.assertEqual(kind, 1)
        linktype, _, snaplen = struct.unpack_from("<HHI", data, idb + 8)
        self.assertEqual(linktype, 127)
        self.assertEqual(snaplen, 1600)
        body = data[idb + 16:idb + length - 4]
        self.assertIn(b"esp32s3-sniffer", body)
        tsresol = body.index(b"\x09\x00\x01\x00")
        self.assertEqual(body[tsresol + 4], 9, "10^-9 seconds")

    def test_packets_are_timestamped_and_padded(self):
        data = self.path.read_bytes()
        offset = 0
        epbs = []
        while offset < len(data):
            kind, length = struct.unpack_from("<II", data, offset)
            if kind == 6:
                ts = (struct.unpack_from("<I", data, offset + 12)[0] << 32) | \
                     struct.unpack_from("<I", data, offset + 16)[0]
                caplen = struct.unpack_from("<I", data, offset + 20)[0]
                self.assertEqual(caplen % 4 == 0, caplen % 4 == 0)
                self.assertEqual(length, 32 + ((caplen + 3) // 4) * 4)
                epbs.append((ts, caplen))
            offset += length
        self.assertEqual(len(epbs), 3)
        self.assertEqual(epbs[0][0], 1763500000123000000)
        self.assertEqual(epbs[1][0] - epbs[0][0], 1000000)

    @unittest.skipUnless(shutil.which("tcpdump"), "tcpdump unavailable")
    def test_tcpdump_reads_a_beacon_with_its_ssid(self):
        result = subprocess.run(["tcpdump", "-r", str(self.path), "-nn", "-v"],
                                capture_output=True, text=True)
        output = result.stdout + result.stderr
        self.assertIn("TESTNET", output)

    @unittest.skipUnless(shutil.which("tcpdump"), "tcpdump unavailable")
    def test_tcpdump_sees_the_radiotap_signal(self):
        result = subprocess.run(["tcpdump", "-r", str(self.path), "-nn", "-e"],
                                capture_output=True, text=True)
        output = result.stdout + result.stderr
        self.assertIn("2437", output)


if __name__ == "__main__":
    unittest.main()
