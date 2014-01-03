/*
 * This file is part of mpv.
 *
 * Original author: Martin Herkt <lachs0r@srsfckn.biz>
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "options/options.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "osdep/endian.h"

#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API

#include <alsa/asoundlib.h>

#include "ao.h"
#include "internal.h"
#include "audio/format.h"
#include "audio/chmap.h"

#define ALSA(msg, err) \
{ \
    if (err < 0) { \
        MP_ERR(ao, "%s: %s\n", (msg), snd_strerror(err)); \
        goto bail; \
    } \
}

struct priv {
    snd_pcm_t *pcm;
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int can_pause;
    float delay_before_pause;
    snd_pcm_sframes_t prepause_frames;

    char *device;
    char *mixer_device;
    char *mixer_name;
    int mixer_index;
    int resample;
    snd_pcm_format_t format;
};

static const int alsa_to_mp_channels[][2] = {
    {SND_CHMAP_FL,      MP_SP(FL)},
    {SND_CHMAP_FR,      MP_SP(FR)},
    {SND_CHMAP_RL,      MP_SP(BL)},
    {SND_CHMAP_RR,      MP_SP(BR)},
    {SND_CHMAP_FC,      MP_SP(FC)},
    {SND_CHMAP_LFE,     MP_SP(LFE)},
    {SND_CHMAP_SL,      MP_SP(SL)},
    {SND_CHMAP_SR,      MP_SP(SR)},
    {SND_CHMAP_RC,      MP_SP(BC)},
    {SND_CHMAP_FLC,     MP_SP(FLC)},
    {SND_CHMAP_FRC,     MP_SP(FRC)},
    {SND_CHMAP_FLW,     MP_SP(WL)},
    {SND_CHMAP_FRW,     MP_SP(WR)},
    {SND_CHMAP_TC,      MP_SP(TC)},
    {SND_CHMAP_TFL,     MP_SP(TFL)},
    {SND_CHMAP_TFR,     MP_SP(TFR)},
    {SND_CHMAP_TFC,     MP_SP(TFC)},
    {SND_CHMAP_TRL,     MP_SP(TBL)},
    {SND_CHMAP_TRR,     MP_SP(TBR)},
    {SND_CHMAP_TRC,     MP_SP(TBC)},
    {SND_CHMAP_MONO,    MP_SP(FC)},
    {SND_CHMAP_LAST,    MP_SP(UNKNOWN_LAST)}
};

static const int mp_to_alsa_format[][2] = {
    {AF_FORMAT_S8,      SND_PCM_FORMAT_S8},
    {AF_FORMAT_U8,      SND_PCM_FORMAT_U8},
    {AF_FORMAT_U16,     SND_PCM_FORMAT_U16},
    {AF_FORMAT_S16,     SND_PCM_FORMAT_S16},
    {AF_FORMAT_U32,     SND_PCM_FORMAT_U32},
    {AF_FORMAT_S32,     SND_PCM_FORMAT_S32},
    {AF_FORMAT_U24,     MP_SELECT_LE_BE(SND_PCM_FORMAT_U24_3LE,
                                        SND_PCM_FORMAT_U24_3BE)},
    {AF_FORMAT_S24,     MP_SELECT_LE_BE(SND_PCM_FORMAT_S24_3LE,
                                        SND_PCM_FORMAT_S24_3BE)},
    {AF_FORMAT_FLOAT,   SND_PCM_FORMAT_FLOAT},
    {AF_FORMAT_UNKNOWN, SND_PCM_FORMAT_UNKNOWN}
};

static int find_alsa_format(int af_format)
{
    af_format = af_fmt_from_planar(af_format);

    for (int n = 0; mp_to_alsa_format[n][0] != AF_FORMAT_UNKNOWN; n++) {
        if (mp_to_alsa_format[n][0] == af_format)
            return mp_to_alsa_format[n][1];
    }

    return SND_PCM_FORMAT_UNKNOWN;
}

static int find_mp_channel(int alsa_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SP(UNKNOWN_LAST); i++) {
        if (alsa_to_mp_channels[i][0] == alsa_channel)
            return alsa_to_mp_channels[i][1];
    }

    return MP_SP(UNKNOWN_LAST);
}

static int find_alsa_channel(int mp_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SP(UNKNOWN_LAST); i++) {
        if (alsa_to_mp_channels[i][1] == mp_channel)
            return alsa_to_mp_channels[i][0];
    }

    return SND_CHMAP_UNKNOWN;
}

static int query_chmaps(struct ao *ao)
{
    struct priv *p = ao->priv;
    struct mp_chmap_sel chmap_sel = {0};

    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(p->pcm);

    for (int i = 0; maps[i] != NULL; i++) {
        struct mp_chmap chmap = {0};

        chmap.num = maps[i]->map.channels;
        for (int c = 0; c < chmap.num; c++) {
            chmap.speaker[c] = find_mp_channel(maps[i]->map.pos[c]);
        }

        char *chmap_str = mp_chmap_to_str(&chmap);
        MP_DBG(ao, "Got supported channel map: %s (type %s)\n",
               chmap_str,
               snd_pcm_chmap_type_name(maps[i]->type));
        talloc_free(chmap_str);

        mp_chmap_sel_add_map(&chmap_sel, &chmap);
    }

    snd_pcm_free_chmaps(maps);

    return ao_chmap_sel_adjust(ao, &chmap_sel, &ao->channels);
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->pcm) {
        ALSA("Cannot drop audio data", snd_pcm_drop(p->pcm));
        ALSA("Cannot close audio device", snd_pcm_close(p->pcm));

        MP_VERBOSE(ao, "Uninit finished\n");
    }

bail:
    p->pcm = NULL;
}

static void drain(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_drain(p->pcm);
}

static int init(struct ao *ao)
{
    int err;
    struct priv *p = ao->priv;
    const char *device;

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_sw_params_t *swparams;

    if (p->device && p->device[0])
        device = p->device;
    else
        device = "default";

    p->delay_before_pause = 0;
    p->prepause_frames = 0;

    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_sw_params_alloca(&swparams);

    ALSA("Failed to open audio device",
         snd_pcm_open(&p->pcm, device, SND_PCM_STREAM_PLAYBACK, 0));

    ALSA("No usable playback configuration found",
         snd_pcm_hw_params_any(p->pcm, hwparams));

    ALSA("Resampling setup failed",
         snd_pcm_hw_params_set_rate_resample(p->pcm, hwparams, p->resample));

    snd_pcm_access_t access = af_fmt_is_planar(ao->format)
                              ? SND_PCM_ACCESS_RW_NONINTERLEAVED
                              : SND_PCM_ACCESS_RW_INTERLEAVED;

    err = snd_pcm_hw_params_set_access(p->pcm, hwparams, access);
    if (err < 0 && access == SND_PCM_ACCESS_RW_NONINTERLEAVED) {
        MP_INFO(ao, "Non-interleaved access not available\n");
        access = SND_PCM_ACCESS_RW_INTERLEAVED;
        err = snd_pcm_hw_params_set_access(p->pcm, hwparams, access);
    }
    ALSA("Access type setup failed", err);

    p->format = find_alsa_format(ao->format);
    if (p->format == SND_PCM_FORMAT_UNKNOWN) {
        MP_INFO(ao, "Format %s is not known to ALSA, trying default",
                af_fmt_to_str(ao->format));

        p->format = SND_PCM_FORMAT_S16;
        ao->format = AF_FORMAT_S16;
    }

    err = snd_pcm_hw_params_test_format(p->pcm, hwparams, p->format);
    if (err < 0) {
        MP_INFO(ao, "Format %s is not supported by hardware, trying default",
                af_fmt_to_str(ao->format));

        p->format = SND_PCM_FORMAT_S16;
        ao->format = AF_FORMAT_S16;
    }
    ALSA("Format setup failed",
         snd_pcm_hw_params_set_format(p->pcm, hwparams, p->format));

    struct mp_chmap oldmap = ao->channels;
    int invalid_map = 0;
    if (!query_chmaps(ao)) {
        MP_WARN(ao, "Did not get a valid channel map from ALSA\n");
        ao->channels = oldmap;
        invalid_map = 1;
    } else if (ao->channels.num != oldmap.num) {
        MP_WARN(ao, "Requested map with %i channels, got %i instead\n",
                oldmap.num, ao->channels.num);
    }

    ALSA("Channel count setup failed",
         snd_pcm_hw_params_set_channels(p->pcm, hwparams, ao->channels.num));

    ALSA("Samplerate setup failed",
         snd_pcm_hw_params_set_rate_near(p->pcm, hwparams,
                                         &ao->samplerate, NULL));

    ALSA("Unable to set hardware parameters",
         snd_pcm_hw_params(p->pcm, hwparams));

    if (!invalid_map) {
        char tmp[128];
        snd_pcm_chmap_t *alsa_chmap = snd_pcm_get_chmap(p->pcm);

        for (int c = 0; c < ao->channels.num; c++) {
            alsa_chmap->pos[c] = find_alsa_channel(ao->channels.speaker[c]);
        }

        if (snd_pcm_chmap_print(alsa_chmap, sizeof(tmp), tmp) > 0)
            MP_DBG(ao, "Attempting to set channel map: %s\n", tmp);

        err = snd_pcm_set_chmap(p->pcm, alsa_chmap);

        if (err == -ENXIO) {
            MP_ERR(ao, "Device does not support requested channel map\n");
            goto bail;
        } else {
            ALSA("Channel map setup failed", err);
        }
    }

    ALSA("Unable to get buffer size",
         snd_pcm_hw_params_get_buffer_size(hwparams, &p->buffer_size));

    ALSA("Unable to get period size",
         snd_pcm_hw_params_get_period_size(hwparams, &p->period_size, NULL));

    p->can_pause = snd_pcm_hw_params_can_pause(hwparams);

    return 0;

bail:
    uninit(ao);
    return -1;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->prepause_frames = 0;
    p->delay_before_pause = 0;
    ALSA("Cannot drop audio data", snd_pcm_drop(p->pcm));
    ALSA("Cannot prepare audio device", snd_pcm_prepare(p->pcm));

bail: ;
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    snd_mixer_t *mixer = NULL;

    switch (cmd) {
        case AOCONTROL_GET_MUTE:
        case AOCONTROL_SET_MUTE:
        case AOCONTROL_GET_VOLUME:
        case AOCONTROL_SET_VOLUME:
        {
            snd_mixer_elem_t *elem;
            snd_mixer_selem_id_t *sid;

            long pmin, pmax;
            float multi;

            snd_mixer_selem_id_alloca(&sid);
            snd_mixer_selem_id_set_index(sid, p->mixer_index);
            snd_mixer_selem_id_set_name(sid, p->mixer_name);

            ALSA("Cannot open mixer", snd_mixer_open(&mixer, 0));
            ALSA("Cannot attach mixer",
                 snd_mixer_attach(mixer, p->mixer_device));
            ALSA("Cannot register mixer",
                 snd_mixer_selem_register(mixer, NULL, NULL));
            ALSA("Cannot load mixer", snd_mixer_load(mixer));

            elem = snd_mixer_find_selem(mixer, sid);
            if (!elem) {
                MP_VERBOSE(ao, "Unable to find simple mixer control '%s' "
                           "(index %i)\n",
                           snd_mixer_selem_id_get_name(sid),
                           snd_mixer_selem_id_get_index(sid));
                goto bail;
            }

            snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
            multi = (100 / (float)(pmax - pmin));

            switch (cmd) {
                case AOCONTROL_GET_MUTE: {
                    if (!snd_mixer_selem_has_playback_switch(elem))
                        goto bail;

                    bool *mute = arg;
                    int tmp = 1;

                    snd_mixer_selem_get_playback_switch(elem,
                                                        SND_MIXER_SCHN_MONO,
                                                        &tmp);

                    *mute = !tmp;
                    break;
                }
                case AOCONTROL_SET_MUTE: {
                    if (!snd_mixer_selem_has_playback_switch(elem))
                        goto bail;

                    bool *mute = arg;

                    snd_mixer_selem_set_playback_switch_all(elem, !*mute);
                    break;
                }
                case AOCONTROL_GET_VOLUME: {
                    ao_control_vol_t *vol = arg;
                    long alsa_vol;
                    snd_mixer_selem_get_playback_volume
                        (elem, SND_MIXER_SCHN_FRONT_LEFT, &alsa_vol);
                    vol->left = (alsa_vol - pmin) * multi;
                    snd_mixer_selem_get_playback_volume
                        (elem, SND_MIXER_SCHN_FRONT_RIGHT, &alsa_vol);
                    vol->right = (alsa_vol - pmin) * multi;
                    break;
                }
                case AOCONTROL_SET_VOLUME: {
                    ao_control_vol_t *vol = arg;

                    long alsa_vol = vol->left / multi + pmin + 0.5;
                    ALSA("Cannot set left channel volume",
                         snd_mixer_selem_set_playback_volume
                            (elem, SND_MIXER_SCHN_FRONT_LEFT, alsa_vol));

                    alsa_vol = vol->right / multi + pmin + 0.5;
                    ALSA("Cannot set right channel volume",
                         snd_mixer_selem_set_playback_volume
                            (elem, SND_MIXER_SCHN_FRONT_RIGHT, alsa_vol));
                    break;
                }
            }

            snd_mixer_close(mixer);
            return CONTROL_OK;
        }
    }

    return CONTROL_UNKNOWN;

bail:
    if (mixer)
        snd_mixer_close(mixer);

    return CONTROL_ERROR;
}

static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t res = 0;

    if (!(flags & AOPLAY_FINAL_CHUNK))
        samples = samples / p->period_size * p->period_size;

    if (samples == 0)
        return 0;

    do {
        int recovered = 0;
retry:
        if (af_fmt_is_planar(ao->format)) {
            res = snd_pcm_writen(p->pcm, data, samples);
        } else {
            res = snd_pcm_writei(p->pcm, data[0], samples);
        }

        if (res < 0) {
            switch (res) {
                case -EINTR:
                case -EPIPE:
                case -ESTRPIPE:
                    if (!recovered) {
                        recovered++;
                        MP_WARN(ao, "Write failed: %s; trying to recover\n",
                                snd_strerror(res));

                        res = snd_pcm_recover(p->pcm, res, 1);
                        if (!res || res == -EAGAIN)
                            goto retry;
                    }
                    break;
            }
        }
    } while (res == 0);

    return res < 0 ? -1 : res;
}

static float get_delay(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t delay;

    if (snd_pcm_state(p->pcm) == SND_PCM_STATE_PAUSED)
        return p->delay_before_pause;

    if (snd_pcm_delay(p->pcm, &delay) < 0)
        return 0;

    if (delay < 0) {
        snd_pcm_forward(p->pcm, -delay);
        delay = 0;
    }

    return (float)delay / (float)ao->samplerate;
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->can_pause) {
        snd_pcm_state_t state = snd_pcm_state(p->pcm);

        switch (state) {
            case SND_PCM_STATE_PREPARED:
                break;
            case SND_PCM_STATE_RUNNING:
                ALSA("Device not ready", snd_pcm_wait(p->pcm, -1));
                p->delay_before_pause = get_delay(ao);
                ALSA("Pause failed", snd_pcm_pause(p->pcm, 1));
                break;
            default:
                MP_ERR(ao, "Device in bad state while pausing\n");
                goto bail;
        }
    } else {
        MP_VERBOSE(ao, "Pause not supported by hardware\n");

        if (snd_pcm_delay(p->pcm, &p->prepause_frames) < 0
            || p->prepause_frames < 0) {

            p->prepause_frames = 0;
        }

        ALSA("Cannot drop audio data", snd_pcm_drop(p->pcm));
    }

bail: ;
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->can_pause) {
        snd_pcm_state_t state = snd_pcm_state(p->pcm);

        switch(state) {
            case SND_PCM_STATE_PREPARED:
                break;
            case SND_PCM_STATE_PAUSED:
                ALSA("Device not ready", snd_pcm_wait(p->pcm, -1));
                ALSA("Unpause failed", snd_pcm_pause(p->pcm, 0));
                break;
            default:
                MP_ERR(ao, "Device in bad state while unpausing\n");
                goto bail;
        }
    } else {
        MP_VERBOSE(ao, "Unpause not supported by hardware\n");
        ALSA("Cannot prepare audio device for playback",
             snd_pcm_prepare(p->pcm));

        if (p->prepause_frames)
            ao_play_silence(ao, p->prepause_frames);
    }

bail: ;
}

static int get_space(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_status_t *status;

    snd_pcm_status_alloca(&status);
    ALSA("Cannot get pcm status", snd_pcm_status(p->pcm, status));

    snd_pcm_sframes_t space = snd_pcm_status_get_avail(status);
    if (space > p->buffer_size)
        space = p->buffer_size;

    return space / p->period_size * p->period_size;

bail:
    return 0;
}

#define MAX_POLL_FDS 20
static int audio_wait(struct ao *ao, pthread_mutex_t *lock)
{
    struct priv *p = ao->priv;

    int num_fds = snd_pcm_poll_descriptors_count(p->pcm);
    if (num_fds <= 0 || num_fds >= MAX_POLL_FDS)
        goto bail;

    struct pollfd fds[MAX_POLL_FDS];
    ALSA("Cannot get pollfds", snd_pcm_poll_descriptors(p->pcm, fds, num_fds));

    while (1) {
        int r = ao_wait_poll(ao, fds, num_fds, lock);
        if (r)
            return r;

        unsigned short revents;
        ALSA("Cannot read poll events",
             snd_pcm_poll_descriptors_revents(p->pcm, fds, num_fds, &revents));

        if (revents & POLLERR)
            return -1;
        if (revents & POLLOUT)
            return 0;
    }
    return 0;

bail:
    return -1;
}

static void list_devs(struct ao *ao, struct ao_device_list *list)
{
    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;

    for (int n = 0; hints[n]; n++) {
        char *name = snd_device_name_get_hint(hints[n], "NAME");
        char *desc = snd_device_name_get_hint(hints[n], "DESC");
        char *io = snd_device_name_get_hint(hints[n], "IOID");
        if (io && strcmp(io, "Output") != 0)
            continue;
        char desc2[1024];
        snprintf(desc2, sizeof(desc2), "%s", desc ? desc : "");
        for (int i = 0; desc2[i]; i++) {
            if (desc2[i] == '\n')
                desc2[i] = '/';
        }
        ao_device_list_add(list, ao, &(struct ao_device_desc){name, desc2});
    }

    snd_device_name_free_hint(hints);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_alsa_ng = {
    .description    = "ALSA audio output",
    .name           = "alsa_ng",
    .init           = init,
    .drain          = drain,
    .uninit         = uninit,
    .reset          = reset,
    .control        = control,
    .play           = play,
    .pause          = audio_pause,
    .resume         = audio_resume,
    .get_space      = get_space,
    .get_delay      = get_delay,
    .wait           = audio_wait,
    .wakeup         = ao_wakeup_poll,
    .list_devs      = list_devs,
    .priv_size      = sizeof(struct priv),
    .priv_defaults  = &(const struct priv) {
        .device = "default",
        .mixer_device = "default",
        .mixer_name = "Master",
        .mixer_index = 0
    },
    .options = (const struct m_option[]) {
        OPT_STRING("device", device, 0),
        OPT_STRING("mixer-device", mixer_device, 0),
        OPT_STRING("mixer-name", mixer_name, 0),
        OPT_INTRANGE("mixer-index", mixer_index, 0, 0, 99),
        OPT_FLAG("resample", resample, 0),
        {0}
    }
};
