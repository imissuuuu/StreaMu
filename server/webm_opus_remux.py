from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterator


ID_EBML = 0x1A45DFA3
ID_SEGMENT = 0x18538067
ID_INFO = 0x1549A966
ID_TIMECODE_SCALE = 0x2AD7B1
ID_TRACKS = 0x1654AE6B
ID_TRACK_ENTRY = 0xAE
ID_TRACK_NUMBER = 0xD7
ID_TRACK_TYPE = 0x83
ID_CODEC_ID = 0x86
ID_CODEC_PRIVATE = 0x63A2
ID_CODEC_DELAY = 0x56AA
ID_SEEK_PRE_ROLL = 0x56BB
ID_AUDIO = 0xE1
ID_SAMPLING_FREQUENCY = 0xB5
ID_CHANNELS = 0x9F
ID_CLUSTER = 0x1F43B675
ID_TIMESTAMP = 0xE7
ID_SIMPLE_BLOCK = 0xA3
ID_BLOCK_GROUP = 0xA0
ID_BLOCK = 0xA1
ID_DISCARD_PADDING = 0x75A2

TRACK_TYPE_AUDIO = 2
DEFAULT_TIMECODE_SCALE_NS = 1_000_000
OGG_SERIAL = 0x5354524D
MAX_EBML_ID_BYTES = 4
MAX_EBML_SIZE_BYTES = 8


@dataclass(frozen=True)
class WebmOpusPacket:
    data: bytes
    timecode_ms: int
    discard_padding_ns: int = 0


@dataclass(frozen=True)
class WebmOpusTrack:
    codec_private: bytes
    channels: int
    sample_rate: float
    packets: list[WebmOpusPacket]
    codec_delay_ns: int = 0
    seek_pre_roll_ns: int = 0
    timecode_scale_ns: int = DEFAULT_TIMECODE_SCALE_NS


@dataclass(frozen=True)
class EbmlElement:
    element_id: int
    header_start: int
    data_start: int
    data_end: int


@dataclass
class _TrackCandidate:
    track_number: int = 0
    track_type: int = 0
    codec_id: str = ""
    codec_private: bytes = b""
    channels: int = 2
    sample_rate: float = 48000.0
    codec_delay_ns: int = 0
    seek_pre_roll_ns: int = 0


class WebmOpusRemuxError(ValueError):
    pass


def _read_vint(data: bytes, offset: int, max_bytes: int,
               mask_marker: bool) -> tuple[int, int]:
    if offset >= len(data):
        raise WebmOpusRemuxError("truncated EBML vint")
    first = data[offset]
    mask = 0x80
    width = 1
    while width <= max_bytes and (first & mask) == 0:
        mask >>= 1
        width += 1
    if width > max_bytes or mask == 0:
        raise WebmOpusRemuxError("invalid EBML vint")
    if offset + width > len(data):
        raise WebmOpusRemuxError("truncated EBML vint payload")

    value = first if not mask_marker else first & (mask - 1)
    for index in range(1, width):
        value = (value << 8) | data[offset + index]
    return value, width


def _iter_elements(data: bytes, start: int, end: int) -> Iterator[EbmlElement]:
    offset = start
    while offset < end:
        element_id, id_len = _read_vint(data, offset, MAX_EBML_ID_BYTES, False)
        size, size_len = _read_vint(
            data, offset + id_len, MAX_EBML_SIZE_BYTES, True
        )
        data_start = offset + id_len + size_len
        data_end = data_start + size
        if data_end > end:
            raise WebmOpusRemuxError("EBML element extends past parent")
        yield EbmlElement(element_id, offset, data_start, data_end)
        offset = data_end


def _read_uint(payload: bytes) -> int:
    if not payload:
        return 0
    if len(payload) > 8:
        raise WebmOpusRemuxError("integer payload too large")
    return int.from_bytes(payload, "big")


def _read_sint(payload: bytes) -> int:
    if not payload:
        return 0
    if len(payload) > 8:
        raise WebmOpusRemuxError("signed integer payload too large")
    return int.from_bytes(payload, "big", signed=True)


def _read_float(payload: bytes) -> float:
    if len(payload) == 4:
        return float(struct.unpack(">f", payload)[0])
    if len(payload) == 8:
        return float(struct.unpack(">d", payload)[0])
    raise WebmOpusRemuxError("unsupported float width")


def _read_ascii(payload: bytes) -> str:
    try:
        return payload.decode("ascii")
    except UnicodeDecodeError as exc:
        raise WebmOpusRemuxError("invalid ASCII string") from exc


def _parse_audio(data: bytes, start: int, end: int,
                 track: _TrackCandidate) -> None:
    for element in _iter_elements(data, start, end):
        payload = data[element.data_start:element.data_end]
        if element.element_id == ID_SAMPLING_FREQUENCY:
            track.sample_rate = _read_float(payload)
        elif element.element_id == ID_CHANNELS:
            track.channels = _read_uint(payload)


def _parse_track_entry(data: bytes, start: int, end: int) -> _TrackCandidate:
    track = _TrackCandidate()
    for element in _iter_elements(data, start, end):
        payload = data[element.data_start:element.data_end]
        if element.element_id == ID_TRACK_NUMBER:
            track.track_number = _read_uint(payload)
        elif element.element_id == ID_TRACK_TYPE:
            track.track_type = _read_uint(payload)
        elif element.element_id == ID_CODEC_ID:
            track.codec_id = _read_ascii(payload)
        elif element.element_id == ID_CODEC_PRIVATE:
            track.codec_private = payload
        elif element.element_id == ID_CODEC_DELAY:
            track.codec_delay_ns = _read_uint(payload)
        elif element.element_id == ID_SEEK_PRE_ROLL:
            track.seek_pre_roll_ns = _read_uint(payload)
        elif element.element_id == ID_AUDIO:
            _parse_audio(data, element.data_start, element.data_end, track)
    return track


def _find_opus_track(data: bytes, start: int,
                     end: int) -> _TrackCandidate:
    candidates: list[_TrackCandidate] = []
    for element in _iter_elements(data, start, end):
        if element.element_id != ID_TRACK_ENTRY:
            continue
        track = _parse_track_entry(data, element.data_start, element.data_end)
        if track.track_type == TRACK_TYPE_AUDIO and track.codec_id == "A_OPUS":
            candidates.append(track)

    if len(candidates) != 1:
        raise WebmOpusRemuxError("expected exactly one A_OPUS audio track")
    track = candidates[0]
    if track.track_number <= 0:
        raise WebmOpusRemuxError("missing Opus track number")
    if not track.codec_private.startswith(b"OpusHead"):
        raise WebmOpusRemuxError("missing OpusHead codec private")
    if track.channels not in (1, 2):
        raise WebmOpusRemuxError("unsupported Opus channel count")
    return track


def _parse_simple_block(payload: bytes,
                        expected_track: int) -> tuple[int, bytes]:
    track_number, track_len = _read_vint(payload, 0, MAX_EBML_SIZE_BYTES, True)
    if track_number != expected_track:
        raise WebmOpusRemuxError("unexpected track in SimpleBlock")
    if track_len + 3 > len(payload):
        raise WebmOpusRemuxError("truncated SimpleBlock")
    rel_time = int.from_bytes(
        payload[track_len:track_len + 2], "big", signed=True
    )
    flags = payload[track_len + 2]
    if flags & 0x06:
        raise WebmOpusRemuxError("laced SimpleBlock is unsupported")
    return rel_time, payload[track_len + 3:]


def _parse_block_group(data: bytes, start: int, end: int,
                       expected_track: int) -> tuple[int, bytes, int]:
    block_time = 0
    block_data = b""
    discard_padding_ns = 0
    for element in _iter_elements(data, start, end):
        payload = data[element.data_start:element.data_end]
        if element.element_id == ID_BLOCK:
            block_time, block_data = _parse_simple_block(payload, expected_track)
        elif element.element_id == ID_DISCARD_PADDING:
            discard_padding_ns = _read_sint(payload)
        else:
            raise WebmOpusRemuxError("unsupported BlockGroup child")
    if not block_data:
        raise WebmOpusRemuxError("BlockGroup without Block")
    return block_time, block_data, discard_padding_ns


def _parse_cluster(data: bytes, start: int, end: int,
                   expected_track: int) -> list[WebmOpusPacket]:
    cluster_time_ms = 0
    packets: list[WebmOpusPacket] = []
    for element in _iter_elements(data, start, end):
        payload = data[element.data_start:element.data_end]
        if element.element_id == ID_TIMESTAMP:
            cluster_time_ms = _read_uint(payload)
        elif element.element_id == ID_SIMPLE_BLOCK:
            rel_time, packet = _parse_simple_block(payload, expected_track)
            packets.append(WebmOpusPacket(packet, cluster_time_ms + rel_time))
        elif element.element_id == ID_BLOCK_GROUP:
            rel_time, packet, discard_padding_ns = _parse_block_group(
                data, element.data_start, element.data_end, expected_track
            )
            packets.append(
                WebmOpusPacket(
                    packet,
                    cluster_time_ms + rel_time,
                    discard_padding_ns,
                )
            )
    return packets


def _find_segment(data: bytes) -> EbmlElement:
    for element in _iter_elements(data, 0, len(data)):
        if element.element_id == ID_SEGMENT:
            return element
    raise WebmOpusRemuxError("Segment not found")


def extract_webm_opus(data: bytes) -> WebmOpusTrack:
    segment = _find_segment(data)
    timecode_scale_ns = DEFAULT_TIMECODE_SCALE_NS
    opus_track: _TrackCandidate | None = None
    packets: list[WebmOpusPacket] = []

    for element in _iter_elements(data, segment.data_start, segment.data_end):
        if element.element_id == ID_INFO:
            for child in _iter_elements(data, element.data_start, element.data_end):
                if child.element_id == ID_TIMECODE_SCALE:
                    payload = data[child.data_start:child.data_end]
                    timecode_scale_ns = _read_uint(payload)
        elif element.element_id == ID_TRACKS:
            opus_track = _find_opus_track(data, element.data_start, element.data_end)
        elif element.element_id == ID_CLUSTER:
            if opus_track is None:
                raise WebmOpusRemuxError("Cluster before Tracks")
            packets.extend(
                _parse_cluster(
                    data, element.data_start, element.data_end,
                    opus_track.track_number
                )
            )

    if opus_track is None:
        raise WebmOpusRemuxError("Opus track not found")
    if not packets:
        raise WebmOpusRemuxError("Opus packets not found")

    return WebmOpusTrack(
        codec_private=opus_track.codec_private,
        channels=opus_track.channels,
        sample_rate=opus_track.sample_rate,
        packets=packets,
        codec_delay_ns=opus_track.codec_delay_ns,
        seek_pre_roll_ns=opus_track.seek_pre_roll_ns,
        timecode_scale_ns=timecode_scale_ns,
    )


def opus_packet_duration_samples(packet: bytes) -> int:
    if not packet:
        raise WebmOpusRemuxError("empty Opus packet")
    config = packet[0] >> 3
    frame_count_code = packet[0] & 0x03

    if config < 12:
        base_samples = 480 if (config & 0x03) == 0 else 960
        if (config & 0x03) == 2:
            base_samples = 1920
        elif (config & 0x03) == 3:
            base_samples = 2880
    elif config < 16:
        base_samples = 480 if (config & 0x01) == 0 else 960
    else:
        base_samples = 120 << (config & 0x03)

    if frame_count_code == 0:
        frames = 1
    elif frame_count_code in (1, 2):
        frames = 2
    else:
        if len(packet) < 2:
            raise WebmOpusRemuxError("truncated Opus code 3 packet")
        frames = packet[1] & 0x3F
        if frames <= 0:
            raise WebmOpusRemuxError("invalid Opus frame count")

    duration = base_samples * frames
    if duration <= 0 or duration > 5760:
        raise WebmOpusRemuxError("invalid Opus packet duration")
    return duration


def _ogg_crc_entry(index: int) -> int:
    value = index << 24
    for _ in range(8):
        if value & 0x80000000:
            value = ((value << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
        else:
            value = (value << 1) & 0xFFFFFFFF
    return value


_OGG_CRC_TABLE: tuple[int, ...] = tuple(
    _ogg_crc_entry(index) for index in range(256)
)


def _ogg_crc(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ _OGG_CRC_TABLE[((crc >> 24) & 0xFF) ^ byte]
    return crc


def _packet_segments(packet: bytes) -> bytes:
    full_segments, last_segment = divmod(len(packet), 255)
    segments = bytearray([255] * full_segments)
    segments.append(last_segment)
    return bytes(segments)


def _ogg_page(packet: bytes, header_type: int, granule_position: int,
              sequence: int) -> bytes:
    segments = _packet_segments(packet)
    if len(segments) > 255:
        raise WebmOpusRemuxError("packet too large for single Ogg page")
    header = bytearray()
    header.extend(b"OggS")
    header.append(0)
    header.append(header_type)
    header.extend(int(granule_position).to_bytes(8, "little", signed=True))
    header.extend(OGG_SERIAL.to_bytes(4, "little"))
    header.extend(sequence.to_bytes(4, "little"))
    header.extend((0).to_bytes(4, "little"))
    header.append(len(segments))
    header.extend(segments)
    page = bytes(header) + packet
    crc = _ogg_crc(page)
    return page[:22] + crc.to_bytes(4, "little") + page[26:]


def _opus_tags(vendor: bytes) -> bytes:
    return b"OpusTags" + len(vendor).to_bytes(4, "little") + vendor + (0).to_bytes(4, "little")


def build_ogg_opus(track: WebmOpusTrack,
                   vendor: bytes = b"StreaMu") -> bytes:
    if not track.codec_private.startswith(b"OpusHead"):
        raise WebmOpusRemuxError("invalid OpusHead")

    pages: list[bytes] = []
    sequence = 0
    pages.append(_ogg_page(track.codec_private, 0x02, 0, sequence))
    sequence += 1
    pages.append(_ogg_page(_opus_tags(vendor), 0x00, 0, sequence))
    sequence += 1

    granule_position = 0
    for index, packet in enumerate(track.packets):
        granule_position += opus_packet_duration_samples(packet.data)
        header_type = 0x04 if index == len(track.packets) - 1 else 0x00
        pages.append(_ogg_page(packet.data, header_type, granule_position, sequence))
        sequence += 1

    return b"".join(pages)


def remux_webm_opus_to_ogg(data: bytes) -> bytes:
    return build_ogg_opus(extract_webm_opus(data))
