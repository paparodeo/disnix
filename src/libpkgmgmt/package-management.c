/*
 * Disnix - A Nix-based distributed service deployment tool
 * Copyright (C) 2008-2022  Sander van der Burg
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

#include "package-management.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

#define BUFFER_SIZE 1024
#define NIX_STORE_CMD "nix-store"
#define NIX_COLLECT_GARBAGE_CMD "nix-collect-garbage"
#define NIX_ENV_CMD "nix-env"

#define RESOLVED_PATH_MAX_SIZE 4096

pid_t pkgmgmt_import_closure(const char *closure, int stdout_fd, int stderr_fd)
{
    int closure_fd = open(closure, O_RDONLY);

    if(closure_fd == -1)
    {
        g_printerr("Cannot open closure file: [%s]\n", closure);
        return -1;
    }
    else
    {
        pid_t pid = fork();

        if(pid == 0)
        {
            char *const args[] = {NIX_STORE_CMD, "--import", NULL};

            dup2(closure_fd, 0);
            dup2(stdout_fd, 1);
            dup2(stderr_fd, 2);
            execvp(args[0], args);
            _exit(1);
        }

        return pid;
    }
}

ProcReact_bool pkgmgmt_import_closure_sync(const char *closure, int stdout_fd, int stderr_fd)
{
    ProcReact_Status status;
    pid_t pid = pkgmgmt_import_closure(closure, stdout_fd, stderr_fd);
    int exit_status = procreact_wait_for_boolean(pid, &status);
    return (status == PROCREACT_STATUS_OK && exit_status);
}

gchar *pkgmgmt_export_closure(gchar *tmpdir, gchar **paths, const unsigned int paths_length, int stderr_fd, pid_t *pid, int *temp_fd)
{
    gchar *tempfilename = g_strconcat(tmpdir, "/disnix.XXXXXX", NULL);
    *temp_fd = mkstemp(tempfilename);

    if(*temp_fd == -1)
    {
        g_free(tempfilename);
        return NULL;
    }
    else
    {
        *pid = fork();

        if(*pid == 0)
        {
            unsigned int i;
            gchar **args = (char**)g_malloc((3 + paths_length) * sizeof(gchar*));

            args[0] = NIX_STORE_CMD;
            args[1] = "--export";

            for(i = 0; i < paths_length; i++)
                args[i + 2] = paths[i];

            args[i + 2] = NULL;

            dup2(*temp_fd, 1);
            dup2(stderr_fd, 2);
            execvp(args[0], args);
            _exit(1);
        }

        return tempfilename;
    }
}

gchar *pkgmgmt_export_closure_sync(gchar *tmpdir, gchar **paths, const unsigned int paths_length, int stderr_fd)
{
    pid_t pid;
    int temp_fd;
    char *tempfile = pkgmgmt_export_closure(tmpdir, paths, paths_length, stderr_fd, &pid, &temp_fd);
    ProcReact_Status status;
    int exit_status = procreact_wait_for_boolean(pid, &status);
    close(temp_fd);

    if(status == PROCREACT_STATUS_OK && exit_status)
        return tempfile;
    else
    {
        g_free(tempfile);
        return NULL;
    }
}

ProcReact_Future pkgmgmt_print_invalid_packages(gchar **paths, const unsigned int paths_length, int stderr_fd)
{
    ProcReact_Future future = procreact_initialize_future(procreact_create_string_array_type('\n'));

    if(future.pid == 0)
    {
        unsigned int i;
        gchar **args = (char**)g_malloc((4 + paths_length) * sizeof(gchar*));

        args[0] = NIX_STORE_CMD;
        args[1] = "--check-validity";
        args[2] = "--print-invalid";

        for(i = 0; i < paths_length; i++)
            args[i + 3] = paths[i];

        args[i + 3] = NULL;

        dup2(future.fd, 1); /* Attach write-end to stdout */
        dup2(stderr_fd, 2); /* Attach logger to stderr */
        execvp(args[0], args);
        _exit(1);
    }

    return future;
}

char **pkgmgmt_print_invalid_packages_sync(gchar **paths, const unsigned int paths_length, int stderr_fd)
{
    ProcReact_Status status;
    ProcReact_Future future = pkgmgmt_print_invalid_packages(paths, paths_length, stderr_fd);
    char **result = procreact_future_get(&future, &status);

    if(status == PROCREACT_STATUS_OK)
        return result;
    else
        return NULL;
}

ProcReact_Future pkgmgmt_realise(gchar **derivation_paths, const unsigned int derivation_paths_length, int stderr_fd)
{
    ProcReact_Future future = procreact_initialize_future(procreact_create_string_array_type('\n'));

    if(future.pid == 0)
    {
        unsigned int i;
        gchar **args = (gchar**)g_malloc((3 + derivation_paths_length) * sizeof(gchar*));

        args[0] = NIX_STORE_CMD;
        args[1] = "-r";

        for(i = 0; i < derivation_paths_length; i++)
            args[i + 2] = derivation_paths[i];

        args[i + 2] = NULL;

        dup2(future.fd, 1); /* Attach write-end to stdout */
        dup2(stderr_fd, 2); /* Attach logger to stderr */
        execvp(args[0], args);
        _exit(1);
    }

    return future;
}

static gchar *determine_profile_dir(void)
{
    if(getuid() == 0)
        return g_strdup(LOCALSTATEDIR "/nix/profiles/disnix");
    else
    {
        char *username = (getpwuid(geteuid()))->pw_name;
        return g_strconcat(LOCALSTATEDIR "/nix/profiles/per-user/", username, "/disnix", NULL);
    }
}

pid_t pkgmgmt_set_profile(gchar *profile, gchar *path, int stdout_fd, int stderr_fd)
{
    gchar *profile_path;
    ssize_t resolved_path_size;
    char resolved_path[BUFFER_SIZE];
    pid_t pid;
    gchar *profile_dir = determine_profile_dir();

    if(mkdir(profile_dir, 0755) == -1 && errno != EEXIST)
    {
        g_printerr("Cannot create directory: %s\n", profile_dir);
        return -1;
    }

    profile_path = g_strconcat(profile_dir, "/", profile, NULL);

    /* Resolve the manifest file to which the disnix profile points */
    resolved_path_size = readlink(profile_path, resolved_path, BUFFER_SIZE);

    if(resolved_path_size != -1 && (strlen(profile_path) != resolved_path_size || strncmp(resolved_path, profile_path, resolved_path_size) != 0)) /* If the symlink resolves not to itself, we get a generation symlink that we must resolve again */
    {
        gchar *generation_path;

        resolved_path[resolved_path_size] = '\0';

        generation_path = g_strconcat(profile_dir, "/", resolved_path, NULL);
        resolved_path_size = readlink(generation_path, resolved_path, BUFFER_SIZE);

        g_free(generation_path);
    }

    pid = fork();

    if(pid == 0)
    {
        if(resolved_path_size == -1 || (strlen(path) == resolved_path_size && strncmp(resolved_path, path, resolved_path_size) != 0)) /* Only configure the configurator profile if the given manifest is not identical to the previous manifest */
        {
            char *const args[] = {NIX_ENV_CMD, "-p", profile_path, "--set", path, NULL};
            dup2(stdout_fd, 1);
            dup2(stderr_fd, 2);
            execvp(args[0], args);
            dprintf(stderr_fd, "Error with executing nix-env\n");
            _exit(1);
        }
        else
            _exit(0);
    }

    g_free(profile_path);
    g_free(profile_dir);
    return pid;
}

ProcReact_Future pkgmgmt_query_requisites(gchar **paths, const unsigned int paths_length, int stderr_fd)
{
    ProcReact_Future future = procreact_initialize_future(procreact_create_string_array_type('\n'));

    if(future.pid == 0)
    {
        unsigned int i;
        char **args = (char**)g_malloc((3 + paths_length) * sizeof(char*));

        args[0] = NIX_STORE_CMD;
        args[1] = "-qR";

        for(i = 0; i < paths_length; i++)
            args[i + 2] = paths[i];

        args[i + 2] = NULL;

        dup2(future.fd, 1);
        dup2(stderr_fd, 2);
        execvp(args[0], args);
        _exit(1);
    }

    return future;
}

char **pkgmgmt_query_requisites_sync(gchar **paths, const unsigned int paths_length, int stderr_fd)
{
    ProcReact_Future future = pkgmgmt_query_requisites(paths, paths_length, stderr_fd);
    ProcReact_Status status;
    char **result = procreact_future_get(&future, &status);

    if(status == PROCREACT_STATUS_OK)
        return result;
    else
        return NULL;
}

pid_t pkgmgmt_collect_garbage(const ProcReact_bool delete_old, int stdout_fd, int stderr_fd)
{
    pid_t pid = fork();

    if(pid == 0)
    {
        dup2(stdout_fd, 1);
        dup2(stderr_fd, 2);

        if(delete_old)
        {
            char *const args[] = {NIX_COLLECT_GARBAGE_CMD, "-d", NULL};
            execvp(args[0], args);
        }
        else
        {
            char *const args[] = {NIX_COLLECT_GARBAGE_CMD, NULL};
            execvp(args[0], args);
        }

        dprintf(stderr_fd, "Error with executing garbage collect process\n");
        _exit(1);
    }

    return pid;
}

ProcReact_Future pkgmgmt_normalize_infrastructure(gchar *infrastructure_expr, gchar *default_target_property, gchar *default_client_interface)
{
    ProcReact_Future future = procreact_initialize_future(procreact_create_string_type());

    if(future.pid == 0)
    {
        char *const args[] = {"disnix-normalize-infra", "--target-property", default_target_property, "--interface", default_client_interface, "--raw", infrastructure_expr, NULL};
        dup2(future.fd, 1); /* Attach write-end to stdout */
        execvp(args[0], args);
        _exit(1);
    }

    return future;
}

char *pkgmgmt_normalize_infrastructure_sync(gchar *infrastructure_expr, gchar *default_target_property, gchar *default_client_interface)
{
    ProcReact_Status status;
    ProcReact_Future future = pkgmgmt_normalize_infrastructure(infrastructure_expr, default_target_property, default_client_interface);
    return procreact_future_get(&future, &status);
}

static pid_t execute_set_coordinator_profile(gchar *profile_path, gchar *manifest_file_path)
{
    pid_t pid = fork();

    if(pid == 0)
    {
        char *const args[] = {NIX_ENV_CMD, "-p", profile_path, "--set", manifest_file_path, NULL};
        execvp(args[0], args);
        _exit(1);
    }

    return pid;
}

static gchar *create_user_profile_dir(void)
{
    /* Get current username */
    char *username = (getpwuid(geteuid()))->pw_name;
    char *user_profile_dir = g_strconcat(LOCALSTATEDIR "/nix/profiles/per-user/", username, NULL);

    if(mkdir(user_profile_dir, 0755) == -1 && errno != EEXIST)
    {
        g_printerr("Cannot create directory: %s\n", user_profile_dir);
        g_free(user_profile_dir);
        return NULL;
    }
    else
        return user_profile_dir;
}

static gchar *compose_coordinator_profile_basedir(const gchar *coordinator_profile_path)
{
    if(coordinator_profile_path == NULL)
    {
        gchar *user_profile_dir = create_user_profile_dir();

        if(user_profile_dir == NULL)
            return NULL;
        else
        {
            gchar *result = g_strconcat(user_profile_dir, "/disnix-coordinator", NULL);
            g_free(user_profile_dir);
            return result;
        }
    }
    else
        return g_strdup(coordinator_profile_path);
}

/*
 * If the manifest file is an absolute path or a relative path starting
 * with ./ then the path is OK
 */
static gchar *normalize_manifest_path(const gchar *manifest_file)
{
    if((strlen(manifest_file) >= 1 && manifest_file[0] == '/') ||
       (strlen(manifest_file) >= 2 && (manifest_file[0] == '.' || manifest_file[1] == '/')))
        return g_strdup(manifest_file); /* If path is absolute or prefixed with ./ just use it */
    else
        return g_strconcat("./", manifest_file, NULL); /* Otherwise add ./ in front of the path */
}

static int compare_profile_paths(const gchar *profile_path, char *resolved_path, ssize_t resolved_path_size)
{
    return (strlen(profile_path) == resolved_path_size && strncmp(resolved_path, profile_path, resolved_path_size) == 0);
}

static ssize_t resolve_profile_symlink(gchar *profile_path, gchar *profile_base_dir, char *resolved_path)
{
    ssize_t resolved_path_size = readlink(profile_path, resolved_path, RESOLVED_PATH_MAX_SIZE);

    if(resolved_path_size == -1)
        return -1;
    else
    {
        if(!compare_profile_paths(profile_path, resolved_path, resolved_path_size)) /* If the symlink resolves not to itself, we get a generation symlink that we must resolve again */
        {
            gchar *generation_path;

            resolved_path[resolved_path_size] = '\0';
            generation_path = g_strconcat(profile_base_dir, "/", resolved_path, NULL);
            resolved_path_size = readlink(generation_path, resolved_path, RESOLVED_PATH_MAX_SIZE);

            g_free(generation_path);
        }

        return resolved_path_size;
    }
}

ProcReact_bool pkgmgmt_set_coordinator_profile(const gchar *coordinator_profile_path, const gchar *manifest_file, const gchar *profile)
{
    gchar *profile_base_dir, *profile_path;
    char resolved_path[RESOLVED_PATH_MAX_SIZE];
    ssize_t resolved_path_size;
    ProcReact_bool exit_status;

    /* Determine which profile path to use, if a coordinator profile path is given use this value otherwise the default */
    profile_base_dir = compose_coordinator_profile_basedir(coordinator_profile_path);

    if(profile_base_dir == NULL)
        return FALSE;

    /* Create the profile directory */
    if(mkdir(profile_base_dir, 0755) == -1 && errno != EEXIST)
    {
        g_printerr("[coordinator]: Cannot create profile directory: %s\n", profile_base_dir);
        g_free(profile_base_dir);
        return FALSE;
    }

    /* Determine the path to the profile */
    profile_path = g_strconcat(profile_base_dir, "/", profile, NULL);

    /* Resolve the manifest file to which the coordinator profile points */
    resolved_path_size = resolve_profile_symlink(profile_path, profile_base_dir, resolved_path);

    if(resolved_path_size == -1 || !compare_profile_paths(manifest_file, resolved_path, resolved_path_size)) /* Only configure the configurator profile if the given manifest is not identical to the previous manifest */
    {
        /*
         * Execute nix-env --set operation to change the coordinator profile so
         * that the new configuration is known
         */

        gchar *manifest_file_path = normalize_manifest_path(manifest_file); /* Normalize manifest file path */
        pid_t pid = execute_set_coordinator_profile(profile_path, manifest_file_path);
        ProcReact_Status status;
        int result = procreact_wait_for_boolean(pid, &status);

        /* If the process suceeds the the operation succeeded */
        exit_status = (status == PROCREACT_STATUS_OK && result);

        /* Cleanup */
        g_free(manifest_file_path);
    }
    else
        exit_status = TRUE;

    /* Cleanup */
    g_free(profile_base_dir);
    g_free(profile_path);

    /* Return status */
    return exit_status;
}
