# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded readers for compact scalar motion tracks (not a skeletal IK solver)."""
from dataclasses import dataclass
import math
import struct


def unpack(data, offset, fmt):
    size = struct.calcsize('<' + fmt)
    if offset < 0 or offset + size > len(data):
        raise ValueError('Record outside input')
    return struct.unpack_from('<' + fmt, data, offset)


def float_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def decode_value(header, payload):
    """20-bit value: sign, three exponent bits, sixteen mantissa bits."""
    exponent = (header >> 12) & 7
    sign = (header & 0x8000) << 16
    if not exponent:
        return (-1 if sign else 1) * payload / 65536
    return float_bits(sign | ((exponent + 126) << 23) | (payload << 7))


def decode_tangent(bits):
    """24-bit derivative, including the second small-number exponent tier."""
    sign = (bits & 0x800000) << 8
    exponent = (bits >> 19) & 15
    mantissa = bits & 0x7ffff
    if exponent:
        return float_bits(sign | ((exponent + 112) << 23) | (mantissa << 4))
    if not mantissa:
        return 0.0
    return float_bits(sign | (((mantissa >> 16) + 105) << 23)
                      | ((mantissa & 65535) << 7))


@dataclass(frozen=True)
class Segment:
    start: int
    duration: int
    value: float
    end_value: float
    slope: float = 0.0
    end_slope: float = 0.0
    linear: bool = False

    def sample(self, tick):
        if not self.duration:
            return self.value
        x = min(max(tick - self.start, 0), self.duration)
        secant = (self.end_value - self.value) / self.duration
        if self.linear:
            return self.value + x * secant
        quadratic = (3 * secant - 2 * self.slope - self.end_slope) / self.duration
        cubic = (self.slope + self.end_slope - 2 * secant) / self.duration**2
        return ((cubic * x + quadratic) * x + self.slope) * x + self.value


def parse_track(data, offset, end):
    if not 0 <= offset < end <= len(data):
        raise ValueError('Invalid track span')
    data = memoryview(data)[:end]
    segments, tick = [], 0
    while offset < end:
        header, payload = unpack(data, offset, 'HH')
        duration, kind = (header >> 2) & 1023, header & 3
        value = decode_value(header, payload)
        if not duration:
            segments.append(Segment(tick, 0, value, value))
            return segments
        if kind not in (0, 1):
            raise ValueError(f'Unsupported segment form {kind}')
        step = 4 if kind == 1 else 10
        next_header, next_payload = unpack(data, offset + step, 'HH')
        slope = end_slope = 0.0
        if kind == 0:
            first, second, high = unpack(data, offset + 4, 'HHH')
            slope = decode_tangent(first | ((high & 255) << 16))
            end_slope = decode_tangent(second | ((high >> 8) << 16))
        segments.append(Segment(tick, duration, value,
                                decode_value(next_header, next_payload),
                                slope, end_slope, kind == 1))
        offset += step
        tick += duration
    raise ValueError('Missing track terminal')


@dataclass(frozen=True)
class Clip:
    count: int
    tracks: tuple

    def sample(self, tick):
        """Sample an integer pose. Fractional game blending is a separate stage."""
        if not isinstance(tick, int) or not 0 <= tick < self.count:
            raise ValueError('Tick must be an integer within the clip')
        result = []
        for track in self.tracks:
            if track is None:
                result.append(None)
                continue
            segment = next((s for s in track if tick < s.start + s.duration), track[-1])
            value = segment.sample(tick)
            if not math.isfinite(value):
                raise ValueError('Nonfinite animation sample')
            result.append(value)
        return result


def parse_clip(bank, slot):
    if not isinstance(slot, int) or not 0 <= slot < 1000:
        raise ValueError('Clip slot outside bank')
    offsets = unpack(bank, 0, '1001I')
    if offsets[-1] != 0:
        raise ValueError('Missing bank table sentinel')
    if any(o and not 4004 <= o <= len(bank) - 4 for o in offsets):
        raise ValueError('Clip outside bank payload')
    start = offsets[slot]
    if not start:
        raise ValueError('Absent clip')
    end = min((o for o in offsets if o > start), default=len(bank))
    data = memoryview(bank)[start:end]
    count, channels = unpack(data, 0, 'HH')
    if not count or channels != 60:
        raise ValueError('Unsupported clip header')
    tracks = unpack(data, 4, '60I')
    if any(o and not 244 <= o <= len(data) - 4 for o in tracks):
        raise ValueError('Track outside clip payload')
    boundaries = sorted(set(tracks) - {0}) + [len(data)]
    ends = dict(zip(boundaries, boundaries[1:]))
    parsed = {o: parse_track(data, o, ends[o]) for o in boundaries[:-1]}
    return Clip(count, tuple(parsed[o] if o else None for o in tracks))
