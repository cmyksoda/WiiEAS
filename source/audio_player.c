/**
 * Alert audio — libmad decode + ASND (the path that sounded good).
 *
 * - Decode to *native* rate mono s16 (EAS is 16 kHz); pitch = that rate.
 * - Feed ASND with 4096-sample (8192-byte) double buffers like stock MP3Player.
 * - Skip ID3v2 so mad never treats the tag as audio.
 * - Nothing is pinned while idle: the MP3 staging buffer lives only for the
 *   fetch+decode, and the PCM only while a clip is loaded — multi-MB blocks
 *   parked next to FreeType are what used to wipe the UI text.
 * - The PCM grows to fit the whole clip. The old fixed 60 s @ 16 kHz cap
 *   silently truncated the decode, which cut most alerts off at one minute.
 */
#include "audio_player.h"
#include "car_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <asndlib.h>
#include <mad.h>

#define BUF_SAMPLES     4096
#define NBUFS           2
/*
 * MP3 staging cap. EAS audio is regulation-capped at two minutes, well under
 * 1 MB at the CDN's mono rates — the old 512 KB cap could truncate the
 * *download* of longer files before the decoder even saw them.
 */
#define AUDIO_MP3_CAP   (2u * 1024 * 1024)
/* First PCM allocation: ~60 s @ 16 kHz. pcm_grow() extends it as the decode
 * demands, up to PCM_MAX_SAMPLES. */
#define PCM_INITIAL_SAMPLES (60u * 16000u)
/*
 * Hard ceiling on decoded PCM: 6M samples (12 MB) is over six minutes at
 * 16 kHz — anything longer than this is not a real EAS clip, and unbounded
 * growth next to FreeType is how the UI died once before.
 */
#define PCM_MAX_SAMPLES (6u * 1024u * 1024u)

static s16 *s_pcm;
static u32  s_pcm_cap;
static u32  s_pcm_len;
static u32  s_pcm_rate;
static volatile u32 s_pcm_rpos;

static s16 *s_dma[NBUFS];
static int  s_dma_idx;
static volatile u8 s_running;
static int  s_ready;

/*
 * Identifies the clip currently sitting in s_pcm — the one-entry cache. Set
 * when a decode succeeds, cleared only when the PCM is released, so it stays
 * valid across a pause and lets a replay skip the download entirely.
 */
static int  s_loaded_hash_set;
static char s_loaded_hash[CAR_HASH_LEN + 1];

static size_t skip_id3v2(const u8 *data, size_t len)
{
	if (len < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3')
		return 0;
	size_t body = ((size_t)(data[6] & 0x7f) << 21)
	            | ((size_t)(data[7] & 0x7f) << 14)
	            | ((size_t)(data[8] & 0x7f) << 7)
	            | ((size_t)(data[9] & 0x7f));
	size_t total = 10 + body;
	return (total > len) ? 0 : total;
}

static s16 mad_to_s16(mad_fixed_t sample)
{
	sample += (1L << (MAD_F_FRACBITS - 16));
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	return (s16)(sample >> (MAD_F_FRACBITS + 1 - 16));
}

/*
 * Make the PCM buffer hold at least `need` samples, preserving what has been
 * decoded so far. Decode-time only: the voice callback is guaranteed idle
 * (s_running == 0, playback not yet started), so swapping the pointer is safe.
 * Returns 0 when the buffer is big enough, -1 when it can't be (out of memory
 * or past the hard cap) — the caller then keeps what it has.
 */
static int pcm_grow(u32 need)
{
	if (need <= s_pcm_cap && s_pcm)
		return 0;
	if (need > PCM_MAX_SAMPLES)
		return -1;

	u32 new_cap = s_pcm_cap ? s_pcm_cap : PCM_INITIAL_SAMPLES;
	while (new_cap < need)
		new_cap *= 2;
	if (new_cap > PCM_MAX_SAMPLES)
		new_cap = PCM_MAX_SAMPLES;

	s16 *np = (s16 *)memalign(32, new_cap * sizeof(s16));
	if (!np)
		return -1;
	if (s_pcm && s_pcm_len)
		memcpy(np, s_pcm, s_pcm_len * sizeof(s16));

	s16 *old = s_pcm;
	s_pcm = np;
	s_pcm_cap = new_cap;
	free(old);
	return 0;
}

static void free_pcm(void)
{
	/*
	 * Clear the pointer *before* releasing the block. voice_cb runs off the
	 * DSP interrupt and reads s_pcm, so it must never be able to see a pointer
	 * into freed memory — the window switching alerts mid-playback walks
	 * straight through here.
	 */
	s16 *pcm = s_pcm;
	s_pcm = NULL;
	s_pcm_cap = 0;
	s_pcm_len = 0;
	s_pcm_rpos = 0;
	s_pcm_rate = 0;
	/* The hash names what was in this buffer, so it dies with it. */
	s_loaded_hash_set = 0;
	s_loaded_hash[0] = '\0';
	free(pcm);
}

static int decode_mp3_native(const u8 *mp3, size_t mp3_len, char *err, size_t err_len)
{
	struct mad_stream stream;
	struct mad_frame frame;
	struct mad_synth synth;

	size_t skip = skip_id3v2(mp3, mp3_len);
	mp3 += skip;
	mp3_len -= skip;

	if (pcm_grow(PCM_INITIAL_SAMPLES) != 0) {
		if (err && err_len)
			snprintf(err, err_len, "PCM alloc failed");
		return -1;
	}

	s_pcm_len = 0;
	s_pcm_rpos = 0;
	s_pcm_rate = 0;

	mad_stream_init(&stream);
	mad_frame_init(&frame);
	mad_synth_init(&synth);
	mad_stream_buffer(&stream, mp3, mp3_len);

	while (1) {
		if (mad_frame_decode(&frame, &stream) == -1) {
			if (stream.error == MAD_ERROR_BUFLEN)
				break;
			if (!MAD_RECOVERABLE(stream.error)) {
				if (err && err_len)
					snprintf(err, err_len, "mad error 0x%04x",
					         (unsigned)stream.error);
				mad_synth_finish(&synth);
				mad_frame_finish(&frame);
				mad_stream_finish(&stream);
				return -1;
			}
			continue;
		}

		mad_synth_frame(&synth, &frame);

		unsigned nch = synth.pcm.channels;
		unsigned ns = synth.pcm.length;
		if (ns == 0 || nch == 0)
			continue;

		if (s_pcm_rate == 0) {
			s_pcm_rate = synth.pcm.samplerate ? synth.pcm.samplerate : 16000;
			/*
			 * Size the buffer for the whole clip in one go: for CBR (which
			 * these are), samples ≈ bytes * 8 / bitrate * rate. ~6% headroom
			 * plus one DMA buffer for the tail padding; a VBR undershoot is
			 * caught by the grow in the append path below. Best effort — on
			 * failure the append path decides what we keep.
			 */
			if (frame.header.bitrate > 0) {
				u64 est = (u64)mp3_len * 8u * s_pcm_rate
				          / frame.header.bitrate;
				est += est / 16 + BUF_SAMPLES;
				if (est > PCM_MAX_SAMPLES)
					est = PCM_MAX_SAMPLES;
				pcm_grow((u32)est);
			}
		}

		/* Out of room and can't grow — keep what we have (never silently
		 * cap at a fixed length again; this is what cut clips at 60 s). */
		if (s_pcm_len + ns > s_pcm_cap &&
		    pcm_grow(s_pcm_len + ns) != 0)
			break;

		for (unsigned i = 0; i < ns; i++) {
			s16 s = mad_to_s16(synth.pcm.samples[0][i]);
			if (nch > 1) {
				s16 r = mad_to_s16(synth.pcm.samples[1][i]);
				s = (s16)(((int)s + (int)r) / 2);
			}
			s_pcm[s_pcm_len++] = s;
		}
	}

	mad_synth_finish(&synth);
	mad_frame_finish(&frame);
	mad_stream_finish(&stream);

	if (s_pcm_len < 256) {
		if (err && err_len)
			snprintf(err, err_len, "decoded PCM too short (%u)",
			         (unsigned)s_pcm_len);
		return -1;
	}

	/* Pad to a whole DMA buffer — an unpadded tail would be dropped, since
	 * fill_dma() only copies full BUF_SAMPLES chunks. */
	while (s_pcm_len % BUF_SAMPLES) {
		if (s_pcm_len + 1 > s_pcm_cap &&
		    pcm_grow(s_pcm_len + 1) != 0)
			break;
		s_pcm[s_pcm_len++] = 0;
	}

	DCFlushRange(s_pcm, s_pcm_len * sizeof(s16));
	return 0;
}

static s16 *fill_dma(void)
{
	s16 *buf = s_dma[s_dma_idx];
	u32 pos = s_pcm_rpos;

	if (s_pcm && pos + BUF_SAMPLES <= s_pcm_len)
		memcpy(buf, s_pcm + pos, BUF_SAMPLES * sizeof(s16));
	else
		memset(buf, 0, BUF_SAMPLES * sizeof(s16));

	DCFlushRange(buf, BUF_SAMPLES * sizeof(s16));
	return buf;
}

static void commit_dma(void)
{
	s_pcm_rpos += BUF_SAMPLES;
	s_dma_idx ^= 1;
}

static void voice_cb(s32 voice)
{
	(void)voice;
	if (!s_running || !s_pcm)
		return;

	if (s_pcm_rpos >= s_pcm_len) {
		s_running = 0;
		return;
	}

	if (ASND_AddVoice(0, fill_dma(),
	                  (s32)(BUF_SAMPLES * sizeof(s16))) == SND_OK)
		commit_dma();
}

/* `from` is a sample offset; it always lands on a BUF_SAMPLES boundary because
 * that is the only step s_pcm_rpos ever takes. Past the end restarts the clip. */
static int start_asnd_playback(u32 from)
{
	if (!s_pcm || s_pcm_len < BUF_SAMPLES || s_pcm_rate == 0)
		return -1;

	if (from >= s_pcm_len)
		from = 0;
	s_pcm_rpos = from;
	s_dma_idx = 0;
	s_running = 0;

	ASND_StopVoice(0);
	ASND_Pause(0);

	s16 *first = fill_dma();
	s_running = 1;

	if (ASND_SetVoice(0, VOICE_MONO_16BIT, (s32)s_pcm_rate, 0,
	                  first, (s32)(BUF_SAMPLES * sizeof(s16)),
	                  255, 255, voice_cb) != SND_OK) {
		s_running = 0;
		return -1;
	}
	commit_dma();

	if (s_pcm_rpos < s_pcm_len) {
		if (ASND_AddVoice(0, fill_dma(),
		                  (s32)(BUF_SAMPLES * sizeof(s16))) == SND_OK)
			commit_dma();
	}
	return 0;
}

void audio_init(void)
{
	ASND_Init();
	ASND_Pause(0);

	for (int i = 0; i < NBUFS; i++)
		s_dma[i] = (s16 *)memalign(32, BUF_SAMPLES * sizeof(s16));

	/* MP3 staging is allocated per fetch in start_play(); PCM on demand
	 * during decode. Nothing multi-MB sits on the heap while idle. */
	s_pcm = NULL;
	s_pcm_cap = 0;
	s_pcm_len = s_pcm_rpos = s_pcm_rate = 0;
	s_running = 0;
	s_ready = (s_dma[0] && s_dma[1]);
	s_loaded_hash_set = 0;
	s_loaded_hash[0] = '\0';
}

void audio_shutdown(void)
{
	audio_stop();
	ASND_Pause(1);
	ASND_End();
	free(s_dma[0]);
	free(s_dma[1]);
	s_dma[0] = s_dma[1] = NULL;
	s_ready = 0;
}

void audio_stop(void)
{
	/* Do not spin/usleep here — a long loop on the main thread during quit
	 * has hung real hardware (hold-Start → hard freeze). */
	s_running = 0;
	ASND_StopVoice(0);
	ASND_StopVoice(0);
	free_pcm();
}

void audio_pause(void)
{
	if (!s_running)
		return;
	s_running = 0;
	ASND_StopVoice(0);

	/*
	 * s_pcm_rpos is the *queued* position, which runs up to NBUFS buffers ahead
	 * of what the speakers have actually reached. Resuming from it would swallow
	 * that much audio, so wind back by the queue depth.
	 * The PCM itself is deliberately kept — that's the cache.
	 */
	u32 queued = (u32)NBUFS * BUF_SAMPLES;
	s_pcm_rpos = (s_pcm_rpos > queued) ? s_pcm_rpos - queued : 0;
}

int audio_is_playing(void)
{
	if (!s_running)
		return 0;
	if (!s_pcm || s_pcm_rpos >= s_pcm_len) {
		s_running = 0;
		return 0;
	}
	return 1;
}

int audio_alert_cached(const CarAlert *alert)
{
	if (!alert || !alert->hash[0] || !s_loaded_hash_set)
		return 0;
	if (!s_pcm || s_pcm_len < BUF_SAMPLES)
		return 0;
	return strcmp(s_loaded_hash, alert->hash) == 0;
}

static int start_play(const CarAlert *alert, audio_progress_fn progress,
                      char *err, size_t err_len)
{
	if (!s_ready) {
		if (err && err_len)
			snprintf(err, err_len, "audio not initialised");
		return -1;
	}
	if (!alert || !alert->audio_url[0]) {
		if (err && err_len)
			snprintf(err, err_len, "no audio for this alert");
		return -1;
	}

	audio_stop();
	ASND_Pause(0);

	u8 *mp3 = (u8 *)malloc(AUDIO_MP3_CAP);
	if (!mp3) {
		if (err && err_len)
			snprintf(err, err_len, "out of memory for download");
		return -1;
	}

	size_t blen = 0;
	if (car_fetch_audio(alert, mp3, AUDIO_MP3_CAP, &blen, err, err_len) != 0) {
		free(mp3);
		return -1;
	}
	if (blen < 64) {
		free(mp3);
		if (err && err_len)
			snprintf(err, err_len, "audio too short (%u bytes)", (unsigned)blen);
		return -1;
	}

	/* Decoding a full clip takes real seconds on the Wii — say so instead of
	 * letting "Downloading..." sit there after the download has finished. */
	if (progress)
		progress("Decoding audio...");

	int rc = decode_mp3_native(mp3, blen, err, err_len);
	free(mp3);
	if (rc != 0) {
		free_pcm();
		return -1;
	}

	/* Name the buffer before playing it: from here on the clip is cacheable,
	 * and free_pcm() is the only thing that takes the name back. */
	if (alert->hash[0]) {
		snprintf(s_loaded_hash, sizeof(s_loaded_hash), "%s", alert->hash);
		s_loaded_hash_set = 1;
	}

	if (start_asnd_playback(0) != 0) {
		free_pcm();
		if (err && err_len)
			snprintf(err, err_len, "ASND start failed (rate=%u len=%u)",
			         (unsigned)s_pcm_rate, (unsigned)s_pcm_len);
		return -1;
	}
	return 0;
}

int audio_play_alert(const CarAlert *alert, audio_progress_fn progress,
                     char *err, size_t err_len)
{
	/* Already decoded? Pick up where the pause left off — no network at all. */
	if (audio_alert_cached(alert)) {
		if (start_asnd_playback(s_pcm_rpos) == 0)
			return 0;
		/* Cache unusable for some reason — drop it and fetch again. */
		audio_stop();
	}
	return start_play(alert, progress, err, err_len);
}
