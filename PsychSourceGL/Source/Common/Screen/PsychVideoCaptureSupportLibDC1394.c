/*
 *    PsychSourceGL/Source/Common/Screen/PsychVideoCaptureSupportLibDC1394.c
 *
 *    PLATFORMS:
 *
 *        GNU/Linux, Apple MacOS/X and (limited and experimental support for) MS-Windows
 *
 *    AUTHORS:
 *
 *        Mario Kleiner           mk              mario.kleiner@tuebingen.mpg.de
 *
 *    HISTORY:
 *
 *    DESCRIPTION:
 *
 *        This is the videocapture engine based on the free, open-source
 *        LibDC-1394 V2 library. It only supports video capture, no sound capture and
 *        no recording of video or sound.
 *
 *        It only supports machine vision cameras that are connected via the IEEE-1394
 *        Firewire-bus and that conform to the IIDC-1.0 (and later) standard for firewire
 *        machine vision cameras. These cameras are handled via the (statically linked)
 *        libdc1394-2.0 in combination with (on Linux) libraw1394.
 *
 *        The functions provide high performance streaming of uncompressed camera data over
 *        firewire and a lot of features (e.g., external sync. triggers) not useful for
 *        consumer cameras, but very useful for computer vision applications and things
 *        like eye-trackers, ...
 *
 *    NOTES:
 *
 *    TODO:
 *        - Implement Query/WaitTrigger support via new Basler smart feature api.
 */

#include "Screen.h"
#include <float.h>

#ifdef PTBVIDEOCAPTURE_LIBDC

#include <inttypes.h>

#if PSYCH_SYSTEM == PSYCH_LINUX
// On Linux we use the syslog facility for logging, and libraw1394 for low-level access
// to the firewire bus, libraw13943 at least on oldish Linux systems without juju-stack:
#include <libraw1394/raw1394.h>
#include <syslog.h>
#endif

// This is the master include file for libdc1394. It includes all other public header files:
#include <dc1394/dc1394.h>

// Sync modes and sync roles for multi-camera synchronization:
#define kPsychIsSyncMaster  1
#define kPsychIsSyncSlave   2
#define kPsychIsSoftSynced  4
#define kPsychIsBusSynced   8
#define kPsychIsHwSynced   16

// Record which defines all state for a capture device:
typedef struct {
    int valid;                        // Is this a valid device record? zero == Invalid.
    int capturehandle;                // Userspace visible capture handle.
    psych_mutex mutex;
    psych_condition condition;
    psych_thread recorderThread;      // Thread handle for background video recording thread.
    int frameAvail;                   // Number of frames in videosink.
    dc1394camera_t *camera;           // Ptr to a DC1394 camera object that holds the internal state for such cams.
    dc1394video_frame_t *frame;       // Ptr to a structure which contains the most recently captured/dequeued frame.
    dc1394video_frame_t *convframe;   // Ptr to a structuve which contains bayer- or YUV- converted frames.
    unsigned char* current_frame;     // Ptr to target buffer for most recent frame if video recorder thread is active in low-latency mode.
    unsigned char* pulled_frame;      // Ptr to fetched frame from video recorder thread (if active). This is the "front buffer" equivalent of current_frame.
    int syncmode;                     // 0 = free-running. 1 = sync-master, 2 = sync-slave, 4 = soft-sync, 8 = bus-sync, 16 = ttl-sync.
    int dropframes;                   // 1 == Always deliver most recent frame in FIFO, even if dropping of frames is neccessary.
    dc1394video_mode_t dc_imageformat;// Encodes image size and pixelformat.
    dc1394framerate_t dc_framerate;   // Encodes framerate.
    dc1394color_coding_t colormode;   // Encodes color encoding of cameras data.
    dc1394bayer_method_t debayer_method; // Type if debayering method to use.
    dc1394color_filter_t color_filter_override; // Override Bayer pattern to use.
    int dataconversionmode;           // Shall raw sensor data be requested or preprocessed data? Postprocess raw data or not?
    int reqlayers;                    // Requested number of layers (1=Luminance, 2 = LA, 3 = RGB, 4 = RGBA, 5 = YUV) in output texture.
    int actuallayers;                 // Actual number of layers (1=Luminance, 2 = LA, 3 = RGB, 4 = RGBA, 5 = YUV) in output texture.
    int bitdepth;                     // Requested or actual bpc - bits per color/luminance channel.
    int num_dmabuffers;               // Number of DMA ringbuffers to use in DMA capture.
    int nrframes;                     // Total count of decompressed images.
    int framecounter;                 // Total number of captured frames.
    double fps;                       // Acquisition framerate of capture device.
    int width;                        // Width x height of captured images.
    int height;
    double current_pts;               // Capture timestamp of current frame fetched from engine.
    double pulled_pts;                // Capture timestamp of actually pulled frame.
    int current_dropped;              // Dropped count for this fetch cycle...
    int pulled_dropped;               // -""- by master thread.
    int nr_droppedframes;             // Counter for dropped frames.
    int frame_ready;                  // Signals availability of new frames for conversion into GL-Texture.
    int grabber_active;               // Grabber running?
    PsychRectType roirect;            // Region of interest rectangle - denotes subarea of full video capture area.
    double avg_decompresstime;        // Average time spent in decompressor.
    double avg_gfxtime;               // Average time spent in buffer --> OpenGL texture conversion and statistics.
    int nrgfxframes;                  // Count of fetched textures.
    psych_bool recording_active;      // Is video recording active/requested on this camera?
    char* targetmoviefilename;        // Filename of a movie file to record.
    char* codecSpec;                  // Codec specification string for video recording.
    int moviehandle;                  // Handle of movie file to be written during video recording.
    unsigned int recordingflags;      // Flags used for recording and similar activities.
    unsigned int specialFlags;        // Additional flags set via 'SetCaptureParameter' functions.
} PsychVidcapRecordType;

static PsychVidcapRecordType vidcapRecordBANK[PSYCH_MAX_CAPTUREDEVICES];
static int numCaptureRecords = 0;
static psych_bool firsttime = TRUE;
static dc1394_t *libdc = NULL;		// Master handle to DC1394 library.

// Forward declaration of internal helper function:
void PsychDCDeleteAllCaptureDevices(void);
void PsychDCLibInit(void);

void PsychDCLibInit(void)
{
    if (firsttime) {
        // First time invocation:

        // Check if linker was able to dynamically runtime-link
        // the library on OSX, where we weak-link the library to
        // allow operation of Screen() without need to have libdc1394
        // installed, as long as user doesn't want to use it.
        #if PSYCH_SYSTEM == PSYCH_OSX
        if (NULL == dc1394_new) {
            printf("\n\n");
            printf("PTB-ERROR: Could not load and link libdc1394 firewire video capture library!\n");
            printf("PTB-ERROR: Most likely because the library is not (properly) installed on this\n");
            printf("PTB-ERROR: machine. Please read 'help VideoCaptureDC1394' for installation or\n");
            printf("PTB-ERROR: troubleshooting instructions. Firewire capture support is disabled\n");
            printf("PTB-ERROR: until you have resolved the problem.\n\n");
            PsychErrorExitMsg(PsychError_user, "Failed to load and link libDC1394 V2 Firewire video capture library! Capture engine unavailable.");
        }
        #endif

        // Initialize library:
        libdc = dc1394_new();
        if (libdc == NULL) PsychErrorExitMsg(PsychError_user, "Failed to initialize libDC1394 V2 Firewire video capture library! Capture engine unavailable.");
        firsttime = FALSE;
    }

    return;
}

/*    PsychGetVidcapRecord() -- Given a handle, return ptr to video capture record.
 *    --> Internal helper function of PsychVideoCaptureSupport.
 */
PsychVidcapRecordType* PsychGetVidcapRecord(int deviceIndex)
{
    // Sanity checks:
    if (deviceIndex < 0) {
        PsychErrorExitMsg(PsychError_user, "Invalid (negative) deviceIndex for video capture device passed!");
    }

    if (deviceIndex >= PSYCH_MAX_CAPTUREDEVICES) {
        PsychErrorExitMsg(PsychError_user, "Invalid deviceIndex for video capture device passed. Index exceeds number of registered devices!");
    }

    if (!vidcapRecordBANK[deviceIndex].valid) {
        PsychErrorExitMsg(PsychError_user, "Invalid deviceIndex for video capture device passed. No such device open!");
    }

    // Ok, we have a valid device record, return a ptr to it:
    return(&vidcapRecordBANK[deviceIndex]);
}

/*
 *     PsychVideoCaptureInit() -- Initialize video capture subsystem.
 *     This routine is called by Screen's RegisterProject.c PsychModuleInit()
 *     routine at Screen load-time. It clears out the vidcapRecordBANK to
 *     bring the subsystem into a clean initial state.
 */
void PsychDCVideoCaptureInit(void)
{
    // Initialize vidcapRecordBANK with NULL-entries:
    int i;
    for (i = 0; i < PSYCH_MAX_CAPTUREDEVICES; i++) {
        vidcapRecordBANK[i].valid = 0;
    }
    numCaptureRecords = 0;
    libdc = NULL;

    return;
}

/*
 *  void PsychExitVideoCapture() - Shutdown handler.
 *
 *  This routine is called by Screen('CloseAll') and on clear Screen time to
 *  do final cleanup. It deletes all capture objects
 *
 */
void PsychDCExitVideoCapture(void)
{
    // Release all capture devices
    PsychDCDeleteAllCaptureDevices();

    // Shutdown library:
    if (libdc && !firsttime) dc1394_free(libdc);
    libdc = NULL;

    // Reset firsttime flag to get a cold restart on next invocation of Screen:
    firsttime = TRUE;
    return;
}

/*
 *  PsychDeleteAllCaptureDevices() -- Delete all capture objects and release all associated ressources.
 */
void PsychDCDeleteAllCaptureDevices(void)
{
    int i;
    for (i=0; i<PSYCH_MAX_CAPTUREDEVICES; i++) {
        if (vidcapRecordBANK[i].valid) PsychDCCloseVideoCaptureDevice(i);
    }
    return;
}

/*
 *  PsychCloseVideoCaptureDevice() -- Close a capture device and release all associated ressources.
 */
void PsychDCCloseVideoCaptureDevice(int capturehandle)
{
    // Retrieve device record for handle:
    PsychVidcapRecordType* capdev = PsychGetVidcapRecord(capturehandle);

    // Stop capture immediately if it is still running:
    PsychDCVideoCaptureRate(capturehandle, 0, 0, NULL);

    // Initiate a power-down cycle to bring camera into standby mode:
    if (dc1394_camera_set_power(capdev->camera, DC1394_OFF)!=DC1394_SUCCESS) {
        printf("PTB-WARNING: Tried to power down camera %i, but powerdown-cycle failed for some reason!\n", capturehandle); fflush(NULL);
    }

    // Close & Shutdown camera, release ressources:
    dc1394_camera_free(capdev->camera);
    capdev->camera = NULL;

    // Release video recording related data:
    if (capdev->codecSpec) free(capdev->codecSpec);
    capdev->codecSpec = NULL;

    if (capdev->targetmoviefilename) free(capdev->targetmoviefilename);
    capdev->targetmoviefilename = NULL;

    PsychDestroyMutex(&capdev->mutex);
    PsychDestroyCondition(&capdev->condition);

    // Invalidate device record to free up this slot in the array:
    capdev->valid = 0;

    // Decrease counter of open capture devices:
    if (numCaptureRecords>0) numCaptureRecords--;

    // Done.
    return;
}

#if PSYCH_SYSTEM == PSYCH_OSX
extern dc1394_t* dc1394_new(void) __attribute__((weak_import));
#endif

/*
 *      PsychOpenVideoCaptureDevice() -- Create a video capture object.
 *
 *      This function tries to open and initialize a connection to a IEEE1394
 *      Firewire machine vision camera and return the associated captureHandle for it.
 *
 *      slotid = Number of slot in vidcapRecordBANK[] array to use for this camera.
 *      win = Pointer to window record of associated onscreen window.
 *      deviceIndex = Index of the grabber device. (Currently ignored)
 *      capturehandle = handle to the new capture object.
 *      capturerectangle = If non-NULL a ptr to a PsychRectangle which contains the ROI for capture.
 *      reqdepth = Number of layers for captured output textures. (0=Don't care, 1=LUMINANCE8, 2=LUMINANCE8_ALPHA8, 3=RGB8, 4=RGBA8)
 *      num_dmabuffers = Number of buffers in the ringbuffer queue (e.g., DMA buffers) - This is OS specific. Zero = Don't care.
 *      allow_lowperf_fallback = If set to 1 then PTB can use a slower, low-performance fallback path to get nasty devices working.
 *      targetmoviefilename and recordingflags are currently ignored, they would refer to video harddics recording capabilities.
 *
 *      bitdepth = Number of bits per color component / channel, aka bpc. Default is 8bpc, but some cams may be up to more.
 *
 */
psych_bool PsychDCOpenVideoCaptureDevice(int slotid, PsychWindowRecordType *win, int deviceIndex, int* capturehandle, double* capturerectangle,
                                         int reqdepth, int num_dmabuffers, int allow_lowperf_fallback, char* targetmoviefilename, unsigned int recordingflags, int bitdepth)
{
    PsychVidcapRecordType *capdev = NULL;
    dc1394camera_list_t   *cameras=NULL;
    unsigned int          numCameras;
    dc1394error_t         err;
    int                   i;
    char                  msgerr[10000];
    char*                 codecSpec = NULL;

    *capturehandle = -1;

    // Perform first-time init, if needed:
    PsychDCLibInit();

    // Slot 'slotid' will contain the record for our new capture object:

    // Initialize new record:
    vidcapRecordBANK[slotid].valid = 1;

    // Retrieve device record for slotid:
    capdev = PsychGetVidcapRecord(slotid);

    // Clear out the record to have a nice clean start:
    memset(capdev, 0, sizeof(PsychVidcapRecordType));
    
    // Need to set valid flag again after the memset():
    capdev->valid = 1;

    // Zero inits are not really needed, but anyhow...
    capdev->camera = NULL;
    capdev->grabber_active = 0;
    capdev->convframe = NULL;
    capdev->debayer_method = DC1394_BAYER_METHOD_NEAREST;
    capdev->capturehandle = slotid;

    PsychInitMutex(&capdev->mutex);
    PsychInitCondition(&capdev->condition, NULL);

    // Name of target movie file for video and audio recording specified?
    if (targetmoviefilename) {
        // Codec settings or type specified?
        if ((codecSpec = strstr(targetmoviefilename, ":CodecSettings="))) {
            // Replace ':' with a zero in targetmoviefilename, so it gets null-terminated
            // and only points to the actual movie filename:
            *codecSpec = 0;

            // Move after null-terminator:
            codecSpec++;

            // Replace the ':CodecSettings=' with the special keyword 'DEFAULTenc', so
            // so the default video codec is chosen, but the given settings override its
            // default parameters.
            strncpy(codecSpec, "DEFAULTenc    ", strlen("DEFAULTenc    "));

            if (strlen(codecSpec) == 0) PsychErrorExitMsg(PsychError_user, "Invalid (empty) :CodecSettings= parameter specified. Aborted.");
        } else if ((codecSpec = strstr(targetmoviefilename, ":CodecType="))) {
            // Replace ':' with a zero in targetmoviefilename, so it gets null-terminated
            // and only points to the actual movie filename:
            *codecSpec = 0;

            // Advance codecSpec to point to the actual codec spec string:
            codecSpec+= 11;

            if (strlen(codecSpec) == 0) PsychErrorExitMsg(PsychError_user, "Invalid (empty) :CodecType= parameter specified. Aborted.");
        } else {
            // No codec specified: Use our default encoder, the one that's been shown to
            // produce good results:
            codecSpec = strdup("DEFAULTenc");
        }

        // Copy codecSpec for later use in start of video capture and recording:
        capdev->codecSpec = strdup(codecSpec);
        if (strcmp(codecSpec, "DEFAULTenc") == 0) free(codecSpec);
        codecSpec = NULL;

        // Audio recording is so far unsupported, so clear the "with audio" flag:
        recordingflags &= ~2;

        if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Video%s recording into file [%s] enabled for camera with deviceIndex %i.\n",
            ((recordingflags & 2) ? " and audio" : ""), targetmoviefilename, deviceIndex);

        capdev->targetmoviefilename = strdup(targetmoviefilename);
        capdev->recording_active = TRUE;
    } else {
        capdev->recording_active = FALSE;
    }

    // Assign recordingflags:
    capdev->recordingflags = recordingflags;
    
    // Query a list of all available (connected) Firewire cameras:
    err = dc1394_camera_enumerate(libdc, &cameras);
    if (err != DC1394_SUCCESS) {
        // Failed to detect any cameras: Invalidate our record.
        capdev->valid = 0;
        printf("Unable to enumerate firewire cameras: %s\n", dc1394_error_get_string(err));
        PsychErrorExitMsg(PsychError_user, "Unable to detect Firewire cameras: Read 'help VideoCaptureDC1394' for troubleshooting tips.\n");
    }

    // Get number of detected cameras:
    numCameras = cameras->num;

    // Any cameras?
    if (numCameras<1) {
        // Failed to find a camera: Invalidate our record.
        capdev->valid = 0;
        PsychErrorExitMsg(PsychError_user, "Unable to find any Firewire camera: Please make sure that there are actually any connected.\n"
        "Please note that this capture engine only supports IIDC compliant machine vision cameras, not standard consumer DV cameras!");
    }

    // Specific cam requested?
    if (deviceIndex==-1) {
        // Nope. We just use the first one.
        capdev->camera = dc1394_camera_new_unit(libdc, cameras->ids[0].guid, cameras->ids[0].unit);
        printf("PTB-INFO: Opening the first Firewire camera on the IEEE1394 bus.\n");
        deviceIndex = 0;
    }
    else {
        // Does a camera with requested index exist?
        if (deviceIndex >= (int) numCameras) {
            // No such cam.
            capdev->valid = 0;
            sprintf(msgerr, "You wanted me to open the %i th camera (deviceIndex: %i), but there are only %i cameras available!",
                    deviceIndex + 1, deviceIndex, numCameras);
            PsychErrorExitMsg(PsychError_user, msgerr);
        }

        // Ok, valid device index: Open and assign cam:
        capdev->camera = dc1394_camera_new_unit(libdc, cameras->ids[deviceIndex].guid, cameras->ids[deviceIndex].unit);
        printf("PTB-INFO: Opening the %i. Firewire camera (deviceIndex=%i) out of %i cams on the IEEE1394 bus.\n",
        deviceIndex + 1, deviceIndex, numCameras);
    }

    fflush(NULL);

    // Prepare error message in case its needed below:
    sprintf(msgerr, "PTB-ERROR: Opening the %i. Firewire camera (deviceIndex=%i) failed! Failed to initialize camera with GUID %"PRIx64"\n", deviceIndex + 1, deviceIndex, cameras->ids[deviceIndex].guid);

    // Free the unused cameras:
    dc1394_camera_free_list(cameras);
    cameras=NULL;

    // Error abort if camera init failed:
    if(capdev->camera == NULL) {
        // Error abort here:
        capdev->valid = 0;
        PsychErrorExitMsg(PsychError_user, msgerr);
    }

    // ROI rectangle specified?
    if (capturerectangle) {
        PsychCopyRect(capdev->roirect, capturerectangle);
    }
    else {
        // Create empty pseudo-rect, which means "don't care":
        PsychMakeRect(capdev->roirect, 0, 0, 1 , 1);
    }

    // Our camera should be ready: Assign final handle.
    *capturehandle = slotid;

    // Increase counter of open capture devices:
    numCaptureRecords++;

    // Set zero framerate:
    capdev->fps = 0;

    // Set image size:
    capdev->width = 0;
    capdev->height = 0;

    // Requested output texture pixel depth in layers:
    capdev->reqlayers = reqdepth;

    // Assign requested bpc bitdepth. libDC1394 and IIDC standard support
    // 8 bpc or - for some higher end cameras - 16 bpc, so these are the
    // only useful values to request and we threshold accordingly. Note
    // that while requesting 16 bpc from a camera will lead to transfer of
    // 16 bpc values per pixel component, ie. 2 Bytes for each color channel,
    // if the camera supports > 8 bpc at all, this doesn't mean that the
    // 16 bpc "container" actually contains 16 bpc of net payload. Usually
    // most cameras will only support some lower bpc payloads, packed into the
    // 16 bpc value. Typical pro-class cameras may support 10 bpc or 12 bpc, ie.
    // a 16 bpc value whose 6 or 4 least significant bits are all zero and therefore
    // meaningless. Video -> Texture conversion code will take this into account,
    // trying a bit of texture memory if possible.
    capdev->bitdepth = (bitdepth <= 8) ? 8 : 16;
    if ((capdev->bitdepth > 8) && (PsychPrefStateGet_Verbosity() > 2)) printf("PTB-INFO: Requesting %i bpc data from camera %i.\n", bitdepth, deviceIndex);

    // Number of DMA ringbuffers to use in DMA capture mode: If no number provided (==0), set it to 8 buffers...
    capdev->num_dmabuffers = (num_dmabuffers>0) ? num_dmabuffers : 8;

    // Reset framecounter:
    capdev->framecounter = 0;
    capdev->nrframes = 0;
    capdev->grabber_active = 0;

    fflush(NULL);

    // Initiate a power-up cycle in case the camera is in standby mode:
    if (dc1394_camera_set_power(capdev->camera, DC1394_ON)!=DC1394_SUCCESS) {
        printf("PTB-WARNING: Tried to power up camera %i, but powerup-cycle failed for some reason!\n", deviceIndex); fflush(NULL);
    }

    // Initiate a reset-cycle on the camera to bring it into a clean state to start with:
    if (dc1394_camera_reset(capdev->camera)!=DC1394_SUCCESS) {
        printf("PTB-WARNING: Tried to reset camera %i, but reset cycle failed for some reason!\n", deviceIndex); fflush(NULL);
    }

    printf("PTB-INFO: Camera successfully opened...\n"); fflush(NULL);

    return(TRUE);
}

/* Internal function: Find best matching non-Format 7 mode:
 */
int PsychVideoFindNonFormat7Mode(PsychVidcapRecordType* capdev, double capturerate)
{
    int maximgarea = 0;
    dc1394video_mode_t maximgmode, mode;
    int i, j, w, h;
    unsigned int mw, mh;
    float framerate;
    dc1394framerate_t dc1394_framerate = DC1394_FRAMERATE_15; // Suppress compiler warning.
    dc1394framerates_t supported_framerates;
    dc1394video_modes_t video_modes;
    dc1394color_coding_t color_code;
    dc1394bool_t iscolor;
    uint32_t bpc;
    int nonyuvbonus;
    float bpp;
    int framerate_matched = false;
    int roi_matched = false;
    int mode_found = false;

    // Query supported video modes for this camera:
    dc1394_video_get_supported_modes(capdev->camera,  &video_modes);
    w = (int) PsychGetWidthFromRect(capdev->roirect);
    h = (int) PsychGetHeightFromRect(capdev->roirect);
    maximgmode = DC1394_VIDEO_MODE_MIN;

    for (i = 0; i < (int) video_modes.num; i++) {
        // Query properties of this mode and match them against our requirements:
        mode = video_modes.modes[i];

        // We first check non-format 7 types: Skip format-7 types...
        if (mode >= DC1394_VIDEO_MODE_FORMAT7_MIN) continue;

        // Pixeldepth supported?
        dc1394_get_color_coding_from_video_mode(capdev->camera, mode, &color_code);
        if (capdev->reqlayers > 0) {
            // Specific pixelsize requested:

            // Luminance only format?
            if (capdev->reqlayers < 3) {
                // mode 1: Only accept raw data, which we will pass on later unprocessed:
                if (capdev->dataconversionmode == 1 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;

                // mode 2: Only accept raw data, which we will post-process later on:
                if (capdev->dataconversionmode == 2 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;

                // mode 3: Only accept filtered post-processed data:
                if (capdev->dataconversionmode == 3 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;

                // mode 4: Only accept MONO data, but treat it as if it were RAW data and post-process it accordingly.
                // This is a workaround for broken cams which deliver sensor raw data as MONO instead of RAW, e.g.,
                // apparently some Bayer cams:
                if (capdev->dataconversionmode == 4 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;
                
                // If we end here, then mode is 0 aka don't care. We take raw or luminance data:
                if ((capdev->bitdepth <= 8 && color_code!=DC1394_COLOR_CODING_RAW8 && color_code!=DC1394_COLOR_CODING_MONO8) ||
                    (capdev->bitdepth  > 8 && color_code!=DC1394_COLOR_CODING_RAW16 && color_code!=DC1394_COLOR_CODING_MONO16)) continue;
            }

            // RGB true color format?
            if (capdev->reqlayers > 2) {
                // mode 1: Is not handled for 3 or 4 layer formats, because 3 or 4 layer formats always
                // require some post-processing of raw data, otherwise it would end up as 1 layer raw!
                
                // mode 2: Only accept raw data, which we will post-process later on:
                if (capdev->dataconversionmode == 2 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;
                
                // mode 4: Only accept MONO data, but treat it as if it were RAW data and post-process it accordingly.
                // This is a workaround for broken cams which deliver sensor raw data as MONO instead of RAW, e.g.,
                // apparently some Bayer cams:
                if (capdev->dataconversionmode == 4 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;
                
                // If we end here, then mode is 0 aka don't care or 3 aka only accept color data. We take any color data of 8/16 bpc depth:
                if ((dc1394_is_color(color_code, &iscolor) != DC1394_SUCCESS) || (!iscolor && (capdev->dataconversionmode != 2) && (capdev->dataconversionmode != 4)) ||
                    (dc1394_get_color_coding_data_depth(color_code, &bpc) != DC1394_SUCCESS) || (bpc != ((capdev->bitdepth <= 8) ? 8 : 16))) continue;
            }

            if (capdev->reqlayers == 5 && color_code!=DC1394_COLOR_CODING_YUV422 && color_code!=DC1394_COLOR_CODING_YUV411) continue;
        }
        else {
            // No specific pixelsize req. check our minimum requirements - Anything of 8/16 bpc depths:
            if ((dc1394_get_color_coding_data_depth(color_code, &bpc) != DC1394_SUCCESS) || (bpc != ((capdev->bitdepth <= 8) ? 8 : 16))) continue;
        }

        // ROI specified?
        dc1394_get_image_size_from_video_mode(capdev->camera, mode, &mw, &mh);
        if (capdev->roirect[kPsychLeft]==0 && capdev->roirect[kPsychTop]==0 && w==1 && h==1) {
            // No. Just find biggest one:
            if (mw*mh < (unsigned int) maximgarea) continue;
            maximgarea = mw * mh;
            maximgmode = mode;
            mode_found = true;
            roi_matched = true;
        }
        else {
            // Yes. Check for exact match, reject everything else:
            if (capdev->roirect[kPsychLeft]!=0 || capdev->roirect[kPsychTop]!=0 || w != (int) mw || h != (int) mh) continue;
            roi_matched = true;

            // Ok, this is a valid mode wrt. reqlayers and exact image size. Check for matching framerate:
            dc1394_video_get_supported_framerates(capdev->camera, mode, &supported_framerates);
            for (j = 0; j < (int) supported_framerates.num; j++) {
                dc1394_framerate = supported_framerates.framerates[j];
                dc1394_framerate_as_float(dc1394_framerate, &framerate);
                if (framerate >= capturerate) break;
            }
            dc1394_framerate_as_float(dc1394_framerate, &framerate);

            // nonyuvbonus is true, if a color capture mode is requested and the given mode
            // allows for RGB8 transfer instead of a YUV format. We try to prefer non-YUV
            // modes in selection of final mode, because YUV modes need a computationally
            // expensive conversion YUVxxx --> RGB8, whereas RGB8 doesn't need that.
            nonyuvbonus = ((capdev->reqlayers == 0) || (capdev->reqlayers > 2)) && (capdev->dataconversionmode == 0) && (color_code == DC1394_COLOR_CODING_RGB8);

            // Compare whatever framerate we've got as closest match against current fastest one:
            if ((framerate > maximgarea) ||
                (framerate == capturerate && nonyuvbonus) ||
                (framerate == maximgarea  && nonyuvbonus)) {
                maximgarea = (int) framerate;
                maximgmode = mode;
                mode_found = true;
            }
        }
    }

    // Sanity check: Any valid mode found?
    if (!mode_found) {
        // None found!
        PsychErrorExitMsg(PsychError_user, "Couldn't find any capture mode settings for your camera which satisfy your minimum requirements! Aborted.");
    }

    // maximgmode contains the best matching non-format-7 mode for our specs:
    mode = maximgmode;
    capdev->dc_imageformat = mode;

    // Query final color format and therefore pixel-depth:
    dc1394_get_color_coding_from_video_mode(capdev->camera, mode, &color_code);

    // This is the actuallayers delivered by the capture engine:
    capdev->actuallayers = (color_code == DC1394_COLOR_CODING_MONO8 || color_code == DC1394_COLOR_CODING_RAW8 || color_code == DC1394_COLOR_CODING_MONO16 || color_code == DC1394_COLOR_CODING_RAW16) ? 1 : 3;
    // Special case: conversion mode 2 or 4 for rgb layers, aka bayer-filter raw data as provided in raw or mono container into rgb data:
    if ((capdev->actuallayers == 1) && (capdev->reqlayers >= 3) && ((capdev->dataconversionmode == 2) || (capdev->dataconversionmode == 4))) capdev->actuallayers = 3;

    // Match this against requested actuallayers:
    if (capdev->reqlayers == 0) {
        // No specific depth requested: Just use native depth of captured image:
        capdev->reqlayers = capdev->actuallayers;
    }
    else {
        // Specific depth requested: Match it against native format:
        switch (capdev->reqlayers) {
            case 1:
                // Pure LUMINANCE8 requested:
            case 2:
                // LUMINANCE+ALPHA requested: This is not yet supported.
                if (capdev->actuallayers != capdev->reqlayers && PsychPrefStateGet_Verbosity()>1) {
                    printf("PTB-WARNING: Wanted a depth of %i layers (%s) for captured images, but capture device delivers\n"
                    "PTB-WARNING: %i layers! Adapted to capture device native format for performance reasons.\n",
                    capdev->reqlayers, (capdev->reqlayers==1) ? "LUMINANCE":"LUMINANCE+ALPHA", capdev->actuallayers);
                }
                capdev->reqlayers = capdev->actuallayers;
                break;
            case 3:
                // RGB requested:
            case 4:
                // RGBA requested: This is not yet supported.
                if (capdev->actuallayers != capdev->reqlayers && PsychPrefStateGet_Verbosity()>1) {
                    printf("PTB-WARNING: Wanted a depth of %i layers (%s) for captured images, but capture device delivers\n"
                    "PTB-WARNING: %i layers! Adapted to capture device native format for performance reasons.\n",
                    capdev->reqlayers, (capdev->reqlayers==3) ? "RGB":"RGB+ALPHA", capdev->actuallayers);
                }
                capdev->reqlayers = capdev->actuallayers;
                break;
            case 5:
                // YUV format requested: This is sort-of a RGB equivalent for us, for now:
                capdev->reqlayers = 3;
                break;
            default:
                capdev->reqlayers = 0;
                PsychErrorExitMsg(PsychError_user, "You requested a invalid capture image format (more than 4 layers). Aborted.");
        }
    }

    if (capdev->reqlayers > 1 && color_code != DC1394_COLOR_CODING_RGB8 && color_code != DC1394_COLOR_CODING_RGB16 && PsychPrefStateGet_Verbosity() > 2) {
        // Color capture with a non RGB mode aka a YUV or RAW/RAW encapsulated in MONO mode -- expensive.
        printf("PTB-INFO: Using a %s input color format instead of a RGB color format. This requires expensive color conversion and\n",
               (color_code == DC1394_COLOR_CODING_RAW8 || color_code == DC1394_COLOR_CODING_MONO8 || color_code == DC1394_COLOR_CODING_RAW16 || color_code == DC1394_COLOR_CODING_MONO16) ? "RAW" : "YUV");
        printf("PTB-INFO: can lead to higher cpu load and longer latencies. You may be able to avoid this with different settings\n");
        printf("PTB-INFO: for ROI, color depth and framerate...\n"); fflush(NULL);
    }

    // Query final image size and therefore ROI:
    dc1394_get_image_size_from_video_mode(capdev->camera, mode, &mw, &mh);
    capdev->roirect[kPsychLeft] = 0;
    capdev->roirect[kPsychTop] = 0;
    capdev->roirect[kPsychRight] = mw;
    capdev->roirect[kPsychBottom] = mh;

    // Recheck capture framerate:
    // We probe all available non mode-7 framerates of camera for the best match, aka
    // the slowest framerate equal or faster to the requested framerate:
    dc1394_video_get_supported_framerates(capdev->camera, mode, &supported_framerates);
    for (i = 0; i < (int) supported_framerates.num; i++) {
        dc1394_framerate = supported_framerates.framerates[i];
        dc1394_framerate_as_float(dc1394_framerate, &framerate);
        if (framerate >= capturerate) break;
    }
    dc1394_framerate_as_float(dc1394_framerate, &framerate);

    // Ok, we've got the closest match we could get. Good enough?
    if ((fabs(framerate - capturerate) < 0.5) || (capturerate == DBL_MAX)) {
        // Perfect match of delivered and requested framerate. Nothing to do so far...
        framerate_matched=true;
    }
    else {
        // No perfect match :(.
        framerate_matched=false;
        if(framerate < capturerate) {
            printf("PTB-WARNING: Camera does not support requested capture framerate of %f fps. Using maximum of %f fps instead.\n",
                              (float) capturerate, framerate);
        fflush(NULL);
        }
    }

    // Return framerate:
    capdev->dc_framerate = dc1394_framerate;

    if(PsychPrefStateGet_Verbosity() > 4){
        printf("PTB-INFO: Will use non-Format7 mode %i: Width x Height = %i x %i, fps=%f, colormode=%i ...\n",
        (int) mode, mw, mh, framerate, (int) color_code); fflush(NULL);
    }

    // Success!
    return(true);
}

/* Internal function: Find best matching Format 7 mode:
 * Returns calculated optimal iso-packet size.
 */
int PsychVideoFindFormat7Mode(PsychVidcapRecordType* capdev, double capturerate)
{
    float mindiff = 1000000;
    float mindifframerate = 0;
    int minpacket_size = 0;
    dc1394video_mode_t minimgmode, mode;
    int i, j, w, h, numF7Available=0;
    dc1394speed_t speed;
    unsigned int mw, mh, pbmin, pbmax, depth;
    int num_packets, packet_size;
    float framerate;
    dc1394framerate_t dc1394_framerate;
    dc1394framerates_t supported_framerates;
    dc1394video_modes_t video_modes;
    dc1394color_coding_t color_code;
    dc1394bool_t iscolor;
    uint32_t bpc;
    float bpp;
    int framerate_matched = false;
    int roi_matched = false;
    float bus_period;

    // Query IEEE1394 bus speed code and map it to bus_period:
    if (dc1394_video_get_iso_speed(capdev->camera, &speed)!=DC1394_SUCCESS) {
        PsychErrorExitMsg(PsychError_user, "Unable to query bus-speed - Start of video capture failed!");
    }

    // Special hack for Unibrain Fire-i: This camera can do 400 Megabit/second, but reports
    // a speed of 100 MBit after a cold-start! We enforce a 400 Megabit speed if this is a
    // Unibrain Fire-i:
    if (strstr(capdev->camera->vendor, "Unibrain") && strstr(capdev->camera->model, "Fire-i")) {
        // Unibrain Fire-i: Enforce correct speed:
        speed = DC1394_ISO_SPEED_400;
    }

    switch(speed) {
        case DC1394_ISO_SPEED_100:
            bus_period = 0.000500f;
            break;
        case DC1394_ISO_SPEED_200:
            bus_period = 0.000250f;
            break;
        case DC1394_ISO_SPEED_400:
            bus_period = 0.000125f;
            break;
        case DC1394_ISO_SPEED_800:
            bus_period = 0.0000625f;
            break;
        case DC1394_ISO_SPEED_1600:
            bus_period = 0.00003125f;
            break;
        case DC1394_ISO_SPEED_3200:
            bus_period = 0.000015625f;
            break;
        default:
            bus_period = 0; // To suppress compiler warnings.
            PsychErrorExitMsg(PsychError_user, "Unknown bus speed specification! Start of video capture failed!");
    }

    if(PsychPrefStateGet_Verbosity() > 4){
        printf("PTB-INFO: IEEE-1394 Firewire bus speed is %i Megabit/second --> Bus period is %f usecs.\n",
        (int) (100 << speed), bus_period * 1000000.0f);
    }

    // Query supported video modes for this camera:
    dc1394_video_get_supported_modes(capdev->camera,  &video_modes);
    minimgmode = DC1394_VIDEO_MODE_MIN;

    for (i = 0; i < (int) video_modes.num; i++) {
        // Query properties of this mode and match them against our requirements:
        mode = video_modes.modes[i];

        // Skip non-format-7 types...
        if (mode < DC1394_VIDEO_MODE_FORMAT7_MIN || mode > DC1394_VIDEO_MODE_FORMAT7_MAX) continue;

        // Increment count of available Format-7 modes:
        numF7Available++;

        if(PsychPrefStateGet_Verbosity() > 4){
            printf("PTB-Info: Probing Format-7 mode %i ...\n", mode);
        }

        // Pixeldepth supported? We reject anything except RAW8 or MONO8 for luminance formats
        // and RGB8 for color formats.
        dc1394_format7_get_color_coding(capdev->camera, mode, &color_code);
        if (capdev->reqlayers > 0) {
            // Specific pixelsize requested:
            
            // Luminance only format?
            if (capdev->reqlayers < 3) {
                // mode 1: Only accept raw data, which we will pass on later unprocessed:
                if (capdev->dataconversionmode == 1 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;
                
                // mode 2: Only accept raw data, which we will post-process later on:
                if (capdev->dataconversionmode == 2 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;
                
                // mode 3: Only accept filtered post-processed data:
                if (capdev->dataconversionmode == 3 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;
                
                // mode 4: Only accept MONO data, but treat it as if it were RAW data and post-process it accordingly.
                // This is a workaround for broken cams which deliver sensor raw data as MONO instead of RAW, e.g.,
                // apparently some Bayer cams:
                if (capdev->dataconversionmode == 4 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;
                
                // If we end here, then mode is 0 aka don't care. We take raw or luminance data:
                if ((capdev->bitdepth <= 8 && color_code!=DC1394_COLOR_CODING_RAW8 && color_code!=DC1394_COLOR_CODING_MONO8) ||
                    (capdev->bitdepth  > 8 && color_code!=DC1394_COLOR_CODING_RAW16 && color_code!=DC1394_COLOR_CODING_MONO16)) continue;
            }
            
            // RGB true color format?
            if (capdev->reqlayers > 2) {
                // mode 1: Is not handled for 3 or 4 layer formats, because 3 or 4 layer formats always
                // require some post-processing of raw data, otherwise it would end up as 1 layer raw!
                
                // mode 2: Only accept raw data, which we will post-process later on:
                if (capdev->dataconversionmode == 2 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_RAW8 : DC1394_COLOR_CODING_RAW16))) continue;
                
                // mode 4: Only accept MONO data, but treat it as if it were RAW data and post-process it accordingly.
                // This is a workaround for broken cams which deliver sensor raw data as MONO instead of RAW, e.g.,
                // apparently some Bayer cams:
                if (capdev->dataconversionmode == 4 && (color_code != ((capdev->bitdepth <= 8) ? DC1394_COLOR_CODING_MONO8 : DC1394_COLOR_CODING_MONO16))) continue;
                
                // If we end here, then mode is 0 aka don't care or 3 aka only accept color data. We take any color data of 8/16 bpc depth:
                if ((dc1394_is_color(color_code, &iscolor) != DC1394_SUCCESS) || (!iscolor && (capdev->dataconversionmode != 2) && (capdev->dataconversionmode != 4)) ||
                    (dc1394_get_color_coding_data_depth(color_code, &bpc) != DC1394_SUCCESS) || (bpc != ((capdev->bitdepth <= 8) ? 8 : 16))) continue;
            }

            if (capdev->reqlayers == 5 && color_code!=DC1394_COLOR_CODING_YUV422 && color_code!=DC1394_COLOR_CODING_YUV411) continue;
        }
        else {
            // No specific pixelsize req. check our minimum requirements - Anything of 8/16 bpc depths:
            if ((dc1394_get_color_coding_data_depth(color_code, &bpc) != DC1394_SUCCESS) || (bpc != ((capdev->bitdepth <= 8) ? 8 : 16))) continue;
        }
        
        // ROI specified?
        w = (int) PsychGetWidthFromRect(capdev->roirect);
        h = (int) PsychGetHeightFromRect(capdev->roirect);

        if (capdev->roirect[kPsychLeft]==0 && capdev->roirect[kPsychTop]==0 && w==1 && h==1) {
            // No. Just set biggest one for this mode:

            // Query maximum size for mode:
            if(dc1394_format7_get_max_image_size(capdev->camera, mode, &mw, &mh)!=DC1394_SUCCESS) continue;
            // Set zero position offset:
            if (dc1394_format7_set_image_position(capdev->camera, mode, 0, 0)!=DC1394_SUCCESS) continue;
            // Set maximum size:
            if (dc1394_format7_set_image_size(capdev->camera, mode, mw, mh)!=DC1394_SUCCESS) continue;
            w=mw; h=mh;
            roi_matched = true;
        }
        else {
            // Yes. Check for exact match, reject everything else:
            if(dc1394_format7_get_max_image_size(capdev->camera, mode, &mw, &mh)!=DC1394_SUCCESS) continue;
            if (w > (int) mw || h > (int) mh) continue;

            // This mode allows for a ROI as big as the one we request. Try to set it up:

            // First set zero position offset:
            if (dc1394_format7_set_image_position(capdev->camera, mode, 0, 0)!=DC1394_SUCCESS) continue;

            // Reject mode if size isn't supported:
            if (dc1394_format7_set_image_size(capdev->camera, mode, (unsigned int) w, (unsigned int) h)!=DC1394_SUCCESS) continue;

            // Now set real position:
            if (dc1394_format7_set_image_position(capdev->camera, mode, (unsigned int) capdev->roirect[kPsychLeft], (unsigned int) capdev->roirect[kPsychTop])!=DC1394_SUCCESS) continue;

            // If we reach this point, then we should have exactly the ROI we wanted.
            roi_matched = true;
        }

        // Try to set the requested framerate as well:
        // We need to calculate the ISO packet size depending on wanted framerate, Firewire bus speed,
        // image size and image depth + some IIDC spec. restrictions...

        // First we query the range of available packet sizes:
        if (dc1394_format7_get_packet_parameters(capdev->camera, mode, &pbmin, &pbmax)!=DC1394_SUCCESS) continue;
        // Special case handling:
        if (pbmin==0) pbmin = pbmax;

        // Compute number of ISO-Packets, assuming a 400 MBit bus (125 usec cycle time):
        num_packets = (int) (1.0/(bus_period * capturerate) + 0.5);
        if (num_packets < 1 || num_packets > 4095) {
            // Invalid num_packets. Adapt it to fit IIDC constraints:
            if (num_packets < 1) {
                num_packets = 1;
            }
            else {
                num_packets = 4095;
            }
        }
        num_packets*=8;
        if (dc1394_format7_get_data_depth(capdev->camera, mode, &depth)!=DC1394_SUCCESS) continue;

        packet_size = (int)((w * h * depth + num_packets - 1) /  num_packets);

        // Make sure that packet_size is an integral multiple of pbmin (IIDC constraint):
        if (packet_size < (int) pbmin) packet_size = pbmin;
        if (packet_size % pbmin != 0) {
            packet_size = packet_size - (packet_size % pbmin);
        }

        // Make sure that packet size is smaller than pbmax:
        while (packet_size > (int) pbmax) packet_size=packet_size - pbmin;

        // Ok, we should now have the closest valid packet size for the given ROI and framerate:
        // Inverse compute framerate for this packetsize:
        num_packets = (int) ((w * h * depth + (packet_size*8) - 1)/(packet_size*8));
        framerate = 1.0/(bus_period * (float) num_packets);

        // Compare whatever framerate we've got as closest match against current fastest one:
        if (fabs(capturerate - framerate) < mindiff) {
            mindiff = fabs(capturerate - framerate);
            mindifframerate = framerate;
            minimgmode = mode;
            minpacket_size = packet_size;
        }

        if(PsychPrefStateGet_Verbosity() > 4){
            if (capdev->roirect[kPsychLeft]!=0 || capdev->roirect[kPsychTop]!=0 || capdev->roirect[kPsychRight]!=1 || capdev->roirect[kPsychBottom]!=1) {
                printf("PTB-INFO: Checking Format-7 mode %i: ROI = [l=%f t=%f r=%f b=%f] , FPS = %f\n", mode, (float) capdev->roirect[kPsychLeft], (float) capdev->roirect[kPsychTop],
                (float) capdev->roirect[kPsychRight], (float) capdev->roirect[kPsychBottom], framerate);
            }
            else {
                printf("PTB-INFO: Checking Format-7 mode %i: ROI = [l=0 t=0 r=%i b=%i] , FPS = %f\n", mode, w, h, framerate);
            }
        }

        // Test next mode...
    }

    // Sanity check: Any valid mode found?
    if (minimgmode == DC1394_VIDEO_MODE_MIN || numF7Available == 0) {
        // None found!
        if (numF7Available > 0) {
            printf("PTB-INFO: Couldn't find any Format-7 capture mode settings for your camera which satisfy your minimum requirements!\n");
        }
        else {
            printf("PTB-INFO: This camera does not support *any* Format-7 capture modes.\n");
        }
        printf("PTB-INFO: Will now try standard (non Format-7) capture modes for the best match and try to use that...\n");
        return(0);
    }

    // Success (more or less...):

    // minimgmode contains the best matching Format-7 mode for our specs:
    mode = minimgmode;
    capdev->dc_imageformat = mode;
    capdev->dc_framerate = DC1394_FRAMERATE_MIN;
    packet_size = minpacket_size;
    framerate = mindifframerate;

    // Query final color format and therefore pixel-depth:
    dc1394_get_color_coding_from_video_mode(capdev->camera, mode, &color_code);

    // This is the actuallayers delivered by the capture engine:
    capdev->actuallayers = (color_code == DC1394_COLOR_CODING_MONO8 || color_code == DC1394_COLOR_CODING_RAW8 || color_code == DC1394_COLOR_CODING_MONO16 || color_code == DC1394_COLOR_CODING_RAW16) ? 1 : 3;

    // Special case: conversion mode 2 or 4 for rgb layers, aka bayer-filter raw data as provided in raw or mono container into rgb data:
    if ((capdev->actuallayers == 1) && (capdev->reqlayers >= 3) && ((capdev->dataconversionmode == 2) || (capdev->dataconversionmode == 4))) capdev->actuallayers = 3;
    
    // Match this against requested actuallayers:
    if (capdev->reqlayers == 0) {
        // No specific depth requested: Just use native depth of captured image:
        capdev->reqlayers = capdev->actuallayers;
    }
    else {
        // Specific depth requested: Match it against native format:
        switch (capdev->reqlayers) {
            case 1:
                // Pure LUMINANCE requested:
            case 2:
                // LUMINANCE+ALPHA requested: This is not yet supported.
                if (capdev->actuallayers != capdev->reqlayers && PsychPrefStateGet_Verbosity()>1) {
                    printf("PTB-WARNING: Wanted a depth of %i layers (%s) for captured images, but capture device delivers\n"
                    "PTB-WARNING: %i layers! Adapted to capture device native format for performance reasons.\n",
                    capdev->reqlayers, (capdev->reqlayers==1) ? "LUMINANCE":"LUMINANCE+ALPHA", capdev->actuallayers);
                }
                capdev->reqlayers = capdev->actuallayers;
                break;
            case 3:
                // RGB requested:
            case 4:
                // RGBA requested: This is not yet supported.
                if (capdev->actuallayers != capdev->reqlayers && PsychPrefStateGet_Verbosity()>1) {
                    printf("PTB-WARNING: Wanted a depth of %i layers (%s) for captured images, but capture device delivers\n"
                    "PTB-WARNING: %i layers! Adapted to capture device native format for performance reasons.\n",
                    capdev->reqlayers, (capdev->reqlayers==3) ? "RGB":"RGB+ALPHA", capdev->actuallayers);
                }
                capdev->reqlayers = capdev->actuallayers;
                break;
            case 5:
                // YUV format requested: This is sort-of a RGB equivalent for us, for now:
                capdev->reqlayers = 3;
                break;
            default:
                capdev->reqlayers = 0;
                PsychErrorExitMsg(PsychError_user, "You requested a invalid capture image format (more than 4 layers). Aborted.");
        }
    }

    // Query final image size and therefore ROI:
    dc1394_get_image_size_from_video_mode(capdev->camera, mode, &mw, &mh);
    capdev->roirect[kPsychRight]  = capdev->roirect[kPsychLeft] + mw;
    capdev->roirect[kPsychBottom] = capdev->roirect[kPsychTop]  + mh;

    // Ok, we've got the closest match we could get. Good enough?
    if ((mindiff < 0.5) || (capturerate == DBL_MAX)) {
        // Perfect match of delivered and requested framerate. Nothing to do so far...
        framerate_matched=true;
    }
    else {
        // No perfect match :(.
        framerate_matched=false;
        if(framerate < capturerate) {
            printf("PTB-WARNING: Camera does not support requested capture framerate of %f fps at given ROI setting. Using %f fps instead.\n",
                              (float) capturerate, framerate);
        fflush(NULL);
        }
    }

    // Assign computed framerate as best guess for real framerate, in case frame-interval query fails...
    capdev->fps = framerate;

    // Return packet_size:
    return(packet_size);
}

// Helper function: Push captured video frame data buffer into GStreamer video encoding pipeline:
static psych_bool PsychDCPushFrameToMovie(PsychVidcapRecordType* capdev, psych_uint16* input_image, psych_bool onMasterthread)
{
    // Yes. Need to add this video frame to the encoding pipeline.
    unsigned int twidth, theight, numChannels, bitdepth, i;
    unsigned int count;
    unsigned char* framepixels;
    
    // Get memory pointer to target memory buffer:
    framepixels = PsychGetVideoFrameForMoviePtr(capdev->moviehandle, &twidth, &theight, &numChannels, &bitdepth);
    
    // Validate number of color channels and bits per channel values for a match:
    if (numChannels != (unsigned int) capdev->actuallayers || bitdepth != ((capdev->bitdepth > 8) ? 16 : 8)) {
        printf("PTB-ERROR: Mismatch between number of color channels %i or bpc %i of captured video frame and number of channels %i or bpc %i of video recording target buffer!\n",
               capdev->actuallayers, ((capdev->bitdepth > 8) ? 16 : 8), numChannels, bitdepth);
        if (onMasterthread) {
            PsychErrorExitMsg(PsychError_system, "Encoding current captured video frame failed. Video format mismatch.");
        }
        else {
            printf("PTB-ERROR: Encoding current captured video frame on video recorder thread failed. Video format mismatch!\n");
        }
        
        return(FALSE);
    }
    
    // Dimensions match?
    if (twidth != (unsigned int) capdev->width || theight > (unsigned int) capdev->height) {
        printf("PTB-ERROR: Mismatch between size of captured video frame %i x %i and size of video recording target buffer %i x %i !\n", capdev->width, capdev->height, twidth, theight);
        if (onMasterthread) {
            PsychErrorExitMsg(PsychError_system, "Encoding current captured video frame failed. Video frame size mismatch.");
        }
        else {
            printf("PTB-ERROR: Encoding current captured video frame failed. Video frame size mismatch!\n");
        }
        
        return(FALSE);
    }
    
    // Target buffer available?
    if (framepixels) {
        // Copy the pixels:
        count = (twidth * theight * ((capdev->actuallayers == 3) ? 3 : 1) * ((capdev->bitdepth > 8) ? 2 : 1));
        
        // True bitdepth in the 9 to 15 bpc range?
        if (capdev->bitdepth > 8 && capdev->bitdepth < 16) {
            // Yes. Need to bit-shift, so the most significant bit of the video data,
            // gets placed in the 16th bit of the 16 bit word. This to make sure the
            // "dead bits" for bpc < 16 are the least significant bits and they are
            // all zeros. This makes sure that black is always all-zero and white is
            // always 0xffff - all ones:
            psych_uint16 *frameinwords = (psych_uint16*) input_image;
            psych_uint16 *frameoutwords = (psych_uint16*) framepixels;
            
            count /= 2; // Half as many words as bytes.
            for (i = 0; i < count; i++) *(frameoutwords++) = *(frameinwords++) << (16 - capdev->bitdepth);
        }
        else {
            // No, either 8 bpc or 16 bpc - A simple memcpy does the job efficiently:
            memcpy(framepixels, (const void*) input_image, count);
        }
        
        // Add to GStreamer encoding pipeline: Format is upright, and 1 video frame duration per frame:
        if (PsychAddVideoFrameToMovie(capdev->moviehandle, 1, FALSE) != 0) {
            if (onMasterthread) {
                PsychErrorExitMsg(PsychError_system, "Encoding current captured video frame failed. Failed to add frame to pipeline.");
            }
            else {
                printf("PTB-ERROR: Encoding current captured video frame failed. Failed to add frame to pipeline!\n");
            }
            
            return(FALSE);
        }
    }
    else {
        if (onMasterthread) {
            PsychErrorExitMsg(PsychError_system, "Encoding current captured video frame failed. No videobuffer available.");
        }
        else {
            printf("PTB-ERROR: Encoding current captured video frame failed. No videobuffer available!\n");
        }
        
        return(FALSE);
    }
    
    return(TRUE);
}

// Helper function: Convert image from dc1394 engine into final color format. Apply YUV->RGB colorspace
// conversion or debayering if neccessary:
static unsigned char* PsychDCPreprocessFrame(PsychVidcapRecordType* capdev)
{
    dc1394error_t error;
    int capturehandle = capdev->capturehandle;
    
    // input_image points to the image buffer in our cam:
    unsigned char* input_image = (unsigned char*) (capdev->frame->image);

    // Do we want to do something with the image data and have a scratch buffer for color conversion alloc'ed?
    if (capdev->convframe) {
        // Yes. Perform color-conversion YUV->RGB from cameras DMA buffer
        // into the scratch buffer and set scratch buffer as source for
        // all further operations:
        if (capdev->colormode == DC1394_COLOR_CODING_RAW8 || capdev->colormode == DC1394_COLOR_CODING_MONO8 ||
            capdev->colormode == DC1394_COLOR_CODING_RAW16 || capdev->colormode == DC1394_COLOR_CODING_MONO16) {
            // Non-Format 7 capture modes do not allow to query the camera for the type of its Bayer-pattern.
            // Therefore, if the bayer color_filter pattern is unknown due to non-Format-7 capture, assign
            // the pattern manually set by usercode as color_filter_override setting:
            if (capdev->frame->color_filter < DC1394_COLOR_FILTER_MIN || capdev->frame->color_filter > DC1394_COLOR_FILTER_MAX) {
                capdev->frame->color_filter = capdev->color_filter_override;
            }

            // Trigger bayer filtering for debayering via 'method':
            if (DC1394_SUCCESS != (error = dc1394_debayer_frames(capdev->frame, capdev->convframe, capdev->debayer_method))) {
                printf("PTB-WARNING: Debayering of raw sensor image data failed! %s\n", dc1394_error_get_string(error));
                if (error == DC1394_INVALID_COLOR_FILTER) {
                    printf("PTB-WARNING: Could not find out proper Bayer filter pattern for camera %i. Either select a\n", capturehandle);
                    printf("PTB-WARNING: Format-7 video capture mode to allow auto-detection, or use Screen('SetVideoCaptureParameter', ..., 'OverrideBayerPattern');\n");
                    printf("PTB-WARNING: to assign a suitable pattern manually.\n");
                }

                if (error == DC1394_INVALID_BAYER_METHOD) {
                    printf("PTB-WARNING: Invalid debayering method selected for camera %i. Select a different method via \n", capturehandle);
                    printf("PTB-WARNING: Screen('SetVideoCaptureParameter', ..., 'DebayerMethod');\n");
                }

                // Failure:
                printf("PTB-ERROR: Bayer filtering of video frame failed.\n");
                return(NULL);
            }
        }
        else {
            // Input data is in YUV format. Convert into RGB8:
            capdev->convframe->color_coding = DC1394_COLOR_CODING_RGB8;
            if (DC1394_SUCCESS != dc1394_convert_frames(capdev->frame, capdev->convframe)) {
                // Failure:
                printf("PTB-ERROR: Colorspace conversion of video frame failed.\n");
                return(NULL);
            }
        }
        
        // Success: Point to decoded image buffer:
        input_image = (unsigned char*) capdev->convframe->image;
    }

    return(input_image);
}

// Main function of the asynchronous background video recording thread:
static void* PsychDCRecorderThreadMain(void* capdevToCast)
{
    int rc;
    double tstart, tend;
    dc1394error_t error;
    unsigned char* input_image = NULL;

    // Get a pointer to our associated capture device:
    PsychVidcapRecordType* capdev = (PsychVidcapRecordType*) capdevToCast;

    // We are running at elevated realtime priority. Enter the while loop
    // which waits for new video frames from libDC1394 and pushes them into
    // the GStreamer movie recording pipeline and the receive slots/sinks for
    // live video capture data:
    while (TRUE) {
        PsychLockMutex(&capdev->mutex);

        // Start timestamp of processing cycle:
        PsychGetAdjustedPrecisionTimerSeconds(&tstart);

        // Abort?
        if (!capdev->grabber_active) break;

        // Check for new video frame in polling mode:
        error = dc1394_capture_dequeue(capdev->camera, DC1394_CAPTURE_POLICY_POLL, &(capdev->frame));

        // Success?
        if (error != DC1394_SUCCESS) {
            // Error! Abort:
            printf("PTB-ERROR: In background video recording thread: dc1394_capture_dequeue() failed [%s]! Aborting recording thread.\n", dc1394_error_get_string(error));
            break;
        }

        // New frame received?
        if (capdev->frame) {
            // Ok, new frame ready and dequeued from DMA ringbuffer:
            capdev->frame_ready = 1;

            // Store count of currently queued frames (in addition to the one just fetched).
            // This is an indication of how well the users script is keeping up with the video stream,
            // technically the number of frames that would need to be dropped to keep in sync with the
            // stream.
            capdev->current_dropped = (int) capdev->frame->frames_behind;

            // Increase counter of total number of captured frames by this camera:
            capdev->framecounter++;

            // Also increase "decompressed frames" counter, which is the same in case of threaded processing:
            capdev->nrframes++;

            // Query capture timestamp (in microseconds) and convert to seconds. This comes from the capture
            // engine with (theroretically) microsecond precision and is assumed to be pretty accurate:
            capdev->current_pts = ((double) capdev->frame->timestamp) / 1000000.0f;

            // On OS/X, current_pts is in gettimeofday() time, just as on Linux, but PTB's GetSecs
            // clock represents host uptime, not gettimeofday() time. Therefore we need to remap
            // on OS/X from  gettimeofday() time to regular PTB GetSecs() time, via an instant
            // clock calibration between both clocks and offset correction:
            #if PSYCH_SYSTEM == PSYCH_OSX
            struct timeval tv;
            gettimeofday(&tv, NULL);
            capdev->current_pts -= (((double) ((psych_uint64) tv.tv_sec * 1000000 + (psych_uint64) tv.tv_usec)) / 1000000.0f) - tend;
            #endif

            // Perform potential processing on image, e.g., debayering:
            input_image = PsychDCPreprocessFrame(capdev);
            if (NULL == input_image) {
                printf("PTB-ERROR: Bayer filtering or color space conversion of video frame in video recorder thread failed. Aborting recorder thread.\n");
                break;
            }
            
            // Push new frame to the GStreamer video encoding pipeline:
            if (capdev->recording_active && (capdev->moviehandle != -1) && (capdev->recordingflags & 16)) {
                // Yes. Push data to encoder now. Abort thread on failure to push/encode:
                if (!PsychDCPushFrameToMovie(capdev, (psych_uint16*) input_image, FALSE)) break;
            }
            
            // Provide new frame to masterthread / usercode, if frame delivery isn't disabled:
            if (!(capdev->recordingflags & 4)) {
                // Copy frame to "current frame" buffer if low-latency mode is active:
                if (capdev->dropframes) {
                    unsigned int count = (capdev->width * capdev->height * ((capdev->actuallayers == 3) ? 3 : 1) * ((capdev->bitdepth > 8) ? 2 : 1));

                    // Release previous one, if not fetched by now by masterthread:
                    if (capdev->current_frame) free(capdev->current_frame);
                    
                    // Allocate target buffer for most recent captured frame from video recorder thread:
                    capdev->current_frame = (unsigned char*) malloc(count);

                    // Copy image into it:
                    memcpy(capdev->current_frame, input_image, count);
                }
                
                // Signal availability of new video frame:
                capdev->frameAvail++;
                if ((rc = PsychSignalCondition(&(capdev->condition)))) {
                    printf("PTB-ERROR: In background video recording thread: PsychSignalCondition() failed [%s]!\n", strerror(rc));
                }
            }
            
            // Requeue the recently dequeued and no longer needed buffer:
            if (dc1394_capture_enqueue(capdev->camera, capdev->frame) != DC1394_SUCCESS) {
                printf("PTB-ERROR: Requeuing of used up video frame buffer in video recorder thread failed! Aborting recorder thread.\n");
                break;
            }

            // Update stats for decompression:
            PsychGetAdjustedPrecisionTimerSeconds(&tend);
            
            // Update avg. decompress time:
            capdev->avg_decompresstime += (tend - tstart);

            // Release mutex, so masterthread can get frame data or control camera/capture:
            PsychUnlockMutex(&capdev->mutex);
        }
        else {
            // No new frame received in this poll iteration.

            // Release mutex, so masterthread can get frame data or control camera/capture:
            PsychUnlockMutex(&capdev->mutex);
            
            // Sleep a bit, so we won't overload cpu. We are more agressive in low latency mode:
            PsychYieldIntervalSeconds((capdev->dropframes) ? 0.001 : 0.004);
        }

        // Next capture loop iteration...
    }

    // End of capture thread execution. Clean up and unlock mutex:
    capdev->frame_ready = 0;
    PsychUnlockMutex(&capdev->mutex);
    
    // Ok, we are done: Go and die peacefully...
    return(NULL);
}

// Helper for start/stop capture with bus-sync:
static void PsychDCEnableBusBroadcast(PsychVidcapRecordType* capdev, psych_bool enable)
{
    dc1394error_t err;

    // Only the sync master ever sends broadcast commands over the bus, so we only need
    // to en/disable its broadcast behaviour. All cameras always listen to bus broadcast,
    // broadcast reception can't be disabled, so we can only control the behaviour of a
    // camera wrt. sending commands. This means that you have to make sure that you only
    // connect cameras to a bus which should participate in synced capture - it is not
    // possible for a camera on such a bus to be free-running, as it will automatically
    // listen in on the broadcast commands from the sync master on the same bus.
    if (capdev->syncmode & kPsychIsSyncMaster) {
        if ((err = dc1394_camera_set_broadcast(capdev->camera, (enable) ? DC1394_TRUE : DC1394_FALSE)) != DC1394_SUCCESS) {
            // Failed! Shutdown DMA capture engine again:
            dc1394_capture_stop(capdev->camera);
            printf("PTB-ERROR: Could not %s bus broadcast for bus-synced multi-camera capture operation: %s\n", (enable) ? "enable" : "disable", dc1394_error_get_string(err));
            PsychErrorExitMsg(PsychError_user, "Unable to control bus broadcast for master camera for bus sync - Operation aborted!");
        }
    }
}

/*
 *  PsychVideoCaptureRate() - Start- and stop video capture.
 *
 *  capturehandle = Grabber to start-/stop.
 *  capturerate = zero == Stop capture, non-zero == Capture. Tries to choose smallest framerate
 *  that is equal or higher than requested rate. Special value capturerate == DBL_MAX will choose
 *  the fasted supported framerate for the other given capture settings.
 *  dropframes = 0 - Always deliver oldest frame in DMA ringbuffer. 1 - Always deliver newest frame.
 *               --> 1 == drop frames in ringbuffer if behind -- low-latency capture.
 *  startattime = Deadline (in system time) for which to wait before real start of capture.
 *  Returns Number of dropped frames during capture.
 */
int PsychDCVideoCaptureRate(int capturehandle, double capturerate, int dropframes, double* startattime)
{
    int dropped = 0;
    float framerate = 0;
    dc1394speed_t speed;
    dc1394video_mode_t maximgmode, mode;
    int i, j, w, h, packetsize;
    unsigned int mw, mh;
    dc1394framerate_t dc1394_framerate;
    dc1394framerates_t supported_framerates;
    dc1394video_modes_t video_modes;
    dc1394color_coding_t color_code;
    float bpp;
    uint32_t depth;
    int framerate_matched = false;
    int roi_matched = false;
    dc1394error_t err;
    int rc;

    // Retrieve device record for handle:
    PsychVidcapRecordType* capdev = PsychGetVidcapRecord(capturehandle);

    // Start- or stop capture?
    if (capturerate > 0) {
        // Start capture:
        if (capdev->grabber_active) PsychErrorExitMsg(PsychError_user, "You tried to start video capture, but capture is already started!");

        // Reset statistics:
        capdev->nr_droppedframes = 0;
        capdev->frame_ready = 0;

        // Select best matching mode for requested image size and pixel format:
        // ====================================================================

        w = (int) PsychGetWidthFromRect(capdev->roirect);
        h = (int) PsychGetHeightFromRect(capdev->roirect);

        // Can we (potentially) get along with a non-Format-7 mode?
        // Check minimum requirements for non-Format-7 mode and if the "prefer Format-7" specialFlags setting 1
        // is chosen:
        if ((capdev->specialFlags & 1) || !((capdev->roirect[kPsychLeft]==0 && capdev->roirect[kPsychTop]==0) &&
            ((capdev->roirect[kPsychRight]==1 && capdev->roirect[kPsychBottom]==1) || (w==640 && h==480) ||
            (w==800 && h==600) || (w==1024 && h==768) || (w==1280 && h==960) || (w==1600 && h==1200) ||
            (w==320 && h==240) || (w==160 && h==120)) &&
            (capturerate==1.875 || capturerate==3.75 || capturerate==7.5 || capturerate==15 || capturerate==30 ||
            capturerate==60 || capturerate==120 || capturerate==240))) {

                // Ok, the requested ROI and/or framerate is not directly supported by non-Format7 capture modes,
                // or usercode explicitely requested preference for Format-7 capture.
                // Try to find a good format-7 mode, fall back to NF-7 if format 7 doesn't work out:
                if ((packetsize=PsychVideoFindFormat7Mode(capdev, capturerate))==0) {
                    // Could not find good Format-7 mode! Try NF-7: This function will exit if we don't find
                    // a useful match at all:
                    PsychVideoFindNonFormat7Mode(capdev, capturerate);
                }
                // Ok either we have a format-7 mode ready to go (packetsize>0) or we have a default
                // non-format-7 mode ready (packetsize==0)...
        }
        else {
            // The requested combo of ROI and framerate should be supported by standard non-format-7 capture:
            // Try it and exit in case of non-match:
            PsychVideoFindNonFormat7Mode(capdev, capturerate);
            packetsize = 0;
        }

        // The PsychVideoFind(Non)Format7Mode() logic above already set the ROI for capture...

        // Setup capture hardware and DMA engine:
        if (dc1394_video_get_iso_speed(capdev->camera, &speed)!=DC1394_SUCCESS) {
            PsychErrorExitMsg(PsychError_user, "Unable to query bus-speed - Start of video capture failed!");
        }

        // Special hack for Unibrain Fire-i: This camera can do 400 Megabit/second, but reports
        // a speed of 100 MBit after a cold-start! We enforce a 400 Megabit speed if this is a
        // Unibrain Fire-i:
        if (strstr(capdev->camera->vendor, "Unibrain") && strstr(capdev->camera->model, "Fire-i")) {
            // Unibrain Fire-i: Enforce correct speed:
            speed = DC1394_ISO_SPEED_400;
            if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Unibrain Fire-i detected. Setting override bus speed 400 MBit...\n"); fflush(NULL);
        }

        // Assign final mode and framerate:
        dc1394_framerate = capdev->dc_framerate;
        mode = capdev->dc_imageformat;

        // Query final color format of captured data:
        dc1394_get_color_coding_from_video_mode(capdev->camera, mode, &color_code);
        capdev->colormode = color_code;

        // Setup DMA engine:
        // =================

        // Set ISO speed:
        if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Setting ISO speed... "); fflush(NULL);
        err = dc1394_video_set_iso_speed(capdev->camera, speed);
        if (err != DC1394_SUCCESS) {
            PsychErrorExitMsg(PsychError_user, "Unable to setup and start capture engine: Setting ISO speed failed!");
        }

        if(PsychPrefStateGet_Verbosity()>5) printf("...done.\n"); fflush(NULL);

        // Set Mode:
        if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Setting mode..."); fflush(NULL);
        err = dc1394_video_set_mode(capdev->camera, mode);
        if (err != DC1394_SUCCESS) {
            PsychErrorExitMsg(PsychError_user, "Unable to setup and start capture engine: Setting mode failed!");
        }
        if(PsychPrefStateGet_Verbosity()>5) printf("...done.\n"); fflush(NULL);

        // Set Framerate for non-Format7 modes: This is redundant in Format7 mode...
        if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Setting framerate (even in nonFormat-7!)..."); fflush(NULL);
        err = dc1394_video_set_framerate(capdev->camera, dc1394_framerate);
        if (err != DC1394_SUCCESS) {
            PsychErrorExitMsg(PsychError_user, "Unable to setup and start capture engine: Setting fixed framerate failed!");
        }
        if(PsychPrefStateGet_Verbosity()>5) printf("...done.\n"); fflush(NULL);

        // Format-7 capture?
        if (packetsize > 0) {
            // Format-7 capture DMA setup: We only set mode, color format, packetsize (and thereby framerate) and ROI (and thereby imagesize):
            if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Setting format-7 ROI..."); fflush(NULL);
            err = dc1394_format7_set_roi(capdev->camera, mode, color_code, packetsize, (unsigned int) capdev->roirect[kPsychLeft], (unsigned int) capdev->roirect[kPsychTop],
                                            (unsigned int) PsychGetWidthFromRect(capdev->roirect), (unsigned int) PsychGetHeightFromRect(capdev->roirect));
            if (err != DC1394_SUCCESS) {
                PsychErrorExitMsg(PsychError_user, "Unable to setup and start capture engine: Setting Format7 ROI failed!");
            }
            if(PsychPrefStateGet_Verbosity()>5) printf("...done. \n"); fflush(NULL);
        }
        else {
            // Non-Format-7 capture DMA setup: mode encodes image size, ROI and color format.
            // Nothing to do for now...
        }

        // Framedropping is no longer supported by libdc, so we implement it ourselves.
        // Store the 'dropframes' flag in our capdev struct, so the PsychDCGetTextureFromCapture()
        // knows how to handle this:
        capdev->dropframes = (dropframes > 0) ? 1 : 0;

        // Finally setup and start DMA video capture engine with default flags (auto allocation and
        // release of iso bandwidth and channels) and num_dmabuffers DMA buffer slots in internal video FIFO:
        if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Setting up DMA capture now!"); fflush(NULL);
        err = dc1394_capture_setup(capdev->camera, capdev->num_dmabuffers, DC1394_CAPTURE_FLAGS_DEFAULT);
        if (err != DC1394_SUCCESS) {
            // Failed! We clean up and fail:
            // Non-DMA path no longer supported by final libdc V2, so this is the end of the game...
            PsychErrorExitMsg(PsychError_system, "Unable to setup and start DMA capture engine - Start of video capture failed!");
        }

        if(PsychPrefStateGet_Verbosity()>5) printf(" DMA-Engine started.\n"); fflush(NULL);

        // Ready to go! Now we just need to tell the camera to start its capture cycle:

        // Wait until start deadline reached:
        if (*startattime != 0) PsychWaitUntilSeconds(*startattime);
        
        // Firewire bus-sync via bus-wide broadcast of iso-on command requested?
        if (capdev->syncmode & kPsychIsBusSynced) {
            // Yes. The master should broadcast its start command to all clients on
            // the bus:
            PsychDCEnableBusBroadcast(capdev, TRUE);            
        }

        // Only actually send start command for iso-transmission if this isn't a slave cam, or
        // if this is hardware-synced via external trigger cable, so iso-start doesn't actually
        // start capture of the slave camera but only engages it for trigger reception:
        if (!(capdev->syncmode & kPsychIsSyncSlave) || (capdev->syncmode & kPsychIsHwSynced)) {
            // Start isochronous data transfer:
            // This will engage the slave camera for trigger reception with hw-sync.
            // It will start capture and engage trigger signal emission on a master cam with hw-sync.
            // It will start capture on a free running cam or a soft-synced master camera.
            // It will start capture of a master camera and broadcast the start signal to all slave
            // cameras with bus-sync.
            if ((capdev->syncmode > 0) && (PsychPrefStateGet_Verbosity() > 3)) {
                printf("PTB-DEBUG: Enabling cameras ISO transmission in syncmode %i -- Start of capture...\n", capdev->syncmode);
                if (capdev->syncmode & kPsychIsBusSynced) printf("PTB-DEBUG: Bus synced operation.");
                if (capdev->syncmode & kPsychIsSoftSynced) printf("PTB-DEBUG: Software synced operation.");
                if (capdev->syncmode & kPsychIsHwSynced) printf("PTB-DEBUG: Hardware synced operation.");
                printf(" Start of %s.\n", (capdev->syncmode & kPsychIsSyncMaster) ? "master" : "slave");
            }

            if (capdev->syncmode & kPsychIsHwSynced) {
                // Disable use of external trigger signals on both master and slaves to start with:
                if (dc1394_external_trigger_set_power(capdev->camera, DC1394_OFF)) {
                    // Failed! Shutdown DMA capture engine again:
                    dc1394_capture_stop(capdev->camera);                    
                    PsychErrorExitMsg(PsychError_user, "Unable to disable trigger reception on camera - Phase I - Start of video capture failed!");
                }

                // If this is a slave, enable use of external trigger signals now:
                if (capdev->syncmode & kPsychIsSyncSlave) {
                    // Now enable trigger reception on slave:
                    if (dc1394_external_trigger_set_power(capdev->camera, DC1394_ON)) {
                        // Failed! Shutdown DMA capture engine again:
                        dc1394_capture_stop(capdev->camera);
                        PsychErrorExitMsg(PsychError_user, "Unable to enable trigger reception on slave camera - Phase II - Start of video capture failed!");
                    }
                }
            }
            
            if (dc1394_video_set_transmission(capdev->camera, DC1394_ON) !=DC1394_SUCCESS) {
                // Failed! Shutdown DMA capture engine again:
                dc1394_capture_stop(capdev->camera);

                PsychErrorExitMsg(PsychError_user, "Unable to start isochronous data transfer from camera - Start of video capture failed!");
            }

            // Is this a soft-synced configuration and the master has just started capturing?
            if ((capdev->syncmode & kPsychIsSoftSynced) && (capdev->syncmode & kPsychIsSyncMaster)) {
                // Yes. Quickly start all soft-synced slaves in our setup:
                // TODO: Would be better to have per-master lists of sync slaves for more flexibility.
                for (i = 0; i < PSYCH_MAX_CAPTUREDEVICES; i++) {
                    // Sync slave participating in this soft sync start operation?
                    if ((vidcapRecordBANK[i].valid) && (i != capturehandle) && (vidcapRecordBANK[i].syncmode & kPsychIsSoftSynced) &&
                        (vidcapRecordBANK[i].syncmode & kPsychIsSyncSlave)) {
                        // Yes. Start it:
                        PsychLockMutex(&vidcapRecordBANK[i].mutex);
                        if (dc1394_video_set_transmission(vidcapRecordBANK[i].camera, DC1394_ON) !=DC1394_SUCCESS) {
                            // Failed!
                            PsychUnlockMutex(&vidcapRecordBANK[i].mutex);                            
                            PsychErrorExitMsg(PsychError_user, "Unable to start isochronous data transfer of soft-synced slave camera - Start of sync video capture failed!");
                        }
                        PsychUnlockMutex(&vidcapRecordBANK[i].mutex);
                    }
                }
            }
        }
        
        // Record real start time:
        PsychGetAdjustedPrecisionTimerSeconds(startattime);

        if(PsychPrefStateGet_Verbosity()>5) printf("PTB-DEBUG: Capture engine fully running...\n"); fflush(NULL);

        // Firewire bus-sync via bus-wide broadcast of iso-on command requested?
        if (capdev->syncmode & kPsychIsBusSynced) {
            // Yes. Capture started, so the master should stop broadcasting all of its other commands to all clients on
            // the bus:
            PsychDCEnableBusBroadcast(capdev, FALSE);
        }

        // Map framerate enum to floating point value and assign it:
        if (packetsize == 0) {
            dc1394_framerate_as_float(dc1394_framerate, &framerate);
        }
        else {
            dc1394_format7_get_frame_interval(capdev->camera, mode, &framerate);
            if (framerate == 0) {
                framerate = capdev->fps;
            }
            else {
                // FIXME: This is most likely a wrong conversion constant 1e9, but
                // could not find the proper value in the spec and don't have a
                // camera which reports a sensible value, so this is pure speculation:

                // This does not seem to work at all on the only camera with which i could
                // test it (all other fw cams just report 0 == not supported),
                // so disable it for more sane results:
                // framerate = 1e9 / framerate;
                framerate = capdev->fps;
            }
        }

        capdev->fps = (double) framerate;

        // Setup size and position:
        capdev->width  = (int) PsychGetWidthFromRect(capdev->roirect);
        capdev->height = (int) PsychGetHeightFromRect(capdev->roirect);

        // Ok, capture is now started:
        capdev->grabber_active = 1;

        // Query effective bpc value of video mode: The number of actual bits of information per color/luminance channel:
        if (DC1394_SUCCESS != dc1394_video_get_data_depth(capdev->camera, &depth)) {
            printf("PTB-WARNING: Could not query data depth of video mode for camera %i - Assuming i got the requested %i bpc and hoping for the best.\n", capturehandle, capdev->bitdepth);
        }
        else {
            // Assign queried size: For a 8 bit mode typically 8 bpc, but for a 16 bit "container" mode,
            // it could be anywhere between 9 bpc and 16 bpc, depending on true sensor bit depth of camera:
            capdev->bitdepth = depth;
        }
        
        // Allocate conversion buffer if needed for YUV->RGB or Bayer->RGB conversions.
        if (capdev->actuallayers == 3 && color_code != DC1394_COLOR_CODING_RGB8 && color_code != DC1394_COLOR_CODING_RGB16) {
            // Software conversion of YUV or RAW -> RGB needed. Allocate a proper scratch-buffer:
            capdev->convframe = (dc1394video_frame_t*) malloc(sizeof(dc1394video_frame_t));
            memset(capdev->convframe, 0, sizeof(dc1394video_frame_t));            
        }

        if (PsychPrefStateGet_Verbosity() > 2) {
            printf("PTB-INFO: Capture started on device %i - Width x Height = %i x %i - Framerate: %f fps, bpc = %i.\n", capturehandle, capdev->width, capdev->height, capdev->fps, capdev->bitdepth);
        }

        // Now that capture is successfully started, do we also want to record video to a file?
        if (capdev->recording_active) {
            // Yes. Setup movie writing:
            capdev->moviehandle = PsychCreateNewMovieFile(capdev->targetmoviefilename, capdev->width, capdev->height, (double) framerate, capdev->actuallayers, ((capdev->bitdepth > 8) ? 16 : 8), capdev->codecSpec);

            // Failed?
            if (capdev->moviehandle == -1) {
                PsychErrorExitMsg(PsychError_user, "Setup of video recording failed.");
            }
            else {
                if (PsychPrefStateGet_Verbosity() > 2) {
                    printf("PTB-INFO: Video recording started on device %i into moviefile '%s'.\n", capturehandle, capdev->targetmoviefilename);
                }
            }

            // Async background recording requested?
            if (capdev->recordingflags & 16) {
                // Yes. Setup and start recording thread.
                PsychLockMutex(&capdev->mutex);
                capdev->frameAvail = 0;
                PsychUnlockMutex(&capdev->mutex);

                // Create and startup thread:
                if ((rc = PsychCreateThread(&(capdev->recorderThread), NULL, PsychDCRecorderThreadMain, (void*) capdev))) {
                    printf("PTB-ERROR: In Screen('StartVideoCapture'): Could not create background video recording thread [%s].\n", strerror(rc));
                    PsychErrorExitMsg(PsychError_system, "Thread creation for video recording failed!");
                }
                
                // Boost priority of recorderThread by 1 level and switch it to RT scheduling,
                // unless it is already RT-Scheduled. As the thread inherited our scheduling
                // priority from PsychCreateThread(), we only need to +1 tweak it from there:
                PsychSetThreadPriority(&(capdev->recorderThread), 2, 1);

                // Recorder thread is in charge of dequeuing video frames from libdc1394 and pushing it
                // into the movie recording pipeline and into our own receive slot or videosink.
                if (PsychPrefStateGet_Verbosity() > 3) {
                    printf("PTB-INFO: Video recording on device %i is performed on async background thread.\n", capturehandle);
                }
            }
        }
    }
    else {
        // Stop capture:
        if (capdev->grabber_active) {

            // recorderThread might be running for this camera, so use locking:
            PsychLockMutex(&capdev->mutex);
            
            // Firewire bus-sync via bus-wide broadcast of iso-on command requested?
            if (capdev->syncmode & kPsychIsBusSynced) {
                // Yes. The master should broadcast its stop command to all clients on the bus:
                PsychDCEnableBusBroadcast(capdev, TRUE);
            }
            
            // Only actually send stop command for iso-transmission if this isn't a slave cam, or
            // if this is hardware-synced via external trigger cable, so iso-stop doesn't actually
            // stop capture of the slave camera but only disable it for trigger reception:
            if (!(capdev->syncmode & kPsychIsSyncSlave) || (capdev->syncmode & kPsychIsHwSynced)) {
                // Stop isochronous data transfer:
                // This will disable the slave camera for trigger reception with hw-sync.
                // It will stop capture and disable trigger signal emission on a master cam with hw-sync.
                // It will stop capture on a free running cam or a soft-synced master camera.
                // It will stop capture of a master camera and broadcast the stop signal to all slave
                // cameras with bus-sync.
                if (dc1394_video_set_transmission(capdev->camera, DC1394_OFF) !=DC1394_SUCCESS) {
                    // Failed! Shutdown DMA capture engine again:
                    dc1394_capture_stop(capdev->camera);
                    PsychUnlockMutex(&capdev->mutex);
                    PsychErrorExitMsg(PsychError_user, "Unable to stop isochronous data transfer from camera - Stop of video capture failed!");
                }
                
                // Is this a soft-synced configuration and the master has just stopped capture?
                if ((capdev->syncmode & kPsychIsSoftSynced) && (capdev->syncmode & kPsychIsSyncMaster)) {
                    // Yes. Quickly stop all soft-synced slaves in our setup:
                    // TODO: Would be better to have per-master lists of sync slaves for more flexibility.
                    for (i = 0; i < PSYCH_MAX_CAPTUREDEVICES; i++) {
                        // Sync slave participating in this soft sync stop operation?
                        if ((vidcapRecordBANK[i].valid) && (i != capturehandle) && (vidcapRecordBANK[i].syncmode & kPsychIsSoftSynced) &&
                            (vidcapRecordBANK[i].syncmode & kPsychIsSyncSlave)) {
                            // Yes. Stop it:
                            PsychLockMutex(&vidcapRecordBANK[i].mutex);
                            if (dc1394_video_set_transmission(vidcapRecordBANK[i].camera, DC1394_OFF) !=DC1394_SUCCESS) {
                                // Failed!
                                PsychUnlockMutex(&vidcapRecordBANK[i].mutex);
                                PsychUnlockMutex(&capdev->mutex);
                                PsychErrorExitMsg(PsychError_user, "Unable to stop isochronous data transfer of soft-synced slave camera - Stop of sync video capture failed!");
                            }
                            PsychUnlockMutex(&vidcapRecordBANK[i].mutex);
                        }
                    }
                }

                if (capdev->syncmode & kPsychIsHwSynced) {
                    // Disable use of external trigger signals:
                    dc1394_external_trigger_set_power(capdev->camera, DC1394_OFF);
                }
            }

            // Firewire bus-sync via bus-wide broadcast of iso-on command requested?
            if (capdev->syncmode & kPsychIsBusSynced) {
                // Yes. Capture stopped, so the master should stop broadcasting all of its other commands to all clients on the bus:
                PsychDCEnableBusBroadcast(capdev, FALSE);
            }

            // Stop capture engine of current cam:
            dc1394_capture_stop(capdev->camera);

            // Ok, capture is now stopped.
            capdev->grabber_active = 0;

            // Done with camera access for now:
            PsychUnlockMutex(&capdev->mutex);

            // Video recording active? Then we should stop it now:
            if ((capdev->recording_active) && (capdev->moviehandle > -1)) {
                if (PsychPrefStateGet_Verbosity() > 2) printf("PTB-INFO: Stopping video recording on device %i and closing moviefile '%s'\n", capturehandle, capdev->targetmoviefilename);

                // Was async background recording active?
                if (capdev->recordingflags & 16) {
                    // Yes. Stop and destroy recording thread.
                    
                    // Wait for thread termination, cleanup and release the thread:
                    PsychDeleteThread(&(capdev->recorderThread));
                    
                    // Ok, thread is dead. Mark it as such:
                    capdev->recorderThread = (psych_thread) NULL;

                    capdev->frameAvail = 0;
                    
                    // Recorder thread is in charge of dequeuing video frames from libdc1394 and pushing it
                    // into the movie recording pipeline and into our own receive slot or videosink.
                    if (PsychPrefStateGet_Verbosity() > 3) {
                        printf("PTB-INFO: Async video recording thread on device %i stopped.\n", capturehandle);
                    }
                }
                
                // Flush and close video encoding pipeline, finalize and close movie file:
                if (PsychFinalizeNewMovieFile(capdev->moviehandle) == 0) {
                    capdev->moviehandle = -1;
                    PsychErrorExitMsg(PsychError_user, "Stop of video recording failed.");
                }

                // Done with recording:
                capdev->moviehandle = -1;
            }

            // Release dc1394video_frame_t convframe used for Debayering, if any:
            if (capdev->convframe) {
                if (capdev->convframe->image) free(capdev->convframe->image);
                free(capdev->convframe);
                capdev->convframe = NULL;
            }

            // Release current frame buffer, if any remaining:
            if (capdev->current_frame) free(capdev->current_frame);
            capdev->current_frame = NULL;
            
            // No frame ready anymore:
            capdev->frame_ready = 0;

            if (PsychPrefStateGet_Verbosity() > 2) {
                // Output count of dropped frames:
                if ((dropped = capdev->nr_droppedframes) > 0) {
                    printf("PTB-INFO: Video capture dropped %i frames on device %i to keep capture running in sync with realtime.\n", dropped, capturehandle);
                }
                
                printf("PTB-INFO: Total number of captured frames since this camera %i was opened: %i\n", capturehandle, capdev->framecounter);
                if (capdev->nrframes > 0) capdev->avg_decompresstime/= (double) capdev->nrframes;
                printf("PTB-INFO: Average time spent %s was %lf milliseconds.\n", ((capdev->recordingflags & 16) ? "in video processing thread" : "waiting/polling for new frames"), capdev->avg_decompresstime * 1000.0f);
                if (capdev->nrgfxframes > 0) capdev->avg_gfxtime/= (double) capdev->nrgfxframes;
                printf("PTB-INFO: Average time spent in GetCapturedImage (intensity calculation and Video->OpenGL texture conversion) was %lf milliseconds.\n", capdev->avg_gfxtime * 1000.0f);
            }
        }
    }

    // Reset framecounters and statistics:
    capdev->nrframes = 0;
    capdev->avg_decompresstime = 0;
    capdev->nrgfxframes = 0;
    capdev->avg_gfxtime = 0;

    // Return either the real capture framerate (at start of capture) or count of dropped frames - at end of capture.
    return((capturerate!=0) ? (int) (capdev->fps + 0.5) : dropped);
}


/*
 *  PsychGetTextureFromCapture() -- Create an OpenGL texturemap from a specific videoframe from given capture object.
 *
 *  win = Window pointer of onscreen window for which a OpenGL texture should be created.
 *  capturehandle = Handle to the capture object.
 *  checkForImage = >0 == Just check if new image available, 0 == really retrieve the image, blocking if necessary.
 *                   2 == Check for new image, block inside this function (if possible) if no image available.
 *
 *  timeindex = This parameter is currently ignored and reserved for future use.
 *  out_texture = Pointer to the Psychtoolbox texture-record where the new texture should be stored.
 *  presentation_timestamp = A ptr to a double variable, where the presentation timestamp of the returned frame should be stored.
 *  summed_intensity = An optional ptr to a double variable. If non-NULL, then the average of intensities over all channels and pixels is calculated and returned.
 *  outrawbuffer = An optional ptr to a memory buffer of sufficient size. If non-NULL, the buffer will be filled with the captured raw image data, e.g., for use inside Matlab or whatever...
 *  Returns Number of pending or dropped frames after fetch on success (>=0), -1 if no new image available yet, -2 if no new image available and there won't be any in future.
 */
int PsychDCGetTextureFromCapture(PsychWindowRecordType *win, int capturehandle, int checkForImage, double timeindex,
                                 PsychWindowRecordType *out_texture, double *presentation_timestamp, double* summed_intensity, rawcapimgdata* outrawbuffer)
{
    GLuint texid;
    int w, h;
    double targetdelta, realdelta, frames;
    psych_uint64 intensity = 0;
    unsigned int count, i;
    unsigned char* pixptr;
    psych_uint16* pixptrs;
    psych_bool newframe = FALSE;
    psych_bool frame_ready;
    double tstart, tend;
    unsigned int pixval, alphacount;
    dc1394error_t error;
    int rc;
    int nrdropped = 0;
    unsigned char* input_image = NULL;

    // Retrieve device record for handle:
    PsychVidcapRecordType* capdev = PsychGetVidcapRecord(capturehandle);

    // Compute width and height for later creation of textures etc. Need to do this here,
    // so we can return the values for raw data retrieval:
    w=capdev->width;
    h=capdev->height;

    // If a outrawbuffer struct is provided, we fill it with info needed to allocate a
    // sufficient memory buffer for returned raw image data later on:
    if (outrawbuffer) {
        outrawbuffer->w = w;
        outrawbuffer->h = h;
        outrawbuffer->depth = ((capdev->actuallayers == 3) ? 3 : 1);
        outrawbuffer->bitdepth = (capdev->bitdepth > 8) ? 16 : 8;
    }

    int waitforframe = (checkForImage > 1) ? 1:0; // Blocking wait for new image requested?

    // A mode 4 means "no op" with the libdc capture engine.
    if (checkForImage == 4) return(0);

    // Take start timestamp for timing stats:
    PsychGetAdjustedPrecisionTimerSeconds(&tstart);

    // Should we just check for new image?
    if (checkForImage) {
        if (capdev->grabber_active == 0) {
            // Grabber stopped. We'll never get a new image:
            return(-2);
        }

        // Synchronous frame fetch from masterthread?
        if (!(capdev->recordingflags & 16)) {
            // Capture handled by masterthread.

            // Reset current dropped count to zero:
            capdev->current_dropped = 0;

            // Grabber active: Polling mode or wait for new frame mode?
            if (waitforframe) {
                // Check for image in blocking mode: We actually try to capture a frame in
                // blocking mode, so we will wait here until a new frame arrives.
                error = dc1394_capture_dequeue(capdev->camera, DC1394_CAPTURE_POLICY_WAIT, &(capdev->frame));

                if (error == DC1394_SUCCESS) {
                    // Ok, new frame ready and dequeued from DMA ringbuffer. We'll return it on next non-poll invocation.
                    capdev->frame_ready = 1;
                }
                else {
                    // Blocking wait failed! Somethings seriously wrong:
                    PsychErrorExitMsg(PsychError_system, "Blocking wait for new frame failed!!!");
                }
            }
            else {
                // Check for image in polling mode: We capture in non-blocking mode:
                if (dc1394_capture_dequeue(capdev->camera, DC1394_CAPTURE_POLICY_POLL,  &(capdev->frame)) == DC1394_SUCCESS) {
                    // Ok, call succeeded. If the 'frame' pointer is non-NULL then there's a new frame ready and dequeued from DMA
                    // ringbuffer. We'll return it on next non-poll invocation. Otherwise no new video data ready yet:
                    capdev->frame_ready = (capdev->frame != NULL) ? 1 : 0;
                }
                else {
                    // Polling wait failed for some reason...
                    PsychErrorExitMsg(PsychError_system, "Polling for new video frame failed!!!");
                }
            }

            if (capdev->frame_ready) {
                // Store count of currently queued frames (in addition to the one just fetched).
                // This is an indication of how well the users script is keeping up with the video stream,
                // technically the number of frames that would need to be dropped to keep in sync with the
                // stream.
                capdev->current_dropped = (int) capdev->frame->frames_behind;

                // Ok, at least one new frame ready. If more than one frame has queued up and
                // we are in 'dropframes' mode, ie. we should always deliver the most recent available
                // frame, then we quickly fetch & discard all queued frames except the last one.
                while((capdev->dropframes) && ((int) capdev->frame->frames_behind > 0)) {
                    // We just poll - fetch the frames. As we know there are some queued frames, it
                    // doesn't matter if we poll or block, but polling sounds like a bit less overhead
                    // at the OS level:

                    // First enqueue the recently dequeued buffer...
                    if (dc1394_capture_enqueue((capdev->camera), (capdev->frame)) != DC1394_SUCCESS) {
                        PsychErrorExitMsg(PsychError_system, "Requeuing of discarded video frame failed while dropping frames (dropframes=1)!!!");
                    }

                    // Then fetch the next one:
                    if (dc1394_capture_dequeue(capdev->camera, DC1394_CAPTURE_POLICY_POLL,  &(capdev->frame)) != DC1394_SUCCESS || capdev->frame == NULL) {
                        // Polling failed for some reason...
                        PsychErrorExitMsg(PsychError_system, "Polling for new video frame failed while dropping frames (dropframes=1)!!!");
                    }

                    // Increase counter of total number of captured frames by this camera:
                    capdev->framecounter++;
                }

                // Update stats for decompression:
                PsychGetAdjustedPrecisionTimerSeconds(&tend);

                // Increase counter of decompressed frames:
                capdev->nrframes++;

                // Increase counter of total number of captured frames by this camera:
                capdev->framecounter++;
                
                // Update avg. decompress time:
                capdev->avg_decompresstime+=(tend - tstart);

                // Query capture timestamp (in microseconds) and convert to seconds. This comes from the capture
                // engine with (theroretically) microsecond precision and is assumed to be pretty accurate:
                capdev->current_pts = ((double) capdev->frame->timestamp) / 1000000.0f;

                // On OS/X, current_pts is in gettimeofday() time, just as on Linux, but PTB's GetSecs
                // clock represents host uptime, not gettimeofday() time. Therefore we need to remap
                // on OS/X from  gettimeofday() time to regular PTB GetSecs() time, via an instant
                // clock calibration between both clocks and offset correction:
                #if PSYCH_SYSTEM == PSYCH_OSX
                struct timeval tv;
                gettimeofday(&tv, NULL);
                capdev->current_pts -= (((double) ((psych_uint64) tv.tv_sec * 1000000 + (psych_uint64) tv.tv_usec)) / 1000000.0f) - tend;
                #endif
            }

            // Assign final ready state:
            frame_ready = capdev->frame_ready;
            capdev->pulled_pts = capdev->current_pts;
            capdev->pulled_dropped = capdev->current_dropped;
        }
        else {
            // Capture and recording handled on recorderThread.

            // Low-Latency fetch requested?
            if (capdev->dropframes) {
                // Check what the recorderThread has for us:
                PsychLockMutex(&capdev->mutex);

                // Loop as long as no new frame is ready...
                while (!capdev->frame_ready) {
                    // ...unless this is a polling request, in which case we immediately give up:
                    if (!waitforframe) break;

                    // ... blocking wait. Release mutex and wait for new frame signal from recorderThread:
                    if ((rc = PsychWaitCondition(&(capdev->condition), &(capdev->mutex)))) {
                        // Failed:
                        printf("PTB-ERROR: Waiting on video recorder thread to deliver new video frame failed [%s]. Aborting wait.\n", strerror(rc));
                        break;
                    }
                }

                // Do we finally have a frame?
                frame_ready = capdev->frame_ready;
                if (capdev->frame_ready) {
                    // Yes! Get a hand on the current video image buffer and timestamp:
                    capdev->pulled_frame = capdev->current_frame;
                    capdev->current_frame = NULL;
                    capdev->pulled_pts = capdev->current_pts;
                    capdev->pulled_dropped = capdev->current_dropped;
                    capdev->current_dropped = 0;
                    capdev->frame_ready = 0;
                }

                PsychUnlockMutex(&capdev->mutex);
            }
            else {
                // Pulling from GStreamer requested:
                // TODO
                frame_ready = FALSE;
            }
        }
        
        // Return availability status: 0 = new frame ready for retrieval. -1 = No new frame ready yet.
        return((frame_ready) ? 0 : -1);
    }

    // This point is only reached if checkForImage == FALSE, which only happens
    // if a new frame is available in our buffer:

    // Presentation timestamp requested?
    if (presentation_timestamp) {
        // Return it:
        *presentation_timestamp = capdev->pulled_pts;
    }

    // Synchronous texture fetch: Copy content of capture buffer into a texture:
    // =========================================================================

    // Synchronous frame fetch from masterthread?
    if (!(capdev->recordingflags & 16)) {
        // Yes. Do pre-processing of frame:
        input_image = PsychDCPreprocessFrame(capdev);
        if (NULL == input_image) PsychErrorExitMsg(PsychError_system, "Bayer filtering or color space conversion of video frame failed.");
    }
    else {
        // No. Already pre-processed in recorderThread, just assign:
        input_image = capdev->pulled_frame;
    }
    
    // Only setup if really a texture is requested (non-benchmarking mode):
    if (out_texture) {
        // Activate OpenGL context of target window:
        PsychSetGLContext(win);
        
        #if PSYCH_SYSTEM == PSYCH_OSX
        // Explicitely disable Apple's Client storage extensions. For now they are not really useful to us.
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        #endif

        PsychMakeRect(out_texture->rect, 0, 0, w, h);

        // Set texture orientation as if it were an inverted Offscreen window: Upside-down.
        out_texture->textureOrientation = 3;

        // Setup a pointer to our buffer as texture data pointer: Setting memsize to zero
        // prevents unwanted free() operation in PsychDeleteTexture...
        out_texture->textureMemorySizeBytes = 0;

        // Set texture depth: Could be 8, 16, 24 or 32 bpp for 1, 2, 3 or 4 layer textures:
        out_texture->depth = capdev->reqlayers * 8;

        // This will retrieve an OpenGL compatible pointer to the pixel data and assign it to our texmemptr:
        out_texture->textureMemory = (GLuint*) input_image;

        // Is this a > 8 bpc image format? If not, we ain't nothing more to prepare.
        // If yes, we need to use a high precision floating point texture to represent
        // the > 8 bpc image payload without loss of image information:
        if (capdev->bitdepth > 8) {
            // highbitthreshold: If the net bpc value is greater than this, then use 32bpc floats
            // instead of 16 bpc half-floats, because 16 bpc would not be sufficient to represent
            // more than highbitthreshold bits faithfully:
            const int highbitthreshold = 11;
            
            // 9 - 16 bpc color/luminance resolution:
            out_texture->depth = capdev->reqlayers * ((capdev->bitdepth > highbitthreshold) ? 32 : 16);
            if (capdev->reqlayers == 1) {
                // 1 layer Luminance:
                out_texture->textureinternalformat = (capdev->bitdepth > highbitthreshold) ? GL_LUMINANCE_FLOAT32_APPLE : GL_LUMINANCE_FLOAT16_APPLE;
                out_texture->textureexternalformat = GL_LUMINANCE;
                // Override for missing floating point texture support: Try to use 16 bit fixed point signed normalized textures [-1.0 ; 1.0] resolved at 15 bits:
                if (!(win->gfxcaps & kPsychGfxCapFPTex16)) out_texture->textureinternalformat = GL_LUMINANCE16_SNORM;
            }
            else {
                // 3 layer RGB:
                out_texture->textureinternalformat = (capdev->bitdepth > highbitthreshold) ? GL_RGB_FLOAT32_APPLE : GL_RGB_FLOAT16_APPLE;
                out_texture->textureexternalformat = GL_RGB;
                // Override for missing floating point texture support: Try to use 16 bit fixed point signed normalized textures [-1.0 ; 1.0] resolved at 15 bits:
                if (!(win->gfxcaps & kPsychGfxCapFPTex16)) out_texture->textureinternalformat = GL_RGB16_SNORM;
            }

            // External datatype is 16 bit unsigned integer, each color component encoded in a 16 bit value:
            out_texture->textureexternaltype = GL_UNSIGNED_SHORT;

            // Scale input data, so highest significant bit of payload is in bit 16:
            glPixelTransferi(GL_RED_SCALE,   1 << (16 - capdev->bitdepth));
            glPixelTransferi(GL_GREEN_SCALE, 1 << (16 - capdev->bitdepth));
            glPixelTransferi(GL_BLUE_SCALE,  1 << (16 - capdev->bitdepth));

            // Let PsychCreateTexture() do the rest of the job of creating, setting up and
            // filling an OpenGL texture with content:
            PsychCreateTexture(out_texture);

            // Undo scaling:
            glPixelTransferi(GL_RED_SCALE, 1);
            glPixelTransferi(GL_GREEN_SCALE, 1);
            glPixelTransferi(GL_BLUE_SCALE, 1);
        }
        else {
            // Let PsychCreateTexture() do the rest of the job of creating, setting up and
            // filling an OpenGL texture with content:
            PsychCreateTexture(out_texture);
        }

        // Undo hack from above after texture creation: Now we need the real width of the
        // texture for proper texture coordinate assignments in drawing code et al.
        PsychMakeRect(out_texture->rect, 0, 0, w, h);
        // Ready to use the texture...
    }

    // Sum of pixel intensities requested? 8 bpc?
    if (summed_intensity && (capdev->bitdepth <= 8)) {
        pixptr = (unsigned char*) input_image;
        count = (w*h*((capdev->actuallayers == 3) ? 3 : 1));
        for (i=0; i<count; i++) intensity+=(psych_uint64) pixptr[i];
        *summed_intensity = ((double) intensity) / w / h / ((capdev->actuallayers == 3) ? 3 : 1) / 255;
    }

    // Sum of pixel intensities requested? 16 bpc?
    if (summed_intensity && (capdev->bitdepth > 8)) {
        pixptrs = (psych_uint16*) input_image;
        count = (w*h*((capdev->actuallayers == 3) ? 3 : 1));
        for (i=0; i<count; i++) intensity+=(psych_uint64) pixptrs[i];
        *summed_intensity = ((double) intensity) / w / h / ((capdev->actuallayers == 3) ? 3 : 1) / ((1 << (capdev->bitdepth)) - 1);
    }

    // Raw data requested?
    if (outrawbuffer) {
        // Copy it out:
        outrawbuffer->w = w;
        outrawbuffer->h = h;
        outrawbuffer->depth = ((capdev->actuallayers == 3) ? 3 : 1);
        outrawbuffer->bitdepth = (capdev->bitdepth > 8) ? 16 : 8;
        count = (w * h * outrawbuffer->depth * (outrawbuffer->bitdepth / 8));

        // True bitdepth in the 9 to 15 bpc range?
        if (capdev->bitdepth > 8 && capdev->bitdepth < 16) {
            // Yes. Need to bit-shift, so the most significant bit of the video data,
            // gets placed in the 16th bit of the 16 bit word. This to make sure the
            // "dead bits" for bpc < 16 are the least significant bits and they are
            // all zeros. This makes sure that black is always all-zero and white is
            // at least close to 0xffff - minus the all-zero undefined lsb bits:
            psych_uint16 *frameinwords = (psych_uint16*) input_image;
            psych_uint16 *frameoutwords = (psych_uint16*) outrawbuffer->data;
            
            count /= 2; // Half as many words as bytes.
            for (i = 0; i < count; i++) *(frameoutwords++) = *(frameinwords++) << (16 - capdev->bitdepth);
        }
        else {
            // No, either 8 bpc or 16 bpc - A simple memcpy does the job efficiently:
            memcpy(outrawbuffer->data, (const void*) input_image, count);
        }
    }

    // Synchronous video recording on masterthread active?
    if (capdev->recording_active && (capdev->moviehandle != -1) && !(capdev->recordingflags & 16)) {
        // Yes. Push data to encoder now:
        PsychDCPushFrameToMovie(capdev, (psych_uint16*) input_image, TRUE);
    }

    // Synchronous operation?
    if (!(capdev->recordingflags & 16)) {
        // Yes: Release the capture buffer. Return it to the DMA ringbuffer pool:
        if (dc1394_capture_enqueue((capdev->camera), (capdev->frame)) != DC1394_SUCCESS) {
            PsychErrorExitMsg(PsychError_system, "Re-Enqueuing processed video frame failed.");
        }

        // Reset current drop count for this cycle:
        capdev->current_dropped = 0;        
    }

    // Release cached frame buffer, if any:
    if (capdev->pulled_frame) free(capdev->pulled_frame);
    capdev->pulled_frame = NULL;

    // Update total count of dropped (or pending) frames:
    capdev->nr_droppedframes += capdev->pulled_dropped;
    
    // Find number of dropped frames at time of return of this frame:
    nrdropped = capdev->pulled_dropped;
    
    // Timestamping:
    PsychGetAdjustedPrecisionTimerSeconds(&tend);

    // Increase counter of retrieved textures:
    capdev->nrgfxframes++;

    // Update average time spent in texture conversion:
    capdev->avg_gfxtime+=(tend - tstart);

    // We're successfully done! Return number of dropped (or pending in DMA ringbuffer) frames:
    return(nrdropped);
}

/* Set capture device specific parameters:
 * Currently, the named parameters are a subset of the parameters supported by the
 * IIDC specification, mapped to more convenient names.
 *
 * Input: pname = Name string to specify the parameter.
 *        value = Either DBL_MAX to not set but only query the parameter, or some other
 *                value, that we try to set in the Firewire camera.
 *
 * Returns: Old value of the setting
 */
double PsychDCVideoCaptureSetParameter(int capturehandle, const char* pname, double value)
{
    dc1394featureset_t features;
    dc1394feature_t feature;
    dc1394bool_t present;
    dc1394error_t err;
    unsigned int minval, maxval, intval, oldintval;
    int triggercount, i;

    double oldvalue = DBL_MAX; // Initialize return value to the "unknown/unsupported" default.
    psych_bool assigned = false;

    // Retrieve device record for handle:
    PsychVidcapRecordType* capdev = (capturehandle != -1) ? PsychGetVidcapRecord(capturehandle) : NULL;

    // A -1 capturehandle is currently not supported on this engine. No-Op return:
    if (capdev == NULL) return(oldvalue);

    oldintval = 0xFFFFFFFF;

    // Round value to integer:
    intval = (int) (value + 0.5);

    // Check parameter name pname and call the appropriate subroutine:
    if (strcmp(pname, "TriggerCount")==0 || strcmp(pname, "WaitTriggerCount")==0) {
        // Query of cameras internal trigger counter or waiting for a specific
        // value in the counter requested. Trigger counters are special features,
        // (so called "Smart Features" or "Advanced Features" in the IIDC spec)
        // which are only available on selected cameras.
        // We currently only know how to do this on Basler cameras.
        if (strstr(capdev->camera->vendor, "Basler")==NULL) {
            // Non Basler cam :( We have to give up for now...
            return(-1);
        }

        // It is a Basler cam. Go ahead:
        // TODO FIXME: IMPLEMENT IT!
        return(-2);
    }

    // Set a new target movie name for video recordings:
    if (strstr(pname, "SetNewMoviename=")) {
        // Find start of movie namestring and assign to pname:
        pname = strstr(pname, "=");
        pname++;
        
        // Child protection:
        if (!capdev->recording_active) {
            if (PsychPrefStateGet_Verbosity() > 1) {
                printf("PTB-WARNING: Tried to change name of target movie file on device %i, but recording not enabled on that device! Ignored.\n", capturehandle);
            }
            return(-2);
        }
        
        // Can't reassign new codec without reopening the device:
        if (strstr(pname, ":CodecType")) {
            *(strstr(pname, ":CodecType")) = 0;
            if (PsychPrefStateGet_Verbosity() > 1) {
                printf("PTB-WARNING: Tried to change recording codec on device %i, but this isn't possible without reopening the device. Ignored.\n", capturehandle);
            }
        }

        // Can't reassign new codec without reopening the device:
        if (strstr(pname, ":CodecSettings=")) {
            *(strstr(pname, ":CodecSettings=")) = 0;
            if (PsychPrefStateGet_Verbosity() > 1) {
                printf("PTB-WARNING: Tried to change recording codec settings on device %i, but this isn't possible without reopening the device. Ignored.\n", capturehandle);
            }
        }

        // Release old movie name:
        if (capdev->targetmoviefilename) free(capdev->targetmoviefilename);
        capdev->targetmoviefilename = NULL;
        
        // Assign new movie name:
        capdev->targetmoviefilename = strdup(pname);
        
        if (PsychPrefStateGet_Verbosity() > 2) {
            printf("PTB-INFO: Changed name of movie file for recording on device %i to '%s'.\n", capturehandle, pname);
        }
        
        return(0);
    }

    if (strcmp(pname, "PrintParameters")==0) {
        // Special command: List and print all features...
        printf("PTB-INFO: The camera provides the following information and featureset:\n");
        if (dc1394_camera_print_info(capdev->camera, stdout) !=DC1394_SUCCESS) {
            printf("PTB-WARNING: Unable to query general information about camera.\n");
        }

        return(0);
    }

    // Return current framerate:
    if (strcmp(pname, "GetFramerate")==0) {
        PsychCopyOutDoubleArg(1, FALSE, capdev->fps);
        return(0);
    }

    // Return current ROI of camera, as requested (and potentially modified during
    // PsychOpenCaptureDevice(). This is a read-only parameter, as the ROI can
    // only be set during Screen('OpenVideoCapture').
    if (strcmp(pname, "GetROI")==0) {
        PsychCopyOutRectArg(1, FALSE, capdev->roirect);
        return(0);
    }

    // Return vendor name string:
    if (strcmp(pname, "GetVendorname")==0) {
        PsychCopyOutCharArg(1, FALSE, capdev->camera->vendor);
        return(0);
    }

    // Return model name string:
    if (strcmp(pname, "GetModelname")==0) {
        PsychCopyOutCharArg(1, FALSE, capdev->camera->model);
        return(0);
    }
    
    // Return current firewire bus bandwidth usage of camera::
    if (strcmp(pname, "GetBandwidthUsage")==0) {
        dc1394_video_get_bandwidth_usage(capdev->camera, (uint32_t *) &intval);
        PsychCopyOutDoubleArg(1, FALSE, ((double) intval) / 4915);
        return(0);
    }

    // Get/Set option to prefer Format7 modes, even if a non-Format7 mode would do:
    if (strcmp(pname, "PreferFormat7Modes")==0) {
        oldvalue = (double) (capdev->specialFlags & 1) ? 1 : 0;
        if (value != DBL_MAX) {
            if (value > 0) {
                capdev->specialFlags |= 1;
            }
            else {
                capdev->specialFlags &= ~1;
            }
        }

        return(oldvalue);
    }
    
    // Get/Set special treatment mode for raw sensor data:
    if (strcmp(pname, "DataConversionMode")==0) {
        oldvalue = capdev->dataconversionmode;
        if (value != DBL_MAX) {
            capdev->dataconversionmode = intval;
        }

        return(oldvalue);
    }

    // Get/Set debayering method for raw sensor data to RGB conversion:
    if (strcmp(pname, "DebayerMethod")==0) {
        oldvalue = capdev->debayer_method - DC1394_BAYER_METHOD_MIN;
        if (value != DBL_MAX) {
            capdev->debayer_method = intval + DC1394_BAYER_METHOD_MIN;
        }
        
        return(oldvalue);
    }

    // Get/Set debayering method for raw sensor data to RGB conversion:
    if (strcmp(pname, "OverrideBayerPattern")==0) {
        oldvalue = capdev->color_filter_override - DC1394_COLOR_FILTER_MIN;
        if (value != DBL_MAX) {
            capdev->color_filter_override = intval + DC1394_COLOR_FILTER_MIN;
        }

        return(oldvalue);
    }
    
    // Get/Set synchronization mode for multi-camera operation:
    if (strcmp(pname, "SyncMode")==0) {
        oldvalue = capdev->syncmode;
        if (value != DBL_MAX) {
            // Sanity check syncmode spec:

            // Free-running? That's always fine.
            if (intval != 0) {
                // Not free-running, but somehow synced:
                if ((intval & kPsychIsSyncMaster) && (intval & kPsychIsSyncSlave)) {
                    // Master and slave at the same time? That's not possible.
                    PsychErrorExitMsg(PsychError_user, "Invalid syncmode provided: Camera can't be master and slave at the same time!");
                }

                if (!(intval & kPsychIsSyncMaster) && !(intval & kPsychIsSyncSlave)) {
                    // Neither master or slave? That's not possible.
                    PsychErrorExitMsg(PsychError_user, "Invalid syncmode provided: Camera must be either master or slave. Can't be none of both!");
                }

                // Must be either soft-, bus-, or hw- synced, but not none or multiple of all if it is a sync slave.
                // Must be at least one of soft-, bus- or hw- synced if it is a sync master:
                oldintval = intval & (kPsychIsSoftSynced | kPsychIsBusSynced | kPsychIsHwSynced);
                if (((intval & kPsychIsSyncMaster) && !oldintval) || ((intval & kPsychIsSyncSlave) && (oldintval != kPsychIsSoftSynced) && (oldintval != kPsychIsBusSynced) && (oldintval != kPsychIsHwSynced))) {
                    PsychErrorExitMsg(PsychError_user, "Invalid syncmode provided: Missing sync strategy, either soft-, or bus-, or hw-synced!");
                }

                // If this is a cam that should take on the role of a hardware synced slave, check if
                // this cam is actually capable of receiving hardware trigger signals:
                if ((intval & kPsychIsHwSynced) && (intval & kPsychIsSyncSlave)) {
                    err = dc1394_feature_is_present(capdev->camera, DC1394_FEATURE_TRIGGER, &present);
                    if (err || !present) {
                        // Camera is not hw sync capable: Do not change the SyncMode but no-op. A successive
                        // query of 'SyncMode' would report the failure to change to hw-synced mode, so the
                        // calling usercode can search for alternatives:
                        if (PsychPrefStateGet_Verbosity() > 3) printf("PTB-INFO: No hardware trigger support on cam %i. Ignored hw-sync setting for slave cam.\n", capturehandle);
                        return(oldvalue);
                    }
                }
            }
            
            // Assign new syncmode:
            capdev->syncmode = (int) intval;
        }

        return(oldvalue);
    }

    // Set trigger mode:
    // Note: Videolab used DC1394_TRIGGER_MODE_0 aka DC1394_TRIGGER_MODE_MIN aka 'TriggerMode' zero,
    // so exposure duration is controlled by programmable shutter time, whereas start of exposure
    // is controlled by trigger signal.
    if (strcmp(pname, "TriggerMode")==0) {
        dc1394trigger_mode_t mode;

        // Double-Check the camera supports hw triggers:
        err = dc1394_feature_is_present(capdev->camera, DC1394_FEATURE_TRIGGER, &present);
        if (!err && present) {
            err = dc1394_external_trigger_get_mode(capdev->camera, &mode);
        }
        else err = DC1394_FUNCTION_NOT_SUPPORTED;
        
        if (err) {
            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Requested capture device setting %s not available on cam %i. Ignored.\n", pname, capturehandle);
            return(oldvalue);
        }
        else {
            oldvalue = (unsigned int) mode - (unsigned int) DC1394_TRIGGER_MODE_MIN;

            if (value != DBL_MAX) {
                mode = DC1394_TRIGGER_MODE_MIN + (dc1394trigger_mode_t) intval;
                if (mode > DC1394_TRIGGER_MODE_MAX || mode < DC1394_TRIGGER_MODE_MIN) PsychErrorExitMsg(PsychError_user, "Invalid TriggerMode provided: Outside valid range!");
                err = dc1394_external_trigger_set_mode(capdev->camera, mode);
                if (err) PsychErrorExitMsg(PsychError_system, "Failed to set current trigger mode!");
            }

            return(oldvalue);
        }
    }

    // Set trigger source:
    // Note: Videolab used DC1394_TRIGGER_SOURCE_0 aka DC1394_TRIGGER_SOURCE_MIN aka TriggerSource zero.
    // This source corresponds to port 0 == Pin 5 of the RJ-45 connector on a Basler camera.
    // Source 1 = Port 1 = Pin 9, Source 2 = Port 2 = Pin 8, Source 3 = Port 3 = Pin 10.
    if (strcmp(pname, "TriggerSource")==0) {
        dc1394trigger_source_t source;

        // Double-Check the camera supports hw triggers:
        err = dc1394_feature_is_present(capdev->camera, DC1394_FEATURE_TRIGGER, &present);
        if (!err && present) {
            err = dc1394_external_trigger_get_source(capdev->camera, &source);
        }
        else err = DC1394_FUNCTION_NOT_SUPPORTED;
        
        if (err) {
            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Requested capture device setting %s not available on cam %i. Ignored.\n", pname, capturehandle);
            return(oldvalue);
        }
        else {        
            oldvalue = (unsigned int) source - (unsigned int) DC1394_TRIGGER_SOURCE_MIN;

            if (value != DBL_MAX) {
                source = DC1394_TRIGGER_SOURCE_MIN + (dc1394trigger_source_t) intval;
                if (source > DC1394_TRIGGER_SOURCE_MAX || source < DC1394_TRIGGER_SOURCE_MIN) PsychErrorExitMsg(PsychError_user, "Invalid TriggerSource provided: Outside valid range!");
                err = dc1394_external_trigger_set_source(capdev->camera, source);
                if (err) PsychErrorExitMsg(PsychError_system, "Failed to set current trigger source!");
            }

            return(oldvalue);
        }
    }

    // Retrieve list of supported trigger sources:
    if (strcmp(pname, "GetTriggerSources")==0) {
        dc1394trigger_sources_t sources;
        double *outsources;

        // Double-Check the camera supports hw triggers:
        err = dc1394_feature_is_present(capdev->camera, DC1394_FEATURE_TRIGGER, &present);
        if (!err && present) {
            err = dc1394_external_trigger_get_supported_sources(capdev->camera, &sources);
        }
        else err = DC1394_FUNCTION_NOT_SUPPORTED;
        
        if (err) {
            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Requested capture device setting %s not available on cam %i. Ignored.\n", pname, capturehandle);
            return(oldvalue);
        }
        
        PsychAllocOutDoubleMatArg(1, FALSE, 1, sources.num, 0, &outsources);
        for (i = 0; i < (int) sources.num; i++) *(outsources++) = (double) (sources.sources[i] - DC1394_TRIGGER_SOURCE_MIN);
        
        return(0);
    }
    
    // Set trigger polarity:
    // Note: Videolab used TriggerPolarity zero == DC1394_TRIGGER_ACTIVE_LOW == Trigger on falling edge.
    if (strcmp(pname, "TriggerPolarity")==0) {        
        dc1394trigger_polarity_t polarity;

        // Double-Check the camera supports hw triggers:
        err = dc1394_feature_is_present(capdev->camera, DC1394_FEATURE_TRIGGER, &present);
        if (!err && present) {
            err = dc1394_external_trigger_has_polarity(capdev->camera, &present);
        }
        else err = DC1394_FUNCTION_NOT_SUPPORTED;
        
        if (err || !present) {
            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to query/set TriggerPolarity on camera %i. Unsupported feature. Ignored.\n", capturehandle);
            return(oldvalue);
        }
        else {
            err = dc1394_external_trigger_get_polarity(capdev->camera, &polarity);
            if (err) PsychErrorExitMsg(PsychError_system, "Failed to query trigger polarity!");
            oldvalue = (polarity == DC1394_TRIGGER_ACTIVE_HIGH) ? 1 : 0;

            if (value != DBL_MAX) {
                err = dc1394_external_trigger_set_polarity(capdev->camera, (intval > 0) ? DC1394_TRIGGER_ACTIVE_HIGH : DC1394_TRIGGER_ACTIVE_LOW);
                if (err) PsychErrorExitMsg(PsychError_system, "Failed to set trigger polarity!");
            }

            return(oldvalue);
        }
    }
    
    if (strstr(pname, "Brightness")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_BRIGHTNESS;
    }

    if (strstr(pname, "Gain")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_GAIN;
    }

    if (strstr(pname, "Exposure")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_EXPOSURE;
    }

    if (strstr(pname, "Shutter")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_SHUTTER;
    }

    if (strstr(pname, "Sharpness")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_SHARPNESS;
    }

    if (strstr(pname, "Saturation")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_SATURATION;
    }

    if (strstr(pname, "Gamma")!=0) {
        assigned = true;
        feature = DC1394_FEATURE_GAMMA;
    }

    // Check if feature is present on this camera:
    if (dc1394_feature_is_present(capdev->camera, feature, &present)!=DC1394_SUCCESS) {
        if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to query presence of feature %s on camera %i! Ignored.\n", pname, capturehandle);
    }
    else if (present) {
        // Feature is available:

        // Retrieve current value:
        if (dc1394_feature_get_value(capdev->camera, feature, &oldintval)!=DC1394_SUCCESS) {
            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to query value of feature %s on camera %i! Ignored.\n", pname, capturehandle);
        }
        else {
            // Do we want to set the value?
            if (value != DBL_MAX) {
                // Query allowed bounds for its value:
                if (dc1394_feature_get_boundaries(capdev->camera, feature, &minval, &maxval)!=DC1394_SUCCESS) {
                    if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to query valid value range for feature %s on camera %i! Ignored.\n", pname, capturehandle);
                }
                else {
                    // Sanity check against range:
                    if (intval < minval || intval > maxval) {
                        if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Requested setting %i for parameter %s not in allowed range (%i - %i) for camera %i. Ignored.\n",
                            intval, pname, minval, maxval, capturehandle);
                    }
                    else {
                        // Ok intval is valid for this feature: Can we manually set this feature?
                        // Switch feature to manual control mode:
                        if (dc1394_feature_set_mode(capdev->camera, feature, DC1394_FEATURE_MODE_MANUAL)!=DC1394_SUCCESS) {
                            if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to set feature %s on camera %i to manual control! Ignored.\n", pname, capturehandle);
                        }
                        else {
                            // Ok, try to set the features new value:
                            if (dc1394_feature_set_value(capdev->camera, feature, intval)!=DC1394_SUCCESS) {
                                if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to set value of feature %s on camera %i to %i! Ignored.\n", pname, capturehandle,
                                    intval);
                            }
                        }
                    }
                }
            }
            else {
                // Don't want to set new value. Do we want to reset feature into auto-mode?
                // Prefixing a parameter name with "Auto"
                // does not switch the parameter into manual
                // control mode + set its value, as normal,
                // but it switches the parameter into automatic
                // mode, if automatic mode is supported by the
                // device.
                if (strstr(pname, "Auto")!=0) {
                    // Switch to automatic control requested - Try it:
                    if (dc1394_feature_set_mode(capdev->camera, feature, DC1394_FEATURE_MODE_AUTO)!=DC1394_SUCCESS) {
                        if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Failed to set feature %s on camera %i to automatic control! Ignored.\n", pname, capturehandle);
                    }
                }
            }
        }
    }
    else {
        if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Requested capture device setting %s not available on cam %i. Ignored.\n", pname, capturehandle);
    }

    // Output a warning on unknown parameters:
    if (!assigned) {
        if(PsychPrefStateGet_Verbosity()>1) printf("PTB-WARNING: Screen('SetVideoCaptureParameter', ...) called with unknown parameter %s. Ignored...\n",
            pname);
    }

    if (assigned && oldintval!=0xFFFFFFFF) oldvalue = (double) oldintval;

    // Return the old value. Could be DBL_MAX if parameter was unknown or not accepted for some reason.
    return(oldvalue);
}

/* PsychDCEnumerateVideoSources(int outPos);
 *
 * Enumerates all connected and supported video sources into an internal
 * array "devices".
 *
 * If deviceIndex >= 0 : Returns pointer to PsychVideosourceRecordType struct
 *                       with info about the detected device with index 'deviceIndex'
 *                       or NULL if no such device exists. The pointer is valid until
 *                       the Screen module returns control to the runtime - then it
 *                       will get deallocated and must not be accessed anymore!
 *
 * If deviceIndex < 0 : Returns NULL to caller, returns a struct array to runtime
 *                      environment return argument position 'outPos' with all info
 *                      about the detected sources.
 */
void PsychDCEnumerateVideoSources(int outPos)
{
    PsychGenericScriptType  *devs;
    const char *FieldNames[]={"DeviceIndex", "ClassIndex", "InputIndex", "ClassName", "InputHandle", "Device", "DevicePath", "DeviceName", "GUID", "DevicePlugin", "DeviceSelectorProperty" };

    dc1394camera_list_t   *cameras = NULL;
    dc1394camera_t        *camera = NULL;
    unsigned int          numCameras, i;
    dc1394error_t         err;
    char                  guid[100];
    char                  unit[10];
    char                  deviceName[1024];
    uint32_t              node, generation;

    // Perform first-time init, if needed:
    PsychDCLibInit();

    // Query a list of all available (connected) Firewire cameras:
    err = dc1394_camera_enumerate(libdc, &cameras);
    if (err != DC1394_SUCCESS) {
        // Failed to detect any cameras:
        printf("Unable to enumerate firewire cameras: %s\n", dc1394_error_get_string(err));
        PsychErrorExitMsg(PsychError_user, "Unable to detect Firewire cameras: Read 'help VideoCaptureDC1394' for troubleshooting tips.\n");
    }

    // Get number of detected cameras:
    numCameras = cameras->num;

    // Create output struct array with n output slots:
    PsychAllocOutStructArray(outPos, TRUE, numCameras, 11, FieldNames, &devs);

    // Iterate all available devices:
    for(i = 0; i < numCameras; i++) {
        // Open connection to camera so we can query some more info about it:
        camera = dc1394_camera_new_unit(libdc, cameras->ids[i].guid, cameras->ids[i].unit);
        if (camera) {
            // Open success: Query vendor and model name:
            sprintf(deviceName, "'%s':'%s'", camera->vendor, camera->model);
            PsychSetStructArrayStringElement("DeviceName", i, deviceName, devs);

            // Get node to which the camera is attached, and its current generation count:
            dc1394_camera_get_node(camera, &node, &generation);
            sprintf(deviceName, "%x:%x", node, generation);
            PsychSetStructArrayStringElement("InputHandle", i, deviceName, devs);

            // Done with query, release camera:
            dc1394_camera_free(camera);
        }
        else {
            // Open failed: Assign empty DeviceName:
            PsychSetStructArrayStringElement("DeviceName", i, "'UNKNOWN':'UNKNOWN'", devs);
            PsychSetStructArrayStringElement("InputHandle", i, "", devs);
        }

        PsychSetStructArrayDoubleElement("DeviceIndex", i, i, devs);
        PsychSetStructArrayDoubleElement("ClassIndex", i, 7, devs);
        PsychSetStructArrayDoubleElement("InputIndex", i, i, devs);
        PsychSetStructArrayStringElement("ClassName", i, "1394-IIDC", devs);
        sprintf(unit, "%i", cameras->ids[i].unit);
        PsychSetStructArrayStringElement("Device", i, unit, devs);
        PsychSetStructArrayStringElement("DevicePath", i, "", devs);
        sprintf(guid, "%" PRIx64, cameras->ids[i].guid);
        PsychSetStructArrayStringElement("GUID", i, guid, devs);
        PsychSetStructArrayStringElement("DevicePlugin", i, "libDC1394", devs);
        PsychSetStructArrayStringElement("DeviceSelectorProperty", i, "GUID + Device(==IIDC-Unit id)", devs);
    }

    // Free camera list:
    dc1394_camera_free_list(cameras);
    cameras=NULL;

    return;
}

#endif
