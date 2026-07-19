/*
 * Copyright (C) 2026 Kris Shannon <kris@shannon.id.au>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

/** @file
 *
 * IPMI commands
 *
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <ipxe/command.h>
#include <ipxe/parseopt.h>
#include <ipxe/ipmi.h>

/** "bmcreset" options */
struct bmcreset_options {
	/** Perform a warm reset */
	int warm;
};

/** "bmcreset" option list */
static struct option_descriptor bmcreset_opts[] = {
	OPTION_DESC ( "warm", 'w', no_argument,
		      struct bmcreset_options, warm, parse_flag ),
};

/** "bmcreset" command descriptor */
static struct command_descriptor bmcreset_cmd =
	COMMAND_DESC ( struct bmcreset_options, bmcreset_opts, 0, 0, NULL );

/**
 * The "bmcreset" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int bmcreset_exec ( int argc, char **argv ) {
	struct bmcreset_options opts;
	unsigned int command;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &bmcreset_cmd, &opts ) ) != 0 )
		return rc;

	/* Reset BMC */
	command = ( opts.warm ? IPMI_WARM_RESET : IPMI_COLD_RESET );
	if ( ( rc = ipmi_message ( IPMI_NETFN_APP, command, NULL, 0,
				   NULL, 0 ) ) < 0 ) {
		printf ( "Could not reset BMC: %s\n", strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** "bmcpower" options */
struct bmcpower_options {};

/** "bmcpower" option list */
static struct option_descriptor bmcpower_opts[] = {};

/** "bmcpower" command descriptor */
static struct command_descriptor bmcpower_cmd =
	COMMAND_DESC ( struct bmcpower_options, bmcpower_opts, 0, 1,
		       "[on|off|cycle|reset|soft]" );

/** A chassis power action */
struct bmcpower_action {
	/** Action name */
	const char *name;
	/** Chassis control action */
	uint8_t action;
};

/** Chassis power actions */
static struct bmcpower_action bmcpower_actions[] = {
	{ "off", IPMI_CHASSIS_POWER_DOWN },
	{ "on", IPMI_CHASSIS_POWER_UP },
	{ "cycle", IPMI_CHASSIS_POWER_CYCLE },
	{ "reset", IPMI_CHASSIS_HARD_RESET },
	{ "soft", IPMI_CHASSIS_SOFT_SHUTDOWN },
};

/**
 * The "bmcpower" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int bmcpower_exec ( int argc, char **argv ) {
	struct bmcpower_options opts;
	struct bmcpower_action *action = NULL;
	uint8_t status[3];
	unsigned int i;
	int len;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &bmcpower_cmd, &opts ) ) != 0 )
		return rc;

	/* With no action, print current power state */
	if ( optind == argc ) {
		len = ipmi_message ( IPMI_NETFN_CHASSIS,
				     IPMI_GET_CHASSIS_STATUS, NULL, 0,
				     status, sizeof ( status ) );
		if ( len < 0 ) {
			rc = len;
			printf ( "Could not get chassis status: %s\n",
				 strerror ( rc ) );
			return rc;
		}
		if ( ( ( size_t ) len ) < sizeof ( status ) )
			return -EPROTO;
		printf ( "Chassis power is %s\n",
			 ( ( status[0] & IPMI_CHASSIS_POWER_ON ) ?
			   "on" : "off" ) );
		return 0;
	}

	/* Identify action */
	for ( i = 0 ; i < ( sizeof ( bmcpower_actions ) /
			    sizeof ( bmcpower_actions[0] ) ) ; i++ ) {
		if ( strcmp ( argv[optind],
			      bmcpower_actions[i].name ) == 0 ) {
			action = &bmcpower_actions[i];
			break;
		}
	}
	if ( ! action ) {
		printf ( "Unrecognised power action \"%s\"\n", argv[optind] );
		return -EINVAL;
	}

	/* Perform action */
	if ( ( rc = ipmi_message ( IPMI_NETFN_CHASSIS, IPMI_CHASSIS_CONTROL,
				   &action->action,
				   sizeof ( action->action ),
				   NULL, 0 ) ) < 0 ) {
		printf ( "Could not control chassis power: %s\n",
			 strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** "bootdev" options */
struct bootdev_options {
	/** Apply to all future boots */
	int persistent;
	/** Apply to a UEFI boot */
	int uefi;
};

/** "bootdev" option list */
static struct option_descriptor bootdev_opts[] = {
	OPTION_DESC ( "persistent", 'p', no_argument,
		      struct bootdev_options, persistent, parse_flag ),
	OPTION_DESC ( "uefi", 'u', no_argument,
		      struct bootdev_options, uefi, parse_flag ),
};

/** "bootdev" command descriptor */
static struct command_descriptor bootdev_cmd =
	COMMAND_DESC ( struct bootdev_options, bootdev_opts, 1, 1,
		       "none|pxe|disk|cdrom|bios|floppy" );

/** A boot device */
struct bootdev_device {
	/** Device name */
	const char *name;
	/** Boot device selector */
	uint8_t device;
};

/** Boot devices */
static struct bootdev_device bootdev_devices[] = {
	{ "none", IPMI_BOOT_DEVICE_NONE },
	{ "pxe", IPMI_BOOT_DEVICE_PXE },
	{ "disk", IPMI_BOOT_DEVICE_DISK },
	{ "cdrom", IPMI_BOOT_DEVICE_CDROM },
	{ "bios", IPMI_BOOT_DEVICE_BIOS },
	{ "floppy", IPMI_BOOT_DEVICE_FLOPPY },
};

/**
 * The "bootdev" command
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
static int bootdev_exec ( int argc, char **argv ) {
	struct bootdev_options opts;
	struct bootdev_device *device = NULL;
	struct ipmi_boot_flags_request flags;
	unsigned int i;
	int rc;

	/* Parse options */
	if ( ( rc = parse_options ( argc, argv, &bootdev_cmd, &opts ) ) != 0 )
		return rc;

	/* Identify boot device */
	for ( i = 0 ; i < ( sizeof ( bootdev_devices ) /
			    sizeof ( bootdev_devices[0] ) ) ; i++ ) {
		if ( strcmp ( argv[optind],
			      bootdev_devices[i].name ) == 0 ) {
			device = &bootdev_devices[i];
			break;
		}
	}
	if ( ! device ) {
		printf ( "Unrecognised boot device \"%s\"\n", argv[optind] );
		return -EINVAL;
	}

	/* Set boot flags */
	memset ( &flags, 0, sizeof ( flags ) );
	flags.param = IPMI_BOOT_OPTION_BOOT_FLAGS;
	flags.valid = IPMI_BOOT_FLAGS_VALID;
	if ( opts.persistent )
		flags.valid |= IPMI_BOOT_FLAGS_PERSISTENT;
	if ( opts.uefi )
		flags.valid |= IPMI_BOOT_FLAGS_UEFI;
	flags.device = IPMI_BOOT_DEVICE ( device->device );
	if ( ( rc = ipmi_message ( IPMI_NETFN_CHASSIS,
				   IPMI_SET_SYSTEM_BOOT_OPTIONS, &flags,
				   sizeof ( flags ), NULL, 0 ) ) < 0 ) {
		printf ( "Could not set boot device: %s\n", strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** IPMI commands */
COMMAND ( bmcreset, bmcreset_exec );
COMMAND ( bmcpower, bmcpower_exec );
COMMAND ( bootdev, bootdev_exec );
