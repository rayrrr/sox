/*
 * Microsoft's WAVE sound format driver
 *
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained. 
 * Lance Norskog And Sundry Contributors are not responsible for 
 * the consequences of using this software.
 *
 * Change History:
 *
 * November  23, 1999 - Stan Brooks (stabro@megsinet.com)
 *   Merged in gsm support patches from Stuart Daines...
 *   Since we had simultaneously made similar changes in
 *   wavwriteheader() and wavstartread(), this was some
 *   work.  Hopefully the result is cleaner than either
 *   version, and nothing broke.
 *
 * November  20, 1999 - Stan Brooks (stabro@megsinet.com)
 *   Mods for faster adpcm decoding and addition of IMA_ADPCM
 *   and ADPCM  writing... low-level codex functions moved to
 *   external modules ima_rw.c and adpcm.c. Some general cleanup,
 *   consistent with writing adpcm and other output formats.
 *   Headers written for adpcm include the 'fact' subchunk.
 *
 * September 11, 1998 - Chris Bagwell (cbagwell@sprynet.com)
 *   Fixed length bug for IMA and MS ADPCM files.
 *
 * June 1, 1998 - Chris Bagwell (cbagwell@sprynet.com)
 *   Fixed some compiler warnings as reported by Kjetil Torgrim Homme
 *   <kjetilho@ifi.uio.no>.
 *   Fixed bug that caused crashes when reading mono MS ADPCM files. Patch
 *   was sent from Michael Brown (mjb@pootle.demon.co.uk).
 *
 * March 15, 1998 - Chris Bagwell (cbagwell@sprynet.com)
 *   Added support for Microsoft's ADPCM and IMA (or better known as
 *   DVI) ADPCM format for wav files.  Thanks goes to Mark Podlipec's
 *   XAnim code.  It gave some real life understanding of how the ADPCM
 *   format is processed.  Actual code was implemented based off of
 *   various sources from the net.
 *
 * NOTE: Previous maintainers weren't very good at providing contact
 * information.
 *
 * Copyright 1992 Rick Richardson
 * Copyright 1991 Lance Norskog And Sundry Contributors
 *
 * Fixed by various contributors previous to 1998:
 * 1) Little-endian handling
 * 2) Skip other kinds of file data
 * 3) Handle 16-bit formats correctly
 * 4) Not go into infinite loop
 *
 * User options should override file header - we assumed user knows what
 * they are doing if they specify options.
 * Enhancements and clean up by Graeme W. Gill, 93/5/17
 *
 * Info for format tags can be found at:
 *   http://www.microsoft.com/asf/resources/draft-ietf-fleischman-codec-subtree-01.txt
 *
 */

#include <string.h>		/* Included for strncmp */
#include <stdlib.h>		/* Included for malloc and free */
#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>		/* For SEEK_* defines if not found in stdio */
#endif

#include "st.h"
#include "wav.h"
#include "ima_rw.h"
#include "adpcm.h"
#ifdef HAVE_LIBGSM
#include "gsm.h"
#endif

#undef PAD_NSAMPS
/* #define PAD_NSAMPS */

/* Private data for .wav file */
typedef struct wavstuff {
    LONG	   numSamples;     /* reading: starts at total count and decremented  */
    		                   /* writing: starts at 0 and counts samples written */
    LONG	   dataLength;     /* needed for ADPCM writing */
    unsigned short formatTag;	   /* What type of encoding file is using */
    unsigned short samplesPerBlock;
    unsigned short blockAlign;
    
    /* following used by *ADPCM wav files */
    unsigned short nCoefs;	    /* ADPCM: number of coef sets */
    short	  *iCoefs;	    /* ADPCM: coef sets           */
    unsigned char *packet;	    /* Temporary buffer for packets */
    short	  *samples;	    /* interleaved samples buffer */
    short	  *samplePtr;       /* Pointer to current sample  */
    short	  *sampleTop;       /* End of samples-buffer      */
    unsigned short blockSamplesRemaining;/* Samples remaining per channel */    
    int 	   state[16];       /* step-size info for *ADPCM writes */

    /* following used by GSM 6.10 wav */
#ifdef HAVE_LIBGSM
    gsm		   gsmhandle;
    gsm_signal	   *gsmsample;
    int		   gsmindex;
    int		   gsmbytecount;    /* counts bytes written to data block */
#endif
} *wav_t;

/*
#if sizeof(struct wavstuff) > PRIVSIZE
#	warn "Uh-Oh"
#endif
*/

static char *wav_format_str();

static int wavwritehdr(P2(ft_t, int));


/****************************************************************************/
/* IMA ADPCM Support Functions Section                                      */
/****************************************************************************/

/*
 *
 * ImaAdpcmReadBlock - Grab and decode complete block of samples
 *
 */
unsigned short  ImaAdpcmReadBlock(ft)
ft_t ft;    
{
    wav_t	wav = (wav_t) ft->priv;
    int bytesRead;
    int samplesThisBlock;

    /* Pull in the packet and check the header */
    bytesRead = fread(wav->packet,1,wav->blockAlign,ft->fp);
    samplesThisBlock = wav->samplesPerBlock;
    if (bytesRead < wav->blockAlign) 
    { 
	/* If it looks like a valid header is around then try and */
	/* work with partial blocks.  Specs say it should be null */
	/* padded but I guess this is better than trailing quiet. */
	samplesThisBlock = ImaSamplesIn(0, ft->info.channels, bytesRead, 0);
	if (samplesThisBlock == 0) 
	{
	    st_warn("Premature EOF on .wav input file");
	    return 0;
	}
    }
    
    wav->samplePtr = wav->samples;
    
    /* For a full block, the following should be true: */
    /* wav->samplesPerBlock = blockAlign - 8byte header + 1 sample in header */
    ImaBlockExpandI(ft->info.channels, wav->packet, wav->samples, samplesThisBlock);
    return samplesThisBlock;

}

/****************************************************************************/
/* MS ADPCM Support Functions Section                                       */
/****************************************************************************/

/*
 *
 * AdpcmReadBlock - Grab and decode complete block of samples
 *
 */
unsigned short  AdpcmReadBlock(ft)
ft_t ft;    
{
    wav_t	wav = (wav_t) ft->priv;
    int bytesRead;
    int samplesThisBlock;
    const char *errmsg;

    /* Pull in the packet and check the header */
    bytesRead = fread(wav->packet,1,wav->blockAlign,ft->fp);
    samplesThisBlock = wav->samplesPerBlock;
    if (bytesRead < wav->blockAlign) 
    {
	/* If it looks like a valid header is around then try and */
	/* work with partial blocks.  Specs say it should be null */
	/* padded but I guess this is better than trailing quiet. */
	samplesThisBlock = AdpcmSamplesIn(0, ft->info.channels, bytesRead, 0);
	if (samplesThisBlock == 0) 
	{
	    st_warn("Premature EOF on .wav input file");
	    return 0;
	}
    }
    
    errmsg = AdpcmBlockExpandI(ft->info.channels, wav->nCoefs, wav->iCoefs, wav->packet, wav->samples, samplesThisBlock);

    if (errmsg)
	st_warn((char*)errmsg);

    return samplesThisBlock;
}

/****************************************************************************/
/* Common ADPCM Write Function                                              */
/****************************************************************************/

static int xxxAdpcmWriteBlock(ft)
ft_t ft;
{
    wav_t wav = (wav_t) ft->priv;
    int chans, ct;
    short *p;

    chans = ft->info.channels;
    p = wav->samplePtr;
    ct = p - wav->samples;
    if (ct>=chans) { 
	/* zero-fill samples if needed to complete block */
	for (p = wav->samplePtr; p < wav->sampleTop; p++) *p=0;
	/* compress the samples to wav->packet */
	if (wav->formatTag == WAVE_FORMAT_ADPCM) {
	    AdpcmBlockMashI(chans, wav->samples, wav->samplesPerBlock, wav->state, wav->packet, wav->blockAlign,9);
	}else{ /* WAVE_FORMAT_IMA_ADPCM */
	    ImaBlockMashI(chans, wav->samples, wav->samplesPerBlock, wav->state, wav->packet, 9);
	}
	/* write the compressed packet */
	if (fwrite(wav->packet, wav->blockAlign, 1, ft->fp) != 1)
	{
	    st_fail("write error");
	    return (ST_EOF);
	}
	/* update lengths and samplePtr */
	wav->dataLength += wav->blockAlign;
#ifndef PAD_NSAMPS
	wav->numSamples += ct/chans;
#else
	wav->numSamples += wav->samplesPerBlock;
#endif
	wav->samplePtr = wav->samples;
    }
    return (ST_SUCCESS);
}

/****************************************************************************/
/* WAV GSM6.10 support functions                                            */
/****************************************************************************/
#ifdef HAVE_LIBGSM
/* create the gsm object, malloc buffer for 160*2 samples */
int wavgsminit(ft)
ft_t ft;
{	
    int valueP=1;
    wav_t	wav = (wav_t) ft->priv;
    wav->gsmbytecount=0;
    wav->gsmhandle=gsm_create();
    if (!wav->gsmhandle)
    {
	st_fail("cannot create GSM object");
	return (ST_EOF);
    }
	
    if(gsm_option(wav->gsmhandle,GSM_OPT_WAV49,&valueP) == -1){
	st_fail("error setting gsm_option for WAV49 format. Recompile gsm library with -DWAV49 option and relink sox");
	return (ST_EOF);
    }

    wav->gsmsample=malloc(sizeof(gsm_signal)*160*2);
    if (wav->gsmsample == NULL){
	st_fail("error allocating memory for gsm buffer");
	return (ST_EOF);
    }
    wav->gsmindex=0;
    return (ST_SUCCESS);
}

/*destroy the gsm object and free the buffer */
void wavgsmdestroy(ft)
ft_t ft;
{	
    wav_t	wav = (wav_t) ft->priv;
    gsm_destroy(wav->gsmhandle);
    free(wav->gsmsample);
}

LONG wavgsmread(ft, buf, len)
ft_t ft;
LONG *buf, len;
{
    wav_t	wav = (wav_t) ft->priv;
    int done=0;
    int bytes;
    gsm_byte	frame[65];

  /* copy out any samples left from the last call */
    while(wav->gsmindex && (wav->gsmindex<160*2) && (done < len))
	buf[done++]=LEFT(wav->gsmsample[wav->gsmindex++],16);

  /* read and decode loop, possibly leaving some samples in wav->gsmsample */
    while (done < len) {
	wav->gsmindex=0;
	bytes = fread(frame,1,65,ft->fp);   
	if (bytes <=0)
	    return done;
	if (bytes<65) {
	    st_warn("invalid wav gsm frame size: %d bytes",bytes);
	    return done;
	}
	/* decode the long 33 byte half */
	if(gsm_decode(wav->gsmhandle,frame, wav->gsmsample)<0)
	{
	    st_fail("error during gsm decode");
	    return 0;
	}
	/* decode the short 32 byte half */
	if(gsm_decode(wav->gsmhandle,frame+33, wav->gsmsample+160)<0)
	{
	    st_fail("error during gsm decode");
	    return 0;
	}

	while ((wav->gsmindex <160*2) && (done < len)){
	    buf[done++]=LEFT(wav->gsmsample[(wav->gsmindex)++],16);
	}
    }

    return done;
}

static int wavgsmflush(ft, pad)
ft_t ft;
int pad; /* normally 0, but 1 to pad last write to even datalen */
{
    gsm_byte	frame[65];
    wav_t	wav = (wav_t) ft->priv;

    /* zero fill as needed */
    while(wav->gsmindex<160*2)
	wav->gsmsample[wav->gsmindex++]=0;

    /*encode the even half short (32 byte) frame */
    gsm_encode(wav->gsmhandle, wav->gsmsample, frame);
    /*encode the odd half long (33 byte) frame */
    gsm_encode(wav->gsmhandle, wav->gsmsample+160, frame+32);
    if (fwrite(frame, 1, 65, ft->fp) != 65)
    {
	st_fail("write error");
	return (ST_EOF);
    }
    wav->gsmbytecount += 65;

    wav->gsmindex = 0;

    if (pad & wav->gsmbytecount){
	/* pad output to an even number of bytes */
	if(st_writeb(ft, 0))
	{
	    st_fail("write error");
	    return (ST_EOF);
	}
	wav->gsmbytecount += 1;
    }
    return (ST_SUCCESS);
}

LONG wavgsmwrite(ft, buf, len)
ft_t ft;
LONG *buf, len;
{
    wav_t	wav = (wav_t) ft->priv;
    int done = 0;
    int rc;

    while (done < len) {
	while ((wav->gsmindex < 160*2) && (done < len))
	    wav->gsmsample[(wav->gsmindex)++] = RIGHT(buf[done++], 16);

	if (wav->gsmindex < 160*2)
	    break;

	rc = wavgsmflush(ft, 0);
	if (rc)
	    return 0;
    }     
    return done;

}

void wavgsmstopwrite(ft)
ft_t ft;
{
    wav_t	wav = (wav_t) ft->priv;

    if (wav->gsmindex)
	wavgsmflush(ft, 1);

    wavgsmdestroy(ft);
}
#endif        /*ifdef out gsm code */
/****************************************************************************/
/* General Sox WAV file code                                                */
/****************************************************************************/

/* FIXME: Use common misc.c skip code */
static void fSkip(FILE *fp, ULONG len)
{
    while (len > 0 && !feof(fp))
    {
	getc(fp);
	len--;
    }
}

static ULONG findChunk(ft_t ft, const char *Label)
{
    char magic[5];
    ULONG len;
    for (;;)
    {
	if (st_reads(ft, magic, 4) == ST_EOF)
	{
	    st_fail("WAVE file has missing %s chunk", Label);
	    return 0;
	}
	st_readdw(ft, &len);
	if (strncmp(Label, magic, 4) == 0)
	    break;		/* Found the data chunk */
	
	fSkip(ft->fp, len); 	/* skip to next chunk */
    }
    return len;
}

/*
 * Do anything required before you start reading samples.
 * Read file header. 
 *	Find out sampling rate, 
 *	size and encoding of samples, 
 *	mono/stereo/quad.
 */
int st_wavstartread(ft) 
ft_t ft;
{
    wav_t	wav = (wav_t) ft->priv;
    char	magic[5];
    ULONG	len;
    int		rc;

    /* wave file characteristics */
    ULONG    wRiffLength;
    unsigned short wChannels;	    /* number of channels */
    ULONG    wSamplesPerSecond;     /* samples per second per channel */
    ULONG    wAvgBytesPerSec;	    /* estimate of bytes per second needed */
    unsigned short wBitsPerSample;  /* bits per sample */
    unsigned short wFmtSize;
    unsigned short wExtSize = 0;    /* extended field for non-PCM */

    ULONG    wDataLength;	    /* length of sound data in bytes */
    ULONG    bytesPerBlock = 0;
    ULONG    bytespersample;	    /* bytes per sample (per channel */

    if (ST_IS_BIGENDIAN) ft->swap = ft->swap ? 0 : 1;

    if (st_reads(ft, magic, 4) == ST_EOF || strncmp("RIFF", magic, 4))
    {
	st_fail("WAVE: RIFF header not found");
	return ST_EOF;
    }

    st_readdw(ft, &wRiffLength);

    if (st_reads(ft, magic, 4) == ST_EOF || strncmp("WAVE", magic, 4))
    {
	st_fail("WAVE header not found");
	return ST_EOF;
    }

    /* Now look for the format chunk */
    wFmtSize = len = findChunk(ft, "fmt ");
    /* findChunk() only returns if chunk was found */
    
    if (wFmtSize < 16)
    {
	st_fail("WAVE file fmt chunk is too short");
	return ST_EOF;
    }

    st_readw(ft, &(wav->formatTag));
    st_readw(ft, &wChannels);
    st_readdw(ft, &wSamplesPerSecond);
    st_readdw(ft, &wAvgBytesPerSec);	/* Average bytes/second */
    st_readw(ft, &(wav->blockAlign));	/* Block align */
    st_readw(ft, &wBitsPerSample);	/* bits per sample per channel */
    len -= 16;

    switch (wav->formatTag)
    {
    case WAVE_FORMAT_UNKNOWN:
	st_fail("WAVE file is in unsupported Microsoft Official Unknown format.");
	return ST_EOF;
	
    case WAVE_FORMAT_PCM:
	/* Default (-1) depends on sample size.  Set that later on. */
	if (ft->info.encoding != -1 && ft->info.encoding != ST_ENCODING_UNSIGNED &&
	    ft->info.encoding != ST_ENCODING_SIGN2)
	    st_warn("User options overriding encoding read in .wav header");

	/* Needed by rawread() functions */
        rc = st_rawstartread(ft);
        if (rc)
	    return rc;

	break;
	
    case WAVE_FORMAT_IMA_ADPCM:
	if (ft->info.encoding == -1 || ft->info.encoding == ST_ENCODING_IMA_ADPCM)
	    ft->info.encoding = ST_ENCODING_IMA_ADPCM;
	else
	    st_warn("User options overriding encoding read in .wav header");
	break;

    case WAVE_FORMAT_ADPCM:
	if (ft->info.encoding == -1 || ft->info.encoding == ST_ENCODING_ADPCM)
	    ft->info.encoding = ST_ENCODING_ADPCM;
	else
	    st_warn("User options overriding encoding read in .wav header");
	break;

    case WAVE_FORMAT_IEEE_FLOAT:
	st_fail("Sorry, this WAV file is in IEEE Float format.");
	return ST_EOF;
	
    case WAVE_FORMAT_ALAW:
	if (ft->info.encoding == -1 || ft->info.encoding == ST_ENCODING_ALAW)
	    ft->info.encoding = ST_ENCODING_ALAW;
	else
	    st_warn("User options overriding encoding read in .wav header");

	/* Needed by rawread() functions */
        rc = st_rawstartread(ft);
        if (rc)
	    return rc;

	break;
	
    case WAVE_FORMAT_MULAW:
	if (ft->info.encoding == -1 || ft->info.encoding == ST_ENCODING_ULAW)
	    ft->info.encoding = ST_ENCODING_ULAW;
	else
	    st_warn("User options overriding encoding read in .wav header");

	/* Needed by rawread() functions */
        rc = st_rawstartread(ft);
        if (rc)
	    return rc;

	break;
	
    case WAVE_FORMAT_OKI_ADPCM:
	st_fail("Sorry, this WAV file is in OKI ADPCM format.");
	return ST_EOF;
    case WAVE_FORMAT_DIGISTD:
	st_fail("Sorry, this WAV file is in Digistd format.");
	return ST_EOF;
    case WAVE_FORMAT_DIGIFIX:
	st_fail("Sorry, this WAV file is in Digifix format.");
	return ST_EOF;
    case WAVE_FORMAT_DOLBY_AC2:
	st_fail("Sorry, this WAV file is in Dolby AC2 format.");
	return ST_EOF;
    case WAVE_FORMAT_GSM610:
#ifdef HAVE_LIBGSM
	if (ft->info.encoding == -1 || ft->info.encoding == ST_ENCODING_GSM )
	    ft->info.encoding = ST_ENCODING_GSM;
	else
	    st_warn("User options overriding encoding read in .wav header");
	break;
#else
	st_fail("Sorry, this WAV file is in GSM6.10 format and no GSM support present, recompile sox with gsm library");
	return ST_EOF;
#endif
    case WAVE_FORMAT_ROCKWELL_ADPCM:
	st_fail("Sorry, this WAV file is in Rockwell ADPCM format.");
	return ST_EOF;
    case WAVE_FORMAT_ROCKWELL_DIGITALK:
	st_fail("Sorry, this WAV file is in Rockwell DIGITALK format.");
	return ST_EOF;
    case WAVE_FORMAT_G721_ADPCM:
	st_fail("Sorry, this WAV file is in G.721 ADPCM format.");
	return ST_EOF;
    case WAVE_FORMAT_G728_CELP:
	st_fail("Sorry, this WAV file is in G.728 CELP format.");
	return ST_EOF;
    case WAVE_FORMAT_MPEG:
	st_fail("Sorry, this WAV file is in MPEG format.");
	return ST_EOF;
    case WAVE_FORMAT_MPEGLAYER3:
	st_fail("Sorry, this WAV file is in MPEG Layer 3 format.");
	return ST_EOF;
    case WAVE_FORMAT_G726_ADPCM:
	st_fail("Sorry, this WAV file is in G.726 ADPCM format.");
	return ST_EOF;
    case WAVE_FORMAT_G722_ADPCM:
	st_fail("Sorry, this WAV file is in G.722 ADPCM format.");
	return ST_EOF;
    default:	st_fail("WAV file has unknown format type of %x",wav->formatTag);
		return ST_EOF;
    }

    /* User options take precedence */
    if (ft->info.channels == -1 || ft->info.channels == wChannels)
	ft->info.channels = wChannels;
    else
	st_warn("User options overriding channels read in .wav header");

    if (ft->info.rate == 0 || ft->info.rate == wSamplesPerSecond)
	ft->info.rate = wSamplesPerSecond;
    else
	st_warn("User options overriding rate read in .wav header");
    

    wav->iCoefs = NULL;
    wav->packet = NULL;
    wav->samples = NULL;

    /* non-PCM formats have extended fmt chunk.  Check for those cases. */
    if (wav->formatTag != WAVE_FORMAT_PCM) {
	if (len >= 2) {
	    st_readw(ft, &wExtSize);
	    len -= 2;
	} else {
	    st_warn("wave header missing FmtExt chunk");
	}
    }

    if (wExtSize > len)
    {
	st_fail("wave header error: wExtSize inconsistent with wFmtLen");
	return ST_EOF;
    }

    switch (wav->formatTag)
    {
    /* ULONG max_spb; */
    case WAVE_FORMAT_ADPCM:
	if (wExtSize < 4)
	{
	    st_fail("format[%s]: expects wExtSize >= %d",
			wav_format_str(wav->formatTag), 4);
	    return ST_EOF;
	}

	if (wBitsPerSample != 4)
	{
	    st_fail("Can only handle 4-bit MS ADPCM in wav files");
	    return ST_EOF;
	}

	st_readw(ft, &(wav->samplesPerBlock));
	bytesPerBlock = AdpcmBytesPerBlock(ft->info.channels, wav->samplesPerBlock);
	if (bytesPerBlock > wav->blockAlign)
	{
	    st_fail("format[%s]: samplesPerBlock(%d) incompatible with blockAlign(%d)",
		wav_format_str(wav->formatTag), wav->samplesPerBlock, wav->blockAlign);
	    return ST_EOF;
	}

	st_readw(ft, &(wav->nCoefs));
	if (wav->nCoefs < 7 || wav->nCoefs > 0x100) {
	    st_fail("ADPCM file nCoefs (%.4hx) makes no sense\n", wav->nCoefs);
	    return ST_EOF;
	}
	wav->packet = (unsigned char *)malloc(wav->blockAlign);
	if (!wav->packet)
	{
	    st_fail("Unable to alloc resources");
	    return ST_EOF;
	}

	len -= 4;

	if (wExtSize < 4 + 4*wav->nCoefs)
	{
	    st_fail("wave header error: wExtSize(%d) too small for nCoefs(%d)", wExtSize, wav->nCoefs);
	    return ST_EOF;
	}

	wav->samples = (short *)malloc(wChannels*wav->samplesPerBlock*sizeof(short));
	if (!wav->samples)
	{
	    st_fail("Unable to alloc resources");
	    return ST_EOF;
	}

	/* nCoefs, iCoefs used by adpcm.c */
	wav->iCoefs = (short *)malloc(wav->nCoefs * 2 * sizeof(short));
	if (!wav->iCoefs)
	{
	    st_fail("Unable to alloc resources");
	    return ST_EOF;
	}
	{
	    int i, errct=0;
	    for (i=0; len>=2 && i < 2*wav->nCoefs; i++) {
		st_readw(ft, &(wav->iCoefs[i]));
		len -= 2;
		if (i<14) errct += (wav->iCoefs[i] != iCoef[i/2][i%2]);
		/* fprintf(stderr,"iCoefs[%2d] %4d\n",i,wav->iCoefs[i]); */
	    }
	    if (errct) st_warn("base iCoefs differ in %d/14 positions",errct);
	}

	bytespersample = ST_SIZE_WORD;  /* AFTER de-compression */
	break;

    case WAVE_FORMAT_IMA_ADPCM:
	if (wExtSize < 2)
	{
	    st_fail("format[%s]: expects wExtSize >= %d",
		    wav_format_str(wav->formatTag), 2);
	    return ST_EOF;
	}

	if (wBitsPerSample != 4)
	{
	    st_fail("Can only handle 4-bit IMA ADPCM in wav files");
	    return ST_EOF;
	}

	st_readw(ft, &(wav->samplesPerBlock));
	bytesPerBlock = ImaBytesPerBlock(ft->info.channels, wav->samplesPerBlock);
	if (bytesPerBlock > wav->blockAlign || wav->samplesPerBlock%8 != 1)
	{
	    st_fail("format[%s]: samplesPerBlock(%d) incompatible with blockAlign(%d)",
		wav_format_str(wav->formatTag), wav->samplesPerBlock, wav->blockAlign);
	    return ST_EOF;
	}

	wav->packet = (unsigned char *)malloc(wav->blockAlign);
	if (!wav->packet)
	{
	    st_fail("Unable to alloc resources");
	    return ST_EOF;
	}
	len -= 2;

	wav->samples = (short *)malloc(wChannels*wav->samplesPerBlock*sizeof(short));
	if (!wav->samples)
	{
	    st_fail("Unable to alloc resources");
	    return ST_EOF;
	}

	bytespersample = ST_SIZE_WORD;  /* AFTER de-compression */
	break;

#ifdef HAVE_LIBGSM
    /* GSM formats have extended fmt chunk.  Check for those cases. */
    case WAVE_FORMAT_GSM610:
	if (wExtSize < 2)
	{
	    st_fail("format[%s]: expects wExtSize >= %d",
		    wav_format_str(wav->formatTag), 2);
	    return ST_EOF;
	}
	st_readw(ft, &wav->samplesPerBlock);
	bytesPerBlock = 65;
	if (wav->blockAlign != 65)
	{
	    st_fail("format[%s]: expects blockAlign(%d) = %d",
		    wav_format_str(wav->formatTag), wav->blockAlign, 65);
	    return ST_EOF;
	}
	if (wav->samplesPerBlock != 320)
	{
	    st_fail("format[%s]: expects samplesPerBlock(%d) = %d",
		    wav_format_str(wav->formatTag), wav->samplesPerBlock, 320);
	    return ST_EOF;
	}
	bytespersample = ST_SIZE_WORD;  /* AFTER de-compression */
	len -= 2;
	break;
#endif

    default:
      bytespersample = (wBitsPerSample + 7)/8;

    }

    switch (bytespersample)
    {
	
    case ST_SIZE_BYTE:
	/* User options take precedence */
	if (ft->info.size == -1 || ft->info.size == ST_SIZE_BYTE)
	    ft->info.size = ST_SIZE_BYTE;
	else
	    st_warn("User options overriding size read in .wav header");

	/* Now we have enough information to set default encodings. */
	if (ft->info.encoding == -1)
	    ft->info.encoding = ST_ENCODING_UNSIGNED;
	break;
	
    case ST_SIZE_WORD:
	if (ft->info.size == -1 || ft->info.size == ST_SIZE_WORD)
	    ft->info.size = ST_SIZE_WORD;
	else
	    st_warn("User options overriding size read in .wav header");

	/* Now we have enough information to set default encodings. */
	if (ft->info.encoding == -1)
	    ft->info.encoding = ST_ENCODING_SIGN2;
	break;
	
    case ST_SIZE_DWORD:
	if (ft->info.size == -1 || ft->info.size == ST_SIZE_DWORD)
	    ft->info.size = ST_SIZE_DWORD;
	else
	    st_warn("User options overriding size read in .wav header");

	/* Now we have enough information to set default encodings. */
	if (ft->info.encoding == -1)
	    ft->info.encoding = ST_ENCODING_SIGN2;
	break;
	
    default:
	st_fail("Sorry, don't understand .wav size");
	return ST_EOF;
    }

    /* Skip anything left over from fmt chunk */
    fSkip(ft->fp, len);

    /* for non-PCM formats, there's a 'fact' chunk before
     * the upcoming 'data' chunk */

    /* Now look for the wave data chunk */
    wDataLength = len = findChunk(ft, "data");
    /* findChunk() only returns if chunk was found */

    switch (wav->formatTag)
    {

    case WAVE_FORMAT_ADPCM:
	wav->numSamples = 
	    AdpcmSamplesIn(wDataLength, ft->info.channels, wav->blockAlign, wav->samplesPerBlock);
	/*st_report("datalen %d, numSamples %d",wDataLength, wav->numSamples);*/
	wav->blockSamplesRemaining = 0;	       /* Samples left in buffer */
	break;

    case WAVE_FORMAT_IMA_ADPCM:
	/* Compute easiest part of number of samples.  For every block, there
	   are samplesPerBlock samples to read. */
	wav->numSamples = 
	    ImaSamplesIn(wDataLength, ft->info.channels, wav->blockAlign, wav->samplesPerBlock);
	/*st_report("datalen %d, numSamples %d",wDataLength, wav->numSamples);*/
	wav->blockSamplesRemaining = 0;	       /* Samples left in buffer */
	initImaTable();
	break;

#ifdef HAVE_LIBGSM
    case WAVE_FORMAT_GSM610:
	wav->numSamples = (((wDataLength / wav->blockAlign) * wav->samplesPerBlock) * ft->info.channels);
	wavgsminit(ft);
	break;
#endif

    default:
	wav->numSamples = wDataLength/ft->info.size;	/* total samples */

    }

    st_report("Reading Wave file: %s format, %d channel%s, %d samp/sec",
	   wav_format_str(wav->formatTag), ft->info.channels,
	   wChannels == 1 ? "" : "s", wSamplesPerSecond);
    st_report("        %d byte/sec, %d block align, %d bits/samp, %u data bytes",
	   wAvgBytesPerSec, wav->blockAlign, wBitsPerSample, wDataLength);

    /* Can also report extended fmt information */
    switch (wav->formatTag)
    {
    case WAVE_FORMAT_ADPCM:
	st_report("        %d Extsize, %d Samps/block, %d bytes/block %d Num Coefs",
		wExtSize,wav->samplesPerBlock,bytesPerBlock,wav->nCoefs);
	break;

    case WAVE_FORMAT_IMA_ADPCM:
	st_report("        %d Extsize, %d Samps/block, %d bytes/block",
		wExtSize,wav->samplesPerBlock,bytesPerBlock);
	break;

#ifdef HAVE_LIBGSM
    case WAVE_FORMAT_GSM610:
	st_report("GSM .wav: %d Extsize, %d Samps/block,  %d samples",
		wExtSize,wav->samplesPerBlock,wav->numSamples);
	break;
#endif

    default:
	break;
    }
    return ST_SUCCESS;
}


/*
 * Read up to len samples from file.
 * Convert to signed longs.
 * Place in buf[].
 * Return number of samples read.
 */

LONG st_wavread(ft, buf, len) 
ft_t ft;
LONG *buf, len;
{
	wav_t	wav = (wav_t) ft->priv;
	LONG	done;
	
	/* If file is in ADPCM encoding then read in multiple blocks else */
	/* read as much as possible and return quickly. */
	switch (ft->info.encoding)
	{
	case ST_ENCODING_IMA_ADPCM:
	case ST_ENCODING_ADPCM:

	    /* FIXME: numSamples is not used consistently in
	     * wav handler.  Sometimes it accounts for stereo,
	     * sometimes it does not.
	     */
	    if (len > (wav->numSamples*ft->info.channels)) 
	        len = (wav->numSamples*ft->info.channels);

	    done = 0;
	    while (done < len) { /* Still want data? */
		/* See if need to read more from disk */
		if (wav->blockSamplesRemaining == 0) { 
		    if (wav->formatTag == WAVE_FORMAT_IMA_ADPCM)
			wav->blockSamplesRemaining = ImaAdpcmReadBlock(ft);
		    else
			wav->blockSamplesRemaining = AdpcmReadBlock(ft);
		    if (wav->blockSamplesRemaining == 0)
		    {
			/* Don't try to read any more samples */
			wav->numSamples = 0;
			return done;
		    }
		    wav->samplePtr = wav->samples;
		}

		/* Copy interleaved data into buf, converting short to LONG */
		{
		    short *p, *top;
		    int ct;
		    ct = len-done;
		    if (ct > (wav->blockSamplesRemaining*ft->info.channels))
			ct = (wav->blockSamplesRemaining*ft->info.channels);

		    done += ct;
		    wav->blockSamplesRemaining -= (ct/ft->info.channels);
		    p = wav->samplePtr;
		    top = p+ct;
		    /* Output is already signed */
		    while (p<top)
			*buf++ = LEFT((*p++), 16);

		    wav->samplePtr = p;
		}
	    }
	    /* "done" for ADPCM equals total data processed and not
	     * total samples procesed.  The only way to take care of that
	     * is to return here and not fall thru.
	     */
	    wav->numSamples -= (done / ft->info.channels);
	    return done;
	    break;

#ifdef HAVE_LIBGSM
	case ST_ENCODING_GSM:
	    if (len > wav->numSamples) 
	        len = wav->numSamples;

	    done = wavgsmread(ft, buf, len);
	    if (done == 0 && wav->numSamples != 0)
		st_warn("Premature EOF on .wav input file");
	break;
#endif
	default: /* assume PCM encoding */
	    if (len > wav->numSamples) 
	        len = wav->numSamples;

	    done = st_rawread(ft, buf, len);
	    /* If software thinks there are more samples but I/O */
	    /* says otherwise, let the user know about this.     */
	    if (done == 0 && wav->numSamples != 0)
		st_warn("Premature EOF on .wav input file");
	}

	wav->numSamples -= done;
	return done;
}

/*
 * Do anything required when you stop reading samples.  
 * Don't close input file! 
 */
int st_wavstopread(ft) 
ft_t ft;
{
    wav_t	wav = (wav_t) ft->priv;
    int		rc = ST_SUCCESS;

    if (wav->packet) free(wav->packet);
    if (wav->samples) free(wav->samples);
    if (wav->iCoefs) free(wav->iCoefs);

    switch (ft->info.encoding)
    {
#ifdef HAVE_LIBGSM
    case ST_ENCODING_GSM:
	wavgsmdestroy(ft);
	break;
#endif
    case ST_ENCODING_IMA_ADPCM:
    case ST_ENCODING_ADPCM:
	break;
    default:
	/* Needed for rawread() */
	rc = st_rawstopread(ft);
    }
    return rc;
}

int st_wavstartwrite(ft) 
ft_t ft;
{
	wav_t	wav = (wav_t) ft->priv;
	int	rc;

	if (ST_IS_BIGENDIAN) ft->swap = ft->swap ? 0 : 1;

	wav->numSamples = 0;
	wav->dataLength = 0;
	if (!ft->seekable)
		st_warn("Length in output .wav header will be wrong since can't seek to fix it");
	rc = wavwritehdr(ft, 0);  /* also calculates various wav->* info */
	if (rc != 0)
	    return rc;
	wav->packet = NULL;
	wav->samples = NULL;
	wav->iCoefs = NULL;
	switch (wav->formatTag)
	{
	int ch, sbsize;
	case WAVE_FORMAT_IMA_ADPCM:
	    initImaTable();
	/* intentional case fallthru! */
	case WAVE_FORMAT_ADPCM:
	    /* #channels already range-checked for overflow in wavwritehdr() */
	    for (ch=0; ch<ft->info.channels; ch++)
	    	wav->state[ch] = 0;
	    sbsize = ft->info.channels * wav->samplesPerBlock;
	    wav->packet = (unsigned char *)malloc(wav->blockAlign);
	    wav->samples = (short *)malloc(sbsize*sizeof(short));
	    if (!wav->packet || !wav->samples)
	    {
		st_fail("Unable to alloc resources");
		return ST_EOF;
	    }
	    wav->sampleTop = wav->samples + sbsize;
	    wav->samplePtr = wav->samples;
	    break;

#ifdef HAVE_LIBGSM
	case WAVE_FORMAT_GSM610:
	    wavgsminit(ft);
	    break;
#endif
	default:
	    break;
	}
	return ST_SUCCESS;
}

/* wavwritehdr:  write .wav headers as follows:
 
bytes      variable      description
0  - 3     'RIFF'
4  - 7     wRiffLength   length of file minus the 8 byte riff header
8  - 11    'WAVE'
12 - 15    'fmt '
16 - 19    wFmtSize       length of format chunk minus 8 byte header 
20 - 21    wFormatTag     identifies PCM, ULAW etc
22 - 23    wChannels      
24 - 27    wSamplesPerSecond   samples per second per channel
28 - 31    wAvgBytesPerSec     non-trivial for compressed formats
32 - 33    wBlockAlign         basic block size
34 - 35    wBitsPerSample      non-trivial for compressed formats

PCM formats then go straight to the data chunk:
36 - 39    'data'
40 - 43     wDataLength   length of data chunk minus 8 byte header
44 - (wDataLength + 43)    the data

non-PCM formats must write an extended format chunk and a fact chunk:

ULAW, ALAW formats:
36 - 37    wExtSize = 0  the length of the format extension
38 - 41    'fact'
42 - 45    wFactSize = 4  length of the fact chunk minus 8 byte header
46 - 49    wSamplesWritten   actual number of samples written out
50 - 53    'data'
54 - 57     wDataLength   length of data chunk minus 8 byte header
58 - (wDataLength + 57)    the data


GSM6.10  format:
36 - 37    wExtSize = 2 the length in bytes of the format-dependent extension
38 - 39    320           number of samples per  block 
40 - 43    'fact'
44 - 47    wFactSize = 4  length of the fact chunk minus 8 byte header
48 - 51    wSamplesWritten   actual number of samples written out
52 - 55    'data'
56 - 59     wDataLength   length of data chunk minus 8 byte header
60 - (wDataLength + 59)     the data
(+ a padding byte if wDataLength is odd) 


note that header contains (up to) 3 separate ways of describing the
length of the file, all derived here from the number of (input)
samples wav->numSamples in a way that is non-trivial for the blocked 
and padded compressed formats:

wRiffLength -      (riff header) the length of the file, minus 8 
wSamplesWritten  -  (fact header) the number of samples written (after padding
                   to a complete block eg for GSM)
wDataLength     -   (data chunk header) the number of (valid) data bytes written

*/

static int wavwritehdr(ft, second_header) 
ft_t ft;
int second_header;
{
	wav_t	wav = (wav_t) ft->priv;

	/* variables written to wav file header */
	/* RIFF header */    
	ULONG wRiffLength ;                 /* length of file after 8 byte riff header */
	/* fmt chunk */
	ULONG wFmtSize = 16;                /* size field of the fmt chunk */
	unsigned short wFormatTag = 0;      /* data format */
	unsigned short wChannels;           /* number of channels */
	ULONG  wSamplesPerSecond;           /* samples per second per channel*/
	ULONG  wAvgBytesPerSec=0;           /* estimate of bytes per second needed */
	unsigned short wBlockAlign=0;       /* byte alignment of a basic sample block */
	unsigned short wBitsPerSample=0;    /* bits per sample */
	/* fmt chunk extension (not PCM) */
	unsigned short wExtSize=0;          /* extra bytes in the format extension */
	unsigned short wSamplesPerBlock;    /* samples per channel per block */
	/* wSamplesPerBlock and other things may go into format extension */

	/* fact chunk (not PCM) */
	ULONG wFactSize=4;		/* length of the fact chunk */
	ULONG wSamplesWritten=0;	/* windows doesnt seem to use this*/

	/* data chunk */
	ULONG  wDataLength=0x7ffff000L;	/* length of sound data in bytes */
	/* end of variables written to header */

	/* internal variables, intermediate values etc */
	ULONG bytespersample; 		/* (uncompressed) bytes per sample (per channel) */
	ULONG blocksWritten = 0;

	int rc;

	wSamplesPerSecond = ft->info.rate;
	wChannels = ft->info.channels;

	switch (ft->info.size)
	{
		case ST_SIZE_BYTE:
		        wBitsPerSample = 8;
			if (ft->info.encoding != ST_ENCODING_UNSIGNED &&
			    ft->info.encoding != ST_ENCODING_ULAW &&
			    ft->info.encoding != ST_ENCODING_ALAW &&
			    ft->info.encoding != ST_ENCODING_GSM)
			{
				st_warn("Do not support %s with 8-bit data.  Forcing to unsigned",st_encodings_str[ft->info.encoding]);
				ft->info.encoding = ST_ENCODING_UNSIGNED;
			}
			break;
		case ST_SIZE_WORD:
			wBitsPerSample = 16;
			if (ft->info.encoding != ST_ENCODING_SIGN2)
			{
				st_warn("Do not support %s with 16-bit data.  Forcing to Signed.",st_encodings_str[ft->info.encoding]);
				ft->info.encoding = ST_ENCODING_SIGN2;
			}
			break;
		case ST_SIZE_DWORD:
			wBitsPerSample = 32;
			if (ft->info.encoding != ST_ENCODING_SIGN2)
			{
				st_warn("Do not support %s with 16-bit data.  Forcing to Signed.",st_encodings_str[ft->info.encoding]);
				ft->info.encoding = ST_ENCODING_SIGN2;
			}

			break;
		default:
			st_warn("Do not support %s in WAV files.  Forcing to Signed Words.",st_sizes_str[ft->info.size]);
			ft->info.encoding = ST_ENCODING_SIGN2;
			ft->info.size = ST_SIZE_WORD;
			wBitsPerSample = 16;
			break;
	}

	if (ft->info.encoding != ST_ENCODING_ADPCM &&
	    ft->info.encoding != ST_ENCODING_IMA_ADPCM &&
	    ft->info.encoding != ST_ENCODING_GSM)
	{
		rc = st_rawstartwrite(ft);
		if (rc)
		    return rc;
	}

	wSamplesPerBlock = 1;	/* common default for PCM data */

	switch (ft->info.encoding)
	{
		case ST_ENCODING_UNSIGNED:
		case ST_ENCODING_SIGN2:
			wFormatTag = WAVE_FORMAT_PCM;
	    		bytespersample = (wBitsPerSample + 7)/8;
	    		wBlockAlign = wChannels * bytespersample;
			break;
		case ST_ENCODING_ALAW:
			wFormatTag = WAVE_FORMAT_ALAW;
	    		wBlockAlign = wChannels;
			break;
		case ST_ENCODING_ULAW:
			wFormatTag = WAVE_FORMAT_MULAW;
	    		wBlockAlign = wChannels;
			break;
		case ST_ENCODING_IMA_ADPCM:
			if (wChannels>16)
			{
			    st_fail("Channels(%d) must be <= 16\n",wChannels);
			    return ST_EOF;
			}
			wFormatTag = WAVE_FORMAT_IMA_ADPCM;
			wBlockAlign = wChannels * 256; /* reasonable default */
			wBitsPerSample = 4;
	    		wExtSize = 2;
			wSamplesPerBlock = ImaSamplesIn(0, wChannels, wBlockAlign, 0);
			break;
		case ST_ENCODING_ADPCM:
			if (wChannels>16)
			{
			    st_fail("Channels(%d) must be <= 16\n",wChannels);
			    return ST_EOF;
			}
			wFormatTag = WAVE_FORMAT_ADPCM;
			wBlockAlign = wChannels * 128; /* reasonable default */
			wBitsPerSample = 4;
	    		wExtSize = 4+4*7;      /* Ext fmt data length */
			wSamplesPerBlock = AdpcmSamplesIn(0, wChannels, wBlockAlign, 0);
			break;
		case ST_ENCODING_GSM:
#ifdef HAVE_LIBGSM
		    if (wChannels!=1)
		    {
			st_fail("Channels(%d) must be == 1\n",wChannels);
			return ST_EOF;
		    }
		    wFormatTag = WAVE_FORMAT_GSM610;
		    /* wAvgBytesPerSec = 1625*(wSamplesPerSecond/8000.)+0.5; */
		    wBlockAlign=65;
		    wBitsPerSample=0;  /* not representable as int   */
		    wExtSize=2;        /* length of format extension */
		    wSamplesPerBlock = 320;
#else
		    st_fail("sorry, no GSM6.10 support, recompile sox with gsm library");
		    return ST_EOF;
#endif
		    break;
	}
	wav->formatTag = wFormatTag;
	wav->blockAlign = wBlockAlign;
	wav->samplesPerBlock = wSamplesPerBlock;

	if (!second_header) { 	/* adjust for blockAlign */
	    blocksWritten = wDataLength/wBlockAlign;
	    wDataLength = blocksWritten * wBlockAlign;
	    wSamplesWritten = blocksWritten * wSamplesPerBlock;
	} else { 	/* fixup with real length */
	    wSamplesWritten = wav->numSamples;
	    switch(wFormatTag)
		{
	    	case WAVE_FORMAT_ADPCM:
	    	case WAVE_FORMAT_IMA_ADPCM:
		    wDataLength = wav->dataLength;
		    break;
#ifdef HAVE_LIBGSM
		case WAVE_FORMAT_GSM610:
		    /* intentional case fallthrough! */
#endif
		default:
		    wSamplesWritten /= wChannels; /* because how rawwrite()'s work */
		    blocksWritten = (wSamplesWritten+wSamplesPerBlock-1)/wSamplesPerBlock;
		    wDataLength = blocksWritten * wBlockAlign;
		}
	}

#ifdef HAVE_LIBGSM
	if (wFormatTag == WAVE_FORMAT_GSM610)
	    wDataLength = (wDataLength+1) & ~1; /*round up to even */
#endif

	if (wFormatTag != WAVE_FORMAT_PCM)
	    wFmtSize += 2+wExtSize; /* plus ExtData */

	wRiffLength = 4 + (8+wFmtSize) + (8+wDataLength); 
	if (wFormatTag != WAVE_FORMAT_PCM) /* PCM omits the "fact" chunk */
	    wRiffLength += (8+wFactSize);
	
	/* wAvgBytesPerSec <-- this is BEFORE compression, isn't it? guess not. */
	wAvgBytesPerSec = (double)wBlockAlign*ft->info.rate / (double)wSamplesPerBlock + 0.5;

	/* figured out header info, so write it */
	st_writes(ft, "RIFF");
	st_writedw(ft, wRiffLength);
	st_writes(ft, "WAVE");
	st_writes(ft, "fmt ");
	st_writedw(ft, wFmtSize);
	st_writew(ft, wFormatTag);
	st_writew(ft, wChannels);
	st_writedw(ft, wSamplesPerSecond);
	st_writedw(ft, wAvgBytesPerSec);
	st_writew(ft, wBlockAlign);
	st_writew(ft, wBitsPerSample); /* end info common to all fmts */

	/* if not PCM, we need to write out wExtSize even if wExtSize=0 */
	if (wFormatTag != WAVE_FORMAT_PCM)
	    st_writew(ft,wExtSize);

	switch (wFormatTag)
	{
	int i;
	case WAVE_FORMAT_IMA_ADPCM:
	    st_writew(ft, wSamplesPerBlock);
	    break;
	case WAVE_FORMAT_ADPCM:
	    st_writew(ft, wSamplesPerBlock);
	    st_writew(ft, 7); /* nCoefs */
	    for (i=0; i<7; i++) {
	      st_writew(ft, iCoef[i][0]);
	      st_writew(ft, iCoef[i][1]);
	    }
	    break;
#ifdef HAVE_LIBGSM
	case WAVE_FORMAT_GSM610:
	    st_writew(ft, wSamplesPerBlock);
	    break;
#endif
	default:
	    break;
	}

	/* if not PCM, write the 'fact' chunk */
	if (wFormatTag != WAVE_FORMAT_PCM){
	    st_writes(ft, "fact");
	    st_writedw(ft,wFactSize); 
	    st_writedw(ft,wSamplesWritten);
	}

	st_writes(ft, "data");
	st_writedw(ft, wDataLength);		/* data chunk size */

	if (!second_header) {
		st_report("Writing Wave file: %s format, %d channel%s, %d samp/sec",
	        	wav_format_str(wFormatTag), wChannels,
	        	wChannels == 1 ? "" : "s", wSamplesPerSecond);
		st_report("        %d byte/sec, %d block align, %d bits/samp",
	                wAvgBytesPerSec, wBlockAlign, wBitsPerSample);
	} else {
		st_report("Finished writing Wave file, %u data bytes %u samples\n",
			wDataLength,wav->numSamples);
#ifdef HAVE_LIBGSM
		if (wFormatTag == WAVE_FORMAT_GSM610){
		    st_report("GSM6.10 format: %u blocks %u padded samples %u padded data bytes\n",
			blocksWritten, wSamplesWritten, wDataLength);
		    if (wav->gsmbytecount != wDataLength)
			st_warn("help ! internal inconsistency - data_written %u gsmbytecount %u",
				wDataLength, wav->gsmbytecount);

		}
#endif
	}
	return ST_SUCCESS;
}

LONG st_wavwrite(ft, buf, len) 
ft_t ft;
LONG *buf, len;
{
	wav_t	wav = (wav_t) ft->priv;
	LONG	save_len = len;

	switch (wav->formatTag)
	{
	case WAVE_FORMAT_IMA_ADPCM:
	case WAVE_FORMAT_ADPCM:
	    while (len>0) {
		short *p = wav->samplePtr;
		short *top = wav->sampleTop;

		if (top>p+len) top = p+len;
		len -= top-p; /* update residual len */
		while (p < top)
		   *p++ = (*buf++) >> 16;

		wav->samplePtr = p;
		if (p == wav->sampleTop)
		    xxxAdpcmWriteBlock(ft);

	    }
	    return save_len - len;
	    break;

#ifdef HAVE_LIBGSM
	case WAVE_FORMAT_GSM610:
	    wav->numSamples += len;
	    return wavgsmwrite(ft, buf, len);
	    break;
#endif
	default:
	    wav->numSamples += len; /* must later be divided by wChannels */
	    return st_rawwrite(ft, buf, len);
	}
}

int st_wavstopwrite(ft) 
ft_t ft;
{
	wav_t	wav = (wav_t) ft->priv;
	int	rc = ST_SUCCESS;

	/* Call this to flush out any remaining data. */
	switch (wav->formatTag)
	{
	case WAVE_FORMAT_IMA_ADPCM:
	case WAVE_FORMAT_ADPCM:
	    rc = xxxAdpcmWriteBlock(ft);
	    break;
#ifdef HAVE_LIBGSM
	case WAVE_FORMAT_GSM610:
	    wavgsmstopwrite(ft);
	    break;
#endif
	default:
	    rc = st_rawstopwrite(ft);
	}
	if (wav->packet) free(wav->packet);
 	if (wav->samples) free(wav->samples);
 	if (wav->iCoefs) free(wav->iCoefs);

	/* Now that we've free()'d memory, return with errors if needed */
	if (rc)
	    return rc;

	/* All samples are already written out. */
	/* If file header needs fixing up, for example it needs the */
 	/* the number of samples in a field, seek back and write them here. */
	if (!ft->seekable)
		return ST_EOF;

	if (fseek(ft->fp, 0L, SEEK_SET) != 0)
	{
		st_fail("Sorry, can't rewind output file to rewrite .wav header.");
		return ST_EOF;
	}
	wavwritehdr(ft, 1);
	return (ST_SUCCESS);
}

/*
 * Return a string corresponding to the wave format type.
 */
static char *
wav_format_str(wFormatTag) 
unsigned wFormatTag;
{
	switch (wFormatTag)
	{
		case WAVE_FORMAT_UNKNOWN:
			return "Microsoft Official Unknown";
		case WAVE_FORMAT_PCM:
			return "Microsoft PCM";
		case WAVE_FORMAT_ADPCM:
			return "Microsoft ADPCM";
	        case WAVE_FORMAT_IEEE_FLOAT:
		       return "IEEE Float";
		case WAVE_FORMAT_ALAW:
			return "Microsoft A-law";
		case WAVE_FORMAT_MULAW:
			return "Microsoft U-law";
		case WAVE_FORMAT_OKI_ADPCM:
			return "OKI ADPCM format.";
		case WAVE_FORMAT_IMA_ADPCM:
			return "IMA ADPCM";
		case WAVE_FORMAT_DIGISTD:
			return "Digistd format.";
		case WAVE_FORMAT_DIGIFIX:
			return "Digifix format.";
		case WAVE_FORMAT_DOLBY_AC2:
			return "Dolby AC2";
		case WAVE_FORMAT_GSM610:
			return "GSM 6.10";
		case WAVE_FORMAT_ROCKWELL_ADPCM:
			return "Rockwell ADPCM";
		case WAVE_FORMAT_ROCKWELL_DIGITALK:
			return "Rockwell DIGITALK";
		case WAVE_FORMAT_G721_ADPCM:
			return "G.721 ADPCM";
		case WAVE_FORMAT_G728_CELP:
			return "G.728 CELP";
		case WAVE_FORMAT_MPEG:
			return "MPEG";
		case WAVE_FORMAT_MPEGLAYER3:
			return "MPEG Layer 3";
		case WAVE_FORMAT_G726_ADPCM:
			return "G.726 ADPCM";
		case WAVE_FORMAT_G722_ADPCM:
			return "G.722 ADPCM";
		default:
			return "Unknown";
	}
}
