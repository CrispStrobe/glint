// glint - AAC storage types (shared by the AAC module headers)
// MIT License - Clean-room implementation

#ifndef GLINT_AAC_CODER_TYPES_FWD_HPP
#define GLINT_AAC_CODER_TYPES_FWD_HPP

#include <cstdint>

namespace glint {
namespace aac {

// Storage types. Under GLINT_SMALL_BUFFERS spectra are stored as float and
// PCM blocks as int16 (arithmetic stays double everywhere); this roughly
// halves the encoder context. Desktop builds keep double storage.
#ifdef GLINT_SMALL_BUFFERS
using SpecT = float;
using PcmT = int16_t;
#else
using SpecT = double;
using PcmT = double;
#endif

}  // namespace aac
}  // namespace glint

#endif  // GLINT_AAC_CODER_TYPES_FWD_HPP
