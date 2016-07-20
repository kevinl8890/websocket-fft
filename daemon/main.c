#include "main.h"

char* latest_fft_json;

int max_poll_elements;
int debug_level = 3;

volatile int force_exit = 0;
struct lws_context *context;

/** RTLSDR Vars **/

static uint8_t *buffer;
static uint32_t dev_index = 0;
static uint32_t frequency = 000000; // Hz
static uint32_t samp_rate = 2048000;
static uint32_t buff_len = 2048;
static int      ppm_error = 0;

static float    lut[256];       /* look-up table to convert U8 to +/- 1.0f */
static float   *log_pwr_fft;    /* dbFS relative to 1.0 */

#define FFT_SIZE    1024
fftw_complex* fft_in;
fftw_complex*   fft_out;
fftw_plan   fft_plan;

static rtlsdr_dev_t *dev = NULL;

static void close_rtlsdr(void)
{
    rtlsdr_close(dev);
    free(buffer);

    fftw_free(fft_in);
    fftw_free(fft_out);
    fftw_destroy_plan(fft_plan);
    free(log_pwr_fft);
}

static void setup_rtlsdr()
{
    int             device_count;
    int             r;

    buffer = malloc(buff_len * sizeof(uint8_t));

    device_count = rtlsdr_get_device_count();
    if (!device_count)
    {
        fprintf(stderr, "No supported devices found.\n");
        exit(1);
    }

    r = rtlsdr_open(&dev, dev_index);
    if (r < 0)
    {
        fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
        exit(1);
    }

    if (ppm_error != 0)
    {
        r = rtlsdr_set_freq_correction(dev, ppm_error);
        if (r < 0)
            fprintf(stderr, "WARNING: Failed to set PPM error.\n");
    }

    r = rtlsdr_set_sample_rate(dev, samp_rate);
    if (r < 0)
        fprintf(stderr, "WARNING: Failed to set sample rate.\n");

    r = rtlsdr_set_center_freq(dev, frequency);
    if (r < 0)
        fprintf(stderr, "WARNING: Failed to set center freq.\n");

    r = rtlsdr_set_tuner_gain_mode(dev, 0);
    if (r < 0)
        fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");

    r = rtlsdr_reset_buffer(dev);
    if (r < 0)
        fprintf(stderr, "WARNING: Failed to reset buffers.\n");

}

static int read_rtlsdr()
{
    int        error = 0;
    int             n_read;
    int             r;

    r = rtlsdr_read_sync(dev, buffer, buff_len, &n_read);
    if (r < 0)
    {
        fprintf(stderr, "WARNING: sync read failed.\n");
        error = 1;
    }

    if ((uint32_t) n_read < buff_len)
    {
        fprintf(stderr, "Short read (%d / %d), samples lost, exiting!\n",
                n_read, buff_len);
        error = 1;
    }

    return error;
}

static void run_fft()
{
    int             i;
    fftw_complex    pt;
    float           pwr, lpwr;
    float           gain;

    for (i = 0; i < FFT_SIZE; i++)
    {
        fft_in[i][0] = lut[buffer[2 * i]];
        fft_in[i][1] = lut[buffer[2 * i + 1]];
    }
    
    fftw_execute(fft_plan);
    
    for (i = 0; i < FFT_SIZE; i++)
    {
        /* shift, normalize and convert to dBFS */
        if (i < FFT_SIZE / 2)
        {
            pt[0] = fft_out[FFT_SIZE / 2 + i][0] / FFT_SIZE;
            pt[1] = fft_out[FFT_SIZE / 2 + i][1] / FFT_SIZE;
        }
        else
        {
            pt[0] = fft_out[i - FFT_SIZE / 2][0] / FFT_SIZE;
            pt[1] = fft_out[i - FFT_SIZE / 2][1] / FFT_SIZE;
        }
        pwr = pt[0] * pt[0] + pt[1] * pt[1];
        lpwr = 10.f * log10(pwr + 1.0e-20f);

        gain = 0.3 * (100.f + lpwr) / 100.f;
        log_pwr_fft[i] = log_pwr_fft[i] * (1.f - gain) + lpwr * gain;
    }

}

void setup_fft(void)
{
    int             i;

    /* LUT */
    for (i = 0; i < 256; i++)
        lut[i] = (float)i / 127.5f - 1.f;


    /* Set up FFTW */
    fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_plan = fftw_plan_dft_1d(FFT_SIZE, fft_in, fft_out, FFTW_FORWARD, FFTW_PATIENT);
    log_pwr_fft = malloc(FFT_SIZE * sizeof(float));
}

/** End of RTLSDR Vars **/

int callback_fft(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	unsigned char buf[LWS_PRE + 16384];//LWS_PRE+512
	/* Out Buffer */
	unsigned char *p = &buf[LWS_PRE];//LWS_PRE
	int n, m;

	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
	    /* Connection Established */
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		n = sprintf((char *)p, "%s", latest_fft_json);
		m = lws_write(wsi, p, n, LWS_WRITE_TEXT);
		if (m < n) {
			lwsl_err("ERROR %d writing to socket\n", n);
			return -1;
		}
		break;

	case LWS_CALLBACK_RECEIVE:
		if (len < 6)
			break;
		if (strcmp((const char *)in, "closeme\n") == 0) {
			//lwsl_notice("dumb_inc: closing as requested\n");
			lws_close_reason(wsi, LWS_CLOSE_STATUS_GOINGAWAY,
					 (unsigned char *)"seeya", 5);
			return -1;
		}
		break;
	
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		lwsl_notice("LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: len %d\n",
			    len);
		for (n = 0; n < (int)len; n++)
			lwsl_notice(" %d: 0x%02X\n", n,
				    ((unsigned char *)in)[n]);
		break;

	default:
		break;
	}

	return 0;
}

enum demo_protocols {
	PROTOCOL_FFT,
	NOP
};

/* list of supported protocols and callbacks */
static struct lws_protocols protocols[] = {
	{
		"fft",
		callback_fft,
		0,
		16384, /* rx buf size must be >= permessage-deflate rx size */
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};

void sighandler(int sig)
{
	force_exit = 1;
	lws_cancel_service(context);
}

static const struct lws_extension exts[] = {
	{
		"permessage-deflate",
		lws_extension_callback_pm_deflate,
		"permessage-deflate"
	},
	{
		"deflate-frame",
		lws_extension_callback_pm_deflate,
		"deflate_frame"
	},
	{ NULL, NULL, NULL /* terminator */ }
};



static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",	required_argument,	NULL, 'd' },
	{ "port",	required_argument,	NULL, 'p' },
	{ "interface",  required_argument,	NULL, 'i' },
	{ "closetest",  no_argument,		NULL, 'c' },
	{ "libev",  no_argument,		NULL, 'e' },
	{ NULL, 0, 0, 0 }
};

int main(int argc, char **argv)
{
	struct lws_context_creation_info info;
	char interface_name[128] = "";
	unsigned int ms, oldms = 0;
	const char *iface = NULL;
	int uid = -1, gid = -1;
	int opts = 0;
	int n = 0;
	int syslog_options = LOG_PID | LOG_PERROR;

	memset(&info, 0, sizeof info);
	
	/* Default Websocket Port */
	info.port = 7681;

	while (n >= 0) {
		n = getopt_long(argc, argv, "ei:hp:d:R:vu:g:", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
		case 'e':
			opts |= LWS_SERVER_OPTION_LIBEV;
			break;
		case 'u':
			uid = atoi(optarg);
			break;
		case 'g':
			gid = atoi(optarg);
			break;
		case 'd':
			debug_level = atoi(optarg);
			break;
		case 'p':
			info.port = atoi(optarg);
			break;
		case 'i':
			strncpy(interface_name, optarg, sizeof interface_name);
			interface_name[(sizeof interface_name) - 1] = '\0';
			iface = interface_name;
			break;
		case 'h':
			fprintf(stderr, "Usage: test-server "
					"[--port=<p>]"
					"[-d <log bitfield>] \n");
			exit(1);
		}
	}

	signal(SIGINT, sighandler);

	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", syslog_options, LOG_DAEMON);

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	info.iface = iface;
	info.protocols = protocols;
	info.gid = gid;
	info.uid = uid;
	info.max_http_header_pool = 16;
	info.options = opts | LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.extensions = exts;
	info.timeout_secs = 5;

    fprintf(stdout, "Initialising Websocket on port %d..\n",info.port);
	context = lws_create_context(&info);
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}
	
	fprintf(stdout, "Initialising RTLSDR..\n");
	setup_rtlsdr();
	
	fprintf(stdout, "Initialising FFT (%d point)..\n", FFT_SIZE);
	setup_fft();

    fprintf(stdout, "Server running.\n");
	n = 0;
	while (n >= 0 && !force_exit) {
		struct timeval tv;

		gettimeofday(&tv, NULL);

		ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
		if ((ms - oldms) > 100) {
		    if(read_rtlsdr())
            {
                fprintf(stdout, "Error reading RTLSDR\n");
            }
            else
            {
                //fprintf(stdout, "Data!\n");
                run_fft();
                int j;
		        JsonNode *jsonData = json_mkobject();
		        JsonNode *fftArray = json_mkarray();
		        JsonNode *fftRow;
		        for(j=0;j<FFT_SIZE;j++)
                {
                    fftRow = json_mknumber(((roundf(log_pwr_fft[j]*10))));
                    json_append_element(fftArray, fftRow);
                }
                json_append_member(jsonData, "fft", fftArray);
                free(latest_fft_json);
		        latest_fft_json = json_stringify(jsonData, NULL);
		        json_delete(jsonData);
            }
		    
		    /* End of FFT Stuffs */
		    
		    

		    /*
		     * This provokes the LWS_CALLBACK_SERVER_WRITEABLE for every
		     * live websocket connection as soon as it can take more packets (usually immediately)
		     */
			lws_callback_on_writable_all_protocol(context, &protocols[PROTOCOL_FFT]);
			
			oldms = ms;
		}
		
		/*
		 * If libwebsockets sockets are all we care about,
		 * you can use this api which takes care of the poll()
		 * and looping through finding who needed service.
		 *
		 * If no socket needs service, it'll return anyway after
		 * the number of ms in the second argument.
		 */

		n = lws_service(context, 50);
	}

	lws_context_destroy(context);
	close_rtlsdr();
	lwsl_notice("libwebsockets-test-server exited cleanly\n");
	closelog();

	return 0;
}
