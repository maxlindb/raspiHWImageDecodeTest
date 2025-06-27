/*  texturecube_min.c  ──────────────────────────────────────────────
 *  Hitch-free texture upload on Raspberry Pi Zero using **MMAL only**
 *  Build (Stretch):                                             *
 *      gcc texturecube_min.c -o texturecube                     \
 *          -std=gnu99 -DGL_GLEXT_PROTOTYPES                     \
 *          -I/opt/vc/include -L/opt/vc/lib                      \
 *          -lGLESv2 -lEGL -lbcm_host                            \
 *          -lmmal_core -lmmal_util -lmmal_vc_client -lpthread -lm
 *
 *  Run:
 *      sudo ./texturecube_min first.jpg  [second.png]           *
 *      (press SPACE to queue the 2nd image, ESC to quit)        *
 *  ────────────────────────────────────────────────────────────── */

#define _GNU_SOURCE                 /* for usleep() */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include <bcm_host.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* MMAL */
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/mmal_parameters_video.h>
#include <interface/mmal/util/mmal_util.h>  


/* ───── Fallback for old MMAL headers (Stretch) ───── */
#ifndef MMAL_PARAMETER_EGL_IMAGE
#define MMAL_PARAMETER_EGL_IMAGE  (MMAL_PARAMETER_GROUP_COMMON + 27)

/* MMAL maps an EGLImageKHR handle into a 32-bit field */
typedef struct
{
    MMAL_PARAMETER_HEADER_T hdr;
    uint32_t               num_images;
    EGLImageKHR            image[1];   /* flexible array, 1..n */
} MMAL_PARAMETER_EGL_IMAGE_T;
#endif


#ifndef MMAL_PARAMETER_EGL_IMAGE_ATTACH        /* Stretch headers lack this */
#define MMAL_PARAMETER_EGL_IMAGE_ATTACH  (MMAL_PARAMETER_GROUP_COMMON + 28)
/* it is just a boolean flag, so we can re-use MMAL_PARAMETER_BOOLEAN_T   */
#endif



/* vertex shader */
static const char *VS =
"#version 100\n"                  /* ← NEW first line               */
"precision mediump float;\n"
"attribute  vec3 aPos;\n"
"attribute  vec2 aUV;\n"
"varying    mediump vec2 vUV;\n"
"uniform    mat4 uMVP;\n"
"void main(void)\n"
"{\n"
"    vUV = aUV;\n"
"    gl_Position = uMVP * vec4(aPos,1.0);\n"
"}\n";

/* fragment shader */
static const char *FS =
"#version 100\n"                  /* ← NEW first line               */
"precision mediump float;\n"
"varying mediump vec2 vUV;\n"
"uniform sampler2D uTex;\n"
"void main(void)\n"
"{\n"
"    gl_FragColor = texture2D(uTex, vUV);\n"
"}\n";




/* ───── cube data (12 tris, 36 indices) ───── */
static const GLfloat cube_v[] = {  /* pos.xyz  uv */
    -1,-1,-1, 0,0,   1,-1,-1, 1,0,   1, 1,-1, 1,1,  -1, 1,-1, 0,1,  /* back  */
    -1,-1, 1, 0,0,   1,-1, 1, 1,0,   1, 1, 1, 1,1,  -1, 1, 1, 0,1   /* front */
};
static const GLushort cube_i[] = {
    0,1,2, 2,3,0,  4,5,6, 6,7,4,  /* back/front */
    0,4,7, 7,3,0,  1,5,6, 6,2,1,  /* left/right */
    3,2,6, 6,7,3,  0,1,5, 5,4,0   /* top/bottom */
};

/* ───── GL helpers ───── */
/*static GLuint compile(GLenum t,const char*s){GLuint sh=glCreateShader(t);
  glShaderSource(sh,1,&s,0); glCompileShader(sh); return sh;}*/
  
static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, 0);
    glCompileShader(sh);

    GLint ok = 0;  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei len = 0;
        glGetShaderInfoLog(sh, sizeof log, &len, log);
        fprintf(stderr, "shader %u len=%d\n%.*s\n", (unsigned)type, len, len, log);
    }
    return sh;
}


/* ----------------------------------------------------
 * wait until decoder has produced a valid output format
 * (old Stretch headers: no component->event_queue)     */
static MMAL_STATUS_T
wait_format_changed(MMAL_PORT_T *ctrl /*unused*/, MMAL_PORT_T *out)
{
    (void)ctrl;                         /* silence “unused” warning */

    /* give the firmware up to ~5 s (500 × 10 ms) */
    for (int i = 0; i < 500; ++i) {
        if (out->format->es->video.width  &&
            out->format->es->video.height)
            return MMAL_SUCCESS;        /* format ready */

        usleep(10 * 1000);              /* 10 ms */
    }
    return MMAL_ENOSYS;                 /* timeout → format never arrived */
}


  

/* ───── shared state ───── */
static EGLDisplay   g_dpy;
static GLuint       tex_front, tex_back;
static volatile EGLImageKHR  next_img = 0;
static volatile int image_ready = 0;

/* ───── MMAL callback ───── */
static void cb_buffer(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    if(buf->length >= sizeof(EGLImageKHR)){
        memcpy((void*)&next_img, buf->data, sizeof(EGLImageKHR));
        image_ready = 1;
    }
    mmal_buffer_header_release(buf);
}



/* ───── worker thread: decode file → egl_render → EGLImage ───── */
static void *loader(void *arg)
{
    const char *file = arg;

/* 0. read compressed file into memory -------------------------------- */
    FILE *fp = fopen(file, "rb");
    if (!fp) { perror(file); return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
    uint8_t *data = malloc(sz);  fread(data, 1, sz, fp);  fclose(fp);

/* 1. create + enable components -------------------------------------- */
    MMAL_COMPONENT_T *dec = NULL, *ren = NULL;  MMAL_CONNECTION_T *con = NULL;
    MMAL_STATUS_T st;

    if ((st = mmal_component_create("vc.ril.image_decode", &dec))) {
        fprintf(stderr, "decoder create %d\n", st); goto end;
    }
    if ((st = mmal_component_create("vc.ril.egl_render", &ren))) {
        fprintf(stderr, "render  create %d\n", st); goto end;
    }
    mmal_component_enable(dec);
    mmal_component_enable(ren);

    MMAL_PORT_T *in  = dec->input[0];
    MMAL_PORT_T *out = dec->output[0];
    MMAL_PORT_T *rin = ren->input[0];

/* 2. ask decoder to output opaque buffers (works for all still formats) */
    out->format->encoding = MMAL_ENCODING_OPAQUE;
    mmal_port_format_commit(out);

    mmal_format_copy(rin->format, out->format);      /* same on render in   */
    mmal_port_format_commit(rin);

/* 3. enable ZERO-COPY on both ports so we get EGLImages directly ------- */
    MMAL_PARAMETER_BOOLEAN_T zc = {{MMAL_PARAMETER_ZERO_COPY,sizeof zc}, 1};
    mmal_port_parameter_set(out, &zc.hdr);
    mmal_port_parameter_set(rin, &zc.hdr);

/* 4. hint egl_render that we’ll pull the image out with glEGLImage… ---- */
    MMAL_PARAMETER_EGL_IMAGE_T cfg = {{MMAL_PARAMETER_EGL_IMAGE,
                                       sizeof cfg}, 1, {0}};
    mmal_port_parameter_set(rin, &cfg.hdr);

/* 5. hook callback, create + enable the tunnel ------------------------ */
    rin->userdata = (void*)cb_buffer;
    mmal_port_enable(rin, cb_buffer);

    st = mmal_connection_create(&con, out, rin,
            MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_DIRECT);
    if (!st) st = mmal_connection_enable(con);
    if (st) { fprintf(stderr, "conn err %d\n", st); goto end; }

/* 6. input pool and push the compressed bit-stream -------------------- */
    MMAL_POOL_T *pool = mmal_port_pool_create(
            in, in->buffer_num_recommended, in->buffer_size_recommended);

    uint8_t *ptr = data;  long left = sz;
    while (left) {
        MMAL_BUFFER_HEADER_T *b = mmal_queue_get(pool->queue);
        if (!b) { usleep(1000); continue; }

        int cp = left < b->alloc_size ? left : b->alloc_size;
        mmal_buffer_header_mem_lock(b);
        memcpy(b->data, ptr, cp);
        mmal_buffer_header_mem_unlock(b);

        b->length = cp;
        ptr  += cp;  left -= cp;
        if (!left) b->flags = MMAL_BUFFER_HEADER_FLAG_EOS;

        mmal_port_send_buffer(in, b);
    }

/* 7. spin until egl_render hands the EGLImage to our callback --------- */
    while (!image_ready) usleep(1000);

end:
    if (con)  mmal_connection_destroy(con);
    if (ren)  mmal_component_destroy(ren);
    if (dec)  mmal_component_destroy(dec);
    free(data);
    return NULL;
}




/* ───── minimal GL + EGL init ───── */
static GLuint init_gl(uint32_t w,uint32_t h,EGLSurface *surf)
{
    fprintf(stderr, "--- init_gl begin\n");   /* debug */
    bcm_host_init();
    fprintf(stderr, "spam bcm\n");      
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    fprintf(stderr, "spam eglGetDisplay\n");
    eglInitialize(g_dpy,0,0);
    fprintf(stderr, "spam eglInitialize\n");

    EGLConfig cfg; EGLint n;
    EGLint attr[]={EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE};
    
    eglChooseConfig(g_dpy,attr,&cfg,1,&n);
    fprintf(stderr, "spam eglChooseConfig\n");
    


    VC_RECT_T dst={0,0,w,h}, src={0,0,w<<16,h<<16};
    
    fprintf(stderr, "opening display…\n");
    DISPMANX_DISPLAY_HANDLE_T disp = vc_dispmanx_display_open(0);
    fprintf(stderr, " display handle = %u\n", disp);
    
    DISPMANX_UPDATE_HANDLE_T  upd =vc_dispmanx_update_start(0);
    static EGL_DISPMANX_WINDOW_T win;
    win.element=vc_dispmanx_element_add(upd,disp,200,&dst,0,&src,0,0,0,0);
    win.width=w; win.height=h;
    vc_dispmanx_update_submit_sync(upd);

    *surf=eglCreateWindowSurface(g_dpy,cfg,&win,0);
    fprintf(stderr, "eglCreateWindowSurface = %p  err=0x%x\n",
        *surf, eglGetError());
        
        
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2,   /* NEW */
             EGL_NONE };
    
    EGLContext ctx=eglCreateContext(g_dpy,cfg,EGL_NO_CONTEXT, ctxattr);
    EGLBoolean ok = eglMakeCurrent(g_dpy,*surf,*surf,ctx);
    printf("eglMakeCurrent → %d  (error 0x%x)\n", ok, eglGetError());
    
    glViewport(0, 0, w, h);          /* set drawable area               */
    glClearColor(0, 0.2f, 0.4f, 1);  /* non-black background to verify  */
    glEnable(GL_DEPTH_TEST);         /* cube faces won’t Z-fight        */

    /* --- shader setup block --- */
    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER,   VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(prog, 0, "aPos");       /* NEW */
    glBindAttribLocation(prog, 1, "aUV");        /* NEW */
    glLinkProgram(prog);    
    
    /* ---------- debug: program link status ---------- */
    GLint linked = 0; GLsizei len = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, &len, log);
        fprintf(stderr, "Program link error:\n%.*s\n", len, log);
    }
    /* ------------------------------------------------- */
    
    glUseProgram(prog);


    /* VBO */
    GLuint vbo, ibo;
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(cube_v),cube_v,GL_STATIC_DRAW);
    glGenBuffers(1,&ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(cube_i),cube_i,GL_STATIC_DRAW);

    GLint aPos=glGetAttribLocation(prog,"aPos");
    GLint aUV =glGetAttribLocation(prog,"aUV");
    glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos,3,GL_FLOAT,0,20,(void*)0);
    glEnableVertexAttribArray(aUV ); glVertexAttribPointer(aUV ,2,GL_FLOAT,0,20,(void*)12);

    /* identity MVP */
    GLint uMVP=glGetUniformLocation(prog,"uMVP");
    float id[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glUniformMatrix4fv(uMVP,1,GL_FALSE,id);

    /* two textures (front/back) */
    glGenTextures(1,&tex_front); glGenTextures(1,&tex_back);
    GLuint t[2]={tex_front,tex_back};
    for(int i=0;i<2;i++){
        glBindTexture(GL_TEXTURE_2D,t[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2048,2048,0,
                     GL_RGBA,GL_UNSIGNED_BYTE,0);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    }
    
    printf("init_gl finished, prog=%u  GL_VENDOR=%s\n",
       prog, glGetString(GL_VENDOR));
    
    return prog;
}


/* ───── main ───── */
int main(int argc,char**argv)
{
    setbuf(stdout, NULL);                 /* ← no stdout buffering  */
    fprintf(stderr, "--- texturecube start persele\n");   /* ← always visible */

    if(argc<2){fprintf(stderr,"usage: %s img1 [img2]\n",argv[0]);return 1;}
    uint32_t W=640,H=480; EGLSurface surf;
    GLuint prog_id = init_gl(W,H,&surf);

    /* start first load */
    pthread_t th; pthread_create(&th,0,loader,argv[1]); pthread_detach(th);

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bindImage =
        (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    float angle=0;
    while(1){
    
        static int frame=0;
        if((frame++ & 255)==0) printf("frame %d\n", frame);
    
        if(image_ready){
            glBindTexture(GL_TEXTURE_2D,tex_back);
            bindImage(GL_TEXTURE_2D,next_img);
            GLuint tmp=tex_front; tex_front=tex_back; tex_back=tmp;
            image_ready=0;
        }

        /* simple spin */
        angle += 0.01f;
        float c=cosf(angle),s=sinf(angle);
        float mvp[16]={ c,0,s,0, 0,1,0,0, -s,0,c,0, 0,0,-5,1};
        //glUniformMatrix4fv(glGetUniformLocation(
        //    glGetIntegerv(GL_CURRENT_PROGRAM,&prog_id),"uMVP"),1,GL_FALSE,mvp);
        glUniformMatrix4fv(glGetUniformLocation(prog_id, "uMVP"),
                   1, GL_FALSE, mvp);

        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D,tex_front);
        glDrawElements(GL_TRIANGLES,36,GL_UNSIGNED_SHORT,0);
        eglSwapBuffers(g_dpy,surf);
        usleep(16666);                     /* ~60 Hz */

        int k = getchar();
        if(k==27) break;                  /* ESC */
        if(k==' ' && argc>2){             /* SPACE → load second */
            pthread_t t2; pthread_create(&t2,0,loader,argv[2]);
            pthread_detach(t2);
        }
    }
    return 0;
}
