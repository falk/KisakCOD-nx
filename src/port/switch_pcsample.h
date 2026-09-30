#pragma once

// Statistical PC/stack sampler for hardware perf runs; see switch_pcsample.cpp.
// `+set switch_pcSample <hz>` turns it on; 0 (default) costs nothing.

// Adds the calling thread to the sampled set; `tag` identifies it in the
// output (the engine uses its ThreadContext_t).
void SwitchPcSample_RegisterCurrentThread(int tag);

// Tags for port threads that have no ThreadContext_t (the engine's run
// 0..11); offline tooling names them.
constexpr int kSwitchPcSampleTagSndMix = 20;    // audren update thread
constexpr int kSwitchPcSampleTagSndStream = 21; // streamed-sound decode thread

// Samples per second; starts the sampler thread on the first non-zero rate.
void SwitchPcSample_SetRate(int hz);
