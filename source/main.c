/**
 * WiiEAS — live EAS alerts from GlobalEAS Central Alert Repository,
 * shown in a DASDEC-style character generator on the Nintendo Wii.
 *
 * Idea originally proposed by RetrokVR (forum thread, Jun 2026).
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
#include <stdlib.h>

/*
 * Alert list refresh cadence, in frames: 3600 ≈ 60 s at 60 Hz NTSC (72 s on
 * 50 Hz PAL) — the CAR website's realtime.js polls at the same 60 s.
 * Frame-counted rather than ticks for the same reason as the page timer in
 * dasdec.c: ticks_to_millisecs runs hot on Dolphin and fired the poll early.
 */
#define POLL_INTERVAL_FRAMES (60 * 60)
/* Frame-based toast dwell (same for normal + error). ~1 s at 60 Hz. */
#define STATUS_HOLD_FRAMES 60
/*
 * Dwell for in-progress states (downloading, decoding, fetching): they never
 * time out on their own — the completion or failure status replaces them.
 * Before this, "DOWNLOADING..." could lapse after 1 s with the transfer still
 * going and the screen saying nothing at all.
 */
#define STATUS_STICKY 0xFFFFFFFFu
/*
 * Frames of quiet required before a queued new alert may grab the screen.
 * Without it, the moment you stop or step off a playing alert the pending
 * auto-play fires in that same frame and yanks you to its own alert — which
 * looks exactly like "browsing while audio plays doesn't work". Each new
 * button press re-arms the wait, so an idle Wii still announces immediately.
 */
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
	/* Upper-cased here, once, so the top line reads as the same kind of text
	 * as the all-caps index line at the bottom. */
	for (char *p = g_status; *p; p++)
		*p = (char)toupper((unsigned char)*p);
	g_status_frames_left = frames;
}

static void set_status(const char *msg)
{
	set_status_hold(msg, STATUS_HOLD_FRAMES);
}

/*
 * Paint one settled frame, setting a *sticky* status first — present() is
 * only ever called for in-progress states right before a blocking stretch,
 * and those must stay on screen until their outcome replaces them.
 *
 * Rendered twice on purpose. GRRLIB flips between two framebuffers, so a single
 * render leaves the *other* one holding whatever was there before — at boot,
 * black. Nothing redraws during network bring-up or an HTTPS fetch, so any flip
 * during those seconds put that stale black buffer on screen and left it there.
 * Drawing twice puts the same picture in both, making the flip invisible; the
 * boot path has always fired two black frames back to back for this reason.
 */
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

	/*
	 * A while playing → pause. The decoded clip stays in memory until you
	 * leave this alert, so pressing A again resumes on the spot: no download,
	 * no decode, nothing to wait out after a mis-press.
	 */
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

/*
 * Refresh active list from CAR. If auto-play is on, any brand-new hashes
 * (not in g_seen) are queued: we jump to the newest unseen and play it
 * once the previous clip finishes (or immediately if idle).
 *
 * Returns number of brand-new alerts discovered.
 */
static int refresh_alerts(int *out_new_index)
{
	if (out_new_index)
		*out_new_index = -1;

	char err[128];
	/*
	 * MUST be static. sizeof(CarAlertList) is ~126 KB and libogc gives main a
	 * 128 KB stack (s_mainStack = 0x20000), so as a local this left ~4 KB for
	 * car_fetch_active -> https_get (needs ~4 KB by itself) -> the whole
	 * mbedTLS handshake. Every fetch ran off the bottom of the stack into
	 * neighbouring libogc BSS — which is what was killing wiiuse state after
	 * init, on hardware and in Dolphin alike. refresh_alerts is only ever
	 * called from the main loop, so a single shared buffer is safe.
	 */
	static CarAlertList fresh;
	present("Fetching alerts...");

	if (car_fetch_active(&fresh, err, sizeof(err)) != 0) {
		char buf[160];
		snprintf(buf, sizeof(buf), "Fetch failed: %s", err);
		set_status(buf);
		/*
		 * Keep showing a good alert if we have one — a dropped poll says
		 * nothing about it. With nothing to show, say why rather than sitting
		 * on "LOADING..." forever; the 60 s poll keeps trying.
		 */
		if (g_list.count <= 0)
			dasdec_set_message("NO CONNECTION");
		return -1;
	}

	/*
	 * Each response's Date: header tells us real UTC; hand the offset to the
	 * details panel so "sent N minutes ago" is measured against UTC and not
	 * the console's local clock (which put fresh alerts "in the future").
	 */
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
	/*
	 * Leaving an alert must kill its audio — never bleed onto the next one.
	 * Unconditional: audio_stop() is idempotent, and calling it even when the
	 * clip already ended on its own frees the multi-MB PCM buffer that
	 * audio_is_playing() leaves behind when playback finishes naturally.
	 */
	audio_stop();
	show_current();
}

/* Safe return to HBC — never call WPAD_Shutdown (can hang on hardware). */
static void app_quit(void)
{
	audio_stop();
	ASND_Pause(1);
	/* audio_shutdown without WPAD; stop voices only */
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
	/*
	 * Kill pink/black launch stripes seen on real hardware (not Dolphin):
	 * clear a framebuffer to black before GRRLIB takes over.
	 */
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

	/*
	 * VectrexWii: GRRLIB_Init → WPAD → PAD.
	 * Pads before audio. Network comes later and must not re-init WPAD.
	 */
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
	/*
	 * Not "NO ACTIVE ALERTS" — we haven't asked anyone yet. That message is
	 * a statement of fact and must wait until a fetch has actually come back
	 * empty (show_current(), below).
	 */
	dasdec_set_message("LOADING...");
	present("Starting network...");

	/*
	 * Drain boot-time button edges (Dolphin sometimes injects Start). Draws
	 * each frame rather than bare VIDEO_WaitVSync — an unrendered stretch this
	 * close to the blocking network call is what left the screen blank.
	 */
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

	/* Initial fetch — seed "seen" with whatever is already active so we
	 * don't blast every open alert's audio on first launch. User can still
	 * press A to play the current one. New arrivals after this will auto-play. */
	int dummy;
	if (refresh_alerts(&dummy) >= 0) {
		for (int i = 0; i < g_list.count; i++)
			mark_seen(g_list.alerts[i].hash);
	}

	/* Drain edges after blocking network — do not reinit WPAD (Vectrex never
	 * does). Drawing here also puts the fetched alerts up immediately instead
	 * of leaving three unpainted frames before the main loop's first render. */
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

		/*
		 * Hold HOME (Wiimote) or START (GC) ~0.5s to quit.
		 * Short START still opens details (edge → IN_PLUS).
		 */
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

		/*
		 * Browsing works at any time, playing or not — apply_current() stops
		 * whatever is speaking before the new alert goes up.
		 */
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

		/*
		 * Expire status toast (frame-counted). Sticky in-progress states are
		 * exempt — they hold until their outcome replaces them. Nothing takes
		 * a lapsed toast's place: the controls legend that used to fill the
		 * idle line lives in the README now, so the bezel keeps a single line
		 * that speaks only when there is something to say.
		 */
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
