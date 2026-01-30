#include "audio_processing.h"
#include "../application/audio_processing.h"

AUDIO_PROCESS_HDL *audio_process_init(uint32_t sampleRate, uint32_t channel)
{
    AUDIO_PROCESS_HDL *audio_hdl = (AUDIO_PROCESS_HDL *)aup_calloc(1, sizeof(AUDIO_PROCESS_HDL));
    if(!audio_hdl)
        goto audio_process_init_err;
    audio_hdl->channel = channel;
    audio_hdl->sampleRate = sampleRate;
    audio_hdl->frame_buf = NULL;

    audio_hdl->frame_buf = (int16_t *)aup_malloc(FRAME_LEN*sizeof(int16_t));
    if (!audio_hdl->frame_buf) {
        goto audio_process_init_err;
    }    
    
#if AECM_PROCESSING || NSX_PROCESSING || HS_PROCESSING
    audio_hdl->time_signal = (int16_t *)aup_calloc(1, (PART_LEN2+16)*sizeof(int16_t));
    if(!audio_hdl->time_signal) {
        goto audio_process_init_err;
    }

    audio_hdl->freq_buf = (int32_t *)aup_calloc(1, (PART_LEN2+8)*sizeof(int32_t));
    if(!audio_hdl->freq_buf) {
        goto audio_process_init_err;
    }
    audio_hdl->freq_signal = (fftInt16 *)audio_hdl->freq_buf;

    audio_hdl->time_outbuf = (int16_t *)aup_calloc(1, (PART_LEN2+8)*sizeof(int16_t));
    if(!audio_hdl->time_outbuf) {
        goto audio_process_init_err;
    }

    audio_hdl->real_fft = Aup_CreateRealFFT(FFT_ORDER);
    if (!audio_hdl->real_fft) {
        goto audio_process_init_err;
    }    
#endif

#if AECM_PROCESSING
    audio_hdl->aecm = aecm_init(sampleRate, ECHOMODE, COMFORTNOISE);
    if(!audio_hdl->aecm)
        goto audio_process_init_err;
#endif

#if HS_PROCESSING
    audio_hdl->hs = hs_init(sampleRate);
    if(!audio_hdl->hs)
        goto audio_process_init_err;
#endif

#if NSX_PROCESSING
    audio_hdl->nsx = nsx_init(sampleRate);
    if(!audio_hdl->nsx)
        goto audio_process_init_err;
#endif

#if VAD_PROCESSING || AGC_PROCESSING
    audio_hdl->vad = vad_init(sampleRate, 1); //aggressive:0-3
    if(!audio_hdl->vad)
        goto audio_process_init_err;
#endif   

#if AGC_PROCESSING
    audio_hdl->agc = agc_init(MAXGAINDB, TARGETDB);
    if(!audio_hdl->agc)
        goto audio_process_init_err;
#endif   

    return audio_hdl;

audio_process_init_err:

    if(audio_hdl->aecm)
        aecm_deinit(audio_hdl->aecm);
    if(audio_hdl->hs)
        hs_deinit(audio_hdl->hs);
    if(audio_hdl->nsx)
        nsx_deinit(audio_hdl->nsx);
    if(audio_hdl->vad)
        vad_deinit(audio_hdl->vad);
    if(audio_hdl->agc)
        agc_deinit(audio_hdl->agc);

    if(audio_hdl->real_fft)
       Aup_FreeRealFFT(audio_hdl->real_fft);

    if(audio_hdl->frame_buf)
        aup_free(audio_hdl->frame_buf);
    if(audio_hdl)
        aup_free(audio_hdl);
    return NULL;
}

int audio_process(AUDIO_PROCESS_HDL *audio_hdl, int16_t *in_buf, uint32_t samples_len)
{
    int ret = -1;
    uint32_t res_samples = samples_len;
    uint32_t write_samples = 0;
    int16_t max_value = 0;
    static uint32_t cnt = 0;

    while(res_samples >= FRAME_LEN) {
        cnt++;
        // if(cnt>400)
        // break;
        aup_memcpy(audio_hdl->frame_buf, in_buf+write_samples, FRAME_LEN2);
#if AECM_PROCESSING || NSX_PROCESSING || HS_PROCESSING
        audio_hdl->time_signal_scaling = 0;
        aup_memcpy(audio_hdl->time_signal+PART_LEN2-FRAME_LEN, audio_hdl->frame_buf, FRAME_LEN2);
        max_value = Aup_MaxAbsValueW16(audio_hdl->time_signal, PART_LEN2);
        audio_hdl->time_signal_scaling = Aup_NormW16(max_value);
        TimeToFrequencyDomain_func(audio_hdl, audio_hdl->time_signal, audio_hdl->freq_signal, 
                                                        audio_hdl->time_signal_scaling);
        aup_memcpy(audio_hdl->time_signal, audio_hdl->time_signal+FRAME_LEN, sizeof(int16_t)*(PART_LEN2-FRAME_LEN));
#endif 

#if AECM_PROCESSING
        ret = aecm_process(audio_hdl->aecm, audio_hdl->freq_signal, FRAME_LEN, 
                                                ESTIMATED_DELAY, audio_hdl->time_signal_scaling);
        if(ret<0)
            goto audio_process_err;
#endif

#if HS_PROCESSING
        hs_process(audio_hdl->hs, (int16_t*)audio_hdl->freq_signal, audio_hdl->time_signal_scaling);
#endif

#if NSX_PROCESSING
        ret = nsx_process(audio_hdl->nsx, (int16_t*)audio_hdl->freq_signal, FRAME_LEN, audio_hdl->time_signal_scaling);
        if(ret<0)
            goto audio_process_err;
#endif

#if AECM_PROCESSING || NSX_PROCESSING || HS_PROCESSING
        FrequencyToTimeDomain_func(audio_hdl, audio_hdl->freq_signal, audio_hdl->frame_buf, audio_hdl->time_signal_scaling);
#endif

#if VAD_PROCESSING || AGC_PROCESSING
        ret = vad_process(audio_hdl->vad, audio_hdl->frame_buf, FRAME_LEN);
#endif

#if AGC_PROCESSING       
        agc_process(audio_hdl->agc, audio_hdl->frame_buf, ret);
#endif

        aup_memcpy(in_buf+write_samples, audio_hdl->frame_buf, FRAME_LEN2);

        res_samples -= FRAME_LEN;
        write_samples += FRAME_LEN;
    }

audio_process_err:
    return ret;
}

int push_audio_farbuf(AUDIO_PROCESS_HDL *audio_hdl, int16_t *in_buf, uint32_t samples_len)
{
    int ret = -1;
	if(!audio_hdl)
		return -1;
    if(!audio_hdl->aecm)
        return -1;
    ret = push_farbuf(audio_hdl->aecm, in_buf, samples_len, audio_hdl->sampleRate);
    if(ret < 0)
        return -1;
    return 1;
}

void audio_deinit(AUDIO_PROCESS_HDL *audio_hdl)
{
#if AECM_PROCESSING
    aecm_deinit(audio_hdl->aecm);
    audio_hdl->aecm = NULL;
#endif
#if NSX_PROCESSING
    nsx_deinit(audio_hdl->nsx);
    audio_hdl->nsx = NULL;
#endif
#if VAD_PROCESSING || AGC_PROCESSING
    vad_deinit(audio_hdl->vad);
    audio_hdl->vad = NULL;
#endif
#if AGC_PROCESSING
    agc_deinit(audio_hdl->agc);
    audio_hdl->agc = NULL;
#endif
    aup_free(audio_hdl);    
}