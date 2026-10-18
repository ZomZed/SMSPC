/*
 * port_audio.h - host audio for the PC port.
 *
 *   sdk_audio.cpp        AR/ARQ/AI/DSP SDK entry points and the DSP mail protocol
 *                        JAudio's microcode speaks (dsptask.c / dspproc.c).
 *   port_audio_dsp.cpp   the host "microcode": renders JAudio's DSP voice blocks
 *                        (JASystem::DSPInterface::DSPBuffer) and FX lines into the
 *                        left/right mix buffers JAudio hands over with DsyncFrame.
 *   port_audio_out.cpp   SDL3 output of the AI DMA buffers, WAV capture
 *                        (SMS_PORT_WAV), and the extra PCM stream API below.
 */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- DSP (port_audio_dsp.cpp), called from sdk_audio.cpp's mail handler ---- */

/* Command 0x81 (DsetupTable): voice blocks, resampling filter table, ADPCM
 * coefficient table, FX line blocks. All are main-RAM addresses (< 4 GB). */
void port_dsp_setup(uint32_t numVoices, uint32_t voices, uint32_t resFilter, uint32_t adpcmFilter,
                    uint32_t fxLines);
/* Command 0x82 (DsyncFrame): the next frame has `subframes` sub-frames that go to
 * the left/right buffers (native s16, subframes * 80 samples each); mixerLevel is
 * the final output gain in 4.12 fixed point. */
void port_dsp_sync_frame(uint32_t subframes, uint32_t left, uint32_t right, uint32_t mixerLevel);
/* DSPReleaseHalt: render one 80-sample sub-frame of every voice whose bit is set
 * in `map` (bit 63 = voice 0, as DSP_CreateMap builds it). */
void port_dsp_render_subframe(uint64_t map);

/* 5.1 / 7.1: the discrete channels (FL, FR, FC, LFE, RL, RR, interleaved, at the
 * output level) the renderer kept for the DSP output buffer whose left half is
 * dspL, or null. */
const int16_t* port_dsp_mc_shadow(const int16_t* dspL, uint32_t* frames);

/* ---- Output mode (port_audio_out.cpp) ----
 * PORT_AUDIO_STEREO: plain stereo whatever the game's Sound option says.
 * PORT_AUDIO_ORIGINAL: the game's option decides (surround = Pro Logic II matrix-
 *   encoded stereo, as on the GameCube). Default.
 * PORT_AUDIO_51 / PORT_AUDIO_71: discrete channels from the DSP buses before the
 *   matrix; the game is switched to surround mixing internally (the saved option is
 *   untouched); stereo device = stereo fallback, the WAV capture keeps all channels. */
enum { PORT_AUDIO_STEREO = 0, PORT_AUDIO_ORIGINAL = 1, PORT_AUDIO_51 = 2, PORT_AUDIO_71 = 3 };
void port_audio_set_output_mode(int mode);
/* Human-readable state for the settings menu. */
const char* port_audio_output_status(void);
/* 6 or 8 while discrete channels are produced (5.1 / 7.1 chosen), else 2. */
int port_audio_mc_channels(void);
/* The JAudio sound output mode to use (0 mono, 1 stereo, 2 surround) when the game
 * asks for gameMode (JAIGlobalParameter::setParamSoundOutputMode). */
int port_audio_game_output_mode(int gameMode);
/* JASAiCtrl vframeWork: the AI buffer dac (AI order R, L) was built from the DSP
 * buffers dspL / dspR plus any extra mix (THP movie sound): pair the discrete
 * channels with it; the extra mix goes to FL/FR. */
void port_audio_mc_bind(const int16_t* dspL, const int16_t* dspR, const int16_t* dac, uint32_t frames);

/* ARAM (sdk_audio.cpp): host pointer for an ARAM address, and the ARAM size. */
uint8_t* port_aram_ptr(uint32_t addr);
uint32_t port_aram_size(void);

/* ---- Output (port_audio_out.cpp) ---- */

/* One AI DMA buffer has started playing: `frames` stereo frames in GameCube AI
 * order (right, left), native-endian s16, at `rate` Hz. Called from the AI
 * interrupt. */
void port_audio_out_ai(const int16_t* rl, uint32_t frames, uint32_t rate);
/* Position of the SMS_PORT_WAV capture in seconds (0 without one); debug logs. */
double port_audio_out_wav_seconds(void);
/* Period for the next AI interrupt: nominalNs, nudged by at most +-2 % to keep the
 * host audio queue near its target (the AI interrupt stays the game's clock). */
long long port_audio_out_next_period(long long nominalNs);

/* Extra PCM stream for code that produces its own audio (THP movie sound, w8):
 * `frames` interleaved stereo frames (left, right), native-endian s16, at `rate`
 * Hz. Mixed with the game output by SDL; safe to call from any thread. A rate
 * change restarts the stream. Does nothing when audio output is off. */
void port_audio_submit_stream(const int16_t* samples, int frames, int rate);
/* Drops anything queued on the extra stream (e.g. when a movie is skipped). */
void port_audio_clear_stream(void);

#ifdef __cplusplus
}
#endif

#endif
