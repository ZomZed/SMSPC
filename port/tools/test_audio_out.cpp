// Host audio output regressions. Includes the implementation so the test can
// capture PCM without opening a device; the public bind/output path is exercised.
#include "../src/port_audio_out.cpp"
#include <cassert>
#include <initializer_list>

static int16_t testShadow[12];
extern "C" const int16_t* port_dsp_mc_shadow(const int16_t*, uint32_t* frames)
{
    *frames = 2;
    return testShadow;
}
extern "C" void port_jai_reapply_output_mode(void) {}

int main()
{
    int16_t dspL[2] = {}, dspR[2] = {}, dac[3][4] = {};
    // Keep capture independent of environment settings and physical devices.
    gInitDone = true;
    for (int mode : {PORT_AUDIO_51, PORT_AUDIO_71}) {
        port_audio_set_output_mode(mode);
        gWavCh = mode == PORT_AUDIO_51 ? 6 : 8;
        gWavRate = 32000;
        gWav = std::tmpfile();
        assert(gWav);
        // The game rotates three DAC buffers, whereas the cache has four slots.
        for (int generation = 1; generation <= 24; ++generation) {
            for (int i = 0; i < 12; ++i)
                testShadow[i] = (int16_t)(generation * 100 + i);
            auto* key = dac[(generation - 1) % 3];
            port_audio_mc_bind(dspL, dspR, key, 2);
            port_audio_out_ai(key, 2, 32000);
            std::fflush(gWav);
            std::fseek(gWav, -(long)(2 * gWavCh * sizeof(int16_t)), SEEK_CUR);
            int16_t actual[16] = {};
            assert(std::fread(actual, sizeof(int16_t), 2 * gWavCh, gWav) == 2 * gWavCh);
            std::fseek(gWav, 0, SEEK_END); // switch the update stream back to writing
            for (unsigned frame = 0; frame < 2; ++frame) {
                for (unsigned ch = 0; ch < gWavCh; ++ch) {
                    int expected;
                    if (gWavCh == 8 && ch >= 4)
                        expected = testShadow[frame * 6 + 4 + (ch % 2)] * 23170 / 32768;
                    else
                        expected = testShadow[frame * 6 + ch];
                    if (actual[frame * gWavCh + ch] != expected) {
                        std::fprintf(stderr, "FAIL: %u-channel PCM generation %d, frame %u, channel %u: got %d, expected %d\n",
                            gWavCh, generation, frame, ch, actual[frame * gWavCh + ch], expected);
                        return 1;
                    }
                }
            }
        }
        std::fclose(gWav);
        gWav = nullptr;
        std::printf("PASS: %u-channel PCM from three rotating DAC buffers across 24 generations\n", gWavCh);
    }
    return 0;
}
