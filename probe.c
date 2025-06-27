/* probe_eglzero.c – minimal check for ZERO_COPY + EGL_IMAGE
   Build:  gcc probe_eglzero.c -o probe \
             -std=gnu99 -I/opt/vc/include -L/opt/vc/lib \
             -lmmal_core -lmmal_util -lmmal_vc_client             */

#include <stdio.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>

/* ------------------------------------------------------------------
   Older Stretch headers have no EGLImageKHR nor the two parameters.
------------------------------------------------------------------- */
#ifndef EGLImageKHR                     /* not in <EGL/eglext.h>   */
typedef void *EGLImageKHR;              /* opaque handle           */
#endif

#ifndef MMAL_PARAMETER_EGL_IMAGE
#define MMAL_PARAMETER_EGL_IMAGE  (MMAL_PARAMETER_GROUP_COMMON + 27)
typedef struct {
    MMAL_PARAMETER_HEADER_T hdr;
    uint32_t               num_images;
    EGLImageKHR            image[1];
} MMAL_PARAMETER_EGL_IMAGE_T;
#endif

#ifndef MMAL_PARAMETER_EGL_IMAGE_ATTACH
#define MMAL_PARAMETER_EGL_IMAGE_ATTACH  (MMAL_PARAMETER_GROUP_COMMON + 28)
#endif
/* ------------------------------------------------------------------ */

static void report(const char *tag, MMAL_STATUS_T st)
{
    const char *msg = (st == MMAL_SUCCESS) ? "OK" :
                      (st == MMAL_ENOSYS ) ? "ENOSYS (unsupported)" :
                                             "error";
    printf("%-10s → %s\n", tag, msg);
}

int main(void)
{
    MMAL_COMPONENT_T *ren = NULL;
    MMAL_STATUS_T st = mmal_component_create("vc.ril.egl_render", &ren);
    if (st) { fprintf(stderr,"create err %d\n", st); return 1; }

    MMAL_PORT_T *in = ren->input[0];

    /* test ZERO_COPY */
    MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY,sizeof zc},1};
    st = mmal_port_parameter_set(in, &zc.hdr);
    report("ZERO_COPY", st);

    /* test EGL_IMAGE */
    MMAL_PARAMETER_EGL_IMAGE_T eg = {{MMAL_PARAMETER_EGL_IMAGE,
                                      sizeof eg},1,{0}};
    st = mmal_port_parameter_set(in, &eg.hdr);
    report("EGL_IMAGE", st);

    mmal_component_destroy(ren);
    return 0;
}
