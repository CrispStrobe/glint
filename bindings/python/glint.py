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
        )

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
