

#gcc texturecube_min.c -o texturecube                                        \
#    -std=gnu99 -DOMX_SKIP64BIT -DGL_GLEXT_PROTOTYPES                        \
#    -I/opt/vc/include                                                      \
#    -L/opt/vc/lib  -Wl,-rpath,/opt/vc/lib                                   \
#    -lbrcmGLESv2 -lbrcmEGL -lbcm_host -lopenmaxil -lilclient                \
#    -lvcos -lvchiq_arm -lpthread


gcc texturecube_min.c -o texturecube \
    -std=gnu99 -DGL_GLEXT_PROTOTYPES \
    -I/opt/vc/include \
    -L/opt/vc/lib \
    -lGLESv2 -lEGL -lbcm_host \
    -lmmal_core -lmmal_util -lmmal_vc_client \
    -lpthread -lm




###############################################################################
# 3)  Run – ‘sudo’ is required because DispmanX needs privileged access
###############################################################################
sudo ./texturecube  tex0.jpg        # hit SPACE to load another image

