/***************************************************************************
 *   Copyright (C) 2010~2010 by CSSlayer                                   *
 *   wengxt@gmail.com                                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "fcitx/fcitx.h"
#include "fcitx/module.h"
#include "fcitx-utils/utarray.h"
#include "fcitx/instance.h"
#include "fcitx-utils/log.h"
#include <dbus/dbus.h>
#include <libintl.h>
#include "dbusstuff.h"
#include <unistd.h>
#include <fcitx-utils/utils.h>


typedef struct FcitxDBusWatch {
  DBusWatch *watch;      
  struct FcitxDBusWatch *next;
} FcitxDBusWatch;

typedef struct FcitxDBus {
    DBusConnection *conn;
    FcitxInstance* owner;
    FcitxDBusWatch* watches;
} FcitxDBus;

static void* DBusCreate(FcitxInstance* instance);
static void DBusSetFD(void* arg);
static void DBusProcessEvent(void* arg);
static void* DBusGetConnection(void* arg, FcitxModuleFunctionArg args);
static dbus_bool_t FcitxDBusAddWatch(DBusWatch *watch, void *data);
static void FcitxDBusRemoveWatch(DBusWatch *watch, void *data);

FCITX_EXPORT_API
FcitxModule module = {
    DBusCreate,
    DBusSetFD,
    DBusProcessEvent,
    NULL,
    NULL
};

void* DBusCreate(FcitxInstance* instance)
{
    FcitxDBus *dbusmodule = (FcitxDBus*) fcitx_malloc0(sizeof(FcitxDBus));
    FcitxAddon* dbusaddon = GetAddonByName(&instance->addons, FCITX_DBUS_NAME);
    DBusError err;
    
    dbus_threads_init_default();

    // first init dbus
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        FcitxLog(DEBUG, _("Connection Error (%s)"), err.message);
        dbus_error_free(&err);
    }
    if (NULL == conn) {
        free(dbusmodule);
        return NULL;
    }
    dbusmodule->conn = conn;
    dbusmodule->owner = instance;

    // request a name on the bus
    int ret = dbus_bus_request_name(conn, FCITX_DBUS_SERVICE,
                                DBUS_NAME_FLAG_REPLACE_EXISTING,
                                &err);
    if (dbus_error_is_set(&err)) {
        FcitxLog(DEBUG, _("Name Error (%s)"), err.message);
        dbus_error_free(&err);
    }
    if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != ret) {
        return NULL;
    }
    if (dbus_connection_set_watch_functions(conn, FcitxDBusAddWatch, FcitxDBusRemoveWatch, 
                      NULL, dbusmodule, NULL))
    {
    }

    dbus_connection_flush(conn);
    AddFunction(dbusaddon, DBusGetConnection);

    return dbusmodule;
}

void* DBusGetConnection(void* arg, FcitxModuleFunctionArg args)
{
    FcitxDBus* dbusmodule = (FcitxDBus*)arg;
    return dbusmodule->conn;
}

static dbus_bool_t FcitxDBusAddWatch(DBusWatch *watch, void *data)
{
    FcitxDBusWatch *w;
    FcitxDBus* dbusmodule = (FcitxDBus*) data;
    
    for (w = dbusmodule->watches; w; w = w->next)
    if (w->watch == watch)
      return TRUE;

  if (!(w = fcitx_malloc0(sizeof(FcitxDBusWatch))))
    return FALSE;

  w->watch = watch;
  w->next = dbusmodule->watches;
  dbusmodule->watches = w;
  return TRUE;
}

static void FcitxDBusRemoveWatch(DBusWatch *watch, void *data)
{
    FcitxDBusWatch **up, *w;  
    FcitxDBus* dbusmodule = (FcitxDBus*) data;
    
    for (up = &(dbusmodule->watches), w = dbusmodule->watches; w; w = w->next)
    {
        if (w->watch == watch)
        {
            *up = w->next;
            free(w);
        }
        else
            up = &(w->next);
    }
}

void DBusSetFD(void* arg)
{
    FcitxDBus* dbusmodule = (FcitxDBus*) arg;
    FcitxDBusWatch *w;
    FcitxInstance* instance = dbusmodule->owner;
    
    for (w = dbusmodule->watches; w; w = w->next)
    if (dbus_watch_get_enabled(w->watch))
    {
        unsigned int flags = dbus_watch_get_flags(w->watch);
        int fd = dbus_watch_get_unix_fd(w->watch);
        
        if (dbusmodule->owner->maxfd < fd)
            dbusmodule->owner->maxfd = fd;
        
        if (flags & DBUS_WATCH_READABLE)
            FD_SET(fd, &instance->rfds);
        
        if (flags & DBUS_WATCH_WRITABLE)
            FD_SET(fd, &instance->wfds);
        
        FD_SET(fd, &instance->efds);
    }
}


void DBusProcessEvent(void* arg)
{
    FcitxDBus* dbusmodule = (FcitxDBus*) arg;
    DBusConnection *connection = (DBusConnection *)dbusmodule->conn;
    FcitxDBusWatch *w;
    
    for (w = dbusmodule->watches; w; w = w->next)
    {
        if (dbus_watch_get_enabled(w->watch))
        {
            unsigned int flags = 0;
            int fd = dbus_watch_get_unix_fd(w->watch);
            
            if (FD_ISSET(fd, &dbusmodule->owner->rfds))
                flags |= DBUS_WATCH_READABLE;
            
            if (FD_ISSET(fd, &dbusmodule->owner->wfds))
                flags |= DBUS_WATCH_WRITABLE;
            
            if (FD_ISSET(fd, &dbusmodule->owner->efds))
                flags |= DBUS_WATCH_ERROR;

            if (flags != 0)
                dbus_watch_handle(w->watch, flags);
        }
    }
    
    if (connection)
    {
        dbus_connection_ref (connection);
        while (dbus_connection_dispatch (connection) == DBUS_DISPATCH_DATA_REMAINS);
        dbus_connection_unref (connection);
    }
}