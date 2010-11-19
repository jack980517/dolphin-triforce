/*
 *	wiiuse
 *
 *	Written By:
 *		Michael Laforest	< para >
 *		Email: < thepara (--AT--) g m a i l [--DOT--] com >
 *
 *	Copyright 2006-2007
 *
 *	This file is part of wiiuse.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <sys/time.h>
#include <errno.h>

#include <sys/types.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

#include "Common.h"
#include "wiiuse_internal.h"

static int wiiuse_connect_single(struct wiimote_t* wm, char* address);

// Find a wiimote or wiimotes.
// Does not replace already found wiimotes even if they are disconnected.
// wm			An array of wiimote_t structures.
// max_wiimotes	The number of wiimote structures in wm.
// timeout		The number of seconds before the search times out.
// Returns the total number of found wiimotes.
// This function will only look for wiimote devices.
// When a device is found the address in the structures will be set.
// You can then call wiimote_connect() to connect to the found devices.
int wiiuse_find(struct wiimote_t** wm, int max_wiimotes, int timeout)
{
	int device_id;
	int device_sock;
	int found_devices;
	int found_wiimotes = 0;
	int i;

	// Count the number of already found wiimotes
	for (i = 0; i < max_wiimotes; ++i)
	{
		if (WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_DEV_FOUND))
			found_wiimotes++;
	}

	// get the id of the first bluetooth device.
	device_id = hci_get_route(NULL);
	if (device_id < 0)
	{
		perror("hci_get_route");
		return 0;
	}

	// create a socket to the device
	device_sock = hci_open_dev(device_id);
	if (device_sock < 0)
	{
		perror("hci_open_dev");
		return 0;
	}

	int try_num = 0;
	while ((try_num < timeout) && (found_wiimotes < max_wiimotes))
	{
		inquiry_info scan_info_arr[128];
		inquiry_info* scan_info = scan_info_arr;
		memset(&scan_info_arr, 0, sizeof(scan_info_arr));

		// scan for bluetooth devices for ~one second
		found_devices = hci_inquiry(device_id, 1, 128, NULL, &scan_info, IREQ_CACHE_FLUSH);
		if (found_devices < 0)
		{
			perror("hci_inquiry");
			return 0;
		}

		NOTICE_LOG(WIIMOTE, "Found %i bluetooth device(s).", found_devices);

		// display discovered devices
		for (i = 0; (i < found_devices) && (found_wiimotes < max_wiimotes); ++i)
		{
			if ((scan_info[i].dev_class[0] == WM_DEV_CLASS_0) &&
					(scan_info[i].dev_class[1] == WM_DEV_CLASS_1) &&
					(scan_info[i].dev_class[2] == WM_DEV_CLASS_2))
			{
				int new_wiimote = 1;
				int j;
				// Determine if this wiimote has already been found.
				for (j = 0; j < found_wiimotes && new_wiimote; ++j)
				{
					if (WIIMOTE_IS_SET(wm[j], WIIMOTE_STATE_DEV_FOUND) &&
							bacmp(&scan_info[i].bdaddr,&wm[j]->bdaddr) == 0)
						new_wiimote = 0;
				}

				if (new_wiimote)
				{
					// found a new device
					ba2str(&scan_info[i].bdaddr, wm[found_wiimotes]->bdaddr_str);

					NOTICE_LOG(WIIMOTE, "Found wiimote (%s) [id %i].",
					   	wm[found_wiimotes]->bdaddr_str, wm[found_wiimotes]->unid);

					wm[found_wiimotes]->bdaddr = scan_info[i].bdaddr;
					WIIMOTE_ENABLE_STATE(wm[found_wiimotes], WIIMOTE_STATE_DEV_FOUND);
					++found_wiimotes;
				}
			}
		}
		try_num++;
	}

	close(device_sock);
	return found_wiimotes;
}

// Connect to a wiimote or wiimotes once an address is known.
// wm			An array of wiimote_t structures.
// wiimotes		The number of wiimote structures in wm.
// Return the number of wiimotes that successfully connected.
// Connect to a number of wiimotes when the address is already set
// in the wiimote_t structures.  These addresses are normally set
// by the wiiuse_find() function, but can also be set manually.
int wiiuse_connect(struct wiimote_t** wm, int wiimotes)
{
	int connected = 0;
	int i = 0;

	for (; i < wiimotes; ++i)
	{
		if (!WIIMOTE_IS_SET(wm[i], WIIMOTE_STATE_DEV_FOUND))
			// if the device address is not set, skip it
			continue;

		if (wiiuse_connect_single(wm[i], NULL))
			++connected;
	}

	return connected;
}

// Connect to a wiimote with a known address.
// wm		Pointer to a wiimote_t structure.
// address	The address of the device to connect to.
// 			If NULL, use the address in the struct set by wiiuse_find().
// Return 1 on success, 0 on failure
static int wiiuse_connect_single(struct wiimote_t* wm, char* address)
{
	struct sockaddr_l2 addr;

	if (!wm || WIIMOTE_IS_CONNECTED(wm))
		return 0;

	addr.l2_family = AF_BLUETOOTH;
	bdaddr_t *bdaddr = &wm->bdaddr;
	if (address)
		// use provided address
		str2ba(address, &addr.l2_bdaddr);
	else
	{
		bdaddr_t bdaddr_any = (bdaddr_t){{0, 0, 0, 0, 0, 0}};
		if (bacmp(bdaddr, &bdaddr_any) == 0)
			return 0;
		// use address of device discovered
		addr.l2_bdaddr = *bdaddr;

	}

	// OUTPUT CHANNEL
	wm->out_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (wm->out_sock == -1)
		return 0;

	addr.l2_cid = 0;
	addr.l2_psm = htobs(WM_OUTPUT_CHANNEL);

	// connect to wiimote
	if (connect(wm->out_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		perror("connect() output sock");
		return 0;
	}

	// INPUT CHANNEL
	wm->in_sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (wm->in_sock == -1)
	{
		close(wm->out_sock);
		wm->out_sock = -1;
		return 0;
	}

	addr.l2_psm = htobs(WM_INPUT_CHANNEL);

	// connect to wiimote
	if (connect(wm->in_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		perror("connect() interrupt sock");
		close(wm->out_sock);
		wm->out_sock = -1;
		return 0;
	}

	NOTICE_LOG(WIIMOTE, "Connected to wiimote [id %i].", wm->unid);
	// do the handshake
	WIIMOTE_ENABLE_STATE(wm, WIIMOTE_STATE_CONNECTED);

	wiiuse_set_report_type(wm);

	return 1;
}

// Disconnect a wiimote.
// wm		Pointer to a wiimote_t structure.
// Note that this will not free the wiimote structure.
void wiiuse_disconnect(struct wiimote_t* wm)
{
	if (!wm || !WIIMOTE_IS_CONNECTED(wm))
		return;

	close(wm->out_sock);
	close(wm->in_sock);

	wm->out_sock = -1;
	wm->in_sock = -1;
	wm->event = WIIUSE_NONE;

	WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_CONNECTED);
	WIIMOTE_DISABLE_STATE(wm, WIIMOTE_STATE_HANDSHAKE);
}

int wiiuse_io_read(struct wiimote_t* wm)
{
	struct timeval tv;
	fd_set fds;
	int r;
	if (!wm) 
		return 0;

	// block select() for 1/2000th of a second
	tv.tv_sec = 0;
	tv.tv_usec = wm->timeout * 1000; // timeout is in Milliseconds tv_usec is in Microseconds!

	FD_ZERO(&fds);
	// only poll it if it is connected
	if (WIIMOTE_IS_SET(wm, WIIMOTE_STATE_CONNECTED))
	{
		FD_SET(wm->in_sock, &fds);
		//highest_fd = wm[i]->in_sock;
	}
	else
		// nothing to poll
		return 0;

	if (select(wm->in_sock + 1, &fds, NULL, NULL, &tv) == -1)
	{
		ERROR_LOG(WIIMOTE, "Unable to select() the wiimote interrupt socket(s).");
		perror("Error Details");
		return 0;
	}

	// if this wiimote is not connected, skip it
	if (!WIIMOTE_IS_CONNECTED(wm))
		return 0;

	if (FD_ISSET(wm->in_sock, &fds)) 
	{
		//memset(wm->event_buf, 0, sizeof(wm->event_buf));
		// read the pending message into the buffer
		r = read(wm->in_sock, wm->event_buf, sizeof(wm->event_buf));
		if (r == -1)
		{
			// error reading data
			ERROR_LOG(WIIMOTE, "Receiving wiimote data (id %i).", wm->unid);
			perror("Error Details");

			if (errno == ENOTCONN)
			{
				// this can happen if the bluetooth dongle is disconnected
				ERROR_LOG(WIIMOTE, "Bluetooth appears to be disconnected.  Wiimote unid %i will be disconnected.", wm->unid);
				wiiuse_disconnect(wm);
				wm->event = WIIUSE_UNEXPECTED_DISCONNECT;
			}

			return 0;
		}
		if (!r)
		{
			// remote disconnect
			wiiuse_disconnected(wm);
			return 0;
		}
		wm->event_buf[0] = 0xa2; // Make sure it's 0xa2, just in case
		return 1;
	}
	return 0;
}

int wiiuse_io_write(struct wiimote_t* wm, byte* buf, int len) 
{
	if(buf[0] == 0xa2)
		buf[0] = 0x52; // May not be needed. Will be changing/correcting in the next few revisions
	return write(wm->out_sock, buf, len);
}