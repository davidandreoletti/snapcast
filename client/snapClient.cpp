//
//  Weather update client in C++
//  Connects SUB socket to tcp://localhost:5556
//  Collects weather updates and finds avg temp in zipcode
//
//  Olivier Chamoux <olivier.chamoux@fr.thalesgroup.com>
//
#include <iostream>
#include <sstream>
#include <sys/time.h>
#include <unistd.h>
#include <deque>
#include <vector>
#include <algorithm>
#include <thread> 
#include <portaudio.h>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <alsa/asoundlib.h>

#include "common/chunk.h"
#include "common/utils.h"
#include "common/log.h"
#include "stream.h"

using boost::asio::ip::tcp;
using namespace std;
namespace po = boost::program_options;


int deviceIdx;  
uint16_t sampleRate;
short channels;
uint16_t bps;
Stream* stream;

#define PCM_DEVICE "default"


void socketRead(tcp::socket* socket, void* to, size_t bytes)
{
	size_t toRead = bytes;
	size_t len = 0;
	do 
	{
		len += socket->read_some(boost::asio::buffer((char*)to + len, toRead));
		toRead = bytes - len;
	}
	while (toRead > 0);
}



void receiver(const std::string& ip, int port) 
{
	try
	{
		boost::asio::io_service io_service;
		tcp::resolver resolver(io_service);
		tcp::resolver::query query(tcp::v4(), ip, boost::lexical_cast<string>(port));
		tcp::resolver::iterator iterator = resolver.resolve(query);

		while (true)
		{
			try
			{
				tcp::socket s(io_service);
				s.connect(*iterator);
				struct timeval tv;
				tv.tv_sec  = 5; 
				tv.tv_usec = 0;         
				setsockopt(s.native(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

				std::clog << kLogNotice << "connected to " << ip << ":" << port << std::endl;
				while (true)
				{
					WireChunk* wireChunk = new WireChunk();
					socketRead(&s, wireChunk, Chunk::getHeaderSize());
//					cout << "WireChunk length: " << wireChunk->length << ", sec: " << wireChunk->tv_sec << ", usec: " << wireChunk->tv_usec << "\n";

					wireChunk->payload = (char*)malloc(wireChunk->length);
					socketRead(&s, wireChunk->payload, wireChunk->length);

/*					void* wireChunk = (void*)malloc(sizeof(WireChunk));
					size_t toRead = sizeof(WireChunk);
					size_t len = 0;
					do 
					{
						len += s.read_some(boost::asio::buffer((char*)wireChunk + len, toRead));
						toRead = sizeof(WireChunk) - len;
						cout << "len: " << len << "\ttoRead: " << toRead << "\n";
					}
					while (toRead > 0);
*/					
					stream->addChunk(new Chunk(sampleRate, channels, bps, wireChunk));
				}
			}
			catch (const std::exception& e)
			{
				std::clog << kLogNotice << "Exception: " << e.what() << ", trying to reconnect" << std::endl;
				stream->clearChunks();
				usleep(500*1000);
			}
		}
	}
	catch (const std::exception& e)
	{
		std::clog << kLogNotice << "Exception: " << e.what() << std::endl;
	}
}


void player(Stream* stream)
{
	unsigned int pcm, tmp, dir, rate;
	int channels, seconds;
	snd_pcm_t *pcm_handle;
	snd_pcm_hw_params_t *params;
	snd_pcm_uframes_t frames;
	char *buff;
	int buff_size;

	rate 	 = stream->getSampleRate();
	channels = stream->getChannels();

	/* Open the PCM device in playback mode */
	if (pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE,
					SND_PCM_STREAM_PLAYBACK, 0) < 0) 
		printf("ERROR: Can't open \"%s\" PCM device. %s\n",
					PCM_DEVICE, snd_strerror(pcm));

	/* Allocate parameters object and fill it with default values*/
	snd_pcm_hw_params_alloca(&params);

	snd_pcm_hw_params_any(pcm_handle, params);

	/* Set parameters */
	if (pcm = snd_pcm_hw_params_set_access(pcm_handle, params,
					SND_PCM_ACCESS_RW_INTERLEAVED) < 0) 
		printf("ERROR: Can't set interleaved mode. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
						SND_PCM_FORMAT_S16_LE) < 0) 
		printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, channels) < 0) 
		printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0) < 0) 
		printf("ERROR: Can't set rate. %s\n", snd_strerror(pcm));

	long unsigned int periodsize = 2*rate / 50;
	if (pcm = snd_pcm_hw_params_set_buffer_size_near(pcm_handle, params, &periodsize) < 0)
                printf("Unable to set buffer size %li: %s\n", (long int)periodsize, snd_strerror(pcm));

	/* Write parameters */
	if (pcm = snd_pcm_hw_params(pcm_handle, params) < 0)
		printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));

	/* Resume information */
	printf("PCM name: '%s'\n", snd_pcm_name(pcm_handle));

	printf("PCM state: %s\n", snd_pcm_state_name(snd_pcm_state(pcm_handle)));

	snd_pcm_hw_params_get_channels(params, &tmp);
	printf("channels: %i ", tmp);

	if (tmp == 1)
		printf("(mono)\n");
	else if (tmp == 2)
		printf("(stereo)\n");

	snd_pcm_hw_params_get_rate(params, &tmp, 0);
	printf("rate: %d bps\n", tmp);

	printf("seconds: %d\n", seconds);	

	/* Allocate buffer to hold single period */
	snd_pcm_hw_params_get_period_size(params, &frames, 0);
	printf("frames: %d\n", frames);

	buff_size = frames * channels * 2 /* 2 -> sample size */;
	buff = (char *) malloc(buff_size);

	snd_pcm_hw_params_get_period_time(params, &tmp, NULL);
	printf("period time: %d\n", tmp);

	while (true)
	{
/*		if (pcm = read(0, buff, buff_size) == 0) {
			printf("Early end of file.\n");
			return 0;
		}
*/
		snd_pcm_sframes_t avail;
		snd_pcm_sframes_t delay; 
		snd_pcm_avail_delay(pcm_handle, &avail, &delay);

		stream->getPlayerChunk(buff, (float)delay / 48000.f, frames);

		if (pcm = snd_pcm_writei(pcm_handle, buff, frames) == -EPIPE) {
			printf("XRUN.\n");
			snd_pcm_prepare(pcm_handle);
		} else if (pcm < 0) {
			printf("ERROR. Can't write to PCM device. %s\n", snd_strerror(pcm));
		}
	}

	snd_pcm_drain(pcm_handle);
	snd_pcm_close(pcm_handle);
	free(buff);
}


/* This routine will be called by the PortAudio engine when audio is needed.
** It may called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/
static int paStreamCallback( const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData )
{
//cout << "paStreamCallback: " << statusFlags << ", currentTime: " << timeInfo->currentTime << ", out: " << timeInfo->outputBufferDacTime << "\n";
    Stream* stream = (Stream*)userData;
    short* out = (short*)outputBuffer;

    (void) timeInfo; /* Prevent unused variable warnings. */
    (void) statusFlags;
    (void) inputBuffer;
    
	stream->getPlayerChunk(out, timeInfo->outputBufferDacTime, framesPerBuffer);
    return paContinue;
}



PaStream* initAudio(PaError& err, uint16_t sampleRate, short channels, uint16_t bps)
{
    PaStreamParameters outputParameters;
    PaStream *paStream = NULL;
//    printf("PortAudio Test: output sine wave. SR = %d, BufSize = %d\n", SAMPLE_RATE, FRAMES_PER_BUFFER);
    
    err = Pa_Initialize();
    if( err != paNoError ) goto error;

	int numDevices;
	numDevices = Pa_GetDeviceCount();
	if( numDevices < 0 )
	{
		printf( "ERROR: Pa_CountDevices returned 0x%x\n", numDevices );
		err = numDevices;
		goto error;
	}
	const PaDeviceInfo *deviceInfo;
	for(int i=0; i<numDevices; i++)
	{
	    deviceInfo = Pa_GetDeviceInfo(i);
		std::cerr << "Device " << i << ": " << deviceInfo->name << "\n";
	}

    outputParameters.device = deviceIdx==-1?Pa_GetDefaultOutputDevice():deviceIdx; /* default output device */
    std::cerr << "Using Device: " << outputParameters.device << "\n";
    if (outputParameters.device == paNoDevice) 
	{
      fprintf(stderr,"Error: No default output device.\n");
      goto error;
    }
    outputParameters.channelCount = channels;       /* stereo output */
	if (bps == 16)
	    outputParameters.sampleFormat = paInt16; /* 32 bit floating point output */
	else if (bps == 8)
	    outputParameters.sampleFormat = paUInt8; /* 32 bit floating point output */
	else if (bps == 32)
	    outputParameters.sampleFormat = paInt32; /* 32 bit floating point output */
	else if (bps == 24)
	    outputParameters.sampleFormat = paInt24; /* 32 bit floating point output */

    outputParameters.suggestedLatency = Pa_GetDeviceInfo( outputParameters.device )->defaultHighOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;
	std::cerr << "HighLatency: " << outputParameters.suggestedLatency << "\t LowLatency: " << Pa_GetDeviceInfo( outputParameters.device )->defaultLowOutputLatency << "\n";
    err = Pa_OpenStream(
              &paStream,
              NULL, /* no input */
              &outputParameters,
              sampleRate,
              paFramesPerBufferUnspecified, //FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              paStreamCallback,
              stream );
    if( err != paNoError ) goto error;


    err = Pa_StartStream( paStream );
	cout << "Latency: " << Pa_GetStreamInfo(paStream)->outputLatency << "\n";
    if( err != paNoError ) goto error;

//    err = Pa_StopStream( paStream );
//    if( err != paNoError ) goto error;

//    err = Pa_CloseStream( paStream );
//    if( err != paNoError ) goto error;

//    Pa_Terminate();
//    printf("Test finished.\n");
	    
    return paStream;
error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return paStream;
}




int main (int argc, char *argv[])
{
	string ip;	
	int bufferMs;
	size_t port;	
	bool runAsDaemon;
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "produce help message")
        ("port,p", po::value<size_t>(&port)->default_value(98765), "port where the server listens on")
        ("ip,i", po::value<string>(&ip)->default_value("192.168.0.2"), "server IP")
        ("soundcard,s", po::value<int>(&deviceIdx)->default_value(-1), "index of the soundcard")
        ("channels,c", po::value<short>(&channels)->default_value(2), "number of channels")
        ("samplerate,r", po::value<uint16_t>(&sampleRate)->default_value(48000), "sample rate")
        ("bps", po::value<uint16_t>(&bps)->default_value(16), "bit per sample")
        ("buffer,b", po::value<int>(&bufferMs)->default_value(300), "buffer size [ms]")
        ("daemon,d", po::bool_switch(&runAsDaemon)->default_value(false), "daemonize")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        cout << desc << "\n";
        return 1;
    }

	std::clog.rdbuf(new Log("snapclient", LOG_DAEMON));
	if (runAsDaemon)
	{
		daemonize();
		std::clog << kLogNotice << "daemon started" << std::endl;
	}

	std::clog << kLogNotice << "test" << std::endl;

	stream = new Stream(sampleRate, channels, bps);
	stream->setBufferLen(bufferMs);
//	PaError paError;
//	PaStream* paStream = initAudio(paError, sampleRate, channels, bps);
//	stream->setLatency(1000*Pa_GetStreamInfo(paStream)->outputLatency);

	std::thread receiverThread(receiver, ip, port);
	std::thread playerThread(player, stream);
	
	std::string cmd;
	while (true && (argc > 3))
	{
		std::cout << "> ";
		std::getline(std::cin, cmd);
//		line = fgets( str, 256, stdin );
//		if (line == NULL)
//			continue;
//		std::string cmd(line);
		std::cerr << "CMD: " << cmd << "\n";
		if (cmd == "quit")
			break;
		else
		{
			stream->setBufferLen(atoi(cmd.c_str()));
		}
	}

	playerThread.join();

    return 0;
}


