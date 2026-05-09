#ifndef _PLAY_PCMTONE_

#define _PLAY_PCMTONE_

typedef struct {
    uint32_t data_size;     /**< Size of the image in bytes*/
    const uint8_t * data;   /**< Pointer to the data of the image*/
} pcmtone_struct;


int32 play_pcmtone(pcmtone_struct * tone);

extern const pcmtone_struct keytone ;
extern const pcmtone_struct shottone ;
extern const pcmtone_struct starttone ;
extern const pcmtone_struct uptone ;
#endif