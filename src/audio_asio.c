/* Áudio pelo ASIO, via PortAudio. Ver audio_asio.h. */
#include "audio_asio.h"

#include <portaudio.h>
#include <pa_asio.h>
#include <stdio.h>
#include <string.h>

#define MAX_DRIVERS 16

static struct {
    int         init;
    int         n;
    PaDeviceIndex dev[MAX_DRIVERS];
    PaStream   *stream;
    PaDeviceIndex open_dev;
    int         in_ch;
    AsioProcess cb;
    void       *user;
    char        err[256];
} A;

static void ensure_init(void)
{
    if (A.init) return;
    if (Pa_Initialize() != paNoError) return;
    A.init = 1;
    PaHostApiIndex api = Pa_HostApiTypeIdToHostApiIndex(paASIO);
    if (api < 0) return;
    const PaHostApiInfo *hi = Pa_GetHostApiInfo(api);
    for (int k = 0; k < hi->deviceCount && A.n < MAX_DRIVERS; k++)
        A.dev[A.n++] = Pa_HostApiDeviceIndexToDeviceIndex(api, k);
}

int asio_driver_count(void)
{
    ensure_init();
    return A.n;
}

const char *asio_driver_name(int i)
{
    if (i < 0 || i >= A.n) return "";
    const PaDeviceInfo *d = Pa_GetDeviceInfo(A.dev[i]);
    return d ? d->name : "";
}

static int pa_cb(const void *in, void *out, unsigned long frames, const PaStreamCallbackTimeInfo *t,
                 PaStreamCallbackFlags flags, void *user)
{
    (void)t; (void)flags; (void)user;
    A.cb(A.user, (const float *)in, in ? A.in_ch : 0, (float *)out, (int)frames);
    return paContinue;
}

int asio_open(int i, double sr, int frames, int want_input, AsioProcess cb, void *user, int *frames_out)
{
    asio_close();
    ensure_init();
    A.err[0] = 0;
    if (i < 0 || i >= A.n) { snprintf(A.err, sizeof A.err, "Driver ASIO não encontrado"); return -1; }
    PaDeviceIndex dev = A.dev[i];
    const PaDeviceInfo *d = Pa_GetDeviceInfo(dev);
    if (!d || d->maxOutputChannels < 2) { snprintf(A.err, sizeof A.err, "O driver ASIO não tem saída estéreo"); return -1; }

    /* bloco: respeita o pedido se o driver aceitar, senão usa o preferido dele */
    long mn, mx, pref, gran;
    if (PaAsio_GetAvailableBufferSizes(dev, &mn, &mx, &pref, &gran) == paNoError) {
        if (frames <= 0) frames = (int)pref;
        if (frames < mn) frames = (int)mn;
        if (frames > mx) frames = (int)mx;
        if (gran > 0 && (frames - mn) % gran) frames = (int)(mn + ((frames - mn) / gran + 1) * gran);
        if (gran == -1) { /* só potências de 2 */
            long p = mn;
            while (p < frames && p < mx) p *= 2;
            frames = (int)p;
        }
    }

    PaStreamParameters op = {dev, 2, paFloat32, (double)frames / sr, NULL};
    PaStreamParameters ip = {dev, d->maxInputChannels, paFloat32, (double)frames / sr, NULL};
    int has_in = want_input && d->maxInputChannels > 0;
    if (ip.channelCount > 32) ip.channelCount = 32;

    A.cb = cb;
    A.user = user;
    A.in_ch = has_in ? ip.channelCount : 0;
    PaError r = Pa_OpenStream(&A.stream, has_in ? &ip : NULL, &op, sr, (unsigned long)frames, paClipOff, pa_cb, NULL);
    if (r == paInvalidSampleRate)
        snprintf(A.err, sizeof A.err, "O driver ASIO não aceitou %.0f Hz. Mude a taxa no painel do driver.", sr);
    else if (r != paNoError)
        snprintf(A.err, sizeof A.err, "ASIO: %s", Pa_GetErrorText(r));
    if (r != paNoError) { A.stream = NULL; A.in_ch = 0; return -1; }
    if (Pa_StartStream(A.stream) != paNoError) {
        snprintf(A.err, sizeof A.err, "O driver ASIO abriu mas não começou a tocar");
        Pa_CloseStream(A.stream);
        A.stream = NULL;
        return -1;
    }
    A.open_dev = dev;
    if (frames_out) *frames_out = frames;
    return 0;
}

void asio_close(void)
{
    if (!A.stream) return;
    Pa_StopStream(A.stream);
    Pa_CloseStream(A.stream);
    A.stream = NULL;
    A.in_ch = 0;
}

int         asio_input_channels(void) { return A.stream ? A.in_ch : 0; }
const char *asio_error(void) { return A.err; }

void asio_control_panel(void)
{
    if (!A.init || A.n == 0) return;
    PaAsio_ShowControlPanel(A.stream ? A.open_dev : A.dev[0], NULL);
}

void asio_shutdown(void)
{
    asio_close();
    if (A.init) Pa_Terminate();
    A.init = 0;
    A.n = 0;
}
