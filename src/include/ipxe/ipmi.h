#ifndef _IPXE_IPMI_H
#define _IPXE_IPMI_H

/** @file
 *
 * Intelligent Platform Management Interface
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <stddef.h>
#include <ipxe/tables.h>

/** An IPMI request */
struct ipmi_request {
	/** Network function and LUN */
	uint8_t netfn_lun;
	/** Command */
	uint8_t command;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** An IPMI response */
struct ipmi_response {
	/** Network function and LUN */
	uint8_t netfn_lun;
	/** Command */
	uint8_t command;
	/** Completion code */
	uint8_t code;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** Construct network function and LUN byte */
#define IPMI_NETFN_LUN( netfn, lun ) ( ( (netfn) << 2 ) | (lun) )

/** Extract network function */
#define IPMI_NETFN( netfn_lun ) ( (netfn_lun) >> 2 )

/** Chassis network function */
#define IPMI_NETFN_CHASSIS 0x00

/** Application network function */
#define IPMI_NETFN_APP 0x06

/** Transport network function */
#define IPMI_NETFN_TRANSPORT 0x0c

/** Group extension network function */
#define IPMI_NETFN_GROUP 0x2c

/** Get Device ID command */
#define IPMI_GET_DEVICE_ID 0x01

/** Cold Reset command */
#define IPMI_COLD_RESET 0x02

/** Warm Reset command */
#define IPMI_WARM_RESET 0x03

/** Get Channel Info command */
#define IPMI_GET_CHANNEL_INFO 0x42

/** Get Channel Info response */
struct ipmi_channel_info {
	/** Channel number */
	uint8_t channel;
	/** Channel medium type */
	uint8_t medium;
	/** Channel protocol type */
	uint8_t protocol;
	/** Session support */
	uint8_t session;
	/** Vendor ID */
	uint8_t vendor[3];
	/** Auxiliary channel information */
	uint8_t aux[2];
} __attribute__ (( packed ));

/** 802.3 LAN channel medium type */
#define IPMI_MEDIUM_LAN 0x04

/** Maximum channel number */
#define IPMI_CHANNEL_MAX 0x0b

/** Get Chassis Status command */
#define IPMI_GET_CHASSIS_STATUS 0x01

/** Chassis Status system power is on */
#define IPMI_CHASSIS_POWER_ON 0x01

/** Chassis Control command */
#define IPMI_CHASSIS_CONTROL 0x02

/** Chassis Control actions */
#define IPMI_CHASSIS_POWER_DOWN 0x00
#define IPMI_CHASSIS_POWER_UP 0x01
#define IPMI_CHASSIS_POWER_CYCLE 0x02
#define IPMI_CHASSIS_HARD_RESET 0x03
#define IPMI_CHASSIS_SOFT_SHUTDOWN 0x05

/** Set System Boot Options command */
#define IPMI_SET_SYSTEM_BOOT_OPTIONS 0x08

/** Boot option parameter: boot flags */
#define IPMI_BOOT_OPTION_BOOT_FLAGS 0x05

/** Set System Boot Options boot flags request */
struct ipmi_boot_flags_request {
	/** Parameter selector */
	uint8_t param;
	/** Boot flags validity */
	uint8_t valid;
	/** Boot device selector */
	uint8_t device;
	/** BIOS verbosity and console redirection */
	uint8_t bios;
	/** BIOS overrides */
	uint8_t overrides;
	/** Device instance selector */
	uint8_t instance;
} __attribute__ (( packed ));

/** Boot flags are valid */
#define IPMI_BOOT_FLAGS_VALID 0x80

/** Boot flags apply to all future boots (rather than the next boot only) */
#define IPMI_BOOT_FLAGS_PERSISTENT 0x40

/** Boot flags apply to a UEFI boot */
#define IPMI_BOOT_FLAGS_UEFI 0x20

/** Construct boot device selector */
#define IPMI_BOOT_DEVICE( device ) ( (device) << 2 )

/** Boot device selectors */
#define IPMI_BOOT_DEVICE_NONE 0x0
#define IPMI_BOOT_DEVICE_PXE 0x1
#define IPMI_BOOT_DEVICE_DISK 0x2
#define IPMI_BOOT_DEVICE_CDROM 0x5
#define IPMI_BOOT_DEVICE_BIOS 0x6
#define IPMI_BOOT_DEVICE_FLOPPY 0xf

/** Set LAN Configuration Parameters command */
#define IPMI_SET_LAN_CONFIG 0x01

/** Get LAN Configuration Parameters command */
#define IPMI_GET_LAN_CONFIG 0x02

/** LAN parameter: set in progress */
#define IPMI_LAN_PARAM_SET_IN_PROGRESS 0

/** Set in progress states */
#define IPMI_LAN_SET_COMPLETE 0x00
#define IPMI_LAN_SET_IN_PROGRESS 0x01

/** LAN parameter: IP address */
#define IPMI_LAN_PARAM_IP 3

/** LAN parameter: IP address source */
#define IPMI_LAN_PARAM_IP_SOURCE 4

/** LAN parameter: MAC address */
#define IPMI_LAN_PARAM_MAC 5

/** LAN parameter: subnet mask */
#define IPMI_LAN_PARAM_NETMASK 6

/** LAN parameter: default gateway address */
#define IPMI_LAN_PARAM_GATEWAY 12

/** LAN parameter: VLAN ID */
#define IPMI_LAN_PARAM_VLAN 20

/** VLAN ID is enabled */
#define IPMI_LAN_VLAN_ENABLED 0x80

/** Get Device ID response */
struct ipmi_device_id {
	/** Device ID */
	uint8_t id;
	/** Device revision */
	uint8_t revision;
	/** Firmware major revision */
	uint8_t fw_major;
	/** Firmware minor revision (BCD) */
	uint8_t fw_minor;
	/** IPMI specification version (BCD) */
	uint8_t version;
	/** Additional device support */
	uint8_t support;
	/** Manufacturer ID */
	uint8_t mfg[3];
	/** Product ID */
	uint8_t product[2];
} __attribute__ (( packed ));

/** Command completed normally */
#define IPMI_CC_OK 0x00

/** Node busy */
#define IPMI_CC_BUSY 0xc0

/** Invalid command */
#define IPMI_CC_INVALID_COMMAND 0xc1

/** Timeout while processing command */
#define IPMI_CC_TIMEOUT 0xc3

/** Maximum length of IPMI message data
 *
 * The KCS system interface is specified to allow message lengths of
 * at least 40 bytes.  All messages that we send or expect to receive
 * are substantially shorter than this.
 */
#define IPMI_MAX_DATA 32

/** An IPMI system interface transport */
struct ipmi_transport {
	/** Name */
	const char *name;
	/**
	 * Open transport
	 *
	 * @ret rc		Return status code
	 */
	int ( * open ) ( void );
	/**
	 * Transfer message
	 *
	 * @v request		IPMI request
	 * @v request_len	Length of IPMI request
	 * @v response		IPMI response buffer
	 * @v response_max	Length of IPMI response buffer
	 * @ret response_len	Length of IPMI response, or negative error
	 */
	int ( * xfer ) ( const struct ipmi_request *request,
			 size_t request_len, struct ipmi_response *response,
			 size_t response_max );
};

/** IPMI system interface transport table */
#define IPMI_TRANSPORTS __table ( struct ipmi_transport, "ipmi_transports" )

/** Declare an IPMI system interface transport */
#define __ipmi_transport __table_entry ( IPMI_TRANSPORTS, 01 )

extern int ipmi_open ( void );
extern int ipmi_message ( unsigned int netfn, unsigned int command,
			  const void *data, size_t len, void *response,
			  size_t response_max );

#endif /* _IPXE_IPMI_H */
