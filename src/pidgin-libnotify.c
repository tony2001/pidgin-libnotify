/*
 * Pidgin-libnotify - Provides a libnotify interface for Pidgin
 * Copyright (C) 2005-2007 Duarte Henriques
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gln_intl.h"

#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#include <pidgin.h>
#include <version.h>
#include <debug.h>
#include <util.h>
#include <privacy.h>

/* for pidgin_create_prpl_icon */
#include <gtkutils.h>

#include <libnotify/notify.h>

#include <string.h>

#define PLUGIN_ID "pidgin-libnotify"

#define DELIMS " "
static char **words;

static GHashTable *buddy_hash;

static PurplePluginPrefFrame *
get_plugin_pref_frame (PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *ppref;

	frame = purple_plugin_pref_frame_new ();

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/newmsg",
                            _("New messages"));
	purple_plugin_pref_frame_add (frame, ppref);
	
	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/newmsgtxt",
                            _("Show new messages text"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/triggerMessages",
                            _("Show messages with these trigger words"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/othermsgs",
                            _("Show all messages in chat rooms"));
	purple_plugin_pref_frame_add (frame, ppref);

	if (purple_prefs_get_int("/plugins/gtk/libnotify/timeout") == 0) {
		/* 3 seconds is the default timeout */
		purple_prefs_set_int("/plugins/gtk/libnotify/timeout", 3000);
	}

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/timeout",
                            _("Display timeout (msec)"));
	purple_plugin_pref_set_bounds(ppref, 100, 100000);
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/blocked",
                            _("Ignore events from blocked users"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/signon",
                            _("Buddy signs on"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/signoff",
                            _("Buddy signs off"));
	purple_plugin_pref_frame_add (frame, ppref);

	ppref = purple_plugin_pref_new_with_name_and_label (
                            "/plugins/gtk/libnotify/only_available",
                            _("Only when available"));
	purple_plugin_pref_frame_add (frame, ppref);

	return frame;
}

/* Signon flood be gone! - thanks to the guifications devs */
static GList *just_signed_on_accounts = NULL;

static gboolean
event_connection_throttle_cb (gpointer data)
{
	PurpleAccount *account;

	account = (PurpleAccount *)data;

	if (!account)
		return FALSE;

	if (!purple_account_get_connection (account)) {
		just_signed_on_accounts = g_list_remove (just_signed_on_accounts, account);
		return FALSE;
	}

	if (!purple_account_is_connected (account))
		return TRUE;

	just_signed_on_accounts = g_list_remove (just_signed_on_accounts, account);
	return FALSE;
}

static void
event_connection_throttle (PurpleConnection *conn, gpointer data)
{
	PurpleAccount *account;

	/* TODO: this function gets called after buddy signs on for GTalk
	   users who have themselves as a buddy */
	purple_debug_info (PLUGIN_ID, "event_connection_throttle() called\n");

	if (!conn)
		return;

	account = purple_connection_get_account(conn);
	if (!account)
		return;

	just_signed_on_accounts = g_list_prepend (just_signed_on_accounts, account);
	g_timeout_add (5000, event_connection_throttle_cb, (gpointer)account);
}

/* do NOT g_free() the string returned by this function */
static gchar *
best_name (PurpleBuddy *buddy)
{
	if (buddy->alias) {
		return buddy->alias;
	} else if (buddy->server_alias) {
		return buddy->server_alias;
	} else {
		return buddy->name;
	}
}

static GdkPixbuf *
pixbuf_from_buddy_icon (PurpleBuddyIcon *buddy_icon)
{
	GdkPixbuf *icon;
	const guchar *data;
	size_t len;
	GdkPixbufLoader *loader;

	data = purple_buddy_icon_get_data (buddy_icon, &len);

	loader = gdk_pixbuf_loader_new ();
	gdk_pixbuf_loader_set_size (loader, 48, 48);
	gdk_pixbuf_loader_write (loader, data, len, NULL);
	gdk_pixbuf_loader_close (loader, NULL);

	icon = gdk_pixbuf_loader_get_pixbuf (loader);

	if (icon) {
		g_object_ref (icon);
	}

	g_object_unref (loader);

	return icon;
}

static void
notify_deleting_conversation_cb (PurpleConversation *conv,
				 gpointer data)
{
    NotifyNotification *notification;

    notification = g_hash_table_lookup (buddy_hash, conv);
    if (notification != NULL)
	notify_notification_close (notification, NULL);
}

static void
action_cb (NotifyNotification *notification,
		   gchar *action, gpointer user_data)
{
	PurpleBuddy *buddy = NULL;
	PurpleConversation *conv = NULL;

	purple_debug_info (PLUGIN_ID, "action_cb(), "
					"notification: 0x%lx, action: '%s'", (unsigned long)notification, action);

	buddy = (PurpleBuddy *)g_object_get_data (G_OBJECT(notification), "buddy");
	conv = (PurpleConversation *)g_object_get_data (G_OBJECT(notification), "conv");

	if (buddy) {
		conv = purple_find_conversation_with_account (PURPLE_CONV_TYPE_ANY, buddy->name, buddy->account);

		if (!conv) {
			conv = purple_conversation_new (PURPLE_CONV_TYPE_IM,
										  buddy->account,
										  buddy->name);
		}
	} else if (!conv) {
		purple_debug_warning (PLUGIN_ID, "Got no buddy nor conv!");
		return;
	}

	conv->ui_ops->present (conv);

	notify_notification_close (notification, NULL);
}

static gboolean
closed_cb (NotifyNotification *notification)
{
	PurpleContact *contact;
	PurpleConversation *conv = NULL;

	purple_debug_info (PLUGIN_ID, "closed_cb(), notification: 0x%lx\n", (unsigned long)notification);

	contact = (PurpleContact *)g_object_get_data (G_OBJECT(notification), "contact");
	conv = (PurpleConversation *)g_object_get_data (G_OBJECT(notification), "conv");
	if (contact)
		g_hash_table_remove (buddy_hash, contact);
	else if (conv)
		g_hash_table_remove (buddy_hash, conv);

	g_object_unref (G_OBJECT(notification));

	return FALSE;
}

/* you must g_free the returned string
 * num_chars is utf-8 characters */
static gchar *
truncate_escape_string (const gchar *str,
						int num_chars)
{
	gchar *escaped_str;

	if (g_utf8_strlen (str, num_chars*2+1) > num_chars) {
		gchar *truncated_str;
		gchar *str2;

		/* allocate number of bytes and not number of utf-8 chars */
		str2 = g_malloc ((num_chars-1) * 2 * sizeof(gchar));

		g_utf8_strncpy (str2, str, num_chars-2);
		truncated_str = g_strdup_printf ("%s..", str2);
		escaped_str = g_markup_escape_text (truncated_str, strlen (truncated_str));
		g_free (str2);
		g_free (truncated_str);
	} else {
		escaped_str = g_markup_escape_text (str, strlen (str));
	}

	return escaped_str;
}

static gboolean
should_notify_unavailable (PurpleAccount *account)
{
	PurpleStatus *status;

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/only_available"))
		return TRUE;

	status = purple_account_get_active_status (account);

	return purple_status_is_online (status) && purple_status_is_available (status);
}

static void
notify (const gchar *title,
		const gchar *body,
		PurpleBuddy *buddy,
		PurpleConversation *conv)
{
	NotifyNotification *notification = NULL;
	GdkPixbuf *icon;
	PurpleBuddyIcon *buddy_icon;
	gchar *tr_body;
	PurpleContact *contact;

	if (buddy)
		contact = purple_buddy_get_contact (buddy);
	else
		contact = NULL;

	if (body)
		tr_body = truncate_escape_string (body, 60);
	else
		tr_body = NULL;

	if (!conv && buddy)
		conv = purple_find_conversation_with_account (PURPLE_CONV_TYPE_ANY, buddy->name, buddy->account);

	if (conv && conv->ui_ops && conv->ui_ops->has_focus) {
#ifndef DEBUG /* in debug mode, always show notifications */
		if (conv->ui_ops->has_focus(conv) == TRUE) {
			purple_debug_info (PLUGIN_ID, "Conversation has focus 0x%lx\n", (unsigned long)conv);
			return;
		}
#endif
	}

	if (contact)
		notification = g_hash_table_lookup (buddy_hash, contact);
	else if (conv)
		notification = g_hash_table_lookup (buddy_hash, conv);
	else
		notification = NULL;

	if (notification != NULL) {
		notify_notification_update (notification, title, tr_body, NULL);
		notify_notification_set_timeout(notification, purple_prefs_get_int("/plugins/gtk/libnotify/timeout"));
		/* this shouldn't be necessary, file a bug */
		notify_notification_show (notification, NULL);

		purple_debug_info (PLUGIN_ID, "notify(), update: "
						 "title: '%s', body: '%s', buddy: '%s'\n",
						 title, tr_body, buddy ? best_name (buddy) : "");

		g_free (tr_body);
		return;
	}
#ifdef LIBNOTIFY_07
	notification = notify_notification_new (title, tr_body, NULL);
#else
	notification = notify_notification_new (title, tr_body, NULL, NULL);
#endif
	purple_debug_info (PLUGIN_ID, "notify(), new: "
					 "title: '%s', body: '%s', buddy: '%s'\n",
					 title, tr_body, buddy ? best_name (buddy) : "");

	g_free (tr_body);

	if (buddy)
		buddy_icon = purple_buddy_get_icon (buddy);
	else
		buddy_icon = NULL;

	if (buddy_icon) {
		icon = pixbuf_from_buddy_icon (buddy_icon);
		purple_debug_info (PLUGIN_ID, "notify(), has a buddy icon.\n");
	} else if (buddy) {
		icon = pidgin_create_prpl_icon (buddy->account, 1);
		purple_debug_info (PLUGIN_ID, "notify(), has a prpl icon.\n");
	} else if (conv) {
		icon = pidgin_create_prpl_icon (conv->account, 1);
		purple_debug_info (PLUGIN_ID, "notify(), has a prpl icon.\n");
	} else {
		icon = NULL;
		purple_debug_info (PLUGIN_ID, "notify(), has no icon.\n");
	}

	if (icon) {
		notify_notification_set_icon_from_pixbuf (notification, icon);
		g_object_unref (icon);
	} else {
		purple_debug_warning (PLUGIN_ID, "notify(), couldn't find any icon!\n");
	}

	if (contact)
		g_hash_table_insert (buddy_hash, contact, notification);
	else if (conv)
		g_hash_table_insert (buddy_hash, conv, notification);

	g_object_set_data (G_OBJECT(notification), "contact", contact);
	g_object_set_data (G_OBJECT(notification), "conv", conv);
	g_object_set_data (G_OBJECT(notification), "buddy", buddy);

	g_signal_connect (notification, "closed", G_CALLBACK(closed_cb), NULL);

	notify_notification_set_urgency (notification, NOTIFY_URGENCY_NORMAL);

	notify_notification_add_action (notification, "show", _("Show"), action_cb, NULL, NULL);

	notify_notification_set_timeout(notification, purple_prefs_get_int("/plugins/gtk/libnotify/timeout"));
	if (!notify_notification_show (notification, NULL)) {
		purple_debug_error (PLUGIN_ID, "notify(), failed to send notification\n");
	}

}

static void
notify_buddy_signon_cb (PurpleBuddy *buddy,
						gpointer data)
{
	gchar *tr_name, *title;
	gboolean blocked;

	g_return_if_fail (buddy);

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/signon"))
		return;

	if (g_list_find (just_signed_on_accounts, buddy->account))
		return;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (!purple_privacy_check (buddy->account, buddy->name) && blocked)
		return;

	if (!should_notify_unavailable (purple_buddy_get_account (buddy)))
		return;

	tr_name = truncate_escape_string (best_name (buddy), 25);

	title = g_strdup_printf (_("%s signed on"), tr_name);

	notify (title, NULL, buddy, NULL);

	g_free (tr_name);
	g_free (title);
}

static void
notify_buddy_signoff_cb (PurpleBuddy *buddy,
						 gpointer data)
{
	gchar *tr_name, *title;
	gboolean blocked;

	g_return_if_fail (buddy);

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/signoff"))
		return;

	if (g_list_find (just_signed_on_accounts, buddy->account))
		return;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (!purple_privacy_check (buddy->account, buddy->name) && blocked)
		return;

	if (!should_notify_unavailable (purple_buddy_get_account (buddy)))
		return;

	tr_name = truncate_escape_string (best_name (buddy), 25);

	title = g_strdup_printf (_("%s signed off"), tr_name);

	notify (title, NULL, buddy, NULL);

	g_free (tr_name);
	g_free (title);
}

static void
notify_msg_sent (PurpleAccount *account,
				 PurpleConversation *conv,
				 const gchar *sender,
				 const gchar *message)
{
	PurpleBuddy *buddy;
	gchar *title, *body, *tr_name;
	gboolean blocked;

	blocked = purple_prefs_get_bool ("/plugins/gtk/libnotify/blocked");
	if (blocked && !purple_privacy_check(account, sender))
		return;

	buddy = purple_find_buddy (account, sender);
	if (buddy)
		tr_name = truncate_escape_string (best_name (buddy), 25);
	else if (conv) {
		char *name = g_strdup_printf (_("%s (%s)"), sender, purple_conversation_get_name (conv));
		tr_name = truncate_escape_string (name, 25);
		g_free (name);
	} else
		tr_name = truncate_escape_string (sender, 25);

	if (purple_prefs_get_bool("/plugins/gtk/libnotify/newmsgtxt")) {
		title = g_strdup_printf (_("%s says:"), tr_name);
		body = purple_markup_strip_html (message);

		notify (title, body, buddy, conv);

		g_free (title);
	} else {
		title = _("new message received");
		body = g_strdup_printf (_("from %s"), tr_name);

		notify (title, body, buddy, conv);
	}
	g_free (tr_name);
	g_free (body);
}

static void
notify_new_message_cb (PurpleAccount *account,
					   const gchar *sender,
					   const gchar *message,
					   int flags,
					   gpointer data)
{
	PurpleConversation *conv;

	if (!purple_prefs_get_bool ("/plugins/gtk/libnotify/newmsg"))
		return;

	conv = purple_find_conversation_with_account (PURPLE_CONV_TYPE_IM, sender, account);

#ifndef DEBUG /* in debug mode, always show notifications */
	if (conv && purple_conversation_has_focus (conv)) {
		purple_debug_info (PLUGIN_ID, "Conversation has focus 0x%lx\n", (unsigned long)conv);
		return;
	}
#endif

	if (conv && purple_prefs_get_bool ("/plugins/gtk/libnotify/newconvonly")) {
		purple_debug_info (PLUGIN_ID, "Conversation is not new 0x%lx\n", (unsigned long)conv);
		return;
	}

	if (!should_notify_unavailable (account))
		return;

	notify_msg_sent (account, conv, sender, message);
}

static void
construct_list()
{
	g_strfreev(words);
	words = g_strsplit_set(purple_prefs_get_string("/plugins/gtk/libnotify/triggerMessages"), DELIMS, -1);
}

static void
notify_chat_nick (PurpleAccount *account,
				  const gchar *sender,
				  const gchar *message,
				  PurpleConversation *conv,
				  gpointer data)
{
	gchar *nick;

	nick = (gchar *)purple_conv_chat_get_nick (PURPLE_CONV_CHAT(conv));
#ifndef DEBUG /* in debug mode, always show notifications */
	if (nick && !strcmp (sender, nick))
		return;
#else
	purple_debug_info (PLUGIN_ID, "Message would be suppressed normally because you said it yourself");
#endif

	int triggermessagefound = 0;
	int i;
	for (i = 0; words[i]; i++  ) {
		if (g_strstr_len(message, strlen(message), words[i])) {
			triggermessagefound=1;
			purple_debug_info (PLUGIN_ID, "found triggermessage in chat");
			break;
		}
	}
	if (!triggermessagefound){
		purple_debug_info (PLUGIN_ID, "no triggermessage found in chat");
	}

	if (!triggermessagefound && !g_strstr_len(message, strlen(message), nick) && !purple_prefs_get_bool("/plugins/gtk/libnotify/othermsgs"))
		return;

	notify_msg_sent (account, conv, sender, message);
}

static gboolean
plugin_load (PurplePlugin *plugin)
{
	void *conv_handle, *blist_handle, *conn_handle;

	construct_list();

	if (!notify_is_initted () && !notify_init ("Pidgin")) {
		purple_debug_error (PLUGIN_ID, "libnotify not running!\n");
		return FALSE;
	}

	conv_handle = purple_conversations_get_handle ();
	blist_handle = purple_blist_get_handle ();
	conn_handle = purple_connections_get_handle();

	buddy_hash = g_hash_table_new (NULL, NULL);

	purple_signal_connect (blist_handle, "buddy-signed-on", plugin,
						PURPLE_CALLBACK(notify_buddy_signon_cb), NULL);

	purple_signal_connect (blist_handle, "buddy-signed-off", plugin,
						PURPLE_CALLBACK(notify_buddy_signoff_cb), NULL);

	purple_signal_connect (conv_handle, "received-im-msg", plugin,
						PURPLE_CALLBACK(notify_new_message_cb), NULL);

	purple_signal_connect (conv_handle, "received-chat-msg", plugin,
						PURPLE_CALLBACK(notify_chat_nick), NULL);

	purple_signal_connect (conv_handle, "deleting-conversation", plugin,
						PURPLE_CALLBACK(notify_deleting_conversation_cb), NULL);

	purple_prefs_connect_callback(plugin, "/plugins/gtk/libnotify/triggerMessages",
					(PurplePrefCallback)construct_list, NULL);

	/* used just to not display the flood of guifications we'd get */
	purple_signal_connect (conn_handle, "signed-on", plugin,
						PURPLE_CALLBACK(event_connection_throttle), NULL);

	return TRUE;
}

static gboolean
plugin_unload (PurplePlugin *plugin)
{
	void *conv_handle, *blist_handle, *conn_handle;

	conv_handle = purple_conversations_get_handle ();
	blist_handle = purple_blist_get_handle ();
	conn_handle = purple_connections_get_handle();

	purple_signal_disconnect (blist_handle, "buddy-signed-on", plugin,
							PURPLE_CALLBACK(notify_buddy_signon_cb));

	purple_signal_disconnect (blist_handle, "buddy-signed-off", plugin,
							PURPLE_CALLBACK(notify_buddy_signoff_cb));

	purple_signal_disconnect (conv_handle, "received-im-msg", plugin,
							PURPLE_CALLBACK(notify_new_message_cb));

	purple_signal_disconnect (conv_handle, "received-chat-msg", plugin,
							PURPLE_CALLBACK(notify_chat_nick));

	purple_signal_disconnect (conv_handle, "deleting-conversation", plugin,
							PURPLE_CALLBACK(notify_deleting_conversation_cb));


	purple_signal_disconnect (conn_handle, "signed-on", plugin,
							PURPLE_CALLBACK(event_connection_throttle));

	g_hash_table_destroy (buddy_hash);

	notify_uninit ();

	return TRUE;
}

static PurplePluginUiInfo prefs_info = {
    get_plugin_pref_frame,
    0,						/* page num (Reserved) */
    NULL					/* frame (Reserved) */
};

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,										/* api version */
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,									/* type */
    0,														/* ui requirement */
    0,														/* flags */
    NULL,													/* dependencies */
    PURPLE_PRIORITY_DEFAULT,									/* priority */
    
    PLUGIN_ID,												/* id */
    NULL,													/* name */
    VERSION,												/* version */
    NULL,													/* summary */
    NULL,													/* description */
    
    "Duarte Henriques <duarte.henriques@gmail.com>",		/* author */
    "http://sourceforge.net/projects/gaim-libnotify/",		/* homepage */
    
    plugin_load,			/* load */
    plugin_unload,			/* unload */
    NULL,					/* destroy */
    NULL,					/* ui info */
    NULL,					/* extra info */
    &prefs_info				/* prefs info */
};

static void
init_plugin (PurplePlugin *plugin)
{
	bindtextdomain (PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (PACKAGE, "UTF-8");

	info.name = _("Libnotify Popups");
	info.summary = _("Displays popups via libnotify.");
	info.description = _("Pidgin-libnotify:\nDisplays popups via libnotify.");

	purple_prefs_add_none ("/plugins/gtk/libnotify");
	purple_prefs_add_bool ("/plugins/gtk/libnotify/newmsg", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/newmsgtxt", TRUE);
	purple_prefs_add_string ("/plugins/gtk/libnotify/triggerMessages", "");
	purple_prefs_add_bool ("/plugins/gtk/libnotify/othermsgs", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/blocked", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/newconvonly", FALSE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/signon", TRUE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/signoff", FALSE);
	purple_prefs_add_bool ("/plugins/gtk/libnotify/only_available", FALSE);
}

PURPLE_INIT_PLUGIN(notify, init_plugin, info)

