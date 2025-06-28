// Copyright (C) 2013 Matt Ownby
// You are free to use this for educational/non-commercial purposes only
// http://my-cool-projects.blogspot.com

#ifdef USE_EGL
#ifdef IS_RPI

#include <stdio.h>
#include "platform/PlatformRPI.h"
#include "io/logger_console.h"
#include "common/common.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <stdexcept>
#include <vector>

using namespace std;

bool g_bQuitFlag = false;

void OnSigInt(int sig)
{
	printf("Properly shutting down...\n");
	g_bQuitFlag = true;
}

byteSA read_file(const char *strFilePath)
{
	FILE *F = fopen(strFilePath, "rb");
	if (!F)
	{
		throw runtime_error((string) "File could not be opened: " + strFilePath);
	}

	fseek(F, 0, SEEK_END);

	off_t stFileSize = ftell(F);

	// back to beginning
	fseek(F, 0, SEEK_SET);

	// allocate buffer
	byteSA buf;
	buf.resize(stFileSize);

	// fill buffer
	fread(buf.data(), 1, stFileSize, F);

	// close file handle
	fclose(F);

	return buf;
}

unsigned int RefreshTimer()
{
	unsigned int result = 0;
	struct timeval tv;

	if (gettimeofday(&tv, NULL) == 0)
	{
		tv.tv_sec &= 0x003FFFFF;        // to prevent overflow from *1000, we only care about relative time anyway
		result = (unsigned int) ((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
	}
	else
	{
		throw runtime_error("gettimeofday failed");
	}

	return result;
}

// entry point for RPIbroad platform
int main(int argc, char **argv)
{
	if (argc != 2)
	{
		printf("Usage: %s [jpeg path]\n", argv[0]);
		return 0;
	}

	// catch common signals so it properly shuts down
	signal(SIGINT, OnSigInt);
	signal(SIGTERM, OnSigInt);
	signal(SIGHUP, OnSigInt);

	IPlatformSPtr platform = PlatformRPI::GetInstance();
	PlatformRPI *pPlatform = (PlatformRPI *) platform.get();
	if (pPlatform == 0)
	{
		return 1;
	}

	ILoggerSPtr logger = ConsoleLogger::GetInstance();

	// init
	pPlatform->SetLogger(logger.get());
	IVideoObject *pVideo = pPlatform->VideoInit();
	IJPEGDecode *pJPEG = pPlatform->GetJPEGDecoder();

	byteSA fileJPEG = read_file(argv[1]);	// load in jpeg file
	const uint8_t *pBufJPEG = fileJPEG.data();
	size_t stSizeBytes = fileJPEG.size();

	// tell jpeg decoder what the buffer size needs to be (mandatory)
	pJPEG->SetInputBufSizeHint(stSizeBytes);
	//pJPEG->SetInputBufSizeHint(1024 * 500);

	unsigned int uStartTime = RefreshTimer();
	unsigned int uFramesDisplayed = 0;


	
	// main loop here, run as fast as possible to benchmark
	while (!g_bQuitFlag)
	{
		// decode
		if(uFramesDisplayed % 25 == 0) {
			pJPEG->DecompressJPEGStart(pBufJPEG, stSizeBytes);
			if(uFramesDisplayed < 600) pJPEG->WaitJPEGDecompressorReady();
		}


		// render
		pVideo->RenderFrame();
		pVideo->Flip();

		uFramesDisplayed++;
	}

	unsigned int uEndTime = RefreshTimer();
	unsigned int uTotalMs = uEndTime - uStartTime;

	printf("Total elapsed milliseconds: %u\n", uTotalMs);
	printf("Total frames displayed: %u\n", uFramesDisplayed);
	printf("Total frames / second is %f\n", (uFramesDisplayed * 1000.0) / uTotalMs);

	// shutdown
	platform.reset();

	return 0;
}

#endif // IS_RPI
#endif // USE_EGL
