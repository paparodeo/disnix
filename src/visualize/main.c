/*
 * Disnix - A Nix-based distributed service deployment tool
 * Copyright (C) 2008-2020  Sander van der Burg
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

#include <stdio.h>
#include <getopt.h>
#include <defaultoptions.h>
#include "graph.h"

static void print_usage(const char *command)
{
    printf("Usage: %s [OPTION] [MANIFEST]\n\n", command);

    puts(
    "The command `disnix-visualize' generates a graph showing services (as nodes),\n"
    "inter-dependencies (as arrows) and target machines (as clusters) from a manifest\n"
    "file generated by `disnix-manifest'. If no manifest file is given, it uses the\n"
    "manifest of the last deployed configuration.\n\n"

    "The graph is exported as dot format, which can be transformed in a raster image\n"
    "format by using the `dot' command.\n\n"

    "Options:\n"
    "  -p, --profile=PROFILE  Name of the profile in which the services are\n"
    "                         registered. Defaults to: default\n"
    "      --coordinator-profile-path=PATH\n"
    "                         Path to the manifest of the previous configuration. By\n"
    "                         default this tool will use the manifest stored in the\n"
    "                         disnix coordinator profile instead of the specified\n"
    "                         one, which is usually sufficient in most cases.\n"
    "      --no-containers    Do not visualize the containers.\n"
    "  -h, --help             Shows the usage of this command to the user\n"
    "  -v, --version          Shows the version of this command to the user\n"
    );
}

int main(int argc, char *argv[])
{
    /* Declarations */
    int c, option_index = 0;
    struct option long_options[] =
    {
        {"coordinator-profile-path", required_argument, 0, DISNIX_OPTION_COORDINATOR_PROFILE_PATH},
        {"profile", required_argument, 0, DISNIX_OPTION_PROFILE},
        {"no-containers", no_argument, 0, DISNIX_OPTION_NO_CONTAINERS},
        {"help", no_argument, 0, DISNIX_OPTION_HELP},
        {"version", no_argument, 0, DISNIX_OPTION_VERSION},
        {0, 0, 0, 0}
    };
    char *profile = NULL;
    char *coordinator_profile_path = NULL;
    char *manifest_file;
    int no_containers = FALSE;

    /* Parse command-line options */
    while((c = getopt_long(argc, argv, "p:hv", long_options, &option_index)) != -1)
    {
        switch(c)
        {
            case DISNIX_OPTION_PROFILE:
                profile = optarg;
                break;
            case DISNIX_OPTION_COORDINATOR_PROFILE_PATH:
                coordinator_profile_path = optarg;
                break;
            case DISNIX_OPTION_NO_CONTAINERS:
                no_containers = TRUE;
                break;
            case DISNIX_OPTION_HELP:
                print_usage(argv[0]);
                return 0;
            case DISNIX_OPTION_VERSION:
                print_version(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Validate options */
    profile = check_profile_option(profile);

    if(optind >= argc)
        manifest_file = NULL;
    else
        manifest_file = argv[optind];

    return generate_graph(manifest_file, coordinator_profile_path, profile, no_containers); /* Execute operation */
}
