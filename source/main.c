/*
 * WiiEAS — live EAS alerts from the GlobalEAS Central Alert Repository, in a
 * DASDEC-style character generator. Idea proposed by RetrokVR (Jun 2026).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <grrlib.h>
#include <wiiuse/wpad.h>

#include "http_tls.h"
#include "car_api.h"
#include "dasdec.h"
#include "input.h"
#include "audio_player.h"

#include <asndlib.h>

/* Refresh cadence in frames (~60 s NTSC), matching the CAR site's 60 s poll.
 * Frame-counted because ticks_to_millisecs runs hot on Dolphin. */
#define POLL_INTERVAL_FRAMES (60 * 60)
/* Frame-based toast dwell (same for normal + error). ~1 s at 60 Hz. */
#define STATUS_HOLD_FRAMES 60
/* In-progress states never time out; the outcome status replaces them. */
#define STATUS_STICKY 0xFFFFFFFFu
/* Quiet frames before a queued alert may grab the screen, so pending
 * auto-play can't yank you mid-browse. Each button press re-arms the wait. */
#define AUTOPLAY_DEFER_FRAMES 120

static CarAlertList g_list;
static int g_index;               /* current alert index, 0..count-1 */
static int g_auto_play = 1;       /* auto-play newly appeared alerts */
static char g_status[160];
static u32 g_status_frames_left;
static char g_ip[16];

/* Hashes we have already auto-announced this session. */
static char g_seen[CAR_MAX_ALERTS][CAR_HASH_LEN + 1];
static int  g_seen_count;

static void set_status_hold(const char *msg, u32 frames)
{
	snprintf(g_status, sizeof(g_status), "%s", msg ? msg : "");
	/* Upper-cased once here to match the all-caps bottom line. */
	for (char *p = g_status; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	g_status_frames_left = frames;
}

static void set_status(const char *msg)
{
	set_status_hold(msg, STATUS_HOLD_FRAMES);
}

/* One settled sticky frame before a blocking stretch; drawn twice so both
 * GRRLIB flip buffers match and a mid-fetch flip can't show a stale frame. */
static void present(const char *msg)
{
	if (msg)
		set_status_hold(msg, STATUS_STICKY);
	dasdec_draw(g_status, g_auto_play, 0);
	GRRLIB_Render();
	dasdec_draw(g_status, g_auto_play, 0);
	GRRLIB_Render();
}

/* Status hook for the long phases inside audio_play_alert(). */
static void audio_progress(const char *msg)
{
	present(msg);
}

static int hash_seen(const char *hash)
{
	if (!hash || !hash[0])
		return 1;
	for (int i = 0; i < g_seen_count; i++) {
		if (strcmp(g_seen[i], hash) == 0)
			return 1;
	}
	return 0;
}

static void mark_seen(const char *hash)
{
	if (!hash || !hash[0] || hash_seen(hash))
		return;
	if (g_seen_count >= CAR_MAX_ALERTS) {
		/* drop oldest */
		memmove(g_seen[0], g_seen[1],
		        (size_t)(CAR_MAX_ALERTS - 1) * (CAR_HASH_LEN + 1));
		g_seen_count = CAR_MAX_ALERTS - 1;
	}
	snprintf(g_seen[g_seen_count], sizeof(g_seen[0]), "%s", hash);
	g_seen_count++;
}

static void show_current(void)
{
	if (g_list.count <= 0) {
		dasdec_set_alert(NULL, 0, 0);
		return;
	}
	if (g_index < 0)
		g_index = 0;
	if (g_index >= g_list.count)
		g_index = g_list.count - 1;
	dasdec_set_alert(&g_list.alerts[g_index], g_index + 1, g_list.count);
}

static void play_current(void)
{
	if (g_list.count <= 0)
		return;
	const CarAlert *a = &g_list.alerts[g_index];

	/* A while playing → pause. The clip stays decoded until you leave this
	 * alert, so A again resumes instantly. */
	if (audio_is_playing()) {
		audio_pause();
		set_status("Paused");
		return;
	}

	if (!a->audio_url[0]) {
		set_status("No audio for this alert");
		return;
	}

	/* Only announce a download when one is actually about to happen. */
	if (!audio_alert_cached(a))
		present("Downloading...");

	char err[128];
	if (audio_play_alert(a, audio_progress, err, sizeof(err)) != 0) {
		char buf[160];
		snprintf(buf, sizeof(buf), "Audio: %s", err);
		set_status(buf);
		return;
	}
	mark_seen(a->hash);
	set_status("Playing");
}

/* Refresh the active list; *out_new_index gets the newest unseen alert (or
 * -1). Returns 1 if anything new, 0 if none, -1 on failure. */
static int refresh_alerts(int *out_new_index)
{
	if (out_new_index)
		*out_new_index = -1;

	char err[128];
	/* MUST be static: ~126 KB against libogc's 128 KB main stack — as a
	 * local, the mbedTLS handshake below it overran into libogc BSS. */
	static CarAlertList fresh;
	present("Fetching alerts...");

	if (car_fetch_active(&fresh, err, sizeof(err)) != 0) {
		char buf[160];
		snprintf(buf, sizeof(buf), "Fetch failed: %s", err);
		set_status(buf);
		/* Keep showing a good alert through a dropped poll; with nothing to
		 * show, say why instead of leaving "LOADING..." up forever. */
		if (g_list.count <= 0)
			dasdec_set_message("NO CONNECTION");
		return -1;
	}

	/* Hand real UTC (from the Date: header) to the details panel so relative
	 * times aren't shifted by the console's local clock. */
	dasdec_set_utc_offset(http_utc_offset());

	/* Preserve current selection by hash if possible. */
	char cur_hash[CAR_HASH_LEN + 1] = {0};
	if (g_list.count > 0 && g_index >= 0 && g_index < g_list.count)
		snprintf(cur_hash, sizeof(cur_hash), "%s", g_list.alerts[g_index].hash);

	/* Find first brand-new alert (API returns newest first). */
	int first_new = -1;
	for (int i = 0; i < fresh.count; i++) {
		if (!hash_seen(fresh.alerts[i].hash)) {
			first_new = i;
			break;
		}
	}

	g_list = fresh;

	if (cur_hash[0]) {
		int found = -1;
		for (int i = 0; i < g_list.count; i++) {
			if (strcmp(g_list.alerts[i].hash, cur_hash) == 0) {
				found = i;
				break;
			}
		}
		g_index = (found >= 0) ? found : 0;
	} else {
		g_index = 0;
	}

	show_current();

	char buf[160];
	snprintf(buf, sizeof(buf), "Active alerts: %d  (ip %s)", g_list.count, g_ip);
	set_status(buf);

	if (out_new_index)
		*out_new_index = first_new;
	return (first_new >= 0) ? 1 : 0;
}

static void apply_current(void)
{
	/* Unconditional: leaving an alert must kill its audio, and this also
	 * frees the multi-MB PCM left behind when a clip ends naturally. */
	audio_stop();
	show_current();
}

/* Safe return to HBC — never call WPAD_Shutdown (can hang on hardware). */
static void app_quit(void)
{
	audio_stop();
	ASND_Pause(1);
	audio_shutdown();
	dasdec_shutdown();
	/* Skip input_shutdown() → WPAD_Shutdown: hardware test hard-froze on hold-Start quit. */
	GRRLIB_Exit();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	exit(0);
}

static void video_blank_early(void)
{
	/* Clear a framebuffer to black before GRRLIB takes over — kills the
	 * pink/black launch stripes seen on real hardware. */
	GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
	if (!mode)
		return;
	void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
	if (!xfb)
		return;
	VIDEO_Configure(mode);
	VIDEO_ClearFrameBuffer(mode, xfb, COLOR_BLACK);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (mode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* Init order: video → pads → audio; network last, and it must not
	 * re-init WPAD. */
	VIDEO_Init();
	video_blank_early();
	GRRLIB_Init();
	/* Extra black frames so the bezel never shows uninit EFB garbage. */
	GRRLIB_FillScreen(0x000000FF);
	GRRLIB_Render();
	GRRLIB_FillScreen(0x000000FF);
	GRRLIB_Render();

	input_init();
	audio_init();

	if (dasdec_init() != 0) {
		/* Still runnable; text just won't show. */
	}

	g_list.count = 0;
	g_index = 0;
	/* Not "NO ACTIVE ALERTS" — that's reserved for a fetch that actually
	 * came back empty. */
	dasdec_set_message("LOADING...");
	present("Starting network...");

	/* Drain boot-time button edges (Dolphin can inject Start), drawing each
	 * frame so the screen isn't blank going into the blocking network call. */
	for (int i = 0; i < 8; i++) {
		input_poll();
		dasdec_draw(g_status, g_auto_play, 0);
		GRRLIB_Render();
	}

	if (net_bringup(g_ip, sizeof(g_ip)) != 0) {
		set_status("Network init failed - check connection");
		snprintf(g_ip, sizeof(g_ip), "?.?.?.?");
	} else {
		char buf[80];
		snprintf(buf, sizeof(buf), "Network up (%s)", g_ip);
		set_status(buf);
	}

	/* Seed "seen" with everything already active so first launch doesn't
	 * blast every open alert's audio; new arrivals still auto-play. */
	int dummy;
	if (refresh_alerts(&dummy) >= 0) {
		for (int i = 0; i < g_list.count; i++)
			mark_seen(g_list.alerts[i].hash);
	}

	/* Drain edges after the blocking fetch (no WPAD reinit) and paint the
	 * fetched alerts immediately. */
	for (int i = 0; i < 3; i++) {
		input_poll();
		dasdec_draw(g_status, g_auto_play, 0);
		GRRLIB_Render();
	}

	u32 poll_frames = 0;  /* frames since the last alert refresh */
	int pending_new = -1; /* index to auto-play when idle */
	int show_details = 0;
	u32 home_hold_frames = 0;
	u32 manual_hold_frames = 0; /* keeps auto-play off your back while browsing */

	while (1) {
		u32 actions = input_poll();

		if (manual_hold_frames > 0)
			manual_hold_frames--;

		/* Hold HOME/START ~0.5 s to quit; short START still opens details. */
		if (input_home_held()) {
			home_hold_frames++;
			if (home_hold_frames >= 30)
				app_quit();
		} else {
			home_hold_frames = 0;
		}

		/* + toggles the details panel (sent / expires). B also closes it
		 * when open, otherwise B still toggles auto-play. */
		if (actions & IN_PLUS) {
			show_details = !show_details;
		} else if (show_details && (actions & IN_B)) {
			show_details = 0;
		} else if (!show_details && (actions & IN_B)) {
			g_auto_play = !g_auto_play;
			set_status(g_auto_play ? "Auto-play ON" : "Auto-play OFF");
		}

		/* Browsing works while playing — apply_current() stops audio first. */
		if (actions & IN_LEFT) {
			if (g_list.count > 0) {
				g_index = (g_index + g_list.count - 1) % g_list.count;
				apply_current();
				manual_hold_frames = AUTOPLAY_DEFER_FRAMES;
			}
		}
		if (actions & IN_RIGHT) {
			if (g_list.count > 0) {
				g_index = (g_index + 1) % g_list.count;
				apply_current();
				manual_hold_frames = AUTOPLAY_DEFER_FRAMES;
			}
		}

		/* Pages: D-pad up/down + face 1/2 (not +/− — those own the panel). */
		if (!show_details) {
			if (actions & (IN_2 | IN_DOWN))
				dasdec_next_page();
			if (actions & (IN_1 | IN_UP))
				dasdec_prev_page();
		}

		if (actions & IN_A) {
			play_current();
			manual_hold_frames = AUTOPLAY_DEFER_FRAMES;
		}

		/* Auto page flip for long messages (pause while the panel is open). */
		if (!show_details)
			dasdec_auto_page_tick();

		/* Periodic poll for new alerts */
		poll_frames++;
		if (poll_frames >= POLL_INTERVAL_FRAMES) {
			poll_frames = 0;
			int new_idx = -1;
			int rc = refresh_alerts(&new_idx);
			if (rc > 0 && g_auto_play && new_idx >= 0)
				pending_new = new_idx;
		}

		/* When idle *and* you're not mid-browse, auto-play the pending alert */
		if (pending_new >= 0 && g_auto_play && !audio_is_playing()
		    && manual_hold_frames == 0) {
			if (pending_new < g_list.count) {
				g_index = pending_new;
				apply_current();
				play_current();
			}
			pending_new = -1;
		}

		/* Expire the status toast; sticky in-progress states hold until
		 * their outcome replaces them. A lapsed toast leaves the line empty. */
		const char *status = g_status;
		if (g_status_frames_left > 0 && g_status_frames_left != STATUS_STICKY)
			g_status_frames_left--;
		if (g_status_frames_left == 0)
			status = "";

		dasdec_draw(status, g_auto_play, show_details);
		GRRLIB_Render();
	}

	app_quit();
	return 0;
}
