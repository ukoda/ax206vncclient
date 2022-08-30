
/*
 * Copyright (C) 2021 David Annett david@annett.co.nz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <stdlib.h>
#include <rfb/rfbclient.h>
#include <stdbool.h>
#include "libusb-1.0/libusb.h"

// VNC stuff

static rfbClient *cl;
static bool framebuffer_allocated = FALSE;


static void ErrorLog (const char *format, ...);
static void DefaultLog (const char *format, ...);



// USB display stuff

#define DPFAXHANDLE void *      // Handle needed for dpf_ax access
#define DPF_BPP 2               //bpp for dfp-ax is currently always 2!

// Convert RGBA pixel to RGB565 pixel(s)

#define _RGB565_0(p) (( ((p.R) & 0xf8)      ) | (((p.G) & 0xe0) >> 5))
#define _RGB565_1(p) (( ((p.G) & 0x1c) << 3 ) | (((p.B) & 0xf8) >> 3))


#define AX206_VID 0x1908        // Hacked frames USB Vendor ID
#define AX206_PID 0x0102        // Hacked frames USB Product ID

#define USBCMD_SETPROPERTY  0x01        // USB command: Set property
#define USBCMD_BLIT         0x12        // USB command: Blit to screen

/* Generic SCSI device stuff */

#define DIR_IN  0
#define DIR_OUT 1

typedef struct {
    unsigned char R;
    unsigned char G;
    unsigned char B;
    unsigned char A;
} RGBA;

/*
 * Dpf status
 */
static struct {
    unsigned char *lcdBuf;      // Display data buffer
    unsigned char *xferBuf;     // USB transfer buffer
    DPFAXHANDLE dpfh;           // Handle for dpf access
    int pwidth;                 // Physical display width
    int pheight;                // Physical display height

    // Flags to translate logical to physical orientation
//    int isPortrait;
//    int rotate90;
//    int flip;

    // Current dirty rectangle
    int minx, maxx;
    int miny, maxy;

    // Config properties
//    int orientation;
    int backlight;
} dpf;

/* The DPF context structure */
typedef
    struct dpf_context {
    libusb_device_handle *udev;
    unsigned int width;
    unsigned int height;
} DPFContext;


DPFAXHANDLE dpf_ax_open(const char *dev);
void dpf_ax_close(DPFAXHANDLE h);
static int wrap_scsi(DPFContext * h, unsigned char *cmd, int cmdlen, char out,
                     unsigned char *data, unsigned long block_len);


int DROWS, DCOLS;        /* display size */


// USB LCD stuff

/**
 * Open DPF device.
 * 
 * Device must be string in the form "usbX" or "dpfX", with X = 0 .. number of connected dpfs.
 * The open function will scan the USB bus and return a handle to access dpf #X.
 * If dpf #X is not found, returns NULL.
 *
 * \param dev	device name to open
 * \return		device handle or NULL
 */
DPFAXHANDLE dpf_ax_open(const char *dev)
{
    DPFContext *dpf;
    int index = -1;
    libusb_device_handle *u;
    libusb_device **devs;
    ssize_t cnt;
    int r, i;
    int enumeration = 0;
    struct libusb_device *d = NULL;
    struct libusb_device_descriptor desc;

    if (dev && strlen(dev) == 4 && (strncmp(dev, "usb", 3) == 0 || strncmp(dev, "dpf", 3) == 0))
        index = dev[3] - '0';

    if (index < 0 || index > 9) {
        ErrorLog("dpf_ax_open: wrong device '%s'. Please specify a string like 'usb0'\n", dev);
        return NULL;
    }

    r = libusb_init(NULL);
    if (r < 0) {
        ErrorLog("dpf_ax_open: libusb_init failed with error %d\n", r);
        return NULL;
    }

    cnt = libusb_get_device_list(NULL, &devs);
    if (cnt < 0) {
        libusb_exit(NULL);
        ErrorLog("dpf_ax_open: libusb_init failed with error %d\n", r);
        return NULL;
    }

    for (i = 0; devs[i]; i++) {
        r = libusb_get_device_descriptor(devs[i], &desc);
        if (r < 0) {
            ErrorLog("dpf_ax_open: failed to get device descriptor");
            return NULL;
        }

        if ((desc.idVendor == AX206_VID) && (desc.idProduct == AX206_PID)) {
            DefaultLog("dpf_ax_open: found AX206 #%d\n", enumeration + 1);
            if (enumeration == index) {
                d = devs[i];
                break;
            } else {
                enumeration++;
            }
        }
    }

    if (!d) {
        ErrorLog("dpf_ax_open: no matching USB device '%s' found!\n", dev);
        return NULL;
    }

    dpf = (DPFContext *) malloc(sizeof(DPFContext));
    if (!dpf) {
        ErrorLog("dpf_ax_open: error allocation memory.\n");
        return NULL;
    }

    r = libusb_open(d, &u);
    if ((r != 0) || (u == NULL)) {
        ErrorLog("dpf_ax_open: failed to open usb device '%s'!\n", dev);
        free(dpf);
        return NULL;
    }

    libusb_free_device_list(devs, 1);
    r = libusb_claim_interface(u, 0);
    if (r < 0) {
        ErrorLog("dpf_ax_open: failed to claim usb device, error %d = %s = %s\n", r, libusb_error_name(r), libusb_strerror(r));
        libusb_close(u);
        free(dpf);
        return NULL;
    }

    dpf->udev = u;

    static unsigned char buf[5];
    static unsigned char cmd[16] = {
        0xcd, 0, 0, 0,
        0, 2, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };
    cmd[5] = 2;                 // get LCD parameters
    if (wrap_scsi(dpf, cmd, sizeof(cmd), DIR_IN, buf, 5) == 0) {
        dpf->width = (buf[0]) | (buf[1] << 8);
        dpf->height = (buf[2]) | (buf[3] << 8);
        DefaultLog("dpf_ax_open: got LCD dimensions: %dx%d\n", dpf->width, dpf->height);
    } else {
        ErrorLog("dpf_ax_open: error reading LCD dimensions!\n");
        dpf_ax_close(dpf);
        return NULL;
    }
    return (DPFAXHANDLE) dpf;
}



/**
 *  Close DPF device
 */
void dpf_ax_close(DPFAXHANDLE h)
{
    DPFContext *dpf = (DPFContext *) h;

    libusb_release_interface(dpf->udev, 0);
    libusb_close(dpf->udev);
    free(dpf);
}


// Convert RGBA pixel to RGB565 pixel(s)

#define _RGB565_0(p) (( ((p.R) & 0xf8)      ) | (((p.G) & 0xe0) >> 5))
#define _RGB565_1(p) (( ((p.G) & 0x1c) << 3 ) | (((p.B) & 0xf8) >> 3))

/*
 * Set one pixel in lcdBuf.
 * 
 * Respects orientation and updates dirty rectangle.
 *
 * in:  x, y - pixel coordinates
 *  pix  - RGBA pixel value
 * out: -
 */
static void drv_set_pixel(int x, int y, RGBA pix)
{
    int changed = 0;

    int sx = DCOLS;
    int sy = DROWS;
    int lx = x % sx;
    int ly = y % sy;

    if (lx < 0 || lx >= (int) dpf.pwidth || ly < 0 || ly >= (int) dpf.pheight) {
        ErrorLog("dpf: x/y out of bounds (x=%d, y=%d, lx=%d, ly=%d)\n", x, y, lx, ly);
        return;
    }

    unsigned char c1 = _RGB565_0(pix);
    unsigned char c2 = _RGB565_1(pix);
    unsigned int i = (ly * dpf.pwidth + lx) * DPF_BPP;
    if (dpf.lcdBuf[i] != c1 || dpf.lcdBuf[i + 1] != c2) {
        dpf.lcdBuf[i] = c1;
        dpf.lcdBuf[i + 1] = c2;
        changed = 1;
    }

    if (changed) {
        if (lx < dpf.minx)
            dpf.minx = lx;
        if (lx > dpf.maxx)
            dpf.maxx = lx;
        if (ly < dpf.miny)
            dpf.miny = ly;
        if (ly > dpf.maxy)
            dpf.maxy = ly;
    }
}



static
unsigned char g_excmd[16] = {
    0xcd, 0, 0, 0,
    0, 6, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0
};

/** Blit data to screen.
 *
 * \param buf     buffer to 16 bpp RGB 565 image data
 * \param rect    rectangle tuple: [x0, y0, x1, y1]
 */
void dpf_ax_screen_blit(DPFAXHANDLE h, const unsigned char *buf, short rect[4])
{
    unsigned long len = (rect[2] - rect[0]) * (rect[3] - rect[1]);
    len <<= 1;
    unsigned char *cmd = g_excmd;

    cmd[6] = USBCMD_BLIT;
    cmd[7] = rect[0];
    cmd[8] = rect[0] >> 8;
    cmd[9] = rect[1];
    cmd[10] = rect[1] >> 8;
    cmd[11] = rect[2] - 1;
    cmd[12] = (rect[2] - 1) >> 8;
    cmd[13] = rect[3] - 1;
    cmd[14] = (rect[3] - 1) >> 8;
    cmd[15] = 0;

    wrap_scsi((DPFContext *) h, cmd, sizeof(g_excmd), DIR_OUT, (unsigned char *) buf, len);
}



static unsigned char g_buf[] = {
    0x55, 0x53, 0x42, 0x43,     // dCBWSignature
    0xde, 0xad, 0xbe, 0xef,     // dCBWTag
    0x00, 0x80, 0x00, 0x00,     // dCBWLength
    0x00,                       // bmCBWFlags: 0x80: data in (dev to host), 0x00: Data out
    0x00,                       // bCBWLUN
    0x10,                       // bCBWCBLength

    // SCSI cmd:
    0xcd, 0x00, 0x00, 0x00,
    0x00, 0x06, 0x11, 0xf8,
    0x70, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

#define ENDPT_OUT 1
#define ENDPT_IN 0x81

static int wrap_scsi(DPFContext * h, unsigned char *cmd, int cmdlen, char out,
                     unsigned char *data, unsigned long block_len)
{
    int len;
    int transfered;
    int ret;
    static unsigned char ansbuf[13];    // Do not change size.

    g_buf[14] = cmdlen;
    memcpy(&g_buf[15], cmd, cmdlen);

    g_buf[8] = block_len;
    g_buf[9] = block_len >> 8;
    g_buf[10] = block_len >> 16;
    g_buf[11] = block_len >> 24;

    ret = libusb_bulk_transfer(h->udev, ENDPT_OUT, g_buf, sizeof(g_buf), &transfered, 1000);
    if (ret != 0)
        return ret;

    if (out == DIR_OUT) {
        if (data) {
            ret = libusb_bulk_transfer(h->udev, ENDPT_OUT, data, block_len, &transfered, 3000);
            if ((ret != 0) || (transfered != (int) block_len)) {
                fprintf(stderr, "dpf_ax ERROR: bulk write.\n");
                return ret;
            }
        }
    } else if (data) {
        ret = libusb_bulk_transfer(h->udev, ENDPT_IN, data, block_len, &transfered, 4000);
        if ((ret != 0) || (transfered != (int) block_len)) {
            fprintf(stderr, "dpf_ax ERROR: bulk read.\n");
            return ret;
        }
    }
    // get ACK:
    len = sizeof(ansbuf);
    int retry = 0;
    int timeout = 0;
    do {
        timeout = 0;
        ret = libusb_bulk_transfer(h->udev, ENDPT_IN, ansbuf, len, &transfered, 5000);
        if ((ret != 0) || (transfered != (int) len)) {
            fprintf(stderr, "dpf_ax ERROR: bulk ACK read. ret = %d transfered = %d expected %d\n", ret, transfered,
                    len);
            timeout = 1;
        }
        retry++;
    } while (timeout && retry < 5);
    if (strncmp((char *) ansbuf, "USBS", 4)) {
        fprintf(stderr, "dpf_ax ERROR: got invalid reply\n.");
        return -1;
    }
    // pass back return code set by peer:
    return ansbuf[12];
}


// VNC stuff

static void got_cut_text (rfbClient *cl, const char *text, int textlen)
{
	DefaultLog("got_cut_text\n");
}

static void kbd_leds (rfbClient *cl, int value, int pad) {
	DefaultLog("kbd_leds\n");
}

static void text_chat (rfbClient *cl, int value, char *text) {
	DefaultLog("text_chat\n");
}

static char * get_password (rfbClient *client)
{
	char *password;

	DefaultLog("get_password\n");
	password = NULL;
	return password;
}



/*
static void request_screen_refresh (GtkMenuItem *menuitem,
                                    gpointer     user_data)
{
	SendFramebufferUpdateRequest (cl, 0, 0, cl->width, cl->height, FALSE);
}
*/

static rfbBool resize (rfbClient *client) {
	static char first=TRUE;
	int tmp_width, tmp_height;

	if (first) {
		first=FALSE;
		DefaultLog("resize first %d x %d\n", client->width, client->height);
		client->frameBuffer = malloc(client->width * client->height * 4);
	} else {
		DefaultLog("resize later %d x %d\n", client->width, client->height);
		free(client->frameBuffer);
		client->frameBuffer = malloc(client->width * client->height * 4);
	}
	return TRUE;
}


static void update (rfbClient *cl, int x, int y, int w, int h) {
    int lx, ly;
    RGBA pixel;

    // Set pixels one by one
    // Note: here is room for optimization :-)
    for (ly = y; ly < y + h; ly++)
        for (lx = x; lx < x + w; lx++) {
            pixel.R = cl->frameBuffer[(ly * dpf.pwidth + lx) * 4];
            pixel.G = cl->frameBuffer[(ly * dpf.pwidth + lx) * 4 + 1];
            pixel.B = cl->frameBuffer[(ly * dpf.pwidth + lx) * 4 + 2];
            drv_set_pixel(lx, ly, pixel);
        }

    // If nothing has changed, skip transfer
    if (dpf.minx > dpf.maxx || dpf.miny > dpf.maxy)
        return;

    // Copy data in dirty rectangle from data buffer to temp transfer buffer
    unsigned int cpylength = (dpf.maxx - dpf.minx + 1) * DPF_BPP;
    unsigned char *ps = dpf.lcdBuf + (dpf.miny * dpf.pwidth + dpf.minx) * DPF_BPP;
    unsigned char *pd = dpf.xferBuf;
    for (ly = dpf.miny; ly <= dpf.maxy; ly++) {
        memcpy(pd, ps, cpylength);
        ps += dpf.pwidth * DPF_BPP;
        pd += cpylength;
    }

    // Send the buffer
    short rect[4];
    rect[0] = dpf.minx;
    rect[1] = dpf.miny;
    rect[2] = dpf.maxx + 1;
    rect[3] = dpf.maxy + 1;
    dpf_ax_screen_blit(dpf.dpfh, dpf.xferBuf, rect);

    // Reset dirty rectangle
    dpf.minx = dpf.pwidth - 1;
    dpf.maxx = 0;
    dpf.miny = dpf.pheight - 1;
    dpf.maxy = 0;

}



static void ErrorLog (const char *format, ...)
{
	va_list args;
	char buf[256];
	time_t log_clock;

	va_start (args, format);

	time (&log_clock);
	strftime (buf, 255, "%d/%m/%Y %X Error: ", localtime (&log_clock));
	fprintf (stderr, "%s", buf);

	vfprintf (stderr, format, args);
	fflush (stderr);

	va_end (args);
}



static void DefaultLog (const char *format, ...)
{
	va_list args;
	char buf[256];
	time_t log_clock;

	va_start (args, format);

	time (&log_clock);
	strftime (buf, 255, "%d/%m/%Y %X Log:   ", localtime (&log_clock));
	fprintf (stdout, "%s", buf);

	vfprintf (stdout, format, args);
	fflush (stdout);

	va_end (args);
}



int main (int argc, char *argv[])
{
	int    i;
	bool   debug;
    int    vncargc;
    char **vncargv;

// Try to open the USB display

    if (argc < 2) {
        ErrorLog("No dpf device or VNC service specified\n");
        DefaultLog("Usage:\n");
        DefaultLog("%s dpf server\n", argv[0]);
        DefaultLog("e.g:\n");
        DefaultLog("%s usb0 server.domain:port\n", argv[0]);
        return -1;
    }

    dpf.dpfh = dpf_ax_open(argv[1]);
    if (dpf.dpfh == NULL) {
        ErrorLog("dpf: cannot open dpf device %s\n", argv[1]);
        return -1;
    }

    // Get dpfs physical dimensions
//    dpf.pwidth = dpf_ax_getwidth(dpf.dpfh);
//    dpf.pheight = dpf_ax_getheight(dpf.dpfh);
    dpf.pwidth = 480;
    dpf.pheight = 320;

    // allocate display buffer + temp transfer buffer
    dpf.lcdBuf = malloc(dpf.pwidth * dpf.pheight * DPF_BPP);
    dpf.xferBuf = malloc(dpf.pwidth * dpf.pheight * DPF_BPP);

    // clear display buffer + set it to "dirty"
    memset(dpf.lcdBuf, 0, dpf.pwidth * dpf.pheight * DPF_BPP);  //Black
    dpf.minx = 0;
    dpf.maxx = dpf.pwidth - 1;
    dpf.miny = 0;
    dpf.maxy = dpf.pheight - 1;
    // set the logical width/height for lcd4linux
    DROWS = dpf.pheight;
    DCOLS = dpf.pwidth;

    vncargc = argc - 1;
    vncargv = &argv[1];


    rfbClientLog = DefaultLog;
    rfbClientErr = ErrorLog;

	cl = rfbGetClient (8, 3, 4);  // TODO: Work out the correct values we need
	cl->MallocFrameBuffer = resize;
	cl->canHandleNewFBSize = FALSE;
	cl->GotFrameBufferUpdate = update;
	cl->GotXCutText = got_cut_text;
	cl->HandleKeyboardLedState = kbd_leds;
	cl->HandleTextChat = text_chat;
	cl->GetPassword = get_password;

//	show_connect_window (argc, argv);

	if (!rfbInitClient (cl, &vncargc, vncargv)) {
        dpf_ax_close(dpf.dpfh);
        return 1;
    }


    debug = true;
    while (1) {
      i = WaitForMessage (cl, 500);
      if (debug) {
        debug = false;
    }
    if (i < 0) {
       ErrorLog("Exiting because i = %d\n", i);
       dpf_ax_close(dpf.dpfh);
       return 1;
   }
   if (i)
       if (!HandleRFBServerMessage(cl)) {
         ErrorLog("Exiting because HandleRFBServerMessage() unhappy\n");
         dpf_ax_close(dpf.dpfh);
         return 2;
     }
 }

 dpf_ax_close(dpf.dpfh);
 return 0;
}
