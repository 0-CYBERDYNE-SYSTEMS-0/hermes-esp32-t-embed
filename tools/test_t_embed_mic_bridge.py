import binascii
import struct
import unittest

from t_embed_mic_bridge import FrameParser, PAYLOAD_BYTES, PACKET_BYTES, mono_to_stereo


def packet(sequence, sample):
    payload = struct.pack("<80h", *([sample] * 80))
    data = b"TMIC" + struct.pack("<I", sequence) + payload
    return data + struct.pack("<H", binascii.crc_hqx(data, 0xFFFF))


class FrameParserTests(unittest.TestCase):
    def test_finds_frames_across_partial_reads_and_ignores_text(self):
        parser = FrameParser()
        first = packet(7, 123)
        second = packet(8, -123)
        self.assertEqual(parser.feed(b"boot log\n" + first[:31]), [])
        frames = parser.feed(first[31:] + second)
        self.assertEqual([(sequence, len(payload)) for sequence, payload in frames],
                         [(7, PAYLOAD_BYTES), (8, PAYLOAD_BYTES)])

    def test_discards_a_misaligned_false_header(self):
        parser = FrameParser()
        first = packet(2, 0)
        false = b"TMIC" + struct.pack("<I", 999) + b"x" * (PACKET_BYTES - 8)
        second = packet(3, 0)
        parser.feed(first)
        frames = parser.feed(false + second)
        self.assertEqual([sequence for sequence, _ in frames], [3])

    def test_duplicates_mono_to_stereo(self):
        mono = struct.pack("<3h", -32768, 0, 32767)
        self.assertEqual(mono_to_stereo(mono), struct.pack("<6h", -32768, -32768, 0, 0, 32767, 32767))


if __name__ == "__main__":
    unittest.main()
