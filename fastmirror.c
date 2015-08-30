#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "videodev2.h"
#include "VG/openvg.h"
#include "VG/vgu.h"
#include "EGL/egl.h"
#include "GLES/gl.h"
#include "bcm_host.h"
#include <jpeglib.h>
#include <gd.h>

// Debug and Info output
#define WARN(s, args...)  fprintf(stdout, s, ## args)
#define ERROR(s, args...) fprintf(stderr, s, ## args)
#define DEBUG(s, args...) fprintf(stdout, s, ## args)
#define INFO(s, args...)  fprintf(stdout, s, ## args)

// Types
typedef struct 
{
	void *start;
	size_t length;
} TSharedBuffer;

// Globals
int videoFD = 0;
struct v4l2_capability camCapabilities;
struct v4l2_format camFormats;
struct v4l2_requestbuffers req;
struct v4l2_buffer buf;
int camInputCount = 0;
int camSelectedInput = -1;
int camSelectedPixelFormat = -1;
int camWidth = 0;
int camHeight = 0;
TSharedBuffer *sharedBuffers;
int sharedBufferCount = -1;
int windowWidth, windowHeight;
unsigned char *offscreenBuffer;


time_t startTime;
void startTimer()
{
	startTime = time(&startTime);
}

void elapsedTime()
{
	time_t now;
	now = time(&now);
	printf("Elapsed time: %ds\n", now-startTime);
}

void fps(int frameCount)
{
	time_t now;
	now = time(&now);
	printf("FPS: %f\n", (double)frameCount / (now-startTime));	
}

int detectWebCam()
{
	// 1. Open the webam
	//videoFD = open("/dev/video0", O_RDWR | O_NONBLOCK);
	videoFD = open("/dev/video0", O_RDWR );
	INFO("videoFD = %d\n", videoFD);
	if (-1 == videoFD)
	{
		ERROR("Unable to open WebCame on /dev/video0");
		return -1;
	}

	// 2. Check capabilities
	if(ioctl(videoFD, VIDIOC_QUERYCAP, &camCapabilities) < 0)
	{
		ERROR("Cam Not a V4L2 device?");
		return(-1);
	}
	INFO("---- WebCam Capabilities ----\n");
	INFO("Driver:    %s\n", camCapabilities.driver);
	INFO("Device:    %s\n", camCapabilities.card);
	INFO("Bus Info:  %s\n", camCapabilities.bus_info);
	INFO("Version:   %d\n", camCapabilities.version);
	INFO("Caps Bits: ");
	if (camCapabilities.capabilities & V4L2_CAP_VIDEO_CAPTURE) INFO("CAPTURE ");
	if (camCapabilities.capabilities & V4L2_CAP_VIDEO_OUTPUT) INFO("OUTPUT ");
	if (camCapabilities.capabilities & V4L2_CAP_VIDEO_OVERLAY) INFO("OVERLAY ");
	if (camCapabilities.capabilities & V4L2_CAP_VBI_CAPTURE) INFO("VBI-CAPTURE ");
	if (camCapabilities.capabilities & V4L2_CAP_VBI_OUTPUT) INFO("VBI-OUTPUT ");
	if (camCapabilities.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE) INFO("SLICED-VBI-CAP ");
	if (camCapabilities.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT) INFO("SLICED-VBI-OUT ");
	if (camCapabilities.capabilities & V4L2_CAP_RDS_CAPTURE) INFO("RDS-CAPTURE ");
	if (camCapabilities.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY) INFO("OUTPUT-OVERLAY ");
	if (camCapabilities.capabilities & V4L2_CAP_HW_FREQ_SEEK) INFO("FREQUENCY-SEEKING ");
	if (camCapabilities.capabilities & V4L2_CAP_TUNER) INFO("TUNER ");
	if (camCapabilities.capabilities & V4L2_CAP_AUDIO) INFO("AUDIO ");
	if (camCapabilities.capabilities & V4L2_CAP_RADIO) INFO("RADIO ");
	if (camCapabilities.capabilities & V4L2_CAP_READWRITE) INFO("READWRITE ");
	if (camCapabilities.capabilities & V4L2_CAP_ASYNCIO) INFO("ASYNCIO ");
	if (camCapabilities.capabilities & V4L2_CAP_STREAMING) INFO("STREAMING ");
	INFO("\n\n");

	if (!camCapabilities.capabilities & V4L2_CAP_VIDEO_CAPTURE)
	{
		ERROR("WebCam doesn't support capture!");
		return -1;
	}
	if (!camCapabilities.capabilities & V4L2_CAP_STREAMING)
	{
		ERROR("WebCam doesn't support streaming!");
		return -1;
	}

	// 3. Enumerate inputs and select one
	struct v4l2_input camInput;
	int err;
	memset(&camInput, 0, sizeof(camInput));
	INFO("---- Available Inputs ----\n");
	do
	{
		camInput.index = camInputCount;
		err = ioctl(videoFD, VIDIOC_ENUMINPUT, &camInput);
		if (!err) 
		{
			++camInputCount;
			INFO(" Input %d: %s [%s]\n", camInput.index, camInput.name, (camInput.type == V4L2_INPUT_TYPE_TUNER) ? "TUNER" : "CAMERA");

			if ((-1 == camSelectedInput) && (camInput.type == V4L2_INPUT_TYPE_CAMERA))
			{
				camSelectedInput = camInput.index;
			}
		}
	} while (!err);
	if (-1 == camSelectedInput)
	{
		ERROR("Unable to find an appropriate camera input\n");
		return -1;
	}

	INFO("Selecting input %d for camera\n", camSelectedInput);
	camInput.index = camSelectedInput;
	if (ioctl(videoFD, VIDIOC_S_INPUT, &camSelectedInput))
	{
		ERROR("Error selecting input\n");
		return -1;
	}

	// 4. Look for a usable pixel format
	INFO("\n---- Available Pixel Formats ----\n");
	struct v4l2_fmtdesc pixelFormat;
	memset(&pixelFormat, 0, sizeof(pixelFormat));
	int pixelEnum = 0;
	pixelFormat.index = pixelEnum;
	pixelFormat.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	while(ioctl(videoFD, VIDIOC_ENUM_FMT, &pixelFormat) != -1)
	{
		DEBUG("%i: [0x%08X] '%c%c%c%c' (%s)\n", pixelEnum,
		      pixelFormat.pixelformat,
		      pixelFormat.pixelformat >> 0,  pixelFormat.pixelformat >> 8,
		      pixelFormat.pixelformat >> 16, pixelFormat.pixelformat >> 24,
		      pixelFormat.description);
		
		if (V4L2_PIX_FMT_YUYV == pixelFormat.pixelformat)
		{
			camSelectedPixelFormat = pixelFormat.index;
		}
		memset(&pixelFormat, 0, sizeof(pixelFormat));
		pixelFormat.index = ++pixelEnum;
		pixelFormat.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	}
	if (camSelectedPixelFormat == -1)
	{
		ERROR("Unable to get YUYV pixel format\n");
		return -1;
	}
	INFO("Selecting pixel format #%d\n", camSelectedPixelFormat);

	// 5. Select pixel format from device
	struct v4l2_format candidateFormat;
	memset(&candidateFormat, 0, sizeof(candidateFormat));
	candidateFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	candidateFormat.fmt.pix.width = 320;
	candidateFormat.fmt.pix.height = 240;
	candidateFormat.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	candidateFormat.fmt.pix.field = V4L2_FIELD_ANY;
	err = ioctl(videoFD, VIDIOC_TRY_FMT, &candidateFormat);
	if (err)
	{
		ERROR("Failed to try format. Err=%d", err);
		return -1;
	}
	err = ioctl(videoFD, VIDIOC_S_FMT, &candidateFormat);
	if (err)
	{
		ERROR("Failed to set format. Err=%d", err);
		return -1;		
	}
	camWidth = candidateFormat.fmt.pix.width;
	camHeight = candidateFormat.fmt.pix.height;

	// 6. Request 30fps
	struct v4l2_streamparm setfps;
	memset(&setfps, 0, sizeof(setfps));
	
	setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	setfps.parm.capture.timeperframe.numerator = 1;
	setfps.parm.capture.timeperframe.denominator = 100;
	if(ioctl(videoFD, VIDIOC_S_PARM, &setfps) == -1)
	{
		/* Not fatal - just warn about it */
		WARN("Error setting frame rate:");
		WARN("VIDIOC_S_PARM: %s", strerror(errno));
		return(-1);
	}
	
	return 0;
}

int startStreaming()
{
	struct v4l2_requestbuffers reqBuffers;
	memset(&reqBuffers, 0, sizeof(reqBuffers));

	reqBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqBuffers.memory = V4L2_MEMORY_MMAP;
	reqBuffers.count = 5;

	if (-1 == ioctl (videoFD, VIDIOC_REQBUFS, &reqBuffers)) 
	{
		ERROR("Failed to request buffers from driver");
		return -1;
	}

	// We asked for 5, but can make do with 2 at a minimum
	if (reqBuffers.count < 2)
	{
		ERROR("Driver only gave us %d buffer(s). Not enough!\n");
		return -1;
	}

	sharedBufferCount = reqBuffers.count;
	sharedBuffers = calloc(reqBuffers.count, sizeof(TSharedBuffer));
	if (!sharedBuffers)
	{
		ERROR("Couldn't allocate buffer list\n");
		return -1;
	}

	int i;
	for (i=0; i<reqBuffers.count; ++i)
	{
		struct v4l2_buffer driverBuffer;
		memset(&driverBuffer, 0, sizeof(driverBuffer));
		driverBuffer.type = reqBuffers.type;
		driverBuffer.memory = V4L2_MEMORY_MMAP;
		driverBuffer.index = i;

		if (-1 == ioctl(videoFD, VIDIOC_QUERYBUF, &driverBuffer))
		{
			ERROR("Failure in VIDIOC_QUERYBUF\n");
			return -1;
		}

		sharedBuffers[i].length = driverBuffer.length;
		sharedBuffers[i].start  = mmap(	NULL, driverBuffer.length,
										PROT_READ | PROT_WRITE,
										MAP_SHARED, videoFD,
										driverBuffer.m.offset);
		if (MAP_FAILED == sharedBuffers[i].start)
		{
			ERROR("Failure in mmap()\n");
			return -1;
		}
	}

	// Now enqueue all buffers
	for (i=0; i<reqBuffers.count; ++i)
	{
		struct v4l2_buffer driverBuffer;
		memset(&driverBuffer, 0, sizeof(driverBuffer));

		driverBuffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		driverBuffer.memory = V4L2_MEMORY_MMAP;
		driverBuffer.index  = i;

		if(ioctl(videoFD, VIDIOC_QBUF, &driverBuffer) == -1)
		{
			ERROR("VIDIOC_QBUF failure: %s", strerror(errno));
			return -1;
		}
	}

	// ...and start streaming!
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(ioctl(videoFD, VIDIOC_STREAMON, &type) == -1)
	{
		ERROR("Error starting stream.");
		ERROR("VIDIOC_STREAMON: %s", strerror(errno));
		
		return(-1);
	}
	return 0;
}

void releaseBuffer(int bufferIndex)
{
	struct v4l2_buffer driverBuffer;
	memset(&driverBuffer, 0, sizeof(driverBuffer));

	driverBuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	driverBuffer.memory = V4L2_MEMORY_MMAP;
	driverBuffer.index = bufferIndex;

	if(ioctl(videoFD, VIDIOC_QBUF, &driverBuffer) == -1)
	{
		ERROR("VIDIOC_QBUF: %s", strerror(errno));
		if (EAGAIN == errno)
		{
			printf("It's just EAGAIN\n");
		}

		exit(-1);
	}
}

int grabFrame()
{
	struct v4l2_buffer driverBuffer;
	memset(&driverBuffer, 0, sizeof(driverBuffer));

	driverBuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	driverBuffer.memory = V4L2_MEMORY_MMAP;
	
	if(ioctl(videoFD, VIDIOC_DQBUF, &driverBuffer) == -1)
	{
		ERROR("VIDIOC_DQBUF: %s", strerror(errno));
		if (EAGAIN == errno)
		{
			printf("It's just EAGAIN\n");
		}
		exit(-1);
	}

	return driverBuffer.index;
}

unsigned char* getBufferPointer(int bufferIndex)
{
	return (unsigned char*)sharedBuffers[bufferIndex].start;
}

void initGraphics()
{
    init(&windowWidth, &windowHeight);
    offscreenBuffer = malloc(windowWidth * windowHeight * 4);
    if (!offscreenBuffer)
    {
    	printf("Failed to allocate offscreen buffer of size %d bytes\n", windowWidth * windowHeight * 4);
    	exit(-1);
    }
    INFO("Display dimensions: %dx%d\n", windowWidth, windowHeight);
}

void myrect(unsigned char *pFrame, int x, int y, int w, int h, uint32_t color)
{
	unsigned char *pScanline = &pFrame[y*windowWidth*4 + x*4];
	while (h-- > 0)
	{
		unsigned char *pWalker = pScanline;
		int xwalk;
		for (xwalk=0; xwalk<w;++xwalk)
		{
			*pWalker++ = color >> 16;
			*pWalker++ = (color >> 8) & 255;
			*pWalker++ = color & 255;
			*pWalker++ = 255;
		}

		pScanline += windowWidth*4;
	}
}

void processFrame(unsigned char *pFrame)
{
  Start(windowWidth, windowHeight);

  // Clear frame to black
  memset(offscreenBuffer, 0x00, windowWidth*windowHeight*4);

#if 1
  int x,y; 
  int yCells = 9;
  int xCells = 16;
  int cellWidth = camWidth/xCells;
  int cellHeight = camHeight/yCells;
  int windowCellWidth = windowWidth / xCells;
  int windowCellHeight = windowHeight / yCells;

  for(y = 0; y < yCells; y++)
    for(x = 0; x < xCells; x++)
    {
        uint8_t *pWalker = &pFrame[y*cellHeight*camWidth*2 + x*cellWidth*2];
        uint32_t accumulator = 0;

        int xincell, yincell;
        for (yincell=0; yincell<cellHeight;++yincell)
        {
          for (xincell=0; xincell<cellWidth;++xincell)
          {
			accumulator += pWalker[xincell*2];
          }
          pWalker += camWidth*2;
        }

	accumulator = accumulator / (cellWidth*cellHeight);
    int centerX = x*windowCellWidth + windowCellWidth/2;
    int centerY =  y*windowCellHeight + windowCellHeight/2;
centerY = windowHeight-centerY;
 //   int radius =  ((VGfloat)accumulator) / 256.0 * windowCellHeight;
 //   radius = radius / 2;
    int radius = windowCellHeight / 2;
    myrect(offscreenBuffer,
        centerX-radius, centerY-radius, radius*2, radius*2, 0x88888888 );
}
#endif

//makeimage(0.0,0.0,windowWidth, windowHeight, offscreenBuffer);
 // makeimage(100.0,100.0, gdImageSX(pFrame)+2, gdImageSY(pFrame), (char*)pFrame->tpixels[0]);
//vgSetPixels(0, 0, offscreenBuffer, 0, 0, windowWidth, windowHeight);

vgWritePixels(offscreenBuffer, windowWidth*4,
                               VG_sABGR_8888,
                               0, 0, windowWidth, windowHeight);
  End();
  }

int main()
{

  initGraphics();

  if (detectWebCam())
  {
  	exit(EXIT_FAILURE);
  }

  if (startStreaming())
  {
  	exit(EXIT_FAILURE);
  }

  printf("\nSleeping 2 seconds...\n\n");
  sleep(2);

  startTimer();

  int loops = 100;
  while (loops--)
  {
  	int bufferIndex = grabFrame();
  	unsigned char *pFrame = getBufferPointer(bufferIndex);

  	processFrame(pFrame);

  	releaseBuffer(bufferIndex);
  }

  elapsedTime();
  fps(100);
  return 0;
}

