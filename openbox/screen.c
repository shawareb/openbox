/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   screen.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "debug.h"
#include "openbox.h"
#include "dock.h"
#include "xerror.h"
#include "prop.h"
#include "grab.h"
#include "startupnotify.h"
#include "moveresize.h"
#include "config.h"
#include "screen.h"
#include "client.h"
#include "session.h"
#include "frame.h"
#include "event.h"
#include "focus.h"
#include "popup.h"
#include "extensions.h"
#include "render/render.h"
#include "gettext.h"

#include <X11/Xlib.h>
#ifdef HAVE_UNISTD_H
#  include <sys/types.h>
#  include <unistd.h>
#endif
#include <assert.h>

/*! The event mask to grab on the root window */
#define ROOT_EVENTMASK (StructureNotifyMask | PropertyChangeMask | \
                        EnterWindowMask | LeaveWindowMask | \
                        SubstructureRedirectMask | FocusChangeMask | \
                        ButtonPressMask | ButtonReleaseMask | ButtonMotionMask)

guint    screen_num_desktops;
guint    screen_num_monitors;
guint    screen_desktop;
guint    screen_last_desktop;
Size     screen_physical_size;
gboolean screen_showing_desktop;
DesktopLayout screen_desktop_layout;
gchar  **screen_desktop_names;
Window   screen_support_win;
Time     screen_desktop_user_time = CurrentTime;

static Rect  **area; /* array of desktop holding array of xinerama areas */
static Rect  *monitor_area;

static ObPagerPopup *desktop_cycle_popup;

static gboolean replace_wm()
{
    gchar *wm_sn;
    Atom wm_sn_atom;
    Window current_wm_sn_owner;
    Time timestamp;

    wm_sn = g_strdup_printf("WM_S%d", ob_screen);
    wm_sn_atom = XInternAtom(ob_display, wm_sn, FALSE);
    g_free(wm_sn);

    current_wm_sn_owner = XGetSelectionOwner(ob_display, wm_sn_atom);
    if (current_wm_sn_owner == screen_support_win)
        current_wm_sn_owner = None;
    if (current_wm_sn_owner) {
        if (!ob_replace_wm) {
            g_message(_("A window manager is already running on screen %d"),
                      ob_screen);
            return FALSE;
        }
        xerror_set_ignore(TRUE);
        xerror_occured = FALSE;

        /* We want to find out when the current selection owner dies */
        XSelectInput(ob_display, current_wm_sn_owner, StructureNotifyMask);
        XSync(ob_display, FALSE);

        xerror_set_ignore(FALSE);
        if (xerror_occured)
            current_wm_sn_owner = None;
    }

    {
        /* Generate a timestamp */
        XEvent event;

        XSelectInput(ob_display, screen_support_win, PropertyChangeMask);

        XChangeProperty(ob_display, screen_support_win,
                        prop_atoms.wm_class, prop_atoms.string,
                        8, PropModeAppend, NULL, 0);
        XWindowEvent(ob_display, screen_support_win,
                     PropertyChangeMask, &event);

        XSelectInput(ob_display, screen_support_win, NoEventMask);

        timestamp = event.xproperty.time;
    }

    XSetSelectionOwner(ob_display, wm_sn_atom, screen_support_win,
                       timestamp);

    if (XGetSelectionOwner(ob_display, wm_sn_atom) != screen_support_win) {
        g_message(_("Could not acquire window manager selection on screen %d"),
                  ob_screen);
        return FALSE;
    }

    /* Wait for old window manager to go away */
    if (current_wm_sn_owner) {
      XEvent event;
      gulong wait = 0;
      const gulong timeout = G_USEC_PER_SEC * 15; /* wait for 15s max */

      while (wait < timeout) {
          if (XCheckWindowEvent(ob_display, current_wm_sn_owner,
                                StructureNotifyMask, &event) &&
              event.type == DestroyNotify)
              break;
          g_usleep(G_USEC_PER_SEC / 10);
          wait += G_USEC_PER_SEC / 10;
      }

      if (wait >= timeout) {
          g_message(_("The WM on screen %d is not exiting"), ob_screen);
          return FALSE;
      }
    }

    /* Send client message indicating that we are now the WM */
    prop_message(RootWindow(ob_display, ob_screen), prop_atoms.manager,
                 timestamp, wm_sn_atom, screen_support_win, 0,
                 SubstructureNotifyMask);

    return TRUE;
}

gboolean screen_annex()
{
    XSetWindowAttributes attrib;
    pid_t pid;
    gint i, num_support;
    Atom *prop_atoms_start, *wm_supported_pos;
    gulong *supported;

    /* create the netwm support window */
    attrib.override_redirect = TRUE;
    screen_support_win = XCreateWindow(ob_display,
                                       RootWindow(ob_display, ob_screen),
                                       -100, -100, 1, 1, 0,
                                       CopyFromParent, InputOutput,
                                       CopyFromParent,
                                       CWOverrideRedirect, &attrib);
    XMapWindow(ob_display, screen_support_win);
    XLowerWindow(ob_display, screen_support_win);

    if (!replace_wm()) {
        XDestroyWindow(ob_display, screen_support_win);
        return FALSE;
    }

    xerror_set_ignore(TRUE);
    xerror_occured = FALSE;
    XSelectInput(ob_display, RootWindow(ob_display, ob_screen),
                 ROOT_EVENTMASK);
    xerror_set_ignore(FALSE);
    if (xerror_occured) {
        g_message(_("A window manager is already running on screen %d"),
                  ob_screen);

        XDestroyWindow(ob_display, screen_support_win);
        return FALSE;
    }

    screen_set_root_cursor();

    /* set the OPENBOX_PID hint */
    pid = getpid();
    PROP_SET32(RootWindow(ob_display, ob_screen),
               openbox_pid, cardinal, pid);

    /* set supporting window */
    PROP_SET32(RootWindow(ob_display, ob_screen),
               net_supporting_wm_check, window, screen_support_win);

    /* set properties on the supporting window */
    PROP_SETS(screen_support_win, net_wm_name, "Openbox");
    PROP_SET32(screen_support_win, net_supporting_wm_check,
               window, screen_support_win);

    /* set the _NET_SUPPORTED_ATOMS hint */

    /* this is all the atoms after net_supported in the prop_atoms struct */
    prop_atoms_start = (Atom*)&prop_atoms;
    wm_supported_pos = (Atom*)&(prop_atoms.net_supported);
    num_support = sizeof(prop_atoms) / sizeof(Atom) -
        (wm_supported_pos - prop_atoms_start) - 1;
    i = 0;
    supported = g_new(gulong, num_support);
    supported[i++] = prop_atoms.net_supporting_wm_check;
    supported[i++] = prop_atoms.net_wm_full_placement;
    supported[i++] = prop_atoms.net_current_desktop;
    supported[i++] = prop_atoms.net_number_of_desktops;
    supported[i++] = prop_atoms.net_desktop_geometry;
    supported[i++] = prop_atoms.net_desktop_viewport;
    supported[i++] = prop_atoms.net_active_window;
    supported[i++] = prop_atoms.net_workarea;
    supported[i++] = prop_atoms.net_client_list;
    supported[i++] = prop_atoms.net_client_list_stacking;
    supported[i++] = prop_atoms.net_desktop_names;
    supported[i++] = prop_atoms.net_close_window;
    supported[i++] = prop_atoms.net_desktop_layout;
    supported[i++] = prop_atoms.net_showing_desktop;
    supported[i++] = prop_atoms.net_wm_name;
    supported[i++] = prop_atoms.net_wm_visible_name;
    supported[i++] = prop_atoms.net_wm_icon_name;
    supported[i++] = prop_atoms.net_wm_visible_icon_name;
    supported[i++] = prop_atoms.net_wm_desktop;
    supported[i++] = prop_atoms.net_wm_strut;
    supported[i++] = prop_atoms.net_wm_strut_partial;
    supported[i++] = prop_atoms.net_wm_icon;
    supported[i++] = prop_atoms.net_wm_icon_geometry;
    supported[i++] = prop_atoms.net_wm_window_type;
    supported[i++] = prop_atoms.net_wm_window_type_desktop;
    supported[i++] = prop_atoms.net_wm_window_type_dock;
    supported[i++] = prop_atoms.net_wm_window_type_toolbar;
    supported[i++] = prop_atoms.net_wm_window_type_menu;
    supported[i++] = prop_atoms.net_wm_window_type_utility;
    supported[i++] = prop_atoms.net_wm_window_type_splash;
    supported[i++] = prop_atoms.net_wm_window_type_dialog;
    supported[i++] = prop_atoms.net_wm_window_type_normal;
    supported[i++] = prop_atoms.net_wm_allowed_actions;
    supported[i++] = prop_atoms.net_wm_action_move;
    supported[i++] = prop_atoms.net_wm_action_resize;
    supported[i++] = prop_atoms.net_wm_action_minimize;
    supported[i++] = prop_atoms.net_wm_action_shade;
    supported[i++] = prop_atoms.net_wm_action_maximize_horz;
    supported[i++] = prop_atoms.net_wm_action_maximize_vert;
    supported[i++] = prop_atoms.net_wm_action_fullscreen;
    supported[i++] = prop_atoms.net_wm_action_change_desktop;
    supported[i++] = prop_atoms.net_wm_action_close;
    supported[i++] = prop_atoms.net_wm_action_above;
    supported[i++] = prop_atoms.net_wm_action_below;
    supported[i++] = prop_atoms.net_wm_state;
    supported[i++] = prop_atoms.net_wm_state_modal;
    supported[i++] = prop_atoms.net_wm_state_maximized_vert;
    supported[i++] = prop_atoms.net_wm_state_maximized_horz;
    supported[i++] = prop_atoms.net_wm_state_shaded;
    supported[i++] = prop_atoms.net_wm_state_skip_taskbar;
    supported[i++] = prop_atoms.net_wm_state_skip_pager;
    supported[i++] = prop_atoms.net_wm_state_hidden;
    supported[i++] = prop_atoms.net_wm_state_fullscreen;
    supported[i++] = prop_atoms.net_wm_state_above;
    supported[i++] = prop_atoms.net_wm_state_below;
    supported[i++] = prop_atoms.net_wm_state_demands_attention;
    supported[i++] = prop_atoms.net_moveresize_window;
    supported[i++] = prop_atoms.net_wm_moveresize;
    supported[i++] = prop_atoms.net_wm_user_time;
    supported[i++] = prop_atoms.net_wm_user_time_window;
    supported[i++] = prop_atoms.net_frame_extents;
    supported[i++] = prop_atoms.net_request_frame_extents;
    supported[i++] = prop_atoms.net_restack_window;
    supported[i++] = prop_atoms.net_startup_id;
#ifdef SYNC
    supported[i++] = prop_atoms.net_wm_sync_request;
    supported[i++] = prop_atoms.net_wm_sync_request_counter;
#endif

    supported[i++] = prop_atoms.kde_wm_change_state;
    supported[i++] = prop_atoms.kde_net_wm_frame_strut;
    supported[i++] = prop_atoms.kde_net_wm_window_type_override;

    supported[i++] = prop_atoms.ob_wm_action_undecorate;
    supported[i++] = prop_atoms.ob_wm_state_undecorated;
    supported[i++] = prop_atoms.openbox_pid;
    supported[i++] = prop_atoms.ob_config;
    supported[i++] = prop_atoms.ob_control;
    g_assert(i == num_support);

    PROP_SETA32(RootWindow(ob_display, ob_screen),
                net_supported, atom, supported, num_support);
    g_free(supported);

    return TRUE;
}

void screen_startup(gboolean reconfig)
{
    guint i, numnames;
    gchar **names;
    GSList *it;
    guint32 d;

    desktop_cycle_popup = pager_popup_new(FALSE);
    pager_popup_height(desktop_cycle_popup, POPUP_HEIGHT);

    if (reconfig) {
        /* update the pager popup's width */
        pager_popup_text_width_to_strings(desktop_cycle_popup,
                                          screen_desktop_names,
                                          screen_num_desktops);
        return;
    }

    /* get the initial size */
    screen_resize();

    /* get the desktop names */
    numnames = g_slist_length(config_desktops_names);
    names = g_new(gchar*, numnames + 1);
    names[numnames] = NULL;
    for (i = 0, it = config_desktops_names; it; ++i, it = g_slist_next(it))
        names[i] = g_strdup(it->data);

    /* set the root window property */
    PROP_SETSS(RootWindow(ob_display, ob_screen), net_desktop_names,names);

    g_strfreev(names);

    /* set the number of desktops */
    screen_num_desktops = 0;
    screen_set_num_desktops(config_desktops_num);

    /* start on the current desktop when a wm was already running */
    if (PROP_GET32(RootWindow(ob_display, ob_screen),
                   net_current_desktop, cardinal, &d) &&
        d < screen_num_desktops)
    {
        screen_set_desktop(d, FALSE);
    } else if (session_desktop >= 0)
        screen_set_desktop(MIN((guint)session_desktop,
                               screen_num_desktops), FALSE);
    else
        screen_set_desktop(MIN(config_screen_firstdesk,
                               screen_num_desktops) - 1, FALSE);

    /* don't start in showing-desktop mode */
    screen_showing_desktop = FALSE;
    PROP_SET32(RootWindow(ob_display, ob_screen),
               net_showing_desktop, cardinal, screen_showing_desktop);

    screen_update_layout();
}

void screen_shutdown(gboolean reconfig)
{
    Rect **r;

    pager_popup_free(desktop_cycle_popup);

    if (reconfig)
        return;

    XSelectInput(ob_display, RootWindow(ob_display, ob_screen),
                 NoEventMask);

    /* we're not running here no more! */
    PROP_ERASE(RootWindow(ob_display, ob_screen), openbox_pid);
    /* not without us */
    PROP_ERASE(RootWindow(ob_display, ob_screen), net_supported);
    /* don't keep this mode */
    PROP_ERASE(RootWindow(ob_display, ob_screen), net_showing_desktop);

    XDestroyWindow(ob_display, screen_support_win);

    g_strfreev(screen_desktop_names);
    screen_desktop_names = NULL;

    for (r = area; *r; ++r)
        g_free(*r);
    g_free(area);
    area = NULL;
}

void screen_resize()
{
    static gint oldw = 0, oldh = 0;
    gint w, h;
    GList *it;
    gulong geometry[2];

    w = WidthOfScreen(ScreenOfDisplay(ob_display, ob_screen));
    h = HeightOfScreen(ScreenOfDisplay(ob_display, ob_screen));

    if (w == oldw && h == oldh) return;

    oldw = w; oldh = h;

    /* Set the _NET_DESKTOP_GEOMETRY hint */
    screen_physical_size.width = geometry[0] = w;
    screen_physical_size.height = geometry[1] = h;
    PROP_SETA32(RootWindow(ob_display, ob_screen),
                net_desktop_geometry, cardinal, geometry, 2);

    if (ob_state() == OB_STATE_STARTING)
        return;

    screen_update_areas();
    dock_configure();

    for (it = client_list; it; it = g_list_next(it))
        client_move_onscreen(it->data, FALSE);
}

void screen_set_num_desktops(guint num)
{
    guint old;
    gulong *viewport;
    GList *it;

    g_assert(num > 0);

    if (screen_num_desktops == num) return;

    old = screen_num_desktops;
    screen_num_desktops = num;
    PROP_SET32(RootWindow(ob_display, ob_screen),
               net_number_of_desktops, cardinal, num);

    /* set the viewport hint */
    viewport = g_new0(gulong, num * 2);
    PROP_SETA32(RootWindow(ob_display, ob_screen),
                net_desktop_viewport, cardinal, viewport, num * 2);
    g_free(viewport);

    /* the number of rows/columns will differ */
    screen_update_layout();

    /* move windows on desktops that will no longer exist! */
    for (it = client_list; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        if (c->desktop >= num && c->desktop != DESKTOP_ALL)
            client_set_desktop(c, num - 1, FALSE);
    }
 
    /* change our struts/area to match (after moving windows) */
    screen_update_areas();

    /* may be some unnamed desktops that we need to fill in with names
     (after updating the areas so the popup can resize) */
    screen_update_desktop_names();

    /* change our desktop if we're on one that no longer exists! */
    if (screen_desktop >= screen_num_desktops)
        screen_set_desktop(num - 1, TRUE);
}

void screen_set_desktop(guint num, gboolean dofocus)
{
    ObClient *c;
    GList *it;
    guint old;
     
    g_assert(num < screen_num_desktops);

    old = screen_desktop;
    screen_desktop = num;
    PROP_SET32(RootWindow(ob_display, ob_screen),
               net_current_desktop, cardinal, num);

    if (old == num) return;

    screen_last_desktop = old;

    ob_debug("Moving to desktop %d\n", num+1);

    if (moveresize_client)
        client_set_desktop(moveresize_client, num, TRUE);

    /* show windows before hiding the rest to lessen the enter/leave events */

    /* show windows from top to bottom */
    for (it = stacking_list; it; it = g_list_next(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            client_show(c);
        }
    }

    /* have to try focus here because when you leave an empty desktop
       there is no focus out to watch for

       do this before hiding the windows so if helper windows are coming
       with us, they don't get hidden
    */
    if (dofocus && (c = focus_fallback(TRUE))) {
        /* only do the flicker reducing stuff ahead of time if we are going
           to call xsetinputfocus on the window ourselves. otherwise there is
           no guarantee the window will actually take focus.. */
        if (c->can_focus) {
            /* reduce flicker by hiliting now rather than waiting for the
               server FocusIn event */
            frame_adjust_focus(c->frame, TRUE);
            /* do this here so that if you switch desktops to a window with
               helper windows then the helper windows won't flash */
            client_bring_helper_windows(c);
        }
    }

    /* hide windows from bottom to top */
    for (it = g_list_last(stacking_list); it; it = g_list_previous(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            client_hide(c);
        }
    }

    event_ignore_queued_enters();

    if (event_curtime != CurrentTime)
        screen_desktop_user_time = event_curtime;
}

static void get_row_col(guint d, guint *r, guint *c)
{
    switch (screen_desktop_layout.orientation) {
    case OB_ORIENTATION_HORZ:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            *r = d / screen_desktop_layout.columns;
            *c = d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_BOTTOMLEFT:
            *r = screen_desktop_layout.rows - 1 -
                d / screen_desktop_layout.columns;
            *c = d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_TOPRIGHT:
            *r = d / screen_desktop_layout.columns;
            *c = screen_desktop_layout.columns - 1 -
                d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_BOTTOMRIGHT:
            *r = screen_desktop_layout.rows - 1 -
                d / screen_desktop_layout.columns;
            *c = screen_desktop_layout.columns - 1 -
                d % screen_desktop_layout.columns;
            break;
        }
        break;
    case OB_ORIENTATION_VERT:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            *r = d % screen_desktop_layout.rows;
            *c = d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_BOTTOMLEFT:
            *r = screen_desktop_layout.rows - 1 -
                d % screen_desktop_layout.rows;
            *c = d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_TOPRIGHT:
            *r = d % screen_desktop_layout.rows;
            *c = screen_desktop_layout.columns - 1 -
                d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_BOTTOMRIGHT:
            *r = screen_desktop_layout.rows - 1 -
                d % screen_desktop_layout.rows;
            *c = screen_desktop_layout.columns - 1 -
                d / screen_desktop_layout.rows;
            break;
        }
        break;
    }
}

static guint translate_row_col(guint r, guint c)
{
    switch (screen_desktop_layout.orientation) {
    case OB_ORIENTATION_HORZ:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            return r % screen_desktop_layout.rows *
                screen_desktop_layout.columns +
                c % screen_desktop_layout.columns;
        case OB_CORNER_BOTTOMLEFT:
            return (screen_desktop_layout.rows - 1 -
                    r % screen_desktop_layout.rows) *
                screen_desktop_layout.columns +
                c % screen_desktop_layout.columns;
        case OB_CORNER_TOPRIGHT:
            return r % screen_desktop_layout.rows *
                screen_desktop_layout.columns +
                (screen_desktop_layout.columns - 1 -
                 c % screen_desktop_layout.columns);
        case OB_CORNER_BOTTOMRIGHT:
            return (screen_desktop_layout.rows - 1 -
                    r % screen_desktop_layout.rows) *
                screen_desktop_layout.columns +
                (screen_desktop_layout.columns - 1 -
                 c % screen_desktop_layout.columns);
        }
    case OB_ORIENTATION_VERT:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            return c % screen_desktop_layout.columns *
                screen_desktop_layout.rows +
                r % screen_desktop_layout.rows;
        case OB_CORNER_BOTTOMLEFT:
            return c % screen_desktop_layout.columns *
                screen_desktop_layout.rows +
                (screen_desktop_layout.rows - 1 -
                 r % screen_desktop_layout.rows);
        case OB_CORNER_TOPRIGHT:
            return (screen_desktop_layout.columns - 1 -
                    c % screen_desktop_layout.columns) *
                screen_desktop_layout.rows +
                r % screen_desktop_layout.rows;
        case OB_CORNER_BOTTOMRIGHT:
            return (screen_desktop_layout.columns - 1 -
                    c % screen_desktop_layout.columns) *
                screen_desktop_layout.rows +
                (screen_desktop_layout.rows - 1 -
                 r % screen_desktop_layout.rows);
        }
    }
    g_assert_not_reached();
    return 0;
}

void screen_desktop_popup(guint d, gboolean show)
{
    Rect *a;

    if (!show) {
        pager_popup_hide(desktop_cycle_popup);
    } else {
        a = screen_physical_area_monitor(0);
        pager_popup_position(desktop_cycle_popup, CenterGravity,
                             a->x + a->width / 2, a->y + a->height / 2);
        pager_popup_icon_size_multiplier(desktop_cycle_popup,
                                         (screen_desktop_layout.columns /
                                          screen_desktop_layout.rows) / 2,
                                         (screen_desktop_layout.rows/
                                          screen_desktop_layout.columns) / 2);
        pager_popup_max_width(desktop_cycle_popup,
                              MAX(a->width/3, POPUP_WIDTH));
        pager_popup_show(desktop_cycle_popup, screen_desktop_names[d], d);
    }
}

guint screen_cycle_desktop(ObDirection dir, gboolean wrap, gboolean linear,
                           gboolean dialog, gboolean done, gboolean cancel)
{
    guint r, c;
    static guint d = (guint)-1;
    guint ret, oldd;

    if (d == (guint)-1)
        d = screen_desktop;

    if ((cancel || done) && dialog)
        goto show_cycle_dialog;

    oldd = d;
    get_row_col(d, &r, &c);

    if (linear) {
        switch (dir) {
        case OB_DIRECTION_EAST:
            if (d < screen_num_desktops - 1)
                ++d;
            else if (wrap)
                d = 0;
            break;
        case OB_DIRECTION_WEST:
            if (d > 0)
                --d;
            else if (wrap)
                d = screen_num_desktops - 1;
            break;
        default:
            assert(0);
            return screen_desktop;
        }
    } else {
        switch (dir) {
        case OB_DIRECTION_EAST:
            ++c;
            if (c >= screen_desktop_layout.columns) {
                if (wrap)
                    c = 0;
                else
                    goto show_cycle_dialog;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap) {
                    ++c;
                } else {
                    d = oldd;
                    goto show_cycle_dialog;
                }
            }
            break;
        case OB_DIRECTION_WEST:
            --c;
            if (c >= screen_desktop_layout.columns) {
                if (wrap)
                    c = screen_desktop_layout.columns - 1;
                else
                    goto show_cycle_dialog;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap) {
                    --c;
                } else {
                    d = oldd;
                    goto show_cycle_dialog;
                }
            }
            break;
        case OB_DIRECTION_SOUTH:
            ++r;
            if (r >= screen_desktop_layout.rows) {
                if (wrap)
                    r = 0;
                else
                    goto show_cycle_dialog;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap) {
                    ++r;
                } else {
                    d = oldd;
                    goto show_cycle_dialog;
                }
            }
            break;
        case OB_DIRECTION_NORTH:
            --r;
            if (r >= screen_desktop_layout.rows) {
                if (wrap)
                    r = screen_desktop_layout.rows - 1;
                else
                    goto show_cycle_dialog;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap) {
                    --r;
                } else {
                    d = oldd;
                    goto show_cycle_dialog;
                }
            }
            break;
        default:
            assert(0);
            return d = screen_desktop;
        }

        d = translate_row_col(r, c);
    }

show_cycle_dialog:
    if (dialog && !cancel && !done) {
        screen_desktop_popup(d, TRUE);
    } else
        screen_desktop_popup(0, FALSE);
    ret = d;

    if (!dialog || cancel || done)
        d = (guint)-1;

    return ret;
}

void screen_update_layout()
{
    ObOrientation orient;
    ObCorner corner;
    guint rows;
    guint cols;
    guint32 *data;
    guint num;
    gboolean valid = FALSE;

    if (PROP_GETA32(RootWindow(ob_display, ob_screen),
                    net_desktop_layout, cardinal, &data, &num)) {
        if (num == 3 || num == 4) {

            if (data[0] == prop_atoms.net_wm_orientation_vert)
                orient = OB_ORIENTATION_VERT;
            else if (data[0] == prop_atoms.net_wm_orientation_horz)
                orient = OB_ORIENTATION_HORZ;
            else
                goto screen_update_layout_bail;

            if (num < 4)
                corner = OB_CORNER_TOPLEFT;
            else {
                if (data[3] == prop_atoms.net_wm_topleft)
                    corner = OB_CORNER_TOPLEFT;
                else if (data[3] == prop_atoms.net_wm_topright)
                    corner = OB_CORNER_TOPRIGHT;
                else if (data[3] == prop_atoms.net_wm_bottomright)
                    corner = OB_CORNER_BOTTOMRIGHT;
                else if (data[3] == prop_atoms.net_wm_bottomleft)
                    corner = OB_CORNER_BOTTOMLEFT;
                else
                    goto screen_update_layout_bail;
            }

            cols = data[1];
            rows = data[2];

            /* fill in a zero rows/columns */
            if ((cols == 0 && rows == 0)) { /* both 0's is bad data.. */
                goto screen_update_layout_bail;
            } else {
                if (cols == 0) {
                    cols = screen_num_desktops / rows;
                    if (rows * cols < screen_num_desktops)
                        cols++;
                    if (rows * cols >= screen_num_desktops + cols)
                        rows--;
                } else if (rows == 0) {
                    rows = screen_num_desktops / cols;
                    if (cols * rows < screen_num_desktops)
                        rows++;
                    if (cols * rows >= screen_num_desktops + rows)
                        cols--;
                }
            }

            /* bounds checking */
            if (orient == OB_ORIENTATION_HORZ) {
                cols = MIN(screen_num_desktops, cols);
                rows = MIN(rows, (screen_num_desktops + cols - 1) / cols);
                cols = screen_num_desktops / rows +
                    !!(screen_num_desktops % rows);
            } else {
                rows = MIN(screen_num_desktops, rows);
                cols = MIN(cols, (screen_num_desktops + rows - 1) / rows);
                rows = screen_num_desktops / cols +
                    !!(screen_num_desktops % cols);
            }

            valid = TRUE;
        }
    screen_update_layout_bail:
        g_free(data);
    }

    if (!valid) {
        /* defaults */
        orient = OB_ORIENTATION_HORZ;
        corner = OB_CORNER_TOPLEFT;
        rows = 1;
        cols = screen_num_desktops;
    }

    screen_desktop_layout.orientation = orient;
    screen_desktop_layout.start_corner = corner;
    screen_desktop_layout.rows = rows;
    screen_desktop_layout.columns = cols;
}

void screen_update_desktop_names()
{
    guint i;

    /* empty the array */
    g_strfreev(screen_desktop_names);
    screen_desktop_names = NULL;

    if (PROP_GETSS(RootWindow(ob_display, ob_screen),
                   net_desktop_names, utf8, &screen_desktop_names))
        for (i = 0; screen_desktop_names[i] && i < screen_num_desktops; ++i);
    else
        i = 0;
    if (i < screen_num_desktops) {
        screen_desktop_names = g_renew(gchar*, screen_desktop_names,
                                       screen_num_desktops + 1);
        screen_desktop_names[screen_num_desktops] = NULL;
        for (; i < screen_num_desktops; ++i)
            screen_desktop_names[i] = g_strdup_printf("desktop %i", i + 1);
    }

    /* resize the pager for these names */
    pager_popup_text_width_to_strings(desktop_cycle_popup,
                                      screen_desktop_names,
                                      screen_num_desktops);
}

void screen_show_desktop(gboolean show, ObClient *show_only)
{
    GList *it;
     
    if (show == screen_showing_desktop) return; /* no change */

    screen_showing_desktop = show;

    if (show) {
        /* hide windows bottom to top */
        for (it = g_list_last(stacking_list); it; it = g_list_previous(it)) {
            if (WINDOW_IS_CLIENT(it->data)) {
                ObClient *client = it->data;
                client_showhide(client);
            }
        }
    }
    else {
        /* restore windows top to bottom */
        for (it = stacking_list; it; it = g_list_next(it)) {
            if (WINDOW_IS_CLIENT(it->data)) {
                ObClient *client = it->data;
                if (client_should_show(client)) {
                    if (!show_only || client == show_only)
                        client_show(client);
                    else
                        client_iconify(client, TRUE, FALSE, TRUE);
                }
            }
        }
    }

    if (show) {
        /* focus the desktop */
        for (it = focus_order; it; it = g_list_next(it)) {
            ObClient *c = it->data;
            if (c->type == OB_CLIENT_TYPE_DESKTOP &&
                (c->desktop == screen_desktop || c->desktop == DESKTOP_ALL) &&
                client_focus(it->data))
                break;
        }
    }
    else if (!show_only) {
        ObClient *c;

        if ((c = focus_fallback(TRUE))) {
            /* only do the flicker reducing stuff ahead of time if we are going
               to call xsetinputfocus on the window ourselves. otherwise there
               is no guarantee the window will actually take focus.. */
            if (c->can_focus) {
                /* reduce flicker by hiliting now rather than waiting for the
                   server FocusIn event */
                frame_adjust_focus(c->frame, TRUE);
            }
        }
    }

    show = !!show; /* make it boolean */
    PROP_SET32(RootWindow(ob_display, ob_screen),
               net_showing_desktop, cardinal, show);
}

void screen_install_colormap(ObClient *client, gboolean install)
{
    if (client == NULL) {
        if (install)
            XInstallColormap(RrDisplay(ob_rr_inst), RrColormap(ob_rr_inst));
        else
            XUninstallColormap(RrDisplay(ob_rr_inst), RrColormap(ob_rr_inst));
    } else {
        xerror_set_ignore(TRUE);
        if (install) {
            if (client->colormap != None)
                XInstallColormap(RrDisplay(ob_rr_inst), client->colormap);
        } else
            XUninstallColormap(RrDisplay(ob_rr_inst), client->colormap);
        xerror_set_ignore(FALSE);
    }
}

static inline void
screen_area_add_strut_left(const StrutPartial *s, const Rect *monitor_area,
                           gint edge, Strut *ret)
{
    if (s->left &&
        ((s->left_end <= s->left_start) ||
         (RECT_TOP(*monitor_area) < s->left_end &&
          RECT_BOTTOM(*monitor_area) > s->left_start)))
        ret->left = MAX(ret->left, edge);
}

static inline void
screen_area_add_strut_top(const StrutPartial *s, const Rect *monitor_area,
                          gint edge, Strut *ret)
{
    if (s->top &&
        ((s->top_end <= s->top_start) ||
         (RECT_LEFT(*monitor_area) < s->top_end &&
          RECT_RIGHT(*monitor_area) > s->top_start)))
        ret->top = MAX(ret->top, edge);
}

static inline void
screen_area_add_strut_right(const StrutPartial *s, const Rect *monitor_area,
                            gint edge, Strut *ret)
{
    if (s->right &&
        ((s->right_end <= s->right_start) ||
         (RECT_TOP(*monitor_area) < s->right_end &&
          RECT_BOTTOM(*monitor_area) > s->right_start)))
        ret->right = MAX(ret->right, edge);
}

static inline void
screen_area_add_strut_bottom(const StrutPartial *s, const Rect *monitor_area,
                             gint edge, Strut *ret)
{
    if (s->bottom &&
        ((s->bottom_end <= s->bottom_start) ||
         (RECT_LEFT(*monitor_area) < s->bottom_end &&
          RECT_RIGHT(*monitor_area) > s->bottom_start)))
        ret->bottom = MAX(ret->bottom, edge);
}

void screen_update_areas()
{
    guint i, x;
    gulong *dims;
    GList *it;
    gint o;

    g_free(monitor_area);
    extensions_xinerama_screens(&monitor_area, &screen_num_monitors);

    if (area) {
        for (i = 0; area[i]; ++i)
            g_free(area[i]);
        g_free(area);
    }

    area = g_new(Rect*, screen_num_desktops + 2);
    for (i = 0; i < screen_num_desktops + 1; ++i)
        area[i] = g_new0(Rect, screen_num_monitors + 1);
    area[i] = NULL;
     
    dims = g_new(gulong, 4 * screen_num_desktops);

    for (i = 0; i < screen_num_desktops + 1; ++i) {
        Strut *struts;
        gint l, r, t, b;

        struts = g_new0(Strut, screen_num_monitors);

        /* calc the xinerama areas */
        for (x = 0; x < screen_num_monitors; ++x) {
            area[i][x] = monitor_area[x];
            if (x == 0) {
                l = monitor_area[x].x;
                t = monitor_area[x].y;
                r = monitor_area[x].x + monitor_area[x].width - 1;
                b = monitor_area[x].y + monitor_area[x].height - 1;
            } else {
                l = MIN(l, monitor_area[x].x);
                t = MIN(t, monitor_area[x].y);
                r = MAX(r, monitor_area[x].x + monitor_area[x].width - 1);
                b = MAX(b, monitor_area[x].y + monitor_area[x].height - 1);
            }
        }
        RECT_SET(area[i][x], l, t, r - l + 1, b - t + 1);

        /* apply the struts */

        /* find the left-most xin heads, i do this in 2 loops :| */
        o = area[i][0].x;
        for (x = 1; x < screen_num_monitors; ++x)
            o = MIN(o, area[i][x].x);

        for (x = 0; x < screen_num_monitors; ++x) {
            for (it = client_list; it; it = g_list_next(it)) {
                ObClient *c = it->data;
                screen_area_add_strut_left(&c->strut,
                                           &monitor_area[x],
                                           o + c->strut.left - area[i][x].x,
                                           &struts[x]);
            }
            screen_area_add_strut_left(&dock_strut,
                                       &monitor_area[x],
                                       o + dock_strut.left - area[i][x].x,
                                       &struts[x]);

            area[i][x].x += struts[x].left;
            area[i][x].width -= struts[x].left;
        }

        /* find the top-most xin heads, i do this in 2 loops :| */
        o = area[i][0].y;
        for (x = 1; x < screen_num_monitors; ++x)
            o = MIN(o, area[i][x].y);

        for (x = 0; x < screen_num_monitors; ++x) {
            for (it = client_list; it; it = g_list_next(it)) {
                ObClient *c = it->data;
                screen_area_add_strut_top(&c->strut,
                                           &monitor_area[x],
                                           o + c->strut.top - area[i][x].y,
                                           &struts[x]);
            }
            screen_area_add_strut_top(&dock_strut,
                                      &monitor_area[x],
                                      o + dock_strut.top - area[i][x].y,
                                      &struts[x]);

            area[i][x].y += struts[x].top;
            area[i][x].height -= struts[x].top;
        }

        /* find the right-most xin heads, i do this in 2 loops :| */
        o = area[i][0].x + area[i][0].width - 1;
        for (x = 1; x < screen_num_monitors; ++x)
            o = MAX(o, area[i][x].x + area[i][x].width - 1);

        for (x = 0; x < screen_num_monitors; ++x) {
            for (it = client_list; it; it = g_list_next(it)) {
                ObClient *c = it->data;
                screen_area_add_strut_right(&c->strut,
                                           &monitor_area[x],
                                           (area[i][x].x +
                                            area[i][x].width - 1) -
                                            (o - c->strut.right),
                                            &struts[x]);
            }
            screen_area_add_strut_right(&dock_strut,
                                        &monitor_area[x],
                                        (area[i][x].x +
                                         area[i][x].width - 1) -
                                        (o - dock_strut.right),
                                        &struts[x]);

            area[i][x].width -= struts[x].right;
        }

        /* find the bottom-most xin heads, i do this in 2 loops :| */
        o = area[i][0].y + area[i][0].height - 1;
        for (x = 1; x < screen_num_monitors; ++x)
            o = MAX(o, area[i][x].y + area[i][x].height - 1);

        for (x = 0; x < screen_num_monitors; ++x) {
            for (it = client_list; it; it = g_list_next(it)) {
                ObClient *c = it->data;
                screen_area_add_strut_bottom(&c->strut,
                                             &monitor_area[x],
                                             (area[i][x].y +
                                              area[i][x].height - 1) - \
                                             (o - c->strut.bottom),
                                             &struts[x]);
            }
            screen_area_add_strut_bottom(&dock_strut,
                                         &monitor_area[x],
                                         (area[i][x].y +
                                          area[i][x].height - 1) - \
                                         (o - dock_strut.bottom),
                                         &struts[x]);

            area[i][x].height -= struts[x].bottom;
        }

        l = RECT_LEFT(area[i][0]);
        t = RECT_TOP(area[i][0]);
        r = RECT_RIGHT(area[i][0]);
        b = RECT_BOTTOM(area[i][0]);
        for (x = 1; x < screen_num_monitors; ++x) {
            l = MIN(l, RECT_LEFT(area[i][x]));
            t = MIN(l, RECT_TOP(area[i][x]));
            r = MAX(r, RECT_RIGHT(area[i][x]));
            b = MAX(b, RECT_BOTTOM(area[i][x]));
        }
        RECT_SET(area[i][screen_num_monitors], l, t,
                 r - l + 1, b - t + 1);

        /* XXX optimize when this is run? */

        /* the area has changed, adjust all the maximized 
           windows */
        for (it = client_list; it; it = g_list_next(it)) {
            ObClient *c = it->data; 
            if (i < screen_num_desktops) {
                if (c->desktop == i)
                    client_reconfigure(c);
            } else if (c->desktop == DESKTOP_ALL)
                client_reconfigure(c);
        }
        if (i < screen_num_desktops) {
            /* don't set these for the 'all desktops' area */
            dims[(i * 4) + 0] = area[i][screen_num_monitors].x;
            dims[(i * 4) + 1] = area[i][screen_num_monitors].y;
            dims[(i * 4) + 2] = area[i][screen_num_monitors].width;
            dims[(i * 4) + 3] = area[i][screen_num_monitors].height;
        }

        g_free(struts);
    }

    PROP_SETA32(RootWindow(ob_display, ob_screen), net_workarea, cardinal,
                dims, 4 * screen_num_desktops);

    g_free(dims);
}

Rect *screen_area(guint desktop)
{
    return screen_area_monitor(desktop, screen_num_monitors);
}

Rect *screen_area_monitor(guint desktop, guint head)
{
    if (head > screen_num_monitors)
        return NULL;
    if (desktop >= screen_num_desktops) {
        if (desktop == DESKTOP_ALL)
            return &area[screen_num_desktops][head];
        return NULL;
    }
    return &area[desktop][head];
}

guint screen_find_monitor(Rect *search)
{
    guint i;
    guint most = 0;
    guint mostv = 0;

    for (i = 0; i < screen_num_monitors; ++i) {
        Rect *area = screen_physical_area_monitor(i);
        if (RECT_INTERSECTS_RECT(*area, *search)) {
            Rect r;
            guint v;

            RECT_SET_INTERSECTION(r, *area, *search);
            v = r.width * r.height;

            if (v > mostv) {
                mostv = v;
                most = i;
            }
        }
    }
    return most;
}

Rect *screen_physical_area()
{
    return screen_physical_area_monitor(screen_num_monitors);
}

Rect *screen_physical_area_monitor(guint head)
{
    if (head > screen_num_monitors)
        return NULL;
    return &monitor_area[head];
}

void screen_set_root_cursor()
{
    if (sn_app_starting())
        XDefineCursor(ob_display, RootWindow(ob_display, ob_screen),
                      ob_cursor(OB_CURSOR_BUSY));
    else
        XDefineCursor(ob_display, RootWindow(ob_display, ob_screen),
                      ob_cursor(OB_CURSOR_POINTER));
}

gboolean screen_pointer_pos(gint *x, gint *y)
{
    Window w;
    gint i;
    guint u;
    gboolean ret;

    ret = !!XQueryPointer(ob_display, RootWindow(ob_display, ob_screen),
                          &w, &w, x, y, &i, &i, &u);
    if (!ret) {
        for (i = 0; i < ScreenCount(ob_display); ++i)
            if (i != ob_screen)
                if (XQueryPointer(ob_display, RootWindow(ob_display, i),
                                  &w, &w, x, y, &i, &i, &u))
                    break;
    }
    return ret;
}
