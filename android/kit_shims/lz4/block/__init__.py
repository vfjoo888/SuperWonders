import ctypes
import ctypes.util
import os

_lib = None


def _load():
    global _lib
    if _lib is not None:
        return _lib
    for cand in ("liblz4.so", "liblz4.so.1"):
        try:
            _lib = ctypes.CDLL(cand)
            break
        except OSError:
            pass
    if _lib is None:
        raise RuntimeError("Could not load liblz4.so")
    _lib.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    _lib.LZ4_decompress_safe.restype = ctypes.c_int
    return _lib


def decompress(source, uncompressed_size=0, return_bytearray=False):
    lib = _load()
    src = bytes(source)
    if uncompressed_size <= 0:
        raise ValueError("uncompressed_size must be positive")
    dst = ctypes.create_string_buffer(uncompressed_size)
    n = lib.LZ4_decompress_safe(src, dst, len(src), uncompressed_size)
    if n < 0:
        raise ValueError(f"LZ4_decompress_safe failed ({n})")
    out = dst.raw[:n]
    return bytearray(out) if return_bytearray else out
