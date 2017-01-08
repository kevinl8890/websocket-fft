#include "main.h"

#define FLOAT32_EL_SIZE_BYTE (4)

/** LWS Vars **/
int max_poll_elements;
int debug_level = 3;
volatile int force_exit = 0;
struct lws_context *context;

pthread_t fftThread;
/* FFT JSON Buffer */
char* latest_fft_json;
pthread_mutex_t fft_json_mutex;
char* websocket_out_json;

/** AirSpy Vars **/
struct airspy_device* device = NULL;
/* Sample type -> 32bit Complex Float */
enum airspy_sample_type sample_type_val = AIRSPY_SAMPLE_FLOAT32_IQ;
/* Sample rate -> 6MSps */
uint32_t sample_rate_val = 10000000;
/* DC Bias Tee -> 0 (disabled) */
uint32_t biast_val = 0;
/* Linear Gain */
#define LINEAR
uint32_t linearity_gain_val = 20; // MAX=21
/* Sensitive Gain */
//#define SENSITIVE
uint32_t sensitivity_gain_val = 10; // MAX=21
/* Frequency */
uint32_t freq_hz = 1250000000;

/** FFTW Vars **/
#define FFT_SIZE    2048
void            *buffer;
pthread_mutex_t buffer_mutex;
static float   *log_pwr_fft;    /* dbFS relative to 1.0 */
fftw_complex* fft_in;
fftw_complex*   fft_out;
fftw_plan   fft_plan;

int airspy_rx(airspy_transfer_t* transfer);

void setup_fft(void)
{
    /* Set up FFTW */
    fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_plan = fftw_plan_dft_1d(FFT_SIZE, fft_in, fft_out, FFTW_FORWARD, FFTW_PATIENT);
    log_pwr_fft = malloc(FFT_SIZE * sizeof(float));
}

static void close_airspy(void)
{
    int result;
    
    free(buffer);
    
    /* De-init AirSpy device */
    if(device != NULL)
    {
	    result = airspy_stop_rx(device);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_stop_rx() failed: %s (%d)\n", airspy_error_name(result), result);
	    }

	    result = airspy_close(device);
	    if( result != AIRSPY_SUCCESS ) 
	    {
		    printf("airspy_close() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
	
	    airspy_exit();
    }
}

static void close_fftw(void)
{
    /* De-init fftw */
    fftw_free(fft_in);
    fftw_free(fft_out);
    fftw_destroy_plan(fft_plan);
    free(log_pwr_fft);
}

static uint8_t setup_airspy()
{
    int result;

    buffer = malloc(FFT_SIZE * FLOAT32_EL_SIZE_BYTE * 2);

    result = airspy_init();
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_init() failed: %s (%d)\n", airspy_error_name(result), result);
	    return 0;
    }
    result = airspy_open(&device);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_open() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_sample_type(device, sample_type_val);
    if (result != AIRSPY_SUCCESS) {
	    printf("airspy_set_sample_type() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_samplerate(device, sample_rate_val);
    if (result != AIRSPY_SUCCESS) {
	    printf("airspy_set_samplerate() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_rf_bias(device, biast_val);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_set_rf_bias() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    #ifdef LINEAR
	    result =  airspy_set_linearity_gain(device, linearity_gain_val);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_set_linearity_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
    #elif SENSITIVE
	    result =  airspy_set_sensitivity_gain(device, sensitivity_gain_val);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_set_sensitivity_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
    #endif

    result = airspy_start_rx(device, airspy_rx, NULL);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_start_rx() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_freq(device, freq_hz);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_set_freq() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }
    
    return 1;
}

/* Airspy RX Callback, this is called by a new thread within libairspy */
int airspy_rx(airspy_transfer_t* transfer)
{    
    if(transfer->samples != NULL && transfer->sample_count>=(FFT_SIZE*2))
    {
        pthread_mutex_lock(&buffer_mutex);
        memcpy(
            buffer,
            &((double *)transfer->samples)[transfer->sample_count-(FFT_SIZE*2)],
            (FFT_SIZE*2 * FLOAT32_EL_SIZE_BYTE)
        );
        pthread_mutex_unlock(&buffer_mutex);
    }
	return 0;
}

static void run_fft()
{
    int             i;
    fftw_complex    pt;
    float           pwr, lpwr;
    float           gain;
    
    pthread_mutex_lock(&buffer_mutex);
    for (i = 0; i < FFT_SIZE; i++)
    {
        fft_in[i][0] = ((float*)buffer)[2*i];
        fft_in[i][1] = ((float*)buffer)[(2*i)+1];
    }
    pthread_mutex_unlock(&buffer_mutex);
    
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

        gain = (100.f + lpwr) / 50.f;
        log_pwr_fft[i] = log_pwr_fft[i] * (1.f - gain) + lpwr * gain;
    }

}

void *thread_fft(void *dummy)
{
    (void*) dummy;
    int j;
    struct timespec ts;
    ts.tv_sec = (100) / 1000;
    ts.tv_nsec = ((100) % 1000) * 1000000;
    while(1)
    {
        run_fft();
        JsonNode *jsonData = json_mkobject();
        JsonNode *fftArray = json_mkarray();
        JsonNode *fftRow;
        for(j=0;j<FFT_SIZE;j++)
        {
            fftRow = json_mknumber(((roundf(log_pwr_fft[j]*10))));
            json_append_element(fftArray, fftRow);
        }
        json_append_member(jsonData, "fft", fftArray);
        
        pthread_mutex_lock(&fft_json_mutex);
        free(latest_fft_json);
        latest_fft_json = json_stringify(jsonData, NULL);
        pthread_mutex_unlock(&fft_json_mutex);
        
        json_delete(jsonData);
        
        nanosleep(&ts, NULL);
    }
}

int callback_fft(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	int n;

	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
	    /* Connection Established */
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		n = lws_write(wsi, websocket_out_json, strlen(websocket_out_json), LWS_WRITE_TEXT);
		if (!n) {
			lwsl_err("ERROR %d writing to socket\n", n);
			return -1;
		}
		break;

	case LWS_CALLBACK_RECEIVE:
		if (len < 6)
			break;
		if (strcmp((const char *)in, "closeme\n") == 0) {
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
	unsigned int ms, oldms = 0;
	int n = 0;
	int result;

	signal(SIGINT, sighandler);

	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", LOG_PID | LOG_PERROR, LOG_DAEMON);

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	memset(&info, 0, sizeof info);
    info.port = 7681;
	info.iface = NULL;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.max_http_header_pool = 16;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.extensions = exts;
	info.timeout_secs = 5;

    fprintf(stdout, "Initialising Websocket on port %d.. ",info.port);
    fflush(stdout);
	context = lws_create_context(&info);
	if (context == NULL)
	{
		lwsl_err("LWS init failed\n");
		return -1;
	}
	websocket_out_json = malloc(16384+1);
	if(websocket_out_json == NULL)
	{
	    fprintf(stderr, "JSON buffer malloc failed.\n");
		return -1;
	}
	fprintf(stdout, "Done.\n");
	
	fprintf(stdout, "Initialising AirSpy (%.01fMSPS, %.03fMHz).. ",(float)sample_rate_val/1000000,(float)freq_hz/1000000);
	fflush(stdout);
	if(!setup_airspy())
	{
	    fprintf(stderr, "AirSpy init failed.\n");
		return -1;
	}
	fprintf(stdout, "Done.\n");
	
	fprintf(stdout, "Initialising FFT (%d bin).. ", FFT_SIZE);
	fflush(stdout);
	setup_fft();
	fprintf(stdout, "Done.\n");
	
	fprintf(stdout, "Starting FFT Thread.. ");
	if (pthread_create(&fftThread, NULL, thread_fft, NULL))
	{
		fprintf(stderr, "Error creating FFT thread\n");
		return -1;
	}
	fprintf(stdout, "Done.\n");

    fprintf(stdout, "Server running.\n");
    fflush(stdout);
	while (n >= 0 && !force_exit)
	{
		struct timeval tv;

		gettimeofday(&tv, NULL);

		ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
		if ((ms - oldms) > 333)
		{
		    pthread_mutex_lock(&fft_json_mutex);
		    if(latest_fft_json!=NULL)
		    {
		        strncpy(websocket_out_json,latest_fft_json,16384);
		    }
		    pthread_mutex_unlock(&fft_json_mutex);
		    
		    /* activate PROTOCOL_FFT on all sockets */
			lws_callback_on_writable_all_protocol(context, &protocols[PROTOCOL_FFT]);
			
			oldms = ms;
		}
		
        /* Service websockets, else wait 50ms */
		n = lws_service(context, 50);
	}

	lws_context_destroy(context);
	close_airspy();
	close_fftw();
	closelog();

	return 0;
}
