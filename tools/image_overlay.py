"""Apply verified project IPS overlays to an in-memory compiler image."""
from pathlib import Path
import hashlib

MAX_IMAGE_SIZE = 16 * 1024 * 1024


def apply_ips(image: bytes, patch: bytes, *, writes=None) -> bytes:
    if patch[:5] != b'PATCH':
        raise ValueError('image overlay is not IPS')
    output = bytearray(image)
    if writes is None:
        writes = {}
    cursor = 5
    while True:
        if cursor + 3 > len(patch):
            raise ValueError('truncated IPS record')
        if patch[cursor:cursor + 3] == b'EOF':
            cursor += 3
            if len(patch) - cursor not in (0, 3):
                raise ValueError('invalid IPS trailer')
            if cursor < len(patch):
                size = int.from_bytes(patch[cursor:], 'big')
                if size > MAX_IMAGE_SIZE:
                    raise ValueError('IPS image is too large')
                output[size:] = b''
                output.extend(b'\0' * max(0, size - len(output)))
            return bytes(output)
        offset = int.from_bytes(patch[cursor:cursor + 3], 'big')
        cursor += 3
        if cursor + 2 > len(patch):
            raise ValueError('truncated IPS size')
        size = int.from_bytes(patch[cursor:cursor + 2], 'big')
        cursor += 2
        if size:
            if cursor + size > len(patch):
                raise ValueError('truncated IPS payload')
            payload = patch[cursor:cursor + size]
            cursor += size
        else:
            if cursor + 3 > len(patch):
                raise ValueError('truncated IPS run')
            size = int.from_bytes(patch[cursor:cursor + 2], 'big')
            payload = patch[cursor + 2:cursor + 3] * size
            cursor += 3
        end = offset + len(payload)
        if end > MAX_IMAGE_SIZE:
            raise ValueError('IPS image is too large')
        for address, value in enumerate(payload, offset):
            if address in writes and writes[address] != value:
                raise ValueError(f'conflicting IPS writes at {address:#x}')
            writes[address] = value
        output.extend(b'\0' * max(0, end - len(output)))
        output[offset:end] = payload


def overlay_image(image: bytes, paths) -> bytes:
    writes = {}
    for path in paths:
        image = apply_ips(image, Path(path).read_bytes(), writes=writes)
    return image


def verified_overlay_image(image: bytes, overlays):
    """Apply exactly the bytes whose fingerprints were declared by the plan."""
    writes = {}
    fingerprints = []
    for path, expected in overlays:
        patch = Path(path).read_bytes()
        actual = hashlib.sha256(patch).hexdigest()
        if actual != expected.lower():
            raise ValueError(f'image overlay identity mismatch: {path}')
        image = apply_ips(image, patch, writes=writes)
        fingerprints.append(actual)
    return image, fingerprints
