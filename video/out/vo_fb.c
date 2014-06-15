
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <sys/vt.h>
#include <sys/kd.h>
#include <linux/fb.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "vo.h"
#include "video/vfcap.h"
#include "video/mp_image.h"
#include "sub/osd.h"

#include "video/memcpy_pic.h"

struct priv {
    int fb, kb;
    void *vidmem;
    size_t vidmem_size;

    struct mp_image buffer;
    struct mp_image *current;

    // options
    char *device;
};

struct fmt_entry {
    int bits;
    int imgfmt;
};

static const struct fmt_entry formats[] = {
    {32,    IMGFMT_BGRA},
    {0}
};

// Needed for the signal handler
struct priv *g_state;

static int get_fb_size(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;

    if (ioctl(p->fb, FBIOGET_FSCREENINFO, &fix) < 0
     || ioctl(p->fb, FBIOGET_VSCREENINFO, &var) < 0
     || fix.type != FB_TYPE_PACKED_PIXELS)
        return -1;

    for (int n = 0; formats[n].imgfmt; n++) {
        if (formats[n].bits == var.bits_per_pixel) {
            if (p->buffer.imgfmt && p->buffer.imgfmt != formats[n].imgfmt) {
                MP_FATAL(vo, "can't change format on the fly\n");
                return -1;
            }
            mp_image_setfmt(&p->buffer, formats[n].imgfmt);
            goto found;
        }
    }
    return -1;

found:
    mp_image_set_size(&p->buffer, var.xres, var.yres);
    p->buffer.planes[0] = p->vidmem;
    p->buffer.stride[0] = fix.line_length;

    vo->want_redraw = true;
    return 0;
}

static void flip_page(struct vo *vo)
{
    // nothing, since Linux framebuffer apparently has no such thing
}

static void draw_osd(struct vo *vo, struct osd_state *osd)
{
    struct priv *p = vo->priv;
    // TODO: draw OSD
    //       ....
    // Now copy the backbuffer to the framebuffer, because draw_osd() is the
    // last operation before flip_page().
    struct mp_image src = *p->current;
    struct mp_image dst = p->buffer;
    // smallest common rectangle
    int w = MPMIN(src.w, dst.w);
    int h = MPMIN(src.h, dst.h);
    mp_image_crop(&src, 0, 0, w, h);
    mp_image_crop(&dst, 0, 0, w, h);
    // might profit from specialized GPU-memory memcpy
    mp_image_copy(&dst, &src);
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct priv *p = vo->priv;
    mp_image_setrefp(&p->current, mpi);
}

static int query_format(struct vo *vo, uint32_t format)
{
    struct priv *p = vo->priv;
    int caps = VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW;
    return p->buffer.imgfmt == format ? caps : 0;
}

static int reconfig(struct vo *vo, struct mp_image_params *params, int flags)
{
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_REDRAW_FRAME:
        return !!p->current;
    case VOCTRL_CHECK_EVENTS:
        get_fb_size(vo);
        return true;
    }

    return VO_NOTIMPL;
}

static void vtswitch(int sig)
{
    struct priv *p = g_state;
    //p->b.repaint = p->b.active = sig == SIGUSR2;
    ioctl(p->kb, VT_RELDISP, VT_ACKACQ);
    signal(sig, vtswitch);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct vt_mode vtm;
    munmap(p->vidmem, p->vidmem_size);
    //tcsetattr(p->kb, TCSANOW, &p->tio);
    ioctl(p->kb, KDSETMODE, KD_TEXT);
    vtm.mode = VT_AUTO;
    vtm.waitv = 0;
    ioctl(p->kb, VT_SETMODE, &vtm);
    close(p->fb);
    close(p->kb);
    g_state = NULL;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    assert(!g_state);
    g_state = p;

    struct fb_fix_screeninfo fix;
    //struct termios tio;
    struct vt_mode vtm;

    p->fb = p->kb = -1;
    p->vidmem = MAP_FAILED;

    if ((p->fb = open(p->device, O_RDWR)) < 0 || get_fb_size(vo) < 0
        || ioctl(p->fb, FBIOGET_FSCREENINFO, &fix) < 0)
        goto error;

    p->vidmem = mmap(0, fix.smem_len, PROT_WRITE|PROT_READ, MAP_SHARED, p->fb, 0);
    if (p->vidmem == MAP_FAILED)
        goto error;
    p->vidmem_size = fix.smem_len;

    signal(SIGUSR1, vtswitch);
    signal(SIGUSR2, vtswitch);

    if ((p->kb = open("/dev/tty", O_RDONLY)) < 0
     || ioctl(p->kb, KDSETMODE, KD_GRAPHICS) < 0)
        goto error;

    /* If the above succeeds, the below cannot fail */
    /*
    tcgetattr(p->kb, &p->tio);
    tio = p->tio;
    tio.c_cflag = B38400 | CS8 | CLOCAL | CREAD;
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tcsetattr(p->kb, TCSANOW, &tio);
    */

    vtm.mode = VT_PROCESS;
    vtm.waitv = 0;
    vtm.relsig = SIGUSR1;
    vtm.acqsig = SIGUSR2;
    vtm.frsig = 0;
    ioctl(p->kb, VT_SETMODE, &vtm);

    return 0;
error:
    if (p->vidmem != MAP_FAILED)
        munmap(p->vidmem, fix.smem_len);
    close(p->fb);
    close(p->kb);
    return -1;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_fb = {
    .description = "Linux Framebuffer",
    .name = "fb",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .draw_osd = draw_osd,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]){
        OPT_STRING("device", device, 0),
        {0}
    },
    .priv_defaults = &(const struct priv){
        .device = "/dev/fb0",
    }
};
