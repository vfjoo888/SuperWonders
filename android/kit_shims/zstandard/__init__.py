import ctypes

_lib = None
_CONTENT_SIZE_UNKNOWN = 0xFFFFFFFFFFFFFFFF
_CONTENT_SIZE_ERROR = 0xFFFFFFFFFFFFFFFE


def _load():
    global _lib
    if _lib is not None:
        return _lib
    for cand in ("libzstd.so.1", "libzstd.so"):
        try:
            _lib = ctypes.CDLL(cand)
            break
        except OSError:
            pass
    if _lib is None:
        raise RuntimeError("Could not load libzstd.so")
    _lib.ZSTD_getFrameContentSize.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    _lib.ZSTD_getFrameContentSize.restype = ctypes.c_uint64
    _lib.ZSTD_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    _lib.ZSTD_decompress.restype = ctypes.c_size_t
    _lib.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
    _lib.ZSTD_compressBound.restype = ctypes.c_size_t
    _lib.ZSTD_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    _lib.ZSTD_compress.restype = ctypes.c_size_t
    _lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
    _lib.ZSTD_isError.restype = ctypes.c_uint
    return _lib


class ZstdError(Exception):
    pass


class ZstdDecompressor:
    def decompress(self, data, max_output_size=0):
        lib = _load()
        src = bytes(data)
        cap = lib.ZSTD_getFrameContentSize(src, len(src))
        if cap == _CONTENT_SIZE_ERROR:
            raise ZstdError("invalid zstd frame")
        if cap == _CONTENT_SIZE_UNKNOWN:
            cap = max_output_size if max_output_size > 0 else max(65536, len(src) * 16)
        dst = ctypes.create_string_buffer(cap)
        res = lib.ZSTD_decompress(dst, cap, src, len(src))
        if lib.ZSTD_isError(res):
            raise ZstdError(f"ZSTD_decompress error {res}")
        return dst.raw[:res]


class ZstdCompressor:
    def __init__(self, level=3):
        self.level = level

    def compress(self, data):
        lib = _load()
        src = bytes(data)
        cap = lib.ZSTD_compressBound(len(src))
        dst = ctypes.create_string_buffer(cap)
        res = lib.ZSTD_compress(dst, cap, src, len(src), self.level)
        if lib.ZSTD_isError(res):
            raise ZstdError(f"ZSTD_compress error {res}")
        return dst.raw[:res]
