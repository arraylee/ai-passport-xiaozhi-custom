"""Losslessly encode RGB565A8 in independently drawable four-line strips."""
import struct

WIDTH = HEIGHT = 144
ROWS = 4
STRIPS = HEIGHT // ROWS


def _runs(values, element_size):
    out = bytearray()
    literal = []

    def flush():
        if literal:
            out.append(len(literal) - 1)
            for value in literal:
                out.extend(value.to_bytes(element_size, "little"))
            literal.clear()

    at = 0
    while at < len(values):
        end = at + 1
        while end < len(values) and end - at < 128 and values[end] == values[at]:
            end += 1
        length = end - at
        if length >= 3:
            flush()
            out.append(0x80 | (length - 1))
            out.extend(values[at].to_bytes(element_size, "little"))
        else:
            for value in values[at:end]:
                literal.append(value)
                if len(literal) == 128:
                    flush()
        at = end
    flush()
    return out


def pack_face(raw):
    area = WIDTH * HEIGHT
    if len(raw) != area * 3:
        raise ValueError("Invalid RGB565A8 face size")
    result = bytearray(4 + 2 * (STRIPS + 1))
    result[:4] = b"XZF1"
    count = ROWS * WIDTH
    for strip in range(STRIPS):
        at = strip * count
        struct.pack_into("<H", result, 4 + 2 * strip, len(result))
        result.extend(_runs(struct.unpack_from(f"<{count}H", raw, 2 * at), 2))
        result.extend(_runs(raw[2 * area + at:2 * area + at + count], 1))
    struct.pack_into("<H", result, 4 + 2 * STRIPS, len(result))
    return bytes(result)
