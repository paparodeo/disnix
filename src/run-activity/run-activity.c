/*
 * Disnix - A Nix-based distributed service deployment tool
 * Copyright (C) 2008-2018  Sander van der Burg
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "run-activity.h"
#include <stdlib.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <procreact_pid.h>
#include <procreact_future.h>
#include <package-management.h>
#include <state-management.h>
#include <profilemanifest.h>

static gchar *check_dysnomia_activity_parameters(gchar *type, gchar **derivation, gchar *container, gchar **arguments)
{
    if(type == NULL)
    {
        g_printerr("ERROR: A type must be specified!\n");
        return NULL;
    }
    else if(derivation[0] == NULL)
    {
        g_printerr("ERROR: A Nix store component has to be specified!\n");
        return NULL;
    }

    if(container == NULL)
        return type;
    else
        return container;
}

static int print_strv(ProcReact_Future future)
{
    ProcReact_Status status;
    char **result = procreact_future_get(&future, &status);

    if(status != PROCREACT_STATUS_OK || result == NULL)
        return 1;
    else
    {
        if(result != NULL)
        {
            unsigned int count = 0;

            while(result[count] != NULL)
            {
                g_print("%s\n", result[count]);
                count++;
            }
        }

        procreact_free_string_array(result);

        return 0;
    }
}

static int lock_or_unlock_services(int log_fd, GPtrArray *profile_manifest_array, gchar *action, pid_t (*notify_function) (gchar *type, gchar *container, gchar *component, int stdout, int stderr))
{
    unsigned int i;
    int exit_status = TRUE;

    /* Notify all services for a lock or unlock */
    for(i = 0; i < profile_manifest_array->len; i++)
    {
        ProfileManifestEntry *entry = g_ptr_array_index(profile_manifest_array, i);
        pid_t pid;
        ProcReact_Status status;
        int result;

        dprintf(log_fd, "Notifying %s on %s: of type: %s in container: %s\n", action, entry->service, entry->type, entry->container);
        pid = notify_function(entry->type, entry->container, entry->service, log_fd, log_fd);
        result = procreact_wait_for_boolean(pid, &status);

        if(status != PROCREACT_STATUS_OK || !result)
        {
            dprintf(log_fd, "Cannot %s service!\n", action);
            exit_status = FALSE;
        }
    }

    return exit_status;
}

static int unlock_services(int log_fd, GPtrArray *profile_manifest_array)
{
    return lock_or_unlock_services(log_fd, profile_manifest_array, "unlock", statemgmt_unlock_component);
}

static int lock_services(int log_fd, GPtrArray *profile_manifest_array)
{
    return lock_or_unlock_services(log_fd, profile_manifest_array, "lock", statemgmt_lock_component);
}

static gchar *create_lock_filename(gchar *tmpdir, gchar *profile)
{
    return g_strconcat(tmpdir, "/disnix-", profile, ".lock", NULL);
}

static int lock_profile(int log_fd, gchar *tmpdir, gchar *profile)
{
    int fd, status;
    gchar *lock_filename = create_lock_filename(tmpdir, profile);

    /* If no lock exists, try to create one */
    if((fd = open(lock_filename, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1)
    {
        dprintf(log_fd, "Cannot exclusively open the lock file!\n");
        status = FALSE;
    }
    else
    {
        close(fd);
        status = TRUE;
    }

    g_free(lock_filename);
    return status;
}

static int unlock_profile(int log_fd, gchar *tmpdir, gchar *profile)
{
    gchar *lock_filename = create_lock_filename(tmpdir, profile);
    int status;
    
    if(unlink(lock_filename) == -1)
    {
        dprintf(log_fd, "There is no lock file!\n");
        status = FALSE;
    }
    else
        status = TRUE;
    
    /* Cleanup */
    g_free(lock_filename);
    
    return status;
}

static int acquire_locks(int log_fd, gchar *tmpdir, GPtrArray *profile_manifest_array, gchar *profile)
{
    if(lock_services(log_fd, profile_manifest_array)) /* Attempt to acquire locks from the services */
        return lock_profile(log_fd, tmpdir, profile); /* Finally, lock the profile */
    else
    {
        unlock_services(log_fd, profile_manifest_array);
        return FALSE;
    }
}

static int release_locks(int log_fd, gchar *tmpdir, GPtrArray *profile_manifest_array, gchar *profile)
{
    int status = TRUE;
    
    if(profile_manifest_array == NULL)
    {
        dprintf(log_fd, "Corrupt profile manifest: a service or type is missing!\n");
        status = FALSE;
    }
    else
    {
        if(!unlock_services(log_fd, profile_manifest_array))
        {
            dprintf(log_fd, "Failed to send unlock notification to old services!\n");
            status = FALSE;
        }
    }
    
    if(!unlock_profile(log_fd, tmpdir, profile))
        status = FALSE; /* There was no lock -> fail */
    
    return status;
}

int run_disnix_activity(Operation operation, gchar **derivation, const unsigned int flags, char *profile, gchar **arguments, char *type, char *container, char *component, int keep, char *command)
{
    int exit_status = 0;
    ProcReact_Status status;
    gchar *tmpdir;
    int temp_fd;
    pid_t pid;
    gchar *tempfilename;
    GPtrArray *profile_manifest_array;

    /* Determine the temp directory */
    tmpdir = getenv("TMPDIR");

    if(tmpdir == NULL)
        tmpdir = "/tmp";

    /* Execute operation */

    switch(operation)
    {
        case OP_IMPORT:
            if(derivation[0] == NULL)
            {
                g_printerr("ERROR: A Nix store component has to be specified!\n");
                exit_status = 1;
            }
            else
                exit_status = procreact_wait_for_exit_status(pkgmgmt_import_closure(derivation[0], 1, 2), &status);

            break;
        case OP_EXPORT:
            tempfilename = pkgmgmt_export_closure(tmpdir, derivation, 2, &pid, &temp_fd);
            g_print("%s\n", tempfilename);
            g_free(tempfilename);
            break;
        case OP_PRINT_INVALID:
            exit_status = print_strv(pkgmgmt_print_invalid_packages(derivation, 2));
            break;
        case OP_REALISE:
            exit_status = print_strv(pkgmgmt_realise(derivation, 2));
            break;
        case OP_SET:
            exit_status = procreact_wait_for_exit_status(pkgmgmt_set_profile((gchar*)profile, derivation[0], 1, 2), &status);
            break;
        case OP_QUERY_INSTALLED:
            profile_manifest_array = create_profile_manifest_array_from_current_deployment(LOCALSTATEDIR, (gchar*)profile);

            if(profile_manifest_array == NULL)
            {
                g_printerr("Cannot query installed services!\n");
                exit_status = 1;
            }
            else
            {
                print_text_from_profile_manifest_array(profile_manifest_array, 1);
                delete_profile_manifest_array(profile_manifest_array);
                exit_status = 0;
            }
            break;
        case OP_QUERY_REQUISITES:
            exit_status = print_strv(pkgmgmt_query_requisites(derivation, 2));
            break;
        case OP_COLLECT_GARBAGE:
            exit_status = procreact_wait_for_exit_status(pkgmgmt_collect_garbage(flags & FLAG_DELETE_OLD, 1, 2), &status);
            break;
        case OP_ACTIVATE:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_run_dysnomia_activity((gchar*)type, "activate", derivation[0], (gchar*)container, arguments, 1, 2), &status);
            break;
        case OP_DEACTIVATE:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_run_dysnomia_activity((gchar*)type, "deactivate", derivation[0], (gchar*)container, arguments, 1, 2), &status);
            break;
        case OP_DELETE_STATE:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_run_dysnomia_activity((gchar*)type, "collect-garbage", derivation[0], (gchar*)container, arguments, 1, 2), &status);
            break;
        case OP_SNAPSHOT:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_run_dysnomia_activity((gchar*)type, "snapshot", derivation[0], (gchar*)container, arguments, 1, 2), &status);
            break;
        case OP_RESTORE:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_run_dysnomia_activity((gchar*)type, "restore", derivation[0], (gchar*)container, arguments, 1, 2), &status);
            break;
        case OP_LOCK:
            profile_manifest_array = create_profile_manifest_array_from_current_deployment(LOCALSTATEDIR, (gchar*)profile);

            if(profile_manifest_array == NULL)
            {
                dprintf(2, "Corrupt profile manifest: a service or type is missing!\n");
                exit_status = 1;
            }
            else
            {
                exit_status = !acquire_locks(2, tmpdir, profile_manifest_array, profile);
                delete_profile_manifest_array(profile_manifest_array);
            }
            break;
        case OP_UNLOCK:
            profile_manifest_array = create_profile_manifest_array_from_current_deployment(LOCALSTATEDIR, (gchar*)profile);

            if(profile_manifest_array == NULL)
            {
                dprintf(2, "Corrupt profile manifest: a service or type is missing!\n");
                exit_status = 1;
            }
            else
            {
                exit_status = !release_locks(2, tmpdir, profile_manifest_array, profile);
                delete_profile_manifest_array(profile_manifest_array);
            }
            break;
        case OP_QUERY_ALL_SNAPSHOTS:
            exit_status = print_strv(statemgmt_query_all_snapshots((gchar*)container, (gchar*)component, 2));
            break;
        case OP_QUERY_LATEST_SNAPSHOT:
            exit_status = print_strv(statemgmt_query_latest_snapshot((gchar*)container, (gchar*)component, 2));
            break;
        case OP_PRINT_MISSING_SNAPSHOTS:
            exit_status = print_strv(statemgmt_print_missing_snapshots((gchar**)derivation, 2));
            break;
        case OP_IMPORT_SNAPSHOTS:
            if(derivation[0] == NULL)
            {
                g_printerr("ERROR: A Dysnomia snapshot has to be specified!\n");
                exit_status = 1;
            }
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_import_snapshots((gchar*)container, (gchar*)component, derivation, 1, 2), &status);

            break;
        case OP_RESOLVE_SNAPSHOTS:
            if(derivation[0] == NULL)
            {
                g_printerr("ERROR: A Dysnomia snapshot has to be specified!\n");
                exit_status = 1;
            }
            else
                exit_status = print_strv(statemgmt_resolve_snapshots(derivation, 2));

            break;
        case OP_CLEAN_SNAPSHOTS:
            if(container == NULL)
                container = "";

            if(component == NULL)
                component = "";

            exit_status = procreact_wait_for_exit_status(statemgmt_clean_snapshots(keep, container, component, 1, 2), &status);
            break;
        case OP_CAPTURE_CONFIG:
            tempfilename = statemgmt_capture_config(tmpdir, 2, &pid, &temp_fd);
            g_print("%s\n", tempfilename);
            g_free(tempfilename);
            break;
        case OP_SHELL:
            container = check_dysnomia_activity_parameters(type, derivation, container, arguments);

            if(container == NULL)
                exit_status = 1;
            else
                exit_status = procreact_wait_for_exit_status(statemgmt_spawn_dysnomia_shell((gchar*)type, derivation[0], (gchar*)container, arguments, command), &status);
            break;
        case OP_NONE:
            g_printerr("ERROR: No operation specified!\n");
            exit_status = 1;
            break;
    }

    /* Cleanup */
    if(derivation != NULL)
        g_strfreev(derivation);
    if(arguments != NULL)
        g_strfreev(arguments);

    return exit_status;
}
