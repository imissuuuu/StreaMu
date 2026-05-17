from __future__ import annotations

from dataclasses import dataclass


_SAMPLE_RATES: tuple[int, ...] = (
    96000,
    88200,
    64000,
    48000,
    44100,
    32000,
    24000,
    22050,
    16000,
    12000,
    11025,
    8000,
    7350,
)


@dataclass(frozen=True)
class AacCodecConfig:
    profile: int
    sample_rate: int
    sample_rate_index: int
    channels: int
    timescale: int


@dataclass(frozen=True)
class Mp4Sample:
    size: int
    duration: int
    decode_time: int


@dataclass(frozen=True)
class Mp4SidxReference:
    offset: int
    size: int
    start_time: int
    duration: int
    timescale: int


@dataclass
class _TrackDefaults:
    sample_duration: int = 0
    sample_size: int = 0


def _read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated u32")
    return int.from_bytes(data[offset:offset + 4], "big")


def _read_u64(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 8 > len(data):
        raise ValueError("truncated u64")
    return int.from_bytes(data[offset:offset + 8], "big")


def _iter_boxes(data: bytes, start: int = 0, end: int | None = None) -> list[tuple[int, int, bytes, int, int]]:
    limit = len(data) if end is None else end
    boxes: list[tuple[int, int, bytes, int, int]] = []
    pos = start
    while pos + 8 <= limit:
        size = _read_u32(data, pos)
        box_type = data[pos + 4:pos + 8]
        header_size = 8
        if size == 1:
            if pos + 16 > limit:
                break
            size = int.from_bytes(data[pos + 8:pos + 16], "big")
            header_size = 16
        elif size == 0:
            size = limit - pos
        if size < header_size or pos + size > limit:
            break
        boxes.append((pos, size, box_type, pos + header_size, pos + size))
        pos += size
    return boxes


def split_mp4_init_and_sidx(data: bytes) -> tuple[bytes, list[Mp4SidxReference]]:
    init_end = 0
    for box_start, size, box_type, payload_start, payload_end in _iter_boxes(data):
        if box_type in (b"ftyp", b"moov", b"sidx"):
            init_end = box_start + size
            if box_type == b"sidx":
                references = _parse_sidx(
                    data[payload_start:payload_end],
                    box_start + size,
                )
                return data[:init_end], references
            continue
        break
    raise ValueError("MP4 sidx not found")


def _parse_sidx(payload: bytes, sidx_end_offset: int) -> list[Mp4SidxReference]:
    if len(payload) < 24:
        raise ValueError("truncated sidx")
    version = payload[0]
    pos = 4
    pos += 4
    timescale = _read_u32(payload, pos)
    pos += 4
    if timescale <= 0:
        raise ValueError("invalid sidx timescale")
    if version == 0:
        earliest_time = _read_u32(payload, pos)
        pos += 4
        first_offset = _read_u32(payload, pos)
        pos += 4
    elif version == 1:
        earliest_time = _read_u64(payload, pos)
        pos += 8
        first_offset = _read_u64(payload, pos)
        pos += 8
    else:
        raise ValueError("unsupported sidx version")
    if pos + 4 > len(payload):
        raise ValueError("truncated sidx references")
    pos += 2
    reference_count = int.from_bytes(payload[pos:pos + 2], "big")
    pos += 2

    references: list[Mp4SidxReference] = []
    segment_offset = sidx_end_offset + first_offset
    segment_time = earliest_time
    for _ in range(reference_count):
        if pos + 12 > len(payload):
            raise ValueError("truncated sidx reference")
        reference_type_and_size = _read_u32(payload, pos)
        pos += 4
        duration = _read_u32(payload, pos)
        pos += 4
        pos += 4
        reference_type = reference_type_and_size >> 31
        reference_size = reference_type_and_size & 0x7FFFFFFF
        if reference_type != 0:
            raise ValueError("unsupported nested sidx reference")
        if reference_size <= 0 or duration <= 0:
            raise ValueError("invalid sidx reference")
        references.append(Mp4SidxReference(
            offset=segment_offset,
            size=reference_size,
            start_time=segment_time,
            duration=duration,
            timescale=timescale,
        ))
        segment_offset += reference_size
        segment_time += duration
    if not references:
        raise ValueError("empty sidx")
    return references


def _descriptor_length(data: bytes, offset: int) -> tuple[int, int]:
    length = 0
    pos = offset
    for _ in range(4):
        if pos >= len(data):
            raise ValueError("incomplete descriptor length")
        value = data[pos]
        pos += 1
        length = (length << 7) | (value & 0x7F)
        if value & 0x80 == 0:
            return length, pos
    return length, pos


def _parse_audio_specific_config(config: bytes) -> AacCodecConfig:
    if len(config) < 2:
        raise ValueError("AAC config too short")
    audio_object_type = (config[0] >> 3) & 0x1F
    sample_rate_index = ((config[0] & 0x07) << 1) | (config[1] >> 7)
    channels = (config[1] >> 3) & 0x0F
    if audio_object_type <= 0:
        raise ValueError("invalid AAC object type")
    if sample_rate_index >= len(_SAMPLE_RATES):
        raise ValueError("unsupported AAC sample rate index")
    if channels not in (1, 2):
        raise ValueError("unsupported AAC channel count")
    return AacCodecConfig(
        profile=audio_object_type - 1,
        sample_rate=_SAMPLE_RATES[sample_rate_index],
        sample_rate_index=sample_rate_index,
        channels=channels,
        timescale=0,
    )


def _parse_esds_config(esds_payload: bytes) -> AacCodecConfig:
    pos = 4
    while pos < len(esds_payload):
        tag = esds_payload[pos]
        pos += 1
        desc_len, pos = _descriptor_length(esds_payload, pos)
        desc_end = pos + desc_len
        if desc_end > len(esds_payload):
            raise ValueError("truncated esds descriptor")
        if tag == 0x05:
            return _parse_audio_specific_config(esds_payload[pos:desc_end])
        if tag == 0x03:
            pos += 3
        elif tag == 0x04:
            pos += 13
        else:
            pos = desc_end
    raise ValueError("AAC config descriptor not found")


def _parse_mdhd_timescale(payload: bytes) -> int:
    if len(payload) < 16:
        raise ValueError("truncated mdhd")
    version = payload[0]
    if version == 1:
        if len(payload) < 24:
            raise ValueError("truncated mdhd v1")
        timescale = _read_u32(payload, 20)
    else:
        timescale = _read_u32(payload, 12)
    if timescale <= 0:
        raise ValueError("invalid mdhd timescale")
    return timescale


def _find_aac_config(data: bytes) -> AacCodecConfig:
    for _, _, box_type, payload_start, payload_end in _iter_boxes(data):
        payload = data[payload_start:payload_end]
        if box_type in (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"stsd"):
            try:
                if box_type == b"stsd":
                    payload = payload[8:]
                return _find_aac_config(payload)
            except ValueError:
                continue
        if box_type == b"mp4a":
            try:
                return _find_aac_config(payload[28:])
            except ValueError:
                continue
        if box_type == b"esds":
            return _parse_esds_config(payload)
    raise ValueError("AAC config not found")


def _find_first_mdhd_timescale(data: bytes) -> int:
    for _, _, box_type, payload_start, payload_end in _iter_boxes(data):
        payload = data[payload_start:payload_end]
        if box_type == b"mdhd":
            return _parse_mdhd_timescale(payload)
        if box_type in (b"moov", b"trak", b"mdia", b"minf", b"stbl"):
            try:
                return _find_first_mdhd_timescale(payload)
            except ValueError:
                continue
    raise ValueError("AAC timescale not found")


def _find_aac_config_and_timescale(data: bytes) -> AacCodecConfig:
    for _, _, box_type, payload_start, payload_end in _iter_boxes(data):
        payload = data[payload_start:payload_end]
        if box_type == b"trak":
            try:
                config = _find_aac_config(payload)
                timescale = _find_first_mdhd_timescale(payload)
                return AacCodecConfig(
                    profile=config.profile,
                    sample_rate=config.sample_rate,
                    sample_rate_index=config.sample_rate_index,
                    channels=config.channels,
                    timescale=timescale,
                )
            except ValueError:
                continue
        if box_type == b"moov":
            try:
                return _find_aac_config_and_timescale(payload)
            except ValueError:
                continue
    config = _find_aac_config(data)
    timescale = _find_first_mdhd_timescale(data)
    return AacCodecConfig(
        profile=config.profile,
        sample_rate=config.sample_rate,
        sample_rate_index=config.sample_rate_index,
        channels=config.channels,
        timescale=timescale,
    )


def _parse_tfhd(payload: bytes) -> _TrackDefaults:
    if len(payload) < 8:
        raise ValueError("truncated tfhd")
    flags = int.from_bytes(payload[1:4], "big")
    pos = 8
    if flags & 0x000001:
        pos += 8
    if flags & 0x000002:
        pos += 4
    defaults = _TrackDefaults()
    if flags & 0x000008:
        defaults.sample_duration = _read_u32(payload, pos)
        pos += 4
    if flags & 0x000010:
        defaults.sample_size = _read_u32(payload, pos)
        pos += 4
    return defaults


def _parse_tfdt_base_time(payload: bytes) -> int:
    if len(payload) < 8:
        raise ValueError("truncated tfdt")
    version = payload[0]
    if version == 1:
        if len(payload) < 12:
            raise ValueError("truncated tfdt v1")
        return _read_u64(payload, 4)
    return _read_u32(payload, 4)


def _parse_trun(payload: bytes, defaults: _TrackDefaults,
                base_decode_time: int) -> list[Mp4Sample]:
    if len(payload) < 8:
        raise ValueError("truncated trun")
    flags = int.from_bytes(payload[1:4], "big")
    sample_count = _read_u32(payload, 4)
    pos = 8
    if flags & 0x000001:
        pos += 4
    if flags & 0x000004:
        pos += 4
    samples: list[Mp4Sample] = []
    decode_time = base_decode_time
    for _ in range(sample_count):
        if flags & 0x000100:
            sample_duration = _read_u32(payload, pos)
            pos += 4
        else:
            sample_duration = defaults.sample_duration
        if flags & 0x000200:
            sample_size = _read_u32(payload, pos)
            pos += 4
        else:
            sample_size = defaults.sample_size
        if flags & 0x000400:
            pos += 4
        if flags & 0x000800:
            pos += 4
        if sample_size <= 0:
            raise ValueError("missing AAC sample size")
        if sample_duration <= 0:
            raise ValueError("missing AAC sample duration")
        samples.append(Mp4Sample(
            size=sample_size,
            duration=sample_duration,
            decode_time=decode_time,
        ))
        decode_time += sample_duration
    return samples


def _parse_moof_samples(moof: bytes) -> list[Mp4Sample]:
    samples: list[Mp4Sample] = []
    for _, _, box_type, payload_start, payload_end in _iter_boxes(moof):
        if box_type != b"traf":
            continue
        defaults = _TrackDefaults()
        base_decode_time: int | None = None
        traf = moof[payload_start:payload_end]
        for _, _, child_type, child_start, child_end in _iter_boxes(traf):
            payload = traf[child_start:child_end]
            if child_type == b"tfhd":
                defaults = _parse_tfhd(payload)
            elif child_type == b"tfdt":
                base_decode_time = _parse_tfdt_base_time(payload)
            elif child_type == b"trun":
                if base_decode_time is None:
                    raise ValueError("AAC tfdt not found")
                samples.extend(_parse_trun(payload, defaults, base_decode_time))
    if not samples:
        raise ValueError("AAC samples not found")
    return samples


def _adts_header(config: AacCodecConfig, payload_size: int) -> bytes:
    frame_length = payload_size + 7
    header = bytearray(7)
    header[0] = 0xFF
    header[1] = 0xF1
    header[2] = ((config.profile & 0x03) << 6) | ((config.sample_rate_index & 0x0F) << 2) | ((config.channels >> 2) & 0x01)
    header[3] = ((config.channels & 0x03) << 6) | ((frame_length >> 11) & 0x03)
    header[4] = (frame_length >> 3) & 0xFF
    header[5] = ((frame_length & 0x07) << 5) | 0x1F
    header[6] = 0xFC
    return bytes(header)


class Mp4AacTransmuxer:
    def __init__(self, start_seconds: int = 0) -> None:
        self._buffer = bytearray()
        self._config: AacCodecConfig | None = None
        self._samples: list[Mp4Sample] = []
        self._start_seconds = max(0, start_seconds)
        self._target_time: int | None = None

    def feed(self, chunk: bytes) -> list[bytes]:
        if chunk:
            self._buffer.extend(chunk)
        frames: list[bytes] = []
        while len(self._buffer) >= 8:
            size = _read_u32(self._buffer, 0)
            header_size = 8
            if size == 1:
                if len(self._buffer) < 16:
                    break
                size = int.from_bytes(self._buffer[8:16], "big")
                header_size = 16
            if size < header_size:
                raise ValueError("invalid MP4 box size")
            if len(self._buffer) < size:
                break

            box = bytes(self._buffer[:size])
            box_type = box[4:8]
            payload = box[header_size:]
            del self._buffer[:size]

            if box_type == b"moov":
                self._config = _find_aac_config_and_timescale(payload)
                self._target_time = self._start_seconds * self._config.timescale
            elif box_type == b"moof":
                self._samples = _parse_moof_samples(payload)
            elif box_type == b"mdat":
                if self._config is None:
                    raise ValueError("mdat arrived before AAC config")
                offset = 0
                target_time = self._target_time or 0
                for sample in self._samples:
                    if offset + sample.size > len(payload):
                        raise ValueError("mdat shorter than sample table")
                    sample_payload = payload[offset:offset + sample.size]
                    if sample.decode_time + sample.duration > target_time:
                        frames.append(_adts_header(self._config, len(sample_payload)) + sample_payload)
                    offset += sample.size
                self._samples = []
            elif box_type in (b"ftyp", b"sidx", b"free", b"skip", b"mfra"):
                continue
            else:
                continue
        return frames

    def flush(self) -> list[bytes]:
        if self._buffer:
            raise ValueError("incomplete MP4 input")
        return []
