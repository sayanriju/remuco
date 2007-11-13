#include <xmmsclient/xmmsclient.h>
#include <xmmsclient/xmmsclient-glib.h>
#include <remuco.h>
#include <signal.h>

struct _RemPPPriv {
	
	xmmsc_connection_t	*xc;
	GMainLoop			*ml;	
	RemServer			*rs;
	
	////////// X2 IPC sync result wait emulation //////////
	
	/**
	 * An extra main context we use to wait for events on the X2 IPC socket
	 * while "synchronously" waiting for a result.
	 */
	GMainContext		*x2ipc_mc;
	/**
	 * A flag indicating that the X2 IPC connections os broken. We need to know
	 * this while "synchronously" waiting for a result.
	 */
	gboolean			x2ipc_disconnected;

	////////// our representation of the X2 status //////////
	
	RemPlaybackState	ps_pbs;
	guint				ps_volume;
	RemRepeatMode		ps_repeat;
	RemShuffleMode		ps_shuffle;
	gint				ps_cap_pos;
	gboolean			ps_status_changed;	// FALSE means no change since last
											// call to rcb_synchronize() in one
											// of the above ps fields
	guint				ps_cap_id;			// id of the current active song
	gboolean			ps_cap_id_changed;	// FALSE means no change since last
											// call to rcb_synchronize()
	xmmsc_result_t		*playlist_result;	// NULL means no change since last
											// call to rcb_synchronize()
				
};

static RemPPPriv	*priv_global;	// we need a global ref to our private data
									// for the signal handler func priv_sigint()

///////////////////////////////////////////////////////////////////////////////
//
// constants
//
///////////////////////////////////////////////////////////////////////////////

/* Defined here because used more than once. */
#define XMETA_NAME_RATING	"rating"
/* Defined here because used more than once. */
#define XMETA_NAME_ART		"album_front_small"

/** X2 names of the meta informatioin we request from X2 */
static const gchar	*XMETA_NAMES[] = {
		"artist", "album",
		"title", "genre",
		"comment", "tracknr",
		"duration", "bitrate",
		XMETA_NAME_RATING, XMETA_NAME_ART };

/** Remuco names of the meta informatioin we request from X2 */
static const gchar	*RMETA_NAMES[] = {
		REM_PLOB_META_ARTIST, REM_PLOB_META_ALBUM,
		REM_PLOB_META_TITLE, REM_PLOB_META_GENRE,
		REM_PLOB_META_COMMENT, REM_PLOB_META_TRACK,
		REM_PLOB_META_LENGTH, REM_PLOB_META_BITRATE,
		REM_PLOB_META_RATING, REM_PLOB_META_ART };

/** X2 value types the meta informatioin we request from X2 */
static const gint	XMETA_TYPES[] = { 
		XMMSC_RESULT_VALUE_TYPE_STRING, XMMSC_RESULT_VALUE_TYPE_STRING,
		XMMSC_RESULT_VALUE_TYPE_STRING, XMMSC_RESULT_VALUE_TYPE_STRING,
		XMMSC_RESULT_VALUE_TYPE_STRING, XMMSC_RESULT_VALUE_TYPE_INT32,
		XMMSC_RESULT_VALUE_TYPE_INT32, XMMSC_RESULT_VALUE_TYPE_INT32,
		XMMSC_RESULT_VALUE_TYPE_INT32, XMMSC_RESULT_VALUE_TYPE_STRING };

/** Count of meta information elements we request from X2 */
#define XMETA_COUNT	10

/** Number of the meta information element 'length' (or duration) */
#define XMETA_NUM_LENGTH	6

/** Sets a dummy result notifier callback for 'result' and unrefs it. */
#define REMX2_RESULT_DISCARD G_STMT_START {						\
	xmmsc_result_notifier_set(result, xcb_result_ready, NULL);	\
	xmmsc_result_unref(result);									\
} G_STMT_END

///////////////////////////////////////////////////////////////////////////////
//
// xmms2 callback functions - prototypes
//
///////////////////////////////////////////////////////////////////////////////

static void
xcb_result_ready(xmmsc_result_t *result, gpointer data);

static void
xcb_result_pbs(xmmsc_result_t *result, gpointer data);

static void
xcb_disconnect(gpointer data);

static void
xcb_state_changed(xmmsc_result_t *result, gpointer data);

static void
xcb_volume_changed(xmmsc_result_t *result, gpointer data);

static void
xcb_cap_id_changed(xmmsc_result_t *result, gpointer data);

static void
xcb_cap_pos_changed(xmmsc_result_t *result, gpointer data);

static void
xcb_playlist_changed_or_loaded(xmmsc_result_t *result, gpointer data);

///////////////////////////////////////////////////////////////////////////////
//
// some functions we use to emulate sync result waiting in an async client
//
///////////////////////////////////////////////////////////////////////////////

/**
 * GIOFunc to be called, when there is data on the XMMS2 IPC socket while we
 * "synchronously" wait for results. Calls xmmsc_io_in_handle() to handle
 * the available data.
 */
static gboolean
priv_x2ipc_data_available_cb(GSource* src, GIOCondition cond, gpointer data)
{
	LOG_NOISE("called\n");
	
	gboolean ret = FALSE;
	
	RemPPPriv *priv = (RemPPPriv*) data;
	if (cond == G_IO_IN) {
    	ret = xmmsc_io_in_handle (priv->xc);
    } else {
		LOG_WARN("x2 disconnected while emulating sync waiting\n");
    		xmmsc_io_disconnect (priv->xc);
    }

    return ret;
}

/**
 * Here we create a new main context and attach a single source: an IO channel
 * for the X2 IPC socket. Then we can iterate this main context to wait for
 * a result. While iterating we can be sure, that only X2 IPC related events
 * occur. Iterating the _default_ main context could cause various side effects
 * since any (not only X2 IPC related) events may occur.
 * 
 * @see REMX2_RESULT_WAIT()
 * @see gcb_x2ipc_data_available()
 */
static void
priv_x2ipc_setup_mc_for_poll(RemPPPriv *priv)
{
	GIOChannel	*x2ipc_ioc;
	GSource		*x2ipc_src;
	GSourceFunc	gsf;
	
	gsf = (GSourceFunc) &priv_x2ipc_data_available_cb;
	
	priv->x2ipc_mc = g_main_context_new();
	
	x2ipc_ioc = g_io_channel_unix_new(xmmsc_io_fd_get(priv->xc));
	
	x2ipc_src = g_io_create_watch(x2ipc_ioc, G_IO_IN | G_IO_ERR | G_IO_HUP);
	
	g_source_set_callback(x2ipc_src, gsf, priv, NULL);
	
	g_source_attach(x2ipc_src, priv->x2ipc_mc);
	
	g_source_unref(x2ipc_src);
	
}

//LOG_NOISE("xmmsc_io_in_handle ..\n");						
//_io_ok = xmmsc_io_in_handle (priv->xc);						
//LOG_NOISE("done: xmmsc_io_in_handle ..\n");					

/**
 * Waits for a result and returns with '_ret' from the current function if
 * xmms2 has disconnectd while waiting.
 * 
 * Note: Variables 'result' and 'priv' must be declared/defined!
 */ 
#define REMX2_RESULT_WAIT(_ret) G_STMT_START {						\
	gboolean _ready = FALSE;										\
	xmmsc_result_notifier_set(result, xcb_result_ready, &_ready);	\
	while (!_ready && !priv->x2ipc_disconnected) {		\
		LOG_NOISE("iterate main context ..\n");			\
		g_main_context_iteration(NULL, TRUE);		\
		LOG_NOISE("done: iterate main context\n");		\
		/*g_usleep(500000);*/								\
	}																\
	if (priv->x2ipc_disconnected) {						\
		LOG_WARN("xmms2d disconnected while waiting for result\n");	\
		xmmsc_result_unref(result);									\
		return _ret;												\
	}																\
} G_STMT_END

///////////////////////////////////////////////////////////////////////////////
//
// misc utility functions
//
///////////////////////////////////////////////////////////////////////////////

/**
 * Catch interrupt signal (SIGINT).
 */
static void
priv_sigint(gint sig)
{
	LOG_DEBUG("received interrupt signal\n");
	
	rem_server_down(priv_global->rs);
}

/**
 * Do the final necessary steps after the playlist position has been changed by
 * a client.
 */
static void
priv_finish_plob_change(RemPPPriv *priv)
{
	xmmsc_result_t	*result;

	result = xmmsc_playback_tickle(priv->xc);
	REMX2_RESULT_DISCARD;

	result = xmmsc_playback_status(priv->xc);
	xmmsc_result_notifier_set(result, xcb_result_pbs, priv);
	xmmsc_result_unref(result);
}

/**
 * Iterates a X2 playlist result and appends the contained IDs as strings to
 * a RemStringList. This function does not clear the string list @a pl before
 * appending the PIDs!
 * 
 * @param[in]  result	playlist result - must be checked for errors already!
 * @param[out] pl		the string list to append the PIDs to
 */
static void
priv_playlist_result_to_sl(RemPPPriv *priv, xmmsc_result_t *result, RemStringList *pl)
{
	LOG_NOISE("called\n");
	
	guint			id;
	gint			ret;
	GString			*pid;

	pid = g_string_new_len("", 255);
	
	LOG_DEBUG("playlist: ");
	for (xmmsc_result_list_first(result);
		 xmmsc_result_list_valid(result);
		 xmmsc_result_list_next(result))
	{
		ret = xmmsc_result_get_uint(result, &id);
		g_assert(ret);
		
		#if LOGLEVEL >= LL_DEBUG
		LOG("%u ", id);
		#endif
		g_string_printf(pid, "%u", id);
		
		
		rem_sl_append_const(pl, pid->str);
	}
	#if LOGLEVEL >= LL_DEBUG
	LOG("\n");
	#endif
	
	g_string_free(pid, TRUE);
}

/**
 * Initially request some player status data from X2 (uses the X2 broadcast
 * callback functions for result handling - that is why these functions
 * must also be prepared for error results).
 */
static void
priv_initially_request_player_status_data(RemPPPriv *priv)
{
	LOG_NOISE("called\n");
	
	xmmsc_result_t	*result;
	
	////////// playback state //////////
	
	result = xmmsc_playback_status(priv->xc);
	xmmsc_result_notifier_set(result, xcb_state_changed, priv);
	xmmsc_result_unref(result);
	
	////////// volume //////////
	
	result = xmmsc_playback_volume_get(priv->xc);
	xmmsc_result_notifier_set(result, xcb_volume_changed, priv);
	xmmsc_result_unref(result);

	////////// cap //////////
	
	result = xmmsc_playback_current_id(priv->xc);
	xmmsc_result_notifier_set(result, xcb_cap_id_changed, priv);
	xmmsc_result_unref(result);
	
	////////// cap position //////////
	
	result = xmmsc_playlist_current_pos(priv->xc, XMMS_ACTIVE_PLAYLIST);
	xmmsc_result_notifier_set(result, xcb_cap_pos_changed, priv);
	xmmsc_result_unref(result);
	
	////////// playlist //////////
	
	result = xmmsc_playlist_list_entries(priv->xc, XMMS_ACTIVE_PLAYLIST);
	xmmsc_result_notifier_set(result, xcb_playlist_changed_or_loaded, priv);
	xmmsc_result_unref(result);
}

/** Connect to XMMS2 */
static void
priv_connect_to_xmms2(RemPPPriv *priv)
{
	LOG_NOISE("called\n");
	
	priv->xc = xmmsc_init("remuco");
	
	g_assert(priv->xc);

	if (xmmsc_connect(priv->xc, g_getenv("XMMS_PATH"))) {
		LOG_INFO("xmms2d is running\n");
		xmmsc_disconnect_callback_set(priv->xc, &xcb_disconnect, priv);
	} else {
		LOG_ERROR("%s\n", xmmsc_get_last_error(priv->xc));
		xmmsc_unref(priv->xc);
		priv->xc = NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////
//
// remuco callback functions
//
///////////////////////////////////////////////////////////////////////////////

static void
rcb_synchronize(RemPPPriv *priv, RemPlayerStatus *ps)
{
	xmmsc_result_t	*result;
	
	if (priv->ps_status_changed) {
		
		priv->ps_status_changed = FALSE;

		LOG_DEBUG("snychronize simple values\n");
		
		ps->cap_pos = priv->ps_cap_pos;
		ps->repeat = priv->ps_repeat;
		ps->shuffle = priv->ps_shuffle;
		ps->pbs = priv->ps_pbs;
		ps->volume = priv->ps_volume;
	}
	
	if (priv->ps_cap_id_changed) {
		
		priv->ps_cap_id_changed = FALSE;

		LOG_DEBUG("snychronize cap pid\n");
		
		if (priv->ps_cap_id == 0)
			g_string_truncate(ps->cap_pid, 0);
		else
			g_string_printf(ps->cap_pid, "%u", priv->ps_cap_id);
	}
	
	if (priv->playlist_result) {

		result = priv->playlist_result;
		priv->playlist_result = NULL;

		LOG_DEBUG("snychronize playlist\n");
		
		rem_sl_clear(ps->playlist);
		priv_playlist_result_to_sl(priv, result, ps->playlist);
		xmmsc_result_unref(result);
	}
}

static RemLibrary*
rcb_get_library(RemPPPriv *priv)
{
	RemLibrary		*lib;
	xmmsc_result_t	*result;
	gchar			*plid;
	gint			ret;
	
	lib = rem_library_new();
	
	result = xmmsc_playlist_list(priv->xc);

	REMX2_RESULT_WAIT(lib);
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		xmmsc_result_unref(result);
		return lib;
	}	

	for (xmmsc_result_list_first(result);
		 xmmsc_result_list_valid(result);
		 xmmsc_result_list_next(result))
	{
		ret = xmmsc_result_get_string(result, &plid);
		g_assert(ret);
		
		rem_library_append_const(
				lib, plid, plid, REM_PLOBLIST_FLAG_STATIC);
	}
	
	xmmsc_result_unref(result);
	
	return lib;
}

static RemPlob*
rcb_get_plob(RemPPPriv *priv, const gchar *pid)
{
	guint			u, id;
	gint			ret, val_i;
	gchar			*val_s;
	GString			*val_gs;
	RemPlob			*plob;
	xmmsc_result_t	*result;

	val_gs = g_string_new("123456");
	
	id = (guint) g_ascii_strtoull(pid, NULL, 10);
	
	g_assert(id);
	
	LOG_DEBUG("read song %u from mlib\n", id);
	
	result = xmmsc_medialib_get_info(priv->xc, id);

	REMX2_RESULT_WAIT(FALSE);

	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		xmmsc_result_unref(result);
		return NULL;
	}
	
	plob = rem_plob_new(pid);

	for (u = 0; u < XMETA_COUNT; u++) {
		
		switch (XMETA_TYPES[u]) {
		
			case XMMSC_RESULT_VALUE_TYPE_STRING:
				
				ret = xmmsc_result_get_dict_entry_string(
									result, XMETA_NAMES[u], &val_s);
				
				if (!ret || !val_s) break;
				
				rem_plob_meta_add_const(plob, RMETA_NAMES[u], val_s);
				
				break;
				
			case XMMSC_RESULT_VALUE_TYPE_INT32:
				
				ret = xmmsc_result_get_dict_entry_int(
									result, XMETA_NAMES[u], &val_i);
				
				if (!ret) break;
				
				if (u == XMETA_NUM_LENGTH) {
					val_i = val_i / 1000;
				}
				
				g_string_printf(val_gs, "%i", val_i);
					
				rem_plob_meta_add_const(plob, RMETA_NAMES[u], val_gs->str);

				break;
				
			default:
				g_assert_not_reached();
				break;
		}
	}
	
	xmmsc_result_unref(result);
	
	g_string_free(val_gs, TRUE);
	
	return plob;	
}

static RemStringList*
rcb_get_ploblist(RemPPPriv *priv, const gchar *plid)
{
	RemStringList	*pl;
	xmmsc_result_t	*result;
	
	result = xmmsc_playlist_list_entries(priv->xc, plid);

	pl = rem_sl_new();
	
	REMX2_RESULT_WAIT(pl);
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		xmmsc_result_unref(result);
		return NULL;
	}	
	
	priv_playlist_result_to_sl(priv, result, pl);
	
	xmmsc_result_unref(result);
	
	return pl;
}

static void
rcb_notify(RemPPPriv *priv, RemServerEvent event)
{
	switch (event) {
		case REM_SERVER_EVENT_ERROR:
			LOG_ERROR("server experienced serious error -> shut down server\n");
			rem_server_down(priv->rs);
			break;
		case REM_SERVER_EVENT_DOWN:
			LOG_DEBUG("server shut down finished\n");
			g_main_loop_quit(priv->ml);
			break;
		default:
			g_assert_not_reached();
			break;
	}
}

static void
rcb_play_ploblist(RemPPPriv *priv, const gchar *plid)
{
	xmmsc_result_t	*result;

	result = xmmsc_playlist_load(priv->xc, plid);
	REMX2_RESULT_DISCARD;
}

// FUTUTE FEATURE
//static RemStringList*
//rcb_search(RemPPPriv *priv, const RemPlob *plob);

static void
rcb_simple_control(RemPPPriv *priv, RemSimpleControlCommand cmd, gint param)
{
	xmmsc_result_t	*result;

	LOG_DEBUG("command: %hu, param: %hu\n", cmd, param);

	switch (cmd) {
		case REM_SCTRL_CMD_JUMP:
			
			result = xmmsc_playlist_set_next(priv->xc, param - 1);
			REMX2_RESULT_DISCARD;
			
			priv_finish_plob_change(priv);
			
			break;
			
		case REM_SCTRL_CMD_NEXT:
			result = xmmsc_playlist_set_next_rel(priv->xc, 1);
			REMX2_RESULT_DISCARD;
			
			priv_finish_plob_change(priv);
			
			break;
			
		case REM_SCTRL_CMD_PREV:
			
			result = xmmsc_playlist_set_next_rel(priv->xc, -1);
			REMX2_RESULT_DISCARD;
			
			priv_finish_plob_change(priv);
			
			break;
			
		case REM_SCTRL_CMD_PLAYPAUSE:
			
			if (priv->ps_pbs == REM_PBS_PLAY) {
				result = xmmsc_playback_pause(priv->xc);
				REMX2_RESULT_DISCARD;
			} else {
				result = xmmsc_playback_start(priv->xc);
				REMX2_RESULT_DISCARD;
			}
			
			break;
			
		case REM_SCTRL_CMD_STOP:
			
			result = xmmsc_playback_stop(priv->xc);
			REMX2_RESULT_DISCARD;
			
			break;
			
		case REM_SCTRL_CMD_RESTART:
			
			result = xmmsc_playback_stop(priv->xc);
			REMX2_RESULT_DISCARD;
			
			result = xmmsc_playlist_set_next(priv->xc, 1);
			REMX2_RESULT_DISCARD;
			
			result = xmmsc_playback_start(priv->xc);
			REMX2_RESULT_DISCARD;
			
			break;
			
		case REM_SCTRL_CMD_VOLUME:
			
			result = xmmsc_playback_volume_set(priv->xc, "left", param);
			REMX2_RESULT_DISCARD;
			
			result = xmmsc_playback_volume_set(priv->xc, "right", param);
			REMX2_RESULT_DISCARD;
			
			break;
			
		case REM_SCTRL_CMD_RATE:
			
			if (!priv->ps_cap_id > 0) break; // no currently active plob
			
			result = xmmsc_medialib_entry_property_set_int(
						priv->xc, priv->ps_cap_id, XMETA_NAME_RATING, param);
			REMX2_RESULT_DISCARD;

			break;
			
		default:
			LOG_WARN("ignore command %hu\n", cmd);
			break;
	}
	
}

// FUTUTE FEATURE
//static void
//rcb_update_plob(RemPPPriv *pp_priv, const RemPlob *plob);

// FUTUTE FEATURE
//static void
//rcb_update_ploblist(RemPPPriv *pp_priv,
//					const gchar *plid,
//					const RemStringList* pids);

///////////////////////////////////////////////////////////////////////////////
//
// xmms2 callback functions (misc)
//
///////////////////////////////////////////////////////////////////////////////

/**
 * Called when a result is ready and either REMX2_RESULT_WAIT or
 * REMX2_RESULT_DISCARD has been called on that result before.
 * 
 * @param data If <code>NULL</code>, this function does nothing except printing
 *             out an error if the result has one. If not <code>NULL</code>,
 *             this must point to a @p gboolean - the gboolean will be set to
 *             @p TRUE to indicate that the result is ready.
 */
static void
xcb_result_ready(xmmsc_result_t *result, gpointer data)
{
	LOG_NOISE("called\n");
	
	if (!data) {	// we are not interested in the result (has been discarded)
		
		if (xmmsc_result_iserror(result))
			LOG_WARN("%s\n", xmmsc_result_get_error(result));
		
	} else {		// somewhere we wait for the result
	
		*((gboolean*) data) = TRUE;
	}
}

/**
 * We previously requested the current playback status to decide if we must
 * start playback because a client changed the playlist position.
 * Here is the result (XMMS2 playback status) ..
 */
static void
xcb_result_pbs(xmmsc_result_t *result, gpointer data)
{
	xmmsc_result_t	*result2;
	RemPPPriv		*priv = (RemPPPriv*) data;
	guint			u;
	gint			ret;
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}
	
	ret = xmmsc_result_get_uint(result, &u);
	g_assert(ret);
	if (u != XMMS_PLAYBACK_STATUS_PLAY) {
		result2 = xmmsc_playback_start(priv->xc);
		REMX2_RESULT_DISCARD;
	}
	
}

static void
xcb_disconnect(gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;

	LOG_DEBUG("XMMS2 wants us to disconnect\n");
	
	priv->x2ipc_disconnected = TRUE;
	
	rem_server_down(priv->rs);
}

///////////////////////////////////////////////////////////////////////////////
//
// xmms2 callback functions (broadcasts)
//
///////////////////////////////////////////////////////////////////////////////

static void
xcb_state_changed(xmmsc_result_t *result, gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;
	guint		st;
	gint		ret;
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}

	ret = xmmsc_result_get_uint(result, &st);
	g_assert(ret);

	LOG_DEBUG("new (xmms2) pbs is %u\n", st);

	switch (st) {
		case XMMS_PLAYBACK_STATUS_PAUSE:
			priv->ps_pbs = REM_PBS_PAUSE;
			break;
		case XMMS_PLAYBACK_STATUS_PLAY:
			priv->ps_pbs = REM_PBS_PLAY;
			break;
		case XMMS_PLAYBACK_STATUS_STOP:
			priv->ps_pbs = REM_PBS_STOP;
			break;
		default:
			LOG_BUG("unknown xmms2 playback status\n");
			break;
	}

	priv->ps_status_changed = TRUE;
	
	rem_server_notify(priv->rs);
}

static void
xcb_volume_changed(xmmsc_result_t *result, gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;
	guint		l, r;
	gint		ret;
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}

	l = 50; r = 50;
	
	ret = xmmsc_result_get_dict_entry_uint(result, "left", &l);
	ret &= xmmsc_result_get_dict_entry_uint(result, "right", &r);
	g_assert(ret);
	
	priv->ps_volume = l < r ? r : l;

	LOG_DEBUG("new volume is %u:%u\n", l, r);

	priv->ps_status_changed = TRUE;

	rem_server_notify(priv->rs);
}

static void
xcb_cap_id_changed(xmmsc_result_t *result, gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;
	gint		ret;
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}

	ret = xmmsc_result_get_uint(result, &priv->ps_cap_id);
	g_assert(ret);
	
	LOG_DEBUG("new cap id is %u\n", priv->ps_cap_id);
	
	priv->ps_cap_id_changed = TRUE;
	
	rem_server_notify(priv->rs);
}

static void
xcb_cap_pos_changed(xmmsc_result_t *result, gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;
	guint		pos;
	gint		ret;
	
	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}

	ret = xmmsc_result_get_uint(result, &pos);
	g_assert(ret);
	
	// TODO the cap position here is allways a song in a playlist, also if
	// the current active song is not part of the current playlist
	LOG_DEBUG("new cap_pos is %u\n", pos + 1);

	priv->ps_cap_pos = (gint) pos + 1;
	
	priv->ps_status_changed = TRUE;

	rem_server_notify(priv->rs);
}

static void
xcb_playlist_changed_or_loaded(xmmsc_result_t *result, gpointer data)
{
	RemPPPriv	*priv = (RemPPPriv*) data;

	if (xmmsc_result_iserror(result)) {
		LOG_WARN("%s\n", xmmsc_result_get_error(result));
		return;
	}
	
	if (priv->playlist_result) // forget the previous 'new' playlist
		xmmsc_result_unref(priv->playlist_result);
	
	priv->playlist_result = result;
	xmmsc_result_ref(priv->playlist_result);
	
	LOG_DEBUG("playlist changed or loaded\n");

	rem_server_notify(priv->rs);
}

///////////////////////////////////////////////////////////////////////////////
//
// main
//
///////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {
	
	RemPPPriv		*priv;
	RemPPCallbacks	*rcb;
	RemPPDescriptor	*ppd;
	GError			*err;
	
    ////// signal handler //////

    if (signal(SIGINT, priv_sigint) == SIG_ERR) {
            LOG_ERRNO("install sighandler for SIGINT failed");
            return 1;
    }

    ////////// init private data //////////
    
    priv = g_new0(RemPPPriv, 1);
	priv_global = priv;
	
	priv_connect_to_xmms2(priv);
	
	if (!priv->xc) return 1;
	
	////////// set callbacks for player changes //////////

	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playback_current_id,
					  &xcb_cap_id_changed, priv);
	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playback_status,
					  &xcb_state_changed, priv);
	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playback_volume_changed,
					  &xcb_volume_changed, priv);
	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playlist_current_pos,
					  &xcb_cap_pos_changed, priv);
	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playlist_changed,
					  &xcb_playlist_changed_or_loaded, priv);
	XMMS_CALLBACK_SET(priv->xc, xmmsc_broadcast_playlist_loaded,
					  &xcb_playlist_changed_or_loaded, priv);
	
	////////// set callbacks for remuco server //////////

	rcb = g_new(RemPPCallbacks, 1);
	
	rcb->get_library = &rcb_get_library;
	rcb->get_plob = &rcb_get_plob;
	rcb->get_ploblist = &rcb_get_ploblist;
	rcb->notify = &rcb_notify;
	rcb->play_ploblist = &rcb_play_ploblist;
	rcb->search = NULL; // FUTURE FEATURE &rcb_search;
	rcb->simple_ctrl = &rcb_simple_control;
	rcb->synchronize = &rcb_synchronize;
	rcb->update_plob = NULL; // FUTURE FEATURE &rcb_update_plob;
	rcb->update_ploblist = NULL; // FUTURE FEATURE &rcb_update_ploblist;

	////////// set up pp descriptor //////////

	ppd = g_new(RemPPDescriptor, 1);

	ppd->charset = NULL;
	ppd->max_rating_value = 5;
	ppd->notifies_changes = TRUE;
	ppd->player_name = g_strdup("XMMS2");
	ppd->supported_repeat_modes = 0;
	ppd->supported_shuffle_modes = 0;
	ppd->supports_playlist = TRUE;
	ppd->supports_playlist_jump = TRUE;
	ppd->supports_queue = FALSE;
	ppd->supports_queue_jump = FALSE;
	ppd->supports_seek = FALSE; // TODO TRUE;
	ppd->supports_tags = FALSE; // TODO TRUE;
	
	////////// start remuco server //////////

	err = NULL;
	priv->rs = rem_server_up(ppd, rcb, priv, &err);
	
	if (err) {
		LOG_ERROR("starting server failed: %s\n", err->message);
		g_error_free(err);
	}
	
	if (!priv->rs) {
		g_free(priv);
		return 1;
	}
	
	////////// create our x2ipc main context //////////
	
	priv_x2ipc_setup_mc_for_poll(priv);
	
	////////// initially get status of player //////////
	
	priv_initially_request_player_status_data(priv);
	
	////////// set up and run main loop (for the default context) //////////
	
	priv->ml = g_main_loop_new(NULL, FALSE);

	xmmsc_mainloop_gmain_init(priv->xc);

	g_usleep(500000);
	
	LOG_DEBUG("now running main loop\n");
	g_main_loop_run(priv->ml);
	LOG_DEBUG("back from main loop\n");

	////////// shut down //////////

	// rem_server_shutdown() already has been called in priv_sigint() or
	// xcb_disconnect(). So when we are here, then because rcb_notify() has been
	// called by the server.
	
	g_main_context_unref(priv->x2ipc_mc);
	
	xmmsc_unref(priv->xc);
	
	g_main_loop_unref(priv->ml);
	
	g_free(priv);
	
	LOG_INFO("bye..\n");
	
	return 0;
}
