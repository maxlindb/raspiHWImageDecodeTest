# Written by Matt Ownby

include Makefile.vars.pi

# send these to all the sub-Makefiles

# name of the executable
EXE = ../jpeg_gles2

# Platform specific cflags defined in the Makefile.vars file
export CFLAGS = ${PFLAGS} -Wall

OBJS = main.o \
	jpeg/*.o  \
	video/VideoObjects/*.o \
	openmax/*.o \
	io/*.o \
	platform/*.o

LOCAL_OBJS = main.o

.SUFFIXES:	.cpp

all:	${LOCAL_OBJS} sub
	${CXX} ${DFLAGS} ${OBJS} -o ${EXE} ${LIBS}

sub:
	cd jpeg && $(MAKE)
	cd io && $(MAKE)
	cd video/VideoObjects && $(MAKE)
	cd openmax && $(MAKE)
	cd platform && $(MAKE)

include $(LOCAL_OBJS:.o=.d)

.cpp.o:
	${CXX} ${CFLAGS} -c $< -o $@

clean_deps:
	find . -name "*.d" -exec rm {} \;

clean:	clean_deps
	find . -name "*.o" -exec rm {} \;
	rm -f ${EXE}

%.d : %.cpp
	set -e; $(CXX) -MM $(CFLAGS) $< \
                | sed 's^\($*\)\.o[ :]*^\1.o $@ : ^g' > $@; \
                [ -s $@ ] || rm -f $@
