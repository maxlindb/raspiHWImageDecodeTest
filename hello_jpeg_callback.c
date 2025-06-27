// hello_jpeg_callback.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>

static volatile int jpeg_done = 0;
static FILE *out_fp = NULL;

/* called for every buffer on decoder output port */
static void jpeg_buffer_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buf)
{
    if (buf->length) {
        mmal_buffer_header_mem_lock(buf);
        fwrite(buf->data, 1, buf->length, out_fp);
        mmal_buffer_header_mem_unlock(buf);
    }
    if (buf->flags & MMAL_BUFFER_HEADER_FLAG_EOS)
        jpeg_done = 1;
    mmal_buffer_header_release(buf);
}

static void *loader(void *arg)
{
    const char *infile  = arg;
    const char *outfile = "out.jpg";

    /* 0) read the entire JPEG into RAM */
    FILE *fp = fopen(infile, "rb");
    if (!fp) { perror(infile); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    uint8_t *in_buf = malloc(sz);
    fread(in_buf,1,sz,fp);
    fclose(fp);

    /* 1) create & enable decoder */
    MMAL_COMPONENT_T *dec = NULL;
    MMAL_STATUS_T st = mmal_component_create("vc.ril.image_decode", &dec);
    if (st) { fprintf(stderr,"can't create decode %d\n",st); goto cleanup; }
    mmal_component_enable(dec);

    MMAL_PORT_T *dec_in  = dec->input[0];
    MMAL_PORT_T *dec_out = dec->output[0];

    /* 2) enable zero-copy & wait for the firmware to fill in width/height */
    mmal_port_parameter_set_boolean(dec_out, MMAL_PARAMETER_ZERO_COPY, 1);
    mmal_port_enable(dec_out, NULL);
    for(int i=0; i<200 && !dec_out->format->es->video.width; ++i)
        usleep(10000);
    mmal_port_disable(dec_out);

    /* 3) commit the real output format */
    if ((st = mmal_port_format_commit(dec_out))) {
      fprintf(stderr,"dec_out commit failed %d\n",st);
      goto cleanup;
    }

    /* 4) open the output file */
    out_fp = fopen(outfile,"wb");
    if (!out_fp) { perror(outfile); goto cleanup; }

    /* 5) create a buffer pool & hook our callback */
    MMAL_POOL_T *pool = mmal_port_pool_create(
      dec_out,
      dec_out->buffer_num_recommended,
      dec_out->buffer_size_recommended
    );
    mmal_port_enable(dec_out, jpeg_buffer_cb);

    /* 6) push your JPEG data into the decoder */
    uint8_t *ptr = in_buf;
    long left = sz;
    while(left) {
      MMAL_BUFFER_HEADER_T *b = mmal_queue_get(pool->queue);
      if (!b) { usleep(1000); continue; }
      int copy = left < b->alloc_size ? left : b->alloc_size;
      mmal_buffer_header_mem_lock(b);
      memcpy(b->data, ptr, copy);
      mmal_buffer_header_mem_unlock(b);
      b->length = copy;
      ptr += copy;
      left -= copy;
      if (!left) b->flags = MMAL_BUFFER_HEADER_FLAG_EOS;
      mmal_port_send_buffer(dec_in, b);
    }

    /* 7) spin until EOS arrives in our callback */
    while(!jpeg_done) usleep(1000);

cleanup:
    if (dec) mmal_component_destroy(dec);
    free(in_buf);
    if (out_fp) fclose(out_fp);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
      fprintf(stderr,"Usage: %s <input.jpg>\n", argv[0]);
      return 1;
    }
    pthread_t th;
    pthread_create(&th, NULL, loader, argv[1]);
    pthread_join(th, NULL);
    printf("Done.  Output in out.jpg\n");
    return 0;
}
