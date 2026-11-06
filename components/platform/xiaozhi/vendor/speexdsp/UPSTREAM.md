# Fixed-point echo cancellation subset

Source: https://github.com/xiph/speexdsp/tree/SpeexDSP-1.2.1
Commit:1b28a0f61bc31162979e1f26f3981fc3637095c8. Original source files are unchanged. COPYING and per-file notices apply. Only mono fixed-point MDF/KISS FFT dependency closure is compiled; no resampler, neural models, raw recording or automatic gain control.

Local integration supplies speexdsp_config_types.h and os_support_custom.h. All library allocations are caught inside one bounded workspace; allocation or fatal failure unwinds to xz_echo instead of dereferencing NULL/aborting. Library arithmetic uses intended two's-complement wrap (-fwrapv) and signed-shift sanitizer exclusion; integration/state tests retain full UBSan. This numerical kernel is not evidence of physical echo-cancellation acceptance.
