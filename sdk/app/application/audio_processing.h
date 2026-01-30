#ifndef _AUDIO_PROCESSING_H_
#define _AUDIO_PROCESSING_H_

#include "os_support.h"

#define AECM_PROCESSING   1
#define NSX_PROCESSING    0
#define VAD_PROCESSING    0
#define AGC_PROCESSING    0
#define HS_PROCESSING     0

#ifndef FRAME_LEN
#define FRAME_LEN         80
#endif
#ifndef FRAME_LEN2        
#define FRAME_LEN2        160
#endif
#ifndef PART_LEN
#define PART_LEN          64
#endif
#ifndef PART_LEN2         
#define PART_LEN2         128
#endif
#ifndef PART_LEN4         
#define PART_LEN4         256
#endif

#define FFT_ORDER         7

/***********AECM**********/
#define COMFORTNOISE      1
#define ECHOMODE          2
#define ESTIMATED_DELAY   1

/***********AGC***********/
#define MAXGAINDB         36
#define TARGETDB          -9 

typedef struct {
    int16_t real;
    int16_t imag;
} fftInt16;

typedef struct {
    uint32_t sampleRate;
    uint32_t channel;
    int32_t energy;
    uint8_t energy_scaling;
    uint8_t time_signal_scaling;
    int16_t *frame_buf;
    int16_t *time_signal;
    int32_t *freq_buf;
    int16_t *time_outbuf;
    fftInt16 *freq_signal;
    void *real_fft;
    
    void *aecm;
    void *nsx;
    void *vad;
    void *agc;
    void *hs;
} AUDIO_PROCESS_HDL;

AUDIO_PROCESS_HDL *audio_process_init(uint32_t sampleRate, uint32_t channel);
int audio_process(AUDIO_PROCESS_HDL *audio_hdl, int16_t *in_buf, uint32_t samples_len);
int push_audio_farbuf(AUDIO_PROCESS_HDL *audio_hdl, int16_t *in_buf, uint32_t samples_len);
void audio_deinit(AUDIO_PROCESS_HDL *audio_hdl);


int TimeToFrequencyDomain_func(AUDIO_PROCESS_HDL *audio_hdl, 
                          int16_t *time_signal, 
                          fftInt16 *freq_signal, 
                          uint8_t time_signal_scaling);
int FrequencyToTimeDomain_func(AUDIO_PROCESS_HDL *audio_hdl,
                          fftInt16 *freq_signal,
                          int16_t *output,
                          uint8_t time_signal_scaling);
struct RealFFT* Aup_CreateRealFFT(int order);
void Aup_FreeRealFFT(struct RealFFT* self);
int16_t Aup_MaxAbsValueW16(int16_t* vector, size_t length);
int16_t Aup_NormW16(int16_t a);

void *aecm_init(uint32_t sampleRate, uint8_t mode, uint8_t cng);
int aecm_process(void *aecm, void *near_freqBuf, 
                 uint32_t inSampleCount, 
                 int16_t msInSndCardBuf, 
                 uint8_t near_signal_scaling);
int push_farbuf(void *aecm, short *data, unsigned int len, uint32_t sampleRate);
void aecm_deinit(void *aecm);

void *agc_init(uint32_t max_change_db, int32_t target_db);
void agc_process(void *agc_init, int16_t *buffer, int32_t vad_ret);
void agc_deinit(void *agc_init);

void *hs_init(uint32_t sampleRate);
void hs_process(void *hs_init, int16_t *freqBuf, uint8_t signal_scaling);
void hs_deinit(void *hs_init);

void * nsx_init(uint32_t samplerate);
int nsx_process(void *nsx, int16_t *near_freqBuf, uint32_t samplecount, uint8_t near_signal_scaling);
void nsx_deinit(void *nsx);

void *vad_init(uint32_t samplerate, int16_t vad_mode);
int vad_process(void *vad, int16_t *buffer, uint32_t samplesCount);
void vad_deinit(void *vad);

#endif