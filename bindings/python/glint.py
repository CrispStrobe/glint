"""
glint -- Python bindings for the glint MP3 encoder.

Usage:
    import glint

    # Encode a WAV file
    glint.encode_file("input.wav", "output.mp3", bitrate=128)

    # Encode raw PCM samples
    encoder = glint.Encoder(sample_rate=44100, channels=1, bitrate=128)
    mp3_data = encoder.encode(pcm_int16_array)
    mp3_data += encoder.flush()

    # NumPy integration
    import numpy as np
    pcm = (np.sin(2*np.pi*440*np.arange(44100)/44100) * 30000).astype(np.int16)
    mp3 = glint.encode_pcm(pcm, sample_rate=44100, bitrate=128)
"""

import ctypes
import ctypes.util
import os
import struct
import sys
from typing import Optional, Union

try:
    import numpy as np
    _HAS_NUMPY = True
except ImportError:
    _HAS_NUMPY = False

# ---------------------------------------------------------------------------
# Library loading
# ---------------------------------------------------------------------------

def _lib_name():
    """Return the platform-specific shared library filename."""
    if sys.platform == "win32":
        return "glint.dll"
    elif sys.platform == "darwin":
        return "libglint.dylib"
    return "libglint.so"


def _find_library(extra_dir: Optional[str] = None):
    """Locate the glint shared library.

    Search order:
      1. GLINT_LIB_PATH environment variable (exact path)
      2. *extra_dir* argument (if provided)
      3. Common relative paths from this file
      4. System library paths via ctypes.util.find_library
    """
    name = _lib_name()

    # 1. Environment variable
    env = os.environ.get("GLINT_LIB_PATH")
    if env:
        if os.path.isfile(env):
            return env
        candidate = os.path.join(env, name)
        if os.path.isfile(candidate):
            return candidate

    # 2. Caller-supplied directory
    if extra_dir:
        candidate = os.path.join(extra_dir, name)
        if os.path.isfile(candidate):
            return candidate

    # 3. Common relative locations
    here = os.path.dirname(os.path.abspath(__file__))
    search_dirs = [
        here,
        os.path.join(here, "build"),
        os.path.join(here, "..", "..", "build"),
        os.path.join(here, "..", "build"),
    ]
    for d in search_dirs:
        candidate = os.path.join(d, name)
        if os.path.isfile(candidate):
            return candidate

    # 4. System search
    path = ctypes.util.find_library("glint")
    if path:
        return path

    return None


def load_library(path: Optional[str] = None):
    """Load and return the glint shared library, setting up function signatures."""
    lib_path = _find_library(path)
    if lib_path is None:
        raise OSError(
            "Cannot find glint shared library. Set GLINT_LIB_PATH or pass "
            "the build directory to load_library()."
        )
    lib = ctypes.CDLL(lib_path)
    _setup_signatures(lib)
    return lib


# ---------------------------------------------------------------------------
# ctypes type aliases
# ---------------------------------------------------------------------------

# glint_t is an opaque pointer (struct glint_context*)
_glint_t = ctypes.c_void_p


class _GlintConfig(ctypes.Structure):
    _fields_ = [
        ("sample_rate", ctypes.c_int),
        ("num_channels", ctypes.c_int),
        ("mode", ctypes.c_int),
        ("bitrate", ctypes.c_int),
        ("path", ctypes.c_int),
        ("simd", ctypes.c_int),
        ("quality", ctypes.c_int),
        # vbr fields exist in the C struct; omitting them made glint_create
        # read uninitialized memory. Keep in sync with include/glint/glint.h.
        ("vbr", ctypes.c_int),
        ("vbr_quality", ctypes.c_int),
    ]

class _DecFrameInfo(ctypes.Structure):
    _fields_ = [
        ("sample_rate", ctypes.c_int),
        ("channels", ctypes.c_int),
        ("samples", ctypes.c_int),
        ("frame_bytes", ctypes.c_int),
    ]


class _GlintAacConfig(ctypes.Structure):
    # Keep in sync with struct glint_aac_config (zero-init: reserved must be 0)
    _fields_ = [
        ("sample_rate", ctypes.c_int),
        ("num_channels", ctypes.c_int),
        ("bitrate", ctypes.c_int),
        ("quality", ctypes.c_int),
        ("vbr", ctypes.c_int),
        ("vbr_quality", ctypes.c_int),
        ("reserved", ctypes.c_int * 4),
    ]


# Callback type: void (*glint_write_cb)(const uint8_t* data, int size, void* user_data)
_glint_write_cb = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_uint8),
                                   ctypes.c_int, ctypes.c_void_p)


def _setup_signatures(lib):
    """Declare C function prototypes so ctypes can marshal arguments."""
    lib.glint_check_config.argtypes = [ctypes.c_int, ctypes.c_int]
    lib.glint_check_config.restype = ctypes.c_int

    lib.glint_create.argtypes = [ctypes.POINTER(_GlintConfig)]
    lib.glint_create.restype = _glint_t

    lib.glint_create_streaming.argtypes = [
        ctypes.POINTER(_GlintConfig), _glint_write_cb, ctypes.c_void_p,
    ]
    lib.glint_create_streaming.restype = _glint_t

    lib.glint_samples_per_frame.argtypes = [_glint_t]
    lib.glint_samples_per_frame.restype = ctypes.c_int

    lib.glint_encode.argtypes = [
        _glint_t,
        ctypes.POINTER(ctypes.POINTER(ctypes.c_int16)),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.glint_encode.restype = ctypes.POINTER(ctypes.c_uint8)

    lib.glint_flush.argtypes = [_glint_t, ctypes.POINTER(ctypes.c_int)]
    lib.glint_flush.restype = ctypes.POINTER(ctypes.c_uint8)

    lib.glint_destroy.argtypes = [_glint_t]
    lib.glint_destroy.restype = None

    # AAC API (present since 0.8; guard for older shared libraries)
    try:
        lib.glint_version.argtypes = []
        lib.glint_version.restype = ctypes.c_int
        lib.glint_aac_create.argtypes = [ctypes.POINTER(_GlintAacConfig)]
        lib.glint_aac_create.restype = _glint_t
        lib.glint_aac_samples_per_frame.argtypes = [_glint_t]
        lib.glint_aac_samples_per_frame.restype = ctypes.c_int
        lib.glint_aac_encode.argtypes = [
            _glint_t,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_int16)),
            ctypes.POINTER(ctypes.c_int),
        ]
        lib.glint_aac_encode.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.glint_aac_flush.argtypes = [_glint_t, ctypes.POINTER(ctypes.c_int)]
        lib.glint_aac_flush.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.glint_aac_destroy.argtypes = [_glint_t]
        lib.glint_aac_destroy.restype = None
    except AttributeError:
        pass

    # Opus API (encoder + decoder; guard for older shared libraries)
    try:
        u8p = ctypes.POINTER(ctypes.c_uint8)
        f32p = ctypes.POINTER(ctypes.c_float)
        lib.glint_opus_enc_create.argtypes = [ctypes.c_int, ctypes.c_int,
                                              ctypes.c_int]
        lib.glint_opus_enc_create.restype = _glint_t
        lib.glint_opus_encode.argtypes = [_glint_t, f32p, ctypes.c_int,
                                          u8p, ctypes.c_int]
        lib.glint_opus_encode.restype = ctypes.c_int
        lib.glint_opus_enc_final_range.argtypes = [_glint_t]
        lib.glint_opus_enc_final_range.restype = ctypes.c_uint32
        lib.glint_opus_enc_destroy.argtypes = [_glint_t]
        lib.glint_opus_enc_destroy.restype = None

        lib.glint_opus_dec_create.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.glint_opus_dec_create.restype = _glint_t
        lib.glint_opus_decode.argtypes = [_glint_t, u8p, ctypes.c_int,
                                          f32p, ctypes.c_int]
        lib.glint_opus_decode.restype = ctypes.c_int
        lib.glint_opus_decode_fec.argtypes = [_glint_t, u8p, ctypes.c_int,
                                              f32p, ctypes.c_int]
        lib.glint_opus_decode_fec.restype = ctypes.c_int
        lib.glint_opus_dec_final_range.argtypes = [_glint_t]
        lib.glint_opus_dec_final_range.restype = ctypes.c_uint32
        lib.glint_opus_dec_destroy.argtypes = [_glint_t]
        lib.glint_opus_dec_destroy.restype = None

        lib.glint_opus_ms_dec_create.argtypes = [
            ctypes.c_int, ctypes.c_int, ctypes.c_int, u8p, ctypes.c_int]
        lib.glint_opus_ms_dec_create.restype = _glint_t
        lib.glint_opus_ms_decode.argtypes = [_glint_t, u8p, ctypes.c_int,
                                             f32p, ctypes.c_int]
        lib.glint_opus_ms_decode.restype = ctypes.c_int
        lib.glint_opus_ms_dec_destroy.argtypes = [_glint_t]
        lib.glint_opus_ms_dec_destroy.restype = None
    except AttributeError:
        pass

    # MP3 + AAC decoders (guard for older shared libraries)
    try:
        u8p = ctypes.POINTER(ctypes.c_uint8)
        f32p = ctypes.POINTER(ctypes.c_float)
        fip = ctypes.POINTER(_DecFrameInfo)
        for pfx in ("mp3", "aac"):
            getattr(lib, f"glint_{pfx}_frame_info").argtypes = [
                u8p, ctypes.c_int, fip]
            getattr(lib, f"glint_{pfx}_frame_info").restype = ctypes.c_int
            getattr(lib, f"glint_{pfx}_dec_create").argtypes = []
            getattr(lib, f"glint_{pfx}_dec_create").restype = _glint_t
            getattr(lib, f"glint_{pfx}_decode").argtypes = [
                _glint_t, u8p, ctypes.c_int, f32p, fip]
            getattr(lib, f"glint_{pfx}_decode").restype = ctypes.c_int
            getattr(lib, f"glint_{pfx}_dec_destroy").argtypes = [_glint_t]
            getattr(lib, f"glint_{pfx}_dec_destroy").restype = None
    except AttributeError:
        pass

    # Resampler + whole-file decode (guard for older shared libraries)
    try:
        f32p = ctypes.POINTER(ctypes.c_float)
        u8p = ctypes.POINTER(ctypes.c_uint8)
        ip = ctypes.POINTER(ctypes.c_int)
        lib.glint_resample.argtypes = [
            f32p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ip]
        lib.glint_resample.restype = f32p
        lib.glint_free.argtypes = [ctypes.c_void_p]
        lib.glint_free.restype = None
        lib.glint_decode_audio.argtypes = [
            u8p, ctypes.c_int, ip, ip, ip]
        lib.glint_decode_audio.restype = f32p
        lib.glint_decode_audio_ex.argtypes = [
            u8p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ip, ip, ip]
        lib.glint_decode_audio_ex.restype = ctypes.c_void_p
        lib.glint_opus_encode_file.argtypes = [
            f32p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ip]
        lib.glint_opus_encode_file.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.glint_wav_read.argtypes = [u8p, ctypes.c_int, ip, ip, ip]
        lib.glint_wav_read.restype = f32p
        lib.glint_wav_write.argtypes = [
            f32p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ip]
        lib.glint_wav_write.restype = ctypes.POINTER(ctypes.c_uint8)
        lib.glint_encode_audio.argtypes = [
            f32p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ip]
        lib.glint_encode_audio.restype = ctypes.POINTER(ctypes.c_uint8)
    except AttributeError:
        pass


# ---------------------------------------------------------------------------
# Constants (mirror the C enums)
# ---------------------------------------------------------------------------

MODE_MONO = 0
MODE_DUAL = 1
MODE_JOINT = 2
MODE_STEREO = 3

QUALITY_SPEED = 0
QUALITY_NORMAL = 1
QUALITY_BEST = 2

# Output codecs for encode_audio / one-call encode.
FORMAT_MP3 = 0
FORMAT_AAC = 1
FORMAT_OPUS = 2
_FORMAT_BY_NAME = {"mp3": FORMAT_MP3, "aac": FORMAT_AAC, "opus": FORMAT_OPUS}

PATH_DEFAULT = 0
PATH_DOUBLE = 1
PATH_FIXED = 2

SIMD_AUTO = 0
SIMD_AVX = 1
SIMD_SSE2 = 2
SIMD_NONE = 3


# ---------------------------------------------------------------------------
# Error types
# ---------------------------------------------------------------------------

class GlintError(Exception):
    """Base exception for glint errors."""


class ConfigError(GlintError):
    """Raised when an invalid configuration is supplied."""


# ---------------------------------------------------------------------------
# Public helpers
# ---------------------------------------------------------------------------

def check_config(sample_rate: int, bitrate: int, *, lib=None) -> bool:
    """Return True if the sample_rate / bitrate combination is valid.

    The C function returns 0 on success, non-zero on error.
    """
    _lib = lib or _get_default_lib()
    return _lib.glint_check_config(sample_rate, bitrate) == 0


# ---------------------------------------------------------------------------
# Encoder class
# ---------------------------------------------------------------------------

class Encoder:
    """Pythonic wrapper around the glint MP3 encoder.

    Parameters
    ----------
    sample_rate : int
        Input sample rate in Hz (e.g. 44100, 48000, 32000).
    channels : int
        Number of input channels (1 or 2).
    bitrate : int
        Target bitrate in kbps (e.g. 128, 192, 320).
    mode : int, optional
        Channel mode constant (MODE_MONO, MODE_JOINT, ...).
        Defaults to MODE_MONO for 1 channel, MODE_JOINT for 2.
    path : int, optional
        Signal path selection (PATH_DEFAULT, PATH_DOUBLE, PATH_FIXED).
    simd : int, optional
        SIMD backend (SIMD_AUTO, SIMD_AVX, SIMD_SSE2, SIMD_NONE).
    lib_path : str, optional
        Directory containing the shared library.
    """

    def __init__(
        self,
        sample_rate: int = 44100,
        channels: int = 1,
        bitrate: int = 128,
        mode: Optional[int] = None,
        path: int = PATH_DEFAULT,
        simd: int = SIMD_AUTO,
        quality: int = QUALITY_NORMAL,
        vbr_quality: Optional[int] = None,
        lib_path: Optional[str] = None,
        write_callback=None,
    ):
        self._lib = load_library(lib_path)
        self._handle = None  # ensure attribute exists for __del__
        self._write_cb_ref = None  # prevent GC of ctypes callback

        if mode is None:
            mode = MODE_MONO if channels == 1 else MODE_JOINT

        cfg = _GlintConfig(
            sample_rate=sample_rate,
            num_channels=channels,
            mode=mode,
            bitrate=bitrate,
            path=path,
            simd=simd,
            quality=quality,
        )
        if vbr_quality is not None:
            cfg.vbr = 1
            cfg.vbr_quality = int(vbr_quality)

        if write_callback is not None:
            # Wrap the Python callback into a ctypes callback.
            # The Python callable receives (bytes, size).
            def _c_callback(data_ptr, size, _user_data):
                if size > 0:
                    write_callback(ctypes.string_at(data_ptr, size), size)
            self._write_cb_ref = _glint_write_cb(_c_callback)
            handle = self._lib.glint_create_streaming(
                ctypes.byref(cfg), self._write_cb_ref, None,
            )
        else:
            handle = self._lib.glint_create(ctypes.byref(cfg))

        if not handle:
            raise ConfigError(
                f"glint_create failed (sr={sample_rate}, ch={channels}, "
                f"br={bitrate}, mode={mode})"
            )
        self._handle = handle
        self._channels = channels

        self._samples_per_frame = self._lib.glint_samples_per_frame(
            self._handle
        )

    # -- properties --

    @property
    def samples_per_frame(self) -> int:
        """Number of samples per channel expected by each encode() call."""
        return self._samples_per_frame

    @property
    def channels(self) -> int:
        return self._channels

    # -- core API --

    def encode(self, pcm) -> bytes:
        """Encode one frame of PCM audio.

        Parameters
        ----------
        pcm : array-like of int16
            For mono: a flat sequence of *samples_per_frame* samples.
            For stereo: either a flat interleaved sequence of
            *samples_per_frame * 2* samples, or a list/tuple of two
            arrays each of length *samples_per_frame*.

        Returns
        -------
        bytes
            Encoded MP3 data (may be empty for the first frame due to
            the bit reservoir look-ahead).
        """
        self._check_alive()
        channel_arrays = self._prepare_channels(pcm)
        return self._do_encode(channel_arrays)

    def flush(self) -> bytes:
        """Flush the encoder and return any remaining MP3 data."""
        self._check_alive()
        out_size = ctypes.c_int(0)
        ptr = self._lib.glint_flush(self._handle, ctypes.byref(out_size))
        if ptr and out_size.value > 0:
            return ctypes.string_at(ptr, out_size.value)
        return b""

    def close(self):
        """Destroy the encoder and free resources."""
        if self._handle:
            self._lib.glint_destroy(self._handle)
            self._handle = None

    # -- context manager --

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        self.close()

    # -- internals --

    def _check_alive(self):
        if not self._handle:
            raise GlintError("Encoder has been closed")

    def _prepare_channels(self, pcm):
        """Convert various PCM input formats to a list of ctypes int16 arrays."""
        spf = self._samples_per_frame
        nch = self._channels

        # Already split into per-channel arrays?
        if isinstance(pcm, (list, tuple)) and len(pcm) == nch and nch > 1:
            arrays = []
            for ch_data in pcm:
                arrays.append(self._to_int16_array(ch_data, spf))
            return arrays

        # Flat buffer -- could be interleaved stereo or mono
        flat = self._to_int16_array(pcm, spf * nch)
        if nch == 1:
            return [flat]

        # De-interleave stereo
        left = (ctypes.c_int16 * spf)()
        right = (ctypes.c_int16 * spf)()
        for i in range(spf):
            left[i] = flat[2 * i]
            right[i] = flat[2 * i + 1]
        return [left, right]

    @staticmethod
    def _to_int16_array(data, expected_len):
        """Convert *data* to a ctypes c_int16 array of *expected_len*."""
        if _HAS_NUMPY and isinstance(data, np.ndarray):
            data = data.astype(np.int16, copy=False)
            arr = (ctypes.c_int16 * expected_len)()
            ctypes.memmove(arr, data.ctypes.data, min(len(data), expected_len) * 2)
            return arr

        if isinstance(data, (bytes, bytearray)):
            arr = (ctypes.c_int16 * expected_len)()
            ctypes.memmove(arr, bytes(data), min(len(data), expected_len * 2))
            return arr

        # Generic iterable (list, array.array, etc.)
        arr = (ctypes.c_int16 * expected_len)()
        for i, v in enumerate(data):
            if i >= expected_len:
                break
            arr[i] = int(v)
        return arr

    def _do_encode(self, channel_arrays):
        """Call glint_encode with prepared per-channel arrays."""
        nch = len(channel_arrays)
        ch_ptrs = (ctypes.POINTER(ctypes.c_int16) * nch)()
        for i, arr in enumerate(channel_arrays):
            ch_ptrs[i] = ctypes.cast(arr, ctypes.POINTER(ctypes.c_int16))

        out_size = ctypes.c_int(0)
        ptr = self._lib.glint_encode(
            self._handle,
            ctypes.cast(ch_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_int16))),
            ctypes.byref(out_size),
        )
        if ptr and out_size.value > 0:
            return ctypes.string_at(ptr, out_size.value)
        return b""


class AacEncoder(Encoder):
    """AAC-LC encoder (ADTS output). Same PCM input conventions as Encoder.

    One encode() call consumes samples_per_frame (1024) samples per channel
    and returns one ADTS frame (the first call returns a silence priming
    frame; encoder delay is 2048 samples). flush() returns the two tail
    frames and MUST be called at end of stream.
    """

    def __init__(
        self,
        sample_rate: int = 44100,
        channels: int = 2,
        bitrate: int = 128,
        quality: int = QUALITY_NORMAL,
        vbr_quality: Optional[int] = None,
        lib_path: Optional[str] = None,
    ):
        self._lib = load_library(lib_path)
        self._handle = None
        self._write_cb_ref = None

        if not hasattr(self._lib, "glint_aac_create"):
            raise GlintError("loaded libglint has no AAC support (< 0.8)")

        cfg = _GlintAacConfig(
            sample_rate=sample_rate,
            num_channels=channels,
            bitrate=bitrate,
            quality=quality,
        )
        if vbr_quality is not None:
            cfg.vbr = 1
            cfg.vbr_quality = int(vbr_quality)
        handle = self._lib.glint_aac_create(ctypes.byref(cfg))
        if not handle:
            raise ConfigError(
                f"glint_aac_create failed (sr={sample_rate}, ch={channels}, "
                f"br={bitrate})"
            )
        self._handle = handle
        self._channels = channels
        self._samples_per_frame = self._lib.glint_aac_samples_per_frame(handle)

    def flush(self) -> bytes:
        self._check_alive()
        out_size = ctypes.c_int(0)
        ptr = self._lib.glint_aac_flush(self._handle, ctypes.byref(out_size))
        if ptr and out_size.value > 0:
            return ctypes.string_at(ptr, out_size.value)
        return b""

    def close(self):
        if self._handle:
            self._lib.glint_aac_destroy(self._handle)
            self._handle = None

    def _do_encode(self, channel_arrays):
        nch = len(channel_arrays)
        ch_ptrs = (ctypes.POINTER(ctypes.c_int16) * nch)()
        for i, arr in enumerate(channel_arrays):
            ch_ptrs[i] = ctypes.cast(arr, ctypes.POINTER(ctypes.c_int16))
        out_size = ctypes.c_int(0)
        ptr = self._lib.glint_aac_encode(
            self._handle,
            ctypes.cast(ch_ptrs, ctypes.POINTER(ctypes.POINTER(ctypes.c_int16))),
            ctypes.byref(out_size),
        )
        if ptr and out_size.value > 0:
            return ctypes.string_at(ptr, out_size.value)
        return b""


# ---------------------------------------------------------------------------
# Default (module-level) library singleton
# ---------------------------------------------------------------------------

_default_lib = None
_default_lib_path = None


def _get_default_lib():
    global _default_lib, _default_lib_path
    if _default_lib is None:
        _default_lib = load_library(_default_lib_path)
    return _default_lib


def set_library_path(path: str):
    """Set the default search directory for the shared library.

    Call this before any other API if the library is not in a standard
    location.
    """
    global _default_lib, _default_lib_path
    _default_lib_path = path
    _default_lib = None  # force reload on next use


# ---------------------------------------------------------------------------
# Convenience functions
# ---------------------------------------------------------------------------

def encode_pcm(
    pcm,
    sample_rate: int = 44100,
    channels: int = 1,
    bitrate: int = 128,
    *,
    lib_path: Optional[str] = None,
) -> bytes:
    """Encode a buffer of PCM int16 samples to MP3 in one shot.

    Parameters
    ----------
    pcm : array-like of int16
        Mono samples or interleaved stereo samples.
    sample_rate : int
        Sample rate in Hz.
    channels : int
        Number of channels (1 or 2).
    bitrate : int
        Target bitrate in kbps.
    lib_path : str, optional
        Directory containing the shared library.

    Returns
    -------
    bytes
        Complete MP3 data.
    """
    with Encoder(
        sample_rate=sample_rate,
        channels=channels,
        bitrate=bitrate,
        lib_path=lib_path,
    ) as enc:
        spf = enc.samples_per_frame
        total = _sample_count(pcm, channels)
        result = bytearray()

        offset = 0
        while offset + spf * channels <= total:
            frame = _slice_pcm(pcm, offset, spf * channels)
            result.extend(enc.encode(frame))
            offset += spf * channels

        # Pad final partial frame with silence
        if offset < total:
            remaining = total - offset
            padded_len = spf * channels
            frame = _slice_pcm(pcm, offset, remaining)
            if _HAS_NUMPY and isinstance(pcm, __import__("numpy").ndarray):
                import numpy as _np
                padded = _np.zeros(padded_len, dtype=_np.int16)
                padded[:remaining] = frame
                frame = padded
            else:
                padded = [0] * padded_len
                for i, v in enumerate(frame):
                    padded[i] = v
                frame = padded
            result.extend(enc.encode(frame))

        result.extend(enc.flush())
        return bytes(result)


def encode_file(
    wav_path: str,
    mp3_path: str,
    bitrate: int = 128,
    *,
    lib_path: Optional[str] = None,
):
    """Encode a WAV file to MP3.

    Only uncompressed 16-bit PCM WAV files are supported.
    """
    sample_rate, channels, pcm_data = _read_wav(wav_path)

    with Encoder(
        sample_rate=sample_rate,
        channels=channels,
        bitrate=bitrate,
        lib_path=lib_path,
    ) as enc:
        spf = enc.samples_per_frame
        samples_per_frame_bytes = spf * channels * 2  # 2 bytes per int16
        result = bytearray()
        offset = 0

        while offset + samples_per_frame_bytes <= len(pcm_data):
            chunk = pcm_data[offset : offset + samples_per_frame_bytes]
            # Convert bytes to list of int16
            frame = list(struct.unpack(f"<{spf * channels}h", chunk))
            result.extend(enc.encode(frame))
            offset += samples_per_frame_bytes

        # Handle partial final frame
        if offset < len(pcm_data):
            remaining_bytes = len(pcm_data) - offset
            n_samples = remaining_bytes // 2
            chunk = pcm_data[offset : offset + n_samples * 2]
            frame = list(struct.unpack(f"<{n_samples}h", chunk))
            # Pad to full frame
            frame.extend([0] * (spf * channels - n_samples))
            result.extend(enc.encode(frame))

        result.extend(enc.flush())

    with open(mp3_path, "wb") as f:
        f.write(result)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _sample_count(pcm, channels):
    """Return the total number of individual sample values in *pcm*."""
    if _HAS_NUMPY and isinstance(pcm, __import__("numpy").ndarray):
        return pcm.size
    if isinstance(pcm, (bytes, bytearray)):
        return len(pcm) // 2
    return len(pcm)


def _slice_pcm(pcm, start, length):
    """Return a slice of *pcm* from *start* for *length* samples."""
    if _HAS_NUMPY and isinstance(pcm, __import__("numpy").ndarray):
        return pcm[start : start + length]
    if isinstance(pcm, (bytes, bytearray)):
        return pcm[start * 2 : (start + length) * 2]
    return pcm[start : start + length]


def _read_wav(path: str):
    """Read a 16-bit PCM WAV file. Returns (sample_rate, channels, pcm_bytes)."""
    with open(path, "rb") as f:
        riff = f.read(4)
        if riff != b"RIFF":
            raise GlintError("Not a valid WAV file (missing RIFF header)")

        f.read(4)  # file size
        wave = f.read(4)
        if wave != b"WAVE":
            raise GlintError("Not a valid WAV file (missing WAVE marker)")

        fmt_found = False
        sample_rate = 0
        channels = 0
        pcm_data = b""

        while True:
            chunk_hdr = f.read(8)
            if len(chunk_hdr) < 8:
                break
            chunk_id = chunk_hdr[:4]
            chunk_size = struct.unpack("<I", chunk_hdr[4:8])[0]

            if chunk_id == b"fmt ":
                fmt_data = f.read(chunk_size)
                audio_fmt = struct.unpack("<H", fmt_data[0:2])[0]
                if audio_fmt != 1:
                    raise GlintError(
                        f"Unsupported WAV format {audio_fmt} (only PCM=1 supported)"
                    )
                channels = struct.unpack("<H", fmt_data[2:4])[0]
                sample_rate = struct.unpack("<I", fmt_data[4:8])[0]
                bits_per_sample = struct.unpack("<H", fmt_data[14:16])[0]
                if bits_per_sample != 16:
                    raise GlintError(
                        f"Unsupported bits per sample {bits_per_sample} (only 16 supported)"
                    )
                fmt_found = True
            elif chunk_id == b"data":
                pcm_data = f.read(chunk_size)
            else:
                f.read(chunk_size)

            # WAV chunks are 2-byte aligned
            if chunk_size % 2 != 0:
                f.read(1)

        if not fmt_found:
            raise GlintError("WAV file missing fmt chunk")

        return sample_rate, channels, pcm_data


# ---------------------------------------------------------------------------
# Opus (RFC 6716/7845): CELT encoder + full decoder
# ---------------------------------------------------------------------------

class OpusEncoder:
    """CELT-only Opus encoder: 48 kHz interleaved float32 PCM in,
    complete Opus packets (TOC + payload) out. Frame sizes 120, 240,
    480 or 960 samples per channel; CBR or unconstrained VBR."""

    def __init__(self, channels: int = 2, bitrate: int = 96000,
                 vbr: bool = False, *, lib=None):
        self._lib = lib or load_library()
        self._enc = self._lib.glint_opus_enc_create(
            channels, bitrate, 1 if vbr else 0)
        if not self._enc:
            raise ConfigError(
                f"invalid Opus encoder config: channels={channels} "
                f"bitrate={bitrate}")
        self._channels = channels
        self._buf = (ctypes.c_uint8 * 1500)()

    @property
    def channels(self) -> int:
        return self._channels

    def encode(self, pcm) -> bytes:
        """Encode one frame of interleaved float PCM (list, array or
        float32 numpy array of frame_size*channels). Returns the
        packet."""
        if _HAS_NUMPY and isinstance(pcm, np.ndarray):
            arr = np.ascontiguousarray(pcm, dtype=np.float32)
            n = arr.size
            ptr = arr.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        else:
            arr = (ctypes.c_float * len(pcm))(*pcm)
            n = len(pcm)
            ptr = arr
        frame = n // self._channels
        ret = self._lib.glint_opus_encode(self._enc, ptr, frame,
                                          self._buf, len(self._buf))
        if ret < 0:
            raise GlintError(f"opus encode failed ({ret}); frame sizes "
                             "must be 120/240/480/960 per channel")
        return bytes(self._buf[:ret])

    def final_range(self) -> int:
        return self._lib.glint_opus_enc_final_range(self._enc)

    def close(self):
        if getattr(self, "_enc", None):
            self._lib.glint_opus_enc_destroy(self._enc)
            self._enc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class OpusDecoder:
    """Opus decoder (SILK/CELT/hybrid, PLC, in-band FEC). Output rates
    48000/24000/16000/12000/8000; interleaved float32 PCM out."""

    def __init__(self, channels: int = 2, sample_rate: int = 48000, *,
                 lib=None):
        self._lib = lib or load_library()
        self._dec = self._lib.glint_opus_dec_create(channels, sample_rate)
        if not self._dec:
            raise ConfigError(
                f"invalid Opus decoder config: channels={channels} "
                f"sample_rate={sample_rate}")
        self._channels = channels
        self._pcm = (ctypes.c_float * (2 * 5760))()

    @property
    def channels(self) -> int:
        return self._channels

    def _out(self, n):
        flat = self._pcm[: n * self._channels]
        if _HAS_NUMPY:
            return np.array(flat, dtype=np.float32).reshape(
                n, self._channels)
        return list(flat)

    def decode(self, packet: bytes):
        """Decode one packet; returns interleaved float PCM (numpy
        (samples, channels) array when numpy is available)."""
        buf = (ctypes.c_uint8 * len(packet)).from_buffer_copy(packet)
        n = self._lib.glint_opus_decode(self._dec, buf, len(packet),
                                        self._pcm, 5760)
        if n < 0:
            raise GlintError(f"opus decode failed ({n})")
        return self._out(n)

    def decode_lost(self, frame_size: int, next_packet: bytes = None):
        """Conceal a LOST packet of frame_size samples/channel. Pass the
        packet FOLLOWING the loss to use SILK in-band FEC when present."""
        if next_packet:
            buf = (ctypes.c_uint8 * len(next_packet)).from_buffer_copy(
                next_packet)
            n = self._lib.glint_opus_decode_fec(
                self._dec, buf, len(next_packet), self._pcm, frame_size)
        else:
            n = self._lib.glint_opus_decode_fec(
                self._dec, None, 0, self._pcm, frame_size)
        if n < 0:
            raise GlintError(f"opus concealment failed ({n})")
        return self._out(n)

    def final_range(self) -> int:
        return self._lib.glint_opus_dec_final_range(self._dec)

    def close(self):
        if getattr(self, "_dec", None):
            self._lib.glint_opus_dec_destroy(self._dec)
            self._dec = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# MP3 + AAC-LC decoders
# ---------------------------------------------------------------------------

class _FrameDecoder:
    """Shared MP3/AAC decoder frontend. Subclasses set _prefix."""
    _prefix = None

    def __init__(self, *, lib=None):
        self._lib = lib or load_library()
        self._create = getattr(self._lib, f"glint_{self._prefix}_dec_create")
        self._decode = getattr(self._lib, f"glint_{self._prefix}_decode")
        self._info = getattr(self._lib, f"glint_{self._prefix}_frame_info")
        self._destroy = getattr(
            self._lib, f"glint_{self._prefix}_dec_destroy")
        self._dec = self._create()
        if not self._dec:
            raise GlintError(f"{self._prefix} decoder create failed")
        self._pcm = (ctypes.c_float * (2 * 1152))()
        self._min_hdr = 4 if self._prefix == "mp3" else 7

    def frame_info(self, data: bytes):
        """Parse one frame header; returns a dict or None if no sync at
        data[0]."""
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        fi = _DecFrameInfo()
        if self._info(buf, len(data), ctypes.byref(fi)) < 0:
            return None
        return {"sample_rate": fi.sample_rate, "channels": fi.channels,
                "samples": fi.samples, "frame_bytes": fi.frame_bytes}

    def decode_frame(self, data: bytes):
        """Decode ONE frame at data[0]. Returns (pcm, info): pcm is
        interleaved float (numpy (samples, channels) when numpy is
        present, else a flat list); info is the frame dict. pcm is empty
        while the MP3 reservoir fills."""
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        fi = _DecFrameInfo()
        n = self._decode(self._dec, buf, len(data), self._pcm,
                         ctypes.byref(fi))
        if n < 0:
            raise GlintError(f"{self._prefix} decode failed ({n})")
        ch = fi.channels or 1
        flat = self._pcm[: n * ch]
        info = {"sample_rate": fi.sample_rate, "channels": fi.channels,
                "samples": n, "frame_bytes": fi.frame_bytes}
        if _HAS_NUMPY:
            arr = np.array(flat, dtype=np.float32).reshape(n, ch) if n \
                else np.zeros((0, ch), dtype=np.float32)
            return arr, info
        return list(flat), info

    def decode(self, data: bytes):
        """Decode a whole stream (walk frames, skip ID3v2). Returns
        interleaved float PCM: numpy (total, channels) or a flat list."""
        off = 0
        if len(data) > 10 and data[:3] == b"ID3":
            sz = ((data[6] & 0x7F) << 21) | ((data[7] & 0x7F) << 14) | \
                 ((data[8] & 0x7F) << 7) | (data[9] & 0x7F)
            off = 10 + sz
        chunks = []
        ch = 0
        while off + self._min_hdr <= len(data):
            info = self.frame_info(data[off:off + 16])
            if info is None:
                off += 1
                continue
            fb = info["frame_bytes"]
            if off + fb > len(data):
                break
            pcm, fi = self.decode_frame(data[off:off + fb])
            ch = fi["channels"] or ch
            if _HAS_NUMPY:
                if len(pcm):
                    chunks.append(pcm)
            else:
                chunks.extend(pcm)
            off += fb
        if _HAS_NUMPY:
            return np.concatenate(chunks) if chunks else \
                np.zeros((0, ch or 1), dtype=np.float32)
        return chunks

    def close(self):
        if getattr(self, "_dec", None):
            self._destroy(self._dec)
            self._dec = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


class Mp3Decoder(_FrameDecoder):
    """MPEG-1/2 Layer III decoder. Keeps a bit reservoir across frames."""
    _prefix = "mp3"


class AacDecoder(_FrameDecoder):
    """ADTS AAC-LC decoder."""
    _prefix = "aac"


# ---------------------------------------------------------------------------
# High-level convenience: resample, whole-file decode, transcode, WAV I/O
# (PLAN buckets A+B — mirrors the CLI over the shared C ABI)
# ---------------------------------------------------------------------------

def resample(pcm, sr_in: int, sr_out: int, channels: int = 1, *, lib=None):
    """Resample interleaved float PCM (±1.0) from *sr_in* to *sr_out* with
    a Kaiser-windowed sinc kernel (anti-aliased, unity passband). *pcm* is
    a numpy float array or a flat sequence of floats; returns the same kind
    (numpy when available). Pass-through when the rates match."""
    _lib = lib or load_library()
    if _HAS_NUMPY and isinstance(pcm, np.ndarray):
        flat = np.ascontiguousarray(pcm.reshape(-1), dtype=np.float32)
        buf = flat.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        n = flat.size
    else:
        flat = list(pcm)
        n = len(flat)
        buf = (ctypes.c_float * n)(*flat)
    n_in = n // channels
    out_frames = ctypes.c_int(0)
    ptr = _lib.glint_resample(buf, n_in, channels, sr_in, sr_out,
                              ctypes.byref(out_frames))
    if not ptr:
        raise GlintError("resample failed")
    total = out_frames.value * channels
    try:
        if _HAS_NUMPY:
            out = np.ctypeslib.as_array(ptr, shape=(total,)).copy()
        else:
            out = [ptr[i] for i in range(total)]
    finally:
        _lib.glint_free(ctypes.cast(ptr, ctypes.c_void_p))
    return out


def decode_bytes(data: bytes, *, rate: int = 0, dtype: str = "float",
                 lib=None):
    """Decode an in-memory MP3 / AAC-LC / Ogg-Opus stream (format auto-
    detected, Opus surround included) to interleaved PCM. Returns (pcm,
    sample_rate, channels). *rate* resamples the output (0 = native);
    *dtype* is 'float' (±1.0) or 'int16'. pcm is a numpy array (frames,
    channels) of the matching dtype when numpy is present, else a flat
    list."""
    if dtype not in ("float", "int16"):
        raise GlintError("dtype must be 'float' or 'int16'")
    _lib = lib or load_library()
    want_i16 = 1 if dtype == "int16" else 0
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    sr = ctypes.c_int(0)
    ch = ctypes.c_int(0)
    fr = ctypes.c_int(0)
    vptr = _lib.glint_decode_audio_ex(buf, len(data), rate, want_i16,
                                      ctypes.byref(sr), ctypes.byref(ch),
                                      ctypes.byref(fr))
    if not vptr:
        raise GlintError("decode failed (unrecognized or corrupt input)")
    total = fr.value * ch.value
    ctype = ctypes.c_int16 if want_i16 else ctypes.c_float
    ptr = ctypes.cast(vptr, ctypes.POINTER(ctype))
    try:
        if _HAS_NUMPY:
            npdt = np.int16 if want_i16 else np.float32
            flat = np.ctypeslib.as_array(ptr, shape=(total,)).copy()
            pcm = flat.astype(npdt).reshape(fr.value, ch.value)
        else:
            pcm = [ptr[i] for i in range(total)]
    finally:
        _lib.glint_free(ctypes.c_void_p(vptr))
    return pcm, sr.value, ch.value


def read_wav_bytes(data: bytes, *, lib=None):
    """Parse a WAV buffer (PCM 8/16/24/32, IEEE float 32/64, A-law,
    mu-law, EXTENSIBLE) to interleaved float PCM. Returns (pcm, sr, ch).
    Falls back to a pure-Python 16-bit reader on older libraries."""
    _lib = lib or load_library()
    if hasattr(_lib, "glint_wav_read"):
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        sr = ctypes.c_int(0)
        ch = ctypes.c_int(0)
        fr = ctypes.c_int(0)
        ptr = _lib.glint_wav_read(buf, len(data), ctypes.byref(sr),
                                  ctypes.byref(ch), ctypes.byref(fr))
        if not ptr:
            raise GlintError("WAV read failed (unsupported format?)")
        total = fr.value * ch.value
        try:
            if _HAS_NUMPY:
                flat = np.ctypeslib.as_array(ptr, shape=(total,)).copy()
                pcm = flat.reshape(fr.value, ch.value)
            else:
                pcm = [ptr[i] for i in range(total)]
        finally:
            _lib.glint_free(ctypes.cast(ptr, ctypes.c_void_p))
        return pcm, sr.value, ch.value
    raise GlintError("libglint too old for flexible WAV read (< 0.9)")


def decode_file(path: str, *, rate: int = 0, dtype: str = "float", lib=None):
    """Decode an MP3 / AAC / Opus / WAV file to interleaved PCM. Returns
    (pcm, sample_rate, channels). WAV inputs of any bit depth are
    supported; *rate* resamples the output, *dtype* is 'float'|'int16'."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        pcm, sr, ch = read_wav_bytes(data, lib=lib)
        if rate and rate != sr:
            flat = pcm.reshape(-1) if _HAS_NUMPY and hasattr(pcm, "reshape") \
                else pcm
            flat = resample(flat, sr, rate, ch, lib=lib)
            sr = rate
            pcm = flat.reshape(-1, ch) if _HAS_NUMPY and \
                hasattr(flat, "reshape") else flat
        if dtype == "int16":
            if _HAS_NUMPY and hasattr(pcm, "astype"):
                pcm = np.round(np.clip(pcm, -1, 1) * 32767.0).astype(np.int16)
            else:
                pcm = [int(round(max(-1.0, min(1.0, x)) * 32767.0))
                       for x in pcm]
        return pcm, sr, ch
    return decode_bytes(data, rate=rate, dtype=dtype, lib=lib)


def read_wav_float(path: str, *, lib=None):
    """Read a WAV file of any supported bit depth as float PCM. Returns
    (pcm, sr, ch)."""
    with open(path, "rb") as f:
        return read_wav_bytes(f.read(), lib=lib)


def write_wav(path: str, pcm, sample_rate: int, channels: int, *,
              bits: int = 16, float_fmt: bool = False, lib=None):
    """Write interleaved float PCM (±1.0) to a WAV file. *pcm* is a numpy
    float array or a flat sequence. bits: 8/16/24/32 integer, or 32/64
    with float_fmt for IEEE float (default 16-bit PCM)."""
    _lib = lib or load_library()
    if _HAS_NUMPY and isinstance(pcm, np.ndarray):
        flat = np.ascontiguousarray(pcm.reshape(-1), dtype=np.float32)
        buf = flat.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        n = flat.size
    else:
        flat = list(pcm)
        n = len(flat)
        buf = (ctypes.c_float * n)(*flat)
    frames = n // channels
    if hasattr(_lib, "glint_wav_write"):
        out_size = ctypes.c_int(0)
        ptr = _lib.glint_wav_write(buf, frames, channels, sample_rate,
                                   bits, 1 if float_fmt else 0,
                                   ctypes.byref(out_size))
        if not ptr or out_size.value <= 0:
            raise GlintError("WAV write failed")
        try:
            data = ctypes.string_at(ptr, out_size.value)
        finally:
            _lib.glint_free(ctypes.cast(ptr, ctypes.c_void_p))
        with open(path, "wb") as f:
            f.write(data)
        return
    # Fallback: pure-Python 16-bit writer.
    vals = flat.tolist() if hasattr(flat, "tolist") else flat
    clipped = [max(-1.0, min(1.0, float(x))) for x in vals]
    i16 = struct.pack(f"<{len(clipped)}h",
                      *[int(round(x * 32767.0)) for x in clipped])
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(i16)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, sample_rate,
                            sample_rate * 2 * channels, 2 * channels, 16))
        f.write(b"data" + struct.pack("<I", len(i16)) + i16)


def _encode_int16(enc, i16, channels):
    """Feed interleaved int16 through an Encoder/AacEncoder frame by frame
    (zero-padding the final partial frame) and return the coded bytes."""
    spf = enc.samples_per_frame
    step = spf * channels
    total = len(i16)
    out = bytearray()
    off = 0
    while off + step <= total:
        out.extend(enc.encode(i16[off:off + step]))
        off += step
    if off < total:
        tail = list(i16[off:total])
        tail.extend([0] * (step - len(tail)))
        out.extend(enc.encode(tail))
    out.extend(enc.flush())
    return bytes(out)


def encode_audio(pcm, channels: int, sample_rate: int, codec="mp3", *,
                 bitrate: int = 128, vbr_quality: Optional[int] = None,
                 quality: int = QUALITY_NORMAL, lib=None):
    """One-call encode: interleaved float PCM (±1.0) at any rate/1-2 ch ->
    a complete MP3 / AAC-LC / Ogg-Opus stream (bytes). The input is
    auto-resampled to a codec-valid rate (Opus->48k, MP3/AAC->nearest
    supported). *codec* is 'mp3'|'aac'|'opus' or a FORMAT_* constant;
    bitrate in kbps; vbr_quality 0..9 selects VBR."""
    _lib = lib or load_library()
    fmt = _FORMAT_BY_NAME.get(codec, codec) if isinstance(codec, str) \
        else codec
    if fmt not in (FORMAT_MP3, FORMAT_AAC, FORMAT_OPUS):
        raise GlintError(f"unknown codec {codec!r}")
    if _HAS_NUMPY and isinstance(pcm, np.ndarray):
        flat = np.ascontiguousarray(pcm.reshape(-1), dtype=np.float32)
        buf = flat.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        n = flat.size
    else:
        flat = list(pcm)
        n = len(flat)
        buf = (ctypes.c_float * n)(*flat)
    frames = n // channels
    out_size = ctypes.c_int(0)
    ptr = _lib.glint_encode_audio(
        buf, frames, channels, sample_rate, fmt, bitrate,
        -1 if vbr_quality is None else int(vbr_quality), quality,
        ctypes.byref(out_size))
    if not ptr or out_size.value <= 0:
        raise GlintError("encode failed")
    try:
        data = ctypes.string_at(ptr, out_size.value)
    finally:
        _lib.glint_free(ctypes.cast(ptr, ctypes.c_void_p))
    return data


def encode_opus_file(pcm, channels: int, *, bitrate: int = 96000,
                     vbr: bool = False, lib=None):
    """Encode interleaved 48 kHz float PCM (±1.0) to a complete Ogg-Opus
    file (CELT-only, 20 ms frames). *pcm* is a numpy float array or a flat
    sequence; returns the .opus file bytes. Input MUST be 48 kHz (resample
    first with resample())."""
    _lib = lib or load_library()
    if _HAS_NUMPY and isinstance(pcm, np.ndarray):
        flat = np.ascontiguousarray(pcm.reshape(-1), dtype=np.float32)
        buf = flat.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        n = flat.size
    else:
        flat = list(pcm)
        n = len(flat)
        buf = (ctypes.c_float * n)(*flat)
    frames = n // channels
    out_size = ctypes.c_int(0)
    ptr = _lib.glint_opus_encode_file(buf, frames, channels, bitrate,
                                      1 if vbr else 0, ctypes.byref(out_size))
    if not ptr or out_size.value <= 0:
        raise GlintError("opus encode failed")
    try:
        data = ctypes.string_at(ptr, out_size.value)
    finally:
        _lib.glint_free(ctypes.cast(ptr, ctypes.c_void_p))
    return data


def transcode_file(in_path: str, out_path: str, *, bitrate: int = 128,
                   rate: Optional[int] = None, gain_db: float = 0.0,
                   lib_path: Optional[str] = None):
    """Decode *in_path* (MP3/AAC/Opus/WAV), optionally resample (*rate*)
    and apply *gain_db*, then encode to *out_path*. The output codec is
    chosen from the extension (.mp3 / .aac / .opus / .wav). Opus output is
    always at 48 kHz (auto-resampled)."""
    pcm, sr, ch = decode_file(in_path)
    if _HAS_NUMPY and isinstance(pcm, np.ndarray):
        flat = pcm.reshape(-1)
    else:
        flat = list(pcm)
    if rate and rate != sr:
        flat = resample(flat, sr, rate, ch)
        sr = rate
    if gain_db != 0.0:
        g = 10.0 ** (gain_db / 20.0)
        if _HAS_NUMPY and isinstance(flat, np.ndarray):
            flat = flat * g
        else:
            flat = [x * g for x in flat]
    ext = out_path.lower().rsplit(".", 1)[-1]
    if ext == "wav":
        write_wav(out_path, flat, sr, ch)
        return
    if ext not in _FORMAT_BY_NAME:
        raise GlintError(f"unsupported output extension .{ext}")
    # encode_audio auto-resamples to a codec-valid rate; one call for all.
    data = encode_audio(flat, ch, sr, ext, bitrate=bitrate,
                        lib=(load_library(lib_path) if lib_path else None))
    with open(out_path, "wb") as f:
        f.write(data)
