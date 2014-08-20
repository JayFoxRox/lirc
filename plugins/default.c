
/****************************************************************************
 ** hw_default.c ************************************************************
 ****************************************************************************
 *
 * routines for hardware that supports ioctl() interface
 *
 * Copyright (C) 1999 Christoph Bartelmus <lirc@bartelmus.de>
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "lirc_driver.h"

extern struct ir_remote *repeat_remote;

static __u32 supported_send_modes[] = {
	/* LIRC_CAN_SEND_LIRCCODE, */
	/* LIRC_CAN_SEND_MODE2, this one would be very easy */
	LIRC_CAN_SEND_PULSE,
	/* LIRC_CAN_SEND_RAW, */
	0
};

static __u32 supported_rec_modes[] = {
	LIRC_CAN_REC_LIRCCODE,
	LIRC_CAN_REC_MODE2,
	/* LIRC_CAN_REC_PULSE, shouldn't be too hard */
	/* LIRC_CAN_REC_RAW, */
	0
};

//Forwards:
int default_init(void);
int default_config(struct ir_remote *remotes);
int default_deinit(void);
int default_send(struct ir_remote *remote, struct ir_ncode *code);
char *default_rec(struct ir_remote *remotes);
int default_ioctl(unsigned int cmd, void *arg);
lirc_t default_readdata(lirc_t timeout);



static const const struct driver hw_default = {
	.name		=	"default",
	.device		=	LIRC_DRIVER_DEVICE,
	.features	=	0,
	.send_mode	=	0,
	.rec_mode	=	0,
	.code_length	=	0,
	.init_func	=	default_init,
	.deinit_func	=	default_deinit,
	.send_func	=	default_send,
	.rec_func	=	default_rec,
	.decode_func	=	receive_decode,
	.ioctl_func	=	default_ioctl,
	.readdata	=	default_readdata,
	.api_version	=	2,
	.driver_version = 	"0.9.2"
};


const struct driver* hardwares[] = { &hw_default,  (const struct hardware*)NULL };


/**********************************************************************
 *
 * internal function prototypes
 *
 **********************************************************************/

static int write_send_buffer(int lirc);

/**********************************************************************
 *
 * decode stuff
 *
 **********************************************************************/

int default_readdata(lirc_t timeout)
{
	int data, ret;

	if (!waitfordata((long)timeout))
		return 0;

	ret = read(drv.fd, &data, sizeof(data));
	if (ret != sizeof(data)) {
		logprintf(LOG_ERR, "error reading from %s (ret %d, expected %d)",
			  drv.device, ret, sizeof(data));
		logperror(LOG_ERR, NULL);
		default_deinit();

		return 0;
	}

	if (data == 0) {
		static int data_warning = 1;

		if (data_warning) {
			logprintf(LOG_WARNING, "read invalid data from device %s", drv.device);
			data_warning = 0;
		}
		data = 1;
	}
	return data ;
}

/*
  interface functions
*/

int default_init()
{
	struct stat s;
	int i;

	/* FIXME: other modules might need this, too */
	init_rec_buffer();
	init_send_buffer();

	if (stat(drv.device, &s) == -1) {
		logprintf(LOG_ERR, "could not get file information for %s", drv.device);
		logperror(LOG_ERR, "default_init()");
		return (0);
	}

	/* file could be unix socket, fifo and native lirc device */
	if (S_ISSOCK(s.st_mode)) {
		struct sockaddr_un addr;
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, drv.device, sizeof(addr.sun_path) - 1);

		drv.fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (drv.fd == -1) {
			logprintf(LOG_ERR, "could not create socket");
			logperror(LOG_ERR, "default_init()");
			return (0);
		}

		if (connect(drv.fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
			logprintf(LOG_ERR, "could not connect to unix socket %s", drv.device);
			logperror(LOG_ERR, "default_init()");
			default_deinit();
			close(drv.fd);
			return (0);
		}

		LOGPRINTF(1, "using unix socket lirc device");
		drv.features = LIRC_CAN_REC_MODE2 | LIRC_CAN_SEND_PULSE;
		drv.rec_mode = LIRC_MODE_MODE2;	/* this might change in future */
		drv.send_mode = LIRC_MODE_PULSE;
		return (1);
	}

	if ((drv.fd = open(drv.device, O_RDWR)) < 0) {
		logprintf(LOG_ERR, "could not open %s", drv.device);
		logperror(LOG_ERR, "default_init()");
		return (0);
	}
	if (S_ISFIFO(s.st_mode)) {
		LOGPRINTF(1, "using defaults for the Irman");
		drv.features = LIRC_CAN_REC_MODE2;
		drv.rec_mode = LIRC_MODE_MODE2;	/* this might change in future */
		return (1);
	} else if (!S_ISCHR(s.st_mode)) {
		default_deinit();
		logprintf(LOG_ERR, "%s is not a character device!!!", drv.device);
		logperror(LOG_ERR, "something went wrong during installation");
		return (0);
	} else if (default_ioctl(LIRC_GET_FEATURES, &drv.features) == -1) {
		logprintf(LOG_ERR, "could not get hardware features");
		logprintf(LOG_ERR, "this device driver does not support the LIRC ioctl interface");
		if (major(s.st_rdev) == 13) {
			logprintf(LOG_ERR, "did you mean to use the devinput driver instead of the %s driver?",
				  drv.name);
		} else {
			logprintf(LOG_ERR, "major number of %s is %lu", drv.device, (__u32) major(s.st_rdev));
			logprintf(LOG_ERR, "make sure %s is a LIRC device and use a current version of the driver",
				  drv.device);
		}
		default_deinit();
		return (0);
	}
	else {
		if (!(LIRC_CAN_SEND(drv.features) || LIRC_CAN_REC(drv.features))) {
			LOGPRINTF(1, "driver supports neither sending nor receiving of IR signals");
		}
		if (LIRC_CAN_SEND(drv.features) && LIRC_CAN_REC(drv.features)) {
			LOGPRINTF(1, "driver supports both sending and receiving");
		} else if (LIRC_CAN_SEND(drv.features)) {
			LOGPRINTF(1, "driver supports sending");
		} else if (LIRC_CAN_REC(drv.features)) {
			LOGPRINTF(1, "driver supports receiving");
		}
	}

	/* set send/receive method */
	drv.send_mode = 0;
	if (LIRC_CAN_SEND(drv.features)) {
		for (i = 0; supported_send_modes[i] != 0; i++) {
			if (LIRC_CAN_SEND(drv.features) == supported_send_modes[i]) {
				drv.send_mode = LIRC_SEND2MODE(supported_send_modes[i]);
				break;
			}
		}
		if (supported_send_modes[i] == 0) {
			logprintf(LOG_NOTICE, "the send method of the driver is not yet supported by lircd");
		}
	}
	drv.rec_mode = 0;
	if (LIRC_CAN_REC(drv.features)) {
		for (i = 0; supported_rec_modes[i] != 0; i++) {
			if (LIRC_CAN_REC(drv.features) == supported_rec_modes[i]) {
				drv.rec_mode = LIRC_REC2MODE(supported_rec_modes[i]);
				break;
			}
		}
		if (supported_rec_modes[i] == 0) {
			logprintf(LOG_NOTICE, "the receive method of the driver is not yet supported by lircd");
		}
	}
	if (drv.rec_mode == LIRC_MODE_MODE2) {
		/* get resolution */
		drv.resolution = 0;
		if ((drv.features & LIRC_CAN_GET_REC_RESOLUTION)
		    && (default_ioctl(LIRC_GET_REC_RESOLUTION, &drv.resolution) != -1)) {
			LOGPRINTF(1, "resolution of receiver: %d", drv.resolution);
		}

	} else if (drv.rec_mode == LIRC_MODE_LIRCCODE) {
		if (default_ioctl(LIRC_GET_LENGTH, (void*) &drv.code_length) == -1) {
			logprintf(LOG_ERR, "could not get code length");
			logperror(LOG_ERR, "default_init()");
			default_deinit();
			return (0);
		}
		if (drv.code_length > sizeof(ir_code) * CHAR_BIT) {
			logprintf(LOG_ERR, "lircd can not handle %lu bit codes", drv.code_length);
			default_deinit();
			return (0);
		}
	}
	if (!(drv.send_mode || drv.rec_mode)) {
		default_deinit();
		return (0);
	}
	return (1);
}

int default_deinit(void)
{
	if (drv.fd != -1) {
		close(drv.fd);
		drv.fd = -1;
	}
	return (1);
}

static int write_send_buffer(int lirc)
{
	if (send_buffer.wptr == 0) {
		LOGPRINTF(1, "nothing to send");
		return (0);
	}
	return (write(lirc, send_buffer.data, send_buffer.wptr * sizeof(lirc_t)));
}

int default_send(struct ir_remote *remote, struct ir_ncode *code)
{
	/* things are easy, because we only support one mode */
	if (drv.send_mode != LIRC_MODE_PULSE)
		return (0);

	if (drv.features & LIRC_CAN_SET_SEND_CARRIER) {
		unsigned int freq;

		freq = remote->freq == 0 ? DEFAULT_FREQ : remote->freq;
		if (default_ioctl(LIRC_SET_SEND_CARRIER, &freq) == -1) {
			logprintf(LOG_ERR, "could not set modulation frequency");
			logperror(LOG_ERR, NULL);
			return (0);
		}
	}
	if (drv.features & LIRC_CAN_SET_SEND_DUTY_CYCLE) {
		unsigned int duty_cycle;

		duty_cycle = remote->duty_cycle == 0 ? 50 : remote->duty_cycle;
		if (default_ioctl(LIRC_SET_SEND_DUTY_CYCLE, &duty_cycle) == -1) {
			logprintf(LOG_ERR, "could not set duty cycle");
			logperror(LOG_ERR, NULL);
			return (0);
		}
	}
	if (!init_send(remote, code))
		return (0);
	if (write_send_buffer(drv.fd) == -1) {
		logprintf(LOG_ERR, "write failed");
		logperror(LOG_ERR, NULL);
		return (0);
	}
	return (1);
}

char *default_rec(struct ir_remote *remotes)
{
	if (!clear_rec_buffer()) {
		default_deinit();
		return NULL;
	}
	return (decode_all(remotes));
}

int default_ioctl(unsigned int cmd, void *arg)
{
	return ioctl(drv.fd, cmd, arg);
}
